/**
 * @file gpu_ppu.cpp
 * @brief Centro e laterais do quadro montados pela GPU (passos 14b-1 a 14b-4). Veja gpu_ppu.h.
 */
#include "gpu_ppu.h"
#include "video_gl.h"
#include "video_gl_fns.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vglx;

namespace {

constexpr int kW = GB_SCREEN_WIDTH;
constexpr int kH = GB_SCREEN_HEIGHT;
constexpr int kMaxScale = PPU_NATIVE_HD_MAX_SCALE;

bool g_ready = false;
GLuint g_prog = 0, g_vao = 0;
GLuint g_fbo = 0, g_out = 0;          /* 1x */
GLuint g_fbo_hd = 0, g_out_hd = 0;    /* HD (160*N x 144*N) */
int g_out_hd_scale = 0;
GLuint g_tex_vram = 0, g_tex_line = 0, g_tex_oam = 0, g_tex_pal = 0;
/* Passo 14b-2: escritas no meio do quadro, organizadas por endereço */
GLuint g_tex_dirty = 0, g_tex_log = 0;
GLint g_loc_haslog = -1, g_loc_scale = -1, g_loc_hd = -1;
constexpr int kLogW = 256, kLogH = (PPU_NATIVE_GPU_LOG_MAX + kLogW - 1) / kLogW;
std::vector<uint16_t> g_dirty;        /* 0x4000 x (início + 1, quantidade) */
std::vector<uint8_t> g_logtex;        /* kLogW*kLogH x (line_plus1, valor) */
std::vector<uint16_t> g_count;        /* por endereço */
std::vector<uint32_t> g_dirty_addrs;  /* endereços sujos no último quadro (para limpar) */
bool g_had_log = false;
bool g_has_log = false;
uint32_t g_uploaded_serial = 0xFFFFFFFFu;

/* Passo 14b-3: tiles HD. Atlas em camadas (2D array), uma entrada de
 * metadados por entrada da tabela de tiles HD do ppu_native. */
GLuint g_tex_slot = 0;     /* R16UI 768x16: slot + 1 por tile e (tipo, paleta) */
GLuint g_tex_meta = 0;     /* RGBA16UI 256x128: x, y, camada | tem_ref << 15, lado */
GLuint g_tex_ref = 0;      /* RGBA8UI 1024x128: 4 cores de referência por slot */
GLuint g_tex_atlas = 0;    /* RGBA8UI 2D array */
GLuint g_tex_tldir = 0;    /* RG16UI 768x1: (início + 1, quantidade) das trocas de tile HD */
GLuint g_tex_tl = 0;       /* R16UI 17 x PPU_NATIVE_GPU_TL_MAX: linha + 1, 16 slots */
GLint g_loc_hastl = -1;
/* Passo 14b-4: laterais do widescreen (registros por ponto vindos de ppu.c) */
constexpr int kMW = PPU_NATIVE_GPU_MARGIN_W;
GLuint g_tex_mrec = 0;     /* RGBA32UI 192x144 (2 por ponto) */
GLuint g_tex_fade = 0;     /* R8UI 256x96 */
GLuint g_fbo_m = 0, g_out_m = 0;   /* laterais HD (96*N x 144*N) */
int g_out_m_scale = 0;
GLint g_loc_margin = -1;
uint32_t g_margin_serial = 0xFFFFFFFFu;

/* Passo 14c-1: tela inteira (a textura do jogo), montada como em
 * render_frame_internal: base 1x da CPU ampliada, centro HD e laterais HD da
 * GPU por cima, fundo ambiente HD nas laterais. */
GLuint g_sprog = 0;
GLuint g_tex_sbase = 0;    /* RGBA8UI 256x144: linha base 1x (bytes de 0xAARRGGBB) */
GLuint g_tex_samb = 0;     /* RGBA16UI 256x144: fundo ambiente * 256 */
GLuint g_fbo_s = 0, g_out_s = 0;
int g_out_s_w = 0, g_out_s_h = 0;
GLint g_sloc_scale = -1, g_sloc_wide = -1, g_sloc_hdc = -1, g_sloc_hdm = -1, g_sloc_amb = -1;

/* Passo 14e-1: filtros de pós-processamento (post_processing.cpp) na tela
 * da GPU. g_final = a tela que vale (filtrada ou não). */
GLuint g_fprog = 0;
GLuint g_fbo_f = 0, g_out_f = 0;
int g_out_f_w = 0, g_out_f_h = 0;
GLint g_floc_mode = -1, g_floc_size = -1;
GLuint g_final = 0;

const char* kFilterFragment =
    "uniform sampler2D u_src;\n"
    "uniform int u_mode;\n"
    "uniform ivec2 u_size;\n"
    "out vec4 o_color;\n"
    "uvec3 px(int x, int y) {\n"
    "    ivec2 p = clamp(ivec2(x, y), ivec2(0), u_size - 1);\n"
    "    return uvec3(round(texelFetch(u_src, p, 0).rgb * 255.0));\n"
    "}\n"
    "bool cdiff(uvec3 a, uvec3 b, int t) { ivec3 d = abs(ivec3(a) - ivec3(b)); return d.x + d.y + d.z > t; }\n"
    "uvec3 mixc(uvec3 a, uvec3 b, uint w) { return (a * (256u - w) + b * w) >> 8u; }\n"
    "void main() {\n"
    "    int X = int(gl_FragCoord.x), Y = int(gl_FragCoord.y);\n"
    "    uvec3 c = px(X, Y);\n"
    "    if (u_mode == 1) {\n"   /* LCD Grid */
    "        if ((Y & 1) == 1 || (X & 1) == 1) c = c * 3u / 4u;\n"
    "    } else if (u_mode == 2) {\n"   /* Scanlines */
    "        if ((Y & 1) == 1) c = c * 6u / 10u;\n"
    "    } else if (u_mode == 3) {\n"   /* Scale2x */
    "        uvec3 A = px(X, Y - 1), E = px(X, Y + 1), C = px(X - 1, Y), D = px(X + 1, Y);\n"
    "        uvec3 E0 = c, E1 = c, E2 = c, E3 = c;\n"
    "        if (C == A && C != D && A != E) E0 = A;\n"
    "        if (A == D && A != C && D != E) E1 = D;\n"
    "        if (C == E && C != D && E != A) E2 = C;\n"
    "        if (E == D && E != C && D != A) E3 = E;\n"
    "        c = (E0 + E1 + E2 + E3) / 4u;\n"
    "    } else if (u_mode == 4) {\n"   /* HQ2x */
    "        uvec3 P2 = px(X, Y - 1), P8 = px(X, Y + 1), P4 = px(X - 1, Y), P6 = px(X + 1, Y);\n"
    "        bool dt = cdiff(c, P2, 30), db = cdiff(c, P8, 30), dl = cdiff(c, P4, 30), dr = cdiff(c, P6, 30);\n"
    "        if (dt && dl) c = mixc(c, mixc(P2, P4, 128u), 64u);\n"
    "        else if (db && dr) c = mixc(c, mixc(P8, P6, 128u), 64u);\n"
    "    } else if (u_mode == 5 || u_mode == 6) {\n"   /* xBRZ 2x / 4x */
    "        uint w = u_mode == 6 ? 115u : 77u;\n"
    "        uvec3 B = px(X, Y - 1), H = px(X, Y + 1), D = px(X - 1, Y), F = px(X + 1, Y);\n"
    "        if (cdiff(D, B, 30) && cdiff(c, F, 30)) c = mixc(c, mixc(D, B, 128u), w);\n"
    "        else if (cdiff(B, F, 30) && cdiff(c, D, 30)) c = mixc(c, mixc(B, F, 128u), w);\n"
    "    } else if (u_mode == 7) {\n"   /* Pixel-Perfect Sharp */
    "        uvec3 r = px(X + 1, Y), d = px(X, Y + 1);\n"
    "        if (cdiff(c, r, 40)) c = mixc(c, r, 31u);\n"
    "        if (cdiff(c, d, 40)) c = mixc(c, d, 31u);\n"
    "    }\n"
    "    o_color = vec4(vec3(c) / 255.0, 1.0);\n"
    "}\n";

/* Passo 14d-1: camadas do 120 fps (ip_finish_capture do platform_sdl.cpp)
 * feitas a partir da tela da GPU: cenário sem os sprites (o buraco de cada
 * trecho de sprite preenchido metade com o ponto da esquerda, metade com o
 * da direita) e só os sprites (alfa). */
GLuint g_iprog = 0;
GLuint g_tex_iruns = 0;    /* RG16UI 256x144: (início + 1, fim) do trecho de sprite; 0 = cenário */
GLuint g_fbo_ibg = 0, g_out_ibg = 0, g_fbo_iobj = 0, g_out_iobj = 0;
int g_out_i_w = 0, g_out_i_h = 0;
GLint g_iloc_scale = -1, g_iloc_obj = -1, g_iloc_w = -1;

const char* kIpFragment =
    "uniform sampler2D u_screen;\n"
    "uniform usampler2D u_runs;\n"
    "uniform int u_scale;\n"
    "uniform int u_obj;\n"
    "uniform int u_w;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    int s = u_scale;\n"
    "    int X = int(gl_FragCoord.x), Y = int(gl_FragCoord.y);\n"
    "    uvec2 r = texelFetch(u_runs, ivec2(X / s, Y / s), 0).rg;\n"
    "    if (u_obj != 0) {\n"
    "        o_color = r.x != 0u ? vec4(texelFetch(u_screen, ivec2(X, Y), 0).rgb, 1.0) : vec4(0.0);\n"
    "        return;\n"
    "    }\n"
    "    if (r.x == 0u) { o_color = vec4(texelFetch(u_screen, ivec2(X, Y), 0).rgb, 1.0); return; }\n"
    "    int ta = (int(r.x) - 1) * s, tb = int(r.y) * s, tm = (ta + tb) / 2;\n"
    "    vec3 cl = ta > 0 ? texelFetch(u_screen, ivec2(ta - 1, Y), 0).rgb\n"
    "                     : (tb < u_w ? texelFetch(u_screen, ivec2(tb, Y), 0).rgb : vec3(0.0));\n"
    "    vec3 cr = tb < u_w ? texelFetch(u_screen, ivec2(tb, Y), 0).rgb : cl;\n"
    "    o_color = vec4(X < tm ? cl : cr, 1.0);\n"
    "}\n";

const char* kScreenFragment =
    "uniform usampler2D u_base;\n"
    "uniform sampler2D u_center;\n"   /* centro HD da GPU (160N x 144N) */
    "uniform sampler2D u_margins;\n"  /* laterais HD da GPU (96N x 144N) */
    "uniform usampler2D u_amb;\n"
    "uniform int u_scale;\n"
    "uniform int u_wide;\n"
    "uniform int u_hdc;\n"
    "uniform int u_hdm;\n"
    "uniform int u_ambon;\n"
    "out vec4 o_color;\n"
    "uvec3 tex8(sampler2D t, ivec2 p) { return uvec3(round(texelFetch(t, p, 0).rgb * 255.0)); }\n"
    /* ws_amb_bilinear do platform_sdl.cpp */
    "uvec3 amb(int X, int Y, int s, int lo, int hi) {\n"
    "    int d = 2 * s;\n"
    "    int fy = clamp(2 * Y + 1 - s, 0, d * 143);\n"
    "    int y0 = fy / d, wy = fy - y0 * d;\n"
    "    int y1 = y0 + 1 < 144 ? y0 + 1 : y0;\n"
    "    int fx = clamp(2 * X + 1 - s, d * lo, d * hi);\n"
    "    int x0 = fx / d, wx = fx - x0 * d;\n"
    "    int x1 = x0 + 1 <= hi ? x0 + 1 : x0;\n"
    "    uint w00 = uint((d - wx) * (d - wy)), w01 = uint(wx * (d - wy));\n"
    "    uint w10 = uint((d - wx) * wy), w11 = uint(wx * wy);\n"
    "    uint den = uint(d * d) * 256u;\n"
    "    uvec3 v = (texelFetch(u_amb, ivec2(x0, y0), 0).rgb * w00 + texelFetch(u_amb, ivec2(x1, y0), 0).rgb * w01 +\n"
    "               texelFetch(u_amb, ivec2(x0, y1), 0).rgb * w10 + texelFetch(u_amb, ivec2(x1, y1), 0).rgb * w11 +\n"
    "               den / 2u) / den;\n"
    "    return min(v, uvec3(255u));\n"
    "}\n"
    "void main() {\n"
    "    int s = u_scale;\n"
    "    int X = int(gl_FragCoord.x), Y = int(gl_FragCoord.y);\n"
    "    int x = X / s, y = Y / s;\n"
    "    uvec4 b = texelFetch(u_base, ivec2(x, y), 0);\n"
    "    uvec3 c = uvec3(b.b, b.g, b.r);\n"
    "    int c0 = u_wide != 0 ? 48 * s : 0;\n"
    "    bool center = X >= c0 && X < c0 + 160 * s;\n"
    "    if (center) {\n"
    "        if (u_hdc != 0) c = tex8(u_center, ivec2(X - c0, Y));\n"
    "    } else if (u_wide != 0) {\n"
    "        int mx = X < c0 ? X : X - 160 * s;\n"
    "        if (u_hdm != 0) c = tex8(u_margins, ivec2(mx, Y));\n"
    "        if (u_ambon != 0 && s > 1) c = X < c0 ? amb(X, Y, s, 0, 47) : amb(X, Y, s, 208, 255);\n"
    "    }\n"
    "    o_color = vec4(vec3(c) / 255.0, 1.0);\n"
    "}\n";
bool g_has_tl = false, g_had_tl = false;
int g_atlas_side = 2048;
int g_atlas_layers = 0;
uint32_t g_atlas_gen = 0;  /* geração do pacote HD que está no atlas */
uint32_t g_atlas_tiles = 0;
bool g_atlas_ok = false;
std::string g_atlas_info = "sem tiles HD";

/* Triângulo que cobre a tela, sem buffer (gl_VertexID). */
const char* kVertex =
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* Mesmas regras do render_line/sample_map/rasterize_objects do ppu_native
 * (modo CGB), e no HD as de render_line com g_hd_scale > 1 (sample_hd_sub,
 * hd_recolor_setup/apply). A linha da tela é gl_FragCoord.y (a leitura de
 * volta começa na linha 0). Tudo em inteiros: cores e PNGs em texturas de
 * inteiros (bytes de 0xAARRGGBB: .r = azul, .g = verde, .b = vermelho). */
const char* kFragment =
    "uniform usampler2D u_vram;\n"   /* 256x64 R8UI: VRAM do início do quadro */
    "uniform usampler2D u_line;\n"   /* 2x144 RGBA8UI */
    "uniform usampler2D u_oam;\n"    /* 40x144 RGBA8UI */
    "uniform usampler2D u_pal;\n"    /* 64x144 RGBA8UI (bytes de 0xAARRGGBB) */
    "uniform usampler2D u_dirty;\n"  /* 256x64 RG16UI: (início + 1, quantidade) das escritas */
    "uniform usampler2D u_log;\n"    /* 256xN RG8UI: (linha + 1, valor), por endereço e na ordem */
    "uniform usampler2D u_slot;\n"   /* 768x16 R16UI: tile HD (slot + 1) por tile e (tipo, paleta) */
    "uniform usampler2D u_meta;\n"   /* 256x128 RGBA16UI */
    "uniform usampler2D u_ref;\n"    /* 1024x128 RGBA8UI */
    "uniform usampler2DArray u_atlas;\n"
    "uniform usampler2D u_tldir;\n" /* 768x1 RG16UI: tiles redesenhados no meio do quadro */
    "uniform usampler2D u_tl;\n"    /* 17xN R16UI: (linha + 1, 16 slots) */
    "uniform int u_hastl;\n"
    "uniform usampler2D u_mrec;\n"  /* 192x144 RGBA32UI: registros das laterais (passo 14b-4) */
    "uniform usampler2D u_fade;\n"  /* 256x96 R8UI: escurecimento da borda por coluna */
    "uniform int u_margin;\n"
    "uniform int u_haslog;\n"
    "uniform int u_scale;\n"
    "uniform int u_hd;\n"
    "out vec4 o_color;\n"
    "int g_y;\n"
    /* Byte da VRAM como estava quando a linha g_y foi desenhada (passo 14b-2). */
    "uint vr(int a) {\n"
    "    uint v = texelFetch(u_vram, ivec2(a & 255, a >> 8), 0).r;\n"
    "    if (u_haslog != 0) {\n"
    "        uvec2 d = texelFetch(u_dirty, ivec2(a & 255, a >> 8), 0).rg;\n"
    "        if (d.y != 0u) {\n"
    "            int s = int(d.x) - 1;\n"
    "            for (int k = 0; k < int(d.y); ++k) {\n"
    "                uvec2 e = texelFetch(u_log, ivec2((s + k) & 255, (s + k) >> 8), 0).rg;\n"
    "                if (int(e.x) <= g_y) v = e.y; else break;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    return v;\n"
    "}\n"
    "int tpix(int bank, int tile, int px, int py) {\n"
    "    int base = bank * 0x2000 + tile * 16 + py * 2;\n"
    "    uint lo = vr(base);\n"
    "    uint hi = vr(base + 1);\n"
    "    uint bit = uint(7 - px);\n"
    "    return int(((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1u));\n"
    "}\n"
    /* cor (R, G, B) da paleta da linha: índice 0-31 fundo, 32-63 sprites */
    "uvec3 pal(int ci, int y) { uvec4 t = texelFetch(u_pal, ivec2(ci, y), 0); return uvec3(t.b, t.g, t.r); }\n"
    /* tile HD (slot + 1) do tile gi na linha g_y: o do início do quadro e,
     * se o tile foi redesenhado no meio do quadro, a última troca até a linha */
    "uint slot_of(int kind, int pal_n, int gi) {\n"
    "    uint s = texelFetch(u_slot, ivec2(gi, kind * 8 + pal_n), 0).r;\n"
    "    if (u_hastl != 0) {\n"
    "        uvec2 d = texelFetch(u_tldir, ivec2(gi, 0), 0).rg;\n"
    "        for (int k = 0; k < int(d.y); ++k) {\n"
    "            int e = int(d.x) - 1 + k;\n"
    "            if (int(texelFetch(u_tl, ivec2(0, e), 0).r) <= g_y) s = texelFetch(u_tl, ivec2(1 + kind * 8 + pal_n, e), 0).r;\n"
    "            else break;\n"
    "        }\n"
    "    }\n"
    "    return s;\n"
    "}\n"
    /* Ponto do tile HD (slot + 1 = s1) para o subpixel (sx, sy) do pixel (u, v)
     * do tile na tela, com flips: mesma conta de sample_hd_sub. */
    "uvec4 hd_texel(uint s1, int u, int v, int flags, int sx, int sy, int n) {\n"
    "    int s = int(s1) - 1;\n"
    "    uvec4 m = texelFetch(u_meta, ivec2(s & 255, s >> 8), 0);\n"
    "    int span = 8 * n;\n"
    "    int cs = u * n + sx;\n"
    "    int rs = v * n + sy;\n"
    "    if ((flags & 0x20) != 0) cs = span - 1 - cs;\n"
    "    if ((flags & 0x40) != 0) rs = span - 1 - rs;\n"
    "    int size = int(m.a);\n"
    "    int hx = cs, hy = rs;\n"
    "    if (size != span) { hx = cs * size / span; hy = rs * size / span; }\n"
    "    return texelFetch(u_atlas, ivec3(int(m.r) + hx, int(m.g) + hy, int(m.b) & 0x7FFF), 0);\n"
    "}\n"
    /* Recoloração proporcional (hd_recolor_setup/apply) */
    "struct Rc { bool on; uvec3 mul; uvec3 add; };\n"
    "Rc rc_setup(uint s1, int idx, uvec3 cur) {\n"
    "    Rc r; r.on = false; r.mul = uvec3(65536u); r.add = uvec3(0u);\n"
    "    int s = int(s1) - 1;\n"
    "    uvec4 m = texelFetch(u_meta, ivec2(s & 255, s >> 8), 0);\n"
    "    if ((m.b & 0x8000u) == 0u) return r;\n"
    "    int ri = s * 4 + (idx & 3);\n"
    "    uvec4 t = texelFetch(u_ref, ivec2(ri & 1023, ri >> 10), 0);\n"
    "    uvec3 ref = uvec3(t.b, t.g, t.r);\n"
    "    if (ref == cur) return r;\n"
    "    r.on = true;\n"
    "    for (int c = 0; c < 3; ++c) {\n"
    "        if (ref[c] != 0u) { r.mul[c] = ((cur[c] << 16) + ref[c] - 1u) / ref[c]; r.add[c] = 0u; }\n"
    "        else { r.mul[c] = 65536u; r.add[c] = cur[c]; }\n"
    "    }\n"
    "    return r;\n"
    "}\n"
    "uvec3 rc_apply(Rc r, uvec4 h) {\n"
    "    uvec3 c = uvec3(h.b, h.g, h.r);\n"
    "    if (!r.on) return c;\n"
    "    return min(((c * r.mul) >> 16u) + r.add, uvec3(255u));\n"
    "}\n"
    /* Passo 14b-4: laterais, como margin_hd do ppu_native (registros de ppu.c) */
    "uvec3 unpack_rgb(uint v) { return uvec3((v >> 16) & 255u, (v >> 8) & 255u, v & 255u); }\n"
    "uvec3 fade3(bool on, int m, uvec3 c) {\n"
    "    if (!on) return c;\n"
    "    return uvec3(texelFetch(u_fade, ivec2(int(c.r), m), 0).r, texelFetch(u_fade, ivec2(int(c.g), m), 0).r,\n"
    "                 texelFetch(u_fade, ivec2(int(c.b), m), 0).r);\n"
    "}\n"
    "int uvf_flags(uint f) { return ((f & 0x40u) != 0u ? 0x20 : 0) | ((f & 0x80u) != 0u ? 0x40 : 0); }\n"
    "void margin_main() {\n"
    "    int n = u_scale;\n"
    "    int X = int(gl_FragCoord.x), Y = int(gl_FragCoord.y);\n"
    "    int m = X / n, y = Y / n;\n"
    "    int sx = X - m * n, sy = Y - y * n;\n"
    "    g_y = y;\n"
    "    uvec4 r = texelFetch(u_mrec, ivec2(2 * m, y), 0);\n"
    "    uvec4 hs = texelFetch(u_mrec, ivec2(2 * m + 1, y), 0);\n"
    "    uvec3 rgba = unpack_rgb(r.x);\n"
    "    if (u_hd == 0) { o_color = vec4(vec3(rgba) / 255.0, 1.0); return; }\n"
    "    bool fade = ((r.x >> 30) & 1u) != 0u;\n"
    "    uvec3 bg_rgba = unpack_rgb(r.y), obj_rgba = unpack_rgb(r.z);\n"
    "    uint bg_uvf = r.y >> 24, obj_uvf = r.z >> 24;\n"
    "    int bg_raw = int(r.w & 3u), obj_raw = int((r.w >> 2) & 3u);\n"
    "    uint bs = hs.x, os = hs.y;\n"   /* tile HD que a CPU achou para este ponto */
    "    bool ot = ((r.w >> 4) & 1u) != 0u;\n"
    "    if (obj_raw == 0 && os == 0u) ot = false;\n"   /* caixa sem HD: não muda nada */
    "    if (bs == 0u && os == 0u) { o_color = vec4(vec3(rgba) / 255.0, 1.0); return; }\n"
    "    uvec3 c = fade3(fade, m, bg_rgba);\n"
    "    if (bs != 0u) {\n"
    "        uvec4 h = hd_texel(bs, int(bg_uvf & 7u), int((bg_uvf >> 3) & 7u), uvf_flags(bg_uvf), sx, sy, n);\n"
    "        if (h.a != 0u) c = fade3(fade, m, rc_apply(rc_setup(bs, bg_raw, bg_rgba), h));\n"
    "    }\n"
    "    if (ot) {\n"
    "        if (os != 0u) {\n"
    "            uvec4 h = hd_texel(os, int(obj_uvf & 7u), int((obj_uvf >> 3) & 7u), uvf_flags(obj_uvf), sx, sy, n);\n"
    "            if (h.a >= 128u) c = rc_apply(rc_setup(os, obj_raw, obj_rgba), h);\n"
    "        } else {\n"
    "            c = obj_rgba;\n"
    "        }\n"
    "    }\n"
    "    o_color = vec4(vec3(c) / 255.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "    if (u_margin != 0) { margin_main(); return; }\n"
    "    int n = u_scale;\n"
    "    int X = int(gl_FragCoord.x), Y = int(gl_FragCoord.y);\n"
    "    int x = X / n, y = Y / n;\n"
    "    int sx = X - x * n, sy = Y - y * n;\n"
    "    g_y = y;\n"
    "    uvec4 l0 = texelFetch(u_line, ivec2(0, y), 0);\n"
    "    uvec4 l1 = texelFetch(u_line, ivec2(1, y), 0);\n"
    "    int lcdc = int(l0.r), scx = int(l0.g), scy = int(l0.b), wx = int(l0.a);\n"
    "    int wline = int(l1.r);\n"
    "    bool win = (l1.g & 1u) != 0u;\n"
    "    bool opri = l1.b != 0u;\n"
    /* fundo / janela (no CGB o fundo está sempre ligado) */
    "    int bsx; int bsy; int map;\n"
    "    if (win && x >= wx - 7) {\n"
    "        bsx = x - (wx - 7); bsy = wline; map = (lcdc & 0x40) != 0 ? 0x1C00 : 0x1800;\n"
    "    } else {\n"
    "        bsx = x + scx; bsy = y + scy; map = (lcdc & 0x08) != 0 ? 0x1C00 : 0x1800;\n"
    "    }\n"
    "    int entry = map + ((bsy >> 3) & 31) * 32 + ((bsx >> 3) & 31);\n"
    "    int num = int(vr(entry));\n"
    "    int attr = int(vr(0x2000 + entry));\n"
    "    int bu = bsx & 7, bv = bsy & 7;\n"
    "    int px = bu, py = bv;\n"
    "    if ((attr & 0x20) != 0) px = 7 - px;\n"
    "    if ((attr & 0x40) != 0) py = 7 - py;\n"
    "    int tidx = (lcdc & 0x10) != 0 ? num : (num < 128 ? 256 + num : num);\n"
    "    int bbank = (attr & 0x08) != 0 ? 1 : 0;\n"
    "    int bgraw = tpix(bbank, tidx, px, py);\n"
    "    int bgpal = attr & 7;\n"
    "    bool bgpri = (attr & 0x80) != 0;\n"
    "    int bgi = bbank * 384 + tidx;\n"
    /* sprites: todos os 40; vencedor e a primeira caixa transparente (HD) */
    "    int oraw = 0, opal = 0, osx = 999, oidx = 99, ou = 0, ov = 0, ofl = 0, ogi = 0;\n"
    "    bool obehind = false;\n"
    "    bool box = false;\n"
    "    int xu = 0, xv = 0, xfl = 0, xpal = 0, xgi = 0;\n"
    "    bool xbehind = false;\n"
    "    if ((lcdc & 0x02) != 0) {\n"
    "        int h = (lcdc & 0x04) != 0 ? 16 : 8;\n"
    "        for (int i = 0; i < 40; ++i) {\n"
    "            uvec4 o = texelFetch(u_oam, ivec2(i, y), 0);\n"
    "            int spy = int(o.r) - 16;\n"
    "            if (y < spy || y >= spy + h) continue;\n"
    "            int spx = int(o.g) - 8;\n"
    "            if (spx <= -8 || spx >= 160) continue;\n"
    "            int p = x - spx;\n"
    "            if (p < 0 || p >= 8) continue;\n"
    "            int fl = int(o.a);\n"
    "            int line = y - spy;\n"
    "            if ((fl & 0x40) != 0) line = h - 1 - line;\n"
    "            int t = h == 16 ? ((int(o.b) & 0xFE) + (line >> 3)) : int(o.b);\n"
    "            int bank = (fl & 0x08) != 0 ? 1 : 0;\n"
    "            int raw = tpix(bank, t, (fl & 0x20) != 0 ? 7 - p : p, line & 7);\n"
    "            if (raw == 0) {\n"
    "                if (!box) { box = true; xu = p; xv = (y - spy) & 7; xfl = fl & 0x60; xpal = fl & 7;\n"
    "                            xbehind = (fl & 0x80) != 0; xgi = bank * 384 + t; }\n"
    "                continue;\n"
    "            }\n"
    "            if (oraw != 0) {\n"
    "                bool wins = opri ? (spx < osx || (spx == osx && i < oidx)) : (i < oidx);\n"
    "                if (!wins) continue;\n"
    "            }\n"
    "            oraw = raw; opal = fl & 7; obehind = (fl & 0x80) != 0; osx = spx; oidx = i;\n"
    "            ou = p; ov = (y - spy) & 7; ofl = fl & 0x60; ogi = bank * 384 + t;\n"
    "        }\n"
    "    }\n"
    "    bool useobj = oraw != 0;\n"
    "    if (useobj && bgraw != 0 && (lcdc & 0x01) != 0 && (bgpri || obehind)) useobj = false;\n"
    "    uvec3 c1 = useobj ? pal(32 + opal * 4 + oraw, y) : pal(bgpal * 4 + bgraw, y);\n"
    "    if (u_hd == 0) { o_color = vec4(vec3(c1) / 255.0, 1.0); return; }\n"
    /* HD (render_line com g_hd_scale > 1) */
    "    uint bs = slot_of(0, bgpal, bgi);\n"
    "    uint os = 0u;\n"
    "    bool ot = false;\n"
    "    int cu = 0, cv = 0, cfl = 0, cpal = 0;\n"
    "    bool cbehind = false;\n"
    "    if (oraw != 0) {\n"
    "        ot = true; cu = ou; cv = ov; cfl = ofl; cpal = opal; cbehind = obehind;\n"
    "        os = slot_of(1, opal, ogi);\n"
    "    } else if (box) {\n"
    "        os = slot_of(1, xpal, xgi);\n"
    "        if (os != 0u) { ot = true; cu = xu; cv = xv; cfl = xfl; cpal = xpal; cbehind = xbehind; }\n"
    "    }\n"
    "    if (bs == 0u && os == 0u) { o_color = vec4(vec3(c1) / 255.0, 1.0); return; }\n"
    "    uvec3 bg_orig = pal(bgpal * 4 + bgraw, y);\n"
    "    uvec3 obj_orig = oraw != 0 ? pal(32 + cpal * 4 + oraw, y) : uvec3(0u);\n"
    "    bool blocked = bgraw != 0 && (lcdc & 0x01) != 0 && (bgpri || cbehind);\n"
    "    bool can_win = ot && !blocked;\n"
    "    uvec3 c = bg_orig;\n"
    "    if (bs != 0u) {\n"
    "        uvec4 hb = hd_texel(bs, bu, bv, attr & 0x60, sx, sy, n);\n"
    "        if (hb.a != 0u) c = rc_apply(rc_setup(bs, bgraw, bg_orig), hb);\n"
    "    }\n"
    "    if (can_win) {\n"
    "        if (os != 0u) {\n"
    "            uvec4 ho = hd_texel(os, cu, cv, cfl, sx, sy, n);\n"
    "            if (ho.a >= 128u) c = rc_apply(rc_setup(os, oraw, pal(32 + cpal * 4 + oraw, y)), ho);\n"
    "        } else if (oraw != 0) {\n"
    "            c = obj_orig;\n"
    "        }\n"
    "    }\n"
    "    o_color = vec4(vec3(c) / 255.0, 1.0);\n"
    "}\n";

GLuint make_tex(GLenum internal, int w, int h, GLenum format, GLenum type) {
    GLuint t = 0;
    gl.GenTextures(1, &t);
    gl.BindTexture(kTexture2D, t);
    gl.TexParameteri(kTexture2D, kTexMinFilter, (GLint)kNearest);
    gl.TexParameteri(kTexture2D, kTexMagFilter, (GLint)kNearest);
    gl.TexParameteri(kTexture2D, kTexWrapS, (GLint)kClampToEdge);
    gl.TexParameteri(kTexture2D, kTexWrapT, (GLint)kClampToEdge);
    gl.TexImage2D(kTexture2D, 0, (GLint)internal, w, h, 0, format, type, nullptr);
    return t;
}

void upload(GLuint tex, int w, int h, GLenum format, GLenum type, const void* data) {
    gl.BindTexture(kTexture2D, tex);
    gl.TexSubImage2D(kTexture2D, 0, 0, 0, w, h, format, type, data);
}

bool make_target(GLuint* fbo, GLuint* tex, int w, int h) {
    *tex = make_tex(kRGBA8, w, h, kRGBA, kUnsignedByte);
    gl.GenFramebuffers(1, fbo);
    gl.BindFramebuffer(kFramebuffer, *fbo);
    gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, *tex, 0);
    const GLenum status = gl.CheckFramebufferStatus(kFramebuffer);
    gl.BindFramebuffer(kFramebuffer, 0);
    return status == kFramebufferComplete;
}

/* Passo 14b-2: organiza o registro por endereço (contagem + soma acumulada),
 * mantendo a ordem das escritas de cada endereço. */
bool build_log(const PPUNativeGpuFrame* f) {
    for (uint32_t a : g_dirty_addrs) {   /* limpa só o que o quadro anterior sujou */
        g_dirty[a * 2] = 0;
        g_dirty[a * 2 + 1] = 0;
        g_count[a] = 0;
    }
    g_dirty_addrs.clear();
    const uint32_t n = f->log_count;
    for (uint32_t i = 0; i < n; ++i) {
        const uint16_t a = f->log[i].addr & 0x3FFFu;
        if (g_count[a]++ == 0) g_dirty_addrs.push_back(a);
    }
    uint32_t start = 0;
    for (uint32_t a : g_dirty_addrs) {
        g_dirty[a * 2] = (uint16_t)(start + 1);
        g_dirty[a * 2 + 1] = g_count[a];
        start += g_count[a];
        g_count[a] = 0;   /* vira o cursor de preenchimento */
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint16_t a = f->log[i].addr & 0x3FFFu;
        const uint32_t pos = (uint32_t)g_dirty[a * 2] - 1u + g_count[a]++;
        g_logtex[pos * 2] = f->log[i].line_plus1;
        g_logtex[pos * 2 + 1] = f->log[i].value;
    }
    return n > 0;
}

/* Passo 14b-3: monta o atlas quando o pacote de tiles HD muda. Prateleiras
 * em camadas de g_atlas_side x g_atlas_side. */
/* Passo 14c-1: erros do OpenGL deixados por outro código (a apresentação, o
 * ImGui) não podem ser contados como nossos: limpa antes de começar e avisa
 * uma vez no terminal com o código e onde foi notado. */
void drain_errors(const char* where) {
    static bool warned = false;
    for (int i = 0; i < 16; ++i) {
        const GLenum e = gl.GetError();
        if (e == 0) break;
        if (!warned) {
            warned = true;
            std::fprintf(stderr, "[GPU] erro do OpenGL de antes (0x%04X), notado em %s; ignorado\n", e, where);
        }
    }
}

void rebuild_atlas(void) {
    const uint32_t gen = ppu_native_hd_pack_generation();
    if (g_atlas_ok && gen == g_atlas_gen) return;
    g_atlas_gen = gen;
    g_atlas_ok = false;
    drain_errors("atlas");
    g_atlas_tiles = 0;
    const uint32_t cap = ppu_native_hd_table_capacity();
    struct Item { uint32_t slot; int size; const uint32_t* px; const uint32_t* ref; };
    std::vector<Item> items;
    for (uint32_t i = 0; i < cap; ++i) {
        Item it{i, 0, nullptr, nullptr};
        if (ppu_native_hd_table_entry(i, &it.size, &it.px, &it.ref)) items.push_back(it);
    }
    std::vector<uint16_t> meta((size_t)256 * 128 * 4, 0);
    std::vector<uint32_t> ref((size_t)1024 * 128, 0);
    /* maiores primeiro: prateleiras mais cheias */
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.size > b.size; });
    struct Pos { int x, y, layer; };
    std::vector<Pos> pos(items.size());
    int layer = 0, x = 0, y = 0, shelf = 0;
    for (size_t k = 0; k < items.size(); ++k) {
        const int s = items[k].size;
        if (x + s > g_atlas_side) { x = 0; y += shelf; shelf = 0; }
        if (y + s > g_atlas_side) { layer++; x = 0; y = 0; shelf = 0; }
        pos[k] = {x, y, layer};
        x += s;
        if (s > shelf) shelf = s;
    }
    const int layers = items.empty() ? 1 : layer + 1;
    GLint max_layers = 256;
    gl.GetIntegerv(kMaxArrayTextureLayers, &max_layers);
    if (layers > max_layers) {
        g_atlas_info = "tiles HD demais para o atlas da GPU";
        return;
    }
    gl.ActiveTexture(kTexture0 + 9);
    if (g_tex_atlas) gl.DeleteTextures(1, &g_tex_atlas);
    gl.GenTextures(1, &g_tex_atlas);
    gl.BindTexture(kTexture2DArray, g_tex_atlas);
    gl.TexParameteri(kTexture2DArray, kTexMinFilter, (GLint)kNearest);
    gl.TexParameteri(kTexture2DArray, kTexMagFilter, (GLint)kNearest);
    gl.TexImage3D(kTexture2DArray, 0, (GLint)kRGBA8UI, g_atlas_side, g_atlas_side, layers, 0,
                  kRGBAInteger, kUnsignedByte, nullptr);
    gl.PixelStorei(kUnpackAlignment, 4);
    gl.PixelStorei(kUnpackRowLength, 0);
    for (size_t k = 0; k < items.size(); ++k) {
        const Item& it = items[k];
        gl.TexSubImage3D(kTexture2DArray, 0, pos[k].x, pos[k].y, pos[k].layer, it.size, it.size, 1,
                         kRGBAInteger, kUnsignedByte, it.px);
        uint16_t* m = &meta[(size_t)it.slot * 4];
        m[0] = (uint16_t)pos[k].x;
        m[1] = (uint16_t)pos[k].y;
        m[2] = (uint16_t)(pos[k].layer | (it.ref ? 0x8000 : 0));
        m[3] = (uint16_t)it.size;
        if (it.ref) {
            for (int c = 0; c < 4; ++c) ref[(size_t)it.slot * 4 + c] = it.ref[c];
        }
    }
    gl.ActiveTexture(kTexture0 + 7);
    upload(g_tex_meta, 256, 128, kRGBAInteger, kUnsignedShort, meta.data());
    gl.ActiveTexture(kTexture0 + 8);
    upload(g_tex_ref, 1024, 128, kRGBAInteger, kUnsignedByte, ref.data());
    gl.ActiveTexture(kTexture0);
    g_atlas_layers = layers;
    g_atlas_tiles = (uint32_t)items.size();
    g_atlas_ok = gl.GetError() == 0;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%u tiles HD no atlas (%d camada%s de %dx%d)", g_atlas_tiles,
                  layers, layers == 1 ? "" : "s", g_atlas_side, g_atlas_side);
    g_atlas_info = g_atlas_ok ? buf : "erro ao montar o atlas";
    std::fprintf(stderr, "[GPU] %s\n", g_atlas_info.c_str());
}

/* Manda os dados do quadro (uma vez por quadro, mesmo com 1x e HD). */
bool upload_frame(const PPUNativeGpuFrame* f) {
    if (f->serial == g_uploaded_serial) return true;
    gl.PixelStorei(kUnpackAlignment, 1);
    gl.PixelStorei(kUnpackRowLength, 0);
    gl.ActiveTexture(kTexture0 + 0);
    upload(g_tex_vram, 256, 64, kRedInteger, kUnsignedByte, f->vram);
    gl.ActiveTexture(kTexture0 + 1);
    upload(g_tex_line, 2, kH, kRGBAInteger, kUnsignedByte, f->line);
    gl.ActiveTexture(kTexture0 + 2);
    upload(g_tex_oam, 40, kH, kRGBAInteger, kUnsignedByte, f->oam);
    gl.ActiveTexture(kTexture0 + 3);
    upload(g_tex_pal, 64, kH, kRGBAInteger, kUnsignedByte, f->pal);
    g_has_log = build_log(f);
    gl.ActiveTexture(kTexture0 + 4);
    if (g_has_log || g_had_log) {   /* manda de novo quando muda (inclusive para limpar) */
        upload(g_tex_dirty, 256, 64, kRGInteger, kUnsignedShort, g_dirty.data());
    }
    gl.ActiveTexture(kTexture0 + 5);
    if (g_has_log) {
        const int rows = (int)((f->log_count + kLogW - 1) / kLogW);
        gl.BindTexture(kTexture2D, g_tex_log);
        gl.TexSubImage2D(kTexture2D, 0, 0, 0, kLogW, rows, kRGInteger, kUnsignedByte, g_logtex.data());
    }
    g_had_log = g_has_log;
    if (f->hd_on) {
        gl.ActiveTexture(kTexture0 + 6);
        upload(g_tex_slot, 768, 16, kRedInteger, kUnsignedShort, f->hd_slot);
        g_has_tl = f->hd_tl_count > 0;
        gl.ActiveTexture(kTexture0 + 10);
        if (g_has_tl || g_had_tl) upload(g_tex_tldir, 768, 1, kRGInteger, kUnsignedShort, f->hd_tl_dir);
        if (g_has_tl) {
            gl.ActiveTexture(kTexture0 + 11);
            upload(g_tex_tl, 17, (int)f->hd_tl_count, kRedInteger, kUnsignedShort, f->hd_tl);
        }
        g_had_tl = g_has_tl;
    } else {
        g_has_tl = false;
    }
    gl.PixelStorei(kUnpackAlignment, 4);
    gl.ActiveTexture(kTexture0);
    g_uploaded_serial = f->serial;
    return true;
}

void bind_inputs(void) {
    const GLenum targets[] = {kTexture2D, kTexture2D, kTexture2D, kTexture2D, kTexture2D, kTexture2D,
                              kTexture2D, kTexture2D, kTexture2D, kTexture2DArray, kTexture2D, kTexture2D,
                              kTexture2D, kTexture2D};
    const GLuint texs[] = {g_tex_vram, g_tex_line, g_tex_oam, g_tex_pal, g_tex_dirty, g_tex_log,
                           g_tex_slot, g_tex_meta, g_tex_ref, g_tex_atlas, g_tex_tldir, g_tex_tl,
                           g_tex_mrec, g_tex_fade};
    for (int i = 0; i < 14; ++i) {
        gl.ActiveTexture(kTexture0 + (GLenum)i);
        gl.BindTexture(targets[i], texs[i]);
    }
    gl.ActiveTexture(kTexture0);
}

bool draw(GLuint fbo, int w, int h, int scale, bool hd, bool margin = false) {
    gl.BindFramebuffer(kFramebuffer, fbo);
    gl.Viewport(0, 0, w, h);
    gl.Disable(kBlend);
    gl.Disable(kScissorTest);
    gl.Disable(kDepthTest);
    gl.Disable(kCullFace);
    gl.UseProgram(g_prog);
    bind_inputs();
    gl.Uniform1i(g_loc_haslog, g_has_log ? 1 : 0);
    gl.Uniform1i(g_loc_scale, scale);
    gl.Uniform1i(g_loc_hd, hd ? 1 : 0);
    gl.Uniform1i(g_loc_hastl, (hd && g_has_tl) ? 1 : 0);
    gl.Uniform1i(g_loc_margin, margin ? 1 : 0);
    gl.BindVertexArray(g_vao);
    gl.DrawArrays(kTriangles, 0, 3);
    gl.BindFramebuffer(kFramebuffer, 0);
    return gl.GetError() == 0;
}

bool read_target(GLuint fbo, int w, int h, uint32_t* argb) {
    std::vector<unsigned char> rgba((size_t)w * h * 4);
    gl.BindFramebuffer(kFramebuffer, fbo);
    gl.PixelStorei(kPackAlignment, 1);
    gl.ReadPixels(0, 0, w, h, kRGBA, kUnsignedByte, rgba.data());
    gl.BindFramebuffer(kFramebuffer, 0);
    for (size_t i = 0; i < (size_t)w * h; ++i) {   /* linha 0 do GL = linha 0 da tela */
        const unsigned char* p = &rgba[i * 4];
        argb[i] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    return gl.GetError() == 0;
}

}  // namespace

bool gpu_ppu_init(char* err, size_t err_size) {
    if (g_ready) return true;
    auto fail = [&](const std::string& why) {
        if (err && err_size) std::snprintf(err, err_size, "%s", why.c_str());
        gpu_ppu_shutdown();
        return false;
    };
    if (!vgl_ready()) return fail("precisa do video OpenGL");
    drain_errors("init");
    std::string e;
    /* no GLES o usampler2DArray precisa de precisão declarada */
    const std::string head = std::string(shader_header()) +
                             (std::strstr(shader_header(), " es") ? "precision highp usampler2DArray;\n" : "");
    g_prog = link_program(head + kVertex, head + kFragment, nullptr, 0, e);
    if (!g_prog) return fail(e);
    gl.UseProgram(g_prog);
    const char* names[] = {"u_vram", "u_line", "u_oam", "u_pal", "u_dirty", "u_log",
                           "u_slot", "u_meta", "u_ref", "u_atlas", "u_tldir", "u_tl",
                           "u_mrec", "u_fade"};
    for (int i = 0; i < 14; ++i) gl.Uniform1i(gl.GetUniformLocation(g_prog, names[i]), i);
    g_loc_haslog = gl.GetUniformLocation(g_prog, "u_haslog");
    g_loc_scale = gl.GetUniformLocation(g_prog, "u_scale");
    g_loc_hd = gl.GetUniformLocation(g_prog, "u_hd");
    g_loc_hastl = gl.GetUniformLocation(g_prog, "u_hastl");
    g_loc_margin = gl.GetUniformLocation(g_prog, "u_margin");
    gl.GenVertexArrays(1, &g_vao);
    gl.PixelStorei(kUnpackAlignment, 1);
    gl.PixelStorei(kUnpackRowLength, 0);
    g_tex_vram = make_tex(kR8UI, 256, 64, kRedInteger, kUnsignedByte);
    g_tex_line = make_tex(kRGBA8UI, 2, kH, kRGBAInteger, kUnsignedByte);
    g_tex_oam = make_tex(kRGBA8UI, 40, kH, kRGBAInteger, kUnsignedByte);
    g_tex_pal = make_tex(kRGBA8UI, 64, kH, kRGBAInteger, kUnsignedByte);
    g_tex_dirty = make_tex(kRG16UI, 256, 64, kRGInteger, kUnsignedShort);
    g_tex_log = make_tex(kRG8UI, kLogW, kLogH, kRGInteger, kUnsignedByte);
    g_tex_slot = make_tex(kR16UI, 768, 16, kRedInteger, kUnsignedShort);
    g_tex_meta = make_tex(kRGBA16UI, 256, 128, kRGBAInteger, kUnsignedShort);
    g_tex_ref = make_tex(kRGBA8UI, 1024, 128, kRGBAInteger, kUnsignedByte);
    g_tex_tldir = make_tex(kRG16UI, 768, 1, kRGInteger, kUnsignedShort);
    g_tex_tl = make_tex(kR16UI, 17, PPU_NATIVE_GPU_TL_MAX, kRedInteger, kUnsignedShort);
    g_tex_mrec = make_tex(kRGBA32UI, kMW * 2, kH, kRGBAInteger, kUnsignedInt);
    g_tex_fade = make_tex(kR8UI, 256, kMW, kRedInteger, kUnsignedByte);
    g_margin_serial = 0xFFFFFFFFu;
    g_dirty.assign(0x4000 * 2, 0);
    g_logtex.assign((size_t)kLogW * kLogH * 2, 0);
    g_count.assign(0x4000, 0);
    upload(g_tex_dirty, 256, 64, kRGInteger, kUnsignedShort, g_dirty.data());
    {
        std::vector<uint16_t> zero((size_t)768 * 16, 0);
        upload(g_tex_slot, 768, 16, kRedInteger, kUnsignedShort, zero.data());
        upload(g_tex_tldir, 768, 1, kRGInteger, kUnsignedShort, zero.data());
    }
    g_has_tl = g_had_tl = false;
    gl.PixelStorei(kUnpackAlignment, 4);
    GLint max_side = 2048;
    gl.GetIntegerv(kMaxTextureSize, &max_side);
    g_atlas_side = max_side >= 2048 ? 2048 : max_side;
    if (!make_target(&g_fbo, &g_out, kW, kH)) return fail("framebuffer da GPU incompleto");
    /* atlas vazio até ter tiles HD */
    g_atlas_gen = 0;
    g_atlas_ok = false;
    rebuild_atlas();
    if (gl.GetError() != 0) return fail("erro do OpenGL ao preparar a GPU");
    g_uploaded_serial = 0xFFFFFFFFu;
    g_ready = true;
    std::fprintf(stderr, "[GPU] Centro pela GPU pronto (shader do passo 14b-3).\n");
    return true;
}

void gpu_ppu_shutdown(void) {
    if (vgl_ready() && gl.DeleteTextures) {
        const GLuint texs[] = {g_tex_vram, g_tex_line, g_tex_oam, g_tex_pal, g_out, g_out_hd,
                               g_tex_dirty, g_tex_log, g_tex_slot, g_tex_meta, g_tex_ref, g_tex_atlas,
                               g_tex_tldir, g_tex_tl, g_tex_mrec, g_tex_fade, g_out_m,
                               g_tex_sbase, g_tex_samb, g_out_s, g_tex_iruns, g_out_ibg, g_out_iobj,
                               g_out_f};
        for (GLuint t : texs) if (t) gl.DeleteTextures(1, &t);
        if (g_fbo) gl.DeleteFramebuffers(1, &g_fbo);
        if (g_fbo_hd) gl.DeleteFramebuffers(1, &g_fbo_hd);
        if (g_fbo_m) gl.DeleteFramebuffers(1, &g_fbo_m);
        if (g_fbo_s) gl.DeleteFramebuffers(1, &g_fbo_s);
        if (g_sprog) gl.DeleteProgram(g_sprog);
        if (g_iprog) gl.DeleteProgram(g_iprog);
        if (g_fprog) gl.DeleteProgram(g_fprog);
        if (g_fbo_f) gl.DeleteFramebuffers(1, &g_fbo_f);
        if (g_fbo_ibg) gl.DeleteFramebuffers(1, &g_fbo_ibg);
        if (g_fbo_iobj) gl.DeleteFramebuffers(1, &g_fbo_iobj);
        if (g_vao) gl.DeleteVertexArrays(1, &g_vao);
        if (g_prog) gl.DeleteProgram(g_prog);
    }
    g_tex_vram = g_tex_line = g_tex_oam = g_tex_pal = g_out = g_out_hd = 0;
    g_tex_dirty = g_tex_log = g_tex_slot = g_tex_meta = g_tex_ref = g_tex_atlas = 0;
    g_tex_tldir = g_tex_tl = 0;
    g_tex_mrec = g_tex_fade = g_out_m = g_fbo_m = 0;
    g_sprog = g_tex_sbase = g_tex_samb = g_fbo_s = g_out_s = 0;
    g_iprog = g_tex_iruns = g_fbo_ibg = g_out_ibg = g_fbo_iobj = g_out_iobj = 0;
    g_out_i_w = g_out_i_h = 0;
    g_fprog = g_fbo_f = g_out_f = g_final = 0;
    g_out_f_w = g_out_f_h = 0;
    g_out_s_w = g_out_s_h = 0;
    g_out_m_scale = 0;
    g_fbo = g_fbo_hd = g_vao = g_prog = 0;
    g_out_hd_scale = 0;
    g_dirty_addrs.clear();
    g_atlas_ok = false;
    g_ready = false;
}

bool gpu_ppu_ready(void) {
    return g_ready;
}

bool gpu_ppu_compose(const PPUNativeGpuFrame* f) {
    if (!g_ready || !f || !f->cgb || f->log_overflow) return false;
    drain_errors("gpu_ppu_compose");
    upload_frame(f);
    return draw(g_fbo, kW, kH, 1, false);
}

bool gpu_ppu_read(uint32_t* argb) {
    if (!g_ready || !argb) return false;
    return read_target(g_fbo, kW, kH, argb);
}

bool gpu_ppu_compose_hd(const PPUNativeGpuFrame* f) {
    if (!g_ready || !f || !f->cgb || f->log_overflow) return false;
    drain_errors("gpu_ppu_compose_hd");
    const int n = f->hd_scale;
    if (n < 2 || n > kMaxScale) return false;
    if (n != g_out_hd_scale) {
        if (g_out_hd) gl.DeleteTextures(1, &g_out_hd);
        if (g_fbo_hd) gl.DeleteFramebuffers(1, &g_fbo_hd);
        g_out_hd = g_fbo_hd = 0;
        if (!make_target(&g_fbo_hd, &g_out_hd, kW * n, kH * n)) return false;
        g_out_hd_scale = n;
    }
    if (f->hd_on) rebuild_atlas();
    upload_frame(f);
    return draw(g_fbo_hd, kW * n, kH * n, n, f->hd_on && g_atlas_ok);
}

bool gpu_ppu_read_hd(uint32_t* argb, int* scale) {
    if (!g_ready || !argb || g_out_hd_scale < 2) return false;
    if (scale) *scale = g_out_hd_scale;
    return read_target(g_fbo_hd, kW * g_out_hd_scale, kH * g_out_hd_scale, argb);
}

/* Passo 14b-4: laterais HD (96*N x 144*N), dos registros de ppu.c. */
bool gpu_ppu_compose_margins_hd(const PPUNativeGpuFrame* f) {
    if (!g_ready || !f || !f->cgb || f->log_overflow || !f->margin_on) return false;
    drain_errors("gpu_ppu_compose_margins_hd");
    const int n = f->hd_scale;
    if (n < 2 || n > kMaxScale) return false;
    if (n != g_out_m_scale) {
        if (g_out_m) gl.DeleteTextures(1, &g_out_m);
        if (g_fbo_m) gl.DeleteFramebuffers(1, &g_fbo_m);
        g_out_m = g_fbo_m = 0;
        if (!make_target(&g_fbo_m, &g_out_m, kMW * n, kH * n)) return false;
        g_out_m_scale = n;
    }
    if (f->hd_on) rebuild_atlas();
    upload_frame(f);
    if (g_margin_serial != f->serial) {
        gl.PixelStorei(kUnpackAlignment, 1);
        gl.PixelStorei(kUnpackRowLength, 0);
        gl.ActiveTexture(kTexture0 + 12);
        upload(g_tex_mrec, kMW * 2, kH, kRGBAInteger, kUnsignedInt, f->margin);
        gl.ActiveTexture(kTexture0 + 13);
        upload(g_tex_fade, 256, kMW, kRedInteger, kUnsignedByte, f->fade_lut);
        gl.ActiveTexture(kTexture0);
        gl.PixelStorei(kUnpackAlignment, 4);
        g_margin_serial = f->serial;
    }
    return draw(g_fbo_m, kMW * n, kH * n, n, f->hd_on && g_atlas_ok, true);
}

bool gpu_ppu_read_margins_hd(uint32_t* argb, int* scale) {
    if (!g_ready || !argb || g_out_m_scale < 2) return false;
    if (scale) *scale = g_out_m_scale;
    return read_target(g_fbo_m, kMW * g_out_m_scale, kH * g_out_m_scale, argb);
}

/* Passo 14c-1 */
bool gpu_ppu_compose_screen(const GpuScreenInput* in) {
    if (!g_ready || !in || !in->base || in->scale < 1 || in->scale > kMaxScale) return false;
    const int bw = in->wide ? 256 : 160;
    const int w = bw * in->scale, h = kH * in->scale;
    /* HD de outra escala (troca no meio): fica a base ampliada, como na CPU */
    const bool hd_center = in->hd_center && g_out_hd_scale == in->scale;
    const bool hd_margins = hd_center && in->hd_margins && g_out_m_scale == in->scale;
    if (!g_sprog) {
        std::string e;
        g_sprog = link_program(std::string(shader_header()) + kVertex,
                               std::string(shader_header()) + kScreenFragment, nullptr, 0, e);
        if (!g_sprog) {
            std::fprintf(stderr, "[GPU] shader da tela: %s\n", e.c_str());
            return false;
        }
        gl.UseProgram(g_sprog);
        gl.Uniform1i(gl.GetUniformLocation(g_sprog, "u_base"), 0);
        gl.Uniform1i(gl.GetUniformLocation(g_sprog, "u_center"), 1);
        gl.Uniform1i(gl.GetUniformLocation(g_sprog, "u_margins"), 2);
        gl.Uniform1i(gl.GetUniformLocation(g_sprog, "u_amb"), 3);
        g_sloc_scale = gl.GetUniformLocation(g_sprog, "u_scale");
        g_sloc_wide = gl.GetUniformLocation(g_sprog, "u_wide");
        g_sloc_hdc = gl.GetUniformLocation(g_sprog, "u_hdc");
        g_sloc_hdm = gl.GetUniformLocation(g_sprog, "u_hdm");
        g_sloc_amb = gl.GetUniformLocation(g_sprog, "u_ambon");
        gl.ActiveTexture(kTexture0);
        g_tex_sbase = make_tex(kRGBA8UI, 256, kH, kRGBAInteger, kUnsignedByte);
        g_tex_samb = make_tex(kRGBA16UI, 256, kH, kRGBAInteger, kUnsignedShort);
    }
    if (w != g_out_s_w || h != g_out_s_h) {
        if (g_out_s) gl.DeleteTextures(1, &g_out_s);
        if (g_fbo_s) gl.DeleteFramebuffers(1, &g_fbo_s);
        g_out_s = g_fbo_s = 0;
        g_out_s_w = g_out_s_h = 0;
        if (!make_target(&g_fbo_s, &g_out_s, w, h)) return false;
        g_out_s_w = w;
        g_out_s_h = h;
    }
    drain_errors("tela");
    gl.PixelStorei(kUnpackAlignment, 1);
    gl.PixelStorei(kUnpackRowLength, 0);
    gl.ActiveTexture(kTexture0);
    upload(g_tex_sbase, bw, kH, kRGBAInteger, kUnsignedByte, in->base);
    if (in->amb_q) {
        gl.ActiveTexture(kTexture0 + 3);
        upload(g_tex_samb, 256, kH, kRGBAInteger, kUnsignedShort, in->amb_q);
    }
    gl.PixelStorei(kUnpackAlignment, 4);
    gl.BindFramebuffer(kFramebuffer, g_fbo_s);
    gl.Viewport(0, 0, w, h);
    gl.Disable(kBlend);
    gl.Disable(kScissorTest);
    gl.Disable(kDepthTest);
    gl.Disable(kCullFace);
    gl.UseProgram(g_sprog);
    const GLuint texs[] = {g_tex_sbase, g_out_hd, g_out_m, g_tex_samb};
    for (int i = 0; i < 4; ++i) {
        gl.ActiveTexture(kTexture0 + (GLenum)i);
        gl.BindTexture(kTexture2D, texs[i]);
    }
    gl.ActiveTexture(kTexture0);
    gl.Uniform1i(g_sloc_scale, in->scale);
    gl.Uniform1i(g_sloc_wide, in->wide ? 1 : 0);
    gl.Uniform1i(g_sloc_hdc, hd_center ? 1 : 0);
    gl.Uniform1i(g_sloc_hdm, hd_margins ? 1 : 0);
    gl.Uniform1i(g_sloc_amb, in->amb_q ? 1 : 0);
    gl.BindVertexArray(g_vao);
    gl.DrawArrays(kTriangles, 0, 3);
    gl.BindFramebuffer(kFramebuffer, 0);
    g_final = g_out_s;   /* passo 14e-1: até um filtro ser aplicado */
    return gl.GetError() == 0;
}

/* Passo 14e-1 */
bool gpu_ppu_apply_filter(int mode) {
    if (!g_ready || !g_out_s) return false;
    if (mode <= 0) {
        g_final = g_out_s;
        return true;
    }
    const int w = g_out_s_w, h = g_out_s_h;
    drain_errors("filtro");
    if (!g_fprog) {
        std::string e;
        g_fprog = link_program(std::string(shader_header()) + kVertex,
                               std::string(shader_header()) + kFilterFragment, nullptr, 0, e);
        if (!g_fprog) {
            std::fprintf(stderr, "[GPU] shader dos filtros: %s\n", e.c_str());
            return false;
        }
        gl.UseProgram(g_fprog);
        gl.Uniform1i(gl.GetUniformLocation(g_fprog, "u_src"), 0);
        g_floc_mode = gl.GetUniformLocation(g_fprog, "u_mode");
        g_floc_size = gl.GetUniformLocation(g_fprog, "u_size");
    }
    if (w != g_out_f_w || h != g_out_f_h) {
        if (g_out_f) gl.DeleteTextures(1, &g_out_f);
        if (g_fbo_f) gl.DeleteFramebuffers(1, &g_fbo_f);
        g_out_f = g_fbo_f = 0;
        g_out_f_w = g_out_f_h = 0;
        if (!make_target(&g_fbo_f, &g_out_f, w, h)) return false;
        g_out_f_w = w;
        g_out_f_h = h;
    }
    gl.BindFramebuffer(kFramebuffer, g_fbo_f);
    gl.Viewport(0, 0, w, h);
    gl.Disable(kBlend);
    gl.Disable(kScissorTest);
    gl.Disable(kDepthTest);
    gl.Disable(kCullFace);
    gl.UseProgram(g_fprog);
    gl.ActiveTexture(kTexture0);
    gl.BindTexture(kTexture2D, g_out_s);
    gl.Uniform1i(g_floc_mode, mode);
    gl.Uniform2i(g_floc_size, w, h);
    gl.BindVertexArray(g_vao);
    gl.DrawArrays(kTriangles, 0, 3);
    gl.BindFramebuffer(kFramebuffer, 0);
    if (gl.GetError() != 0) return false;
    g_final = g_out_f;
    return true;
}

/* Passo 14d-1 */
bool gpu_ppu_compose_ip_layers(const uint16_t* runs, int scale) {
    if (!g_ready || !runs || !g_out_s || scale < 1 || scale > kMaxScale) return false;
    const int w = g_out_s_w, h = g_out_s_h;
    drain_errors("camadas 120 fps");
    if (!g_iprog) {
        std::string e;
        g_iprog = link_program(std::string(shader_header()) + kVertex,
                               std::string(shader_header()) + kIpFragment, nullptr, 0, e);
        if (!g_iprog) {
            std::fprintf(stderr, "[GPU] shader do 120 fps: %s\n", e.c_str());
            return false;
        }
        gl.UseProgram(g_iprog);
        gl.Uniform1i(gl.GetUniformLocation(g_iprog, "u_screen"), 0);
        gl.Uniform1i(gl.GetUniformLocation(g_iprog, "u_runs"), 1);
        g_iloc_scale = gl.GetUniformLocation(g_iprog, "u_scale");
        g_iloc_obj = gl.GetUniformLocation(g_iprog, "u_obj");
        g_iloc_w = gl.GetUniformLocation(g_iprog, "u_w");
        gl.ActiveTexture(kTexture0);
        g_tex_iruns = make_tex(kRG16UI, 256, kH, kRGInteger, kUnsignedShort);
    }
    if (w != g_out_i_w || h != g_out_i_h) {
        const GLuint old[] = {g_out_ibg, g_out_iobj};
        for (GLuint t : old) if (t) gl.DeleteTextures(1, &t);
        if (g_fbo_ibg) gl.DeleteFramebuffers(1, &g_fbo_ibg);
        if (g_fbo_iobj) gl.DeleteFramebuffers(1, &g_fbo_iobj);
        g_out_ibg = g_out_iobj = g_fbo_ibg = g_fbo_iobj = 0;
        g_out_i_w = g_out_i_h = 0;
        if (!make_target(&g_fbo_ibg, &g_out_ibg, w, h) || !make_target(&g_fbo_iobj, &g_out_iobj, w, h)) {
            return false;
        }
        g_out_i_w = w;
        g_out_i_h = h;
    }
    gl.PixelStorei(kUnpackAlignment, 1);
    gl.PixelStorei(kUnpackRowLength, 0);
    gl.ActiveTexture(kTexture0 + 1);
    upload(g_tex_iruns, 256, kH, kRGInteger, kUnsignedShort, runs);
    gl.PixelStorei(kUnpackAlignment, 4);
    gl.ActiveTexture(kTexture0);
    gl.BindTexture(kTexture2D, g_final ? g_final : g_out_s);   /* passo 14e-1: com o filtro */
    gl.Disable(kBlend);
    gl.Disable(kScissorTest);
    gl.Disable(kDepthTest);
    gl.Disable(kCullFace);
    gl.UseProgram(g_iprog);
    gl.Uniform1i(g_iloc_scale, scale);
    gl.Uniform1i(g_iloc_w, w);
    gl.BindVertexArray(g_vao);
    for (int pass = 0; pass < 2; ++pass) {
        gl.BindFramebuffer(kFramebuffer, pass ? g_fbo_iobj : g_fbo_ibg);
        gl.Viewport(0, 0, w, h);
        gl.Uniform1i(g_iloc_obj, pass);
        gl.DrawArrays(kTriangles, 0, 3);
    }
    gl.BindFramebuffer(kFramebuffer, 0);
    return gl.GetError() == 0;
}

unsigned int gpu_ppu_ip_texture(bool obj, int* w, int* h) {
    if (!g_ready) return 0;
    if (w) *w = g_out_i_w;
    if (h) *h = g_out_i_h;
    return obj ? g_out_iobj : g_out_ibg;
}

/* Lê uma camada (RGBA de verdade: 0xAARRGGBB com o alfa). */
bool gpu_ppu_read_ip_layer(bool obj, uint32_t* argb) {
    if (!g_ready || !argb || !g_out_i_w) return false;
    const int w = g_out_i_w, h = g_out_i_h;
    std::vector<unsigned char> rgba((size_t)w * h * 4);
    gl.BindFramebuffer(kFramebuffer, obj ? g_fbo_iobj : g_fbo_ibg);
    gl.PixelStorei(kPackAlignment, 1);
    gl.ReadPixels(0, 0, w, h, kRGBA, kUnsignedByte, rgba.data());
    gl.BindFramebuffer(kFramebuffer, 0);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        const unsigned char* p = &rgba[i * 4];
        argb[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    return gl.GetError() == 0;
}

unsigned int gpu_ppu_screen_texture(int* w, int* h) {
    if (!g_ready || !g_out_s) return 0;
    if (w) *w = g_out_s_w;
    if (h) *h = g_out_s_h;
    return g_final ? g_final : g_out_s;   /* passo 14e-1: a filtrada, se houver */
}

bool gpu_ppu_read_screen(uint32_t* argb, int* w, int* h) {
    if (!g_ready || !argb || !g_out_s) return false;
    if (w) *w = g_out_s_w;
    if (h) *h = g_out_s_h;
    return read_target(g_fbo_s, g_out_s_w, g_out_s_h, argb);
}

/* Passo 14e-1: lê a tela filtrada (a mesma de gpu_ppu_screen_texture). */
bool gpu_ppu_read_final(uint32_t* argb) {
    if (!g_ready || !argb || !g_out_s) return false;
    if (g_final == g_out_f && g_out_f) return read_target(g_fbo_f, g_out_f_w, g_out_f_h, argb);
    return read_target(g_fbo_s, g_out_s_w, g_out_s_h, argb);
}

const char* gpu_ppu_atlas_info(void) {
    return g_atlas_info.c_str();
}
