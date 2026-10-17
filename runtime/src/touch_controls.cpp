/**
 * @file touch_controls.cpp
 * @brief Gamepad virtual na tela.
 */
#include "touch_controls.h"

#include "imgui.h"

#include <cmath>
#include <cstring>

namespace touch_controls {

namespace {

constexpr int kMaxFingers = 10;
constexpr Uint32 kShowAfterTouchMs = 8000;   /* com controle físico, some após 8 s sem toque */

enum Zone { ZONE_NONE = 0, ZONE_DPAD, ZONE_BUTTONS };

struct Finger {
    bool active = false;
    SDL_FingerID id = 0;
    Zone zone = ZONE_NONE;
    float x = 0.0f, y = 0.0f;   /* pixels da janela */
};

struct Layout {
    float u;                     /* unidade: menor lado da tela */
    ImVec2 dpad; float dpad_r;
    ImVec2 a, b; float ab_r;
    ImVec2 select, start; ImVec2 pill;   /* centro + tamanho (w,h) */
    ImVec2 menu; float menu_r;
};

#if defined(__ANDROID__)
bool g_enabled = true;
#else
bool g_enabled = false;
#endif
Finger g_fingers[kMaxFingers];
bool g_pressed[BTN_JOYPAD_COUNT] = {};
bool g_menu_request = false;
Uint32 g_last_touch_ms = 0;
float g_last_w = 0.0f, g_last_h = 0.0f;

Layout compute_layout(float w, float h) {
    Layout l;
    l.u = (w < h) ? w : h;
    const float pad = 0.05f * l.u;

    l.dpad_r = 0.20f * l.u;
    l.dpad = ImVec2(pad + l.dpad_r, h - pad - l.dpad_r);

    l.ab_r = 0.085f * l.u;
    /* Layout do Game Boy: A em cima à direita, B embaixo à esquerda */
    l.a = ImVec2(w - pad - l.ab_r, h - pad - l.dpad_r - 0.6f * l.ab_r);
    l.b = ImVec2(l.a.x - 2.4f * l.ab_r, l.a.y + 1.0f * l.ab_r);

    l.pill = ImVec2(0.17f * l.u, 0.065f * l.u);
    const float pill_y = h - pad * 0.6f - l.pill.y * 0.5f;
    l.select = ImVec2(w * 0.5f - l.pill.x * 0.65f, pill_y);
    l.start  = ImVec2(w * 0.5f + l.pill.x * 0.65f, pill_y);

    l.menu_r = 0.05f * l.u;
    l.menu = ImVec2(w - pad * 0.6f - l.menu_r, pad * 0.6f + l.menu_r);
    return l;
}

float dist(ImVec2 p, ImVec2 c) {
    const float dx = p.x - c.x, dy = p.y - c.y;
    return std::sqrt(dx * dx + dy * dy);
}

bool in_pill(ImVec2 p, ImVec2 c, ImVec2 size, float grow) {
    return std::fabs(p.x - c.x) <= size.x * 0.5f * grow &&
           std::fabs(p.y - c.y) <= size.y * 0.5f * grow;
}

void dpad_directions(const Layout& l, ImVec2 p, bool out[4]) {
    const float dx = p.x - l.dpad.x, dy = p.y - l.dpad.y;
    out[BTN_RIGHT] = out[BTN_LEFT] = out[BTN_UP] = out[BTN_DOWN] = false;
    if (std::sqrt(dx * dx + dy * dy) < l.dpad_r * 0.18f) return;   /* zona morta */
    /* 8 direções: setores de 45° (tan 67,5° ≈ 2,414) */
    const float ax = std::fabs(dx), ay = std::fabs(dy);
    if (ay < 2.414f * ax) { out[dx > 0 ? BTN_RIGHT : BTN_LEFT] = true; }
    if (ax < 2.414f * ay) { out[dy > 0 ? BTN_DOWN : BTN_UP] = true; }
}

void button_hits(const Layout& l, ImVec2 p, bool out[BTN_JOYPAD_COUNT]) {
    if (dist(p, l.a) <= l.ab_r * 1.35f) out[BTN_A] = true;
    if (dist(p, l.b) <= l.ab_r * 1.35f) out[BTN_B] = true;
    if (in_pill(p, l.select, l.pill, 1.4f)) out[BTN_SELECT] = true;
    if (in_pill(p, l.start, l.pill, 1.4f)) out[BTN_START] = true;
}

bool hits_any_button(const Layout& l, ImVec2 p) {
    bool tmp[BTN_JOYPAD_COUNT] = {};
    button_hits(l, p, tmp);
    for (bool b : tmp) if (b) return true;
    return false;
}

void recompute(const Layout& l) {
    std::memset(g_pressed, 0, sizeof(g_pressed));
    for (const Finger& f : g_fingers) {
        if (!f.active) continue;
        const ImVec2 p(f.x, f.y);
        if (f.zone == ZONE_DPAD) {
            bool dirs[4];
            dpad_directions(l, p, dirs);
            for (int i = 0; i < 4; ++i) g_pressed[i] = g_pressed[i] || dirs[i];
        } else if (f.zone == ZONE_BUTTONS) {
            button_hits(l, p, g_pressed);
        }
    }
}

Finger* find_finger(SDL_FingerID id) {
    for (Finger& f : g_fingers) if (f.active && f.id == id) return &f;
    return nullptr;
}

Finger* alloc_finger() {
    for (Finger& f : g_fingers) if (!f.active) return &f;
    return nullptr;
}

} // namespace

bool enabled_by_default() {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

void set_enabled(bool enabled) {
    g_enabled = enabled;
    if (!enabled) reset();
}

bool enabled() { return g_enabled; }

void reset() {
    for (Finger& f : g_fingers) f = Finger();
    std::memset(g_pressed, 0, sizeof(g_pressed));
}

bool pressed(int button) {
    return button >= 0 && button < BTN_JOYPAD_COUNT && g_pressed[button];
}

bool take_menu_request() {
    const bool r = g_menu_request;
    g_menu_request = false;
    return r;
}

bool handle_event(const SDL_Event& event, SDL_Window* window, bool menu_open) {
    if (event.type != SDL_FINGERDOWN && event.type != SDL_FINGERMOTION && event.type != SDL_FINGERUP) {
        return false;
    }
    if (!g_enabled) return false;

    g_last_touch_ms = SDL_GetTicks();

    if (menu_open) {
        bool any = false;
        for (const Finger& f : g_fingers) any = any || f.active;
        if (any) { reset(); return true; }
        return false;
    }

    int win_w = 0, win_h = 0;
    if (window) SDL_GetWindowSize(window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return false;
    g_last_w = (float)win_w;
    g_last_h = (float)win_h;
    const Layout l = compute_layout(g_last_w, g_last_h);
    const ImVec2 p(event.tfinger.x * g_last_w, event.tfinger.y * g_last_h);

    switch (event.type) {
        case SDL_FINGERDOWN: {
            if (dist(p, l.menu) <= l.menu_r * 1.5f) {
                g_menu_request = true;
                return true;
            }
            Finger* f = alloc_finger();
            if (!f) return false;
            f->active = true;
            f->id = event.tfinger.fingerId;
            f->x = p.x;
            f->y = p.y;
            if (dist(p, l.dpad) <= l.dpad_r * 1.5f)      f->zone = ZONE_DPAD;
            else if (hits_any_button(l, p))               f->zone = ZONE_BUTTONS;
            else                                          f->zone = ZONE_NONE;
            break;
        }
        case SDL_FINGERMOTION: {
            Finger* f = find_finger(event.tfinger.fingerId);
            if (!f) return false;
            f->x = p.x;
            f->y = p.y;
            /* Dedo que caiu fora dos controles pode "entrar" nos botões deslizando */
            if (f->zone == ZONE_NONE && hits_any_button(l, p)) f->zone = ZONE_BUTTONS;
            break;
        }
        case SDL_FINGERUP: {
            Finger* f = find_finger(event.tfinger.fingerId);
            if (!f) return false;
            *f = Finger();
            break;
        }
        default:
            return false;
    }
    recompute(l);
    return true;
}

void draw(ImDrawList* dl, float w, float h, bool controller_connected) {
    if (!g_enabled || !dl || w <= 0.0f || h <= 0.0f) return;
    if (controller_connected && (g_last_touch_ms == 0 || SDL_GetTicks() - g_last_touch_ms > kShowAfterTouchMs)) {
        return;
    }

    const Layout l = compute_layout(w, h);
    const ImU32 base   = IM_COL32(255, 255, 255, 55);
    const ImU32 active = IM_COL32(255, 255, 255, 140);
    const ImU32 edge   = IM_COL32(255, 255, 255, 90);
    const ImU32 label  = IM_COL32(255, 255, 255, 190);
    ImFont* font = ImGui::GetFont();

    auto text_centered = [&](ImVec2 c, float size, const char* txt) {
        const ImVec2 ts = font->CalcTextSizeA(size, 1e9f, 0.0f, txt);
        dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), label, txt);
    };

    /* D-pad em cruz */
    {
        const float arm = l.dpad_r * 0.36f;     /* meia largura do braço */
        const float len = l.dpad_r;
        const ImVec2 c = l.dpad;
        dl->AddCircle(c, l.dpad_r * 1.08f, IM_COL32(255, 255, 255, 25), 48, 2.0f);
        struct Arm { int btn; ImVec2 a, b; } arms[4] = {
            {BTN_RIGHT, ImVec2(c.x + arm, c.y - arm), ImVec2(c.x + len, c.y + arm)},
            {BTN_LEFT,  ImVec2(c.x - len, c.y - arm), ImVec2(c.x - arm, c.y + arm)},
            {BTN_UP,    ImVec2(c.x - arm, c.y - len), ImVec2(c.x + arm, c.y - arm)},
            {BTN_DOWN,  ImVec2(c.x - arm, c.y + arm), ImVec2(c.x + arm, c.y + len)},
        };
        dl->AddRectFilled(ImVec2(c.x - arm, c.y - arm), ImVec2(c.x + arm, c.y + arm), base);
        for (const Arm& a : arms) {
            dl->AddRectFilled(a.a, a.b, g_pressed[a.btn] ? active : base, arm * 0.35f);
        }
        dl->AddRect(ImVec2(c.x - arm, c.y - len), ImVec2(c.x + arm, c.y + len), edge, arm * 0.35f, 0, 1.5f);
        dl->AddRect(ImVec2(c.x - len, c.y - arm), ImVec2(c.x + len, c.y + arm), edge, arm * 0.35f, 0, 1.5f);
    }

    /* A e B */
    const float big = l.ab_r * 0.9f;
    dl->AddCircleFilled(l.a, l.ab_r, g_pressed[BTN_A] ? active : base, 40);
    dl->AddCircle(l.a, l.ab_r, edge, 40, 1.5f);
    text_centered(l.a, big, "A");
    dl->AddCircleFilled(l.b, l.ab_r, g_pressed[BTN_B] ? active : base, 40);
    dl->AddCircle(l.b, l.ab_r, edge, 40, 1.5f);
    text_centered(l.b, big, "B");

    /* Select / Start */
    auto pill = [&](ImVec2 c, bool on, const char* txt) {
        const ImVec2 a(c.x - l.pill.x * 0.5f, c.y - l.pill.y * 0.5f);
        const ImVec2 b(c.x + l.pill.x * 0.5f, c.y + l.pill.y * 0.5f);
        dl->AddRectFilled(a, b, on ? active : base, l.pill.y * 0.5f);
        dl->AddRect(a, b, edge, l.pill.y * 0.5f, 0, 1.5f);
        text_centered(c, l.pill.y * 0.55f, txt);
    };
    pill(l.select, g_pressed[BTN_SELECT], "SELECT");
    pill(l.start, g_pressed[BTN_START], "START");

    /* Menu (três traços) */
    dl->AddCircleFilled(l.menu, l.menu_r, base, 32);
    for (int i = -1; i <= 1; ++i) {
        const float y = l.menu.y + i * l.menu_r * 0.35f;
        dl->AddLine(ImVec2(l.menu.x - l.menu_r * 0.45f, y), ImVec2(l.menu.x + l.menu_r * 0.45f, y), label, 2.0f);
    }
}

} // namespace touch_controls
