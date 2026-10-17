/**
 * @file touch_controls.h
 * @brief Gamepad virtual na tela (D-pad, A, B, Start, Select e botão de menu).
 *
 * Multitoque: cada dedo é rastreado pelo SDL_FingerID. O dedo que começa no
 * D-pad fica no D-pad até soltar; o que começa nos botões pode deslizar entre
 * A e B (e apertar os dois na região entre eles).
 */
#pragma once

#include <SDL.h>

struct ImDrawList;

namespace touch_controls {

/* Mesma ordem dos primeiros 8 GBInputAction do platform_sdl.cpp. */
enum Button {
    BTN_RIGHT = 0,
    BTN_LEFT = 1,
    BTN_UP = 2,
    BTN_DOWN = 3,
    BTN_A = 4,
    BTN_B = 5,
    BTN_SELECT = 6,
    BTN_START = 7,
    BTN_JOYPAD_COUNT = 8,
};

bool enabled_by_default();      /* true no Android */
void set_enabled(bool enabled);
bool enabled();

/*
 * Processa SDL_FINGERDOWN/MOTION/UP. Devolve true se o estado dos botões
 * pode ter mudado (o chamador recalcula o joypad). Com o menu aberto, solta
 * todos os dedos e devolve true uma vez.
 */
bool handle_event(const SDL_Event& event, SDL_Window* window, bool menu_open);

bool pressed(int button);

/* true uma única vez depois de tocar no botão de menu. */
bool take_menu_request();

/* Desenha a sobreposição. w/h em pixels da janela (= ImGui DisplaySize). */
void draw(ImDrawList* dl, float w, float h, bool controller_connected);

/* Solta tudo (ex.: ao abrir o menu ou trocar de app). */
void reset();

} // namespace touch_controls
