#pragma once
// One player's input for one lockstep frame: a fixed 16-byte record,
// little-endian, serialised field by field (never a struct memcpy):
//   offset  size  field
//   0       2     buttons u16 (PAD_BUTTON_* bits)
//   2       1     stickX s8
//   3       1     stickY s8
//   4       1     substickX s8
//   5       1     substickY s8
//   6       1     triggerL u8
//   7       1     triggerR u8
//   8       2     control yaw u16 (pc_input_log_yaw_quantise)
//   10      6     flags, fragSeq, fragData[4]: spare, zero for now
//
// Engine-free: only <cstddef>/<cstdint>.

#include <cstddef>
#include <cstdint>

namespace pc_netplay_gekko {
constexpr size_t kInputBytes = 16;
} // namespace pc_netplay_gekko

struct PcNetplayInput {
	uint16_t buttons = 0;
	int8_t stickX = 0;
	int8_t stickY = 0;
	int8_t substickX = 0;
	int8_t substickY = 0;
	uint8_t triggerL = 0;
	uint8_t triggerR = 0;
	uint16_t controlYaw = 0;
	uint8_t flags = 0;
	uint8_t fragSeq = 0;
	uint8_t fragData[4] = {};
};

// Encodes exactly 16 bytes. Returns bytes written (16).
inline size_t pc_netplay_input_encode(const PcNetplayInput& in, uint8_t out[16])
{
	out[0]  = (uint8_t)(in.buttons & 0xFF);
	out[1]  = (uint8_t)((in.buttons >> 8) & 0xFF);
	out[2]  = (uint8_t)in.stickX;
	out[3]  = (uint8_t)in.stickY;
	out[4]  = (uint8_t)in.substickX;
	out[5]  = (uint8_t)in.substickY;
	out[6]  = in.triggerL;
	out[7]  = in.triggerR;
	out[8]  = (uint8_t)(in.controlYaw & 0xFF);
	out[9]  = (uint8_t)((in.controlYaw >> 8) & 0xFF);
	out[10] = in.flags;
	out[11] = in.fragSeq;
	out[12] = in.fragData[0];
	out[13] = in.fragData[1];
	out[14] = in.fragData[2];
	out[15] = in.fragData[3];
	return pc_netplay_gekko::kInputBytes;
}

// Decodes from the first 16 bytes. Returns false when avail < 16.
inline bool pc_netplay_input_decode(const uint8_t* data, size_t avail, PcNetplayInput& out)
{
	if (data == nullptr || avail < pc_netplay_gekko::kInputBytes) return false;
	out.buttons     = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
	out.stickX      = (int8_t)data[2];
	out.stickY      = (int8_t)data[3];
	out.substickX   = (int8_t)data[4];
	out.substickY   = (int8_t)data[5];
	out.triggerL    = data[6];
	out.triggerR    = data[7];
	out.controlYaw  = (uint16_t)(data[8] | ((uint16_t)data[9] << 8));
	out.flags       = data[10];
	out.fragSeq     = data[11];
	out.fragData[0] = data[12];
	out.fragData[1] = data[13];
	out.fragData[2] = data[14];
	out.fragData[3] = data[15];
	return true;
}

// Neutral (hands-off) input: no buttons, centred sticks, yaw 0.
inline PcNetplayInput pc_netplay_input_neutral()
{
	return PcNetplayInput();
}
