// Probe which OpenGL context profiles SDL2 can create on this Mac.
// Mirrors the attribute sets pc_window.cpp uses, plus the 3.2 core and 4.1 core
// profiles macOS documents.
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>

static void probe(const char* label, int major, int minor, int profile, int forwardCompat) {
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, forwardCompat ? SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG : 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow("gl-probe", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        printf("%-30s window FAILED: %s\n", label, SDL_GetError());
        return;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        printf("%-30s context FAILED: %s\n", label, SDL_GetError());
        SDL_DestroyWindow(window);
        return;
    }
    SDL_GL_MakeCurrent(window, context);
    printf("%-30s OK  GL_VERSION=\"%s\"  GLSL=\"%s\"\n", label,
           (const char*)glGetString(GL_VERSION),
           (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
}

int main(void) {
    SDL_version linked;
    SDL_GetVersion(&linked);
    printf("SDL2 linked: %d.%d.%d  platform: %s\n", linked.major, linked.minor, linked.patch, SDL_GetPlatform());
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    probe("3.3 compatibility (pc_window)", 3, 3, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 0);
    probe("2.1 default (pc_window fallback)", 2, 1, 0, 0);
    probe("3.2 core, forward-compat", 3, 2, SDL_GL_CONTEXT_PROFILE_CORE, 1);
    probe("4.1 core, forward-compat", 4, 1, SDL_GL_CONTEXT_PROFILE_CORE, 1);
    SDL_Quit();
    return 0;
}
