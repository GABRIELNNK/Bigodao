/**
 * @file app_icon.h
 * @brief Ícone da janela (Linux/Windows). No Android não faz nada:
 *        lá o ícone vem dos mipmaps do APK.
 */
#pragma once

#include <SDL.h>

void app_icon_apply(SDL_Window* window);
