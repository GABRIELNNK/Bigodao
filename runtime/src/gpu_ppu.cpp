/**
 * @file gpu_ppu.cpp
 * @brief Centro e laterais do quadro montados pela GPU (passos 14b-1 a 14b-4). Veja gpu_ppu.h.
 */
#include "gpu_ppu.h"
#include "video_gl.h"
#include "video_gl_fns.h"

#include <algorithm>
#include <cmath>
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
GLuint g_tex_layout = 0;   /* passo 3D-2 */
bool g_level = false;
GLint g_loc_level = -1;
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
GLint g_loc_out = -1;   /* passo 3D-1 */
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
GLint g_sloc_cls = -1, g_sloc_bf = -1;   /* passo 3D-1 */

/* Passo 3D-1: camadas para o modo 3D. Para centro, laterais e tela, três
 * imagens: [0] cor, [1] só o fundo (o que fica atrás dos sprites), [2] classe
 * (0 longe, 1 fundo, 2 HUD, 3 sprite). */
struct Target3 {
    GLuint fbo[3] = {0, 0, 0}, out[3] = {0, 0, 0};
    int w = 0, h = 0;
};
Target3 g_3c, g_3m, g_3s;
bool g_3d_ok = false;
bool g_3d_level = false;   /* passo 3D-2: a fase está na tela (há chão separado) */
int g_3d_bw = 0;   /* largura base da tela 3D (256 ou 160) */
GLuint g_dprog = 0;
GLint g_dloc_mvp = -1, g_dloc_rect = -1, g_dloc_z = -1, g_dloc_layer = -1;

const char* kView3DVertex =
    "uniform mat4 u_mvp;\n"
    "uniform vec4 u_rect;\n"   /* x0, y0 (canto de cima à esquerda), largura, altura */
    "uniform float u_z;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
    "    v_uv = c;\n"
    "    vec3 p = vec3(u_rect.x + c.x * u_rect.z, u_rect.y - c.y * u_rect.w, u_z);\n"
    "    gl_Position = u_mvp * vec4(p, 1.0);\n"
    "}\n";

/* Passo 3D-3: lados dos blocos do chão. Uma instância por célula de 8x8 (alinhada
 * aos tiles do mundo), 4 faces (cima, baixo, esquerda, direita) de 6 vértices; a
 * face só existe se a célula é chão e a vizinha daquele lado não é. A cor do lado
 * é a borda do próprio tile esticada para dentro (como na prévia). Mesmo espaço
 * dos planos: x, y em pontos da tela ampliados por (dist - z) / dist. */
const char* kBoxVertex =
    "uniform mat4 u_mvp;\n"
    "uniform sampler2D u_clsmap;\n"
    "uniform vec2 u_off;\n"     /* canto da primeira célula, em pontos da tela (0..7) */
    "uniform float u_bw;\n"     /* largura da tela (256 ou 160) */
    "uniform float u_dist;\n"
    "uniform float u_zf;\n"     /* frente do bloco */
    "uniform float u_zb;\n"     /* trás do bloco */
    "uniform int u_cols;\n"
    "uniform int u_mode;\n"     /* 0 lados dos blocos; 1 (3D-3c) chão da superfície da água */
    "uniform sampler2D u_wp;\n"
    "out vec2 v_uv;\n"
    "out vec2 v_in;\n"      /* passo 3D-3b: um ponto para dentro do tile, em uv */
    "out float v_shade;\n"
    /* 0 vazio, 1 chão (bloco). 3D-3c: a água não é mais bloco (é volume à parte) */
    "int solid_at(int cx, int cy) {\n"
    "    vec2 c = u_off + vec2(float(cx) * 8.0 + 4.0, float(cy) * 8.0 + 4.0);\n"
    "    if (c.x < 0.0 || c.y < 0.0 || c.x >= u_bw || c.y >= 144.0) return 0;\n"
    "    ivec2 sz = textureSize(u_clsmap, 0);\n"
    "    ivec2 p = clamp(ivec2(c * vec2(sz) / vec2(u_bw, 144.0)), ivec2(0), sz - 1);\n"
    "    int code = int(round(texelFetch(u_clsmap, p, 0).r * 255.0));\n"
    "    int k = code & 7, u = (code >> 3) & 3;\n"
    "    if (k == 4 || (k == 3 && u == 1)) return 1;\n"
    "    return 0;\n"
    "}\n"
    "bool band_at(float px, float py) {\n"
    "    if (px < 0.0 || py < 0.0 || px >= u_bw || py >= 144.0) return false;\n"
    "    return (int(round(texelFetch(u_wp, ivec2(int(px), int(py)), 0).r * 255.0)) & 1) != 0;\n"
    "}\n"
    "void main() {\n"
    "    int cx = gl_InstanceID % u_cols - 1, cy = gl_InstanceID / u_cols - 1;\n"
    "    int face = gl_VertexID / 6, cor = gl_VertexID - face * 6;\n"
    "    int ci = cor < 3 ? cor : (cor == 3 ? 0 : (cor == 4 ? 2 : 3));\n"
    /* 3D-3c: debaixo das cópias das ondas, um chão horizontal da cor da água (na
     * célula de baixo da faixa das ondas), da frente até o fundo: tapa o que fica
     * entre as cópias */
    "    if (u_mode == 1) {\n"
    "        float x0 = u_off.x + float(cx) * 8.0, y0 = u_off.y + float(cy) * 8.0;\n"
    "        v_in = vec2(0.0); v_shade = 0.9;\n"
    "        if (face != 0 || !band_at(x0 + 4.0, y0 + 4.0) || band_at(x0 + 4.0, y0 + 12.0)) { v_uv = vec2(0.0); gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }\n"
    "        vec2 ab = ci == 0 ? vec2(0.0, 0.0) : (ci == 1 ? vec2(1.0, 0.0) : (ci == 2 ? vec2(1.0, 1.0) : vec2(0.0, 1.0)));\n"
    "        float z = mix(u_zf, u_zb, ab.y);\n"
    "        float px = x0 + ab.x * 8.0, py = y0 + 6.0;\n"
    "        v_uv = vec2(clamp(px, x0 + 0.5, x0 + 7.5), y0 + 7.5) / vec2(u_bw, 144.0);\n"
    "        float sc = (u_dist - z) / u_dist;\n"
    "        gl_Position = u_mvp * vec4((px - u_bw * 0.5) * sc, (72.0 - py) * sc, z, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    ivec2 nd = face == 0 ? ivec2(0, -1) : (face == 1 ? ivec2(0, 1) : (face == 2 ? ivec2(-1, 0) : ivec2(1, 0)));\n"
    "    v_uv = vec2(0.0); v_in = vec2(0.0); v_shade = 0.0;\n"
    "    int me = solid_at(cx, cy);\n"
    "    if (me == 0 || solid_at(cx + nd.x, cy + nd.y) != 0) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }\n"
    "    float x0 = u_off.x + float(cx) * 8.0, y0 = u_off.y + float(cy) * 8.0;\n"
    "    vec2 ab = ci == 0 ? vec2(0.0, 0.0) : (ci == 1 ? vec2(1.0, 0.0) : (ci == 2 ? vec2(1.0, 1.0) : vec2(0.0, 1.0)));\n"
    "    float z = mix(u_zf, u_zb, ab.y);\n"
    /* a cor de cada lado: a partir da borda do tile, para dentro, o primeiro
     * ponto que não é contorno escuro (no fragmento). Em cima da água (onda) a
     * busca começa por baixo, para pegar a cor da água e não o céu. */
    "    vec2 pos; vec2 uvb; vec2 dir;\n"
    "    float xa = clamp(x0 + ab.x * 8.0, x0 + 0.5, x0 + 7.5), ya = clamp(y0 + ab.x * 8.0, y0 + 0.5, y0 + 7.5);\n"
    "    if (face == 0) {\n"
    "        pos = vec2(x0 + ab.x * 8.0, y0); v_shade = 1.0;\n"
    "        if (me == 2) { uvb = vec2(xa, y0 + 7.5); dir = vec2(0.0, -1.0); }\n"
    "        else { uvb = vec2(xa, y0 + 0.5); dir = vec2(0.0, 1.0); }\n"
    "    }\n"
    "    else if (face == 1) { pos = vec2(x0 + ab.x * 8.0, y0 + 8.0); uvb = vec2(xa, y0 + 7.5); dir = vec2(0.0, -1.0); v_shade = 0.55; }\n"
    "    else if (face == 2) { pos = vec2(x0, y0 + ab.x * 8.0); uvb = vec2(x0 + 0.5, ya); dir = vec2(1.0, 0.0); v_shade = 0.75; }\n"
    "    else { pos = vec2(x0 + 8.0, y0 + ab.x * 8.0); uvb = vec2(x0 + 7.5, ya); dir = vec2(-1.0, 0.0); v_shade = 0.75; }\n"
    "    v_uv = uvb / vec2(u_bw, 144.0);\n"
    "    v_in = dir / vec2(u_bw, 144.0);\n"
    "    float sc = (u_dist - z) / u_dist;\n"
    "    gl_Position = u_mvp * vec4((pos.x - u_bw * 0.5) * sc, (72.0 - pos.y) * sc, z, 1.0);\n"
    "}\n";
const char* kBoxFragment =
    "uniform sampler2D u_tex;\n"
    "in vec2 v_uv;\n"
    "in vec2 v_in;\n"
    "in float v_shade;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    vec3 c = texture(u_tex, v_uv).rgb;\n"
    "    for (int i = 1; i < 7; ++i) {\n"
    "        c = texture(u_tex, v_uv + v_in * float(i)).rgb;\n"
    "        if (max(c.r, max(c.g, c.b)) > 0.25) break;\n"
    "    }\n"
    "    o_color = vec4(c * v_shade, 1.0);\n"
    "}\n";
GLuint g_bprog = 0;
GLint g_bloc_mode = -1;
GLint g_bloc_mvp = -1, g_bloc_off = -1, g_bloc_bw = -1, g_bloc_dist = -1, g_bloc_zf = -1, g_bloc_zb = -1, g_bloc_cols = -1;
/* imagem 3D fora da tela (com profundidade), do tamanho do retângulo do jogo */
GLuint g_fbo_v = 0, g_out_v = 0, g_rb_v = 0;
int g_v_w = 0, g_v_h = 0;
float g_3d_offx = 0.0f, g_3d_offy = 0.0f;   /* alinhamento das células com os tiles */
const char* kView3DFragment =
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_clsmap;\n"
    "uniform sampler2D u_wp;\n"   /* 3D-3c: água pré-calculada (1x): bandeiras e linha de origem */
    "uniform float u_alpha;\n"
    "uniform int u_layer;\n"   /* 0 longe, 1 fundo, 2 HUD, 3 sprites, 4 chão, 5 água, 6 porta, 7 sprites na frente do bloco (3D-3b), 8 superfície da água, 9 véu da água (3D-3c) */
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    /* plano de cada ponto: -1 longe, 0 fundo, 1 água, 2 chão/porta, 3 HUD; o sprite
     * conta como o que está atrás dele */
    "int rank_of(int code) {\n"
    "    int c = code & 7, u = (code >> 3) & 3;\n"
    "    if (c == 3) c = u == 0 ? 1 : (u == 1 ? 4 : (u == 2 ? 5 : 6));\n"
    "    if (c == 1) return 0;\n"
    "    if (c == 5) return 1;\n"
    "    if (c == 6) return 2;\n"   /* 3D-3: porta/escada no meio do bloco */
    "    if (c == 4) return 3;\n"
    "    if (c == 2) return 4;\n"
    "    return -1;\n"
    "}\n"
    "void main() {\n"
    "    ivec2 sz = textureSize(u_clsmap, 0);\n"
    "    ivec2 p = clamp(ivec2(v_uv * vec2(sz)), ivec2(0), sz - 1);\n"
    "    int code = int(round(texelFetch(u_clsmap, p, 0).r * 255.0));\n"
    "    int cls = code & 7;\n"   /* 0 longe, 1 fundo, 2 HUD, 3 sprite, 4 chão, 5 água, 6 porta */
    "    int under = (code >> 3) & 3;\n"   /* sprite: 1 = chão atrás, 2 = água, 3 = porta */
    "    float shade = 1.0;\n"
    "    vec2 uv = v_uv;\n"
    /* 3D-3c: água em volume. 5 = parede do fundo da água (corpo da água e, atrás
     * do que está dentro dela, a cor da água); 8 = superfície: cópias da faixa das
     * ondas da frente até o fundo (sem os pontos de céu); 9 = véu da água na frente
     * de tudo que está abaixo das ondas */
    "    if (u_layer == 5 || u_layer == 8 || u_layer == 9) {\n"
    "        ivec2 ws = textureSize(u_wp, 0);\n"
    "        vec4 w = texelFetch(u_wp, clamp(ivec2(v_uv * vec2(ws)), ivec2(0), ws - 1), 0);\n"
    "        int fl = int(round(w.r * 255.0)), row = int(round(w.g * 255.0));\n"
    "        vec2 src = vec2(v_uv.x, (float(row) + 0.5) / float(ws.y));\n"
    "        if (u_layer == 5) {\n"
    "            if ((fl & 4) != 0) { o_color = vec4(texture(u_tex, v_uv).rgb, 1.0); return; }\n"
    "            if ((fl & 8) != 0) { o_color = vec4(texture(u_tex, src).rgb, 1.0); return; }\n"
    "            discard;\n"
    "        }\n"
    "        if (u_layer == 8) {\n"
    "            if ((fl & 1) == 0) discard;\n"
    "            vec3 c = texture(u_tex, v_uv).rgb;\n"
    "            if (row != 255) { vec3 d = abs(c - texture(u_tex, src).rgb); if (d.r + d.g + d.b < 0.04) discard; }\n"
    "            o_color = vec4(c, 1.0);\n"
    "            return;\n"
    "        }\n"
    "        if ((fl & 2) == 0) discard;\n"
    "        o_color = vec4(texture(u_tex, (fl & 4) != 0 ? v_uv : src).rgb, u_alpha);\n"
    "        return;\n"
    "    }\n"
    "    int R = u_layer == 1 ? 0 : (u_layer == 5 ? 1 : (u_layer == 6 ? 2 : (u_layer == 4 ? 3 : -9)));\n"
    "    if (R < 0) {\n"   /* longe, sprites, HUD: só os próprios pontos */
    "        if (u_layer == 3 || u_layer == 7) {\n"   /* 3D-3b: 3 sprite no meio do bloco, 7 sprite na frente do chão/água */
    "            bool fr = under == 1 || under == 2;\n"
    "            if (cls != 3 || fr != (u_layer == 7)) discard;\n"
    "        } else if (cls != u_layer) discard;\n"
    "    } else {\n"
    /* 3D-2d: planos fundo (0), água (1), chão/porta/escada (2). Cada um desenha
     * os seus pontos e, atrás dos pontos de planos da frente, continua a si mesmo:
     * a cor do primeiro ponto acima, na mesma coluna, que não seja de um plano da
     * frente — se for deste plano. Assim a água continua atrás do bloco que está
     * dentro dela, o chão continua atrás do HUD, o céu atrás da água. */
    "        int r = rank_of(code);\n"
    "        if (r < R) discard;\n"
    "        if (r > R) {\n"
    "            int step = max(1, sz.y / 144);\n"
    "            ivec2 q = p;\n"
    "            int rq = 99;\n"
    "            for (int k = 0; k < 144 && q.y >= step; ++k) {\n"
    "                q.y -= step;\n"
    "                rq = rank_of(int(round(texelFetch(u_clsmap, q, 0).r * 255.0)));\n"
    "                if (rq <= R) break;\n"
    "            }\n"
    "            if (rq == R) uv.y = (float(q.y) + 0.5) / float(sz.y);\n"
    "            else if (R == 0) shade = 0.55;\n"   /* nada de fundo acima: o próprio ponto escurecido */
    "            else discard;\n"
    "        }\n"
    "    }\n"
    "    o_color = vec4(texture(u_tex, uv).rgb * shade, 1.0);\n"
    "}\n";

/* Passo 3D-3c: água pré-calculada, 1 ponto por ponto da tela (largura base x 144).
 * r = bandeiras: 1 faixa das ondas (até 16 pontos abaixo do céu, só água no
 * caminho), 2 abaixo das ondas (véu), 4 corpo da água (cor própria), 8 algo dentro
 * da água (bloco, porta, sprite): no fundo vai a cor da água da linha g.
 * g = linha: do céu acima da onda (para tirar os pontos de céu; 255 = nenhum) ou
 * da água que dá a cor. */
const char* kWaterPrepFragment =
    "uniform sampler2D u_clsmap;\n"
    "uniform float u_bw;\n"
    "out vec4 o_color;\n"
    "ivec2 g_sz;\n"
    "int rk(int x, int y) {\n"
    "    ivec2 p = clamp(ivec2((vec2(float(x), float(y)) + 0.5) * vec2(g_sz) / vec2(u_bw, 144.0)), ivec2(0), g_sz - 1);\n"
    "    int code = int(round(texelFetch(u_clsmap, p, 0).r * 255.0));\n"
    "    int c = code & 7, u = (code >> 3) & 3;\n"
    "    if (c == 3) c = u == 0 ? 1 : (u == 1 ? 4 : (u == 2 ? 5 : 6));\n"
    "    if (c == 1) return 0;\n"
    "    if (c == 5) return 1;\n"
    "    if (c == 6) return 2;\n"
    "    if (c == 4) return 3;\n"
    "    if (c == 2) return 4;\n"
    "    return -1;\n"
    "}\n"
    "void main() {\n"
    "    g_sz = textureSize(u_clsmap, 0);\n"
    "    int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);\n"
    "    int r = rk(x, y);\n"
    "    int fl = 0, row = 255;\n"
    "    if (r == 1) {\n"
    "        int k = 1, q = 1;\n"
    "        for (; k <= 16; ++k) {\n"
    "            if (y - k < 0) { q = -2; break; }\n"
    "            q = rk(x, y - k);\n"
    "            if (q != 1) break;\n"
    "        }\n"
    "        if (k <= 16 && q != 2 && q != 3) { fl = 1; row = q == 0 ? y - k : 255; }\n"
    "        else { fl = 4 | 2; row = y; }\n"
    "    } else if (r == 2 || r == 3) {\n"
    "        int ya = -1;\n"
    "        for (int yy = y - 1; yy >= 0; --yy) {\n"
    "            int q = rk(x, yy);\n"
    "            if (q == 1) { ya = yy; break; }\n"
    "            if (q != 2 && q != 3) break;\n"
    "        }\n"
    "        if (ya >= 0) {\n"
    "            row = ya;\n"
    "            for (int yy = y + 1; yy < 144; ++yy) {\n"
    "                int q = rk(x, yy);\n"
    "                if (q == 1) { row = yy; break; }\n"
    "                if (q != 2 && q != 3) break;\n"
    "            }\n"
    "            fl = 8 | 2;\n"
    "        }\n"
    "    }\n"
    "    o_color = vec4(float(fl) / 255.0, float(row) / 255.0, 0.0, 1.0);\n"
    "}\n";
GLuint g_wprog = 0, g_fbo_w = 0, g_out_w = 0;
int g_w_w = 0;
GLint g_wloc_bw = -1, g_dloc_alpha = -1;

bool make_target(GLuint* fbo, GLuint* tex, int w, int h);

bool ensure3(Target3& t, int w, int h) {
    if (t.w == w && t.h == h && t.out[0]) return true;
    for (int k = 0; k < 3; ++k) {
        if (t.out[k]) gl.DeleteTextures(1, &t.out[k]);
        if (t.fbo[k]) gl.DeleteFramebuffers(1, &t.fbo[k]);
        t.out[k] = t.fbo[k] = 0;
    }
    t.w = t.h = 0;
    for (int k = 0; k < 3; ++k) {
        if (!make_target(&t.fbo[k], &t.out[k], w, h)) return false;
    }
    t.w = w;
    t.h = h;
    return true;
}

void free3(Target3& t) {
    for (int k = 0; k < 3; ++k) {
        if (t.out[k]) gl.DeleteTextures(1, &t.out[k]);
        if (t.fbo[k]) gl.DeleteFramebuffers(1, &t.fbo[k]);
        t.out[k] = t.fbo[k] = 0;
    }
    t.w = t.h = 0;
}

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
    "uniform int u_cls;\n"         /* passo 3D-1: montando o mapa de classes */
    "uniform int u_blackfill;\n"   /* laterais sem cenário (classe 0 = longe) */
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
    "        else if (u_cls != 0) c = uvec3(u_blackfill != 0 ? 0u : 1u, 0u, 0u);\n"
    "        if (u_ambon != 0 && s > 1 && u_cls == 0) c = X < c0 ? amb(X, Y, s, 0, 47) : amb(X, Y, s, 208, 255);\n"
    "    } else if (u_cls != 0) {\n"
    "        c = uvec3(1u, 0u, 0u);\n"
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
    "uniform usampler2D u_line;\n"   /* 3x144 RGBA8UI (passo 3D-2: 3º texel = posição no mundo) */
    "uniform usampler2D u_layout;\n" /* 256x32 R8UI: blocos da fase (passo 3D-2) */
    "uniform int u_level;\n"
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
    "uniform int u_out;\n"   /* passo 3D-1: 0 = cor, 1 = só o fundo (sem sprites), 2 = classe */
    "out vec4 o_color;\n"
    "int g_y;\n"
    "int g_kind = 0;\n"   /* 3D-2: o fundo deste ponto é 0 fundo, 1 chão (bloco sólido), 2 água, 3 porta */
    /* classe do ponto (3D-1): 1 = fundo, 2 = janela (HUD), 3 = sprite; 3D-2: 4 = chão,
     * e +8 no sprite quando o que está atrás dele é chão */
    "void emit(uvec3 col, uvec3 bgc, int cls) {\n"
    "    int code = cls;\n"
    "    if (cls == 1 && g_kind == 1) code = 4;\n"
    "    if (cls == 1 && g_kind == 2) code = 5;\n"
    "    if (cls == 1 && g_kind == 3) code = 6;\n"   /* 3D-2c: porta */
    "    if (cls == 3) code = 3 + 8 * g_kind;\n"
    "    if (u_out == 2) o_color = vec4(float(code) / 255.0, 0.0, 0.0, 1.0);\n"
    "    else o_color = vec4(vec3(u_out == 1 ? bgc : col) / 255.0, 1.0);\n"
    "}\n"
    /* Blocos da fase que viram chão no 3D (disassembly, bank02 ActBGColi_IsSolid):
     * $00-$27 sólidos; $28-$3F caixas, quebráveis, gelo, areia etc. (menos $37 vazio e
     * $3D sem uso); $40-$43 plataformas; quebráveis na água; espinhos $5D-$5F. */
    "bool ground_block(int id) {\n"
    "    if (id < 0x28) return true;\n"
    "    if (id < 0x40) return id != 0x37 && id != 0x3D;\n"
    "    if (id < 0x44) return true;\n"
    "    if (id == 0x50 || id == 0x51 || id == 0x52 || id == 0x54) return true;\n"
    "    return id >= 0x5D && id <= 0x5F;\n"
    "}\n"
    /* 3D-2c/2d: no plano do chão, sem ser chão: porta ($48, $4B, $5B) e escada ($44, $45) */
    "bool door_block(int id) { return id == 0x48 || id == 0x4B || id == 0x5B || id == 0x44 || id == 0x45; }\n"
    "bool water_block(int id) { return id >= 0x4A && id <= 0x5B && !ground_block(id) && !door_block(id); }\n"
    /* 3D-2b: tipo pelo bloco e pelo de baixo (as ondas ficam num bloco vazio logo acima da água) */
    "int kind_of(int id, int down) {\n"
    "    if (ground_block(id)) return 1;\n"
    "    if (door_block(id)) return 3;\n"
    "    if (water_block(id)) return 2;\n"
    /* bloco vazio sobre a porta (o alto dela) e sobre a água (as ondas) */
    "    if (id >= 0x60 && (down == 0x48 || down == 0x4B)) return 3;\n"   /* alto da porta (não da escada) */
    "    if (id >= 0x60 && down >= 0 && water_block(down)) return 2;\n"
    "    return 0;\n"
    "}\n"
    "int s16(uint lo, uint hi) { int v = int(lo | (hi << 8u)); return v >= 32768 ? v - 65536 : v; }\n"
    "int lay(int bx, int by) {\n"
    "    if (bx < 0 || by < 0 || bx >= 256 || by >= 32) return -1;\n"
    "    return int(texelFetch(u_layout, ivec2(bx, by), 0).r & 0x7Fu);\n"
    "}\n"
    /* 3D-3c: ondas por cima de blocos que estão na água (o bloco vazio acima de um
     * bloco de chão não tem água logo embaixo): o chão tem água embaixo dele, ou na
     * mesma linha, andando por cima de blocos, chega-se a uma onda normal */
    "bool wave_over(int bx, int by) {\n"
    "    int d = lay(bx, by + 1);\n"
    "    if (d < 0 || !ground_block(d)) return false;\n"
    "    if (water_block(lay(bx, by + 2))) return true;\n"
    "    for (int sd = -1; sd <= 1; sd += 2)\n"
    "        for (int k = 1; k <= 8; ++k) {\n"
    "            int e = lay(bx + sd * k, by), dd = lay(bx + sd * k, by + 1);\n"
    "            if (e < 0x60) break;\n"
    "            if (water_block(dd)) return true;\n"
    "            if (dd < 0 || !ground_block(dd)) break;\n"
    "        }\n"
    "    return false;\n"
    "}\n"
    /* bloco da fase no ponto x (pode sair do centro: laterais) da linha y */
    "ivec2 blk_at(int x, int y) {\n"
    "    uvec4 l2 = texelFetch(u_line, ivec2(2, y), 0);\n"
    "    int wx = s16(l2.r, l2.g) + x, wy = s16(l2.b, l2.a);\n"
    "    if (wx < 0 || wy < 0 || wx >= 4096 || wy >= 512) return ivec2(-1);\n"
    "    return ivec2(wx >> 4, wy >> 4);\n"
    "}\n"
    "int kind_at(int x, int y) {\n"
    "    if (u_level == 0) return 0;\n"
    "    ivec2 b = blk_at(x, y);\n"
    "    if (b.x < 0) return 0;\n"
    "    int id = lay(b.x, b.y);\n"
    "    int k = kind_of(id, lay(b.x, b.y + 1));\n"
    "    if (k == 0 && id >= 0x60 && wave_over(b.x, b.y)) k = 2;\n"
    "    return k;\n"
    "}\n"
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
    "    bool fade = ((r.x >> 30) & 1u) != 0u;\n"
    "    uvec3 bg_rgba = unpack_rgb(r.y), obj_rgba = unpack_rgb(r.z);\n"
    "    uint bg_uvf = r.y >> 24, obj_uvf = r.z >> 24;\n"
    "    int bg_raw = int(r.w & 3u), obj_raw = int((r.w >> 2) & 3u);\n"
    /* 3D-1: sem HD o ponto é o rgba; só o fundo = rgba sem sprite, ou o fundo escurecido */
    "    uvec3 bg1 = obj_raw != 0 ? fade3(fade, m, bg_rgba) : rgba;\n"
    "    bool hudl = (texelFetch(u_line, ivec2(1, y), 0).a & 1u) != 0u;\n"   /* 3D-1b: faixa do mapa regional */
    "    g_kind = (u_level != 0 && !hudl && hs.z != 0u) ? kind_of(int(hs.z) - 1, int(hs.w) - 1) : 0;\n"   /* 3D-2 */
    "    if (g_kind == 0 && u_level != 0 && !hudl && hs.z != 0u && int(hs.z) - 1 >= 0x60) {\n"   /* 3D-3c: onda sobre blocos */
    "        ivec2 b = blk_at(m < 48 ? m - 48 : m + 112, y);\n"
    "        if (b.x >= 0 && lay(b.x, b.y) == int(hs.z) - 1 && wave_over(b.x, b.y)) g_kind = 2;\n"
    "    }\n"
    "    int clsb = hudl ? 2 : 1;\n"
    "    int cls1 = obj_raw != 0 ? 3 : clsb;\n"
    "    if (u_hd == 0) { emit(rgba, bg1, cls1); return; }\n"
    "    uint bs = hs.x, os = hs.y;\n"   /* tile HD que a CPU achou para este ponto */
    "    bool ot = ((r.w >> 4) & 1u) != 0u;\n"
    "    if (obj_raw == 0 && os == 0u) ot = false;\n"   /* caixa sem HD: não muda nada */
    "    if (bs == 0u && os == 0u) { emit(rgba, bg1, cls1); return; }\n"
    "    uvec3 c = fade3(fade, m, bg_rgba);\n"
    "    if (bs != 0u) {\n"
    "        uvec4 h = hd_texel(bs, int(bg_uvf & 7u), int((bg_uvf >> 3) & 7u), uvf_flags(bg_uvf), sx, sy, n);\n"
    "        if (h.a != 0u) c = fade3(fade, m, rc_apply(rc_setup(bs, bg_raw, bg_rgba), h));\n"
    "    }\n"
    "    uvec3 cbg = c;\n"
    "    int cls = clsb;\n"
    "    if (ot) {\n"
    "        if (os != 0u) {\n"
    "            uvec4 h = hd_texel(os, int(obj_uvf & 7u), int((obj_uvf >> 3) & 7u), uvf_flags(obj_uvf), sx, sy, n);\n"
    "            if (h.a >= 128u) { c = rc_apply(rc_setup(os, obj_raw, obj_rgba), h); cls = 3; }\n"
    "        } else {\n"
    "            c = obj_rgba; cls = 3;\n"
    "        }\n"
    "    }\n"
    "    emit(c, cbg, cls);\n"
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
    "    bool inwin = (win && x >= wx - 7);\n"
    "    bool hudl = (l1.a & 1u) != 0u;\n"   /* 3D-1b: faixa do mapa regional = HUD */
    "    g_kind = (!inwin && !hudl) ? kind_at(x, y) : 0;\n"   /* 3D-2 */
    "    if (inwin) {\n"
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
    "    uvec3 bgc1 = pal(bgpal * 4 + bgraw, y);\n"
    "    uvec3 c1 = useobj ? pal(32 + opal * 4 + oraw, y) : bgc1;\n"
    "    int cls1 = useobj ? 3 : ((inwin || hudl) ? 2 : 1);\n"
    "    if (u_hd == 0) { emit(c1, bgc1, cls1); return; }\n"
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
    "    if (bs == 0u && os == 0u) { emit(c1, bgc1, cls1); return; }\n"
    "    uvec3 bg_orig = pal(bgpal * 4 + bgraw, y);\n"
    "    uvec3 obj_orig = oraw != 0 ? pal(32 + cpal * 4 + oraw, y) : uvec3(0u);\n"
    "    bool blocked = bgraw != 0 && (lcdc & 0x01) != 0 && (bgpri || cbehind);\n"
    "    bool can_win = ot && !blocked;\n"
    "    uvec3 c = bg_orig;\n"
    "    if (bs != 0u) {\n"
    "        uvec4 hb = hd_texel(bs, bu, bv, attr & 0x60, sx, sy, n);\n"
    "        if (hb.a != 0u) c = rc_apply(rc_setup(bs, bgraw, bg_orig), hb);\n"
    "    }\n"
    "    uvec3 cbg = c;\n"
    "    int cls = (inwin || hudl) ? 2 : 1;\n"
    "    if (can_win) {\n"
    "        if (os != 0u) {\n"
    "            uvec4 ho = hd_texel(os, cu, cv, cfl, sx, sy, n);\n"
    "            if (ho.a >= 128u) { c = rc_apply(rc_setup(os, oraw, pal(32 + cpal * 4 + oraw, y)), ho); cls = 3; }\n"
    "        } else if (oraw != 0) {\n"
    "            c = obj_orig; cls = 3;\n"
    "        }\n"
    "    }\n"
    "    emit(c, cbg, cls);\n"
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
    upload(g_tex_line, 3, kH, kRGBAInteger, kUnsignedByte, f->line);
    if (f->level_layout) {   /* passo 3D-2 */
        gl.ActiveTexture(kTexture0 + 14);
        upload(g_tex_layout, 256, 32, kRedInteger, kUnsignedByte, f->layout);
    }
    g_level = f->level_layout;
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
                              kTexture2D, kTexture2D, kTexture2D};
    const GLuint texs[] = {g_tex_vram, g_tex_line, g_tex_oam, g_tex_pal, g_tex_dirty, g_tex_log,
                           g_tex_slot, g_tex_meta, g_tex_ref, g_tex_atlas, g_tex_tldir, g_tex_tl,
                           g_tex_mrec, g_tex_fade, g_tex_layout};
    for (int i = 0; i < 15; ++i) {
        gl.ActiveTexture(kTexture0 + (GLenum)i);
        gl.BindTexture(targets[i], texs[i]);
    }
    gl.ActiveTexture(kTexture0);
}

bool draw(GLuint fbo, int w, int h, int scale, bool hd, bool margin = false, int out = 0) {
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
    gl.Uniform1i(g_loc_out, out);   /* passo 3D-1 */
    gl.Uniform1i(g_loc_level, (out == 2 && g_level) ? 1 : 0);   /* passo 3D-2: só a classe usa */
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
                           "u_mrec", "u_fade", "u_layout"};
    for (int i = 0; i < 15; ++i) gl.Uniform1i(gl.GetUniformLocation(g_prog, names[i]), i);
    g_loc_level = gl.GetUniformLocation(g_prog, "u_level");
    g_loc_haslog = gl.GetUniformLocation(g_prog, "u_haslog");
    g_loc_scale = gl.GetUniformLocation(g_prog, "u_scale");
    g_loc_hd = gl.GetUniformLocation(g_prog, "u_hd");
    g_loc_hastl = gl.GetUniformLocation(g_prog, "u_hastl");
    g_loc_margin = gl.GetUniformLocation(g_prog, "u_margin");
    g_loc_out = gl.GetUniformLocation(g_prog, "u_out");
    gl.GenVertexArrays(1, &g_vao);
    gl.PixelStorei(kUnpackAlignment, 1);
    gl.PixelStorei(kUnpackRowLength, 0);
    g_tex_vram = make_tex(kR8UI, 256, 64, kRedInteger, kUnsignedByte);
    g_tex_line = make_tex(kRGBA8UI, 3, kH, kRGBAInteger, kUnsignedByte);
    g_tex_layout = make_tex(kR8UI, 256, 32, kRedInteger, kUnsignedByte);   /* passo 3D-2 */
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
        if (g_tex_layout) gl.DeleteTextures(1, &g_tex_layout);
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
        if (g_dprog) gl.DeleteProgram(g_dprog);
        if (g_bprog) gl.DeleteProgram(g_bprog);
        if (g_wprog) gl.DeleteProgram(g_wprog);
        if (g_out_w) gl.DeleteTextures(1, &g_out_w);
        if (g_fbo_w) gl.DeleteFramebuffers(1, &g_fbo_w);
        if (g_out_v) gl.DeleteTextures(1, &g_out_v);
        if (g_fbo_v) gl.DeleteFramebuffers(1, &g_fbo_v);
        if (g_rb_v) gl.DeleteRenderbuffers(1, &g_rb_v);
        free3(g_3c);
        free3(g_3m);
        free3(g_3s);
        if (g_fbo_f) gl.DeleteFramebuffers(1, &g_fbo_f);
        if (g_fbo_ibg) gl.DeleteFramebuffers(1, &g_fbo_ibg);
        if (g_fbo_iobj) gl.DeleteFramebuffers(1, &g_fbo_iobj);
        if (g_vao) gl.DeleteVertexArrays(1, &g_vao);
        if (g_prog) gl.DeleteProgram(g_prog);
    }
    g_tex_vram = g_tex_line = g_tex_oam = g_tex_pal = g_out = g_out_hd = 0;
    g_tex_layout = 0;
    g_tex_dirty = g_tex_log = g_tex_slot = g_tex_meta = g_tex_ref = g_tex_atlas = 0;
    g_tex_tldir = g_tex_tl = 0;
    g_tex_mrec = g_tex_fade = g_out_m = g_fbo_m = 0;
    g_sprog = g_tex_sbase = g_tex_samb = g_fbo_s = g_out_s = 0;
    g_iprog = g_tex_iruns = g_fbo_ibg = g_out_ibg = g_fbo_iobj = g_out_iobj = 0;
    g_out_i_w = g_out_i_h = 0;
    g_fprog = g_fbo_f = g_out_f = g_final = 0;
    g_dprog = g_bprog = 0;
    g_wprog = g_fbo_w = g_out_w = 0;
    g_w_w = 0;
    g_out_v = g_fbo_v = g_rb_v = 0;
    g_v_w = g_v_h = 0;
    g_3c = Target3();
    g_3m = Target3();
    g_3s = Target3();
    g_3d_ok = false;
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
void upload_margins(const PPUNativeGpuFrame* f) {
    if (g_margin_serial == f->serial) return;
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
    upload_margins(f);
    return draw(g_fbo_m, kMW * n, kH * n, n, f->hd_on && g_atlas_ok, true);
}

/* Passo 3D-1: centro, laterais e tela em três imagens (cor, só o fundo,
 * classe), na escala da textura do jogo (o HD funciona em qualquer escala). */
bool gpu_ppu_compose_3d(const PPUNativeGpuFrame* f, const GpuScreenInput* in) {
    g_3d_ok = false;
    if (!g_ready || !f || !f->cgb || f->log_overflow || !in || !in->base) return false;
    const int n = in->scale;
    if (n < 1 || n > kMaxScale) return false;
    drain_errors("3D");
    const bool hd = f->hd_on;
    if (hd) rebuild_atlas();
    upload_frame(f);
    if (!ensure3(g_3c, kW * n, kH * n)) return false;
    for (int k = 0; k < 3; ++k) {
        if (!draw(g_3c.fbo[k], kW * n, kH * n, n, hd && g_atlas_ok, false, k)) return false;
    }
    const bool margins = in->wide && in->hd_margins && f->margin_on;
    if (margins) {
        upload_margins(f);
        if (!ensure3(g_3m, kMW * n, kH * n)) return false;
        for (int k = 0; k < 3; ++k) {
            if (!draw(g_3m.fbo[k], kMW * n, kH * n, n, hd && g_atlas_ok, true, k)) return false;
        }
    }
    /* tela: a mesma montagem do 2D, com as três imagens */
    GpuScreenInput si = *in;
    si.hd_center = false;   /* o centro vem de g_3c (ligado abaixo) */
    si.hd_margins = false;
    if (!gpu_ppu_compose_screen(&si)) return false;   /* prepara programa, base e ambiente */
    const int bw = in->wide ? 256 : 160;
    if (!ensure3(g_3s, bw * n, kH * n)) return false;
    gl.UseProgram(g_sprog);
    gl.Uniform1i(g_sloc_hdc, 1);
    gl.Uniform1i(g_sloc_hdm, margins ? 1 : 0);
    gl.Uniform1i(g_sloc_bf, in->black_fill ? 1 : 0);
    gl.BindVertexArray(g_vao);
    for (int k = 0; k < 3; ++k) {
        gl.BindFramebuffer(kFramebuffer, g_3s.fbo[k]);
        gl.Viewport(0, 0, bw * n, kH * n);
        gl.ActiveTexture(kTexture0 + 1);
        gl.BindTexture(kTexture2D, g_3c.out[k]);
        gl.ActiveTexture(kTexture0 + 2);
        gl.BindTexture(kTexture2D, margins ? g_3m.out[k] : 0);
        gl.ActiveTexture(kTexture0);
        gl.Uniform1i(g_sloc_cls, k == 2 ? 1 : 0);
        gl.Uniform1i(g_sloc_amb, (k < 2 && in->amb_q) ? 1 : 0);
        gl.DrawArrays(kTriangles, 0, 3);
    }
    gl.BindFramebuffer(kFramebuffer, 0);
    if (gl.GetError() != 0) return false;
    g_3d_bw = bw;
    g_3d_level = f->level_layout;
    {   /* 3D-3: onde começam as células de 8x8 (bordas dos tiles), pela linha do meio */
        const uint8_t* l = f->line[72];
        const int wide_off = in->wide ? 48 : 0;
        int ox, oy;
        if (f->level_layout) {
            const int wx0 = (int16_t)(uint16_t)(l[8] | (l[9] << 8));
            const int wy72 = (int16_t)(uint16_t)(l[10] | (l[11] << 8));
            ox = wide_off - wx0;
            oy = -(wy72 - 72);
        } else {
            ox = wide_off - (int)l[1];
            oy = -(int)l[2];
        }
        g_3d_offx = (float)(((ox % 8) + 8) % 8) - 8.0f;
        g_3d_offy = (float)(((oy % 8) + 8) % 8) - 8.0f;
    }
    g_3d_ok = true;
    return true;
}

/* Matrizes 4x4 (colunas), para a câmera do 3D. */
static void mat_mul(const float* a, const float* b, float* o) {
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    std::memcpy(o, r, sizeof(r));
}

static void mat_perspective(float fovy, float aspect, float zn, float zf, float* m) {
    const float f = 1.0f / std::tan(fovy * 0.5f);
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zf + zn) / (zn - zf);
    m[11] = -1.0f;
    m[14] = 2.0f * zf * zn / (zn - zf);
}

static void mat_look_at(const float* eye, const float* at, float* m) {
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= fl;
    const float up[3] = {0.0f, 1.0f, 0.0f};
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (float& v : s) v /= sl;
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[14] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    m[15] = 1.0f;
}

/* Passo 3D-1: desenha as camadas na janela (no retângulo do jogo, já em
 * pixels do framebuffer). Cada camada fica a uma distância e é ampliada na
 * mesma proporção, então com inclinação 0 a imagem é a mesma do 2D. */
bool gpu_ppu_draw_3d(const Gpu3DView* v) {
    if (!g_ready || !g_3d_ok || !v || v->vp_w <= 0 || v->vp_h <= 0) return false;
    if (!g_dprog) {
        std::string e;
        g_dprog = link_program(std::string(shader_header()) + kView3DVertex,
                               std::string(shader_header()) + kView3DFragment, nullptr, 0, e);
        if (!g_dprog) {
            std::fprintf(stderr, "[GPU] shader do 3D: %s\n", e.c_str());
            return false;
        }
        gl.UseProgram(g_dprog);
        gl.Uniform1i(gl.GetUniformLocation(g_dprog, "u_tex"), 0);
        gl.Uniform1i(gl.GetUniformLocation(g_dprog, "u_clsmap"), 1);
        g_dloc_mvp = gl.GetUniformLocation(g_dprog, "u_mvp");
        g_dloc_rect = gl.GetUniformLocation(g_dprog, "u_rect");
        g_dloc_z = gl.GetUniformLocation(g_dprog, "u_z");
        g_dloc_layer = gl.GetUniformLocation(g_dprog, "u_layer");
        gl.Uniform1i(gl.GetUniformLocation(g_dprog, "u_wp"), 2);
        g_dloc_alpha = gl.GetUniformLocation(g_dprog, "u_alpha");
    }
    if (!g_bprog) {   /* passo 3D-3 */
        std::string e;
        g_bprog = link_program(std::string(shader_header()) + kBoxVertex,
                               std::string(shader_header()) + kBoxFragment, nullptr, 0, e);
        if (!g_bprog) {
            std::fprintf(stderr, "[GPU] shader dos blocos 3D: %s\n", e.c_str());
            return false;
        }
        gl.UseProgram(g_bprog);
        gl.Uniform1i(gl.GetUniformLocation(g_bprog, "u_tex"), 0);
        gl.Uniform1i(gl.GetUniformLocation(g_bprog, "u_clsmap"), 1);
        g_bloc_mvp = gl.GetUniformLocation(g_bprog, "u_mvp");
        g_bloc_off = gl.GetUniformLocation(g_bprog, "u_off");
        g_bloc_bw = gl.GetUniformLocation(g_bprog, "u_bw");
        g_bloc_dist = gl.GetUniformLocation(g_bprog, "u_dist");
        g_bloc_zf = gl.GetUniformLocation(g_bprog, "u_zf");
        g_bloc_zb = gl.GetUniformLocation(g_bprog, "u_zb");
        g_bloc_cols = gl.GetUniformLocation(g_bprog, "u_cols");
        g_bloc_mode = gl.GetUniformLocation(g_bprog, "u_mode");
        gl.Uniform1i(gl.GetUniformLocation(g_bprog, "u_wp"), 2);
    }
    if (!g_wprog) {   /* passo 3D-3c */
        std::string e;
        g_wprog = link_program(std::string(shader_header()) + kVertex,
                               std::string(shader_header()) + kWaterPrepFragment, nullptr, 0, e);
        if (!g_wprog) {
            std::fprintf(stderr, "[GPU] shader da agua 3D: %s\n", e.c_str());
            return false;
        }
        gl.UseProgram(g_wprog);
        gl.Uniform1i(gl.GetUniformLocation(g_wprog, "u_clsmap"), 0);
        g_wloc_bw = gl.GetUniformLocation(g_wprog, "u_bw");
    }
    if (g_w_w != g_3d_bw) {
        if (g_out_w) gl.DeleteTextures(1, &g_out_w);
        if (g_fbo_w) gl.DeleteFramebuffers(1, &g_fbo_w);
        g_out_w = g_fbo_w = 0;
        g_w_w = 0;
        if (!make_target(&g_fbo_w, &g_out_w, g_3d_bw, kH)) return false;
        g_w_w = g_3d_bw;
    }
    /* água pré-calculada (1x) a partir das classes */
    gl.BindFramebuffer(kFramebuffer, g_fbo_w);
    gl.Viewport(0, 0, g_3d_bw, kH);
    gl.Disable(kScissorTest);
    gl.Disable(kBlend);
    gl.Disable(kDepthTest);
    gl.UseProgram(g_wprog);
    gl.Uniform1f(g_wloc_bw, (float)g_3d_bw);
    gl.ActiveTexture(kTexture0);
    gl.BindTexture(kTexture2D, g_3s.out[2]);
    gl.BindVertexArray(g_vao);
    gl.DrawArrays(kTriangles, 0, 3);
    /* passo 3D-3: imagem fora da tela com profundidade (a janela não tem) */
    if (g_v_w != v->vp_w || g_v_h != v->vp_h) {
        if (g_out_v) gl.DeleteTextures(1, &g_out_v);
        if (g_fbo_v) gl.DeleteFramebuffers(1, &g_fbo_v);
        if (g_rb_v) gl.DeleteRenderbuffers(1, &g_rb_v);
        g_out_v = g_fbo_v = g_rb_v = 0;
        g_v_w = g_v_h = 0;
        if (!make_target(&g_fbo_v, &g_out_v, v->vp_w, v->vp_h)) return false;
        gl.GenRenderbuffers(1, &g_rb_v);
        gl.BindRenderbuffer(kRenderbuffer, g_rb_v);
        gl.RenderbufferStorage(kRenderbuffer, kDepthComponent24, v->vp_w, v->vp_h);
        gl.BindFramebuffer(kFramebuffer, g_fbo_v);
        gl.FramebufferRenderbuffer(kFramebuffer, kDepthAttachment, kRenderbuffer, g_rb_v);
        const GLenum st = gl.CheckFramebufferStatus(kFramebuffer);
        gl.BindFramebuffer(kFramebuffer, 0);
        if (st != kFramebufferComplete) return false;
        g_v_w = v->vp_w;
        g_v_h = v->vp_h;
    }
    const float bw = (float)g_3d_bw, bh = (float)kH;
    const float fov = 30.0f * 3.14159265f / 180.0f;
    const float dist = (bh * 0.5f) / std::tan(fov * 0.5f);
    const float pitch = v->tilt_deg * 3.14159265f / 180.0f;
    const float yaw = v->yaw_deg * 3.14159265f / 180.0f;
    const float eye[3] = {dist * std::sin(yaw) * std::cos(pitch), dist * std::sin(pitch),
                          dist * std::cos(yaw) * std::cos(pitch)};
    const float at[3] = {0.0f, 0.0f, 0.0f};
    float proj[16], view[16], mvp[16];
    mat_perspective(fov, bw / bh, dist * 0.05f, dist * 20.0f, proj);
    mat_look_at(eye, at, view);
    mat_mul(proj, view, mvp);
    /* linha 0 da imagem = topo (como as outras texturas da tela e o vgl_copy) */
    mvp[1] = -mvp[1]; mvp[5] = -mvp[5]; mvp[9] = -mvp[9]; mvp[13] = -mvp[13];

    gl.BindFramebuffer(kFramebuffer, g_fbo_v);
    gl.Viewport(0, 0, v->vp_w, v->vp_h);
    gl.Disable(kScissorTest);
    gl.Disable(kBlend);
    gl.Disable(kCullFace);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(kColorBufferBit | kDepthBufferBit);
    gl.Enable(kDepthTest);
    gl.DepthFunc(kLequal);
    gl.DepthMask(1);
    const GLint filt = (GLint)(v->linear ? kLinear : kNearest);
    for (int k = 0; k < 2; ++k) {
        gl.BindTexture(kTexture2D, g_3s.out[k]);
        gl.TexParameteri(kTexture2D, kTexMinFilter, filt);
        gl.TexParameteri(kTexture2D, kTexMagFilter, filt);
    }
    gl.ActiveTexture(kTexture0 + 1);
    gl.BindTexture(kTexture2D, g_3s.out[2]);
    gl.ActiveTexture(kTexture0 + 2);
    gl.BindTexture(kTexture2D, g_out_w);
    gl.BindVertexArray(g_vao);

    const float d = v->depth;
    /* 3D-3: o bloco tem profundidade bd; a frente fica em +bd/2, a porta/escada e
     * os sprites no meio (0), como na prévia; o fundo atrás. 3D-3c: a água é um
     * volume da frente dos blocos (+bd/2) até o fundo (zf): parede no fundo,
     * superfície com cópias das ondas e um véu translúcido na frente. */
    const float bd = g_3d_level ? 0.10f * dist * d : 0.0f;
    const float zf = g_3d_level ? -0.3f * dist * d : 0.0f;
    const float zw = zf;   /* 3D-3c: parede do fundo da água, junto do fundo */
    struct L { int layer; int tex; float z; };
    auto plane = [&](const L& l) {
        gl.ActiveTexture(kTexture0);
        gl.BindTexture(kTexture2D, g_3s.out[l.tex]);
        const float sc = (dist - l.z) / dist;
        gl.UniformMatrix4fv(g_dloc_mvp, 1, 0, mvp);
        gl.Uniform4f(g_dloc_rect, -bw * 0.5f * sc, bh * 0.5f * sc, bw * sc, bh * sc);
        gl.Uniform1f(g_dloc_z, l.z);
        gl.Uniform1i(g_dloc_layer, l.layer);
        gl.DrawArrays(kTriangleStrip, 0, 4);
    };
    gl.UseProgram(g_dprog);
    const L back[4] = {{0, 0, -0.8f * dist * d}, {1, 1, zf}, {5, 1, zw}, {6, 1, 0.0f}};
    for (const L& l : back) plane(l);
    /* lados dos blocos do chão */
    if (bd > 0.0f) {
        gl.UseProgram(g_bprog);
        gl.ActiveTexture(kTexture0);
        gl.BindTexture(kTexture2D, g_3s.out[1]);
        const int cols = (int)bw / 8 + 2, rows = kH / 8 + 2;
        gl.UniformMatrix4fv(g_bloc_mvp, 1, 0, mvp);
        gl.Uniform2f(g_bloc_off, g_3d_offx, g_3d_offy);
        gl.Uniform1f(g_bloc_bw, bw);
        gl.Uniform1f(g_bloc_dist, dist);
        gl.Uniform1f(g_bloc_zf, bd * 0.5f);
        gl.Uniform1f(g_bloc_zb, -bd * 0.5f);
        gl.Uniform1i(g_bloc_cols, cols);
        gl.Uniform1i(g_bloc_mode, 0);
        gl.DrawArraysInstanced(kTriangles, 0, 24, cols * rows);
        /* 3D-3c: chão da superfície da água, da frente dos blocos até o fundo */
        gl.Uniform1f(g_bloc_zb, zf);
        gl.Uniform1i(g_bloc_mode, 1);
        gl.DrawArraysInstanced(kTriangles, 0, 6, cols * rows);
        gl.UseProgram(g_dprog);
    }
    plane({4, 1, bd * 0.5f});   /* frente do chão */
    if (g_3d_level) {   /* 3D-3c: superfície da água, cópias das ondas da frente até o fundo */
        const float span = bd * 0.5f - zf;
        int n = (int)std::ceil(span * std::sin(pitch) / 6.0f) + 1;   /* uma cópia a cada ~6 pontos na tela */
        n = std::max(1, std::min(n, 48));
        for (int k = 0; k < n; ++k)
            plane({8, 1, n == 1 ? bd * 0.5f : bd * 0.5f - span * (float)k / (float)(n - 1)});
    }
    /* sprites com espessura: fatias de -t a +t (3D-3); HUD em relevo à frente */
    const float t = g_3d_level ? bd * 0.2f : 0.0f;
    const int slices = t > 0.0f ? 5 : 1;
    const float zs = g_3d_level ? 0.0f : 0.07f * dist * d;
    for (int k = 0; k < slices; ++k) {
        const float z = slices == 1 ? zs : zs - t + 2.0f * t * (float)k / (float)(slices - 1);
        plane({3, 0, z});
        /* 3D-3b: o sprite na frente de um bloco ou da água (Wario nadando, moeda
         * no bloco) não fica dentro do volume: vai para a frente dele */
        plane({7, 0, slices == 1 ? zs : bd * 0.5f + 2.0f * t * (float)k / (float)(slices - 1)});
    }
    /* 3D-3c: véu da água (30% da cor da água) na frente de tudo que está abaixo das
     * ondas; entra aos poucos até 10 graus, para a inclinação 0 continuar igual ao 2D */
    const float alpha = 0.30f * std::min(1.0f, std::max(0.0f, v->tilt_deg / 10.0f));
    if (g_3d_level && alpha > 0.0f) {
        gl.Enable(kBlend);
        gl.BlendFuncSeparate(kSrcAlpha, kOneMinusSrcAlpha, 0, kOne);
        gl.Disable(kDepthTest);   /* na frente da água (+bd/2), por cima de tudo que já está lá */
        gl.Uniform1f(g_dloc_alpha, alpha);
        plane({9, 1, bd * 0.5f});
        gl.Enable(kDepthTest);
        gl.Disable(kBlend);
    }
    /* HUD: fora das fases em relevo à frente; nas fases (3D-3c) chapado no plano do chão */
    plane({2, 0, g_3d_level ? bd * 0.5f : 0.12f * dist * d});
    gl.Disable(kDepthTest);
    gl.BindFramebuffer(kFramebuffer, 0);
    gl.ActiveTexture(kTexture0);
    return gl.GetError() == 0;
}

/* Passo 3D-3: a imagem 3D pronta (RGBA, linha 0 = topo). */
unsigned int gpu_ppu_3d_texture(int* w, int* h) {
    if (!g_ready || !g_out_v) return 0;
    if (w) *w = g_v_w;
    if (h) *h = g_v_h;
    return g_out_v;
}

/* Passo 3D-1: para testes (lê uma das três imagens da tela 3D). */
bool gpu_ppu_read_3d(int which, uint32_t* argb, int* w, int* h) {
    if (!g_ready || !g_3d_ok || which < 0 || which > 2 || !argb) return false;
    if (w) *w = g_3s.w;
    if (h) *h = g_3s.h;
    return read_target(g_3s.fbo[which], g_3s.w, g_3s.h, argb);
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
        g_sloc_cls = gl.GetUniformLocation(g_sprog, "u_cls");
        g_sloc_bf = gl.GetUniformLocation(g_sprog, "u_blackfill");
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
    gl.Uniform1i(g_sloc_cls, 0);
    gl.Uniform1i(g_sloc_bf, 0);
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
