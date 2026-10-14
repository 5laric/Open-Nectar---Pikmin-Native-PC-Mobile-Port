// Netplay lockstep co-op session over GekkoNet.
//
// Each System::run loop turn while a session is active:
//   1. mControllerMgr.update() pumps SDL and samples the local pad (the F1
//      menu's consume path already zeroes the pads while it is open); the
//      sample is folded into the button-OR / latest-sticks accumulator.
//   2. When a submit is due, the local input (the accumulator, or the next
//      record of a scripted input file) goes to gekko_add_local_input.
//   3. gekko_update_session.
//   4. Per Advance: both inputs are injected into pads 0 and 1 with each
//      pad's control yaw, and exactly one tick runs (updateSysClock,
//      pc_netplay_on_tick_begin, app->idle, the state hash).
//   5. Per Save: an 8-byte handle {frame} and the state hash as checksum.
//   6. A Load cannot happen with prediction window 0: logged, then abort.
//   7. Session events: Connected, Started, Disconnected, DesyncDetected.
//   8. No Advance (waiting): no tick; the turn still pumped the network and
//      the window.
//
// Pacing: the session owns the single 30 Hz schedule (the presentation
// limiter in pc_window.cpp stays off). Drift-free deadline, next += 1000/30
// ms per Advance, with bounded catch-up; gekko_frames_ahead() stretches the
// deadline slightly while this peer is ahead, so the pair converges. Waiting
// turns pump the network and wait about 1 ms on a high-resolution timer.
// PIKMIN_NETPLAY_UNTHROTTLED=1 runs as fast as the session allows (tests).
//
// The local input delay is PIKMIN_NETPLAY_DELAY frames (default 2) or "auto"
// (from the median handshake RTT), then adapted to the stalls the peer
// reports (pc_netplay_adaptive.h). It is per peer and not hashed. Long
// blocking loads are covered by the load guard (pc_netplay_loadguard.h).
//
// Handshake: a Hello on the handshake channel with the exe and config
// digests, the netplay seed and the mode descriptor (pc_netplay_transfer.h).
// The first mismatching field refuses on both peers (exit 4).

#include "netplay/pc_netplay_session.h"

#include "netplay/pc_coop_switch.h"
#include "netplay/pc_input_log.h"
#include "netplay/pc_netplay_adaptive.h"
#include "netplay/pc_netplay_camlead.h"
#include "netplay/pc_netplay_det.h"
#include "netplay/pc_netplay_exit_codes.h"
#include "netplay/pc_netplay_gekko_input.h"
#include "netplay/pc_netplay_input_sel.h"
#include "netplay/pc_netplay_loadguard.h"
#include "netplay/pc_netplay_pad.h"
#include "netplay/pc_netplay_present.h"
#include "netplay/pc_netplay_sha256.h"
#include "netplay/pc_netplay_transfer.h"
#include "netplay/pc_netplay_udp.h"
#include "netplay/pc_state_hash.h"
#include "pc_coop.h"
#include "pc_permadeath.h"
#include "pc_speedrun.h"
#include "pc_vs.h"
#include "pc_window.h"
#include "randomizer/pc_randomizer.h"
#include "settings/pc_settings.h"

#include "gekkonet.h"

#include "system.h"
#include "BaseApp.h"
#include "Dolphin/os.h"
#include "Dolphin/pad.h"
#include "jaudio/interface.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <synchapi.h>
#else
#include <unistd.h>
#endif

namespace {

namespace npx = pc_netplay_exit;
namespace xfer = pc_netplay_xfer;

enum Phase {
	kIdle,      // no netplay switch: drive() returns false at once
	kHandshake,
	kSession,
	kDone,      // ended: keep owning the loop until the process quits
};

constexpr unsigned kMaxLocalDelay = pc_netplay_adaptive::kMaxLocalDelay;
static_assert(pc_netplay_loadguard::kLoadWindowFrames > kMaxLocalDelay + 1,
              "the load window must outlast the largest delay");

struct Config {
	bool isHost        = false;
	bool active        = false;
	uint16_t hostPort  = 0; // --netplay-host <port>
	uint32_t joinIp    = 0; // --netplay-join <ip:port>
	uint16_t joinPort  = 0;
	unsigned localDelay = 2; // PIKMIN_NETPLAY_DELAY
	bool delayAuto     = false;
	uint32_t seed      = 0; // PIKMIN_NETPLAY_SEED
	std::string localInputFile; // --netplay-input <file> / PIKMIN_NETPLAY_LOCAL_INPUT_FILE
	uint64_t exitAfter = 0;     // PIKMIN_NETPLAY_EXIT_AFTER_TICKS
	std::string inputSpec;      // --netplay-input keyboard|gamepad[:N]|auto
	int inputKind      = 0;     // pc_netplay_input_sel::Kind
	int inputGamepad   = 0;
};

int sArgc     = 0;
char** sArgv  = nullptr;
Config sCfg;
Phase sPhase       = kIdle;
bool sInitialised  = false;
bool sForcedModes  = false;

constexpr uint16_t kProtocolVersion = xfer::kProtocolV1;
constexpr size_t kHsHeaderLen       = xfer::kHsHeaderLen;
constexpr size_t kHsLen             = xfer::kHsLen;
using Hello = xfer::Hello;

Hello sLocal;
std::string sExeHexStr;
std::string sCfgHexStr;
std::string sCfgText;

pc_netplay_transport::UdpSocket* sSock = nullptr;
pc_netplay_transport::GekkoLink* sLink = nullptr;
pc_netplay_transport::LossyLink* sLossy = nullptr;
GekkoNetAdapter* sAdapter = nullptr;
GekkoSession* sGekko      = nullptr;
uint8_t sRemoteAddrBlob[6] = { 0 };
bool sHaveRemote      = false;
uint32_t sRemoteIp    = 0;
uint16_t sRemotePort  = 0;
int sLocalHandle      = 0;
int sLocalRole        = 0; // 0 = host/P1, 1 = joiner/P2
bool sGekkoStarted    = false;

// Handshake progress. Every Hello send carries a fresh nonce with its send
// time recorded; each Ack echoes the Hello nonce it answers, so an RTT
// sample is matched to its own send. Samples are median-filtered.
bool sSentAck       = false;
bool sGotAck        = false;
double sHsStartMs   = 0;
double sHsLastSendMs = 0;
uint64_t sHsNextNonce = 1;
std::vector<std::pair<uint64_t, double>> sHsSendTimes;
std::vector<double> sHsSamples;
// Test hook: PIKMIN_NETPLAY_TEST_DROP_FINAL_ACK=1 drops handshake-phase Acks,
// so the peer must recover through in-session Hello answers.
bool sDropFinalAckInit = false;
bool sDropFinalAck     = false;
// Test hook: PIKMIN_NETPLAY_TEST_LOAD_DELAY_MS sleeps once inside the next
// stage load on that peer only.
bool sLoadDelayDone = false;

// Load guard (pc_netplay_loadguard.h). sInAdvance is true only while
// app->idle() runs inside an Advance, the only place the keep-alive may
// pump, and only on the session's own thread.
std::atomic<bool> sInAdvance{ false };
std::thread::id sLgMainThread;
bool sLgConfigured  = false;
bool sLgKeepAlive   = true;
bool sLgSpeculative = false;
pc_netplay_loadguard::KeepAliveGate sLgGate;
pc_netplay_loadguard::LoadWindow sLgWindow;
pc_netplay_loadguard::StallPlan sLgStall;
bool sLgStallDone     = false;
uint32_t sLgStageLoads = 0;
uint32_t sLgTickFrame  = 0;
double sLgTickStartMs  = 0;
double sLgLastPumpMs   = 0;
double sLgTickMaxGapMs = 0;
uint64_t sLgTickPumps  = 0;
uint64_t sLgPumps      = 0;
uint64_t sLgSitePumps[pc_netplay_loadguard::kSiteCount] = {};
uint64_t sLgWindows    = 0;
uint64_t sLgLongTicks  = 0;
double sLgLongestTickMs = 0;
double sLgWorstGapMs    = 0;
double sLgWindowOpenMs  = 0;
uint32_t sLgWindowFirst = 0;
bool sLgSummaryDone     = false;

// Run stats.
uint64_t sSessionTicks = 0;
uint64_t sAdvances     = 0;
uint64_t sStalls       = 0;
uint64_t sSaves        = 0;
uint64_t sSubmitted    = 0;
// The frame the next local input lands on (GekkoNet's local last-received
// frame + 1). A submit is due when sNextLand == sAdvances + localDelay.
uint64_t sNextLand     = 0;
double sNextTurnMs     = 0;
double sStallMs        = 0;
double sSessionStartMs = 0;
uint32_t sLastAdvanceFrame = 0;
double sLastAdvanceWallMs  = 0;
PcNetplayAccum sPadAccum;
// TEST ONLY: PIKMIN_NETPLAY_TEST_DESYNC_AT_FRAME=<f> flips this peer's
// reported checksum from frame f on, so both peers take the desync path
// without any sim change. Not in the config hash.
int64_t sTestDesyncFrame = -1;
bool sTestDesyncLogged   = false;
bool sNaviPosTrace       = false; // PIKMIN_NETPLAY_DEBUG_NAVI_POS=1

// Ring of per-tick hashes so a desync report can print the desynced frame's
// sub-hashes. GekkoNet frame F is hash tick F+1.
struct HashEntry {
	bool valid      = false;
	uint64_t tick   = 0;
	uint64_t total  = 0;
	uint64_t subs[7] = { 0, 0, 0, 0, 0, 0, 0 };
};
constexpr size_t kHashRing = 2048;
HashEntry sHashRing[kHashRing];

void hash_ring_store(uint64_t tick, uint64_t total, const uint64_t subs[7])
{
	HashEntry& e = sHashRing[tick % kHashRing];
	e.valid      = true;
	e.tick       = tick;
	e.total      = total;
	for (int i = 0; i < 7; ++i) e.subs[i] = subs[i];
}

const HashEntry* hash_ring_find(uint64_t tick)
{
	const HashEntry& e = sHashRing[tick % kHashRing];
	return (e.valid && e.tick == tick) ? &e : nullptr;
}

// Scripted local input (pkni v2, pad 0 of each record).
std::vector<uint8_t> sScriptBytes;
size_t sScriptTicks  = 0;
size_t sScriptIdx    = 0;
bool sScriptActive   = false;

double now_ms()
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// One shared high-resolution timer for every short wait. Hidden processes
// on Windows 11 do not get the timeBeginPeriod(1) granularity they ask for
// (Sleep(1) sleeps ~15.6 ms), which would quantise the handshake RTT and the
// 30 Hz slot wait, so the bulk of a wait runs on a waitable timer created
// with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION. Slot waits spin only their
// last 1 ms; poll waits never spin. Without the high-resolution flag (before
// Windows 10 1803) a plain timer with timeBeginPeriod(1) is used.
#ifdef _WIN32
bool sHiresTimerReady   = false;
HANDLE sHiresTimer      = NULL;
bool sHiresTimerHighRes = false;
bool sHiresPeriodBegun  = false;
bool sHiresCoarse       = false;

void hires_timer_teardown()
{
	if (sHiresTimer != NULL) {
		CloseHandle(sHiresTimer);
		sHiresTimer = NULL;
	}
	if (sHiresPeriodBegun) {
		sHiresPeriodBegun = false;
		timeEndPeriod(1);
	}
}

void ensure_hires_timer()
{
	if (sHiresTimerReady) return;
	sHiresTimerReady = true;
	HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (timer != NULL) {
		sHiresTimer        = timer;
		sHiresTimerHighRes = true;
	} else {
		timer = CreateWaitableTimerW(NULL, FALSE, NULL);
		if (timer != NULL) {
			sHiresTimer        = timer;
			sHiresTimerHighRes = false;
			timeBeginPeriod(1);
			sHiresPeriodBegun = true;
		}
	}
	if (sHiresTimer != NULL) {
		printf("[netplay] timer path: %swaitable timer%s\n", sHiresTimerHighRes ? "high-resolution " : "plain ",
		       sHiresTimerHighRes ? "" : " + timeBeginPeriod(1)");
	} else {
		timeBeginPeriod(1);
		sHiresPeriodBegun = true;
		const double t0 = now_ms();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		sHiresCoarse = (now_ms() - t0) > 5.0;
		printf("[netplay] timer path: timeBeginPeriod fallback (%s)\n",
		       sHiresCoarse ? "coarse: spin waits" : "fine: sleep+spin tail");
	}
	fflush(stdout);
	std::atexit(hires_timer_teardown);
}
#else
inline void ensure_hires_timer() {}
#endif

// Waits ms. Everything except the last spinTailMs (at most 1 ms) blocks;
// poll waits pass spinTailMs = 0 and never spin.
void sleep_hires_ms(double ms, double spinTailMs = 1.0)
{
	if (ms <= 0) return;
	if (spinTailMs < 0) spinTailMs = 0;
	if (spinTailMs > 1.0) spinTailMs = 1.0;
	ensure_hires_timer();
	const double start   = now_ms();
	const double blockMs = ms - spinTailMs;
#ifdef _WIN32
	if (sHiresTimer != NULL) {
		if (blockMs > 0.05) {
			LARGE_INTEGER due;
			due.QuadPart = -(LONGLONG)(blockMs * 10000.0); // 100 ns units
			if (SetWaitableTimer(sHiresTimer, &due, 0, NULL, NULL, FALSE)) {
				WaitForSingleObject(sHiresTimer, INFINITE);
			} else {
				std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(blockMs));
			}
		}
	} else if (!sHiresCoarse || spinTailMs == 0) {
		if (blockMs > 0.05) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(blockMs));
	}
#else
	if (blockMs > 0.05) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(blockMs));
#endif
	if (spinTailMs == 0) return;
	const double deadline = start + ms;
	while (now_ms() < deadline - 0.3) std::this_thread::yield();
	while (now_ms() < deadline) {
	}
}

// While a session runs, the process opts out of timer-resolution power
// throttling (its timer-resolution requests are always honoured); at the end
// control returns to the system. SetProcessInformation is resolved at run
// time: its declaration needs _WIN32_WINNT >= 0x0602.
void netplay_timer_power_opt(bool on)
{
#ifdef _WIN32
	static bool resolved = false;
	typedef BOOL(WINAPI * SetProcessInformationFn)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
	static SetProcessInformationFn fn = nullptr;
	static bool active = false;
	if (!resolved) {
		resolved        = true;
		HMODULE kernel  = GetModuleHandleW(L"kernel32.dll");
		if (kernel != nullptr) fn = (SetProcessInformationFn)(void*)GetProcAddress(kernel, "SetProcessInformation");
	}
	if (fn == nullptr || on == active) return;
	PROCESS_POWER_THROTTLING_STATE state;
	state.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	state.ControlMask = on ? PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION : 0;
	state.StateMask   = 0;
	if (fn(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) active = on;
#else
	(void)on;
#endif
}

const char* getenv_nonempty(const char* name)
{
	const char* v = std::getenv(name);
	return (v != nullptr && *v != '\0') ? v : nullptr;
}

unsigned read_unsigned_env(const char* name, unsigned fallback)
{
	const char* v = getenv_nonempty(name);
	if (v == nullptr) return fallback;
	char* end       = nullptr;
	unsigned long n = strtoul(v, &end, 10);
	if (end == v || *end != '\0') return fallback;
	return (unsigned)n;
}

double read_double_env(const char* name, double fallback)
{
	const char* v = getenv_nonempty(name);
	if (v == nullptr) return fallback;
	char* end = nullptr;
	double n  = strtod(v, &end);
	if (end == v || *end != '\0') return fallback;
	return n;
}

uint32_t read_u32_env(const char* name, uint32_t fallback)
{
	const char* v = getenv_nonempty(name);
	if (v == nullptr) return fallback;
	char* end       = nullptr;
	unsigned long n = strtoul(v, &end, 0);
	if (end == v || *end != '\0' || n > 0xFFFFFFFFul) return fallback;
	return (uint32_t)n;
}

const char* argv_value(int argc, char** argv, const char* flag)
{
	for (int i = 1; i + 1 < argc; ++i) {
		if (argv[i] != nullptr && std::strcmp(argv[i], flag) == 0) return argv[i + 1];
	}
	return nullptr;
}

bool env_is_one(const char* name)
{
	const char* v = getenv_nonempty(name);
	return v != nullptr && std::strcmp(v, "1") == 0;
}

[[noreturn]] void usage_exit(const char* fmt, const char* arg)
{
	printf("[netplay] ");
	printf(fmt, arg != nullptr ? arg : "");
	printf("\n");
	fflush(stdout);
	std::exit(npx::kUsage);
}

// ---- digests ----

std::string exe_path()
{
#ifdef _WIN32
	char path[4096];
	DWORD n = GetModuleFileNameA(nullptr, path, sizeof(path));
	if (n == 0 || n >= sizeof(path)) return std::string();
	return std::string(path, n);
#elif defined(__linux__)
	char path[4096];
	const ssize_t n = readlink("/proc/self/exe", path, sizeof(path));
	if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) return std::string();
	return std::string(path, static_cast<size_t>(n));
#else
	return std::string();
#endif
}

bool sha_file(const char* path, uint8_t out[32])
{
	FILE* f = fopen(path, "rb");
	if (f == nullptr) return false;
	pc_netplay_sha::Sha256 s;
	static uint8_t chunk[65536];
	bool readErr = false;
	while (true) {
		const size_t n = fread(chunk, 1, sizeof(chunk), f);
		if (n > 0) s.update(chunk, n);
		if (n < sizeof(chunk)) {
			readErr = ferror(f) != 0;
			break;
		}
	}
	fclose(f);
	if (readErr) return false;
	s.final(out);
	return true;
}

// The session config the handshake hashes: every sim-relevant setting
// (pc_settings_netplay_config_text, the list is in pc_settings.cpp), the
// co-op mode and captains, the netplay seed and the protocol version.
// The local input delay is per peer and deliberately not part of it.
std::string build_config_string()
{
	std::string s = "netplay-config-v1;";
	s += pc_settings_netplay_config_text();
	char buf[96];
	snprintf(buf, sizeof(buf), "coop=1;captainP1=%d;captainP2=%d;netplaySeed=%u;protocolVersion=%u;",
	         pc_coop_captain(0), pc_coop_captain(1), (unsigned)sCfg.seed, (unsigned)kProtocolVersion);
	s += buf;
	return s;
}

void compute_local_hello()
{
	const std::string path = exe_path();
	if (path.empty() || !sha_file(path.c_str(), sLocal.exe)) {
		printf("[netplay] refusing session: could not hash running executable %s\n", path.c_str());
		fflush(stdout);
		std::exit(npx::kUsage);
	}
	sExeHexStr = pc_netplay_sha::hex(sLocal.exe, 32);
	sCfgText   = build_config_string();
	pc_netplay_sha::sha256(sCfgText.data(), sCfgText.size(), sLocal.cfg);
	sCfgHexStr            = pc_netplay_sha::hex(sLocal.cfg, 32);
	sLocal.seed           = sCfg.seed;
	sLocal.mode.mode      = pc_coop_direct_requested() ? xfer::kModeCoopDirect : xfer::kModeNone;
	sLocal.mode.captainP1 = (uint8_t)pc_coop_captain(0);
	sLocal.mode.captainP2 = (uint8_t)pc_coop_captain(1);
}

bool local_ui_open() { return pc_settings_menu_is_open() != 0; }

// ---- input ----

void apply_input_selection()
{
	using namespace pc_netplay_input_sel;
	if (sCfg.inputKind == (int)kInputAuto) return;
	const Kind kind = (Kind)sCfg.inputKind;
	bool ignoreKb = false, ignorePad = false;
	filter_for_selection(kind, &ignoreKb, &ignorePad);
	pc_window_set_netplay_input_filter(ignoreKb, ignorePad);
	const char* who = sLocalRole == 0 ? "host/P1" : "joiner/P2";
	if (kind == kInputKeyboard) {
		pc_window_input_assign(sLocalRole, PC_INPUT_DEV_KEYBOARD, -1);
		printf("[netplay] input: keyboard and mouse feed %s (every gamepad ignored)\n", who);
	} else {
		pc_window_set_netplay_gamepad(sLocalRole, sCfg.inputGamepad);
		printf("[netplay] input: gamepad #%d feeds %s (every key ignored; the pad keeps working while another "
		       "window has focus)\n",
		       sCfg.inputGamepad, who);
		if (pc_window_num_gamepads() <= sCfg.inputGamepad)
			printf("[netplay] input: gamepad #%d is not connected yet; %s stays neutral until it is plugged in\n",
			       sCfg.inputGamepad, who);
	}
	fflush(stdout);
}

void load_scripted_file()
{
	const char* path = sCfg.localInputFile.c_str();
	if (path[0] == '\0') return;
	FILE* f = fopen(path, "rb");
	if (f == nullptr) {
		printf("[netplay] local input file: cannot open %s\n", path);
		fflush(stdout);
		std::exit(npx::kInputFile);
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 10) {
		printf("[netplay] local input file: truncated %s\n", path);
		fflush(stdout);
		fclose(f);
		std::exit(npx::kInputFile);
	}
	sScriptBytes.resize((size_t)size);
	const size_t got = fread(sScriptBytes.data(), 1, (size_t)size, f);
	fclose(f);
	sScriptBytes.resize(got);
	if (got < 10 || memcmp(sScriptBytes.data(), "PKNI", 4) != 0) {
		printf("[netplay] local input file: bad magic %s\n", path);
		fflush(stdout);
		std::exit(npx::kInputFile);
	}
	const uint16_t version = (uint16_t)(sScriptBytes[4] | ((uint16_t)sScriptBytes[5] << 8));
	const uint16_t pads    = (uint16_t)(sScriptBytes[6] | ((uint16_t)sScriptBytes[7] << 8));
	const uint16_t rec     = (uint16_t)(sScriptBytes[8] | ((uint16_t)sScriptBytes[9] << 8));
	if (version != 2 || pads != 4 || rec != 56) {
		printf("[netplay] local input file: need pkni v2 4x14, got v%u %ux%u (%s)\n", (unsigned)version,
		       (unsigned)pads, (unsigned)rec, path);
		fflush(stdout);
		std::exit(npx::kInputFile);
	}
	sScriptTicks  = (sScriptBytes.size() - 10) / 56;
	sScriptActive = true;
	printf("[netplay] local input file: %s (%llu ticks)\n", path, (unsigned long long)sScriptTicks);
	fflush(stdout);
}

PcNetplayInput scripted_record(size_t idx)
{
	PcNetplayInput in;
	if (!sScriptActive || idx >= sScriptTicks) return in; // neutral past the end
	const uint8_t* rec = sScriptBytes.data() + 10 + idx * 56;
	// Pad 0 of the 4-pad record (14 bytes: 11 PADStatus + yaw u16 + flags).
	in.buttons    = (uint16_t)(rec[0] | ((uint16_t)rec[1] << 8));
	in.stickX     = (int8_t)rec[2];
	in.stickY     = (int8_t)rec[3];
	in.substickX  = (int8_t)rec[4];
	in.substickY  = (int8_t)rec[5];
	in.triggerL   = rec[6];
	in.triggerR   = rec[7];
	in.controlYaw = (uint16_t)(rec[11] | ((uint16_t)rec[12] << 8));
	return in;
}

PcNetplayInput build_local_input()
{
	if (sScriptActive) {
		PcNetplayInput in = scripted_record(sScriptIdx++);
		if (local_ui_open()) in = pc_netplay_input_neutral();
		return in;
	}
	if (local_ui_open()) {
		// The F1 menu is local: the local input stays neutral, and the latch
		// is cleared so buttons held before opening do not leak in after it.
		sPadAccum.reset();
		return pc_netplay_input_neutral();
	}
	return sPadAccum.take();
}

// Every session turn folds the current physical sample into the
// accumulator, so a press shorter than the submit interval still reaches
// the next submit. The fresh yaw capture keeps the submitted yaw following
// the live camera; inject_input rewrites the slots before every tick.
void accum_add_current()
{
	if (sScriptActive) return;
	pc_input_log_capture_yaw_fresh();
	PADStatus* pads  = pc_netplay_pad_status();
	const int padIdx = pc_netplay_input_sel::local_pad_index(sLocalRole, (pc_netplay_input_sel::Kind)sCfg.inputKind);
	const PADStatus s = pads[padIdx];
	const uint16_t yaw = pc_input_log_yaw_valid(sLocalRole) ? pc_input_log_yaw_raw(sLocalRole) : 0;
	sPadAccum.add(s.button, s.stickX, s.stickY, s.substickX, s.substickY, s.triggerLeft, s.triggerRight, yaw);
}

void inject_input(int pad, const PcNetplayInput& in)
{
	PADStatus* pads        = pc_netplay_pad_status();
	pads[pad].button       = in.buttons;
	pads[pad].stickX       = in.stickX;
	pads[pad].stickY       = in.stickY;
	pads[pad].substickX    = in.substickX;
	pads[pad].substickY    = in.substickY;
	pads[pad].triggerLeft  = in.triggerL;
	pads[pad].triggerRight = in.triggerR;
	pads[pad].analogA      = 0;
	pads[pad].analogB      = 0;
	pads[pad].err          = PAD_ERR_NONE;
	pc_input_log_yaw_set(pad, in.controlYaw, pc_input_log::kFlagsNone);
}

void inject_neutral_pad(int pad)
{
	PADStatus* pads = pc_netplay_pad_status();
	memset(&pads[pad], 0, sizeof(PADStatus));
	pads[pad].err = (pad < 2) ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
	pc_input_log_yaw_set(pad, 0, pc_input_log::kFlagsNone);
}

uint32_t fold_hash64(uint64_t v) { return (uint32_t)(v ^ (v >> 32)); }

// ---- adaptive input delay (pc_netplay_adaptive.h) ----
//
//   PIKMIN_NETPLAY_ADAPTIVE_DELAY  unset/1: on in real-time sessions (off in
//                                  unthrottled test runs); 0: off; force: on
//                                  even when unthrottled
//   PIKMIN_NETPLAY_TEST_DELAY_SCHEDULE=<frame>:<delay>[,...]  test only:
//                                  forced changes at those frames
//   PIKMIN_NETPLAY_STALL_TRACE=1   one line per stall event and per tick of
//                                  50 ms or more
//
// Wall clock only: nothing here reads or writes sim state, and the only
// GekkoNet calls are the local delay setters and gekko_network_stats. Each
// peer reports its own counted stalls to the other every 250 ms (kHsAdvice
// on the handshake channel, unreliable, cumulative).
constexpr uint64_t kAdaptiveWarmupFrames = 150;
constexpr double kRttSampleMs = 500.0;
constexpr double kAdviceMs    = 250.0;
constexpr uint8_t kHsAdvice   = 0x40; // handshake-channel type: stable header + Advice payload
pc_netplay_adaptive::SessionStats sStats;
pc_netplay_adaptive::DelayController sDelayCtl;
bool sAdaptiveConfigured = false;
bool sAdaptive           = false;
const char* sAdaptiveWhy = "";
std::vector<pc_netplay_adaptive::ScheduleStep> sDelaySched;
size_t sDelaySchedIdx = 0;
unsigned sDelayStart  = 0;
unsigned sDelayLow    = 0;
unsigned sDelayHigh   = 0;
uint64_t sDelayUps    = 0;
uint64_t sDelayDowns  = 0;
std::string sDelayTimeline;
std::string sPendingWhy;
uint64_t sSettleUntil  = 0;
bool sStallOpen        = false;
double sStallStartMs   = 0;
double sStallDurMs     = 0;
bool sStallExcluded    = false;
double sLastFrameEndMs = 0;
double sRttNextMs      = 0;
bool sStatsFinalDone   = false;
double sLateMs         = 0;
uint32_t sLateEvents   = 0;
uint32_t sAdviceSeq    = 0;
double sAdviceNextMs   = 0;
pc_netplay_adaptive::AdviceReceiver sAdviceIn;
double sPeerLateMs     = 0;
double sSelfOverrunMs  = 0;
double sNextDueMs      = 0;
bool sHaveDue          = false;
bool sPrevTurnStalled  = true;
double sOwnLagPrev     = 0;
float sStallAhead      = 0;
int sStallTrace        = -1;

bool stall_trace()
{
	if (sStallTrace < 0) sStallTrace = env_is_one("PIKMIN_NETPLAY_STALL_TRACE") ? 1 : 0;
	return sStallTrace == 1;
}

double session_s(double nowMs) { return sSessionStartMs > 0 ? (nowMs - sSessionStartMs) / 1000.0 : 0.0; }

void adaptive_timeline_add(double nowMs, uint64_t frame, unsigned delay)
{
	char cell[64];
	snprintf(cell, sizeof(cell), "%s+%.1fs@%llu:%u", sDelayTimeline.empty() ? "" : " ", session_s(nowMs),
	         (unsigned long long)frame, delay);
	sDelayTimeline += cell;
	if (delay < sDelayLow) sDelayLow = delay;
	if (delay > sDelayHigh) sDelayHigh = delay;
}

// Session start, once the delay (including DELAY=auto) is final.
void adaptive_configure()
{
	sAdaptiveConfigured = true;
	sStats.reset();
	sDelayStart = sDelayLow = sDelayHigh = sCfg.localDelay;
	sDelayUps = sDelayDowns = 0;
	sDelayTimeline.clear();
	sStallOpen       = false;
	sLastFrameEndMs  = 0;
	sRttNextMs       = 0;
	sStatsFinalDone  = false;
	sLateMs          = 0;
	sLateEvents      = 0;
	sAdviceSeq       = 0;
	sAdviceNextMs    = 0;
	sAdviceIn.reset();
	sPeerLateMs      = 0;
	sSelfOverrunMs   = 0;
	sNextDueMs       = 0;
	sHaveDue         = false;
	sPrevTurnStalled = true;
	sOwnLagPrev      = 0;
	sNextLand        = sCfg.localDelay; // the first add fills frames 0..d-1 with GekkoNet's empty input
	sSettleUntil     = kAdaptiveWarmupFrames;
	pc_netplay_adaptive::Policy pol;
	if (pol.maxDelay > kMaxLocalDelay) pol.maxDelay = kMaxLocalDelay;
	sDelayCtl.configure(pol);
	sDelayCtl.start(now_ms());
	sDelaySched.clear();
	sDelaySchedIdx = 0;
	std::string err;
	const char* sched = getenv_nonempty("PIKMIN_NETPLAY_TEST_DELAY_SCHEDULE");
	if (sched != nullptr && !pc_netplay_adaptive::parse_schedule(sched, pol.minDelay, pol.maxDelay, &sDelaySched, &err)) {
		printf("[netplay] adaptive delay: test schedule ignored (%s)\n", err.c_str());
		sDelaySched.clear();
	}
	const char* mode = getenv_nonempty("PIKMIN_NETPLAY_ADAPTIVE_DELAY");
	const bool off   = mode != nullptr && (std::strcmp(mode, "0") == 0 || std::strcmp(mode, "off") == 0);
	const bool force = mode != nullptr && std::strcmp(mode, "force") == 0;
	if (!sDelaySched.empty()) {
		sAdaptive    = false;
		sAdaptiveWhy = "off (test schedule drives the delay)";
	} else if (off) {
		sAdaptive    = false;
		sAdaptiveWhy = "off (PIKMIN_NETPLAY_ADAPTIVE_DELAY=0)";
	} else if (pc_netplay_unthrottled() && !force) {
		sAdaptive    = false;
		sAdaptiveWhy = "off (unthrottled test run)";
	} else {
		sAdaptive    = true;
		sAdaptiveWhy = force ? "on (forced)" : "on";
	}
	printf("[netplay] adaptive delay: %s start=%u range=%u..%u up=%.0fms stall in %.1fs down=%.0fs clean+rtt "
	       "schedule=%llu steps\n",
	       sAdaptiveWhy, sCfg.localDelay, pol.minDelay, pol.maxDelay, pol.upStallMs, pol.upWindowMs / 1000.0,
	       pol.downHoldMs / 1000.0, (unsigned long long)sDelaySched.size());
	fflush(stdout);
}

void adaptive_log_change(unsigned from, unsigned to, const std::string& why)
{
	const double now = now_ms();
	if (to > from) ++sDelayUps;
	else ++sDelayDowns;
	adaptive_timeline_add(now, sAdvances, to);
	printf("[netplay] delay change: %u -> %u at frame=%llu next-land=%llu t=%.1fs (%s)\n", from, to,
	       (unsigned long long)sAdvances, (unsigned long long)sNextLand, session_s(now), why.c_str());
	fflush(stdout);
}

// Why no change may start now (nullptr: free to change).
const char* adaptive_frozen()
{
	if (sLgWindow.is_open()) return "load window";
	if (!pc_netplay_adaptive::submit_due(sNextLand, sAdvances, sCfg.localDelay)) return "transition";
	if (sAdvances < sSettleUntil) return "settle";
	return nullptr;
}

// Target delay for this submit turn (the current delay: nothing to do).
unsigned adaptive_target()
{
	const unsigned cur = sCfg.localDelay;
	sPendingWhy.clear();
	if (!sDelaySched.empty()) {
		if (sDelaySchedIdx >= sDelaySched.size()) return cur;
		const pc_netplay_adaptive::ScheduleStep& st = sDelaySched[sDelaySchedIdx];
		if (sAdvances < st.frame || sLgWindow.is_open()) return cur;
		// A shrink before the first submit would skip GekkoNet's start fill
		// of frames 0..d-1 and the submit gate would never be due again.
		if (sSubmitted == 0 && st.delay < cur) return cur;
		++sDelaySchedIdx;
		char buf[64];
		snprintf(buf, sizeof(buf), "test schedule step %llu for frame %llu", (unsigned long long)sDelaySchedIdx,
		         (unsigned long long)st.frame);
		sPendingWhy = buf;
		return st.delay;
	}
	if (!sAdaptive) return cur;
	const char* frozen = adaptive_frozen();
	pc_netplay_adaptive::Decision d = sDelayCtl.decide(now_ms(), cur, frozen != nullptr);
	if (!d.changed) {
		if (!d.reason.empty()) {
			printf("[netplay] delay hold at %u: %s\n", cur, d.reason.c_str());
			fflush(stdout);
		}
		return cur;
	}
	sPendingWhy = d.reason;
	return d.target;
}

// A shrink: GekkoNet touches no input; the next (cur - target) turns submit
// nothing, until sNextLand == sAdvances + target.
void adaptive_shrink(unsigned target)
{
	const unsigned from = sCfg.localDelay;
	gekko_set_local_delay_nofill(sGekko, sLocalHandle, (unsigned char)target);
	sCfg.localDelay = target;
	adaptive_log_change(from, target, sPendingWhy);
}

// The submit of this turn: n == 1 normally; n == 1 + k grows the delay by k.
// Each input lands on sNextLand. Scripted inputs consume one record per
// frame; a live pad repeats this turn's sample on the k extra frames.
void submit_local_inputs(unsigned n)
{
	const unsigned from = sCfg.localDelay;
	PcNetplayInput first;
	for (unsigned j = 0; j < n; ++j) {
		if (j > 0) gekko_set_local_delay_nofill(sGekko, sLocalHandle, (unsigned char)(from + j));
		PcNetplayInput local = (j == 0 || sScriptActive) ? build_local_input() : first;
		if (j == 0) first = local;
		uint8_t wire[16];
		pc_netplay_input_encode(local, wire);
		gekko_add_local_input(sGekko, sLocalHandle, wire);
		// The lead camera replays this peer's submitted inputs until the sim
		// applies them, so every add is noted at the frame it lands on.
		pc_netplay_camlead_note_local_input(sNextLand, local);
		++sSubmitted;
		++sNextLand;
	}
	if (n > 1) {
		sCfg.localDelay = from + n - 1;
		adaptive_log_change(from, sCfg.localDelay, sPendingWhy);
	}
}

void adaptive_note_frame()
{
	const double now = now_ms();
	if (sLastFrameEndMs > 0) sStats.add_frame(now - sLastFrameEndMs);
	sLastFrameEndMs = now;
}

void adaptive_close_stall()
{
	if (!sStallOpen) return;
	sStallOpen = false;
	pc_netplay_adaptive::StallEvent e;
	e.startMs  = sStallStartMs;
	e.durMs    = sStallDurMs;
	e.excluded = sStallExcluded;
	sStats.add_stall(e);
	// Reported to the peer (its inputs were late here), never fed to this
	// peer's own controller.
	const bool counted = pc_netplay_adaptive::stall_counted(e, sDelayCtl.policy().hitchMs);
	if (counted) {
		sLateMs += e.durMs;
		++sLateEvents;
	}
	if (stall_trace()) {
		printf("[netplay] stall-trace: stall frame=%llu t=%.3fs dur=%.1fms excluded=%d counted=%d ahead=%.2f "
		       "delay=%u\n",
		       (unsigned long long)sAdvances, session_s(e.startMs), e.durMs, (int)e.excluded, (int)counted,
		       sStallAhead, sCfg.localDelay);
		fflush(stdout);
	}
}

// Own lateness: a turn that starts behind this peer's 30 Hz schedule delays
// its next input, and the peer will report the wait. It counts only where
// it grows, never after a stall turn and not inside a load window.
void adaptive_note_turn_start(double turnStartMs)
{
	if (!sAdaptiveConfigured || !sGekkoStarted || sAdvances == 0 || pc_netplay_unthrottled()) return;
	const double lag = (sHaveDue && turnStartMs > sNextDueMs) ? turnStartMs - sNextDueMs : 0.0;
	if (sPrevTurnStalled || !sHaveDue) {
		sOwnLagPrev = lag;
		return;
	}
	const double grew = lag - sOwnLagPrev;
	sOwnLagPrev       = lag;
	if (grew <= 0 || sLgWindow.is_open()) return;
	sSelfOverrunMs += grew;
	sDelayCtl.add_self_overrun(turnStartMs, grew);
}

void adaptive_note_turn_end(bool advanced, bool haveDue, double nextDueMs)
{
	sPrevTurnStalled = !advanced;
	if (advanced && haveDue) {
		sNextDueMs = nextDueMs;
		sHaveDue   = true;
	}
}

void hs_send(const uint8_t* msg, size_t len);

void adaptive_send_advice(double nowMs)
{
	if (nowMs < sAdviceNextMs) return;
	sAdviceNextMs = nowMs + kAdviceMs;
	pc_netplay_adaptive::Advice a;
	a.seq            = ++sAdviceSeq;
	a.lateMs         = (uint32_t)(sLateMs + 0.5);
	a.lateEvents     = sLateEvents;
	a.delay          = (uint8_t)sCfg.localDelay;
	const double p50 = sStats.rtt_percentile(50);
	a.rttP50         = p50 < 0 ? 0xFFFF : (uint16_t)(p50 > 65534 ? 65534 : p50);
	a.flags          = sAdaptive ? 1 : 0;
	uint8_t msg[kHsHeaderLen + pc_netplay_adaptive::kAdvicePayload];
	memcpy(msg, xfer::kHsMagic, 4);
	msg[4] = kHsAdvice;
	msg[5] = (uint8_t)(kProtocolVersion & 0xFF);
	msg[6] = (uint8_t)((kProtocolVersion >> 8) & 0xFF);
	pc_netplay_adaptive::advice_encode(a, msg + kHsHeaderLen);
	hs_send(msg, sizeof(msg));
}

void adaptive_on_advice(const uint8_t* payload, size_t len)
{
	if (!sAdaptiveConfigured) return;
	pc_netplay_adaptive::Advice a;
	if (!pc_netplay_adaptive::advice_decode(payload, len, &a)) return;
	double delta    = 0;
	uint32_t events = 0;
	if (!sAdviceIn.take(a, &delta, &events)) return; // stale or duplicate
	const double now = now_ms();
	sDelayCtl.add_report(now);
	if (delta > 0) {
		sPeerLateMs += delta;
		sDelayCtl.add_lateness(now, delta, events);
	}
}

void adaptive_note_stall_turn(double startMs, double durMs)
{
	if (sAdvances == 0) return; // session start, not a stall
	if (!sStallOpen) {
		sStallOpen     = true;
		sStallStartMs  = startMs;
		sStallDurMs    = 0;
		sStallExcluded = sLgWindow.is_open() || sAdvances < sSettleUntil;
		sStallAhead    = sGekko != nullptr ? gekko_frames_ahead(sGekko) : 0.0f;
	}
	sStallDurMs += durMs;
}

void adaptive_poll(double nowMs)
{
	if (sGekko == nullptr || !sGekkoStarted) return;
	if (sLgWindow.is_open() || sAdvances < sSettleUntil) sDelayCtl.note_pause(nowMs);
	adaptive_send_advice(nowMs);
	if (nowMs < sRttNextMs) return;
	sRttNextMs = nowMs + kRttSampleMs;
	GekkoNetworkStats st;
	memset(&st, 0, sizeof(st));
	gekko_network_stats(sGekko, 1 - sLocalHandle, &st);
	if (st.avg_ping <= 0.0f && st.last_ping == 0) return; // no sample yet
	sStats.add_rtt((double)st.last_ping);
	pc_netplay_adaptive::RttSample s;
	s.atMs  = nowMs;
	s.rttMs = (double)st.last_ping;
	sDelayCtl.add_rtt(s);
}

void adaptive_stats_line(const char* tag)
{
	const double now = now_ms();
	uint64_t n10     = 0;
	double ms10      = 0;
	sStats.recent(now, 10000.0, &n10, &ms10);
	const pc_netplay_adaptive::FrameTimeHist& f = sStats.frames();
	printf("[netplay] %s: t=%.1fs frame=%llu delay=%u (start %u, range %u..%u, up %llu down %llu) "
	       "stalls=%llu total=%.0fms max=%.0fms excluded=%llu last10s=%llu/%.0fms reported=%.0fms "
	       "own-lag=%.0fms peer: delay=%d late=%.0fms reports=%llu "
	       "rtt last=%.0f p50=%.0f p95=%.0f jitter=%.1f samples=%llu "
	       "frames=%llu p50=%.1f p95=%.1f p99=%.1f max=%.1f >50ms=%llu >100ms=%llu\n",
	       tag, session_s(now), (unsigned long long)sAdvances, sCfg.localDelay, sDelayStart, sDelayLow, sDelayHigh,
	       (unsigned long long)sDelayUps, (unsigned long long)sDelayDowns, (unsigned long long)sStats.stall_count(),
	       sStats.stall_total_ms(), sStats.stall_max_ms(), (unsigned long long)sStats.stall_excluded(),
	       (unsigned long long)n10, ms10, sLateMs, sSelfOverrunMs, sAdviceIn.have() ? (int)sAdviceIn.last().delay : -1,
	       sPeerLateMs, (unsigned long long)sAdviceIn.reports(), sStats.rtt_last(), sStats.rtt_percentile(50),
	       sStats.rtt_percentile(95), sStats.rtt_jitter(), (unsigned long long)sStats.rtt_samples(),
	       (unsigned long long)f.count(), f.percentile(50), f.percentile(95), f.percentile(99), f.max_ms(),
	       (unsigned long long)f.at_least(50.0), (unsigned long long)f.at_least(100.0));
	fflush(stdout);
}

void adaptive_final_stats()
{
	if (!sAdaptiveConfigured || sStatsFinalDone || sSessionStartMs <= 0) return;
	sStatsFinalDone = true;
	adaptive_close_stall();
	adaptive_stats_line("stats final");
	printf("[netplay] delay timeline: +0.0s@0:%u%s%s\n", sDelayStart, sDelayTimeline.empty() ? "" : " ",
	       sDelayTimeline.c_str());
	printf("[netplay] frame-time histogram (ms): %s\n", sStats.frames().summary().c_str());
	fflush(stdout);
}

// ---- load guard (pc_netplay_loadguard.h) ----
//   PIKMIN_NETPLAY_LOAD_GUARD=0        (a) and (b) off
//   PIKMIN_NETPLAY_LOAD_KEEPALIVE=0    (a) off (the keep-alive pump)
//   PIKMIN_NETPLAY_LOAD_WINDOW=0       (b) off (the load-window extension)
//   PIKMIN_NETPLAY_LOAD_DISCONNECT_MS  the timeout inside a load window
//                                      (default 60000; never below the normal one)
// plus the PIKMIN_NETPLAY_TEST_STALL_* injector.
void loadguard_configure(unsigned normalMs)
{
	using namespace pc_netplay_loadguard;
	const bool guard  = read_unsigned_env("PIKMIN_NETPLAY_LOAD_GUARD", 1) != 0;
	sLgKeepAlive      = guard && read_unsigned_env("PIKMIN_NETPLAY_LOAD_KEEPALIVE", 1) != 0;
	const bool window = guard && read_unsigned_env("PIKMIN_NETPLAY_LOAD_WINDOW", 1) != 0;
	sLgWindow.configure(window, normalMs,
	                    read_unsigned_env("PIKMIN_NETPLAY_LOAD_DISCONNECT_MS", kDefaultLoadDisconnectMs));
	sLgGate.reset();
	sLgStallDone  = false;
	sLgStageLoads = 0;
	sLgPumps = sLgWindows = sLgLongTicks = 0;
	for (uint64_t& n : sLgSitePumps) n = 0;
	sLgSpeculative   = false;
	sLgLongestTickMs = sLgWorstGapMs = 0;
	sLgSummaryDone   = false;
	sInAdvance       = false;
	sLgMainThread    = std::this_thread::get_id();
	std::string err;
	if (!parse_stall_plan(getenv_nonempty("PIKMIN_NETPLAY_TEST_STALL_MS"), getenv_nonempty("PIKMIN_NETPLAY_TEST_STALL_AT"),
	                      getenv_nonempty("PIKMIN_NETPLAY_TEST_STALL_ROLE"),
	                      getenv_nonempty("PIKMIN_NETPLAY_TEST_STALL_SLICE_MS"), &sLgStall, &err))
		printf("[netplay] test stall: ignored (%s)\n", err.c_str());
	sLgConfigured = true;
	const char* ka = sLgKeepAlive ? "on (network poll at most every 50 ms inside a tick)" : "off";
	if (window)
		printf("[netplay] load guard: keep-alive=%s load window=on (disconnect timeout %u ms from a stage load until "
		       "%u frames later, %u ms otherwise)\n",
		       ka, sLgWindow.load_ms(), (unsigned)kLoadWindowFrames, normalMs);
	else
		printf("[netplay] load guard: keep-alive=%s load window=off (disconnect timeout %u ms throughout)\n", ka,
		       normalMs);
	if (sLgStall.enabled) {
		const char* at = sLgStall.at == StallAt::Load ? "load" : sLgStall.at == StallAt::Shader ? "shader" : "tick";
		printf("[netplay] test stall: armed at=%s#%llu role=%s total=%ums slice=%ums; this peer %s\n", at,
		       (unsigned long long)sLgStall.index, stall_role_name(sLgStall.roles), sLgStall.totalMs,
		       sLgStall.sliceMs, stall_applies_to(sLgStall, sCfg.isHost) ? "stalls" : "does not stall");
	}
	fflush(stdout);
}

// One keep-alive pump inside a tick: GekkoNet's network layer only (receive,
// ack, resend unacked inputs, NetworkHealth, idle-timer check; no Advance,
// no event consumed). A disconnect found here is queued as a session event
// and handled right after this tick.
void loadguard_pump(double now, int site)
{
	if (sGekko != nullptr) gekko_network_poll(sGekko);
	if (site >= 0 && site < pc_netplay_loadguard::kSiteCount) ++sLgSitePumps[site];
	const double gap = now - sLgLastPumpMs;
	if (gap > sLgTickMaxGapMs) sLgTickMaxGapMs = gap;
	sLgLastPumpMs = now;
	sLgGate.pumped(now);
	++sLgTickPumps;
	++sLgPumps;
}

void loadguard_keepalive(int site)
{
	if (!sInAdvance || !sLgKeepAlive || sPhase != kSession || sGekko == nullptr) return;
	if (std::this_thread::get_id() != sLgMainThread) return;
	const double now = now_ms();
	if (!sLgGate.due(now)) return;
	loadguard_pump(now, site);
}

// Test stall (wall clock only): slices separated by the keep-alive entry
// the real long loops call.
void loadguard_run_stall(const char* where)
{
	using namespace pc_netplay_loadguard;
	sLgStallDone = true;
	printf("[netplay] test stall: begin role=%s at=%s frame=%u total=%ums slice=%ums keep-alive=%s\n",
	       sCfg.isHost ? "host" : "join", where, sLgTickFrame, sLgStall.totalMs, sLgStall.sliceMs,
	       sLgKeepAlive ? "on" : "off");
	fflush(stdout);
	const double t0 = now_ms();
	for (unsigned i = 0;; ++i) {
		const unsigned ms = stall_slice_ms(sLgStall, i);
		if (ms == 0) break;
		if (i > 0) loadguard_keepalive(kSiteStall);
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
	}
	printf("[netplay] test stall: end after %.0f ms\n", now_ms() - t0);
	fflush(stdout);
}

void loadguard_maybe_stall(pc_netplay_loadguard::StallAt at, uint64_t index, const char* where)
{
	if (sLgStallDone || !sInAdvance || sLgSpeculative || !stall_applies_to(sLgStall, sCfg.isHost)) return;
	if (sLgStall.at != at || sLgStall.index != index) return;
	loadguard_run_stall(where);
}

void loadguard_tick_begin(uint32_t frame, bool speculative)
{
	sLgSpeculative  = speculative;
	sInAdvance      = true;
	sLgTickFrame    = frame;
	sLgTickStartMs  = now_ms();
	sLgLastPumpMs   = sLgTickStartMs;
	sLgTickMaxGapMs = 0;
	sLgTickPumps    = 0;
	// tick:<n> is the hash tick (GekkoNet frame F is hash tick F+1).
	loadguard_maybe_stall(pc_netplay_loadguard::StallAt::Tick, (uint64_t)frame + 1, "tick");
}

void loadguard_tick_end()
{
	sInAdvance       = false;
	const double now = now_ms();
	const double gap = now - sLgLastPumpMs;
	if (gap > sLgTickMaxGapMs) sLgTickMaxGapMs = gap;
	const double tickMs = now - sLgTickStartMs;
	if (tickMs > sLgLongestTickMs) sLgLongestTickMs = tickMs;
	if (tickMs > pc_netplay_loadguard::kLongTickMs) {
		++sLgLongTicks;
		if (sLgTickMaxGapMs > sLgWorstGapMs) sLgWorstGapMs = sLgTickMaxGapMs;
		printf("[netplay] long tick: frame=%u took %.0f ms; keep-alive pumps=%llu, longest stretch without a network "
		       "poll %.0f ms (load window %s)\n",
		       sLgTickFrame, tickMs, (unsigned long long)sLgTickPumps, sLgTickMaxGapMs,
		       sLgWindow.is_open() ? "open" : "closed");
		fflush(stdout);
	}
	if (stall_trace() && tickMs >= 50.0) {
		printf("[netplay] stall-trace: slow tick frame=%u tick=%.1fms window=%d\n", sLgTickFrame, tickMs,
		       (int)sLgWindow.is_open());
		fflush(stdout);
	}
}

// After each completed Advance: close the load window once frame F+30 ran.
void loadguard_after_advance(uint32_t frame, bool speculative)
{
	if (speculative || !sLgWindow.after_advance(frame)) return;
	if (sGekko != nullptr) gekko_set_disconnect_timeout(sGekko, sLgWindow.normal_ms());
	printf("[netplay] load window: closed at frame=%u (opened at frame=%u, last stage load at frame=%u, %.0f ms "
	       "open); disconnect timeout %u ms\n",
	       frame, sLgWindowFirst, sLgWindow.open_frame(), now_ms() - sLgWindowOpenMs, sLgWindow.normal_ms());
	fflush(stdout);
}

void loadguard_on_stage_load()
{
	if (!sInAdvance || sPhase != kSession || sGekko == nullptr) return;
	if (sLgSpeculative) {
		loadguard_keepalive(pc_netplay_loadguard::kSiteStageLoad);
		return;
	}
	++sLgStageLoads;
	const bool wasOpen = sLgWindow.is_open();
	if (sLgWindow.open(sLgTickFrame)) {
		gekko_set_disconnect_timeout(sGekko, sLgWindow.load_ms());
		sLgWindowOpenMs = now_ms();
		sLgWindowFirst  = sLgTickFrame;
		++sLgWindows;
		printf("[netplay] load window: open at frame=%u (stage load %u); disconnect timeout %u ms until frame=%u\n",
		       sLgTickFrame, sLgStageLoads, sLgWindow.load_ms(), sLgWindow.close_frame());
		fflush(stdout);
	} else if (wasOpen) {
		printf("[netplay] load window: stage load %u at frame=%u extends it to frame=%u\n", sLgStageLoads, sLgTickFrame,
		       sLgWindow.close_frame());
		fflush(stdout);
	}
	loadguard_keepalive(pc_netplay_loadguard::kSiteStageLoad);
	loadguard_maybe_stall(pc_netplay_loadguard::StallAt::Load, sLgStageLoads, "load");
}

void loadguard_summary()
{
	if (!sLgConfigured || sLgSummaryDone) return;
	sLgSummaryDone = true;
	using pc_netplay_loadguard::site_name;
	printf("[netplay] load guard summary: keep-alive pumps=%llu (%s=%llu %s=%llu %s=%llu %s=%llu %s=%llu) load "
	       "windows=%llu long ticks=%llu longest tick=%.0f ms longest stretch without a network poll in a long "
	       "tick=%.0f ms\n",
	       (unsigned long long)sLgPumps, site_name(1), (unsigned long long)sLgSitePumps[1], site_name(2),
	       (unsigned long long)sLgSitePumps[2], site_name(3), (unsigned long long)sLgSitePumps[3], site_name(4),
	       (unsigned long long)sLgSitePumps[4], site_name(5), (unsigned long long)sLgSitePumps[5],
	       (unsigned long long)sLgWindows, (unsigned long long)sLgLongTicks, sLgLongestTickMs, sLgWorstGapMs);
	fflush(stdout);
}

// ---- lifecycle ----

void stop_session()
{
	adaptive_final_stats();
	loadguard_summary();
	pc_netplay_camlead_session_end();
	sInAdvance = false;
	if (sGekko != nullptr) {
		GekkoSession* s = sGekko;
		sGekko          = nullptr;
		gekko_destroy(&s);
	}
	delete sLossy;
	sLossy   = nullptr;
	sAdapter = nullptr;
	delete sLink;
	sLink = nullptr;
	if (sSock != nullptr) {
		sSock->close();
		delete sSock;
		sSock = nullptr;
	}
	netplay_timer_power_opt(false);
}

void request_quit()
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = SDL_QUIT;
	SDL_PushEvent(&ev);
}

void parse_config()
{
	sCfg = Config();
	const char* hostCli = argv_value(sArgc, sArgv, "--netplay-host");
	const char* joinCli = argv_value(sArgc, sArgv, "--netplay-join");
	const char* hostVal = hostCli != nullptr ? hostCli : getenv_nonempty("PIKMIN_NETPLAY_HOST");
	const char* joinVal = joinCli != nullptr ? joinCli : getenv_nonempty("PIKMIN_NETPLAY_JOIN");
	if (hostVal != nullptr && joinVal != nullptr) usage_exit("--netplay-host and --netplay-join are exclusive%s", "");
	if (hostVal != nullptr) {
		char* end    = nullptr;
		const long p = strtol(hostVal, &end, 10);
		if (end == hostVal || *end != '\0' || p <= 0 || p > 65535) usage_exit("bad --netplay-host port %s", hostVal);
		sCfg.isHost   = true;
		sCfg.active   = true;
		sCfg.hostPort = (uint16_t)p;
	} else if (joinVal != nullptr) {
		uint32_t ip   = 0;
		uint16_t port = 0;
		if (!pc_netplay_transport::parse_endpoint(joinVal, &ip, &port))
			usage_exit("bad --netplay-join endpoint %s (want ip:port)", joinVal);
		sCfg.active   = true;
		sCfg.joinIp   = ip;
		sCfg.joinPort = port;
	}
	if (!sCfg.active) {
		sPhase = kIdle;
		return;
	}
	if (const char* d = getenv_nonempty("PIKMIN_NETPLAY_DELAY")) {
		if (std::strcmp(d, "auto") == 0) {
			sCfg.delayAuto = true;
		} else {
			sCfg.localDelay = read_unsigned_env("PIKMIN_NETPLAY_DELAY", 2);
			if (sCfg.localDelay < 1) sCfg.localDelay = 1;
			if (sCfg.localDelay > kMaxLocalDelay) sCfg.localDelay = kMaxLocalDelay;
		}
	}
	sCfg.seed = read_u32_env("PIKMIN_NETPLAY_SEED", 0);
	// --netplay-input keyboard|gamepad[:N]|auto picks the device that feeds
	// this peer's captain; any other value is a scripted input file (pkni v2,
	// tools/netplay/gen_inputs.py), like PIKMIN_NETPLAY_LOCAL_INPUT_FILE.
	if (const char* in = argv_value(sArgc, sArgv, "--netplay-input")) {
		pc_netplay_input_sel::Kind kind = pc_netplay_input_sel::kInputAuto;
		int index                       = 0;
		if (pc_netplay_input_sel::parse_input_spec(in, &kind, &index)) {
			sCfg.inputSpec    = in;
			sCfg.inputKind    = (int)kind;
			sCfg.inputGamepad = index;
			// A gamepad peer keeps its pad while another window has focus (a
			// two-window test on one PC). Must be set before SDL_Init.
			if (kind == pc_netplay_input_sel::kInputGamepad)
				SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
		} else {
			sCfg.localInputFile = in;
		}
	}
	if (sCfg.localInputFile.empty()) {
		if (const char* lif = getenv_nonempty("PIKMIN_NETPLAY_LOCAL_INPUT_FILE")) sCfg.localInputFile = lif;
	}
	if (const char* ea = getenv_nonempty("PIKMIN_NETPLAY_EXIT_AFTER_TICKS")) {
		char* end             = nullptr;
		const unsigned long n = strtoul(ea, &end, 10);
		if (end != ea && *end == '\0' && n > 0) sCfg.exitAfter = n;
	}
	if (pc_vs_direct_requested()) usage_exit("online VS is not supported yet; --vs-map cannot be used with netplay%s", "");
	sLocalRole = sCfg.isHost ? 0 : 1;
	// Each peer presents its own captain full screen: host P1, joiner P2.
	pc_netplay_present_set_local_player_default(sLocalRole);
	pc_netplay_camlead_session_begin(sLocalRole);
	// Both peers boot straight into two-captain co-op with a fresh save.
	PcCoopSwitch sw = pc_coop_switch_parse(sArgc, sArgv);
	sw.coop         = true;
	pc_coop_switch_apply(sw);
	pc_coop_direct_request();
	sPhase = kHandshake;
}

// Run modes the session cannot keep in lockstep yet. They are chosen on the
// new-game prompts the direct boot skips, so this only fires if something
// arms one anyway; both peers see it on the same tick.
const char* unsupported_run_mode()
{
	if (pc_permadeath_active()) return "Permadeath";
	if (pc_hardmode_active()) return "Hard mode";
	if (pc_randomizer_active()) return "the save randomizer";
	if (pc_speedrun_active()) return "Speedrun mode";
	if (pc_vs_active()) return "VS";
	return nullptr;
}

// ---- handshake ----

void hs_send(const uint8_t* msg, size_t len)
{
	if (sSock == nullptr) return;
	if (sCfg.isHost) {
		if (sHaveRemote) sSock->send_to(pc_netplay_transport::kChannelHandshake, msg, len, sRemoteIp, sRemotePort);
	} else {
		sSock->send_payload(pc_netplay_transport::kChannelHandshake, msg, len);
	}
}

std::vector<pc_netplay_transport::UdpSocket::Datagram> hs_drain()
{
	if (sLink != nullptr) return sLink->drain_handshake();
	return std::vector<pc_netplay_transport::UdpSocket::Datagram>();
}

void send_hello_msg(uint8_t type, uint8_t refuseField, uint64_t nonce)
{
	uint8_t msg[kHsLen];
	Hello h = sLocal;
	h.nonce = nonce;
	xfer::encode_hello(type, kProtocolVersion, h, refuseField, msg);
	if (type == xfer::kHsAck && sPhase == kHandshake) {
		if (!sDropFinalAckInit) {
			sDropFinalAckInit = true;
			sDropFinalAck     = env_is_one("PIKMIN_NETPLAY_TEST_DROP_FINAL_ACK");
		}
		if (sDropFinalAck) {
			printf("[netplay] test: dropped final handshake Ack (echo=%llu)\n", (unsigned long long)nonce);
			fflush(stdout);
			return;
		}
	}
	hs_send(msg, sizeof(msg));
}

void log_refusal_detail(uint8_t field, const Hello* peer)
{
	if (peer == nullptr) return;
	if (field == xfer::kFieldExe)
		printf("[netplay] refusal: exe local=%s peer=%s\n", sExeHexStr.c_str(), pc_netplay_sha::hex(peer->exe, 32).c_str());
	else if (field == xfer::kFieldConfig)
		printf("[netplay] refusal: config local=%s peer=%s; local config text: %s\n", sCfgHexStr.c_str(),
		       pc_netplay_sha::hex(peer->cfg, 32).c_str(), sCfgText.c_str());
	else if (field == xfer::kFieldSeed)
		printf("[netplay] refusal: seed local=%u peer=%u (PIKMIN_NETPLAY_SEED)\n", sLocal.seed, peer->seed);
	else if (field == xfer::kFieldMode)
		printf("[netplay] refusal: mode local=%s %d,%d peer=%s %d,%d\n", xfer::mode_name(sLocal.mode.mode),
		       sLocal.mode.captainP1, sLocal.mode.captainP2, xfer::mode_name(peer->mode.mode), peer->mode.captainP1,
		       peer->mode.captainP2);
}

[[noreturn]] void refuse_and_exit(uint8_t field, const Hello* peer = nullptr)
{
	printf("[netplay] handshake refused: %s\n", xfer::field_name(field));
	log_refusal_detail(field, peer);
	fflush(stdout);
	// Best effort: tell the peer why (the channel is unreliable).
	for (int i = 0; i < 5; ++i) send_hello_msg(xfer::kHsRefuse, field, 0);
	stop_session();
	std::exit(npx::kRefused);
}

[[noreturn]] void peer_refused_exit(uint8_t field)
{
	printf("[netplay] handshake refused: %s (by the peer)\n", xfer::field_name(field == 0 ? 99 : field));
	fflush(stdout);
	stop_session();
	std::exit(npx::kRefused);
}

double handshake_rtt_median()
{
	if (sHsSamples.empty()) return -1;
	std::vector<double> v = sHsSamples;
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

// Returns true once the GekkoNet session may start.
bool handshake_pump()
{
	const double now = now_ms();
	if (sHsStartMs == 0) {
		sHsStartMs    = now;
		sHsLastSendMs = 0;
	}
	const double timeoutMs = (double)read_unsigned_env("PIKMIN_NETPLAY_HANDSHAKE_TIMEOUT_MS", 30000);
	if (now - sHsStartMs > timeoutMs) {
		printf("[netplay] handshake timeout after %.0f ms (sentAck=%d gotAck=%d rttSamples=%llu)\n", now - sHsStartMs,
		       (int)sSentAck, (int)sGotAck, (unsigned long long)sHsSamples.size());
		fflush(stdout);
		stop_session();
		std::exit(npx::kRefused);
	}
	if (now - sHsLastSendMs >= 100) {
		const uint64_t nonce = sHsNextNonce++;
		send_hello_msg(xfer::kHsHello, 0, nonce);
		sHsSendTimes.emplace_back(nonce, now);
		if (sHsSendTimes.size() > 64) sHsSendTimes.erase(sHsSendTimes.begin(), sHsSendTimes.begin() + (sHsSendTimes.size() - 64));
		// Periodic Ack resends echo nonce 0 (ignored for RTT); they exist only
		// so a lost Ack cannot stall the peer.
		if (sSentAck) send_hello_msg(xfer::kHsAck, 0, 0);
		sHsLastSendMs = now;
	}
	for (auto& g : hs_drain()) {
		uint8_t hdrType   = 0;
		uint16_t hdrProto = 0;
		if (!xfer::decode_hello_header(g.payload.data(), g.payload.size(), &hdrType, &hdrProto)) continue;
		if (sCfg.isHost && !sHaveRemote) {
			// Learn the joiner's endpoint from its first header-valid
			// datagram (before any refusal, so a refuse can go back).
			sRemoteIp   = g.fromIpHostOrder;
			sRemotePort = g.fromPort;
			sHaveRemote = true;
			sSock->set_peer(sRemoteIp, sRemotePort);
			printf("[netplay] handshake: peer %u.%u.%u.%u:%u\n", (sRemoteIp >> 24) & 0xFF, (sRemoteIp >> 16) & 0xFF,
			       (sRemoteIp >> 8) & 0xFF, sRemoteIp & 0xFF, (unsigned)sRemotePort);
			fflush(stdout);
		}
		if (sCfg.isHost && (g.fromIpHostOrder != sRemoteIp || g.fromPort != sRemotePort)) continue;
		// The stable header first: a protocol mismatch refuses fast even
		// across length changes.
		if (hdrProto != kProtocolVersion) {
			if (hdrType == xfer::kHsRefuse && g.payload.size() > xfer::kHsRefuseOffset)
				peer_refused_exit(g.payload[xfer::kHsRefuseOffset]);
			refuse_and_exit(xfer::kFieldProto);
		}
		if (hdrType == kHsAdvice) continue;
		uint8_t type   = 0;
		uint16_t proto = 0;
		Hello h;
		uint8_t refuse = 0;
		if (!xfer::decode_hello(g.payload.data(), g.payload.size(), &type, &proto, &h, &refuse)) continue;
		if (type == xfer::kHsRefuse) peer_refused_exit(refuse);
		if (type != xfer::kHsHello && type != xfer::kHsAck) continue;
		const uint8_t field = xfer::first_mismatch(sLocal, h);
		if (field != 0) refuse_and_exit(field, &h);
		if (type == xfer::kHsHello) {
			// Ack every Hello at once, echoing its nonce.
			send_hello_msg(xfer::kHsAck, 0, h.nonce);
			sSentAck = true;
		} else {
			sGotAck = true;
			if (h.nonce == 0) continue;
			for (auto& st : sHsSendTimes) {
				if (st.first == h.nonce) {
					const double sample = now - st.second;
					if (sample >= 0 && sample < 60000) sHsSamples.push_back(sample);
					break;
				}
			}
		}
	}
	// DELAY=auto needs at least 5 RTT samples for a meaningful median.
	if (sCfg.delayAuto && sSentAck && sGotAck && sHsSamples.size() < 5) return false;
	return sSentAck && sGotAck;
}

// Late or duplicate handshake traffic once the session is up: a peer whose
// last Ack was lost keeps sending Hello, so every Hello gets an Ack (never
// an Ack for an Ack, or they would bounce for the whole session). Also the
// peer's stall advice.
void answer_handshake_in_session()
{
	if (sLink == nullptr || sSock == nullptr) return;
	for (auto& g : hs_drain()) {
		uint8_t hdrType   = 0;
		uint16_t hdrProto = 0;
		if (!xfer::decode_hello_header(g.payload.data(), g.payload.size(), &hdrType, &hdrProto)) continue;
		if (hdrProto != kProtocolVersion) continue;
		if (sCfg.isHost && sHaveRemote && (g.fromIpHostOrder != sRemoteIp || g.fromPort != sRemotePort)) continue;
		if (hdrType == kHsAdvice) {
			adaptive_on_advice(g.payload.data() + kHsHeaderLen, g.payload.size() - kHsHeaderLen);
			continue;
		}
		uint8_t type   = 0;
		uint16_t proto = 0;
		Hello h;
		uint8_t refuse = 0;
		if (!xfer::decode_hello(g.payload.data(), g.payload.size(), &type, &proto, &h, &refuse)) continue;
		if (type != xfer::kHsHello) continue;
		send_hello_msg(xfer::kHsAck, 0, h.nonce);
		printf("[netplay] answered in-session Hello with Ack (echo=%llu)\n", (unsigned long long)h.nonce);
		fflush(stdout);
	}
}

// ---- the GekkoNet session ----

void start_gekko_session()
{
	sGekko = nullptr;
	if (!gekko_create(&sGekko, GekkoGameSession) || sGekko == nullptr) {
		printf("[netplay] gekko_create failed\n");
		fflush(stdout);
		std::exit(npx::kTransport);
	}
	// The lossy test wrapper only when impairments are configured.
	pc_netplay_transport::LossyParams lp;
	lp.latencyMs      = read_double_env("PIKMIN_NETPLAY_TEST_LATENCY_MS", 0.0);
	lp.jitterMs       = read_double_env("PIKMIN_NETPLAY_TEST_JITTER_MS", 0.0);
	lp.lossPct        = read_double_env("PIKMIN_NETPLAY_TEST_LOSS_PCT", 0.0);
	lp.reorderPct     = read_double_env("PIKMIN_NETPLAY_TEST_REORDER_PCT", 0.0);
	lp.reorderExtraMs = read_double_env("PIKMIN_NETPLAY_TEST_REORDER_MS", 0.0);
	lp.seed           = read_u32_env("PIKMIN_NETPLAY_TEST_SEED", 0);
	if (lp.latencyMs < 0) lp.latencyMs = 0;
	if (lp.jitterMs < 0) lp.jitterMs = 0;
	if (lp.lossPct < 0) lp.lossPct = 0;
	if (lp.lossPct > 100) lp.lossPct = 100;
	const bool lossy = lp.latencyMs > 0 || lp.jitterMs > 0 || lp.lossPct > 0 || (lp.reorderPct > 0 && lp.reorderExtraMs > 0);
	GekkoNetAdapter* base = sLink->adapter();
	if (lossy) {
		sLossy   = new pc_netplay_transport::LossyLink(base, lp);
		sAdapter = sLossy->adapter();
		printf("[netplay] lossy adapter: latency=%.1fms jitter=%.1fms loss=%.1f%% seed=%u\n", lp.latencyMs, lp.jitterMs,
		       lp.lossPct, lp.seed);
	} else {
		sAdapter = base;
	}
	GekkoConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.num_players             = 2;
	cfg.max_spectators          = 0;
	cfg.input_prediction_window = 0; // lockstep
	cfg.spectator_delay         = 0;
	cfg.input_size              = 16;
	cfg.state_size              = 8; // 8-byte handle {frame}
	cfg.limited_saving          = false;
	cfg.desync_detection        = true;
	cfg.check_distance          = 7;
	gekko_start(sGekko, &cfg);
	// The adapter must be set after gekko_start: Init() clears it.
	gekko_net_adapter_set(sGekko, sAdapter);
	// Role-ordered actors, so handle 0 is the host/P1 and handle 1 the
	// joiner/P2 on both peers.
	const uint32_t ip   = sCfg.isHost ? sRemoteIp : sCfg.joinIp;
	const uint16_t port = sCfg.isHost ? sRemotePort : sCfg.joinPort;
	sRemoteAddrBlob[0]  = (uint8_t)((ip >> 24) & 0xFF);
	sRemoteAddrBlob[1]  = (uint8_t)((ip >> 16) & 0xFF);
	sRemoteAddrBlob[2]  = (uint8_t)((ip >> 8) & 0xFF);
	sRemoteAddrBlob[3]  = (uint8_t)(ip & 0xFF);
	sRemoteAddrBlob[4]  = (uint8_t)((port >> 8) & 0xFF);
	sRemoteAddrBlob[5]  = (uint8_t)(port & 0xFF);
	GekkoNetAddress raddr;
	raddr.data = sRemoteAddrBlob;
	raddr.size = 6;
	if (sCfg.isHost) {
		sLocalHandle = gekko_add_actor(sGekko, GekkoLocalPlayer, nullptr);
		(void)gekko_add_actor(sGekko, GekkoRemotePlayer, &raddr);
	} else {
		(void)gekko_add_actor(sGekko, GekkoRemotePlayer, &raddr);
		sLocalHandle = gekko_add_actor(sGekko, GekkoLocalPlayer, nullptr);
	}
	unsigned disconnectMs = read_unsigned_env("PIKMIN_NETPLAY_DISCONNECT_MS", 15000);
	if (disconnectMs < 1000) disconnectMs = 1000;
	gekko_set_disconnect_timeout(sGekko, disconnectMs);
	printf("[netplay] disconnect timeout: %ums\n", disconnectMs);
	loadguard_configure(disconnectMs);
	pc_state_hash_set_netplay_capture(true);
	load_scripted_file();
	sNaviPosTrace = env_is_one("PIKMIN_NETPLAY_DEBUG_NAVI_POS");
	// DELAY=auto: ceil((RTT/2)/33.333 ms - 0.05) + 1 from the median
	// handshake RTT, clamped to 1..8. The 0.05-slot tolerance keeps exact
	// boundaries from flipping one frame high on measurement overhead.
	if (sCfg.delayAuto) {
		const double rttMed = handshake_rtt_median();
		unsigned d          = 2;
		if (rttMed >= 0) d = pc_netplay_adaptive::delay_for_rtt(rttMed, 1, kMaxLocalDelay);
		sCfg.localDelay = d;
		printf("[netplay] auto delay: rtt=%.1fms (median of %llu) delay=%u\n", rttMed,
		       (unsigned long long)sHsSamples.size(), d);
	}
	gekko_set_local_delay(sGekko, sLocalHandle, (unsigned char)sCfg.localDelay);
	printf("[netplay] session started: role=%s localHandle=%d delay=%u seed=%u mode=%s captains=%d,%d\n",
	       sCfg.isHost ? "host/P1" : "joiner/P2", sLocalHandle, sCfg.localDelay, sCfg.seed,
	       xfer::mode_name(sLocal.mode.mode), sLocal.mode.captainP1, sLocal.mode.captainP2);
	adaptive_configure();
	printf("[netplay] exe=%s\n", sExeHexStr.c_str());
	printf("[netplay] config=%s\n", sCfgHexStr.c_str());
	fflush(stdout);
	if (const char* e = getenv_nonempty("PIKMIN_NETPLAY_TEST_DESYNC_AT_FRAME")) {
		char* end         = nullptr;
		const long long v = strtoll(e, &end, 10);
		if (end != e && *end == '\0' && v >= 0) sTestDesyncFrame = v;
	}
	sSessionStartMs = now_ms();
	// The drift-free deadline starts here, so the first turn never overruns.
	sNextTurnMs = sSessionStartMs + 1000.0 / 30.0;
	sPhase      = kSession;
}

// GekkoNet reported a desync: print the desynced tick's sub-hashes, keep
// polling for a moment so this peer's own checksum reaches the other game
// (both then stop with exit 5), and exit.
[[noreturn]] void handle_desync(int frame, uint32_t localCk, uint32_t remoteCk, int remoteHandle)
{
	const uint64_t wantTick = frame >= 0 ? (uint64_t)frame + 1 : 0;
	const HashEntry* e      = hash_ring_find(wantTick);
	uint64_t total = 0, subs[7] = { 0, 0, 0, 0, 0, 0, 0 }, tick = 0;
	if (e != nullptr) {
		total = e->total;
		tick  = e->tick;
		for (int k = 0; k < 7; ++k) subs[k] = e->subs[k];
	} else {
		pc_state_hash_current(&total, subs, &tick);
	}
	printf("[netplay] desync detected: frame=%d local=%08x remote=%08x handle=%d\n", frame, localCk, remoteCk,
	       remoteHandle);
	printf("[netplay] desync subs at tick=%llu%s: total=%016llx (fold %08x) navi=%016llx piki=%016llx teki=%016llx "
	       "item=%016llx world=%016llx rng=%016llx mode=%016llx\n",
	       (unsigned long long)tick, e != nullptr ? "" : " (ring miss: latest)", (unsigned long long)total,
	       fold_hash64(total), (unsigned long long)subs[0], (unsigned long long)subs[1], (unsigned long long)subs[2],
	       (unsigned long long)subs[3], (unsigned long long)subs[4], (unsigned long long)subs[5],
	       (unsigned long long)subs[6]);
	fflush(stdout);
	pc_state_hash_flush();
	const double t0 = now_ms();
	while (sGekko != nullptr && now_ms() - t0 < 1000.0) {
		gekko_network_poll(sGekko);
		sleep_hires_ms(5.0, 0.0);
	}
	stop_session();
	sPhase = kDone;
	printf("[netplay] session stopped: desync (exit %d)\n", npx::kDesync);
	fflush(stdout);
	std::exit(npx::kDesync);
}

void handle_session_events()
{
	if (sGekko == nullptr) return;
	int n                  = 0;
	GekkoSessionEvent** ev = gekko_session_events(sGekko, &n);
	if (ev == nullptr || n <= 0) return;
	for (int i = 0; i < n; ++i) {
		if (ev[i] == nullptr) continue;
		switch (ev[i]->type) {
		case GekkoPlayerConnected:
			printf("[netplay] connected: handle=%d\n", ev[i]->data.connected.handle);
			break;
		case GekkoSessionStarted:
			printf("[netplay] gekko session started\n");
			sGekkoStarted = true;
			break;
		case GekkoPlayerDisconnected: {
			printf("[netplay] disconnected: handle=%d\n", ev[i]->data.disconnected.handle);
			const double sinceAdvanceMs = sLastAdvanceWallMs > 0 ? now_ms() - sLastAdvanceWallMs : 1e9;
			const bool peerQuit = sinceAdvanceMs < std::min(2500.0, 0.5 * (double)sLgWindow.normal_ms());
			printf("[netplay] session ended: %s at frame=%u\n",
			       peerQuit ? "the other player left" : "connection lost", sLastAdvanceFrame);
			fflush(stdout);
			pc_state_hash_flush();
			stop_session();
			sPhase = kDone;
			request_quit();
			return; // the session is gone: stop processing this batch
		}
		case GekkoDesyncDetected:
			handle_desync(ev[i]->data.desynced.frame, ev[i]->data.desynced.local_checksum,
			              ev[i]->data.desynced.remote_checksum, ev[i]->data.desynced.remote_handle);
		default:
			break;
		}
	}
	fflush(stdout);
}

// A local quit (the window closed mid-session) tells the other game at once
// instead of letting it wait for its disconnect timeout.
void send_quit_notice()
{
	if (sGekko == nullptr || !sGekkoStarted) return;
	gekko_disconnect_actor(sGekko, sLocalHandle);
	const double t0 = now_ms();
	while (now_ms() - t0 < 250.0) {
		gekko_network_poll(sGekko);
		sleep_hires_ms(5.0, 0.0);
	}
	printf("[netplay] quit: this game left the session; the other game was told\n");
	fflush(stdout);
}

// One simulated frame from a pair of 16-byte inputs (host p0, joiner p1).
// `frame` is the GekkoNet frame (hash tick frame + 1). Returns false when the
// session ended inside it.
bool run_advance(System* sys, BaseApp* app, int frame, const uint8_t* inputs, bool speculative)
{
	PcNetplayInput p0, p1;
	if (!pc_netplay_input_decode(inputs, 16, p0) || !pc_netplay_input_decode(inputs + 16, 16, p1)) {
		printf("[netplay] advance decode failed\n");
		fflush(stdout);
		std::abort();
	}
	inject_input(0, p0);
	inject_input(1, p1);
	inject_neutral_pad(2);
	inject_neutral_pad(3);
	// The per-tick work of the normal path, once per Advance: the audio event
	// timers (Jac_Gsync) and the thread liveness check.
	Jac_Gsync();
	(void)OSCheckActiveThreads();
	sys->updateSysClock();
	pc_netplay_on_tick_begin();
	pc_netplay_camlead_check_applied((uint64_t)frame, inputs + 16 * sLocalRole);
	pc_netplay_camlead_begin_frame((uint64_t)frame);
	loadguard_tick_begin((uint32_t)frame, speculative);
	app->idle();
	loadguard_tick_end();
	pc_netplay_det_profile_note_tick();
	pc_input_log_tick_end();
	pc_state_hash_tick_end();
	{
		uint64_t total = 0, subs[7] = { 0, 0, 0, 0, 0, 0, 0 }, tick = 0;
		if (pc_state_hash_current(&total, subs, &tick) && tick > 0) hash_ring_store(tick, total, subs);
	}
	sLastAdvanceFrame = (uint32_t)frame;
	loadguard_after_advance((uint32_t)frame, speculative);
	++sSessionTicks;
	++sAdvances;
	adaptive_note_frame();
	// Test evidence (PIKMIN_NETPLAY_DEBUG_NAVI_POS=1): every captain's
	// position, so a pair run can prove both captains really moved.
	if (sNaviPosTrace && sSessionTicks % 150 == 0) pc_state_hash_print_navi_positions(sSessionTicks);
	if (const char* what = unsupported_run_mode()) {
		printf("[netplay] %s is not supported online yet; stopping the session at frame=%d\n", what, frame);
		fflush(stdout);
		pc_state_hash_flush();
		stop_session();
		std::exit(npx::kUsage);
	}
	if (sCfg.exitAfter > 0 && sSessionTicks >= sCfg.exitAfter) {
		const double wallS = sSessionStartMs > 0 ? (now_ms() - sSessionStartMs) / 1000.0 : 0.0;
		const double tps   = wallS > 0 ? (double)sAdvances / wallS : 0.0;
		const double stallPct    = wallS > 0 ? 100.0 * sStallMs / (wallS * 1000.0) : 0.0;
		const double slotLossPct = tps >= 30.0 ? 0.0 : 100.0 * (1.0 - tps / 30.0);
		printf("[netplay] exit after %llu ticks\n", (unsigned long long)sSessionTicks);
		printf("[netplay] script records consumed: %llu/%llu\n", (unsigned long long)sScriptIdx,
		       (unsigned long long)sScriptTicks);
		printf("[netplay] wall=%.1fs tps=%.1f stall=%.1f%% slot-loss=%.1f%% vs 30Hz\n", wallS, tps, stallPct,
		       slotLossPct);
		fflush(stdout);
		pc_state_hash_flush();
		stop_session();
		sPhase = kDone;
		request_quit();
		return false;
	}
	return true;
}

// Runs one tick per Advance event, in event order. Returns the number of
// Advances processed (0 while waiting for remote input).
int handle_game_events(System* sys, BaseApp* app)
{
	int count           = 0;
	GekkoGameEvent** ev = gekko_update_session(sGekko, &count);
	if (ev == nullptr || count <= 0) return 0;
	int advances = 0;
	for (int i = 0; i < count; ++i) {
		if (sPhase != kSession || sGekko == nullptr) break;
		if (ev[i] == nullptr) continue;
		switch (ev[i]->type) {
		case GekkoAdvanceEvent: {
			GekkoGameEvent* e = ev[i];
			if (e->data.adv.input_len != 32 || e->data.adv.inputs == nullptr) {
				printf("[netplay] bad advance: input_len=%u\n", e->data.adv.input_len);
				fflush(stdout);
				std::abort();
			}
			run_advance(sys, app, e->data.adv.frame, e->data.adv.inputs,
			            e->data.adv.rolling_back || e->data.adv.running_ahead);
			++advances;
			break;
		}
		case GekkoSaveEvent: {
			GekkoGameEvent* e = ev[i];
			uint64_t total = 0, subs[7] = { 0, 0, 0, 0, 0, 0, 0 }, tick = 0;
			pc_state_hash_current(&total, subs, &tick);
			if (e->data.save.state != nullptr && e->data.save.state_len != nullptr && *e->data.save.state_len >= 8) {
				const uint64_t frame = (uint64_t)e->data.save.frame;
				for (int b = 0; b < 8; ++b) e->data.save.state[b] = (uint8_t)((frame >> (b * 8)) & 0xFF);
				*e->data.save.state_len = 8;
			}
			if (e->data.save.checksum != nullptr) {
				*e->data.save.checksum = fold_hash64(total);
				if (sTestDesyncFrame >= 0 && (int64_t)e->data.save.frame >= sTestDesyncFrame) {
					*e->data.save.checksum ^= 0x5A5A5A5Au;
					if (!sTestDesyncLogged) {
						sTestDesyncLogged = true;
						printf("[netplay] test: this peer reports altered checksums from frame=%d (the sim is "
						       "unchanged)\n",
						       e->data.save.frame);
						fflush(stdout);
					}
				}
			}
			++sSaves;
			break;
		}
		case GekkoLoadEvent:
			// Window 0 never rolls back: a Load means the lockstep contract broke.
			printf("[netplay] unexpected load event at frame=%d (window 0 forbids loads)\n", ev[i]->data.load.frame);
			fflush(stdout);
			std::abort();
			break;
		default:
			break;
		}
	}
	if (sPhase == kSession && sSessionTicks > 0 && sSessionTicks % 300 == 0 && advances > 0) {
		const double wallS = sSessionStartMs > 0 ? (now_ms() - sSessionStartMs) / 1000.0 : 0.0;
		const double tps   = wallS > 0 ? (double)sAdvances / wallS : 0.0;
		printf("[netplay] tick=%llu adv=%llu stalls=%llu tps=%.1f ahead=%.2f saves=%llu submitted=%llu delay=%u\n",
		       (unsigned long long)sSessionTicks, (unsigned long long)sAdvances, (unsigned long long)sStalls, tps,
		       gekko_frames_ahead(sGekko), (unsigned long long)sSaves, (unsigned long long)sSubmitted,
		       sCfg.localDelay);
		fflush(stdout);
		adaptive_stats_line("stats");
	}
	return advances;
}

// First turn: force the modes, hash the local Hello and bring the socket up.
void session_setup(System* sys)
{
	pc_netplay_det_force_on();
	pc_settings_netplay_session_begin();
	apply_input_selection();
	compute_local_hello();
	printf("[netplay] mode=%s delay=%s seed=%u bind=%s\n", sCfg.isHost ? "host" : "join",
	       sCfg.delayAuto ? "auto" : std::to_string(sCfg.localDelay).c_str(), sCfg.seed,
	       getenv_nonempty("PIKMIN_NETPLAY_UDP_BIND") != nullptr ? getenv_nonempty("PIKMIN_NETPLAY_UDP_BIND")
	                                                             : "127.0.0.1");
	printf("[netplay] config text: %s\n", sCfgText.c_str());
	fflush(stdout);
	sSock = new pc_netplay_transport::UdpSocket();
	if (!sSock->bind(sCfg.isHost ? sCfg.hostPort : 0)) {
		printf("[netplay] bind %s failed\n", sCfg.isHost ? "host port" : "ephemeral port");
		fflush(stdout);
		std::exit(npx::kTransport);
	}
	if (!sCfg.isHost) sSock->set_peer(sCfg.joinIp, sCfg.joinPort);
	if (getenv_nonempty("PIKMIN_NETPLAY_UDP_BIND") == nullptr && (sCfg.isHost || (sCfg.joinIp >> 24) != 127)) {
		printf("[netplay] note: bound to 127.0.0.1 only; for a game over the network set "
		       "PIKMIN_NETPLAY_UDP_BIND=0.0.0.0 on both PCs\n");
	}
	printf("[netplay] udp bound on port %u\n", (unsigned)sSock->local_port());
	fflush(stdout);
	sLink = new pc_netplay_transport::GekkoLink(sSock);
	netplay_timer_power_opt(true);
	(void)sys;
}

} // namespace

bool pc_netplay_is_host(void) { return sCfg.active && sCfg.isHost; }

bool pc_netplay_live_stats(PcNetplayLiveStats* out)
{
	if (out == nullptr || sPhase != kSession || !sAdaptiveConfigured) return false;
	uint64_t n10     = 0;
	double ms10      = 0;
	const double now = now_ms();
	sStats.recent(now, 10000.0, &n10, &ms10);
	out->delay          = sCfg.localDelay;
	out->adaptive       = sAdaptive ? 1 : 0;
	out->rttLastMs      = (float)sStats.rtt_last();
	out->rttP50Ms       = (float)sStats.rtt_percentile(50);
	out->jitterMs       = (float)sStats.rtt_jitter();
	out->stallsLast10s  = (unsigned)n10;
	out->stallMsLast10s = (float)ms10;
	out->stallCount     = (unsigned long long)sStats.stall_count();
	out->stallTotalMs   = (float)sStats.stall_total_ms();
	out->stallOpen      = sStallOpen ? 1 : 0;
	return true;
}

void pc_netplay_session_notify_argv(int argc, char** argv)
{
	sArgc = argc;
	sArgv = argv;
	parse_config();
	sInitialised = true;
}

// Stage-load hook (GameFlow::softReset, inside the synchronous load that
// runs within app->idle() during one Advance). PIKMIN_NETPLAY_TEST_LOAD_DELAY_MS
// sleeps once, unpumped, to exercise the load window alone.
void pc_netplay_on_stage_load(void)
{
	if (!sInitialised || !sCfg.active) return;
	loadguard_on_stage_load();
	if (sLoadDelayDone) return;
	const unsigned n = read_unsigned_env("PIKMIN_NETPLAY_TEST_LOAD_DELAY_MS", 0);
	if (n == 0) return;
	sLoadDelayDone = true;
	printf("[netplay] test load delay: sleeping %ums inside stage load\n", n);
	fflush(stdout);
	std::this_thread::sleep_for(std::chrono::milliseconds(n));
	printf("[netplay] test load delay: done\n");
	fflush(stdout);
}

void pc_netplay_load_keepalive(int site)
{
	if (!sInAdvance) return;
	if (std::this_thread::get_id() != sLgMainThread) return; // the DVD, audio and loading threads
	if (site == pc_netplay_loadguard::kSiteShader) loadguard_maybe_stall(pc_netplay_loadguard::StallAt::Shader, 1, "shader");
	loadguard_keepalive(site);
}

bool pc_netplay_session_active(void) { return sInitialised && sCfg.active; }

bool pc_netplay_session_drive(System* sys, BaseApp* app)
{
	if (!pc_netplay_session_active()) return false;
	if (sys == nullptr || app == nullptr) return false;

	if (!sForcedModes) {
		sForcedModes = true;
		session_setup(sys);
	}

	if (pc_window_should_close()) {
		// Relinquish the loop so System::run breaks at its should_close
		// check and shuts down normally.
		if (sPhase != kDone) {
			const bool inSession = sPhase == kSession && sGekko != nullptr && sGekkoStarted;
			if (inSession) send_quit_notice();
			pc_state_hash_flush();
			stop_session();
			sPhase = kDone;
		}
		return false;
	}

	if (sPhase == kDone) {
		// Drain the quit event requested above (nothing else polls while the
		// driver owns the loop), then relinquish so the loop breaks.
		pc_window_poll_events(nullptr);
		if (pc_window_should_close()) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return true;
	}

	if (sPhase == kHandshake) {
		// mControllerMgr.update() pumps the window through PADRead. Poll
		// about every 1 ms on the waitable timer, so the RTT samples measure
		// the wire, not the poll phase, and a waiting host idles.
		sys->mControllerMgr.update();
		if (handshake_pump()) start_gekko_session();
		if (sPhase == kHandshake) sleep_hires_ms(1.0, 0.0);
		return true;
	}

	// kSession.
	const bool unthrottled   = pc_netplay_unthrottled();
	const double turnStartMs = now_ms();
	adaptive_note_turn_start(turnStartMs);
	sys->mControllerMgr.update();
	int advances = 0;
	if (sGekko != nullptr) {
		answer_handshake_in_session();
		accum_add_current();
		// GekkoNet accepts exactly one local input per frame, so a submit is
		// due only when the next local input lands on sAdvances + delay. On
		// a due turn the adaptive delay may shrink the delay (skipping this
		// and the next submits) or grow it (one input per new frame now).
		if (sGekkoStarted) {
			unsigned grow = 0;
			if (pc_netplay_adaptive::submit_due(sNextLand, sAdvances, sCfg.localDelay)) {
				const unsigned target = adaptive_target();
				if (target < sCfg.localDelay) adaptive_shrink(target);
				else grow = target - sCfg.localDelay;
			}
			if (pc_netplay_adaptive::submit_due(sNextLand, sAdvances, sCfg.localDelay)) submit_local_inputs(1 + grow);
		}
		advances = handle_game_events(sys, app);
		handle_session_events();
		if (sPhase != kSession) return true; // stopped inside the handlers
	}
	// Pacing (see the top of the file). At most 2 slots of catch-up per turn,
	// and a snap forward when more than 5 slots behind (a long load).
	double nextDueMs = 0;
	bool haveDue     = false;
	if (!unthrottled) {
		constexpr double kSlotMs          = 1000.0 / 30.0;
		constexpr double kMaxCatchupSlots = 5.0;
		constexpr double kMaxAdvanceSlots = 2.0;
		if (advances > 0) {
			const float aheadNow = (sGekko != nullptr) ? gekko_frames_ahead(sGekko) : 0.0f;
			double extraMs       = 0.0;
			if (aheadNow > 0.75f) {
				extraMs          = (double)(aheadNow - 0.75f) * kSlotMs * 0.05;
				const double cap = kSlotMs * 0.10;
				if (extraMs > cap) extraMs = cap;
			}
			double effAdv = (double)advances;
			if (effAdv > kMaxAdvanceSlots) effAdv = kMaxAdvanceSlots;
			const double now = now_ms();
			if (sNextTurnMs == 0) sNextTurnMs = now + kSlotMs;
			nextDueMs = sNextTurnMs;
			haveDue   = true;
			if (now < sNextTurnMs) {
				const double wait = sNextTurnMs - now;
				if (wait > 0.05) sleep_hires_ms(wait);
				sNextTurnMs += kSlotMs * effAdv + extraMs;
			} else if (now - sNextTurnMs > kMaxCatchupSlots * kSlotMs) {
				sNextTurnMs = now + kSlotMs;
				nextDueMs   = now;
			} else {
				sNextTurnMs += kSlotMs * effAdv + extraMs;
			}
		} else {
			// A waiting turn keeps pumping without spending a whole slot.
			sleep_hires_ms(1.0, 0.0);
		}
	}
	if (advances > 0) {
		sLastAdvanceWallMs = now_ms();
		adaptive_close_stall();
	} else {
		++sStalls;
		const double turnMs = now_ms() - turnStartMs;
		sStallMs += turnMs;
		adaptive_note_stall_turn(turnStartMs, turnMs);
	}
	adaptive_note_turn_end(advances > 0, haveDue, nextDueMs);
	adaptive_poll(now_ms());
	return true;
}
