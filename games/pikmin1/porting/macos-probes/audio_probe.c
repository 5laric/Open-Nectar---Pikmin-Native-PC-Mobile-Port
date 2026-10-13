// Does the macOS SDL2 audio device accept the game's 32 kHz S16 stereo spec?
#include <SDL.h>
#include <stdio.h>
int main(void) {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) { printf("SDL_Init(AUDIO) failed: %s\n", SDL_GetError()); return 1; }
    printf("output devices: %d\n", SDL_GetNumAudioDevices(0));
    SDL_AudioSpec desired = {0}, obtained = {0};
    desired.freq = 32000; desired.format = AUDIO_S16SYS; desired.channels = 2; desired.samples = 1024;
    desired.callback = NULL;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (!dev) { printf("open failed: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    printf("opened 32000 Hz S16 stereo: obtained freq=%d format=0x%x channels=%d samples=%d\n",
           obtained.freq, obtained.format, obtained.channels, obtained.samples);
    SDL_CloseAudioDevice(dev);
    SDL_Quit();
    return 0;
}
