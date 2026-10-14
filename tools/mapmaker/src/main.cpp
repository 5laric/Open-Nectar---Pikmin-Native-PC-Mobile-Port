// Nectar Map Maker: editor de mapas del modo VS de Open Nectar.
// docs: games/pikmin1/docs/PLAN_EDITOR_MAPAS.md

#include "app.h"
#include "gl.h"

#include "font_fredoka.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#include <SDL.h>

#include "nectar_assets/image.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "nectar_mapcore/i18n.h"

using nectar::tr;

int main(int argc, char** argv)
{
	SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_Window* window = SDL_CreateWindow("Nectar Map Maker", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 900,
	                                      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_MAXIMIZED);
	if (!window) {
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}
	SDL_GLContext gl = SDL_GL_CreateContext(window);
	if (!gl || !nmLoadGl()) {
		fprintf(stderr, tr("Hace falta OpenGL 3.3: %s\n"), SDL_GetError());
		return 1;
	}
	SDL_GL_MakeCurrent(window, gl);
	SDL_GL_SetSwapInterval(1);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io    = ImGui::GetIO();
	io.IniFilename = nullptr; // la disposición es fija
	ImGui::StyleColorsDark();
	ImGuiStyle& style       = ImGui::GetStyle();
	style.WindowRounding    = 4;
	style.FrameRounding     = 4;
	style.GrabRounding      = 4;
	style.WindowBorderSize  = 1;
	style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.13f, 1);
	ImFontConfig fc;
	fc.FontDataOwnedByAtlas = false; // los datos van dentro del ejecutable
	io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kFontFredoka), int(kFontFredokaSize), 17.0f, &fc, io.Fonts->GetGlyphRangesDefault());
	ImGui_ImplSDL2_InitForOpenGL(window, gl);
	ImGui_ImplOpenGL3_Init("#version 330");

	// Para comprobaciones sin pantalla (SDL_VIDEODRIVER=offscreen): guarda una
	// captura a los NECTAR_MAPMAKER_SCREENSHOT_AFTER segundos (8) y cierra.
	const char* shotPath   = std::getenv("NECTAR_MAPMAKER_SCREENSHOT");
	const char* shotAfter  = std::getenv("NECTAR_MAPMAKER_SCREENSHOT_AFTER");
	const Uint64 shotTicks = SDL_GetTicks64() + Uint64((shotAfter ? std::atof(shotAfter) : 8.0) * 1000.0);

	int exitCode = 0;
	{
		nm::App app;
		std::string error;
		if (!app.init(argc > 1 ? argv[1] : "", error)) {
			fprintf(stderr, tr("No se pudo iniciar el editor: %s\n"), error.c_str());
			exitCode = 1;
		}
		while (exitCode == 0 && !app.wantsQuit()) {
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				ImGui_ImplSDL2_ProcessEvent(&e);
				if (e.type == SDL_QUIT || (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE && e.window.windowID == SDL_GetWindowID(window)))
					app.requestQuit();
			}
			if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
				SDL_Delay(30);
				continue;
			}
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplSDL2_NewFrame();
			ImGui::NewFrame();
			app.frame();
			ImGui::Render();
			int w = 0, h = 0;
			SDL_GL_GetDrawableSize(window, &w, &h);
			glViewport(0, 0, w, h);
			glClearColor(0.08f, 0.09f, 0.1f, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			if (shotPath && SDL_GetTicks64() >= shotTicks) {
				nectar::Image img;
				img.resize(w, h);
				glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
				nectar::Image flipped = img;
				for (int y = 0; y < h; y++) std::copy(img.at(0, h - 1 - y), img.at(0, h - 1 - y) + w * 4, flipped.at(0, y));
				nectar::writePng(shotPath, flipped);
				break;
			}
			SDL_GL_SwapWindow(window);
		}
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	SDL_GL_DeleteContext(gl);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return exitCode;
}
