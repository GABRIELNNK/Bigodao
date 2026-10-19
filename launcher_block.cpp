namespace launcher {

/* [android-port] textos que mudam por plataforma */
#ifdef __ANDROID__
#define LAUNCHER_HINT_EMPTY "Tap the button to choose the file."
#define LAUNCHER_HINT_READY "Tap LOAD & PLAY."
#define LAUNCHER_HINT_TIP   "The files are copied into the app's own storage."
#else
#define LAUNCHER_HINT_EMPTY "Or drag the file into this window."
#define LAUNCHER_HINT_READY "Click LOAD & PLAY or press Enter."
#define LAUNCHER_HINT_TIP   "Tip: drag the ROM and the patch straight into this window."
#endif

// ---- Paleta (mesmas cores do launcher em HTML) ------------------------------
constexpr ImU32 COL_BG          = IM_COL32( 13,  15,  23, 255);
constexpr ImU32 COL_CARD        = IM_COL32( 22,  25,  37, 242);
constexpr ImU32 COL_CARD_HOVER  = IM_COL32( 27,  30,  44, 246);
constexpr ImU32 COL_BORDER      = IM_COL32(255, 255, 255,  20);
constexpr ImU32 COL_BORDER_GOLD = IM_COL32(255, 204,   0,  80);
constexpr ImU32 COL_TEXT        = IM_COL32(255, 255, 255, 255);
constexpr ImU32 COL_SUB         = IM_COL32(148, 163, 184, 255);
constexpr ImU32 COL_MUTED       = IM_COL32(100, 116, 139, 255);
constexpr ImU32 COL_DIM         = IM_COL32( 71,  85, 105, 255);
constexpr ImU32 COL_GOLD        = IM_COL32(255, 204,   0, 255);
constexpr ImU32 COL_GREEN       = IM_COL32( 74, 222, 128, 255);
constexpr ImU32 COL_RED         = IM_COL32(248, 113, 113, 255);
constexpr ImU32 COL_BLUE        = IM_COL32( 37,  99, 235, 255);
constexpr ImU32 COL_BLUE_HOVER  = IM_COL32( 59, 130, 246, 255);
constexpr ImU32 COL_BLUE_ACTIVE = IM_COL32( 29,  78, 216, 255);
constexpr ImU32 COL_PLAY        = IM_COL32(225,  29,  72, 255);
constexpr ImU32 COL_PLAY_HOVER  = IM_COL32(244,  63,  94, 255);
constexpr ImU32 COL_PLAY_ACTIVE = IM_COL32(190,  18,  60, 255);
constexpr ImU32 COL_OFF_BG      = IM_COL32( 39,  39,  42, 255);
constexpr ImU32 COL_OFF_TEXT    = IM_COL32( 82,  82,  91, 255);
constexpr ImU32 COL_GLOW        = IM_COL32(150,  60, 230, 255);   // brilho roxo atrás do ícone

// ---- Layout -----------------------------------------------------------------
// [passo 9a] Tudo é medido em "unidades" e multiplicado por g_u, a escala da
// interface, calculada a cada quadro para o conteúdo preencher a tela. No PC a
// janela de 960x760 dá g_u perto de 1; num celular, 2 a 3. As fontes do ImGui
// (1.92+) são rasterizadas no tamanho pedido, então continuam nítidas.
constexpr int   WINDOW_W   = 960;
constexpr int   WINDOW_H   = 760;
constexpr float MARGIN     = 22.0f;
constexpr float GAP        = 16.0f;
constexpr float CARD_GAP   = 12.0f;

#ifdef __ANDROID__
// No celular: letras um pouco maiores, alvos de toque maiores e sem a caixa do
// SHA-256 (ocupa altura e só interessa a quem confere o arquivo à mão).
constexpr bool  SHOW_HASH  = false;
constexpr float FS_TITLE   = 19.0f;
constexpr float FS_BODY    = 15.0f;
constexpr float FS_SMALL   = 13.5f;
constexpr float FS_BUTTON  = 15.5f;
constexpr float FS_PLAY    = 18.0f;
constexpr float FS_MONO    = 13.0f;
constexpr float BTN_H      = 40.0f;
constexpr float PLAY_W     = 200.0f;
constexpr float PLAY_H     = 52.0f;
#else
constexpr bool  SHOW_HASH  = true;
constexpr float FS_TITLE   = 18.0f;
constexpr float FS_BODY    = 14.0f;
constexpr float FS_SMALL   = 12.5f;
constexpr float FS_BUTTON  = 14.0f;
constexpr float FS_PLAY    = 17.0f;
constexpr float FS_MONO    = 13.0f;
constexpr float BTN_H      = 34.0f;
constexpr float PLAY_W     = 212.0f;
constexpr float PLAY_H     = 46.0f;
#endif
constexpr float CARD_H     = SHOW_HASH ? 180.0f : (16.0f + 24.0f + 22.0f + BTN_H + 10.0f + 18.0f + 12.0f);
constexpr float FOOTER_H   = 14.0f + PLAY_H + 8.0f;

static float g_u = 1.0f;
static inline float U(float v) { return v * g_u; }
static inline ImVec2 U2(float x, float y) { return ImVec2(x * g_u, y * g_u); }

struct Fonts {
    ImFont* title  = nullptr;
    ImFont* body   = nullptr;
    ImFont* small  = nullptr;
    ImFont* button = nullptr;
    ImFont* play   = nullptr;
    ImFont* mono   = nullptr;
};

static ImFont* add_embedded_font(const unsigned char* data, unsigned int len, float px) {
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2022, 0x2022, 0x2026, 0x2026, 0 };
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // os dados são estáticos, o atlas não deve liberar
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), (int)len,
                                                      px, &cfg, ranges);
}

static Fonts load_fonts() {
    Fonts f;
    // A primeira fonte adicionada vira a padrão do ImGui.
    f.body   = add_embedded_font(LAUNCHER_FONT_SEMIBOLD,  LAUNCHER_FONT_SEMIBOLD_LEN,  FS_BODY);
    f.small  = add_embedded_font(LAUNCHER_FONT_SEMIBOLD,  LAUNCHER_FONT_SEMIBOLD_LEN,  FS_SMALL);
    f.title  = add_embedded_font(LAUNCHER_FONT_EXTRABOLD, LAUNCHER_FONT_EXTRABOLD_LEN, FS_TITLE);
    f.button = add_embedded_font(LAUNCHER_FONT_EXTRABOLD, LAUNCHER_FONT_EXTRABOLD_LEN, FS_BUTTON);
    f.play   = add_embedded_font(LAUNCHER_FONT_EXTRABOLD, LAUNCHER_FONT_EXTRABOLD_LEN, FS_PLAY);
    ImFontConfig mono_cfg;
    mono_cfg.SizePixels = FS_MONO;
    f.mono = ImGui::GetIO().Fonts->AddFontDefault(&mono_cfg);   // ProggyClean: monoespaçada, cara retrô
    return f;
}

// [passo 9a] arte = ícone do Bigodão (PNG com cantos transparentes)
static SDL_Texture* load_embedded_art(SDL_Renderer* renderer, int& w, int& h) {
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(LAUNCHER_ICON_PNG, (int)LAUNCHER_ICON_PNG_LEN,
                                                  &w, &h, &channels, 4);
    if (!pixels) return nullptr;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_UpdateTexture(tex, nullptr, pixels, w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    stbi_image_free(pixels);
    return tex;
}

// ---- Helpers de desenho -----------------------------------------------------
static ImU32 col_lerp(ImU32 a, ImU32 b, float t) {
    ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a), cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
                                                 ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
}

static ImU32 col_alpha(ImU32 c, float mul) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w *= mul;
    return ImGui::ColorConvertFloat4ToU32(v);
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float ease_out(float t) { t = clamp01(t); return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }

// Valor 0..1 que anima suavemente em direção ao alvo (para hovers).
static float anim(const char* key, bool on, float speed = 10.0f) {
    ImGui::PushID(key);
    ImGuiID id = ImGui::GetID("##anim");
    ImGui::PopID();
    ImGuiStorage* st = ImGui::GetStateStorage();
    float v = st->GetFloat(id, 0.0f);
    v = clamp01(v + (on ? 1.0f : -1.0f) * ImGui::GetIO().DeltaTime * speed);
    st->SetFloat(id, v);
    return v;
}

static ImVec2 text_size(ImFont* f, float size, const char* text, float wrap = 0.0f) {
    return f->CalcTextSizeA(size, FLT_MAX, wrap, text);
}

// Corta o texto com "…" se não couber na largura.
static std::string ellipsize(ImFont* f, float size, const std::string& s, float max_w) {
    if (text_size(f, size, s.c_str()).x <= max_w) return s;
    const char* ell = "\xE2\x80\xA6";
    const float ell_w = text_size(f, size, ell).x;
    std::string out = s;
    while (!out.empty()) {
        size_t i = out.size();
        do { --i; } while (i > 0 && (static_cast<unsigned char>(out[i]) & 0xC0) == 0x80);
        out.resize(i);
        if (text_size(f, size, out.c_str()).x + ell_w <= max_w) break;
    }
    return out + ell;
}

// Sombra suave: camadas translúcidas crescentes (spread e y_off já em pixels).
static void soft_shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 col,
                        float strength, float spread, int layers, float y_off) {
    for (int i = layers; i >= 1; --i) {
        const float e = spread * (float)i;
        const float alpha = strength * (1.0f - (float)i / (float)(layers + 1)) / (float)layers * 2.2f;
        dl->AddRectFilled(ImVec2(a.x - e, a.y - e + y_off), ImVec2(b.x + e, b.y + e + y_off),
                          col_alpha(col, alpha), rounding + e);
    }
}

// Botão "invisível" com hit-test do ImGui e desenho próprio.
struct ButtonState { bool pressed = false; bool hovered = false; bool held = false; float hover_t = 0.0f; };

static ButtonState hit_button(const char* id, ImVec2 pos, ImVec2 size, bool enabled, bool interactive) {
    ButtonState s;
    if (interactive) {
        ImGui::SetCursorScreenPos(pos);
        s.pressed = ImGui::InvisibleButton(id, size) && enabled;
        s.hovered = ImGui::IsItemHovered() && enabled;
        s.held    = ImGui::IsItemActive() && enabled;
        if (s.hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    s.hover_t = anim(id, s.hovered);
    return s;
}

static float blue_button_width(const Fonts& f, const char* label) {
    return text_size(f.button, U(FS_BUTTON), label).x + U(34.0f);
}

static bool blue_button(ImDrawList* dl, const char* id, const char* label, ImVec2 pos, float height,
                        const Fonts& f, bool interactive) {
    const ImVec2 ts = text_size(f.button, U(FS_BUTTON), label);
    const ImVec2 size(blue_button_width(f, label), height);
    ButtonState s = hit_button(id, pos, size, true, interactive);
    const float lift = s.held ? 0.0f : -U(1.0f) * s.hover_t;
    const ImVec2 a(pos.x, pos.y + lift), b(pos.x + size.x, pos.y + size.y + lift);

    soft_shadow(dl, a, b, U(8.0f), COL_BLUE, 0.25f + 0.25f * s.hover_t, U(2.0f), 4, U(2.0f));
    dl->AddRectFilled(a, b, s.held ? COL_BLUE_ACTIVE : col_lerp(COL_BLUE, COL_BLUE_HOVER, s.hover_t), U(8.0f));
    dl->AddRectFilled(a, ImVec2(b.x, a.y + size.y * 0.5f), IM_COL32(255, 255, 255, 18), U(8.0f),
                      ImDrawFlags_RoundCornersTop);
    dl->AddText(f.button, U(FS_BUTTON), ImVec2(a.x + (size.x - ts.x) * 0.5f, a.y + (size.y - ts.y) * 0.5f),
                COL_TEXT, label);
    return s.pressed;
}

// ---- Ícones vetoriais (as fontes não têm ✓ ✖ ▶) ------------------------------
static void icon_check(ImDrawList* dl, ImVec2 p, float s, ImU32 col, float th) {
    ImVec2 pts[3] = { ImVec2(p.x, p.y + s * 0.55f), ImVec2(p.x + s * 0.38f, p.y + s * 0.90f),
                      ImVec2(p.x + s, p.y + s * 0.12f) };
    dl->AddPolyline(pts, 3, col, ImDrawFlags_None, th);
}

static void icon_cross(ImDrawList* dl, ImVec2 p, float s, ImU32 col, float th) {
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + s, p.y + s), col, th);
    dl->AddLine(ImVec2(p.x + s, p.y), ImVec2(p.x, p.y + s), col, th);
}

static void icon_play(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    dl->AddTriangleFilled(ImVec2(p.x, p.y), ImVec2(p.x + s * 0.88f, p.y + s * 0.5f), ImVec2(p.x, p.y + s), col);
}

// ---- Estado de cada arquivo -------------------------------------------------
struct FileSlot {
    enum class State { Empty, Valid, Invalid, Missing };
    fs::path    path;
    std::string name;       // nome do arquivo em UTF-8 (para exibição)
    State       state = State::Empty;
    float       changed_at = -10.0f;   // para o "flash" de feedback

    // O hash só é calculado quando o arquivo muda (antes rodava a cada frame).
    void set(const fs::path& p, const std::string& expected, float now) {
        path = p;
        name = path_to_utf8(p.filename());
        std::error_code ec;
        if (!fs::is_regular_file(p, ec))                 state = State::Missing;
        else if (calculate_file_sha256(p) == expected)  state = State::Valid;
        else                                             state = State::Invalid;
        changed_at = now;
    }
    bool valid() const { return state == State::Valid; }
};

static void draw_badge(ImDrawList* dl, ImVec2 right_top, FileSlot::State st, const Fonts& f) {
    const char* text;  ImU32 fg, bg, border;
    switch (st) {
        case FileSlot::State::Valid:   text = "Valid";          fg = COL_GREEN; bg = IM_COL32(34, 197, 94, 38); border = IM_COL32(34, 197, 94, 77); break;
        case FileSlot::State::Invalid: text = "Invalid";        fg = COL_RED;   bg = IM_COL32(239, 68, 68, 38); border = IM_COL32(239, 68, 68, 77); break;
        case FileSlot::State::Missing: text = "Not found";      fg = COL_RED;   bg = IM_COL32(239, 68, 68, 38); border = IM_COL32(239, 68, 68, 77); break;
        default:                       text = "Pending";        fg = COL_MUTED; bg = IM_COL32(148, 163, 184, 26); border = 0; break;
    }
    const ImVec2 ts = text_size(f.small, U(FS_SMALL), text);
    const float icon = U(9.0f), pad = U(9.0f), h = U(FS_SMALL + 10.5f);
    const float w = pad + icon + U(6.0f) + ts.x + pad;
    const ImVec2 a(right_top.x - w, right_top.y), b(right_top.x, right_top.y + h);
    dl->AddRectFilled(a, b, bg, U(6.0f));
    if (border) dl->AddRect(a, b, border, U(6.0f), 0, U(1.0f));
    const ImVec2 ip(a.x + pad, a.y + (h - icon) * 0.5f);
    if (st == FileSlot::State::Valid)      icon_check(dl, ip, icon, fg, U(1.8f));
    else if (st == FileSlot::State::Empty) dl->AddCircle(ImVec2(ip.x + icon * 0.5f, ip.y + icon * 0.5f), icon * 0.45f, fg, 12, U(1.6f));
    else                                   icon_cross(dl, ip, icon * 0.9f, fg, U(1.8f));
    dl->AddText(f.small, U(FS_SMALL), ImVec2(ip.x + icon + U(6.0f), a.y + (h - ts.y) * 0.5f), fg, text);
}

struct CardSpec {
    const char* id;
    const char* title;
    const char* subtitle;
    const char* sha256;
    const char* button;
    const char* empty_hint;
    const char* invalid_msg;
};

// Desenha um card (pos/size em pixels); devolve true se o botão "Browse" foi clicado.
static bool draw_card(ImDrawList* dl, ImVec2 pos, ImVec2 size, const CardSpec& spec,
                      const FileSlot& slot, const Fonts& f, bool interactive, float now) {
    const ImVec2 a = pos, b(pos.x + size.x, pos.y + size.y);
    const bool hovered = interactive && ImGui::IsMouseHoveringRect(a, b);
    const float ht = anim(spec.id, hovered, 8.0f);
    const float r = U(14.0f);

    ImU32 accent = COL_GOLD;
    if (slot.state == FileSlot::State::Valid) accent = COL_GREEN;
    else if (slot.state != FileSlot::State::Empty) accent = COL_RED;

    // Moldura
    soft_shadow(dl, a, b, r, IM_COL32(0, 0, 0, 255), 0.55f, U(3.0f), 6, U(6.0f));
    dl->AddRectFilled(a, b, col_lerp(COL_CARD, COL_CARD_HOVER, ht), r);
    const float flash = 1.0f - clamp01((now - slot.changed_at) / 0.6f);   // brilho rápido ao validar
    dl->AddRect(a, b, col_lerp(col_lerp(COL_BORDER, COL_BORDER_GOLD, ht), col_alpha(accent, 0.9f), flash),
                r, 0, U(1.0f + flash));
    // Filete de destaque no topo (cor do estado)
    const float acc_a = (slot.state == FileSlot::State::Empty) ? 0.35f + 0.35f * ht : 0.9f;
    dl->AddRectFilledMultiColor(ImVec2(a.x + r, a.y), ImVec2(a.x + size.x * 0.55f, a.y + U(2.0f)),
                                col_alpha(accent, acc_a), col_alpha(accent, 0.0f),
                                col_alpha(accent, 0.0f), col_alpha(accent, acc_a));

    const float px = a.x + U(18.0f), inner_w = size.x - U(36.0f);
    float y = a.y + U(16.0f);

    // Título + badge
    dl->AddText(f.title, U(FS_TITLE), ImVec2(px, y - U(1.0f)), COL_TEXT, spec.title);
    draw_badge(dl, ImVec2(b.x - U(16.0f), y - U(2.0f)), slot.state, f);
    y += U(24.0f);

    // Subtítulo
    const std::string sub = ellipsize(f.small, U(FS_SMALL), spec.subtitle, inner_w);
    dl->AddText(f.small, U(FS_SMALL), ImVec2(px, y), COL_SUB, sub.c_str());
    y += U(22.0f);

    // Caixa com o SHA-256 esperado (2 linhas, fonte monoespaçada). Só no PC.
    if (SHOW_HASH) {
        const float box_h = U(38.0f);
        dl->AddRectFilled(ImVec2(px, y), ImVec2(px + inner_w, y + box_h), IM_COL32(0, 0, 0, 70), U(7.0f));
        dl->AddRect(ImVec2(px, y), ImVec2(px + inner_w, y + box_h), IM_COL32(255, 255, 255, 10), U(7.0f));
        dl->AddText(f.small, U(FS_SMALL), ImVec2(px + U(12.0f), y + (box_h - U(FS_SMALL)) * 0.5f - U(1.0f)), COL_DIM, "SHA-256");
        const std::string hash = spec.sha256;
        const float hx = px + U(78.0f);
        dl->AddText(f.mono, U(FS_MONO), ImVec2(hx, y + U(5.0f)),  COL_MUTED, hash.substr(0, 32).c_str());
        dl->AddText(f.mono, U(FS_MONO), ImVec2(hx, y + U(20.0f)), COL_MUTED, hash.substr(32).c_str());
        y += box_h + U(12.0f);
    }

    // Botão + nome do arquivo
    const float btn_h = U(BTN_H);
    const bool clicked = blue_button(dl, spec.id, spec.button, ImVec2(px, y), btn_h, f, interactive);
    const float btn_w = blue_button_width(f, spec.button);
    const float name_x = px + btn_w + U(14.0f);
    std::string shown = slot.state == FileSlot::State::Empty ? "No file selected" : slot.name;
    ImU32 name_col = COL_MUTED;
    if (slot.state == FileSlot::State::Valid) name_col = COL_GREEN;
    else if (slot.state != FileSlot::State::Empty) name_col = COL_RED;
    shown = ellipsize(f.body, U(FS_BODY), shown, (px + inner_w) - name_x);
    dl->AddText(f.body, U(FS_BODY), ImVec2(name_x, y + (btn_h - U(FS_BODY)) * 0.5f - U(1.0f)), name_col, shown.c_str());
    y += btn_h + U(10.0f);

    // Linha de feedback
    const char* msg;  ImU32 msg_col;
    switch (slot.state) {
        case FileSlot::State::Valid:   msg = "Hash matches. All good!"; msg_col = col_alpha(COL_GREEN, 0.85f); break;
        case FileSlot::State::Invalid: msg = spec.invalid_msg;          msg_col = COL_RED; break;
        case FileSlot::State::Missing: msg = "File not found.";         msg_col = COL_RED; break;
        default:                       msg = spec.empty_hint;           msg_col = COL_DIM; break;
    }
    const std::string fb = ellipsize(f.small, U(FS_SMALL), msg, inner_w);
    dl->AddText(f.small, U(FS_SMALL), ImVec2(px, y), msg_col, fb.c_str());
    return clicked;
}

// [passo 9a] Ícone com brilho roxo atrás e entrada suave.
static void draw_icon(ImDrawList* dl, SDL_Texture* art, ImVec2 center, float size, float appear, float t) {
    const float pulse = 0.5f + 0.5f * std::sin(t * 1.6f);
    for (int i = 7; i >= 1; --i) {
        const float rr = size * (0.50f + 0.035f * (float)i);
        dl->AddCircleFilled(center, rr, col_alpha(COL_GLOW, (0.030f + 0.012f * pulse) * appear), 64);
    }
    if (!art) return;
    const float s = size * (0.94f + 0.06f * appear);   // leve "zoom" na abertura
    const ImVec2 a(center.x - s * 0.5f, center.y - s * 0.5f), b(center.x + s * 0.5f, center.y + s * 0.5f);
    dl->AddImage((ImTextureID)(intptr_t)art, a, b, ImVec2(0, 0), ImVec2(1, 1),
                 IM_COL32(255, 255, 255, (int)(255 * appear)));
}

} // namespace launcher

// Execute Launcher GUI via ImGui + SDL2
static bool run_imgui_launcher(std::string& out_rom_path, std::string& out_patch_path, const char* exec_path) {
    using namespace launcher;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "Failed to initialize SDL2 for Launcher: " << SDL_GetError() << "\n";
        return false;
    }
#ifdef __ANDROID__   /* [passo 9a] o "voltar" do Android chega como tecla (SDLK_AC_BACK) */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif

    SDL_Window* window = SDL_CreateWindow(
        "Bigodão Launcher - Wario Land DX",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window) {
        SDL_Quit();
        return false;
    }

    app_icon_apply(window);   /* [fix-v3] ícone da janela do launcher */
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);   // fallback: renderer por software
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // não criar imgui.ini ao lado do jogo

    ImGui::StyleColorsDark();
    const Fonts fonts = load_fonts();

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    int art_w = 0, art_h = 0;
    SDL_Texture* art = load_embedded_art(renderer, art_w, art_h);

    const CardSpec rom_card = {
        "rom",
        "Wario Land (World)",
        "Original ROM  \xE2\x80\xA2  Super Mario Land 3  \xE2\x80\xA2  .gb",
        EXPECTED_ROM_SHA256.c_str(),
        "Browse ROM",
        LAUNCHER_HINT_EMPTY,
        "SHA-256 mismatch. Please select the original ROM.",
    };
    const CardSpec patch_card = {
        "patch",
        "Patch Wario Land DX",
        "IPS patch  \xE2\x80\xA2  author: korxo  \xE2\x80\xA2  version 1.2",
        EXPECTED_PATCH_SHA256.c_str(),
        "Browse Patch",
        LAUNCHER_HINT_EMPTY,
        "SHA-256 mismatch. This is not the DX v1.2 patch.",
    };
    const DialogSpec rom_dialog = {
        L"Select the original ROM",
        L"Game Boy ROMs (*.gb, *.gbc)\0*.gb;*.gbc\0All files (*.*)\0*.*\0",
        "Select the original ROM",
        "--file-filter='ROMs (*.gb *.gbc) | *.gb *.gbc'",
        "*.gb *.gbc|Game Boy ROMs",
    };
    const DialogSpec patch_dialog = {
        L"Select the IPS patch",
        L"IPS patches (*.ips)\0*.ips\0All files (*.*)\0*.*\0",
        "Select the IPS patch",
        "--file-filter='Patches (*.ips) | *.ips'",
        "*.ips|IPS patches",
    };

    FileSlot rom, patch;
#ifdef __ANDROID__   /* [android-port] */
    (void)rom_dialog; (void)patch_dialog;
    // [passo 9a] Densidade da tela (dp = dpi / 160), para limitar a escala em tablets.
    float android_dp = 2.5f;
    {
        float ddpi = 0.0f;
        if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0 && ddpi > 0.0f) android_dp = ddpi / 160.0f;
    }
#endif
    std::string modal_msg;
    bool ready_to_launch = false;
    bool done = false;
    const Uint64 t_start = SDL_GetPerformanceCounter();
    const double freq = (double)SDL_GetPerformanceFrequency();
    auto now = [&]() { return (float)((SDL_GetPerformanceCounter() - t_start) / freq); };

    // Arquivo solto na janela: decide pelo hash; se não bater, pela extensão.
    auto handle_drop = [&](const fs::path& p) {
        const std::string h = calculate_file_sha256(p);
        std::string ext = path_to_utf8(p.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (h == EXPECTED_PATCH_SHA256 || (h != EXPECTED_ROM_SHA256 && ext == ".ips"))
            patch.set(p, EXPECTED_PATCH_SHA256, now());
        else
            rom.set(p, EXPECTED_ROM_SHA256, now());
    };

    auto try_launch = [&]() {
        // Revalida (o arquivo pode ter mudado desde a seleção).
        rom.set(rom.path, EXPECTED_ROM_SHA256, now());
        patch.set(patch.path, EXPECTED_PATCH_SHA256, now());
        if (!rom.valid() || !patch.valid()) return;
        try {
            find_patch_manifest(exec_path);
            out_rom_path = rom.path.string();
            out_patch_path = patch.path.string();
            ready_to_launch = true;
            done = true;
        } catch (const std::exception& e) {
            modal_msg = std::string("Failed to prepare the ROM:\n") + e.what();
        }
    };

    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
            else if (event.type == SDL_DROPFILE) {
                if (event.drop.file) {
                    if (modal_msg.empty()) handle_drop(path_from_utf8(event.drop.file));
                    SDL_free(event.drop.file);
                }
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Keycode k = event.key.keysym.sym;
                // [passo 9a] "Voltar" do Android: fecha a mensagem de erro, ou sai do launcher.
                const bool back = (k == SDLK_AC_BACK) ||
                                  (event.key.keysym.scancode == SDL_SCANCODE_AC_BACK);
                if (back) {
                    if (!modal_msg.empty()) modal_msg.clear();
                    else done = true;
                }
                else if (!modal_msg.empty() && (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE))
                    modal_msg.clear();
                else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && rom.valid() && patch.valid())
                    try_launch();
            }
        }
        if (done) break;

#ifdef __ANDROID__   /* [android-port] */
        {
            // Resposta do seletor de arquivos: decide ROM/patch pelo hash, como no drag-and-drop.
            int picked_kind = 0;
            std::string picked;
            if (android_bridge::poll_picked_file(picked_kind, picked) && !picked.empty() && modal_msg.empty())
                handle_drop(path_from_utf8(picked));
            // [passo 9a] Sem tamanho lógico: desenha nos pixels reais da tela, com as
            // fontes rasterizadas no tamanho final (antes, 960x760 ampliado = letras
            // pequenas e borradas).
            SDL_RenderSetLogicalSize(renderer, 0, 0);
        }
#endif

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
#ifdef __ANDROID__   /* [passo 9a] tela inteira em pixels */
        {
            int out_w = 0, out_h = 0;
            SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
            int win_w = 0, win_h = 0;
            SDL_GetWindowSize(window, &win_w, &win_h);
            if (out_w > 0 && out_h > 0 && win_w > 0 && win_h > 0) {
                io.DisplaySize = ImVec2((float)win_w, (float)win_h);
                io.DisplayFramebufferScale = ImVec2((float)out_w / (float)win_w, (float)out_h / (float)win_h);
            }
        }
#endif
        ImGui::NewFrame();

        const float t = now();
        const float W = io.DisplaySize.x, H = io.DisplaySize.y;
        const bool interactive = modal_msg.empty();
        const float appear = ease_out(t / 0.6f);   // animação de entrada

        // ---- Escala e disposição ([passo 9a]) --------------------------------
        // Deitado: ícone à esquerda, cards empilhados à direita.
        // Em pé:   ícone em cima, cards embaixo.
        const bool portrait = H > W * 1.05f;
        const float content_h = CARD_H * 2.0f + CARD_GAP + FOOTER_H;
        float need_w, need_h;
        if (!portrait) { need_w = 820.0f; need_h = MARGIN * 2.0f + content_h; }
        else           { need_w = 440.0f; need_h = MARGIN * 2.0f + 220.0f + GAP + content_h; }
        float u = std::min(W / need_w, H / need_h);
#ifdef __ANDROID__
        u = clampf(u, 1.0f, std::max(1.0f, android_dp * 1.5f));
#else
        u = clampf(u, 1.0f, 1.35f);
#endif
        g_u = u;
        const float Wu = W / u, Hu = H / u;   // tela em unidades

        float icon_size, icon_cx, icon_cy, col_x, col_w, col_y;
        if (!portrait) {
            icon_size = std::min(Hu - MARGIN * 2.0f, Wu * 0.38f);
            icon_size = std::min(icon_size, 460.0f);
            icon_cx = MARGIN + icon_size * 0.5f;
            icon_cy = Hu * 0.5f;
            col_x = MARGIN + icon_size + GAP * 1.5f;
            col_w = Wu - col_x - MARGIN;
            col_y = std::max(MARGIN, (Hu - content_h) * 0.5f);
        } else {
            col_x = MARGIN;
            col_w = Wu - MARGIN * 2.0f;
            icon_size = std::min(col_w, Hu - MARGIN * 2.0f - GAP - content_h);
            icon_size = clampf(icon_size, 160.0f, 420.0f);
            const float total = icon_size + GAP + content_h;
            const float top = std::max(MARGIN, (Hu - total) * 0.5f);
            icon_cx = Wu * 0.5f;
            icon_cy = top + icon_size * 0.5f;
            col_y = top + icon_size + GAP;
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::Begin("MainLauncher", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // ---- Fundo e ícone ---------------------------------------------------
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), COL_BG);
        draw_icon(dl, art, U2(icon_cx, icon_cy), U(icon_size), appear, t);

        // ---- Cards -----------------------------------------------------------
        const float slide = 18.0f * (1.0f - appear);
        const ImVec2 card_size = U2(col_w, CARD_H);
        const float card1_y = col_y + slide;
        const float card2_y = col_y + CARD_H + CARD_GAP + slide;
        if (draw_card(dl, U2(col_x, card1_y), card_size, rom_card, rom, fonts, interactive, t)) {
#ifdef __ANDROID__   /* [android-port] */
            android_bridge::request_file_pick(android_bridge::PICK_ROM);
#else
            const fs::path p = open_file_dialog(window, rom_dialog);
            if (!p.empty()) rom.set(p, EXPECTED_ROM_SHA256, now());
#endif
        }
        if (draw_card(dl, U2(col_x, card2_y), card_size, patch_card, patch, fonts, interactive, t)) {
#ifdef __ANDROID__   /* [android-port] */
            android_bridge::request_file_pick(android_bridge::PICK_PATCH);
#else
            const fs::path p = open_file_dialog(window, patch_dialog);
            if (!p.empty()) patch.set(p, EXPECTED_PATCH_SHA256, now());
#endif
        }

        // ---- Rodapé ----------------------------------------------------------
        const bool can_play = rom.valid() && patch.valid();
        const float footer_y = card2_y + CARD_H + 14.0f;   // em unidades
        dl->AddLine(U2(col_x, footer_y), U2(col_x + col_w, footer_y), IM_COL32(255, 255, 255, 18), U(1.0f));

        const ImVec2 play_size = U2(PLAY_W, PLAY_H);
        const ImVec2 play_pos = U2(col_x + col_w - PLAY_W, footer_y + 8.0f);

        // Status à esquerda
        const char* status;  ImU32 status_col;
        if (can_play)                                                       { status = "Ready to play!";              status_col = COL_GREEN; }
        else if (rom.state == FileSlot::State::Empty ||
                 patch.state == FileSlot::State::Empty)                     { status = "Select the ROM and the patch"; status_col = COL_SUB; }
        else                                                                { status = "Invalid files";               status_col = COL_RED; }
        const float text_x = U(col_x + 18.0f);
        const float text_w = play_pos.x - U(14.0f) - text_x;
        const float sy = play_pos.y + U(2.0f);
        const float pulse = 0.5f + 0.5f * std::sin(t * 4.0f);
        const ImVec2 dot(U(col_x + 5.0f), sy + U(FS_BODY) * 0.55f);
        if (can_play) dl->AddCircleFilled(dot, U(5.0f + 4.0f * pulse), col_alpha(COL_GREEN, 0.25f * (1.0f - pulse)), 20);
        dl->AddCircleFilled(dot, U(4.0f), status_col, 16);
        const std::string status_s = ellipsize(fonts.body, U(FS_BODY), status, text_w);
        dl->AddText(fonts.body, U(FS_BODY), ImVec2(text_x, sy), status_col, status_s.c_str());
        dl->AddText(fonts.small, U(FS_SMALL), ImVec2(text_x, sy + U(FS_BODY + 6.0f)), COL_DIM,
                    can_play ? LAUNCHER_HINT_READY : LAUNCHER_HINT_TIP, nullptr, text_w);

        // Botão LOAD & PLAY
        {
            ButtonState s = hit_button("##play", play_pos, play_size, can_play, interactive);
            const float grow = s.held ? -U(1.0f) : U(3.0f) * s.hover_t;
            const ImVec2 a(play_pos.x - grow, play_pos.y - grow * 0.5f);
            const ImVec2 b(play_pos.x + play_size.x + grow, play_pos.y + play_size.y + grow * 0.5f);
            const float r = (b.y - a.y) * 0.5f;
            ImU32 fill = COL_OFF_BG, text_col = COL_OFF_TEXT;
            if (can_play) {
                soft_shadow(dl, a, b, r, COL_PLAY, 0.45f + 0.25f * pulse + 0.3f * s.hover_t, U(3.0f), 6, U(3.0f));
                fill = s.held ? COL_PLAY_ACTIVE : col_lerp(COL_PLAY, COL_PLAY_HOVER, s.hover_t);
                text_col = COL_TEXT;
            }
            dl->AddRectFilled(a, b, fill, r);
            if (can_play) {
                dl->AddRectFilled(a, ImVec2(b.x, (a.y + b.y) * 0.5f), IM_COL32(255, 255, 255, 22), r,
                                  ImDrawFlags_RoundCornersTop);
            } else {
                dl->AddRect(a, b, IM_COL32(255, 255, 255, 12), r, 0, U(1.0f));
            }
            const char* label = "LOAD & PLAY";
            const ImVec2 ts = text_size(fonts.play, U(FS_PLAY), label);
            const float icon = U(13.0f), gap = U(10.0f);
            const float cx = (a.x + b.x - (icon + gap + ts.x)) * 0.5f;
            const float cy = (a.y + b.y) * 0.5f;
            icon_play(dl, ImVec2(cx, cy - icon * 0.5f), icon, text_col);
            dl->AddText(fonts.play, U(FS_PLAY), ImVec2(cx + icon + gap, cy - ts.y * 0.5f), text_col, label);
            if (s.pressed) try_launch();
        }

        // ---- Modal de erro ---------------------------------------------------
        if (!modal_msg.empty()) {
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(5, 6, 10, 190));
            const float mw = std::min(U(480.0f), W - U(MARGIN * 2.0f)), pad = U(24.0f);
            const ImVec2 msg_sz = text_size(fonts.body, U(FS_BODY), modal_msg.c_str(), mw - pad * 2.0f);
            const float mh = pad + U(26.0f + 10.0f) + msg_sz.y + U(16.0f) + U(BTN_H) + pad;
            const ImVec2 a((W - mw) * 0.5f, (H - mh) * 0.5f), b(a.x + mw, a.y + mh);
            soft_shadow(dl, a, b, U(16.0f), IM_COL32(0, 0, 0, 255), 0.8f, U(4.0f), 6, U(8.0f));
            dl->AddRectFilled(a, b, IM_COL32(24, 26, 38, 255), U(16.0f));
            dl->AddRect(a, b, IM_COL32(239, 68, 68, 90), U(16.0f), 0, U(1.5f));
            icon_cross(dl, ImVec2(a.x + pad, a.y + pad + U(5.0f)), U(12.0f), COL_RED, U(2.5f));
            dl->AddText(fonts.title, U(FS_TITLE), ImVec2(a.x + pad + U(24.0f), a.y + pad), COL_TEXT, "Could not start the game");
            dl->AddText(fonts.body, U(FS_BODY), ImVec2(a.x + pad, a.y + pad + U(36.0f)), COL_SUB,
                        modal_msg.c_str(), nullptr, mw - pad * 2.0f);
            const float ok_w = blue_button_width(fonts, "OK");
            if (blue_button(dl, "##modal_ok", "OK", ImVec2(b.x - pad - ok_w, b.y - pad - U(BTN_H)), U(BTN_H), fonts, true))
                modal_msg.clear();
        }

        ImGui::SetCursorScreenPos(ImVec2(0, 0));
        ImGui::Dummy(ImVec2(0, 0));   // fecha o layout sem estender a janela
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 13, 15, 23, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData()
#if IMGUI_VERSION_NUM >= 19070   // backend passou a exigir o SDL_Renderer*
                                              , renderer
#endif
        );
        SDL_RenderPresent(renderer);
    }

    if (art) SDL_DestroyTexture(art);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
#ifndef __ANDROID__   /* [android-port] no Android o SDL segue vivo até o fim do app */
    SDL_Quit();
#endif

    return ready_to_launch;
}

