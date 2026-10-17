/**
 * @file app_icon.cpp
 * @brief Ícone da janela, embutido no executável (64x64 RGBA).
 */
#include "app_icon.h"

#if !defined(__ANDROID__)
#include "app_icon_data.h"
#endif

void app_icon_apply(SDL_Window* window) {
#if defined(__ANDROID__)
    (void)window;
#else
    if (!window) return;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
        (void*)APP_ICON_RGBA, APP_ICON_W, APP_ICON_H, 32, APP_ICON_W * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return;
    SDL_SetWindowIcon(window, surface);   /* o SDL copia os pixels */
    SDL_FreeSurface(surface);
#endif
}
