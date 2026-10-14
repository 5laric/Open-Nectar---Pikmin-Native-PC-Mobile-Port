#include "app.h"

#include "launcher_platform.h"

#include "nectar_assets/raster.h"
#include "nectar_mapcore/map.h"

#include "pc_vs_default_map.h" // la arena de serie (maps/vs/impact_arena/map.json)

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <SDL.h>

#include "nectar_mapcore/i18n.h"

namespace nm {

using nectar::tr;

namespace fs = std::filesystem;
using nectar::MapDoc;

namespace {

constexpr double kAutosaveSeconds = 120.0;
constexpr float kTopBarH = 58.0f, kToolboxW = 300.0f, kSideW = 340.0f;

const ImVec4 kGood(0.45f, 0.9f, 0.45f, 1), kWarn(1, 0.8f, 0.4f, 1), kBad(1, 0.45f, 0.4f, 1), kDim(0.62f, 0.66f, 0.74f, 1);

double now() { return double(SDL_GetTicks64()) / 1000.0; }

bool readText(const std::string& path, std::string& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

bool writeText(const std::string& path, const std::string& text)
{
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	const std::string tmp = path + ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary);
		if (!f) return false;
		f << text;
		if (!f) return false;
	}
	fs::rename(tmp, path, ec);
	return !ec;
}

void copyStr(char* dst, std::size_t n, const std::string& s) { std::snprintf(dst, n, "%s", s.c_str()); }

/// Nombre de carpeta a partir del nombre del mapa: minúsculas, sin espacios.
std::string slug(const std::string& name)
{
	std::string out;
	for (unsigned char c : name) {
		if (std::isalnum(c)) out += char(std::tolower(c));
		else if ((c == ' ' || c == '-' || c == '_') && !out.empty() && out.back() != '_') out += '_';
	}
	while (!out.empty() && out.back() == '_') out.pop_back();
	return out.empty() ? "mi_mapa" : out.substr(0, 48);
}

/// Texto gris y con salto de línea: las explicaciones.
void hint(const char* text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, kDim);
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}

/// Botón que ocupa todo el ancho.
bool wideButton(const char* label, float h = 36.0f) { return ImGui::Button(label, ImVec2(-1, h)); }

/// Botón que se queda encendido cuando `on`.
bool choiceButton(const char* label, bool on, ImVec2 size = ImVec2(0, 0))
{
	if (on) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.6f, 0.15f, 1));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.7f, 0.25f, 1));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.08f, 0.05f, 1));
	}
	const bool pressed = ImGui::Button(label, size);
	if (on) ImGui::PopStyleColor(3);
	return pressed;
}

const char* courseName(const std::string& path)
{
	if (path == "courses/practice/practice.mod") return tr("Zona del Impacto");
	if (path == "courses/stage1/forest.mod") return tr("Bosque de la Esperanza");
	if (path == "courses/stage2/cave.mod") return tr("El Ombligo del Bosque");
	if (path == "courses/stage3/yakusima.mod") return tr("La Fuente Lejana");
	if (path == "courses/laststage/garden.mod") return tr("La Prueba Final");
	if (path == "courses/route/route.mod") return tr("Mapa del viaje");
	return nullptr;
}

std::string sceneName(const std::string& path)
{
	if (path == "stages/chal0.ini" || path == "stages/practice.ini") return tr("Zona del Impacto");
	if (path.find("stage1") != std::string::npos || path.find("chal1") != std::string::npos) return tr("Bosque de la Esperanza");
	if (path.find("stage2") != std::string::npos || path.find("chal2") != std::string::npos) return tr("El Ombligo del Bosque");
	if (path.find("stage3") != std::string::npos || path.find("chal3") != std::string::npos) return tr("La Fuente Lejana");
	if (path.find("last") != std::string::npos || path.find("chal4") != std::string::npos) return tr("La Prueba Final");
	return path;
}

// ── Iconos dibujados a trazos (sin imágenes ni emojis) ──
enum class Icon { Move, Place, Floor, Height, Plateau, Paint, Water, Walls, Parts, Pellets, Enemies, Bosses, Structures, Decor, More };

/// Dibuja `icon` centrado en `c`, dentro de un cuadrado de lado 2·h.
void drawIcon(ImDrawList* dl, ImVec2 c, float h, Icon icon, ImU32 col)
{
	const float t = std::max(1.5f, h * 0.16f);
	auto P        = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
	switch (icon) {
	case Icon::Move: // cruz de flechas
		dl->AddLine(P(-0.9f, 0), P(0.9f, 0), col, t);
		dl->AddLine(P(0, -0.9f), P(0, 0.9f), col, t);
		for (int k = 0; k < 4; k++) {
			const float dx[] = { 1, -1, 0, 0 }, dy[] = { 0, 0, 1, -1 };
			const ImVec2 tip = P(dx[k] * 0.95f, dy[k] * 0.95f);
			dl->AddTriangleFilled(tip, P(dx[k] * 0.55f - dy[k] * 0.3f, dy[k] * 0.55f - dx[k] * 0.3f), P(dx[k] * 0.55f + dy[k] * 0.3f, dy[k] * 0.55f + dx[k] * 0.3f), col);
		}
		break;
	case Icon::Place: // chincheta: cabeza redonda y punta
		dl->AddCircleFilled(P(0, -0.35f), h * 0.5f, col);
		dl->AddTriangleFilled(P(-0.3f, -0.05f), P(0.3f, -0.05f), P(0, 0.95f), col);
		break;
	case Icon::Floor: // trozo de suelo con un borde suave
		dl->AddBezierCubic(P(-0.9f, 0.2f), P(-0.4f, -0.6f), P(0.3f, 0.4f), P(0.9f, -0.3f), col, t);
		dl->AddLine(P(0.9f, -0.3f), P(0.9f, 0.8f), col, t);
		dl->AddLine(P(0.9f, 0.8f), P(-0.9f, 0.8f), col, t);
		dl->AddLine(P(-0.9f, 0.8f), P(-0.9f, 0.2f), col, t);
		break;
	case Icon::Height: // montañas
		dl->AddTriangleFilled(P(-0.95f, 0.75f), P(-0.25f, -0.6f), P(0.45f, 0.75f), col);
		dl->AddTriangle(P(0.0f, 0.75f), P(0.5f, -0.15f), P(0.95f, 0.75f), col, t);
		break;
	case Icon::Plateau: { // meseta con escalón
		const ImVec2 pts[] = { P(-0.95f, 0.7f), P(-0.45f, 0.7f), P(-0.45f, -0.3f), P(0.45f, -0.3f), P(0.45f, 0.7f), P(0.95f, 0.7f) };
		dl->AddPolyline(pts, 6, col, 0, t);
		dl->AddLine(P(0, -0.95f), P(0, -0.55f), col, t);
		dl->AddTriangleFilled(P(-0.22f, -0.7f), P(0.22f, -0.7f), P(0, -1.0f), col);
		break;
	}
	case Icon::Paint: // rodillo
		dl->AddRectFilled(P(-0.8f, -0.85f), P(0.6f, -0.35f), col, h * 0.12f);
		dl->AddLine(P(0.6f, -0.6f), P(0.85f, -0.6f), col, t);
		dl->AddLine(P(0.85f, -0.6f), P(0.85f, 0.0f), col, t);
		dl->AddLine(P(0.85f, 0.0f), P(0.0f, 0.1f), col, t);
		dl->AddLine(P(0.0f, 0.1f), P(0.0f, 0.95f), col, t * 1.6f);
		break;
	case Icon::Water: // gota
		dl->PathArcTo(P(0, 0.3f), h * 0.6f, 3.14159f * -0.2f, 3.14159f * 1.2f, 20);
		dl->PathLineTo(P(0, -0.95f));
		dl->PathFillConvex(col);
		break;
	case Icon::Walls: // ladrillos
		for (int r = 0; r < 3; r++) {
			const float y0 = -0.8f + r * 0.55f;
			dl->AddRect(P(-0.9f, y0), P(0.9f, y0 + 0.5f), col, 0, 0, t * 0.8f);
			const float off = (r % 2) ? 0.0f : 0.45f;
			dl->AddLine(P(-0.45f + off, y0), P(-0.45f + off, y0 + 0.5f), col, t * 0.8f);
			if (!off) dl->AddLine(P(0.45f, y0), P(0.45f, y0 + 0.5f), col, t * 0.8f);
		}
		break;
	case Icon::Parts: // engranaje
		for (int k = 0; k < 8; k++) {
			const float a = k * 3.14159f / 4.0f;
			dl->AddLine(P(std::cos(a) * 0.5f, std::sin(a) * 0.5f), P(std::cos(a) * 0.95f, std::sin(a) * 0.95f), col, t * 1.8f);
		}
		dl->AddCircle(c, h * 0.55f, col, 20, t * 1.4f);
		dl->AddCircleFilled(c, h * 0.18f, col);
		break;
	case Icon::Pellets: // pastilla con su número
		dl->AddCircle(c, h * 0.85f, col, 24, t);
		dl->AddLine(P(-0.12f, -0.3f), P(0.05f, -0.45f), col, t);
		dl->AddLine(P(0.05f, -0.45f), P(0.05f, 0.45f), col, t);
		break;
	case Icon::Enemies: // bicho: cuerpo, ojos y patas
		dl->AddCircleFilled(P(0, 0.05f), h * 0.6f, col);
		dl->AddCircleFilled(P(-0.25f, -0.55f), h * 0.2f, col);
		dl->AddCircleFilled(P(0.25f, -0.55f), h * 0.2f, col);
		dl->AddLine(P(-0.4f, 0.55f), P(-0.55f, 0.95f), col, t);
		dl->AddLine(P(0.4f, 0.55f), P(0.55f, 0.95f), col, t);
		break;
	case Icon::Bosses: { // corona
		const ImVec2 pts[] = { P(-0.9f, 0.6f), P(-0.9f, -0.5f), P(-0.45f, 0.0f), P(0, -0.8f), P(0.45f, 0.0f), P(0.9f, -0.5f), P(0.9f, 0.6f) };
		dl->AddPolyline(pts, 7, col, ImDrawFlags_Closed, t);
		break;
	}
	case Icon::Structures: // puente
		dl->AddLine(P(-0.95f, -0.1f), P(0.95f, -0.1f), col, t * 1.4f);
		dl->PathArcTo(P(0, 0.75f), h * 0.6f, 3.14159f, 3.14159f * 2.0f, 16);
		dl->PathStroke(col, 0, t);
		dl->AddLine(P(-0.6f, -0.1f), P(-0.6f, 0.75f), col, t);
		dl->AddLine(P(0.6f, -0.1f), P(0.6f, 0.75f), col, t);
		break;
	case Icon::Decor: // flor
		for (int k = 0; k < 5; k++) {
			const float a = k * 3.14159f * 2.0f / 5.0f - 1.5708f;
			dl->AddCircleFilled(P(std::cos(a) * 0.38f, -0.25f + std::sin(a) * 0.38f), h * 0.26f, col);
		}
		dl->AddLine(P(0, 0.1f), P(0, 0.95f), col, t);
		break;
	case Icon::More:
		for (int k = -1; k <= 1; k++) dl->AddCircleFilled(P(k * 0.6f, 0), h * 0.17f, col);
		break;
	}
}

/// CollapsingHeader con icono tras la flecha.
bool iconHeader(const char* label, Icon icon, ImGuiTreeNodeFlags flags = 0)
{
	const std::string padded = std::string("      ") + label;
	const bool open          = ImGui::CollapsingHeader(padded.c_str(), flags);
	const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	const float x  = a.x + ImGui::GetStyle().FramePadding.x + ImGui::GetFontSize() + 14.0f;
	drawIcon(ImGui::GetWindowDrawList(), ImVec2(x, (a.y + b.y) * 0.5f), (b.y - a.y) * 0.32f, icon, ImGui::GetColorU32(ImGuiCol_Text));
	return open;
}

/// Selector de idioma (cada opción en su propio idioma). True si cambia.
bool languagePicker(Config& cfg)
{
	bool changed = false;
	ImGui::TextUnformatted("Language / Idioma");
	ImGui::SameLine();
	const struct {
		const char* code;
		const char* name;
	} langs[] = { { "en", "English" }, { "es", "Español" } };
	for (const auto& l : langs) {
		ImGui::SameLine();
		if (ImGui::RadioButton(l.name, cfg.language == l.code) && cfg.language != l.code) {
			cfg.language = l.code;
			nectar::setLanguage(nectar::languageFromCode(cfg.language));
			cfg.save();
			changed = true;
		}
	}
	return changed;
}

Rgba playerColour(int p) { return p == 0 ? Rgba { 0.95f, 0.3f, 0.25f, 1 } : Rgba { 0.25f, 0.5f, 1.0f, 1 }; }

M4 xformMatrix(const Xform& x)
{
	const float s = float(x.scale);
	return M4::srt(V3(s, s, s), V3(float(x.rx), float(x.ry), float(x.rz)) * kDeg, V3(float(x.x), float(x.y), float(x.z)));
}

// Tramos de un muro (los arcos, como los despliega el compilador).
struct Segment {
	double ax, az, bx, bz;
};
std::vector<Segment> wallSegments(const nectar::DocWall& w)
{
	std::vector<Segment> out;
	if (!w.isArc) {
		out.push_back({ w.ax, w.az, w.bx, w.bz });
		return out;
	}
	const int n = std::max(1, w.segments);
	for (int k = 0; k < n; k++) {
		const double a0 = (w.from + k * (w.to - w.from) / n) * 3.14159265358979 / 180.0;
		const double a1 = (w.from + (k + 1) * (w.to - w.from) / n) * 3.14159265358979 / 180.0;
		out.push_back({ w.cx + w.radius * std::cos(a0), w.cz + w.radius * std::sin(a0), w.cx + w.radius * std::cos(a1), w.cz + w.radius * std::sin(a1) });
	}
	return out;
}

M4 segmentMatrix(const Segment& s, float y)
{
	const float ang = float(-std::atan2(s.bz - s.az, s.bx - s.ax));
	return M4::srt(V3(1, 1, 1), V3(0, ang, 0), V3(float((s.ax + s.bx) * 0.5), y, float((s.az + s.bz) * 0.5)));
}

void fallbackBox(ItemType t, V3& lo, V3& hi)
{
	if (t == ItemType::Rocket) {
		lo = V3(-45, 0, -45);
		hi = V3(45, 150, 45);
	} else {
		lo = V3(-30, 0, -30);
		hi = V3(30, 60, 30);
	}
}

// Rayo contra caja en el espacio de `m`; t del rayo del mundo o -1.
float rayBox(const Ray& ray, const M4& m, const V3& lo, const V3& hi)
{
	const M4 inv = m.inverse();
	const V3 o = inv.point(ray.origin), e = inv.point(ray.origin + ray.dir);
	const V3 d = e - o;
	float t0 = -1e30f, t1 = 1e30f;
	const float os[3] = { o.x, o.y, o.z }, ds[3] = { d.x, d.y, d.z }, ls[3] = { lo.x, lo.y, lo.z }, hs[3] = { hi.x, hi.y, hi.z };
	for (int i = 0; i < 3; i++) {
		if (std::fabs(ds[i]) < 1e-9f) {
			if (os[i] < ls[i] || os[i] > hs[i]) return -1;
			continue;
		}
		float a = (ls[i] - os[i]) / ds[i], b = (hs[i] - os[i]) / ds[i];
		if (a > b) std::swap(a, b);
		t0 = std::max(t0, a);
		t1 = std::min(t1, b);
		if (t0 > t1) return -1;
	}
	if (t1 < 0) return -1;
	return t0 >= 0 ? t0 : t1;
}

void boxLines(std::vector<V3>& out, const M4& m, const V3& lo, const V3& hi)
{
	V3 c[8];
	for (int i = 0; i < 8; i++) c[i] = m.point(V3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z));
	const int e[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
	for (const auto& k : e) {
		out.push_back(c[k[0]]);
		out.push_back(c[k[1]]);
	}
}

std::string pelletId(int colour, int value)
{
	static const char cs[] = { 'r', 'b', 'y' };
	char buf[8];
	std::snprintf(buf, sizeof(buf), "p%c%02d", cs[std::max(0, std::min(2, colour))], value);
	return buf;
}

const nectar::EnemyInfo* enemyInfo(const std::string& id)
{
	for (const nectar::EnemyInfo& e : nectar::enemyTypes()) {
		if (id == e.id) return &e;
	}
	return nullptr;
}

const nectar::ObjectInfo* objectInfo(const std::string& id)
{
	for (const nectar::ObjectInfo& o : nectar::objectKinds()) {
		if (id == o.id) return &o;
	}
	return nullptr;
}

const nectar::ShipPartInfo* partInfo(const std::string& id)
{
	for (const nectar::ShipPartInfo& p : nectar::shipParts()) {
		if (id == p.id) return &p;
	}
	return nullptr;
}

int pelletColour(const std::string& id) { return id.size() > 1 ? (id[1] == 'b' ? 1 : id[1] == 'y' ? 2 : 0) : 0; }
int pelletValue(const std::string& id) { return id.size() == 4 ? std::atoi(id.c_str() + 2) : 1; }

} // namespace

// ── Arranque ────────────────────────────────────────────────────────────────

App::App() = default;

App::~App()
{
	if (catalogThread_.joinable()) catalogThread_.join();
	for (auto& kv : thumbs_) {
		GLuint t = kv.second;
		if (t) glDeleteTextures(1, &t);
	}
	for (auto& kv : swatches_) {
		GLuint t = kv.second;
		if (t) glDeleteTextures(1, &t);
	}
}

bool App::init(const std::string& openPath, std::string& error)
{
	openAtStart_ = openPath;
	if (!renderer_.init(error)) return false;
	cfg_.load();
	nectar::setLanguage(nectar::languageFromCode(cfg_.language));
	std::string why;
	const std::string assets = resolveAssetsDir(cfg_.assetsDir, why);
	copyStr(assetsBuf_, sizeof(assetsBuf_), assets.empty() ? guessAssetsDir() : assets);
	copyStr(gameBuf_, sizeof(gameBuf_), cfg_.gameExe.empty() ? guessGameExe() : cfg_.gameExe);
	if (cfg_.gameExe.empty()) cfg_.gameExe = gameBuf_;
	if (!assets.empty()) {
		// Ya configurado: solo hay que poner al día el catálogo (rápido si no
		// ha cambiado nada).
		cfg_.assetsDir = assets;
		startCatalog();
	}
	return true;
}

void App::startCatalog()
{
	if (catalogThread_.joinable()) catalogThread_.join();
	catalogFinished_ = false;
	catalogDone_     = 0;
	catalogTotal_    = 0;
	catalogError_.clear();
	screen_                 = Screen::Building;
	const std::string data  = cfg_.dataDir();
	const std::string cache = nectar::defaultCacheDir();
	catalogThread_          = std::thread([this, data, cache] {
        nectar::Catalog cat;
        std::string err;
        const bool ok = nectar::buildCatalog(data, cache, cat, err, [this](std::size_t done, std::size_t total, const std::string&) {
            catalogDone_  = int(done);
            catalogTotal_ = int(total);
        });
        if (ok) catalog_ = std::move(cat);
        else catalogError_ = err;
        catalogFinished_ = true;
    });
}

void App::frame()
{
	thumbLoadsThisFrame_ = 0;
	switch (screen_) {
	case Screen::Setup: drawSetup(); break;
	case Screen::Building: drawBuilding(); break;
	case Screen::Welcome: drawWelcome(); break;
	case Screen::Editor: drawEditor(); break;
	}
}

void App::drawSetup()
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(720, 0));
	ImGui::Begin("Nectar Map Maker", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
	ImGui::TextWrapped(tr("¡Bienvenido! Con este programa puedes crear tus propios mapas para el modo VS de Open Nectar."));
	hint(tr("Usa los datos del juego que ya tienes instalado: ni el programa ni tus mapas llevan nada de Nintendo dentro."));
	languagePicker(cfg_);
	ImGui::Spacing();
	ImGui::SeparatorText(tr("1. ¿Dónde está el juego?"));
	hint(tr("Elige la carpeta «assets» del juego (la crea el launcher al instalarlo)."));
	ImGui::SetNextItemWidth(560);
	ImGui::InputText("##assets", assetsBuf_, sizeof(assetsBuf_));
	ImGui::SameLine();
	if (ImGui::Button(tr("Buscar...##a"))) {
		const fs::path p = pikmin::launcher::platform::askForInstallDirectory(tr("Elige la carpeta assets del juego"));
		if (!p.empty()) copyStr(assetsBuf_, sizeof(assetsBuf_), p.string());
	}
	std::string why;
	const std::string assets = resolveAssetsDir(assetsBuf_, why);
	ImGui::PushStyleColor(ImGuiCol_Text, assets.empty() ? kBad : kGood);
	ImGui::TextWrapped("%s", assets.empty() ? (tr("Todavía no: ") + why).c_str() : tr("¡Encontrado!"));
	ImGui::PopStyleColor();

	ImGui::Spacing();
	ImGui::SeparatorText(tr("2. El programa del juego (para probar tus mapas)"));
	hint(tr("Normalmente se encuentra solo. Sin él puedes crear y guardar mapas, pero no probarlos."));
	ImGui::SetNextItemWidth(560);
	ImGui::InputText("##game", gameBuf_, sizeof(gameBuf_));
	ImGui::SameLine();
	if (ImGui::Button(tr("Buscar...##g"))) {
		const fs::path p = pikmin::launcher::platform::askForFile(tr("Elige el programa del juego"), tr("Juego"), "*");
		if (!p.empty()) copyStr(gameBuf_, sizeof(gameBuf_), p.string());
	}
	std::error_code ec;
	if (gameBuf_[0] && !fs::is_regular_file(gameBuf_, ec)) ImGui::TextColored(kWarn, tr("No existe ese archivo."));

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::BeginDisabled(assets.empty());
	if (ImGui::Button(tr("Empezar"), ImVec2(180, 40))) {
		cfg_.assetsDir = assets;
		cfg_.gameExe   = gameBuf_;
		cfg_.save();
		startCatalog();
	}
	ImGui::EndDisabled();
	ImGui::End();
}

void App::drawBuilding()
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(560, 0));
	ImGui::Begin(tr("Preparando"), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
	if (!catalogFinished_) {
		const int done = catalogDone_, total = catalogTotal_;
		ImGui::Text(tr("Preparando las cosas del juego..."));
		char overlay[64];
		if (total > 0) std::snprintf(overlay, sizeof(overlay), "%d / %d", done, total);
		else std::snprintf(overlay, sizeof(overlay), tr("Buscando..."));
		ImGui::ProgressBar(total > 0 ? float(done) / float(total) : 0.0f, ImVec2(-1, 0), overlay);
		hint(tr("Solo tarda la primera vez."));
	} else {
		if (catalogThread_.joinable()) catalogThread_.join();
		if (!catalogError_.empty()) {
			ImGui::TextColored(kBad, tr("No se pudo leer el juego: %s"), catalogError_.c_str());
			if (ImGui::Button(tr("Elegir otra carpeta"))) screen_ = Screen::Setup;
		} else {
			ImGui::End();
			enterEditor();
			return;
		}
	}
	ImGui::End();
}

void App::enterEditor()
{
	preview_.start(cfg_.dataDir());
	lastAutosave_ = now();
	// Al arrancar, la bienvenida (o el mapa que se pasó al abrir el programa);
	// si ya había un mapa abierto, a él.
	if (ed_.generation == 0 && !openAtStart_.empty()) {
		openMap(openAtStart_);
		openAtStart_.clear();
	}
	screen_ = ed_.generation == 0 ? Screen::Welcome : Screen::Editor;
	if (screen_ == Screen::Editor && !cfg_.tutorialDone) tutorialStep_ = 0;
}

bool App::shapeButton(const char* id, nectar::TerrainShape shape, const char* label, float size)
{
	ImGui::BeginGroup();
	ImGui::PushID(id);
	const bool pressed  = ImGui::Button("##shape", ImVec2(size, size));
	const ImVec2 a      = ImGui::GetItemRectMin();
	const ImVec2 c      = ImVec2(a.x + size * 0.5f, a.y + size * 0.5f);
	ImDrawList* dl      = ImGui::GetWindowDrawList();
	const ImU32 green   = IM_COL32(110, 190, 90, 255);
	const float w = size * 0.4f, h = size * 0.26f;
	switch (shape) {
	case nectar::TerrainShape::Rectangle: dl->AddRectFilled(ImVec2(c.x - w, c.y - h), ImVec2(c.x + w, c.y + h), green, 3); break;
	case nectar::TerrainShape::Oval: dl->AddEllipseFilled(c, ImVec2(w, h), green); break;
	case nectar::TerrainShape::Cross:
		dl->AddRectFilled(ImVec2(c.x - w, c.y - h * 0.38f), ImVec2(c.x + w, c.y + h * 0.38f), green);
		dl->AddRectFilled(ImVec2(c.x - w * 0.22f, c.y - h), ImVec2(c.x + w * 0.22f, c.y + h), green);
		break;
	case nectar::TerrainShape::TwoIslands:
		dl->AddEllipseFilled(ImVec2(c.x - w * 0.55f, c.y), ImVec2(w * 0.45f, h * 0.9f), green);
		dl->AddEllipseFilled(ImVec2(c.x + w * 0.55f, c.y), ImVec2(w * 0.45f, h * 0.9f), green);
		dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.14f), ImVec2(c.x + w * 0.5f, c.y + h * 0.14f), green);
		break;
	case nectar::TerrainShape::Ring:
		dl->AddEllipse(c, ImVec2(w * 0.82f, h * 0.82f), green, 0.0f, 0, h * 0.36f);
		dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.12f), ImVec2(c.x + w * 0.5f, c.y + h * 0.12f), green);
		break;
	}
	ImGui::PopID();
	const float tw = ImGui::CalcTextSize(label).x;
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (size - tw) * 0.5f));
	ImGui::TextUnformatted(label);
	ImGui::EndGroup();
	return pressed;
}

void App::drawWelcome()
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(760, 0));
	ImGui::Begin(tr("¿Qué quieres hacer?"), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
	std::error_code ec;
	if (!cfg_.lastMap.empty() && fs::exists(cfg_.lastMap, ec)) {
		std::string text, err;
		MapDoc d;
		std::string name = fs::path(cfg_.lastMap).parent_path().filename().string();
		if (readText(cfg_.lastMap, text) && nectar::parseMapDoc(text, d, err)) name = d.name;
		if (wideButton((tr("Seguir con «") + name + "»").c_str(), 46)) {
			openMap(cfg_.lastMap);
			screen_ = Screen::Editor;
		}
		ImGui::Spacing();
	}
	ImGui::SeparatorText(tr("Crear un mapa nuevo"));
	hint(tr("Elige la forma del suelo para empezar. Después puedes cambiarla como quieras."));
	struct S {
		const char* id;
		nectar::TerrainShape shape;
		const char* label;
	};
	const S shapes[] = { { "r", nectar::TerrainShape::Rectangle, tr("Rectángulo") }, { "o", nectar::TerrainShape::Oval, tr("Óvalo") },
		                 { "c", nectar::TerrainShape::Cross, tr("Cruz") },          { "i", nectar::TerrainShape::TwoIslands, tr("Dos islas") },
		                 { "a", nectar::TerrainShape::Ring, tr("Anillo") } };
	for (int i = 0; i < 5; i++) {
		if (i) ImGui::SameLine(0, 18);
		if (shapeButton(shapes[i].id, shapes[i].shape, shapes[i].label, 124)) {
			newMap(shapes[i].shape);
			screen_ = Screen::Editor;
		}
	}
	ImGui::Spacing();
	if (templatePickers(132)) screen_ = Screen::Editor;
	ImGui::Spacing();
	if (coursePickers()) screen_ = Screen::Editor;
	ImGui::Spacing();
	ImGui::SeparatorText(tr("O bien"));
	if (ImGui::Button(tr("Abrir uno de mis mapas"), ImVec2(240, 40))) {
		refreshOpenList();
		openOpen_ = true;
		newMap(nectar::TerrainShape::Rectangle);
		screen_ = Screen::Editor;
	}
	ImGui::SameLine();
	if (ImGui::Button(tr("Copiar la arena del juego"), ImVec2(240, 40))) {
		openBuiltIn();
		screen_ = Screen::Editor;
	}
	ImGui::End();
}

// ── Archivo ─────────────────────────────────────────────────────────────────

void App::notify(int level, const std::string& text)
{
	toasts_.push_back({ level, text, now() + (level == 2 ? 7.0 : 4.0) });
	if (toasts_.size() > 4) toasts_.erase(toasts_.begin());
	fprintf(stderr, "[mapmaker] %s\n", text.c_str());
}

void App::newMap(nectar::TerrainShape shape)
{
	MapDoc d = nectar::defaultMapDoc();
	if (shape != nectar::TerrainShape::Rectangle) {
		nectar::applyTerrainShape(d, shape);
		// Que salidas, cebollas y piezas no se queden sin suelo con la forma.
		nectar::DocTerrain& g = d.terrain;
		for (const ItemRef& r : allItems(d)) {
			const Xform x = getXform(d, r);
			// También su copia del otro jugador (mapa espejo).
			for (int side = 0; side < 2; side++) {
				const double px = side ? -x.x : x.x, pz = side ? -x.z : x.z;
				const int cx = int(std::lround((px - d.minX) / d.cell)), cz = int(std::lround((pz - d.minZ) / d.cell));
				for (int dz = -3; dz <= 3; dz++) {
					for (int dx = -3; dx <= 3; dx++) {
						const int ix = cx + dx, iz = cz + dz;
						if (ix < 1 || iz < 1 || ix >= g.nx || iz >= g.nz || dx * dx + dz * dz > 10) continue;
						g.floor[std::size_t(g.vertexIndex(ix, iz))] = 255;
					}
				}
			}
		}
	}
	d.name = tr("Mi mapa");
	ed_.reset(d, "");
	cam_.target   = V3(0, 0, 0);
	cam_.distance = 4000;
	cam_.topDown  = false;
	tool_         = Tool::Move;
	if (!cfg_.tutorialDone) tutorialStep_ = 0;
}

unsigned App::templateThumb(const TemplateInfo& t)
{
	// Se dibujan una vez (una por fotograma) y se guardan en la caché.
	const std::string path = (fs::path(catalog_.cacheDir) / "templates" / (std::string(t.id) + "_v3.png")).string();
	std::error_code ec;
	if (!fs::exists(path, ec)) {
		static int lastFrame = -1;
		if (lastFrame == ImGui::GetFrameCount()) return 0;
		lastFrame = ImGui::GetFrameCount();
		PreviewResult r;
		Preview::compileNow(cfg_.dataDir(), t.build(), r);
		if (!r.ok) return 0;
		fs::create_directories(fs::path(path).parent_path(), ec);
		nectar::RenderOptions o;
		o.size  = 200;
		o.pitch = 58.0f;
		o.yaw   = 0.0f;
		nectar::writePng(path, nectar::renderModel(r.full, o));
	}
	return thumb(path);
}

bool App::templatePickers(float size)
{
	ImGui::SeparatorText(tr("O empieza con una plantilla"));
	hint(tr("Mapas ya hechos, listos para jugar o para cambiarlos a tu gusto."));
	const TemplateInfo* chosen = nullptr;
	const int perRow           = std::max(1, int(ImGui::GetContentRegionAvail().x / (size + 16.0f)));
	int col                    = 0;
	for (const TemplateInfo& t : mapTemplates()) {
		ImGui::PushID(t.id);
		ImGui::BeginGroup();
		const unsigned tex = templateThumb(t);
		const bool pressed = tex ? ImGui::ImageButton("##t", ImTextureID(std::intptr_t(tex)), ImVec2(size, size * 0.75f))
		                         : ImGui::Button("...", ImVec2(size + 8, size * 0.75f + 8));
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size + 8);
		ImGui::TextUnformatted(tr(t.name));
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		ImGui::SetItemTooltip("%s", tr(t.blurb));
		if (pressed) chosen = &t;
		ImGui::PopID();
		if (++col % perRow) ImGui::SameLine(0, 12);
	}
	if (col % perRow) ImGui::NewLine();
	if (!chosen) return false;
	const MapDoc d = chosen->build();
	ed_.reset(d, "");
	cam_.target   = V3(0, 0, 0);
	cam_.distance = 4000;
	cam_.topDown  = false;
	tool_         = Tool::Move;
	notify(0, std::string(tr("Plantilla cargada: ")) + d.name);
	return true;
}

bool App::coursePickers()
{
	ImGui::SeparatorText(tr("O un escenario del juego"));
	hint(tr("El nivel tal cual, con su vegetación y sus enemigos: puedes moverlos, quitarlos y añadir lo tuyo. El suelo del nivel no se puede cambiar."));
	const CourseChoice* chosen = nullptr;
	const struct {
		const char* label;
		const std::vector<CourseChoice>* list;
	} groups[] = { { tr("Historia"), &storyCourses() }, { tr("Desafío"), &challengeCourses() } };
	for (int k = 0; k < 2; k++) {
		if (k) ImGui::SameLine();
		ImGui::SetNextItemWidth(240);
		ImGui::PushID(k);
		if (ImGui::BeginCombo("##course", groups[k].label)) {
			for (const CourseChoice& c : *groups[k].list) {
				if (ImGui::Selectable(tr(c.name))) chosen = &c;
			}
			ImGui::EndCombo();
		}
		ImGui::PopID();
	}
	if (!chosen) return false;
	MapDoc d;
	std::string err;
	if (!importCourse(cfg_.dataDir(), *chosen, d, err)) {
		notify(2, std::string(tr("No se pudo cargar ese escenario: ")) + err);
		return false;
	}
	ed_.reset(d, "");
	cam_.target   = V3(float(d.bases[0].x), 0, float(d.bases[0].z));
	cam_.distance = 2500;
	cam_.topDown  = false;
	cam_.topZoom  = float(std::max(d.maxX - d.minX, d.maxZ - d.minZ)) * 0.45f;
	tool_         = Tool::Move;
	notify(0, std::string(tr("Escenario cargado: ")) + d.name);
	return true;
}

void App::openMap(const std::string& path)
{
	std::string text, error;
	MapDoc doc;
	if (!readText(path, text)) {
		notify(2, tr("No se puede abrir ese mapa."));
		return;
	}
	if (!nectar::parseMapDoc(text, doc, error)) {
		notify(2, tr("Ese archivo de mapa está dañado: ") + error);
		return;
	}
	ed_.reset(doc, path);
	cfg_.lastMap = path;
	cfg_.save();
	cam_.target   = V3(float((doc.minX + doc.maxX) * 0.5), 0, float((doc.minZ + doc.maxZ) * 0.5));
	cam_.distance = float(std::max(doc.maxX - doc.minX, doc.maxZ - doc.minZ)) * 1.25f;
	cam_.topZoom  = float(std::max(doc.maxX - doc.minX, doc.maxZ - doc.minZ)) * 0.45f;
	notify(0, tr("Abierto «") + doc.name + "».");
}

void App::openBuiltIn()
{
	MapDoc doc;
	std::string error;
	if (!nectar::parseMapDoc(kPcVsDefaultMap, doc, error)) return;
	doc.name = tr("Mi arena");
	doc.author.clear();
	ed_.reset(doc, "");
	cam_.target   = V3(0, 0, 0);
	cam_.distance = 4000;
	notify(0, tr("Copia de la arena del juego: cámbiala como quieras."));
}

bool App::saveMap(const std::string& path)
{
	if (!writeText(path, nectar::writeMapDoc(ed_.doc))) {
		notify(2, tr("No se pudo guardar (¿falta permiso en la carpeta del juego?)."));
		return false;
	}
	// Miniatura para el menú del juego: el mapa entero visto desde arriba.
	PreviewResult r;
	Preview::compileNow(cfg_.dataDir(), ed_.doc, r);
	if (r.ok) {
		nectar::RenderOptions o;
		o.size  = 256;
		o.pitch = 90.0f;
		o.yaw   = 0.0f;
		nectar::writePng((fs::path(path).parent_path() / "thumb.png").string(), nectar::renderModel(r.full, o));
	}
	ed_.path     = path;
	ed_.dirty    = false;
	cfg_.lastMap = path;
	cfg_.save();
	notify(0, tr("Guardado. Lo encontrarás en el juego: modo VS > Mapa."));
	return true;
}

void App::saveOrAsk(bool thenTest)
{
	if (ed_.path.empty()) {
		copyStr(saveName_, sizeof(saveName_), ed_.doc.name);
		testAfterSave_ = thenTest;
		openSaveAs_    = true;
		return;
	}
	if (saveMap(ed_.path) && thenTest) testInGame();
}

void App::testInGame()
{
	if (ed_.path.empty() || ed_.dirty) {
		saveOrAsk(true);
		return;
	}
	for (const Issue& i : checkMap()) {
		if (i.blocking) {
			notify(2, tr("Antes de probar: ") + i.text);
			return;
		}
	}
	PreviewResult r;
	if (!Preview::compileNow(cfg_.dataDir(), ed_.doc, r)) {
		notify(2, tr("Todavía no se puede jugar: ") + r.error);
		return;
	}
	std::string exe = cfg_.gameExe.empty() ? guessGameExe() : cfg_.gameExe;
	std::error_code ec;
	if (exe.empty() || !fs::is_regular_file(exe, ec)) {
		notify(2, tr("No encuentro el juego. Indícalo en Ayuda > Cambiar carpetas."));
		return;
	}
	std::string err;
	const std::string mapPath = fs::absolute(ed_.path, ec).string();
	if (!launchGame(exe, cfg_.gameDir(), { "--vs-map", mapPath }, err)) {
		notify(2, err);
		return;
	}
	notify(0, tr("Abriendo el juego... Jugador 1: teclado o mando 1. Jugador 2: mando 2."));
}

void App::autosave()
{
	if (!ed_.dirty || now() - lastAutosave_ < kAutosaveSeconds) return;
	lastAutosave_ = now();
	writeText((fs::path(Config::directory()) / "autosave.json").string(), nectar::writeMapDoc(ed_.doc));
}

bool App::requestQuit()
{
	if (screen_ != Screen::Editor || !ed_.dirty) {
		quit_ = true;
		return true;
	}
	confirmQuit_ = true;
	return false;
}

void App::refreshOpenList()
{
	openList_.clear();
	std::error_code ec;
	for (fs::directory_iterator it(cfg_.userMapsDir(), ec), end; !ec && it != end; it.increment(ec)) {
		const fs::path f = it->path() / "map.json";
		if (!fs::exists(f, ec)) continue;
		std::string text, err, name = it->path().filename().string();
		MapDoc d;
		if (readText(f.string(), text) && nectar::parseMapDoc(text, d, err)) name = d.name;
		openList_.push_back({ name, f.string() });
	}
	std::sort(openList_.begin(), openList_.end());
}

// ── Edición ─────────────────────────────────────────────────────────────────

double App::snap(double v) const { return cfg_.snap ? std::round(v / cfg_.snapStep) * cfg_.snapStep : v; }

void App::deleteSelection()
{
	std::vector<ItemRef> del;
	for (const ItemRef& r : ed_.selection) {
		if (itemCaps(r.type).removable) del.push_back(r);
	}
	if (del.empty()) {
		if (!ed_.selection.empty()) notify(1, tr("Eso no se puede borrar: el juego lo necesita. Muévelo a donde quieras."));
		return;
	}
	removeItems(ed_.doc, del);
	ed_.clearSelection();
	ed_.touch();
}

void App::copySelection()
{
	clipboard_    = copyItems(ed_.doc, ed_.selection);
	hasClipboard_ = !(clipboard_.pellets.empty() && clipboard_.pieces.empty() && clipboard_.guards.empty() && clipboard_.props.empty()
	                  && clipboard_.walls.empty() && clipboard_.ponds.empty() && clipboard_.enemies.empty() && clipboard_.objects.empty());
}

void App::paste()
{
	if (!hasClipboard_) return;
	ed_.selection = pasteItems(ed_.doc, clipboard_, 60, 60);
	ed_.touch();
}

void App::duplicateSelection()
{
	const MapDoc clip                 = copyItems(ed_.doc, ed_.selection);
	const std::vector<ItemRef> pasted = pasteItems(ed_.doc, clip, 60, 60);
	if (pasted.empty()) {
		if (!ed_.selection.empty()) notify(1, tr("Eso no se puede duplicar: cada jugador tiene solo uno."));
		return;
	}
	ed_.selection = pasted;
	ed_.touch();
}

void App::focusSelection()
{
	if (ed_.selection.empty()) return;
	const Xform x = getXform(ed_.doc, ed_.selection.back());
	cam_.target   = V3(float(x.x), docHeight(float(x.x), float(x.z)), float(x.z));
	cam_.distance = 700;
	cam_.topZoom  = 500;
}

void App::rotateSelection(double degrees)
{
	bool any = false;
	for (const ItemRef& r : ed_.selection) {
		if (!itemCaps(r.type).rotateY) continue;
		Xform x = getXform(ed_.doc, r);
		x.ry += degrees;
		setXform(ed_.doc, r, x);
		ItemRef other;
		if (mirrorEdits_ && ed_.doc.symmetric && counterpart(r, other) && !ed_.selected(other)) mirrorToCounterpart(ed_.doc, r);
		any = true;
	}
	if (any) ed_.touch();
}

void App::liftSelection(double dy)
{
	bool any = false;
	for (const ItemRef& r : ed_.selection) {
		if (!itemCaps(r.type).height) continue;
		Xform x = getXform(ed_.doc, r);
		x.y += dy;
		setXform(ed_.doc, r, x);
		ItemRef other;
		if (mirrorEdits_ && ed_.doc.symmetric && counterpart(r, other) && !ed_.selected(other)) mirrorToCounterpart(ed_.doc, r);
		any = true;
	}
	if (any) ed_.touch();
}

std::string App::placeAsset(const PlaceChoice& c) const
{
	switch (c.type) {
	case ItemType::Prop: return c.asset;
	case ItemType::Pellet: return "objects/pellets/white1.mod";
	case ItemType::Piece: {
		const nectar::ShipPartInfo* info = partInfo(c.label);
		return info ? info->model : "objects/ufoparts/fuzoku1.mod";
	}
	case ItemType::Guard: return "tekis/swallow/swallow.mod";
	case ItemType::Enemy: {
		const nectar::EnemyInfo* info = enemyInfo(c.label);
		return info ? info->model : "";
	}
	case ItemType::Object: {
		const nectar::ObjectInfo* info = objectInfo(c.label);
		return info ? info->model : "";
	}
	default: return "";
	}
}

void App::placeAt(const V3& at)
{
	const double x = snap(at.x), z = snap(at.z);
	ItemRef r;
	if (place_.type == ItemType::Prop) {
		r                      = addItem(ed_.doc, ItemType::Prop, x, z, place_.asset);
		ed_.doc.props.back().y = docHeight(float(x), float(z));
	} else if (place_.type == ItemType::Enemy || place_.type == ItemType::Object) {
		r = addItem(ed_.doc, place_.type, x, z, place_.label);
	} else {
		r = addItem(ed_.doc, place_.type, x, z);
		if (place_.type == ItemType::Pellet) ed_.doc.pellets.back().id = place_.label;
		if (place_.type == ItemType::Piece) ed_.doc.pieces.back().part = place_.label;
	}
	ed_.selection = { r };
	ed_.touch();
}

std::vector<App::Issue> App::checkMap() const
{
	std::vector<Issue> out;
	const MapDoc& d = ed_.doc;
	// Cosas sin suelo o fuera del mapa: las dice por su nombre.
	for (const ItemRef& r : allItems(d)) {
		if (r.type == ItemType::Wall) continue;
		const Xform x = getXform(d, r);
		if (x.x <= d.minX || x.x >= d.maxX || x.z <= d.minZ || x.z >= d.maxZ) {
			out.push_back({ true, itemName(d, r) + tr(" está fuera del mapa."), true, r });
		} else if (r.type != ItemType::Pond && !docSolid(float(x.x), float(x.z))) {
			out.push_back({ true, itemName(d, r) + tr(" está sobre un hueco: ponle suelo debajo o muévelo."), true, r });
		}
		// Con el mapa espejo, también su gemelo del otro lado.
		if (d.symmetric && itemCaps(r.type).twinned && r.type != ItemType::Pond && !docSolid(float(-x.x), float(-x.z))
		    && x.x > d.minX && x.x < d.maxX) {
			out.push_back({ true, tr("La copia de «") + itemName(d, r) + tr("» del lado del Jugador 2 cae sobre un hueco."), true, r });
		}
	}
	const int k = d.symmetric ? 2 : 1;
	if (d.pieces.empty()) out.push_back({ true, tr("Pon al menos una pieza de la nave para que haya algo que recoger."), false, {} });
	if (int(d.pieces.size()) * k > 16) out.push_back({ true, tr("Hay demasiadas piezas de la nave (el máximo son 16 en total)."), false, {} });
	if (int(d.pellets.size()) * k > 16) out.push_back({ true, tr("Hay demasiadas pastillas (el máximo son 16 en total)."), false, {} });
	if (int(d.guards.size()) * k > 4) out.push_back({ true, tr("Hay demasiados Bulborbs (el máximo son 4 en total)."), false, {} });
	// Lo que diga el compilador (tamaño, archivos que faltan...).
	if (!compiled_.playable && !compiled_.error.empty() && compiled_.generation == ed_.liveGeneration) {
		bool known = false;
		for (const Issue& i : out) known = known || i.blocking;
		if (!known) {
			std::string text = compiled_.error;
			if (text.find(tr("demasiado grande")) != std::string::npos) text = tr("El mapa es demasiado grande para el juego. Hazlo más pequeño o quita decoración.");
			out.push_back({ true, text, false, {} });
		}
	}
	if (d.pellets.empty()) out.push_back({ false, tr("Consejo: pon algunas pastillas para que los jugadores consigan más Pikmin."), false, {} });
	return out;
}

// ── Recursos ────────────────────────────────────────────────────────────────

std::shared_ptr<GpuModel> App::model(const std::string& asset)
{
	auto it = models_.find(asset);
	if (it != models_.end()) return it->second;
	nectar::Model m;
	std::string err;
	std::shared_ptr<GpuModel> gpu;
	if (nectar::loadMod(cfg_.dataDir() + "/" + asset, m, err)) gpu = renderer_.upload(m);
	else modelErrors_[asset] = err;
	models_[asset] = gpu;
	return gpu;
}

unsigned App::thumb(const std::string& path)
{
	if (path.empty()) return 0;
	auto it = thumbs_.find(path);
	if (it != thumbs_.end()) return it->second;
	if (thumbLoadsThisFrame_ >= 6) return 0; // el resto, en los siguientes fotogramas
	thumbLoadsThisFrame_++;
	const GLuint t = loadPngTexture(path);
	thumbs_[path]  = t;
	return t;
}

unsigned App::assetThumb(const std::string& asset)
{
	for (const nectar::CatalogEntry& e : catalog_.entries) {
		if (e.path == asset) return thumb(e.thumb);
	}
	return 0;
}

std::string App::markerAsset(const ItemRef& r) const
{
	switch (r.type) {
	case ItemType::Base: return r.index == 0 ? "pikis/nv1Model.mod" : "pikis/nv2Model.mod";
	case ItemType::Rocket: return ""; // la nave depende del TEV y sale negra: una columna del color del jugador
	case ItemType::Gate: return "objects/gate/gt_hard.mod";
	case ItemType::BombPile: return "objects/bomb/bomb.mod";
	case ItemType::Onion: return "objects/goal/goal.mod";
	case ItemType::Pellet: {
		const int v = pelletValue(ed_.doc.pellets[std::size_t(r.index)].id);
		return v == 5 ? "objects/pellets/white2.mod" : v == 10 ? "objects/pellets/white3.mod" : v == 20 ? "objects/pellets/white4.mod" : "objects/pellets/white1.mod";
	}
	case ItemType::Piece: {
		const nectar::DocPiece& p = ed_.doc.pieces[std::size_t(r.index)];
		if (const nectar::ShipPartInfo* info = partInfo(p.part)) return info->model;
		return "objects/ufoparts/fuzoku" + std::to_string(1 + std::max(0, p.kind)) + ".mod";
	}
	case ItemType::Enemy: {
		const nectar::EnemyInfo* info = enemyInfo(ed_.doc.enemies[std::size_t(r.index)].type);
		return info ? info->model : "";
	}
	case ItemType::Object: {
		const nectar::ObjectInfo* info = objectInfo(ed_.doc.objects[std::size_t(r.index)].kind);
		return info ? info->model : "";
	}
	case ItemType::Gen: return ed_.doc.gens[std::size_t(r.index)].model;
	case ItemType::BigPiece: return "objects/ufoparts/fuzoku1.mod";
	case ItemType::Guard: return "tekis/chappy/chappy.mod";
	case ItemType::Prop: return ed_.doc.props[std::size_t(r.index)].asset;
	default: return "";
	}
}

Rgba App::markerTint(const ItemRef& r) const
{
	auto colourOf = [](int c) {
		return c == 1 ? Rgba { 0.45f, 0.6f, 1.0f, 1 } : c == 2 ? Rgba { 1.0f, 0.9f, 0.35f, 1 } : Rgba { 1.0f, 0.45f, 0.4f, 1 };
	};
	if (r.type == ItemType::Onion) return colourOf(r.sub == 0 ? 1 : r.sub == 1 ? 0 : 2);
	if (r.type == ItemType::Pellet) return colourOf(pelletColour(ed_.doc.pellets[std::size_t(r.index)].id));
	return Rgba();
}

M4 App::itemMatrix(const ItemRef& r, bool twin) const
{
	Xform x = getXform(ed_.doc, r);
	if (twin) {
		x.x  = -x.x;
		x.z  = -x.z;
		x.ry = x.ry + 180.0;
		if (r.type == ItemType::Prop) x.y += docHeight(float(x.x), float(x.z)) - docHeight(float(-x.x), float(-x.z));
	}
	if (r.type != ItemType::Prop && r.type != ItemType::Gen) {
		x.scale = 1;
		// Apoyados en el terreno (las construcciones, a su altura sobre él).
		x.y = docHeight(float(x.x), float(x.z)) + (r.type == ItemType::Object ? x.y : 0.0);
	}
	return xformMatrix(x);
}

// ── Editor ──────────────────────────────────────────────────────────────────

void App::drawEditor()
{
	if (courseMode() && courseField_.course != ed_.doc.course) buildCourseField(cfg_.dataDir(), ed_.doc.course, courseField_);
	// Resultados de la compilación en segundo plano.
	PreviewResult r;
	if (preview_.poll(r)) {
		if (r.ok) {
			baseGpu_ = renderer_.upload(r.base);
			// La superficie del agua (la capa 15 de las charcas), para dibujarla en azul.
			waterTris_.clear();
			for (const nectar::BakedTri& t : nectar::bakeModel(r.base)) {
				if (t.material != 15) continue;
				for (const nectar::Vec3& p : t.p) waterTris_.push_back(V3(p.x, p.y + 1, p.z));
			}
		}
		compiled_ = std::move(r);
	}
	if (requestedLive_ != ed_.liveGeneration && !preview_.busy() && now() - lastRequest_ > 0.16) {
		requestedLive_ = ed_.liveGeneration;
		lastRequest_   = now();
		preview_.request(ed_.doc, ed_.liveGeneration);
	}

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float X = vp->WorkPos.x, Y = vp->WorkPos.y, W = vp->WorkSize.x, H = vp->WorkSize.y;
	drawTopBar({ X, Y, W, kTopBarH });
	drawToolbox({ X, Y + kTopBarH, kToolboxW, H - kTopBarH });
	drawViewport({ X + kToolboxW, Y + kTopBarH, W - kToolboxW - kSideW, H - kTopBarH });
	drawSidePanel({ X + W - kSideW, Y + kTopBarH, kSideW, H - kTopBarH });
	drawModals();
	drawTutorial();
	handleShortcuts();

	ed_.commit(ImGui::IsAnyItemActive() || dragging_ || drawingWall_ || ImGui::IsMouseDown(ImGuiMouseButton_Left));
	autosave();
}

void App::drawTopBar(const Rect& r)
{
	ImGui::SetNextWindowPos(ImVec2(r.x, r.y));
	ImGui::SetNextWindowSize(ImVec2(r.w, r.h));
	ImGui::Begin("##top", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	const ImVec2 b(110, 38);
	if (ImGui::Button(tr("Nuevo"), b)) openNew_ = true;
	ImGui::SetItemTooltip(tr("Empezar otro mapa"));
	ImGui::SameLine();
	if (ImGui::Button(tr("Abrir"), b)) {
		refreshOpenList();
		openOpen_ = true;
	}
	ImGui::SetItemTooltip(tr("Abrir uno de tus mapas"));
	ImGui::SameLine();
	if (ImGui::Button(tr("Guardar"), b)) saveOrAsk(false);
	ImGui::SetItemTooltip(tr("Guardar (Ctrl+S)"));
	ImGui::SameLine(0, 24);
	ImGui::BeginDisabled(!ed_.canUndo());
	if (ImGui::Button(tr("Deshacer"), b)) ed_.undo();
	ImGui::SetItemTooltip(tr("Volver atrás el último cambio (Ctrl+Z)"));
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!ed_.canRedo());
	if (ImGui::Button(tr("Rehacer"), b)) ed_.redo();
	ImGui::SetItemTooltip(tr("Repetir lo que deshiciste (Ctrl+Y)"));
	ImGui::EndDisabled();
	ImGui::SameLine(0, 24);
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 9);
	ImGui::Text("%s%s", ed_.doc.name.c_str(), ed_.dirty ? tr("  (sin guardar)") : "");

	// A la derecha: ayuda y probar.
	const float testW = 230, helpW = 100;
	ImGui::SameLine(r.w - testW - helpW - 24);
	ImGui::SetCursorPosY(8);
	if (ImGui::Button(tr("Ayuda"), ImVec2(helpW, 38))) openHelp_ = true;
	ImGui::SameLine();
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.58f, 0.25f, 1));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.7f, 0.32f, 1));
	if (ImGui::Button(tr("Probar en el juego"), ImVec2(testW, 38))) testInGame();
	const ImVec2 tmin = ImGui::GetItemRectMin(), tmax = ImGui::GetItemRectMax();
	testButtonRect_   = { tmin.x, tmin.y, tmax.x - tmin.x, tmax.y - tmin.y };
	ImGui::PopStyleColor(2);
	ImGui::SetItemTooltip(tr("Guarda el mapa y lo abre en el juego para jugarlo (F5)"));
	ImGui::End();
}

void App::drawToolbox(const Rect& r)
{
	ImGui::SetNextWindowPos(ImVec2(r.x, r.y));
	ImGui::SetNextWindowSize(ImVec2(r.w, r.h));
	ImGui::Begin(tr("Herramientas"), nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
	toolboxRect_ = { r.x, r.y, r.w, r.h };
	struct T {
		Tool tool;
		const char* label;
		const char* tip;
		bool terrain;
		Icon icon = Icon::Move;
	};
	const T tools[] = { { Tool::Move, tr("Mover y girar"), tr("Coge cosas del mapa y arrástralas"), false, Icon::Move },
		                { Tool::Place, tr("Poner cosas"), tr("Piezas, pastillas, enemigos y decoración"), false, Icon::Place },
		                { Tool::Floor, tr("Dibujar suelo"), tr("Da forma al mapa: añade suelo o haz huecos"), true, Icon::Floor },
		                { Tool::Height, tr("Montañas y hoyos"), tr("Sube y baja el terreno con suavidad"), true, Icon::Height },
		                { Tool::Plateau, tr("Elevar o hundir zona"), tr("Plataformas y fosos planos, con bordes de acantilado"), true, Icon::Plateau },
		                { Tool::Paint, tr("Pintar el suelo"), tr("Cambia el aspecto del suelo"), true, Icon::Paint },
		                { Tool::Water, tr("Agua"), tr("Pinta lagos (solo los Pikmin azules entran)"), true, Icon::Water },
		                { Tool::Walls, tr("Muros"), tr("Dibuja muros arrastrando"), false, Icon::Walls },
		                { Tool::Scatter, tr("Decorar"), tr("Esparce hierbas, flores y piedras pintando"), false, Icon::Decor } };
	int n = 1;
	for (const T& t : tools) {
		char label[64];
		std::snprintf(label, sizeof(label), "%s##tool%d", t.label, n++);
		// Con un escenario del juego, el suelo y los muros no se tocan.
		const bool locked = courseMode() && (t.terrain || t.tool == Tool::Walls || t.tool == Tool::Scatter);
		if (locked) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
		const bool pressedTool = choiceButton(label, tool_ == t.tool, ImVec2(-1, 38));
		if (locked) ImGui::PopStyleVar();
		if (pressedTool) {
			if (courseMode() && (t.tool == Tool::Walls || t.tool == Tool::Scatter)) notify(1, tr("En los escenarios del juego todavía no se pueden poner muros ni decoración."));
			else if (!t.terrain || ensureTerrain()) tool_ = t.tool;
		}
		{
			// Icono a la izquierda y el atajo (número) a la derecha.
			const ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
			ImDrawList* dl  = ImGui::GetWindowDrawList();
			const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
			drawIcon(dl, ImVec2(ra.x + 22.0f, (ra.y + rb.y) * 0.5f), 10.0f, t.icon, ink);
			char key[4];
			std::snprintf(key, sizeof(key), "%d", n - 1);
			dl->AddText(ImVec2(rb.x - 20.0f, (ra.y + rb.y) * 0.5f - ImGui::GetFontSize() * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), key);
		}
		ImGui::SetItemTooltip("%s", t.tip);
		const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
		if (t.tool == Tool::Floor) floorButtonRect_ = { a.x, a.y, b.x - a.x, b.y - a.y };
		if (t.tool == Tool::Place) placeButtonRect_ = { a.x, a.y, b.x - a.x, b.y - a.y };
	}
	ImGui::Separator();
	ImGui::BeginChild("toolopts");
	drawToolOptions();
	ImGui::EndChild();
	ImGui::End();
}

void App::drawToolOptions()
{
	switch (tool_) {
	case Tool::Move:
		ImGui::SeparatorText(tr("Mover y girar"));
		hint(tr("Haz clic en algo para elegirlo y arrástralo por el suelo. Con Ctrl eliges varias cosas a la vez."));
		hint(tr("Q y E lo giran. Supr lo borra. Ctrl+D lo duplica."));
		if (ImGui::Checkbox(tr("Encajar en la cuadrícula"), &cfg_.snap)) cfg_.save();
		ImGui::SetItemTooltip(tr("Las cosas se colocan alineadas, a saltos de %.0f."), cfg_.snapStep);
		break;
	case Tool::Place: drawPlaceOptions(); break;
	case Tool::Floor: drawFloorOptions(); break;
	case Tool::Height: drawHeightOptions(); break;
	case Tool::Plateau: drawPlateauOptions(); break;
	case Tool::Paint: drawPaintOptions(); break;
	case Tool::Water: drawWaterOptions(); break;
	case Tool::Walls: drawWallOptions(); break;
	case Tool::Scatter: drawScatterOptions(); break;
	}
}

void App::drawPlaceOptions()
{
	ImGui::SeparatorText(tr("¿Qué quieres poner?"));
	hint(tr("Elige algo y haz clic en el mapa. Puedes poner varios seguidos."));
	// Rejilla de miniaturas de modelos del juego.
	auto grid = [&](const char* id, const std::vector<std::pair<PlaceChoice, std::string>>& items, const char* tipFmt) {
		ImGui::PushID(id);
		int col = 0;
		for (const auto& it : items) {
			const PlaceChoice& c = it.first;
			ImGui::PushID(c.label.c_str());
			const unsigned t = assetThumb(placeAsset(c));
			const bool on    = place_.type == c.type && place_.label == c.label;
			if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.6f, 0.15f, 1));
			const bool pressed = t ? ImGui::ImageButton("##g", ImTextureID(std::intptr_t(t)), ImVec2(72, 72)) : ImGui::Button(it.second.c_str(), ImVec2(80, 80));
			if (on) ImGui::PopStyleColor();
			if (pressed) place_ = c;
			ImGui::SetItemTooltip(tipFmt, it.second.c_str());
			ImGui::PopID();
			if (++col < 3) ImGui::SameLine();
			else col = 0;
		}
		if (col) ImGui::NewLine();
		ImGui::PopID();
	};

	if (iconHeader(tr("Piezas de la nave"), Icon::Parts, ImGuiTreeNodeFlags_DefaultOpen)) {
		hint(tr("Lo que hay que llevar al cohete. Las grandes necesitan muchos Pikmin y dan más puntos."));
		std::vector<std::pair<PlaceChoice, std::string>> parts;
		for (const nectar::ShipPartInfo& p : nectar::shipParts()) {
			char text[96];
			std::snprintf(text, sizeof(text), tr("La levantan %d Pikmin (más rápido con %d)"), p.carryMin, p.carryMax);
			parts.push_back({ { ItemType::Piece, "", p.id }, text });
		}
		grid("parts", parts, "%s");
	}
	if (iconHeader(tr("Pastillas"), Icon::Pellets)) {
		static int colour = 0, value = 1;
		const char* colours[] = { tr("Roja"), tr("Azul"), tr("Amarilla") };
		for (int c = 0; c < 3; c++) {
			if (c) ImGui::SameLine();
			if (choiceButton(colours[c], colour == c, ImVec2(80, 0))) colour = c;
		}
		const int values[] = { 1, 5, 10, 20 };
		for (int i = 0; i < 4; i++) {
			if (i) ImGui::SameLine();
			char lbl[8];
			std::snprintf(lbl, sizeof(lbl), "%d", values[i]);
			if (choiceButton(lbl, value == values[i], ImVec2(58, 0))) value = values[i];
		}
		const std::string pid = pelletId(colour, value);
		const bool on         = place_.type == ItemType::Pellet && place_.label == pid;
		if (choiceButton((tr("Pastilla ") + std::string(colours[colour]) + tr(" de ") + std::to_string(value)).c_str(), on, ImVec2(-1, 32)))
			place_ = { ItemType::Pellet, "", pid };
		hint(tr("Al llevarla a la cebolla de su color salen Pikmin nuevos (tantos como su número)."));
	}
	if (iconHeader(tr("Enemigos"), Icon::Enemies)) {
		std::vector<std::pair<PlaceChoice, std::string>> list;
		for (const nectar::EnemyInfo& e : nectar::enemyTypes()) {
			if (e.boss >= 0 && e.boss <= 3) continue;
			list.push_back({ { ItemType::Enemy, "", e.id }, std::string(tr(e.label)) + "\n" + tr(e.hint) });
		}
		grid("enemies", list, "%s");
	}
	if (iconHeader(tr("Jefes"), Icon::Bosses)) {
		hint(tr("Enormes y muy peligrosos: mejor en mapas grandes."));
		std::vector<std::pair<PlaceChoice, std::string>> list;
		for (const nectar::EnemyInfo& e : nectar::enemyTypes()) {
			if (e.boss < 0 || e.boss > 3) continue;
			list.push_back({ { ItemType::Enemy, "", e.id }, std::string(tr(e.label)) + "\n" + tr(e.hint) });
		}
		grid("bosses", list, "%s");
	}
	if (iconHeader(tr("Construcciones"), Icon::Structures)) {
		hint(tr("Funcionan como en el juego: los Pikmin construyen puentes y rampas, empujan bloques y derriban puertas."));
		std::vector<std::pair<PlaceChoice, std::string>> list;
		for (const nectar::ObjectInfo& o : nectar::objectKinds()) list.push_back({ { ItemType::Object, "", o.id }, std::string(tr(o.label)) + "\n" + tr(o.hint) });
		grid("objects", list, "%s");
	}

	if (courseMode()) {
		hint(tr("En los escenarios del juego todavía no se puede añadir decoración: el nivel ya trae la suya."));
		return;
	}
	{
		const ImVec2 at = ImGui::GetCursorScreenPos();
		drawIcon(ImGui::GetWindowDrawList(), ImVec2(at.x + 9.0f, at.y + ImGui::GetFontSize() * 0.55f), 8.0f, Icon::Decor, ImGui::GetColorU32(ImGuiCol_Text));
		ImGui::Indent(24.0f);
		ImGui::SeparatorText(tr("Decoración"));
		ImGui::Unindent(24.0f);
	}
	ImGui::SetNextItemWidth(-1);
	ImGui::InputTextWithHint("##buscar", tr("Buscar..."), search_, sizeof(search_));
	std::string q = search_;
	std::transform(q.begin(), q.end(), q.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
	std::string group;
	int col = 0;
	for (const DecorItem& it : decorItems()) {
		std::string name = tr(it.name);
		std::string low  = name;
		std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
		if (!q.empty() && low.find(q) == std::string::npos) continue;
		if (group != it.group) {
			if (col) ImGui::NewLine();
			col   = 0;
			group = it.group;
			ImGui::TextColored(kDim, "%s", tr(it.group));
		}
		ImGui::PushID(it.path);
		const unsigned t = assetThumb(it.path);
		const bool on    = place_.type == ItemType::Prop && place_.asset == it.path;
		if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.6f, 0.15f, 1));
		const bool pressed = t ? ImGui::ImageButton("##d", ImTextureID(std::intptr_t(t)), ImVec2(72, 72)) : ImGui::Button(tr(it.name), ImVec2(80, 80));
		if (on) ImGui::PopStyleColor();
		if (pressed) place_ = { ItemType::Prop, it.path, tr(it.name) };
		ImGui::SetItemTooltip("%s", tr(it.name));
		if (ImGui::BeginDragDropSource()) {
			ImGui::SetDragDropPayload("NM_ASSET", it.path, std::strlen(it.path) + 1);
			ImGui::Text("%s", tr(it.name));
			ImGui::EndDragDropSource();
		}
		ImGui::PopID();
		if (++col < 3) ImGui::SameLine();
		else col = 0;
	}
	if (col) ImGui::NewLine();
	if (iconHeader(tr("Más cosas del juego (avanzado)"), Icon::More)) {
		hint(tr("Cualquier modelo del juego como decoración. Algunos se ven raros o son muy grandes."));
		for (const nectar::CatalogEntry& e : catalog_.entries) {
			if (e.kind != nectar::AssetKind::Model || !e.error.empty()) continue;
			// Fuera lo que ya tiene su sitio (piezas, enemigos…) o no sirve
			// de adorno (pruebas, flechas, colisiones, personajes).
			static const char* const kSkipCat[] = { "terrain", "effects", "ship_parts", "pellets", "enemies", "pikmin" };
			static const char* const kSkipPath[] = { "shapes/", "testdata/", "intro/", "objects/gate/", "objects/bridge/", "objects/slimeEff",
				                                     "objects/water/", "objects/fl_water/", "objects/goal/", "objects/pikihead/",
				                                     "objects/rocket/rocket", "objects/ufo/", "cinemas/titles/" };
			bool skip = friendlyAssetName(e.path) != nullptr;
			for (const char* c : kSkipCat) skip |= e.category == c;
			for (const char* p : kSkipPath) skip |= e.path.rfind(p, 0) == 0;
			if (skip) continue;
			if (!q.empty() && e.path.find(q) == std::string::npos) continue;
			const bool on = place_.type == ItemType::Prop && place_.asset == e.path;
			if (ImGui::Selectable(e.path.c_str(), on)) place_ = { ItemType::Prop, e.path, e.path };
		}
	}
}

void App::drawSwatches(bool allowAdd)
{
	nectar::DocTerrain& g = ed_.doc.terrain;
	paintMaterial_        = std::max(0, std::min(paintMaterial_, int(g.palette.size()) - 1));
	for (std::size_t k = 0; k < g.palette.size(); k++) {
		ImGui::PushID(int(k));
		const unsigned t = swatch(g.palette[k].source, g.palette[k].index);
		const bool sel   = paintMaterial_ == int(k);
		if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.6f, 0.15f, 1));
		if (t ? ImGui::ImageButton("##p", ImTextureID(std::intptr_t(t)), ImVec2(56, 56)) : ImGui::Button("?", ImVec2(64, 64))) paintMaterial_ = int(k);
		if (sel) ImGui::PopStyleColor();
		const char* course = courseName(g.palette[k].source);
		ImGui::SetItemTooltip(tr("Suelo %zu (de %s)"), k + 1, course ? course : g.palette[k].source.c_str());
		if (ImGui::BeginPopupContextItem("ctx")) {
			if (ImGui::MenuItem(tr("Cambiar por otro"))) {
				matBrowserTarget_ = int(k);
				matBrowserCourse_ = g.palette[k].source;
				openMatBrowser_   = true;
			}
			if (g.palette.size() > 1 && ImGui::MenuItem(tr("Quitar de la lista"))) {
				// Donde mandaba, pasa a mandar lo que quede (o el primero).
				g.palette.erase(g.palette.begin() + long(k));
				g.layers.erase(g.layers.begin() + long(k));
				for (std::size_t v = 0; v < g.vertexCount(); v++) {
					bool any = false;
					for (const auto& l : g.layers) any = any || l[v] > 0;
					if (!any) g.layers[0][v] = 255;
				}
				ed_.touch();
				ImGui::EndPopup();
				ImGui::PopID();
				return;
			}
			ImGui::EndPopup();
		}
		ImGui::PopID();
		if ((k + 1) % 4) ImGui::SameLine();
	}
	if (allowAdd && g.palette.size() < 250) {
		if (ImGui::Button("+", ImVec2(64, 64))) {
			matBrowserTarget_ = -1;
			openMatBrowser_   = true;
		}
		ImGui::SetItemTooltip(tr("Añadir otro suelo de los escenarios del juego"));
	} else {
		ImGui::NewLine();
	}
	hint(tr("Clic derecho en un suelo para cambiarlo o quitarlo."));
}

void App::drawFloorOptions()
{
	ImGui::SeparatorText(tr("Dibujar suelo"));
	if (choiceButton(tr("Añadir suelo"), !floorErase_, ImVec2(130, 34))) floorErase_ = false;
	ImGui::SameLine();
	if (choiceButton(tr("Hacer hueco"), floorErase_, ImVec2(130, 34))) floorErase_ = true;
	hint(floorErase_ ? tr("Pinta para quitar suelo. Alrededor de los huecos se ponen paredes solas.")
	                 : tr("Pinta para añadir suelo. Mantén Mayús para hacer huecos."));
	ImGui::SliderFloat(tr("Pincel"), &brushRadius_, 25, 1000, "%.0f");
	ImGui::SetItemTooltip(tr("Tamaño del pincel (también con Ctrl + rueda del ratón)."));
	ImGui::SeparatorText(tr("¿Qué suelo?"));
	drawSwatches(true);

	ImGui::SeparatorText(tr("Tamaño del mapa"));
	ImGui::Text(tr("Ancho"));
	ImGui::SameLine(80);
	if (ImGui::Button(tr("Más##w"), ImVec2(80, 0))) growMap(4, 0);
	ImGui::SameLine();
	if (ImGui::Button(tr("Menos##w"), ImVec2(80, 0))) growMap(-4, 0);
	ImGui::Text(tr("Largo"));
	ImGui::SameLine(80);
	if (ImGui::Button(tr("Más##l"), ImVec2(80, 0))) growMap(0, 4);
	ImGui::SameLine();
	if (ImGui::Button(tr("Menos##l"), ImVec2(80, 0))) growMap(0, -4);
	const MapDoc& d = ed_.doc;
	ImGui::TextColored(kDim, tr("Ahora: %.0f x %.0f"), d.maxX - d.minX, d.maxZ - d.minZ);

	ImGui::SeparatorText(tr("Formas rápidas"));
	hint(tr("Cambian dónde hay suelo de golpe (se puede deshacer)."));
	struct S {
		const char* id;
		nectar::TerrainShape shape;
		const char* label;
	};
	const S shapes[] = { { "r", nectar::TerrainShape::Rectangle, tr("Rectángulo") }, { "o", nectar::TerrainShape::Oval, tr("Óvalo") },
		                 { "c", nectar::TerrainShape::Cross, tr("Cruz") },          { "i", nectar::TerrainShape::TwoIslands, tr("Dos islas") },
		                 { "a", nectar::TerrainShape::Ring, tr("Anillo") } };
	for (int i = 0; i < 5; i++) {
		if (i % 3) ImGui::SameLine();
		if (shapeButton(shapes[i].id, shapes[i].shape, shapes[i].label, 76)) {
			nectar::applyTerrainShape(ed_.doc, shapes[i].shape);
			ed_.touch();
		}
	}
	ImGui::SeparatorText(tr("Paredes del borde"));
	nectar::DocTerrain& g = ed_.doc.terrain;
	const unsigned t      = swatch(g.border.source, g.border.index);
	if (t ? ImGui::ImageButton("##border", ImTextureID(std::intptr_t(t)), ImVec2(48, 48)) : ImGui::Button("?", ImVec2(56, 56))) {
		matBrowserTarget_ = -2;
		matBrowserCourse_ = g.border.source;
		openMatBrowser_   = true;
	}
	ImGui::SetItemTooltip(tr("Pulsa para elegir otro aspecto"));
	ImGui::SameLine();
	ImGui::BeginGroup();
	float h = float(ed_.doc.wallHeight);
	ImGui::SetNextItemWidth(150);
	if (ImGui::SliderFloat(tr("Altura"), &h, 20, 600, "%.0f")) {
		ed_.doc.wallHeight = h;
		ed_.touch();
	}
	ImGui::EndGroup();
}

void App::drawHeightOptions()
{
	ImGui::SeparatorText(tr("Montañas y hoyos"));
	const char* ops[]  = { tr("Subir"), tr("Bajar"), tr("Allanar"), tr("Suavizar") };
	const char* tips[] = { tr("Pinta para levantar el terreno (montañas, rampas)."), tr("Pinta para hundir el terreno (hoyos, valles)."),
		                   tr("Deja el terreno a la altura donde empiezas a pintar."), tr("Quita picos y escalones.") };
	for (int i = 0; i < 4; i++) {
		if (i % 2) ImGui::SameLine();
		if (choiceButton(ops[i], int(heightOp_) == i, ImVec2(130, 34))) heightOp_ = HeightOp(i);
	}
	hint(tips[int(heightOp_)]);
	if (heightOp_ == HeightOp::Raise || heightOp_ == HeightOp::Lower) hint(tr("Con Mayús hace lo contrario."));
	ImGui::SliderFloat(tr("Pincel"), &brushRadius_, 25, 1000, "%.0f");
	ImGui::SetItemTooltip(tr("Tamaño del pincel (también con Ctrl + rueda del ratón)."));
	ImGui::SliderFloat(tr("Fuerza"), &brushStrength_, 20, 600, "%.0f");
	ImGui::Spacing();
	ImGui::TextColored(kWarn, tr("Las zonas rojas son tan empinadas"));
	ImGui::TextColored(kWarn, tr("que los Pikmin resbalan."));
	ImGui::Spacing();
	if (ImGui::Button(tr("Allanar todo el mapa"), ImVec2(-1, 0))) {
		std::fill(ed_.doc.terrain.heights.begin(), ed_.doc.terrain.heights.end(), std::int16_t(0));
		ed_.touch();
	}
}

void App::drawPaintOptions()
{
	ImGui::SeparatorText(tr("Pintar el suelo"));
	hint(tr("Elige un suelo y pinta encima del mapa. Solo cambia el aspecto: la forma no."));
	ImGui::SliderFloat(tr("Pincel"), &brushRadius_, 25, 1000, "%.0f");
	drawSwatches(true);
	if (ImGui::CollapsingHeader(tr("Ajustes del suelo elegido (avanzado)"))) {
		nectar::DocTerrain& g = ed_.doc.terrain;
		nectar::DocMaterial& m = g.palette[std::size_t(paintMaterial_)];
		const char* const kinds[] = { tr("Sólido"), tr("Roca"), tr("Hierba"), tr("Madera"), tr("Barro") };
		if (ImGui::Combo(tr("Al pisarlo suena a"), &m.attribute, kinds, 5)) ed_.touch();
		float uv = float(m.uvScale * 1000.0);
		if (ImGui::SliderFloat(tr("Tamaño del dibujo"), &uv, 0.0f, 10.0f, uv > 0 ? "%.2f" : tr("Normal"))) {
			m.uvScale = uv / 1000.0;
			ed_.touch();
		}
	}
}

void App::drawWaterOptions()
{
	ImGui::SeparatorText(tr("Agua"));
	if (choiceButton(tr("Poner agua"), !waterErase_, ImVec2(130, 34))) waterErase_ = false;
	ImGui::SameLine();
	if (choiceButton(tr("Quitar agua"), waterErase_, ImVec2(130, 34))) waterErase_ = true;
	hint(waterErase_ ? tr("Pinta para secar el agua.") : tr("Pinta sobre el suelo para hacer un lago. Mantén Mayús para quitar agua."));
	ImGui::SliderFloat(tr("Pincel"), &brushRadius_, 25, 1000, "%.0f");
	ImGui::SetItemTooltip(tr("Tamaño del pincel (también con Ctrl + rueda del ratón)."));
	hint(tr("Cada lago queda plano, a la altura de su orilla más baja."));
	hint(tr("Solo los Pikmin azules pueden entrar en el agua: es buen sitio para esconder piezas."));
}

void App::drawPlateauOptions()
{
	ImGui::SeparatorText(tr("Elevar o hundir zona"));
	if (choiceButton(tr("Elevar"), plateauUp_, ImVec2(130, 34))) plateauUp_ = true;
	ImGui::SameLine();
	if (choiceButton(tr("Hundir"), !plateauUp_, ImVec2(130, 34))) plateauUp_ = false;
	hint(plateauUp_ ? tr("Pinta una zona: queda plana y más alta que donde empiezas a pintar (una plataforma).")
	                : tr("Pinta una zona: queda plana y más baja que donde empiezas a pintar (un foso)."));
	hint(tr("Mantén Mayús para hacer lo contrario."));
	ImGui::Text(tr("¿Cuánto?"));
	const struct {
		const char* label;
		float amount;
	} amounts[] = { { tr("Poco"), 50 }, { tr("Medio"), 100 }, { tr("Mucho"), 200 }, { tr("Muchísimo"), 400 } };
	for (int i = 0; i < 4; i++) {
		if (i % 2) ImGui::SameLine();
		if (choiceButton(amounts[i].label, plateauAmount_ == amounts[i].amount, ImVec2(130, 0))) plateauAmount_ = amounts[i].amount;
	}
	ImGui::SliderFloat(tr("Pincel"), &brushRadius_, 25, 1000, "%.0f");
	ImGui::SetItemTooltip(tr("Tamaño del pincel (también con Ctrl + rueda del ratón)."));
	ImGui::Spacing();
	ImGui::TextColored(kWarn, tr("Los bordes son acantilados:"));
	ImGui::TextColored(kWarn, tr("los Pikmin no pueden subirlos."));
	hint(tr("Para unir alturas pon una rampa (Poner cosas > Puentes y rampas) o suaviza el borde con «Montañas y hoyos»."));
}

void App::drawScatterOptions()
{
	ImGui::SeparatorText(tr("Decorar"));
	if (choiceButton(tr("Poner"), !scatterErase_, ImVec2(110, 34))) scatterErase_ = false;
	ImGui::SameLine();
	if (choiceButton(tr("Quitar"), scatterErase_, ImVec2(110, 34))) scatterErase_ = true;
	hint(scatterErase_ ? tr("Pinta para quitar la decoración elegida abajo (sin nada elegido, toda).")
	                   : tr("Pinta para esparcir lo elegido abajo, girado y de tamaños distintos. Mantén Mayús para quitar."));
	ImGui::SliderFloat(tr("Pincel"), &brushRadius_, 25, 1000, "%.0f");
	ImGui::SliderFloat(tr("Cuánta"), &scatterDensity_, 1.0f, 30.0f, "%.0f");
	ImGui::SliderFloat(tr("Variedad de tamaño"), &scatterSizeVar_, 0.0f, 0.6f, "%.2f");
	ImGui::Checkbox(tr("Los Pikmin no lo atraviesan"), &scatterCollide_);
	hint(tr("Para hierbas y flores, mejor sin marcar: así no cortan el paso."));

	// Mezclas rápidas.
	ImGui::SeparatorText(tr("¿Qué pones?"));
	const struct {
		const char* label;
		std::vector<std::string> set;
	} mixes[] = {
		{ tr("Pradera"), { "objects/weeds/weed1.mod", "objects/plants/hutaba.mod", "objects/plants/hae.mod" } },
		{ tr("Flores"), { "objects/plants/tanpopo.mod", "objects/plants/ooinu_l.mod", "objects/plants/ooinu_s.mod", "objects/plants/clover.mod" } },
		{ tr("Rocas"), { "objects/weeds/stone1.mod", "objects/weeds/stone2.mod", "objects/weeds/stone3.mod" } },
		{ tr("Orilla"), { "objects/plants/mizukusa.mod", "objects/plants/ine.mod", "objects/plants/wakame_s.mod" } },
		{ tr("Bosque"), { "objects/plants/hae.mod", "objects/plants/chidome.mod", "objects/plants/kinokolt.mod", "objects/weeds/weed1.mod" } },
	};
	int k = 0;
	for (const auto& m : mixes) {
		if (k++ % 3) ImGui::SameLine();
		if (choiceButton(m.label, scatterSet_ == m.set, ImVec2(86, 30))) scatterSet_ = m.set;
	}
	hint(tr("O elige uno a uno (puedes marcar varios):"));
	int col = 0;
	for (const DecorItem& it : decorItems()) {
		const std::string grp = it.group;
		if (grp != "Plantas" && grp != "Rocas y obstáculos") continue;
		ImGui::PushID(it.path);
		auto pos      = std::find(scatterSet_.begin(), scatterSet_.end(), std::string(it.path));
		const bool on = pos != scatterSet_.end();
		if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.6f, 0.15f, 1));
		const unsigned t   = assetThumb(it.path);
		const bool pressed = t ? ImGui::ImageButton("##s", ImTextureID(std::intptr_t(t)), ImVec2(52, 52)) : ImGui::Button(tr(it.name), ImVec2(60, 60));
		if (on) ImGui::PopStyleColor();
		ImGui::SetItemTooltip("%s", tr(it.name));
		if (pressed) {
			if (on) scatterSet_.erase(pos);
			else scatterSet_.push_back(it.path);
		}
		ImGui::PopID();
		if (++col % 4) ImGui::SameLine();
	}
	if (col % 4) ImGui::NewLine();
}

void App::scatter(const V3& at)
{
	MapDoc& d     = ed_.doc;
	const float r = brushRadius_;
	auto rnd      = [&]() {
        scatterSeed_ = scatterSeed_ * 1664525u + 1013904223u;
        return float((scatterSeed_ >> 8) & 0xFFFF) / 65535.0f;
	};
	auto inSet = [&](const std::string& a) { return scatterSet_.empty() || std::find(scatterSet_.begin(), scatterSet_.end(), a) != scatterSet_.end(); };
	if (scatterErase_) {
		std::vector<ItemRef> gone;
		for (std::size_t i = 0; i < d.props.size(); i++) {
			const nectar::DocProp& p = d.props[i];
			for (int side = 0; side < (d.symmetric ? 2 : 1); side++) {
				const float px = side ? float(-p.x) : float(p.x), pz = side ? float(-p.z) : float(p.z);
				if ((px - at.x) * (px - at.x) + (pz - at.z) * (pz - at.z) <= r * r && inSet(p.asset)) {
					gone.push_back({ ItemType::Prop, int(i), 0 });
					break;
				}
			}
		}
		if (!gone.empty()) {
			ed_.clearSelection();
			removeItems(d, gone);
			ed_.touch();
		}
		return;
	}
	if (scatterSet_.empty()) return;
	// El juego tiene un tope de vértices por mapa: se para un poco antes.
	constexpr std::size_t kMaxProps = 1500;
	if (d.props.size() >= kMaxProps || (compiled_.ok && compiled_.vertices > 58000)) {
		if (!scatterFullWarned_) notify(1, tr("Ya hay mucha decoración: el mapa no cabría en el juego."));
		scatterFullWarned_ = true;
		return;
	}
	scatterFullWarned_  = false;
	const float spacing = 100.0f / std::sqrt(scatterDensity_);
	const nectar::DocTerrain& g = d.terrain;
	auto wet = [&](float x, float z) {
		if (!g.grid || g.water.empty()) return false;
		const int gx = std::max(0, std::min(g.nx, int(std::lround((x - d.minX) / d.cell))));
		const int gz = std::max(0, std::min(g.nz, int(std::lround((z - d.minZ) / d.cell))));
		return g.water[std::size_t(g.vertexIndex(gx, gz))] >= 128;
	};
	bool changed = false;
	for (int attempt = 0; attempt < 10; attempt++) {
		const float a = rnd() * 6.2831853f, dist = std::sqrt(rnd()) * r;
		const float x = at.x + std::cos(a) * dist, z = at.z + std::sin(a) * dist;
		if (!docSolid(x, z) || wet(x, z)) continue;
		// Ni en cuestas fuertes ni junto a un borde.
		const float h = docHeight(x, z);
		bool steep    = false;
		for (int k = 0; k < 4 && !steep; k++) {
			const float ox = (k == 0 ? 20.0f : k == 1 ? -20.0f : 0.0f), oz = (k == 2 ? 20.0f : k == 3 ? -20.0f : 0.0f);
			steep = !docSolid(x + ox, z + oz) || std::fabs(docHeight(x + ox, z + oz) - h) > 12.0f;
		}
		if (steep) continue;
		// Separación con lo que ya hay (y con su copia del otro lado).
		bool crowded = false;
		for (const nectar::DocProp& p : d.props) {
			for (int side = 0; side < (d.symmetric ? 2 : 1) && !crowded; side++) {
				const float px = side ? float(-p.x) : float(p.x), pz = side ? float(-p.z) : float(p.z);
				crowded = (px - x) * (px - x) + (pz - z) * (pz - z) < spacing * spacing;
			}
			if (crowded) break;
		}
		if (crowded) continue;
		nectar::DocProp p;
		p.asset   = scatterSet_[std::min(scatterSet_.size() - 1, std::size_t(rnd() * float(scatterSet_.size())))];
		p.x       = x;
		p.z       = z;
		p.y       = h;
		p.ry      = rnd() * 360.0;
		p.scale   = std::max(0.2, 1.0 + (rnd() * 2.0 - 1.0) * scatterSizeVar_);
		p.collide = scatterCollide_;
		d.props.push_back(p);
		changed = true;
		if (d.props.size() >= kMaxProps) break;
	}
	if (changed) ed_.touch();
}

void App::drawWallOptions()
{
	ImGui::SeparatorText(tr("Muros"));
	hint(tr("Haz clic donde empieza el muro y arrastra hasta donde acaba."));
	ImGui::Text(tr("Altura"));
	if (choiceButton(tr("Bajo"), wallHeight_ == 60, ImVec2(80, 0))) wallHeight_ = 60;
	ImGui::SameLine();
	if (choiceButton(tr("Medio"), wallHeight_ == 150, ImVec2(80, 0))) wallHeight_ = 150;
	ImGui::SameLine();
	if (choiceButton(tr("Alto"), wallHeight_ == 300, ImVec2(80, 0))) wallHeight_ = 300;
	ImGui::SliderFloat(tr("Grosor"), &wallThickness_, 10, 200, "%.0f");
	hint(tr("Los muros bajos se pueden ver por encima, pero los Pikmin no los cruzan."));
}

void App::growMap(int dx, int dz)
{
	if (!ensureTerrain()) return;
	nectar::resizeTerrain(ed_.doc, dx / 2, dx - dx / 2, dz / 2, dz - dz / 2);
	ed_.touch();
}

const char* App::toolHelp() const
{
	switch (tool_) {
	case Tool::Move: return tr("Haz clic en algo y arrástralo. Q / E: girar. Supr: borrar. Botón derecho: girar la vista.");
	case Tool::Place: return tr("Haz clic en el mapa para poner lo elegido a la izquierda.");
	case Tool::Scatter: return scatterErase_ ? tr("Pinta para quitar decoración.") : tr("Pinta para esparcir decoración (Mayús: quitarla).");
	case Tool::Floor: return floorErase_ ? tr("Pinta con el ratón para hacer huecos.") : tr("Pinta con el ratón para añadir suelo (Mayús: hacer huecos).");
	case Tool::Height: return tr("Pinta con el ratón para subir o bajar el terreno (Mayús: al revés).");
	case Tool::Plateau: return plateauUp_ ? tr("Pinta una zona para elevarla, plana (Mayús: hundir).") : tr("Pinta una zona para hundirla, plana (Mayús: elevar).");
	case Tool::Paint: return tr("Pinta con el ratón para cambiar el aspecto del suelo.");
	case Tool::Water: return waterErase_ ? tr("Pinta para quitar agua.") : tr("Pinta sobre el suelo para hacer un lago (Mayús: quitar agua).");
	case Tool::Walls: return tr("Haz clic y arrastra para dibujar un muro.");
	}
	return "";
}

void App::drawSidePanel(const Rect& r)
{
	ImGui::SetNextWindowPos(ImVec2(r.x, r.y));
	ImGui::SetNextWindowSize(ImVec2(r.w, r.h));
	ImGui::Begin("##side", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	ed_.pruneSelection();
	ImGui::BeginChild("top", ImVec2(0, r.h * 0.6f));
	if (!ed_.selection.empty()) drawSelected();
	else drawMapPanel();
	ImGui::EndChild();
	const ImVec2 cmin = ImGui::GetCursorScreenPos();
	checklistRect_    = { cmin.x, cmin.y, r.w - 16, r.h * 0.4f - 16 };
	ImGui::BeginChild("check");
	drawChecklist();
	ImGui::EndChild();
	ImGui::End();
}

void App::drawMinimap()
{
	const MapDoc& d             = ed_.doc;
	const nectar::DocTerrain& g = d.terrain;
	const float spanX = float(d.maxX - d.minX), spanZ = float(d.maxZ - d.minZ);
	if (spanX <= 0 || spanZ <= 0) return;
	ImGui::SeparatorText(tr("Vista del mapa"));
	const float w = ImGui::GetContentRegionAvail().x;
	const float h = std::min(w * spanZ / spanX, w * 1.2f);
	const ImVec2 o = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##minimap", ImVec2(w, h));
	const bool hovered = ImGui::IsItemHovered();
	const float sx = w / spanX, sz = h / spanZ;
	// Mundo -> minimapa (x a la derecha, z hacia abajo, como la vista cenital).
	auto toMap   = [&](float x, float z) { return ImVec2(o.x + (x - float(d.minX)) * sx, o.y + (z - float(d.minZ)) * sz); };
	auto toWorld = [&](const ImVec2& p) { return V3(float(d.minX) + (p.x - o.x) / sx, 0, float(d.minZ) + (p.y - o.y) / sz); };
	if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		const V3 p = toWorld(ImGui::GetIO().MousePos);
		cam_.target = V3(p.x, docHeight(p.x, p.z), p.z);
	}
	if (hovered) ImGui::SetTooltip("%s", tr("Haz clic para mover la vista ahí."));

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(24, 26, 31, 255), 4.0f);
	// Suelo: celdas sombreadas por altura; agua en azul; huecos vacíos.
	const int cols = std::max(1, std::min(96, g.grid ? g.nx : 48));
	const int rows = std::max(1, std::min(96, int(float(cols) * spanZ / spanX + 0.5f)));
	float hmin = 0, hmax = 1;
	if (courseMode()) {
		hmin = 1e30f, hmax = -1e30f;
		for (float v : courseField_.h) {
			if (std::isnan(v)) continue;
			hmin = std::min(hmin, v);
			hmax = std::max(hmax, v);
		}
		if (hmin > hmax) hmin = 0, hmax = 1;
		hmax = std::max(hmin + 1.0f, hmax);
	} else if (g.grid && !g.heights.empty()) {
		hmin = float(*std::min_element(g.heights.begin(), g.heights.end()));
		hmax = std::max(hmin + 1.0f, float(*std::max_element(g.heights.begin(), g.heights.end())));
	}
	for (int iz = 0; iz < rows; iz++) {
		for (int ix = 0; ix < cols; ix++) {
			const float x = float(d.minX) + (ix + 0.5f) * spanX / cols, z = float(d.minZ) + (iz + 0.5f) * spanZ / rows;
			if (!docSolid(x, z)) continue;
			bool wet = false;
			if (g.grid && !g.water.empty()) {
				const int gx = std::max(0, std::min(g.nx, int(std::lround((x - d.minX) / d.cell))));
				const int gz = std::max(0, std::min(g.nz, int(std::lround((z - d.minZ) / d.cell))));
				wet          = g.water[std::size_t(g.vertexIndex(gx, gz))] >= 128;
			}
			const float k = 0.62f + 0.38f * std::max(0.0f, std::min(1.0f, (docHeight(x, z) - hmin) / (hmax - hmin)));
			const ImU32 c = wet ? IM_COL32(58, 118, 196, 255) : IM_COL32(int(96 * k), int(150 * k), int(62 * k), 255);
			const ImVec2 a = toMap(float(d.minX) + ix * spanX / cols, float(d.minZ) + iz * spanZ / rows);
			const ImVec2 b = toMap(float(d.minX) + (ix + 1) * spanX / cols, float(d.minZ) + (iz + 1) * spanZ / rows);
			dl->AddRectFilled(a, ImVec2(b.x + 0.5f, b.y + 0.5f), c);
		}
	}
	// Muros (y su copia si el mapa es espejo).
	for (const nectar::DocWall& wl : d.walls) {
		const int segs = wl.isArc ? std::max(1, wl.segments) : 1;
		for (int side = 0; side < (d.symmetric ? 2 : 1); side++) {
			const float m = side ? -1.0f : 1.0f;
			for (int k = 0; k < segs; k++) {
				float ax = float(wl.ax), az = float(wl.az), bx = float(wl.bx), bz = float(wl.bz);
				if (wl.isArc) {
					const float a0 = float(wl.from + k * (wl.to - wl.from) / segs) * 0.0174533f;
					const float a1 = float(wl.from + (k + 1) * (wl.to - wl.from) / segs) * 0.0174533f;
					ax = float(wl.cx + wl.radius * std::cos(a0)), az = float(wl.cz + wl.radius * std::sin(a0));
					bx = float(wl.cx + wl.radius * std::cos(a1)), bz = float(wl.cz + wl.radius * std::sin(a1));
				}
				dl->AddLine(toMap(m * ax, m * az), toMap(m * bx, m * bz), IM_COL32(150, 120, 90, 255), 2.5f);
			}
		}
	}
	// Cosas del mapa: puntos con el color de su jugador.
	for (const ItemRef& it : allItems(d)) {
		const ItemCaps caps = itemCaps(it.type);
		if (it.type == ItemType::Wall || it.type == ItemType::Pond) continue;
		const Xform x = getXform(d, it);
		ImU32 c       = IM_COL32(235, 235, 235, 255);
		if (caps.perPlayer) {
			const Rgba pc = playerColour(it.index);
			c             = ImGui::ColorConvertFloat4ToU32(ImVec4(pc.r, pc.g, pc.b, 1));
		} else if (it.type == ItemType::Piece || it.type == ItemType::BigPiece) {
			c = IM_COL32(240, 200, 60, 255);
		} else if (it.type == ItemType::Enemy || it.type == ItemType::Guard) {
			c = IM_COL32(230, 90, 200, 255);
		} else if (it.type == ItemType::Prop) {
			c = IM_COL32(140, 150, 140, 200);
		}
		const float r = caps.perPlayer ? 4.0f : 3.0f;
		dl->AddCircleFilled(toMap(float(x.x), float(x.z)), r, c);
		if (d.symmetric && caps.twinned) dl->AddCircleFilled(toMap(float(-x.x), float(-x.z)), r, c);
	}
	// Dónde mira la cámara.
	const ImVec2 t = toMap(cam_.target.x, cam_.target.z);
	if (cam_.topDown) {
		dl->AddCircle(t, 7.0f, IM_COL32(255, 255, 255, 230), 16, 2.0f);
	} else {
		const V3 e      = cam_.eye();
		ImVec2 dir(cam_.target.x - e.x, cam_.target.z - e.z);
		const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
		if (len > 0) dir = ImVec2(dir.x / len, dir.y / len);
		const ImVec2 back(t.x - dir.x * 14.0f, t.y - dir.y * 14.0f);
		dl->AddTriangleFilled(ImVec2(t.x + dir.x * 6, t.y + dir.y * 6), ImVec2(back.x - dir.y * 8, back.y + dir.x * 8), ImVec2(back.x + dir.y * 8, back.y - dir.x * 8),
		                      IM_COL32(255, 255, 255, 110));
		dl->AddCircleFilled(t, 3.5f, IM_COL32(255, 255, 255, 255));
	}
	dl->AddRect(o, ImVec2(o.x + w, o.y + h), IM_COL32(80, 86, 96, 255), 4.0f);
}

void App::drawSelected()
{
	MapDoc& d           = ed_.doc;
	const ItemRef r     = ed_.selection.back();
	const ItemCaps caps = itemCaps(r.type);
	bool ch             = false;
	if (ed_.selection.size() > 1) ImGui::TextColored(kDim, tr("%zu cosas elegidas"), ed_.selection.size());
	ImGui::SeparatorText(itemName(d, r).c_str());

	if (caps.rotateY) {
		if (ImGui::Button(tr("Girar a la izquierda (Q)"), ImVec2(-1, 32))) rotateSelection(-cfg_.snapAngle);
		if (ImGui::Button(tr("Girar a la derecha (E)"), ImVec2(-1, 32))) rotateSelection(cfg_.snapAngle);
	}
	if (caps.height) {
		const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		if (ImGui::Button(tr("Subir (Re Pág)"), ImVec2(half, 32))) liftSelection(ImGui::GetIO().KeyShift ? 100 : 20);
		ImGui::SameLine();
		if (ImGui::Button(tr("Hundir (Av Pág)"), ImVec2(half, 32))) liftSelection(ImGui::GetIO().KeyShift ? -100 : -20);
		hint(tr("Con Mayúsculas sube o baja más de golpe."));
	}
	const auto i = std::size_t(r.index);
	switch (r.type) {
	case ItemType::Pellet: {
		std::string& id = d.pellets[i].id;
		int colour = pelletColour(id), value = pelletValue(id);
		const char* colours[] = { tr("Roja"), tr("Azul"), tr("Amarilla") };
		for (int c = 0; c < 3; c++) {
			if (c) ImGui::SameLine();
			if (choiceButton(colours[c], colour == c, ImVec2(90, 0))) {
				id = pelletId(c, value);
				ch = true;
			}
		}
		const int values[] = { 1, 5, 10, 20 };
		for (int k = 0; k < 4; k++) {
			if (k) ImGui::SameLine();
			char lbl[8];
			std::snprintf(lbl, sizeof(lbl), "%d", values[k]);
			if (choiceButton(lbl, value == values[k], ImVec2(64, 0))) {
				id = pelletId(colour, values[k]);
				ch = true;
			}
		}
		hint(tr("El número es cuántos Pikmin salen al llevarla a la cebolla de su color."));
		break;
	}
	case ItemType::Piece: {
		if (const nectar::ShipPartInfo* info = partInfo(d.pieces[i].part)) {
			ImGui::TextColored(kDim, tr("La levantan %d Pikmin (más rápido con %d)."), info->carryMin, info->carryMax);
			if (ImGui::SmallButton(tr("Que la elija el juego"))) {
				d.pieces[i].part.clear();
				ch = true;
			}
		} else {
			hint(tr("Pieza elegida por el juego según su tipo. Para una concreta, ponla desde «Poner cosas»."));
		}
		int& k = d.pieces[i].kind;
		if (choiceButton(tr("Normal"), k <= 2, ImVec2(-1, 0))) {
			k  = 0;
			ch = true;
		}
		if (choiceButton(tr("Vigilada (junto a un Bulborb)"), k == 3, ImVec2(-1, 0))) {
			k  = 3;
			ch = true;
		}
		if (choiceButton(tr("En el agua"), k == 4, ImVec2(-1, 0))) {
			k  = 4;
			ch = true;
		}
		hint(tr("Cada pieza llevada al cohete da puntos. La de en medio vale más."));
		break;
	}
	case ItemType::BigPiece: hint(tr("Aparece durante la partida (a los pocos minutos) y vale mucho.")); break;
	case ItemType::Enemy: {
		if (const nectar::EnemyInfo* info = enemyInfo(d.enemies[i].type)) hint(tr(info->hint));
		hint(tr("Se comporta como en el juego."));
		break;
	}
	case ItemType::Gen: hint(tr("Es del nivel original: el juego lo crea igual que en la historia. Puedes moverlo, subirlo o quitarlo.")); break;
	case ItemType::Object: {
		nectar::DocObject& o            = d.objects[i];
		const nectar::ObjectInfo* info = objectInfo(o.kind);
		if (info) hint(tr(info->hint));
		float above = float(o.y);
		if (ImGui::DragFloat(tr("Altura sobre el suelo"), &above, 2.0f, -400.0f, 1000.0f, "%.0f")) {
			o.y = above;
			ch  = true;
		}
		if (o.kind.rfind("bridge", 0) == 0 || o.kind.rfind("slope", 0) == 0) {
			ImGui::Text(tr("Trabajo para construirlo:"));
			const struct {
				const char* label;
				int work;
			} works[] = { { tr("Poco"), 1 }, { tr("Normal"), 2 }, { tr("Mucho"), 4 } };
			for (int k = 0; k < 3; k++) {
				if (k) ImGui::SameLine();
				if (choiceButton(works[k].label, o.work == works[k].work, ImVec2(90, 0))) {
					o.work = works[k].work;
					ch     = true;
				}
			}
			hint(tr("Se construye hacia donde mira: gíralo con Q / E."));
		}
		if (o.kind == "push_rock") {
			float push = float(o.push);
			if (ImGui::SliderFloat(tr("Hasta dónde se empuja"), &push, 50, 1500, "%.0f")) {
				o.push = push;
				ch     = true;
			}
			hint(tr("La línea amarilla enseña hacia dónde va. Gíralo con Q / E."));
		}
		break;
	}
	case ItemType::Guard: hint(tr("Un Bulborb dormido: si los Pikmin pasan cerca, se despierta.")); break;
	case ItemType::Onion: hint(tr("Aquí se llevan las pastillas de su color para conseguir Pikmin.")); break;
	case ItemType::Base: hint(tr("Donde empieza el capitán de este jugador.")); break;
	case ItemType::Rocket: hint(tr("Aquí se llevan las piezas. Si el rival lo destruye, pierdes.")); break;
	case ItemType::Gate: hint(tr("Una puerta de roca que se rompe con rocas bomba: un atajo.")); break;
	case ItemType::BombPile: hint(tr("Rocas bomba para que los Pikmin amarillos rompan la puerta.")); break;
	case ItemType::Prop: {
		nectar::DocProp& p = d.props[i];
		float s = float(p.scale);
		if (ImGui::SliderFloat(tr("Tamaño"), &s, 0.25f, 4.0f, "%.2f")) {
			p.scale = s;
			ch      = true;
		}
		float above = float(p.y - docHeight(float(p.x), float(p.z)));
		if (ImGui::DragFloat(tr("Altura sobre el suelo"), &above, 2.0f, -600.0f, 1500.0f, "%.0f")) {
			p.y = docHeight(float(p.x), float(p.z)) + above;
			ch  = true;
		}
		ch |= ImGui::Checkbox(tr("Los Pikmin no lo atraviesan"), &p.collide);
		break;
	}
	case ItemType::Wall: {
		nectar::DocWall& w = d.walls[i];
		float h = float(w.height), t = float(w.thickness);
		if (ImGui::SliderFloat(tr("Altura"), &h, 20, 600, "%.0f")) {
			w.height = h;
			ch       = true;
		}
		if (ImGui::SliderFloat(tr("Grosor"), &t, 10, 200, "%.0f")) {
			w.thickness = t;
			ch          = true;
		}
		if (!w.isArc) hint(tr("Arrastra los círculos de los extremos para alargarlo o girarlo."));
		if (ImGui::Checkbox(tr("Muro curvo"), &w.isArc)) {
			ch = true;
			if (w.isArc) {
				w.cx     = (w.ax + w.bx) * 0.5;
				w.cz     = (w.az + w.bz) * 0.5;
				w.radius = std::max(60.0, std::hypot(w.bx - w.ax, w.bz - w.az) * 0.5);
				w.from   = 0;
				w.to     = 90;
			} else {
				w.ax = w.cx - w.radius;
				w.az = w.cz;
				w.bx = w.cx + w.radius;
				w.bz = w.cz;
			}
		}
		if (w.isArc) {
			float rad = float(w.radius), open = float(w.to - w.from);
			if (ImGui::SliderFloat(tr("Radio"), &rad, 40, 2000, "%.0f")) {
				w.radius = rad;
				ch       = true;
			}
			if (ImGui::SliderFloat(tr("Cuánto cierra"), &open, 10, 360, "%.0f°")) {
				w.to = w.from + open;
				ch   = true;
			}
			w.segments = std::max(3, int(std::fabs(w.to - w.from) / 15.0));
		}
		break;
	}
	case ItemType::Pond: {
		nectar::DocPond& p = d.ponds[i];
		float rx = float(p.rx), rz = float(p.rz), island = float(p.island);
		if (ImGui::SliderFloat(tr("Ancho"), &rx, 40, 1500, "%.0f")) {
			p.rx = rx;
			ch   = true;
		}
		if (ImGui::SliderFloat(tr("Largo"), &rz, 40, 1500, "%.0f")) {
			p.rz = rz;
			ch   = true;
		}
		if (ImGui::SliderFloat(tr("Isla en el centro"), &island, 0, float(std::min(p.rx, p.rz)) - 20, "%.0f")) {
			p.island = island;
			ch       = true;
		}
		hint(tr("Solo los Pikmin azules pueden entrar en el agua."));
		break;
	}
	}

	ImGui::Spacing();
	if (caps.perPlayer) {
		ImGui::TextColored(r.index == 0 ? ImVec4(1, 0.5f, 0.45f, 1) : ImVec4(0.45f, 0.65f, 1, 1), tr("Es del %s."), r.index == 0 ? tr("Jugador 1") : tr("Jugador 2"));
		if (d.symmetric && mirrorEdits_) hint(tr("Al moverlo, el del otro jugador se coloca solo en el lado contrario."));
	}
	if (caps.twinned && d.symmetric) hint(tr("Como el mapa es espejo, el otro jugador tiene uno igual en su lado (se ve más claro)."));
	if (caps.removable) {
		if (ImGui::Button(tr("Duplicar (Ctrl+D)"), ImVec2(150, 32))) duplicateSelection();
		ImGui::SameLine();
		if (ImGui::Button(tr("Borrar (Supr)"), ImVec2(-1, 32))) {
			deleteSelection();
			return;
		}
	}
	if (ImGui::Button(tr("Ver de cerca (F)"), ImVec2(-1, 0))) focusSelection();
	if (ch) ed_.touch();
}

void App::drawMapPanel()
{
	MapDoc& d = ed_.doc;
	bool ch   = false;
	ImGui::SeparatorText(tr("Tu mapa"));
	char buf[256];
	copyStr(buf, sizeof(buf), d.name);
	ImGui::SetNextItemWidth(-90);
	if (ImGui::InputText(tr("Nombre"), buf, sizeof(buf))) {
		d.name = buf;
		ch     = true;
	}
	copyStr(buf, sizeof(buf), d.author);
	ImGui::SetNextItemWidth(-90);
	if (ImGui::InputText(tr("Autor"), buf, sizeof(buf))) {
		d.author = buf;
		ch       = true;
	}
	ImGui::Spacing();
	ch |= ImGui::Checkbox(tr("Mapa espejo"), &d.symmetric);
	hint(d.symmetric ? tr("Lo que pongas en un lado aparece igual en el otro, para que sea justo para los dos jugadores.")
	                 : tr("Cada lado es distinto: tú decides dónde va todo."));
	if (d.symmetric) ImGui::Checkbox(tr("Mover a la vez lo de los dos jugadores"), &mirrorEdits_);
	ImGui::Checkbox(tr("Ver el lado del otro jugador"), &showTwins_);
	ImGui::Spacing();
	hint(tr("Elige algo del mapa con «Mover y girar» para ver sus opciones aquí."));
	drawMinimap();

	if (ImGui::CollapsingHeader(tr("Ambiente"))) {
		hint(tr("La luz, la niebla y los colores del cielo salen de un escenario del juego."));
		if (ImGui::BeginCombo(tr("Como en"), sceneName(d.baseStage).c_str())) {
			for (const nectar::CatalogEntry& e : catalog_.entries) {
				if (e.kind != nectar::AssetKind::Scene || !e.error.empty()) continue;
				const std::string name = sceneName(e.path);
				if (name == e.path) continue; // solo los escenarios de verdad
				if (ImGui::Selectable((name + "##" + e.path).c_str(), e.path == d.baseStage)) {
					d.baseStage = e.path;
					ch          = true;
				}
			}
			ImGui::EndCombo();
		}
		float sun = float(std::atan2(d.sun[2], d.sun[0]) / kDeg);
		if (ImGui::SliderFloat(tr("De dónde viene el sol"), &sun, -180, 180, "%.0f°")) {
			const double a = sun * kDeg, len = std::hypot(d.sun[0], d.sun[2]);
			d.sun[0]       = std::cos(a) * len;
			d.sun[2]       = std::sin(a) * len;
			ch             = true;
		}
	}
	if (ImGui::CollapsingHeader(tr("Opciones avanzadas"))) {
		hint(tr("Los Pikmin siguen una red de caminos que se hace sola. Aquí puedes ajustarla."));
		float step = float(d.routeStep);
		if (ImGui::SliderFloat(tr("Separación de caminos"), &step, 80, 300, "%.0f")) {
			d.routeStep = step;
			ch          = true;
		}
		hint(tr("Líneas que los Pikmin solo cruzan por puntos concretos (como la muralla de cada base)."));
		for (std::size_t k = 0; k < d.barrierX.size(); k++) {
			ImGui::PushID(int(k));
			float v = float(d.barrierX[k]);
			ImGui::SetNextItemWidth(160);
			if (ImGui::DragFloat(tr("Línea en x"), &v, 1, -6000, 6000, "%.0f")) {
				d.barrierX[k] = v;
				ch            = true;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(tr("Quitar"))) {
				d.barrierX.erase(d.barrierX.begin() + long(k));
				ch = true;
				ImGui::PopID();
				break;
			}
			ImGui::PopID();
		}
		for (std::size_t k = 0; k < d.passes.size(); k++) {
			ImGui::PushID(1000 + int(k));
			float v[2] = { float(d.passes[k].first), float(d.passes[k].second) };
			ImGui::SetNextItemWidth(160);
			if (ImGui::DragFloat2(tr("Paso"), v, 1)) {
				d.passes[k] = { v[0], v[1] };
				ch          = true;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(tr("Quitar"))) {
				d.passes.erase(d.passes.begin() + long(k));
				ch = true;
				ImGui::PopID();
				break;
			}
			ImGui::PopID();
		}
		if (ImGui::SmallButton(tr("Añadir línea"))) {
			d.barrierX.push_back(0);
			ch = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(tr("Añadir paso"))) {
			d.passes.push_back({ 0, 0 });
			ch = true;
		}
	}
	if (ch) ed_.touch();
}

void App::drawChecklist()
{
	ImGui::SeparatorText(tr("¿Está listo para jugar?"));
	const std::vector<Issue> issues = checkMap();
	bool blocking = false;
	for (const Issue& i : issues) blocking = blocking || i.blocking;
	if (preview_.busy() && compiled_.generation != ed_.liveGeneration) ImGui::TextColored(kDim, tr("Comprobando..."));
	else if (!blocking) ImGui::TextColored(kGood, tr("¡Sí! Pulsa «Probar en el juego»."));
	else ImGui::TextColored(kBad, tr("Todavía no. Arregla esto:"));
	int n = 0;
	for (const Issue& i : issues) {
		ImGui::PushID(n++);
		ImGui::PushStyleColor(ImGuiCol_Text, i.blocking ? kBad : kWarn);
		ImGui::Bullet();
		ImGui::TextWrapped("%s", i.text.c_str());
		ImGui::PopStyleColor();
		if (i.hasItem && ImGui::SmallButton(tr("Llévame allí"))) {
			ed_.selection = { i.item };
			tool_         = Tool::Move;
			focusSelection();
		}
		ImGui::PopID();
	}
	if (compiled_.ok) {
		const float use = float(compiled_.vertices) / 65535.0f;
		ImGui::Spacing();
		ImGui::TextColored(kDim, tr("Tamaño para el juego:"));
		ImGui::ProgressBar(use, ImVec2(-1, 0), use < 0.75f ? tr("Bien") : use < 1.0f ? tr("Casi lleno") : tr("Demasiado"));
	}
}

void App::drawModals()
{
	drawMaterialBrowser();
	drawHelp();
	if (openSaveAs_) {
		ImGui::OpenPopup(tr("Guardar mapa"));
		openSaveAs_ = false;
	}
	if (openOpen_) {
		ImGui::OpenPopup(tr("Abrir mapa"));
		openOpen_ = false;
	}
	if (openNew_) {
		ImGui::OpenPopup(tr("Mapa nuevo"));
		openNew_ = false;
	}
	if (confirmQuit_) {
		ImGui::OpenPopup(tr("Salir"));
		confirmQuit_ = false;
	}

	if (ImGui::BeginPopupModal(tr("Guardar mapa"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text(tr("¿Cómo se llama tu mapa?"));
		ImGui::SetNextItemWidth(360);
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		const bool enter = ImGui::InputText("##name", saveName_, sizeof(saveName_), ImGuiInputTextFlags_EnterReturnsTrue);
		hint(tr("Aparecerá con ese nombre en el juego: modo VS > Mapa."));
		if (ImGui::Button(tr("Guardar"), ImVec2(150, 34)) || enter) {
			ed_.doc.name = saveName_[0] ? saveName_ : tr("Mi mapa");
			// Carpeta a partir del nombre, sin pisar otro mapa.
			std::string id = slug(ed_.doc.name), path;
			std::error_code ec;
			for (int n = 1;; n++) {
				const std::string folder = n == 1 ? id : id + "_" + std::to_string(n);
				path                     = (fs::path(cfg_.userMapsDir()) / folder / "map.json").string();
				if (!fs::exists(path, ec)) break;
			}
			ImGui::CloseCurrentPopup();
			if (saveMap(path) && testAfterSave_) testInGame();
		}
		ImGui::SameLine();
		if (ImGui::Button(tr("Cancelar"), ImVec2(150, 34))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopupModal(tr("Abrir mapa"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text(tr("Tus mapas"));
		ImGui::BeginChild("list", ImVec2(560, 340), ImGuiChildFlags_Borders);
		for (const auto& m : openList_) {
			ImGui::PushID(m.second.c_str());
			const unsigned t = thumb((fs::path(m.second).parent_path() / "thumb.png").string());
			if (t) {
				ImGui::Image(ImTextureID(std::intptr_t(t)), ImVec2(64, 64));
				ImGui::SameLine();
			}
			if (ImGui::Selectable(m.first.c_str(), false, 0, ImVec2(0, 64))) {
				if (ed_.dirty) autosave();
				openMap(m.second);
				ImGui::CloseCurrentPopup();
			}
			ImGui::PopID();
		}
		if (openList_.empty()) hint(tr("Todavía no has guardado ninguno."));
		ImGui::EndChild();
		if (ImGui::Button(tr("Abrir otro archivo..."))) {
			const fs::path p = pikmin::launcher::platform::askForFile(tr("Abrir un mapa"), tr("Mapa"), "*.json");
			if (!p.empty()) {
				openMap(p.string());
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(tr("Cancelar"))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopupModal(tr("Mapa nuevo"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (ed_.dirty) ImGui::TextColored(kWarn, tr("Ojo: el mapa actual tiene cambios sin guardar."));
		ImGui::Text(tr("Elige la forma para empezar:"));
		struct S {
			const char* id;
			nectar::TerrainShape shape;
			const char* label;
		};
		const S shapes[] = { { "r", nectar::TerrainShape::Rectangle, tr("Rectángulo") }, { "o", nectar::TerrainShape::Oval, tr("Óvalo") },
			                 { "c", nectar::TerrainShape::Cross, tr("Cruz") },          { "i", nectar::TerrainShape::TwoIslands, tr("Dos islas") },
			                 { "a", nectar::TerrainShape::Ring, tr("Anillo") } };
		for (int i = 0; i < 5; i++) {
			if (i) ImGui::SameLine();
			if (shapeButton(shapes[i].id, shapes[i].shape, shapes[i].label, 104)) {
				newMap(shapes[i].shape);
				ImGui::CloseCurrentPopup();
			}
		}
		if (templatePickers(96)) ImGui::CloseCurrentPopup();
		if (coursePickers()) ImGui::CloseCurrentPopup();
		if (ImGui::Button(tr("Copiar la arena del juego"))) {
			openBuiltIn();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(tr("Cancelar"))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopupModal(tr("Salir"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text(tr("Tienes cambios sin guardar."));
		if (ImGui::Button(tr("Guardar y salir"))) {
			ImGui::CloseCurrentPopup();
			if (!ed_.path.empty() && saveMap(ed_.path)) quit_ = true;
			else saveOrAsk(false);
		}
		ImGui::SameLine();
		if (ImGui::Button(tr("Salir sin guardar"))) quit_ = true;
		ImGui::SameLine();
		if (ImGui::Button(tr("Cancelar"))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

void App::drawHelp()
{
	if (openHelp_) {
		ImGui::OpenPopup("###help");
		openHelp_ = false;
	}
	ImGui::SetNextWindowSize(ImVec2(620, 0));
	if (!ImGui::BeginPopupModal((std::string(tr("Ayuda")) + "###help").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	languagePicker(cfg_);
	ImGui::SeparatorText(tr("Cómo se hace un mapa"));
	ImGui::TextWrapped(tr("1. Da forma al suelo con «Dibujar suelo» y súbelo o bájalo con «Montañas y hoyos»."));
	ImGui::TextWrapped(tr("2. Con «Poner cosas» añade piezas de la nave, pastillas, Bulborbs y decoración."));
	ImGui::TextWrapped(tr("3. Coloca las salidas, cebollas y cohetes de cada jugador con «Mover y girar»."));
	ImGui::TextWrapped(tr("4. Mira a la derecha si está listo y pulsa «Probar en el juego»."));
	ImGui::SeparatorText(tr("La vista"));
	ImGui::TextWrapped("%s%s", tr("Botón derecho + arrastrar: girar la vista. Rueda: acercar y alejar. Botón central: desplazar. "),
	                   tr("Botón derecho + W A S D: volar."));
	ImGui::SeparatorText(tr("Teclas"));
	ImGui::TextWrapped("%s%s", tr("1-9: herramientas · Ctrl+Z: deshacer · Ctrl+Y: rehacer · Ctrl+S: guardar · F5: probar · "),
	                   tr("Q / E: girar · Supr: borrar · Ctrl+D: duplicar · Ctrl+C / Ctrl+V: copiar y pegar · F: ver de cerca · T: vista desde arriba"));
	ImGui::Spacing();
	if (ImGui::Button(tr("Ver el tutorial otra vez"))) {
		tutorialStep_ = 0;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button(tr("Cambiar carpetas del juego"))) {
		copyStr(assetsBuf_, sizeof(assetsBuf_), cfg_.assetsDir);
		copyStr(gameBuf_, sizeof(gameBuf_), cfg_.gameExe);
		screen_ = Screen::Setup;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	std::error_code ec;
	const std::string autosavePath = (fs::path(Config::directory()) / "autosave.json").string();
	if (fs::exists(autosavePath, ec) && ImGui::Button(tr("Recuperar copia automática"))) {
		openMap(autosavePath);
		ed_.path  = "";
		ed_.dirty = true;
		ImGui::CloseCurrentPopup();
	}
	ImGui::Spacing();
	if (ImGui::Button(tr("Cerrar"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void App::drawTutorial()
{
	if (tutorialStep_ < 0) return;
	struct Step {
		const Rect* target;
		const char* title;
		const char* text;
	};
	const Step steps[] = {
		{ &viewRect_, tr("Este es tu mapa"), tr("Mantén el botón derecho del ratón y arrastra para girar la vista. Usa la rueda para acercarte o alejarte.") },
		{ &toolboxRect_, tr("Las herramientas"), tr("Aquí eliges qué hacer. Cada herramienta explica debajo cómo se usa.") },
		{ &floorButtonRect_, tr("Dibujar suelo"), tr("Con esta herramienta cambias la forma del mapa: añade suelo o haz huecos pintando con el ratón.") },
		{ &placeButtonRect_, tr("Poner cosas"), tr("Elige piezas de la nave, pastillas, enemigos o decoración y haz clic en el mapa para ponerlos.") },
		{ &checklistRect_, tr("¿Está listo?"), tr("Aquí ves si el mapa ya se puede jugar. Si algo falla, pulsa «Llévame allí» para ir directo.") },
		{ &testButtonRect_, tr("¡A jugar!"), tr("Cuando quieras, pulsa aquí: el mapa se guarda y se abre en el juego al momento.") },
	};
	const int count = int(sizeof(steps) / sizeof(steps[0]));
	if (tutorialStep_ >= count) {
		tutorialStep_     = -1;
		cfg_.tutorialDone = true;
		cfg_.save();
		return;
	}
	const Step& s = steps[tutorialStep_];
	ImDrawList* fg = ImGui::GetForegroundDrawList();
	const float t  = float(std::fmod(now(), 1.0));
	fg->AddRect(ImVec2(s.target->x - 4, s.target->y - 4), ImVec2(s.target->x + s.target->w + 4, s.target->y + s.target->h + 4),
	            IM_COL32(255, 200, 40, int(170 + 85 * std::sin(t * 6.2831853f))), 8, 0, 4);
	// El globo, junto a lo señalado y dentro de la pantalla.
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImVec2 pos(s.target->x + s.target->w + 16, s.target->y + 10);
	if (pos.x + 380 > vp->WorkPos.x + vp->WorkSize.x) pos.x = s.target->x - 396;
	if (s.target == &viewRect_) pos = ImVec2(s.target->x + s.target->w * 0.5f - 190, s.target->y + 80);
	pos.y = std::min(pos.y, vp->WorkPos.y + vp->WorkSize.y - 200);
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(380, 0));
	ImGui::SetNextWindowFocus();
	ImGui::Begin("##tutorial", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
	ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), tr("%s  (%d de %d)"), s.title, tutorialStep_ + 1, count);
	ImGui::TextWrapped("%s", s.text);
	ImGui::Spacing();
	if (tutorialStep_ > 0 && ImGui::Button(tr("Atrás"))) tutorialStep_--;
	if (tutorialStep_ > 0) ImGui::SameLine();
	if (ImGui::Button(tutorialStep_ + 1 < count ? tr("Siguiente") : tr("¡Empezar!"), ImVec2(120, 0))) tutorialStep_++;
	ImGui::SameLine();
	if (ImGui::Button(tr("Saltar"))) tutorialStep_ = count;
	ImGui::End();
}

void App::handleShortcuts()
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.WantTextInput) return;
	const bool ctrl = io.KeyCtrl;
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) saveOrAsk(false);
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
		if (io.KeyShift) ed_.redo();
		else ed_.undo();
	}
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) ed_.redo();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelection();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) paste();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateSelection();
	if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) testInGame();
	if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteSelection();
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		ed_.clearSelection();
		drawingWall_ = false;
	}
	// Letras sueltas: no mientras se vuela con el botón derecho (WASD).
	if (!ctrl && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
		if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) rotateSelection(-cfg_.snapAngle);
		if (ImGui::IsKeyPressed(ImGuiKey_E, false)) rotateSelection(cfg_.snapAngle);
		if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) liftSelection(io.KeyShift ? 100 : 20);
		if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) liftSelection(io.KeyShift ? -100 : -20);
		if (ImGui::IsKeyPressed(ImGuiKey_T, false)) cam_.topDown = !cam_.topDown;
		if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSelection();
		const Tool keys[] = { Tool::Move, Tool::Place, Tool::Floor, Tool::Height, Tool::Plateau, Tool::Paint, Tool::Water, Tool::Walls, Tool::Scatter };
		for (int i = 0; i < 9; i++) {
			if (!ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_1 + i), false)) continue;
			const bool terrain = keys[i] != Tool::Move && keys[i] != Tool::Place && keys[i] != Tool::Walls && keys[i] != Tool::Scatter;
			if (courseMode() && (keys[i] == Tool::Walls || keys[i] == Tool::Scatter))
				notify(1, tr("En los escenarios del juego todavía no se pueden poner muros ni decoración."));
			else if (!terrain || ensureTerrain()) tool_ = keys[i];
		}
	}
}

// ── Vista 3D ────────────────────────────────────────────────────────────────

bool App::project(const V3& p, const ImVec2& origin, const ImVec2& size, ImVec2& out) const
{
	const M4 vp    = cam_.projection(viewAspect_) * cam_.view();
	const float cx = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
	const float cy = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
	const float cw = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
	if (cw <= 0) return false;
	out = ImVec2(origin.x + (cx / cw * 0.5f + 0.5f) * size.x, origin.y + (1 - (cy / cw * 0.5f + 0.5f)) * size.y);
	return true;
}

void App::drawViewport(const Rect& r)
{
	ImGui::SetNextWindowPos(ImVec2(r.x, r.y));
	ImGui::SetNextWindowSize(ImVec2(r.w, r.h));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::Begin("##view", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	ImGui::PopStyleVar();
	viewRect_ = r;

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 avail  = ImGui::GetContentRegionAvail();
	const int vw = std::max(1, int(avail.x)), vh = std::max(1, int(avail.y));
	viewAspect_ = float(vw) / float(vh);
	target_.resize(vw, vh);

	ImGuiIO& io        = ImGui::GetIO();
	const float nx     = (io.MousePos.x - origin.x) / avail.x * 2 - 1;
	const float ny     = 1 - (io.MousePos.y - origin.y) / avail.y * 2;
	const Ray mouseRay = cameraRay(cam_, viewAspect_, nx, ny);

	renderScene(vw, vh);
	ImGui::Image(ImTextureID(std::intptr_t(target_.color)), avail, ImVec2(0, 1), ImVec2(1, 0));
	viewportHovered_ = ImGui::IsItemHovered();

	// Soltar decoración arrastrada desde la izquierda.
	if (ImGui::BeginDragDropTarget()) {
		V3 hit;
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("NM_ASSET")) {
			if (terrainRay(mouseRay, hit)) {
				place_ = { ItemType::Prop, static_cast<const char*>(p->Data), "" };
				placeAt(hit);
			}
		}
		ImGui::EndDragDropTarget();
	}

	drawLabels(origin, avail);
	viewportMouse(mouseRay, origin, avail);

	// Ayuda de la herramienta, arriba.
	ImDrawList* dl   = ImGui::GetWindowDrawList();
	const char* help = toolHelp();
	const ImVec2 ts  = ImGui::CalcTextSize(help);
	dl->AddRectFilled(ImVec2(origin.x + avail.x * 0.5f - ts.x * 0.5f - 12, origin.y + 8), ImVec2(origin.x + avail.x * 0.5f + ts.x * 0.5f + 12, origin.y + 16 + ts.y),
	                  IM_COL32(15, 18, 24, 200), 6);
	dl->AddText(ImVec2(origin.x + avail.x * 0.5f - ts.x * 0.5f, origin.y + 12), IM_COL32(240, 240, 245, 255), help);

	// Botones de cámara, abajo a la derecha.
	ImGui::SetCursorScreenPos(ImVec2(origin.x + avail.x - 410, origin.y + avail.y - 44));
	if (ImGui::Button(cam_.topDown ? tr("Vista en 3D (T)") : tr("Vista desde arriba (T)"), ImVec2(190, 34))) cam_.topDown = !cam_.topDown;
	ImGui::SameLine();
	if (ImGui::Button(tr("Ver todo el mapa"), ImVec2(200, 34))) {
		const MapDoc& d = ed_.doc;
		cam_.target     = V3(float((d.minX + d.maxX) * 0.5), 0, float((d.minZ + d.maxZ) * 0.5));
		cam_.distance   = float(std::max(d.maxX - d.minX, d.maxZ - d.minZ));
		cam_.topZoom    = float(std::max((d.maxX - d.minX) / viewAspect_, d.maxZ - d.minZ)) * 0.55f;
	}
	dl->AddText(ImVec2(origin.x + 10, origin.y + avail.y - 24), IM_COL32(230, 232, 240, 170), tr("Botón derecho: girar la vista  ·  Rueda: acercar  ·  Botón central: desplazar"));

	// Avisos.
	float ty = origin.y + avail.y - 60;
	for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
		if (it->until < now()) continue;
		const ImVec2 sz = ImGui::CalcTextSize(it->text.c_str(), nullptr, false, avail.x * 0.6f);
		const float x0  = origin.x + avail.x * 0.5f - sz.x * 0.5f - 14;
		const ImU32 bg  = it->level == 2 ? IM_COL32(150, 45, 40, 235) : it->level == 1 ? IM_COL32(150, 110, 30, 235) : IM_COL32(35, 110, 55, 235);
		dl->AddRectFilled(ImVec2(x0, ty - sz.y - 14), ImVec2(x0 + sz.x + 28, ty), bg, 8);
		dl->AddText(nullptr, 0, ImVec2(x0 + 14, ty - sz.y - 7), IM_COL32(255, 255, 255, 255), it->text.c_str(), nullptr, avail.x * 0.6f);
		ty -= sz.y + 22;
	}
	toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [](const Toast& t) { return t.until < now(); }), toasts_.end());
	ImGui::End();
}

void App::viewportMouse(const Ray& ray, const ImVec2& origin, const ImVec2& size)
{
	ImGuiIO& io = ImGui::GetIO();
	const bool overButtons = io.MousePos.y > origin.y + size.y - 48 && io.MousePos.x > origin.x + size.x - 420;
	const bool hovered     = viewportHovered_ && !overButtons && tutorialStep_ < 0;
	const bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left), leftClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
	const bool brushTool = tool_ == Tool::Floor || tool_ == Tool::Height || tool_ == Tool::Plateau || tool_ == Tool::Paint || tool_ == Tool::Water
	                    || tool_ == Tool::Scatter;

	// Pinceles de terreno.
	brushValid_ = brushTool && hovered && terrainRay(ray, brushPos_);
	if (brushValid_ && leftDown) {
		// Mayús hace lo contrario mientras se pinta.
		const bool erase = floorErase_, raise = heightOp_ == HeightOp::Raise, wErase = waterErase_, up = plateauUp_, sErase = scatterErase_;
		if (io.KeyShift) {
			if (tool_ == Tool::Scatter) scatterErase_ = !scatterErase_;
			if (tool_ == Tool::Floor) floorErase_ = !floorErase_;
			if (tool_ == Tool::Water) waterErase_ = !waterErase_;
			if (tool_ == Tool::Plateau) plateauUp_ = !plateauUp_;
		}
		if (!brushWasDown_) {
			// Allanar a la altura donde se empieza; elevar o hundir desde ella.
			flattenTarget_ = docHeight(brushPos_.x, brushPos_.z);
			if (tool_ == Tool::Plateau) flattenTarget_ += plateauUp_ ? plateauAmount_ : -plateauAmount_;
		}
		if (io.KeyShift && tool_ == Tool::Height && (heightOp_ == HeightOp::Raise || heightOp_ == HeightOp::Lower))
			heightOp_ = raise ? HeightOp::Lower : HeightOp::Raise;
		applyBrush(brushPos_, io.DeltaTime);
		floorErase_   = erase;
		waterErase_   = wErase;
		scatterErase_ = sErase;
		plateauUp_  = up;
		if (heightOp_ == HeightOp::Raise || heightOp_ == HeightOp::Lower) heightOp_ = raise ? HeightOp::Raise : HeightOp::Lower;
	}
	brushWasDown_ = brushValid_ && leftDown;
	if (hovered && brushTool && io.KeyCtrl && io.MouseWheel != 0) {
		brushRadius_  = std::max(25.0f, std::min(1000.0f, brushRadius_ * std::pow(1.15f, io.MouseWheel)));
		io.MouseWheel = 0; // Ctrl + rueda: tamaño del pincel, no zoom
	}

	// Poner cosas y agua: vista previa bajo el ratón y clic para poner.
	placeValid_ = tool_ == Tool::Place && hovered && terrainRay(ray, placePos_);
	if (placeValid_ && leftClick) placeAt(placePos_);

	// Muros: arrastrar de un extremo al otro.
	if (tool_ == Tool::Walls) {
		V3 hit;
		const bool onGround = hovered && terrainRay(ray, hit);
		if (onGround && leftClick) {
			drawingWall_ = true;
			wallStart_   = V3(float(snap(hit.x)), hit.y, float(snap(hit.z)));
		}
		if (drawingWall_) {
			if (onGround) placePos_ = V3(float(snap(hit.x)), hit.y, float(snap(hit.z)));
			if (!leftDown) {
				drawingWall_ = false;
				if ((placePos_ - wallStart_).length() >= 30) {
					nectar::DocWall w;
					w.ax        = wallStart_.x;
					w.az        = wallStart_.z;
					w.bx        = placePos_.x;
					w.bz        = placePos_.z;
					w.height    = wallHeight_;
					w.thickness = wallThickness_;
					ed_.doc.walls.push_back(w);
					ed_.selection = { { ItemType::Wall, int(ed_.doc.walls.size()) - 1, 0 } };
					ed_.touch();
				}
			}
		}
	} else {
		drawingWall_ = false;
	}

	// Mover: clic para elegir y arrastrar por el suelo.
	if (tool_ == Tool::Move) {
		if (hovered && leftClick) {
			dragHandle_ = -1;
			// ¿Un extremo del muro elegido?
			if (ed_.selection.size() == 1 && ed_.selection[0].type == ItemType::Wall && !ed_.doc.walls[std::size_t(ed_.selection[0].index)].isArc) {
				const nectar::DocWall& w = ed_.doc.walls[std::size_t(ed_.selection[0].index)];
				for (int e = 0; e < 2; e++) {
					const float ex = float(e ? w.bx : w.ax), ez = float(e ? w.bz : w.az);
					ImVec2 s;
					if (project(V3(ex, docHeight(ex, ez) + float(w.height), ez), origin, size, s) && std::hypot(s.x - io.MousePos.x, s.y - io.MousePos.y) < 14)
						dragHandle_ = e;
				}
			}
			ItemRef hit;
			float t = 0;
			if (dragHandle_ < 0) {
				if (pick(ray, hit, t)) {
					if (io.KeyCtrl) ed_.select(hit, true);
					else if (!ed_.selected(hit)) ed_.select(hit, false);
				} else if (!io.KeyCtrl) {
					ed_.clearSelection();
				}
			}
			dragArmed_ = !ed_.selection.empty() && terrainRay(ray, dragGrab_);
			pressX_    = io.MousePos.x;
			pressY_    = io.MousePos.y;
			dragStart_.clear();
			for (const ItemRef& r : ed_.selection) dragStart_.push_back({ r, getXform(ed_.doc, r) });
		}
		if (dragArmed_ && leftDown && !dragging_ && std::hypot(io.MousePos.x - pressX_, io.MousePos.y - pressY_) > 4) dragging_ = true;
		if (dragging_) {
			V3 at;
			if (terrainRay(ray, at)) {
				MapDoc& d = ed_.doc;
				if (dragHandle_ >= 0 && !ed_.selection.empty()) {
					nectar::DocWall& w = d.walls[std::size_t(ed_.selection[0].index)];
					(dragHandle_ ? w.bx : w.ax) = snap(at.x);
					(dragHandle_ ? w.bz : w.az) = snap(at.z);
				} else {
					const double dx = at.x - dragGrab_.x, dz = at.z - dragGrab_.z;
					for (const auto& st : dragStart_) {
						if (!itemExists(d, st.first)) continue;
						Xform x = st.second;
						x.x     = snap(st.second.x + dx);
						x.z     = snap(st.second.z + dz);
						// La decoración sigue el suelo sin perder su altura sobre él.
						if (st.first.type == ItemType::Prop || st.first.type == ItemType::Gen)
							x.y = st.second.y - docHeight(float(st.second.x), float(st.second.z)) + docHeight(float(x.x), float(x.z));
						setXform(d, st.first, x);
						ItemRef other;
						if (mirrorEdits_ && d.symmetric && counterpart(st.first, other) && !ed_.selected(other)) mirrorToCounterpart(d, st.first);
					}
				}
				ed_.touch();
			}
		}
		if (!leftDown) {
			dragging_   = false;
			dragArmed_  = false;
			dragHandle_ = -1;
		}
	}

	// Cámara.
	if (hovered && io.MouseWheel != 0) {
		if (cam_.topDown) cam_.topZoom = std::max(100.0f, cam_.topZoom * std::pow(0.88f, io.MouseWheel));
		else cam_.distance = std::max(80.0f, cam_.distance * std::pow(0.88f, io.MouseWheel));
	}
	if (viewportHovered_) {
		const float scale = cam_.topDown ? cam_.topZoom * 2 / size.y : cam_.distance * 0.0016f;
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) && !cam_.topDown) {
			cam_.yaw -= io.MouseDelta.x * 0.006f;
			cam_.pitch = std::max(0.08f, std::min(1.55f, cam_.pitch + io.MouseDelta.y * 0.006f));
		}
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || (cam_.topDown && ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
			const V3 right   = cam_.topDown ? V3(1, 0, 0) : V3(std::cos(cam_.yaw), 0, -std::sin(cam_.yaw));
			const V3 forward = cam_.topDown ? V3(0, 0, -1) : V3(-std::sin(cam_.yaw), 0, -std::cos(cam_.yaw));
			cam_.target      = cam_.target - right * (io.MouseDelta.x * scale) + forward * (io.MouseDelta.y * scale);
		}
		if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
			const float speed = (io.KeyShift ? 3.0f : 1.0f) * std::max(400.0f, cam_.distance) * io.DeltaTime;
			const V3 right    = V3(std::cos(cam_.yaw), 0, -std::sin(cam_.yaw));
			const V3 forward  = V3(-std::sin(cam_.yaw), 0, -std::cos(cam_.yaw));
			if (ImGui::IsKeyDown(ImGuiKey_W)) cam_.target = cam_.target + forward * speed;
			if (ImGui::IsKeyDown(ImGuiKey_S)) cam_.target = cam_.target - forward * speed;
			if (ImGui::IsKeyDown(ImGuiKey_D)) cam_.target = cam_.target + right * speed;
			if (ImGui::IsKeyDown(ImGuiKey_A)) cam_.target = cam_.target - right * speed;
		}
	}
}

void App::renderScene(int w, int h)
{
	const M4 view = cam_.view(), proj = cam_.projection(float(w) / float(h));
	renderer_.begin(target_, view, proj, Rgba { 0.36f, 0.47f, 0.58f, 1 });
	const MapDoc& d = ed_.doc;
	if (baseGpu_) renderer_.drawModel(*baseGpu_, M4());

	for (const ItemRef& r : allItems(d)) {
		drawItem(r, false);
		if (showTwins_ && d.symmetric && itemCaps(r.type).twinned) drawItem(r, true);
	}
	drawWater();
	if (tool_ == Tool::Height) drawSlopes();
	// Hacia dónde va cada bloque que se empuja (y el del otro lado).
	{
		std::vector<V3> lines;
		for (const nectar::DocObject& o : d.objects) {
			if (o.kind != "push_rock") continue;
			for (int twin = 0; twin < (d.symmetric ? 2 : 1); twin++) {
				const float sgn = twin ? -1.0f : 1.0f, face = float((o.face + (twin ? 180.0 : 0.0)) * kDeg);
				const V3 a(float(o.x) * sgn, 0, float(o.z) * sgn);
				const V3 b(a.x + std::sin(face) * float(o.push), 0, a.z + std::cos(face) * float(o.push));
				lines.push_back(V3(a.x, docHeight(a.x, a.z) + 20, a.z));
				lines.push_back(V3(b.x, docHeight(b.x, b.z) + 20, b.z));
			}
		}
		if (!lines.empty()) renderer_.drawLines(lines, Rgba { 1, 0.85f, 0.2f, 1 });
	}
	if (tool_ == Tool::Move) drawSelection();

	// Pincel.
	if (brushValid_) {
		std::vector<V3> ring;
		for (int k = 0; k < 64; k++) {
			for (int e = 0; e < 2; e++) {
				const float a = (k + e) * 6.2831853f / 64;
				const float x = brushPos_.x + std::cos(a) * brushRadius_, z = brushPos_.z + std::sin(a) * brushRadius_;
				ring.push_back(V3(x, docHeight(x, z) + 4, z));
			}
		}
		renderer_.drawLines(ring, Rgba { 1, 0.85f, 0.2f, 1 });
		if (d.symmetric) {
			for (V3& v : ring) {
				v.x = -v.x;
				v.z = -v.z;
				v.y = docHeight(v.x, v.z) + 4;
			}
			renderer_.drawLines(ring, Rgba { 1, 0.85f, 0.2f, 0.45f });
		}
	}
	// Lo que se va a poner, bajo el ratón.
	if (placeValid_) {
		const std::string asset      = placeAsset(place_);
		std::shared_ptr<GpuModel> gm = asset.empty() ? nullptr : model(asset);
		const M4 m                   = M4::translate(V3(float(snap(placePos_.x)), placePos_.y, float(snap(placePos_.z))));
		if (gm) renderer_.drawModel(*gm, m, Rgba { 1, 1, 1, 0.55f });
		else renderer_.drawCylinder(m * M4::scale(30, 60, 30), Rgba { 1, 0.85f, 0.2f, 0.55f });
	}
	// El muro que se está dibujando.
	if (drawingWall_ && (placePos_ - wallStart_).length() > 1) {
		const Segment s { wallStart_.x, wallStart_.z, placePos_.x, placePos_.z };
		const float len = (placePos_ - wallStart_).length();
		renderer_.drawBox(segmentMatrix(s, (wallStart_.y + placePos_.y) * 0.5f) * M4::translate(V3(0, wallHeight_ * 0.5f, 0)) * M4::scale(len, wallHeight_, wallThickness_),
		                  Rgba { 0.85f, 0.75f, 0.55f, 0.7f });
	}
	renderer_.end();
}

void App::drawWater()
{
	// El agua del juego son capas transparentes con efectos que el editor no
	// dibuja (se ve solo el fondo): aquí, una capa azul con su misma forma.
	if (!waterTris_.empty()) renderer_.drawTriangles(waterTris_, Rgba { 0.25f, 0.55f, 0.95f, 0.5f }, false);
}

void App::drawSlopes()
{
	// Celdas donde se resbala: más de 30° (tan 30° = 0,577).
	const MapDoc& d             = ed_.doc;
	const nectar::DocTerrain& g = d.terrain;
	if (!g.grid) return;
	const float cell = float(d.cell);
	std::vector<V3> tris;
	for (int iz = 0; iz < g.nz; iz++) {
		for (int ix = 0; ix < g.nx; ix++) {
			const std::uint8_t f[4] = { g.floor[std::size_t(g.vertexIndex(ix, iz))], g.floor[std::size_t(g.vertexIndex(ix + 1, iz))],
				                        g.floor[std::size_t(g.vertexIndex(ix, iz + 1))], g.floor[std::size_t(g.vertexIndex(ix + 1, iz + 1))] };
			if (std::max({ f[0], f[1], f[2], f[3] }) < 128) continue; // sin suelo
			const float h00 = g.heights[std::size_t(g.vertexIndex(ix, iz))], h10 = g.heights[std::size_t(g.vertexIndex(ix + 1, iz))];
			const float h01 = g.heights[std::size_t(g.vertexIndex(ix, iz + 1))], h11 = g.heights[std::size_t(g.vertexIndex(ix + 1, iz + 1))];
			const float steep = std::max({ std::fabs(h10 - h00), std::fabs(h01 - h00), std::fabs(h11 - h10), std::fabs(h11 - h01) }) / cell;
			if (steep <= 0.577f) continue;
			const float x0 = float(d.minX) + ix * cell, z0 = float(d.minZ) + iz * cell;
			const V3 a(x0, h00 + 3, z0), b(x0 + cell, h10 + 3, z0), c(x0, h01 + 3, z0 + cell), e(x0 + cell, h11 + 3, z0 + cell);
			tris.insert(tris.end(), { a, b, c, b, e, c });
		}
	}
	renderer_.drawTriangles(tris, Rgba { 1, 0.2f, 0.15f, 0.45f }, false);
}

void App::drawItem(const ItemRef& r, bool ghost)
{
	// Muros y charcas ya van en el mapa compilado.
	if (r.type == ItemType::Wall || r.type == ItemType::Pond) return;
	const M4 m              = itemMatrix(r, ghost);
	Rgba tint               = markerTint(r);
	tint.a                  = ghost ? 0.4f : 1.0f;
	const std::string asset = markerAsset(r);
	std::shared_ptr<GpuModel> gm = asset.empty() ? nullptr : model(asset);
	if (gm) renderer_.drawModel(*gm, m, tint);
	else if (r.type == ItemType::Rocket) {
		Rgba c = playerColour(r.index);
		c.a    = tint.a;
		renderer_.drawCylinder(m * M4::scale(45, 150, 45), c);
		renderer_.drawCylinder(m * M4::translate(V3(0, 150, 0)) * M4::scale(25, 40, 25), Rgba { 0.9f, 0.9f, 0.95f, tint.a });
	} else renderer_.drawCylinder(m * M4::scale(30, 60, 30), Rgba { 1, 0.3f, 0.8f, tint.a });

	// Dueño: un disco de su color bajo lo que es de un jugador.
	if (itemCaps(r.type).perPlayer) {
		Rgba c          = playerColour(r.index);
		c.a             = 0.85f;
		const float rad = r.type == ItemType::Rocket ? 160.0f : r.type == ItemType::Gate ? 140.0f : 50.0f;
		renderer_.drawCylinder(M4::translate(V3(m.at(0, 3), m.at(1, 3) + 0.5f, m.at(2, 3))) * M4::scale(rad, 2, rad), c);
	}
}

void App::drawSelection()
{
	std::vector<V3> lines;
	const MapDoc& d = ed_.doc;
	for (const ItemRef& r : ed_.selection) {
		if (!itemExists(d, r)) continue;
		if (r.type == ItemType::Wall) {
			const nectar::DocWall& w = d.walls[std::size_t(r.index)];
			for (const Segment& s : wallSegments(w)) {
				const float len = float(std::hypot(s.bx - s.ax, s.bz - s.az));
				const float y   = docHeight(float((s.ax + s.bx) * 0.5), float((s.az + s.bz) * 0.5));
				boxLines(lines, segmentMatrix(s, y), V3(-len / 2, 0, float(-w.thickness / 2)), V3(len / 2, float(w.height), float(w.thickness / 2)));
			}
			// Los extremos, para arrastrarlos.
			if (!w.isArc && ed_.selection.size() == 1) {
				for (int e = 0; e < 2; e++) {
					const float ex = float(e ? w.bx : w.ax), ez = float(e ? w.bz : w.az);
					renderer_.drawCylinder(M4::translate(V3(ex, docHeight(ex, ez), ez)) * M4::scale(14, float(w.height) + 20, 14), Rgba { 1, 0.85f, 0.1f, 1 });
				}
			}
			continue;
		}
		if (r.type == ItemType::Pond) {
			const nectar::DocPond& p = d.ponds[std::size_t(r.index)];
			for (int k = 0; k < 48; k++) {
				const float a0 = k * 6.2831853f / 48, a1 = (k + 1) * 6.2831853f / 48;
				const float y  = docHeight(float(p.cx), float(p.cz)) + 3;
				lines.push_back(V3(float(p.cx + p.rx * std::cos(a0)), y, float(p.cz + p.rz * std::sin(a0))));
				lines.push_back(V3(float(p.cx + p.rx * std::cos(a1)), y, float(p.cz + p.rz * std::sin(a1))));
			}
			continue;
		}
		const std::string asset      = markerAsset(r);
		std::shared_ptr<GpuModel> gm = asset.empty() ? nullptr : model(asset);
		const M4 m                   = itemMatrix(r, false);
		V3 lo, hi;
		fallbackBox(r.type, lo, hi);
		if (gm) boxLines(lines, m, gm->boundsMin, gm->boundsMax);
		else boxLines(lines, m, lo, hi);
	}
	renderer_.drawLines(lines, Rgba { 1, 0.85f, 0.1f, 1 });
}

void App::drawLabels(const ImVec2& origin, const ImVec2& size)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
	for (const ItemRef& r : allItems(ed_.doc)) {
		if (!itemCaps(r.type).perPlayer && r.type != ItemType::BigPiece) continue;
		const Xform x = getXform(ed_.doc, r);
		ImVec2 s;
		if (!project(V3(float(x.x), docHeight(float(x.x), float(x.z)) + 90, float(x.z)), origin, size, s)) continue;
		// Cortas: el color ya dice de qué jugador es.
		static const char* const onions[] = { "Cebolla azul", "Cebolla roja", "Cebolla amarilla" };
		const std::string text = r.type == ItemType::Base ? tr("Salida") : r.type == ItemType::Rocket ? tr("Cohete") : r.type == ItemType::Gate ? tr("Puerta de roca")
		                       : r.type == ItemType::BombPile ? tr("Rocas bomba") : r.type == ItemType::Onion ? tr(onions[r.sub]) : tr("Pieza grande");
		const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
		const Rgba c           = r.type == ItemType::BigPiece ? Rgba { 0.9f, 0.75f, 0.2f, 1 } : playerColour(r.index);
		dl->AddRectFilled(ImVec2(s.x - ts.x / 2 - 5, s.y - ts.y - 6), ImVec2(s.x + ts.x / 2 + 5, s.y), IM_COL32(int(c.r * 200), int(c.g * 200), int(c.b * 200), 200), 4);
		dl->AddText(ImVec2(s.x - ts.x / 2, s.y - ts.y - 3), IM_COL32(255, 255, 255, 255), text.c_str());
	}
	dl->PopClipRect();
}

bool App::groundHit(const Ray& ray, V3& hit) const { return terrainRay(ray, hit); }

bool App::pick(const Ray& ray, ItemRef& out, float& bestT)
{
	bestT           = -1;
	bool found      = false;
	const MapDoc& d = ed_.doc;
	auto consider   = [&](const ItemRef& r, float t) {
        if (t >= 0 && (!found || t < bestT)) {
            bestT = t;
            out   = r;
            found = true;
        }
	};
	for (const ItemRef& r : allItems(d)) {
		if (r.type == ItemType::Wall) {
			const nectar::DocWall& w = d.walls[std::size_t(r.index)];
			for (const Segment& s : wallSegments(w)) {
				const float len = float(std::hypot(s.bx - s.ax, s.bz - s.az));
				const float y   = docHeight(float((s.ax + s.bx) * 0.5), float((s.az + s.bz) * 0.5));
				consider(r, rayBox(ray, segmentMatrix(s, y), V3(-len / 2, -20, float(-w.thickness / 2)), V3(len / 2, float(w.height), float(w.thickness / 2))));
			}
			continue;
		}
		if (r.type == ItemType::Pond) {
			const nectar::DocPond& p = d.ponds[std::size_t(r.index)];
			V3 h;
			if (rayPlaneY(ray, docHeight(float(p.cx), float(p.cz)), h)) {
				const double ex = (h.x - p.cx) / p.rx, ez = (h.z - p.cz) / p.rz;
				// Las charcas, solo por su orilla: el centro deja elegir lo que hay dentro.
				const double e = ex * ex + ez * ez;
				if (e < 1.0 && e > 0.55) consider(r, (h - ray.origin).length() + 1.0f);
			}
			continue;
		}
		const std::string asset      = markerAsset(r);
		std::shared_ptr<GpuModel> gm = asset.empty() ? nullptr : model(asset);
		V3 lo, hi;
		fallbackBox(r.type, lo, hi);
		if (gm) {
			lo = gm->boundsMin;
			hi = gm->boundsMax;
		}
		// Que lo pequeño también se pueda coger.
		for (float* v : { &lo.x, &lo.z }) *v = std::min(*v, -25.0f);
		for (float* v : { &hi.x, &hi.z }) *v = std::max(*v, 25.0f);
		hi.y = std::max(hi.y, 40.0f);
		consider(r, rayBox(ray, itemMatrix(r, false), lo, hi));
	}
	return found;
}

// ── Terreno ─────────────────────────────────────────────────────────────────

bool App::ensureTerrain()
{
	if (courseMode()) {
		notify(1, tr("En los escenarios del juego el suelo no se puede cambiar: mueve, quita o añade cosas."));
		return false;
	}
	if (ed_.doc.terrain.grid) return true;
	nectar::makeTerrainGrid(ed_.doc);
	ed_.touch();
	return true;
}

float App::docHeight(float x, float z) const
{
	const MapDoc& d             = ed_.doc;
	const nectar::DocTerrain& g = d.terrain;
	if (!d.course.empty()) {
		float y = 0.0f;
		return courseField_.at(x, z, y) ? y : 0.0f;
	}
	if (!g.grid || g.nx < 1 || g.nz < 1) return 0.0f;
	const float cell = float(d.cell);
	const float fx   = std::max(0.0f, std::min(float(g.nx), (x - float(d.minX)) / cell));
	const float fz   = std::max(0.0f, std::min(float(g.nz), (z - float(d.minZ)) / cell));
	const int ix = std::min(g.nx - 1, int(fx)), iz = std::min(g.nz - 1, int(fz));
	const float tx = fx - ix, tz = fz - iz;
	auto h         = [&](int a, int b) { return float(g.heights[std::size_t(g.vertexIndex(a, b))]); };
	if (tx + tz <= 1.0f) return h(ix, iz) + (h(ix + 1, iz) - h(ix, iz)) * tx + (h(ix, iz + 1) - h(ix, iz)) * tz;
	return h(ix + 1, iz + 1) + (h(ix, iz + 1) - h(ix + 1, iz + 1)) * (1.0f - tx) + (h(ix + 1, iz) - h(ix + 1, iz + 1)) * (1.0f - tz);
}

bool App::docSolid(float x, float z) const
{
	const MapDoc& d             = ed_.doc;
	const nectar::DocTerrain& g = d.terrain;
	if (x <= d.minX || x >= d.maxX || z <= d.minZ || z >= d.maxZ) return false;
	if (!d.course.empty()) {
		float y;
		return courseField_.at(x, z, y);
	}
	if (!g.grid) return true;
	// El suelo, interpolado como los triángulos del terreno.
	const float cell = float(d.cell);
	const float fx = (x - float(d.minX)) / cell, fz = (z - float(d.minZ)) / cell;
	const int ix = std::min(g.nx - 1, int(fx)), iz = std::min(g.nz - 1, int(fz));
	const float tx = fx - ix, tz = fz - iz;
	auto f         = [&](int a, int b) {
        const bool ring = a == 0 || b == 0 || a == g.nx || b == g.nz; // el borde del mapa nunca tiene suelo
        return ring ? 0.0f : float(g.floor[std::size_t(g.vertexIndex(a, b))]);
	};
	float v;
	if (tx + tz <= 1.0f) v = f(ix, iz) + (f(ix + 1, iz) - f(ix, iz)) * tx + (f(ix, iz + 1) - f(ix, iz)) * tz;
	else v = f(ix + 1, iz + 1) + (f(ix, iz + 1) - f(ix + 1, iz + 1)) * (1.0f - tx) + (f(ix + 1, iz) - f(ix + 1, iz + 1)) * (1.0f - tz);
	return v >= 128.0f;
}

bool App::terrainRay(const Ray& ray, V3& hit) const
{
	// Corte con el plano a la altura del terreno, afinando unas cuantas veces.
	float h = 0;
	for (int i = 0; i < 8; i++) {
		if (!rayPlaneY(ray, h, hit)) return false;
		const float nh = docHeight(hit.x, hit.z);
		if (std::fabs(nh - h) < 0.5f) break;
		h = h + (nh - h) * 0.7f;
	}
	const MapDoc& d = ed_.doc;
	return hit.x >= d.minX && hit.x <= d.maxX && hit.z >= d.minZ && hit.z <= d.maxZ;
}

void App::applyBrush(const V3& at, float dt)
{
	if (tool_ == Tool::Scatter) {
		scatter(at);
		return;
	}
	MapDoc& d             = ed_.doc;
	nectar::DocTerrain& g = d.terrain;
	if (!g.grid) return;
	const float cell = float(d.cell), r = brushRadius_;
	paintMaterial_   = std::max(0, std::min(paintMaterial_, int(g.palette.size()) - 1));
	std::vector<V3> centres { at };
	if (d.symmetric) centres.push_back(V3(-at.x, at.y, -at.z)); // el lado del otro jugador igual
	bool changed = false;
	for (const V3& c : centres) {
		const int x0 = std::max(0, int(std::floor((c.x - r - d.minX) / cell))), x1 = std::min(g.nx, int(std::ceil((c.x + r - d.minX) / cell)));
		const int z0 = std::max(0, int(std::floor((c.z - r - d.minZ) / cell))), z1 = std::min(g.nz, int(std::ceil((c.z + r - d.minZ) / cell)));
		if (tool_ == Tool::Plateau) {
			// Borde duro: todo lo de dentro, a la misma altura.
			const std::int16_t v = std::int16_t(std::max(-3000.0f, std::min(3000.0f, std::round(flattenTarget_))));
			for (int iz = z0; iz <= z1; iz++) {
				for (int ix = x0; ix <= x1; ix++) {
					const float vx = float(d.minX) + ix * cell, vz = float(d.minZ) + iz * cell;
					if (std::hypot(vx - c.x, vz - c.z) > r) continue;
					std::int16_t& h = g.heights[std::size_t(g.vertexIndex(ix, iz))];
					if (h != v) {
						h       = v;
						changed = true;
					}
				}
			}
		} else if (tool_ == Tool::Water || tool_ == Tool::Floor || tool_ == Tool::Paint) {
			// Por punto: núcleo firme y borde suave, para que las formas salgan redondas.
			for (int iz = z0; iz <= z1; iz++) {
				for (int ix = x0; ix <= x1; ix++) {
					const float vx = float(d.minX) + ix * cell, vz = float(d.minZ) + iz * cell;
					const float a  = std::max(0.0f, std::min(1.0f, (r - std::hypot(vx - c.x, vz - c.z)) / (r * 0.4f)));
					if (a <= 0.0f) continue;
					const std::size_t v = std::size_t(g.vertexIndex(ix, iz));
					const std::uint8_t full = std::uint8_t(std::lround(255.0f * a)), keep = std::uint8_t(std::lround(255.0f * (1.0f - a)));
					auto raise = [&](std::uint8_t& x) {
						if (x < full) {
							x       = full;
							changed = true;
						}
					};
					auto lower = [&](std::uint8_t& x) {
						if (x > keep) {
							x       = keep;
							changed = true;
						}
					};
					if (tool_ == Tool::Water) {
						if (waterErase_) lower(g.water[v]);
						else raise(g.water[v]);
						continue;
					}
					if (tool_ == Tool::Floor) {
						if (floorErase_) {
							lower(g.floor[v]);
							continue;
						}
						raise(g.floor[v]);
					}
					// Pintar (y el suelo nuevo, del material elegido).
					for (std::size_t k = 0; k < g.layers.size(); k++) {
						if (int(k) == paintMaterial_) raise(g.layers[k][v]);
						else lower(g.layers[k][v]);
					}
				}
			}
		} else if (tool_ == Tool::Height) {
			const std::vector<std::int16_t> before = g.heights;
			for (int iz = z0; iz <= z1; iz++) {
				for (int ix = x0; ix <= x1; ix++) {
					const float vx = float(d.minX) + ix * cell, vz = float(d.minZ) + iz * cell;
					const float dd = std::hypot(vx - c.x, vz - c.z);
					if (dd > r) continue;
					const float t   = 1.0f - (dd / r) * (dd / r);
					const float w   = t * t; // suave hacia el borde del pincel
					std::int16_t& h = g.heights[std::size_t(g.vertexIndex(ix, iz))];
					float nh        = h;
					switch (heightOp_) {
					case HeightOp::Raise: nh += brushStrength_ * dt * w; break;
					case HeightOp::Lower: nh -= brushStrength_ * dt * w; break;
					case HeightOp::Flatten: nh += (flattenTarget_ - nh) * std::min(1.0f, w * dt * 6.0f); break;
					case HeightOp::Smooth: {
						float sum = 0;
						int n     = 0;
						const int nb[4][2] = { { ix - 1, iz }, { ix + 1, iz }, { ix, iz - 1 }, { ix, iz + 1 } };
						for (const auto& q : nb) {
							if (q[0] < 0 || q[1] < 0 || q[0] > g.nx || q[1] > g.nz) continue;
							sum += before[std::size_t(g.vertexIndex(q[0], q[1]))];
							n++;
						}
						if (n) nh += (sum / n - nh) * std::min(1.0f, w * dt * 8.0f);
						break;
					}
					}
					const std::int16_t v = std::int16_t(std::max(-3000.0f, std::min(3000.0f, std::round(nh))));
					if (v != h) {
						h       = v;
						changed = true;
					}
				}
			}
		}
	}
	if (changed) ed_.touch();
}

std::shared_ptr<nectar::Model> App::courseModel(const std::string& source)
{
	auto it = courseModels_.find(source);
	if (it != courseModels_.end()) return it->second;
	auto m = std::make_shared<nectar::Model>();
	std::string err;
	if (!nectar::loadMod(cfg_.dataDir() + "/" + source, *m, err)) m.reset();
	courseModels_[source] = m;
	return m;
}

unsigned App::swatch(const std::string& source, int index)
{
	const std::string key = source + "#" + std::to_string(index);
	auto it               = swatches_.find(key);
	if (it != swatches_.end()) return it->second;
	unsigned tex = 0;
	if (std::shared_ptr<nectar::Model> m = courseModel(source)) {
		if (index >= 0 && std::size_t(index) < m->materials.size()) {
			const int attr = m->materials[std::size_t(index)].texAttr;
			if (attr >= 0 && std::size_t(attr) < m->texAttrs.size()) {
				const int t = m->texAttrs[std::size_t(attr)].texture;
				if (t >= 0 && std::size_t(t) < m->textures.size()) tex = uploadTexture(nectar::shrinkToFit(m->textures[std::size_t(t)].decode(), 96, 96), false);
			}
		}
	}
	swatches_[key] = tex;
	return tex;
}

void App::drawMaterialBrowser()
{
	if (openMatBrowser_) {
		ImGui::OpenPopup(tr("Elige un suelo"));
		openMatBrowser_ = false;
	}
	ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal(tr("Elige un suelo"), nullptr)) return;
	hint(matBrowserTarget_ == -2 ? tr("Elige el aspecto de las paredes del borde: sale de los escenarios del juego.")
	                             : tr("Elige un suelo de los escenarios del juego. Los primeros de cada lista son los que más se usan como suelo."));
	ImGui::BeginChild("courses", ImVec2(240, -40), ImGuiChildFlags_Borders);
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1) {
			ImGui::Spacing();
			ImGui::TextColored(kDim, tr("Escenarios de prueba"));
		}
		for (const nectar::CatalogEntry& e : catalog_.entries) {
			if (e.kind != nectar::AssetKind::Model || e.category != "terrain" || !e.error.empty()) continue;
			const char* name = courseName(e.path);
			if ((name != nullptr) != (pass == 0)) continue;
			const std::string label = name ? name : fs::path(e.path).stem().string();
			if (ImGui::Selectable((label + "##" + e.path).c_str(), matBrowserCourse_ == e.path)) matBrowserCourse_ = e.path;
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("mats", ImVec2(0, -40), ImGuiChildFlags_Borders);
	if (!matBrowserCourse_.empty()) {
		std::shared_ptr<nectar::Model> m = courseModel(matBrowserCourse_);
		if (m && !courseStats_.count(matBrowserCourse_)) {
			// Cuántos triángulos usan cada material, y cuántos miran hacia arriba.
			std::map<int, std::array<int, 3>> st;
			for (const nectar::BakedTri& t : nectar::bakeModel(*m)) {
				std::array<int, 3>& e = st[t.material];
				e[0]                  = t.material;
				e[1]++;
				const V3 a(t.p[1].x - t.p[0].x, t.p[1].y - t.p[0].y, t.p[1].z - t.p[0].z), b(t.p[2].x - t.p[0].x, t.p[2].y - t.p[0].y, t.p[2].z - t.p[0].z);
				if (std::fabs(a.cross(b).normalized().y) > 0.8f) e[2]++;
			}
			std::vector<std::array<int, 3>> list;
			for (auto& kv : st) list.push_back(kv.second);
			std::sort(list.begin(), list.end(), [&](const std::array<int, 3>& x, const std::array<int, 3>& y) {
				return matBrowserTarget_ == -2 ? x[1] - x[2] > y[1] - y[2] : x[2] > y[2];
			});
			courseStats_[matBrowserCourse_] = list;
		}
		int col        = 0;
		const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / 104));
		for (const std::array<int, 3>& e : courseStats_[matBrowserCourse_]) {
			const unsigned t = swatch(matBrowserCourse_, e[0]);
			if (!t) continue;
			ImGui::PushID(e[0]);
			if (ImGui::ImageButton("##m", ImTextureID(std::intptr_t(t)), ImVec2(88, 88))) {
				nectar::DocMaterial nm;
				nm.source             = matBrowserCourse_;
				nm.index              = e[0];
				nectar::DocTerrain& g = ed_.doc.terrain;
				if (matBrowserTarget_ == -2) {
					nm.attribute = 1; // roca
					g.border     = nm;
				} else if (matBrowserTarget_ >= 0 && std::size_t(matBrowserTarget_) < g.palette.size()) {
					nm.attribute                              = g.palette[std::size_t(matBrowserTarget_)].attribute;
					g.palette[std::size_t(matBrowserTarget_)] = nm;
				} else {
					g.palette.push_back(nm);
					g.layers.emplace_back(std::size_t(g.vertexCount()), std::uint8_t(0));
					paintMaterial_ = int(g.palette.size()) - 1;
				}
				ed_.touch();
				ImGui::CloseCurrentPopup();
			}
			ImGui::PopID();
			if (++col < cols) ImGui::SameLine();
			else col = 0;
		}
	} else {
		hint(tr("Elige un escenario a la izquierda."));
	}
	ImGui::EndChild();
	if (ImGui::Button(tr("Cancelar"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

} // namespace nm
