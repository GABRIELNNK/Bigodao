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
GLint g_loc_mono = -1, g_loc_mono_ramp = -1;   /* modo de cor original (P&B) */
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
GLuint g_tex_slot = 0;     /* R16UI 768x17: slot + 1 por tile e (tipo, paleta); linha 16 = marcas do 3D-2b */
uint16_t g_marks[768] = {0};
bool g_marks_any = false, g_marks_dirty = true;
GLint g_loc_marks = -1;
bool g_winfloor = false;   /* passo 3D-2g */
GLint g_loc_winfloor = -1;
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
/* Passo 3D-4a: tela + a fase além dela (EX de cada lado, EY em cima e embaixo);
 * o 3D desenha a partir de g_cv (g_3e com a fase além, ou g_3s sem). */
Target3 g_3e;
Target3* g_cv = &g_3s;
int g_cw = 0, g_ch = 0, g_ex = 0, g_ey = 0;
GLuint g_tex_ext = 0;
GLint g_loc_ext = -1, g_loc_extg = -1, g_loc_exto = -1, g_loc_tx = -1, g_loc_gh = -1, g_loc_ngh = -1;
GLuint g_cprog = 0;   /* cópia de uma textura para um pedaço do alvo */
GLint g_cloc_off = -1;
bool g_3d_ok = false;
bool g_3d_level = false;   /* passo 3D-2: a fase está na tela (há chão separado) */
int g_3d_bw = 0;   /* largura base da tela 3D (256 ou 160) */
GLuint g_dprog = 0;
GLint g_dloc_mvp = -1, g_dloc_rect = -1, g_dloc_z = -1, g_dloc_layer = -1;

const char* kView3DVertex =
    "uniform mat4 u_mvp;\n"
    "uniform vec4 u_rect;\n"   /* x0, y0 (canto de cima à esquerda), largura, altura */
    "uniform float u_z;\n"
    "uniform vec4 u_uvr;\n"   /* 3D-4a: pedaço da imagem (o HUD usa só a tela) */
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
    "    v_uv = u_uvr.xy + c * u_uvr.zw;\n"
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
    "uniform float u_bw;\n"     /* largura da imagem (tela + fase além, 3D-4a) */
    "uniform float u_ch;\n"     /* altura da imagem (144 + fase além) */
    "uniform float u_dist;\n"
    "uniform float u_zf;\n"     /* frente do bloco */
    "uniform float u_zb;\n"     /* trás do bloco */
    "uniform int u_cols;\n"
    "uniform int u_mode;\n"     /* 0 lados dos blocos; 1 (3D-3c) chão da superfície da água; 2 porta; 3 sprites (3D-5c) */
    "uniform sampler2D u_wp;\n"
    "out vec2 v_uv;\n"
    "out vec2 v_in;\n"      /* passo 3D-3b: um ponto para dentro do tile, em uv */
    "out float v_shade;\n"
    "out float v_tint;\n"   /* 3D-3e: célula solta dentro da água (véu) */
    "out vec2 v_wuv;\n"     /* cor da água para o véu */
    "out vec2 v_out;\n"     /* 3D-3f: ponto logo fora da borda (céu/água ao lado), em uv */
    "out float v_dep;\n"    /* 3D-6c: 0 na frente da extrusão, 1 atrás */
    "uniform int u_front;\n"   /* 3D-6c: extrusão do plano da frente (só blocos de terreno) */
    "uniform int u_half;\n"    /* 3D-2k: lados do chão: 0 = chão normal, 1 = só o de meia extrusão */
    "bool pix_solid(int px, int py) {\n"
    "    if (px < 0 || py < 0 || float(px) >= u_bw || float(py) >= u_ch) return false;\n"
    "    if (u_mode == 0 && u_front == 0) {\n"
    "        ivec2 cs = textureSize(u_clsmap, 0);\n"
    "        ivec2 q = clamp(ivec2((vec2(float(px), float(py)) + 0.5) * vec2(cs) / vec2(u_bw, u_ch)), ivec2(0), cs - 1);\n"
    "        if ((texelFetch(u_clsmap, q, 0).b > 0.5) != (u_half != 0)) return false;\n"
    "    }\n"
    "    if (u_front != 0) {\n"
    "        ivec2 cs = textureSize(u_clsmap, 0);\n"
    "        ivec2 q = clamp(ivec2((vec2(float(px), float(py)) + 0.5) * vec2(cs) / vec2(u_bw, u_ch)), ivec2(0), cs - 1);\n"
    "        if (texelFetch(u_clsmap, q, 0).g < 0.5) return false;\n"
    "    }\n"
    "    if (u_mode == 3) {\n"   /* 3D-5c: ponto de sprite (extrusão dos sprites) */
    "        ivec2 cs = textureSize(u_clsmap, 0);\n"
    "        ivec2 q = clamp(ivec2((vec2(float(px), float(py)) + 0.5) * vec2(cs) / vec2(u_bw, u_ch)), ivec2(0), cs - 1);\n"
    "        return (int(round(texelFetch(u_clsmap, q, 0).r * 255.0)) & 7) == 3;\n"
    "    }\n"
    "    return (int(round(texelFetch(u_wp, ivec2(px, py), 0).r * 255.0)) & (u_mode == 2 ? 64 : 32)) != 0;\n"
    "}\n"
    "bool band_at(float px, float py) {\n"
    "    if (px < 0.0 || py < 0.0 || px >= u_bw || py >= u_ch) return false;\n"
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
    "        v_in = vec2(0.0); v_shade = 0.9; v_tint = 0.0; v_wuv = vec2(0.0); v_out = vec2(-1.0); v_dep = 0.0;\n"
    "        if (face != 0 || !band_at(x0 + 4.0, y0 + 4.0) || band_at(x0 + 4.0, y0 + 12.0)) { v_uv = vec2(0.0); gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }\n"
    "        vec2 ab = ci == 0 ? vec2(0.0, 0.0) : (ci == 1 ? vec2(1.0, 0.0) : (ci == 2 ? vec2(1.0, 1.0) : vec2(0.0, 1.0)));\n"
    "        float z = mix(u_zf, u_zb, ab.y);\n"
    "        float px = x0 + ab.x * 8.0, py = y0 + 6.0;\n"
    /* 3D-4k: a cor vem da coluna de onde a água foi copiada (3D-3k: atrás de um
     * objeto ao lado da água a faixa é emprestada do vizinho); antes vinha do
     * próprio ponto, e atrás da caixa batida aparecia a cor dela (azul) */
    "        float dx = 0.0;\n"
    "        {\n"
    "            ivec2 q = clamp(ivec2(int(x0 + 4.0), int(y0 + 7.0)), ivec2(0), textureSize(u_wp, 0) - 1);\n"
    "            int sxp = int(round(texelFetch(u_wp, q, 0).b * 255.0));\n"
    "            if (sxp != 255) dx = float(sxp - 128);\n"
    "        }\n"
    "        v_uv = vec2(clamp(px, x0 + 0.5, x0 + 7.5) + dx, y0 + 7.5) / vec2(u_bw, u_ch);\n"
    "        float sc = (u_dist - z) / u_dist;\n"
    "        gl_Position = u_mvp * vec4((px - u_bw * 0.5) * sc, (u_ch * 0.5 - py) * sc, z, 1.0);\n"
    "        return;\n"
    "    }\n"
    /* 3D-3h: extrusão por ponto (1x): o ponto sólido (chão, sem os buracos) ganha
     * uma face onde o vizinho não é sólido; assim o volume segue o contorno do
     * desenho (barril redondo) e não o quadrado do tile */
    "    int px = gl_InstanceID % int(u_bw), py = gl_InstanceID / int(u_bw);\n"
    "    ivec2 nd = face == 0 ? ivec2(0, -1) : (face == 1 ? ivec2(0, 1) : (face == 2 ? ivec2(-1, 0) : ivec2(1, 0)));\n"
    "    v_uv = vec2(0.0); v_in = vec2(0.0); v_shade = 0.0; v_tint = 0.0; v_wuv = vec2(0.0); v_out = vec2(-1.0);\n"
    "    v_dep = 0.0;\n"
    "    if (!pix_solid(px, py) || pix_solid(px + nd.x, py + nd.y)) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }\n"
    "    float x0 = float(px), y0 = float(py);\n"
    "    vec2 ab = ci == 0 ? vec2(0.0, 0.0) : (ci == 1 ? vec2(1.0, 0.0) : (ci == 2 ? vec2(1.0, 1.0) : vec2(0.0, 1.0)));\n"
    "    float z = mix(u_zf, u_zb, ab.y);\n"
    "    v_dep = ab.y;\n"
    "    {\n"
    "        vec4 w = texelFetch(u_wp, ivec2(px, py), 0);\n"
    "        int fl = int(round(w.r * 255.0)), row = int(round(w.g * 255.0));\n"
    "        if ((fl & 2) != 0) { v_tint = 1.0; v_wuv = vec2(x0 + 0.5, (fl & 4) != 0 ? y0 + 0.5 : float(row) + 0.5) / vec2(u_bw, u_ch); }\n"
    "    }\n"
    "    vec2 pos = face == 0 ? vec2(x0 + ab.x, y0) : (face == 1 ? vec2(x0 + ab.x, y0 + 1.0) : (face == 2 ? vec2(x0, y0 + ab.x) : vec2(x0 + 1.0, y0 + ab.x)));\n"
    "    vec2 dir = vec2(-nd);\n"
    "    vec2 uvb = vec2(x0 + 0.5, y0 + 0.5);\n"
    "    v_shade = face == 0 ? 1.0 : (face == 1 ? 0.55 : 0.75);\n"
    "    v_uv = uvb / vec2(u_bw, u_ch);\n"
    "    v_in = dir / vec2(u_bw, u_ch);\n"
    "    v_out = (uvb - dir) / vec2(u_bw, u_ch);\n"
    "    float sc = (u_dist - z) / u_dist;\n"
    "    gl_Position = u_mvp * vec4((pos.x - u_bw * 0.5) * sc, (u_ch * 0.5 - pos.y) * sc, z, 1.0);\n"
    "}\n";
const char* kBoxFragment =
    "uniform sampler2D u_tex;\n"
    "uniform float u_alpha;\n"
    "uniform float u_light;\n"   /* 3D-3i: lados da porta mais claros */
    "uniform int u_simple;\n"   /* 3D-5c: lados dos sprites */
    "uniform sampler2D u_clsmap;\n"   /* 3D-5d: só pontos do próprio sprite na cor do lado */
    "in vec2 v_uv;\n"
    "in vec2 v_in;\n"
    "in float v_shade;\n"
    "in float v_tint;\n"
    "in vec2 v_wuv;\n"
    "in vec2 v_out;\n"
    "in float v_dep;\n"
    "uniform int u_front;\n"
    "uniform float u_edge;\n"   /* 3D-6c: espessura do contorno preto (fração da extrusão) */
    "out vec4 o_color;\n"
    "void main() {\n"
    /* 3D-6c: contorno preto onde a extrusão do plano da frente encosta no chão de
     * antes (aspecto de camada nova) */
    "    if (u_front != 0 && v_dep >= 1.0 - u_edge) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
    "    if (u_simple != 0) {\n"
    /* 3D-5d: como nos blocos, a cor que mais aparece para dentro (8 pontos), sem
     * os escuros do contorno e só pontos do próprio sprite */
    "        vec3 sc[8]; bool ok[8];\n"
    "        for (int i = 0; i < 8; ++i) {\n"
    "            vec2 u = v_uv + v_in * float(i);\n"
    "            sc[i] = texture(u_tex, u).rgb;\n"
    "            ivec2 cz = textureSize(u_clsmap, 0);\n"
    "            int k = int(round(texelFetch(u_clsmap, clamp(ivec2(u * vec2(cz)), ivec2(0), cz - 1), 0).r * 255.0)) & 7;\n"
    "            ok[i] = k == 3 && max(sc[i].r, max(sc[i].g, sc[i].b)) > 0.25;\n"
    "        }\n"
    "        int sb = -1, sn = 0;\n"
    "        for (int i = 0; i < 8; ++i) {\n"
    "            if (!ok[i]) continue;\n"
    "            int n = 0;\n"
    "            for (int j = 0; j < 8; ++j) { vec3 d = abs(sc[i] - sc[j]); if (ok[j] && d.r + d.g + d.b < 0.06) ++n; }\n"
    "            if (n > sn) { sn = n; sb = i; }\n"
    "        }\n"
    "        vec3 c = (sb >= 0 ? sc[sb] : sc[0]) * v_shade;\n"
    "        if (v_tint > 0.5) c = mix(c, texture(u_tex, v_wuv).rgb, u_alpha);\n"
    "        o_color = vec4(c, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    vec3 cs[8];\n"
    "    for (int i = 0; i < 8; ++i) cs[i] = texture(u_tex, v_uv + v_in * float(i)).rgb;\n"
    /* 3D-3f: fora as cores do que está do lado de fora (céu/água nos cantos de
     * um barril redondo) */
    "    bool hasout = v_out.x >= 0.0 && v_out.y >= 0.0 && v_out.x <= 1.0 && v_out.y <= 1.0;\n"
    "    vec3 oc = hasout ? texture(u_tex, v_out).rgb : vec3(-1.0);\n"
    "    int best = -1, bn = 0;\n"
    "    for (int i = 0; i < 8; ++i) {\n"
    "        if (max(cs[i].r, max(cs[i].g, cs[i].b)) <= 0.25) continue;\n"
    "        if (hasout) { vec3 e = abs(cs[i] - oc); if (e.r + e.g + e.b < 0.06) continue; }\n"
    "        int n = 0;\n"
    "        for (int j = 0; j < 8; ++j) { vec3 d = abs(cs[i] - cs[j]); if (d.r + d.g + d.b < 0.06) ++n; }\n"
    "        if (n > bn) { bn = n; best = i; }\n"
    "    }\n"
    "    vec3 c = mix(best >= 0 ? cs[best] : cs[1], vec3(1.0), u_light) * v_shade;\n"
    "    if (v_tint > 0.5) c = mix(c, texture(u_tex, v_wuv).rgb, u_alpha);\n"
    "    o_color = vec4(c, 1.0);\n"
    "}\n";
GLuint g_bprog = 0;
GLint g_bloc_mode = -1, g_bloc_alpha = -1, g_bloc_light = -1, g_bloc_simple = -1;
GLint g_bloc_front = -1, g_bloc_edge = -1;   /* 3D-6c */
GLint g_bloc_half = -1;   /* 3D-2k */
GLint g_bloc_mvp = -1, g_bloc_off = -1, g_bloc_bw = -1, g_bloc_dist = -1, g_bloc_zf = -1, g_bloc_zb = -1, g_bloc_cols = -1;
/* imagem 3D fora da tela (com profundidade), do tamanho do retângulo do jogo */
GLuint g_fbo_v = 0, g_out_v = 0, g_rb_v = 0;
int g_v_w = 0, g_v_h = 0;
float g_3d_offx = 0.0f, g_3d_offy = 0.0f;   /* alinhamento das células com os tiles */
const char* kView3DFragment =
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_clsmap;\n"
    "uniform sampler2D u_wp;\n"   /* 3D-3c: água pré-calculada (1x): bandeiras e linha de origem */
    "uniform sampler2D u_bgt;\n"
    "uniform sampler2D u_bs;\n"   /* 3D-4i: origem do fundo atrás dos objetos (1x) */  /* 3D-3e: só o fundo (cor da água para o véu) */
    "uniform float u_alpha;\n"
    "uniform float u_lshade;\n"
    "uniform int u_chi;\n"
    "uniform int u_halfp;\n"   /* 3D-2l: plano da frente do meio chão (no meio do bloco) */
    "uniform int u_nospr;\n"   /* 3D-5b: cópia das ondas na frente dos sprites: não desenha sobre sprite */   /* 3D-4a: altura da imagem em pontos 1x */   /* 3D-3g: cópias das ondas mais escuras quanto mais fundo */
    "uniform int u_layer;\n"   /* 0 longe, 1 fundo, 2 HUD, 3 sprites, 4 chão, 5 água, 6 porta, 8 superfície da água, 9 véu da água (3D-3c) */
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
    "    if (u_layer == 10) {\n"   /* 3D-3f: diagnóstico das classes */
    "        ivec2 ws = textureSize(u_wp, 0);\n"
    "        int fl = int(round(texelFetch(u_wp, clamp(ivec2(v_uv * vec2(ws)), ivec2(0), ws - 1), 0).r * 255.0));\n"
    "        int k = cls == 3 ? 3 : cls;\n"
    "        vec3 c = k == 0 ? vec3(0.0) : (k == 1 ? vec3(0.45) : (k == 2 ? vec3(1.0) : (k == 3 ? vec3(1.0, 0.0, 1.0) :\n"
    "                 (k == 4 ? vec3(0.5, 0.3, 0.1) : (k == 5 ? vec3(0.0, 0.2, 1.0) : vec3(1.0, 0.55, 0.0))))));\n"
    "        if (cls == 5 && fl == 0) c = vec3(0.6, 0.8, 1.0);\n"
    "        if ((fl & 1) != 0) c = vec3(0.0, 1.0, 1.0);\n"
    "        if (cls == 4 && texelFetch(u_clsmap, p, 0).b > 0.5) c = vec3(1.0, 0.9, 0.1);\n"   /* 3D-2m: meio chão em amarelo */
    "        o_color = vec4(c, 0.7);\n"
    "        return;\n"
    "    }\n"
    /* 3D-3c: água em volume. 5 = parede do fundo da água (corpo da água e, atrás
     * do que está dentro dela, a cor da água); 8 = superfície: cópias da faixa das
     * ondas da frente até o fundo (sem os pontos de céu); 9 = véu da água na frente
     * de tudo que está abaixo das ondas */
    "    if (u_layer == 5 || u_layer == 8 || u_layer == 9) {\n"
    "        ivec2 ws = textureSize(u_wp, 0);\n"
    "        vec4 w = texelFetch(u_wp, clamp(ivec2(v_uv * vec2(ws)), ivec2(0), ws - 1), 0);\n"
    "        int fl = int(round(w.r * 255.0)), row = int(round(w.g * 255.0));\n"
    "        int sxp = int(round(w.b * 255.0)), ref = int(round(w.a * 255.0));\n"   /* 3D-3i: coluna da cor, linha do céu */
    "        bool own = sxp == 255;\n"
    "        float ux = own ? v_uv.x : (float(int(v_uv.x * float(ws.x)) + sxp - 128) + 0.5) / float(ws.x);\n"
    "        vec2 src = vec2(ux, (float(row) + 0.5) / float(ws.y));\n"
    "        if (u_layer == 5) {\n"
    "            if ((fl & 4) != 0) { o_color = vec4(texture(u_tex, v_uv).rgb, 1.0); return; }\n"
    "            if ((fl & 8) != 0) { o_color = vec4(texture(u_tex, src).rgb, 1.0); return; }\n"
    "            discard;\n"
    "        }\n"
    "        if (u_layer == 8) {\n"
    "            if ((fl & 1) == 0) discard;\n"
    "            if (u_nospr != 0 && cls == 3) discard;\n"
    "            vec3 c = texture(u_tex, own ? v_uv : src).rgb;\n"
    /* 3D-6e: céu em degradê pontilhado (duas cores alternando): compara com as cores
     * do céu em 2 linhas x 3 colunas em volta da linha de referência, não só com uma
     * (sobravam riscos claros verticais nas cópias das ondas) */
    "            if (ref != 255) {\n"
    "                for (int j = 0; j < 2; ++j) for (int i = -1; i <= 1; ++i) {\n"
    "                    vec2 su = vec2(ux + float(i) / float(ws.x), (float(max(ref - j, 0)) + 0.5) / float(ws.y));\n"
    "                    vec3 d = abs(c - texture(u_tex, su).rgb);\n"
    "                    if (d.r + d.g + d.b < 0.04) discard;\n"
    "                }\n"
    "            }\n"
    "            o_color = vec4(c * u_lshade, 1.0);\n"
    "            return;\n"
    "        }\n"
    "        if ((fl & 2) == 0) discard;\n"
    "        o_color = vec4(texture(u_tex, (fl & 4) != 0 ? v_uv : src).rgb, u_alpha);\n"
    "        return;\n"
    "    }\n"
    /* 3D-6a: plano da frente: cópia só dos pontos de chão (sem porta/escada/moeda,
     * sem sprites), na frente de tudo; 3D-6b: sem a água */
    "    if (u_layer == 11) {\n"
    "        if (cls != 4) discard;\n"
    "        if (texelFetch(u_clsmap, p, 0).g < 0.5) discard;\n"
    "        if ((code & 64) != 0) discard;\n"
    "        ivec2 ws = textureSize(u_wp, 0);\n"
    "        vec4 w = texelFetch(u_wp, clamp(ivec2(v_uv * vec2(ws)), ivec2(0), ws - 1), 0);\n"
    "        int fl = int(round(w.r * 255.0)), ref = int(round(w.a * 255.0));\n"
    "        vec3 c = texture(u_tex, v_uv).rgb;\n"
    "        if (cls == 4) {\n"
    "            if ((fl & (16 | 128)) != 0) discard;\n"   /* buracos e enfeite da porta */
    "        } else if ((fl & 4) == 0) {\n"   /* água: o corpo; na faixa das ondas, sem os pontos de céu */
    "            if ((fl & 1) == 0) discard;\n"
    "            if (ref != 255) { vec3 d = abs(c - texture(u_tex, vec2(v_uv.x, (float(ref) + 0.5) / float(ws.y))).rgb); if (d.r + d.g + d.b < 0.04) discard; }\n"
    "        }\n"
    "        o_color = vec4(c, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    bool deco = false;\n"   /* 3D-3k: enfeite da porta (vai com a porta) */
    "    if (u_layer == 1 || u_layer == 6) {\n"
    "        ivec2 ws = textureSize(u_wp, 0);\n"
    "        deco = (int(round(texelFetch(u_wp, clamp(ivec2(v_uv * vec2(ws)), ivec2(0), ws - 1), 0).r * 255.0)) & 128) != 0;\n"
    "        if (deco && u_layer == 6) { o_color = vec4(texture(u_tex, v_uv).rgb, 1.0); return; }\n"
    "    }\n"
    /* 3D-4o: o ponto de sprite não é desenhado pelos planos do chão e da porta (eles
     * ficam na frente dos sprites e cobriam o sprite com o fundo que está atrás dele:
     * o bloco grande da caverna, que é um sprite sobre blocos sólidos) */
    "    if ((u_layer == 4 || u_layer == 6) && cls == 3) discard;\n"
    /* 3D-2l: meio chão (azul no mapa de classes): a frente dele fica no meio do bloco */
    "    if (u_layer == 4) {\n"
    "        bool hp = texelFetch(u_clsmap, p, 0).b > 0.5;\n"
    "        if (u_halfp != 0) { if (cls != 4 || !hp) discard; }\n"
    "        else if (cls == 4 && hp) discard;\n"
    "    }\n"
    "    if (u_layer == 4 || u_layer == 6) {\n"   /* 3D-3g: buracos (cantos do barril, água em volta da moeda) */
    "        ivec2 ws = textureSize(u_wp, 0);\n"
    "        if ((int(round(texelFetch(u_wp, clamp(ivec2(v_uv * vec2(ws)), ivec2(0), ws - 1), 0).r * 255.0)) & 16) != 0) discard;\n"
    "    }\n"
    "    int R = u_layer == 1 ? 0 : (u_layer == 5 ? 1 : (u_layer == 6 ? 2 : (u_layer == 4 ? 3 : -9)));\n"
    "    if (R < 0) {\n"   /* longe, sprites, HUD: só os próprios pontos */
    "        if (cls != u_layer) discard;\n"
    "    } else {\n"
    /* 3D-2d: planos fundo (0), água (1), chão/porta/escada (2). Cada um desenha
     * os seus pontos e, atrás dos pontos de planos da frente, continua a si mesmo:
     * a cor do primeiro ponto acima, na mesma coluna, que não seja de um plano da
     * frente — se for deste plano. Assim a água continua atrás do bloco que está
     * dentro dela, o chão continua atrás do HUD, o céu atrás da água. */
    "        int r = deco ? 2 : rank_of(code);\n"
    "        if (r < R) discard;\n"
    "        if (r > R) {\n"
    "            if (R == 2) discard;\n"   /* 3D-3h: a porta não continua atrás do chão (linhas pretas) */
    /* 3D-4i: no fundo, a origem da cor atrás dos objetos vem pré-calculada em 1x
     * (u_bs, a mesma regra do 3D-4g/4h); antes a busca rodava em cada ponto da tela
     * (com 4x e a tela cheia, milhões de buscas por quadro: lag nos barris) */
    "            if (R == 0) {\n"
    "                ivec2 bsz = textureSize(u_bs, 0);\n"
    "                vec2 fp = v_uv * vec2(bsz);\n"
    "                vec4 e = texelFetch(u_bs, clamp(ivec2(fp), ivec2(0), bsz - 1), 0);\n"
    "                int fa = int(round(e.a * 255.0));\n"
    "                if ((fa & 64) != 0) {\n"
    "                    vec2 src = vec2(float(int(round(e.r * 255.0)) + 256 * int(round(e.g * 255.0))),\n"
    "                                    float(int(round(e.b * 255.0)) + 256 * (fa & 3)));\n"
    "                    uv = (src + fract(fp)) / vec2(bsz);\n"
    "                } else shade = 0.55;\n"   /* nada de fundo perto: o próprio ponto escurecido */
    "            } else {\n"
    "            int step = max(1, sz.y / u_chi);\n"
    "            ivec2 q = p;\n"
    "            int rq = 99;\n"
    "            for (int k = 0; k < u_chi && q.y >= step; ++k) {\n"
    "                q.y -= step;\n"
    "                rq = rank_of(int(round(texelFetch(u_clsmap, q, 0).r * 255.0)));\n"
    "                if (rq <= R) break;\n"
    "            }\n"
    "            if (rq == R) uv.y = (float(q.y) + 0.5) / float(sz.y);\n"
    "            else discard;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    vec3 oc = texture(u_tex, uv).rgb * shade;\n"
    /* 3D-3e: véu da água em cada objeto dentro dela (sprites, blocos soltos, moedas) */
    "    if (u_alpha > 0.0 && (u_layer == 3 || u_layer == 4 || u_layer == 6)) {\n"
    "        ivec2 ws = textureSize(u_wp, 0);\n"
    "        vec4 w = texelFetch(u_wp, clamp(ivec2(v_uv * vec2(ws)), ivec2(0), ws - 1), 0);\n"
    "        int fl = int(round(w.r * 255.0)), row = int(round(w.g * 255.0));\n"
    "        if ((fl & 2) != 0) {\n"
    "            vec2 src = (fl & 4) != 0 ? v_uv : vec2(v_uv.x, (float(row) + 0.5) / float(ws.y));\n"
    "            oc = mix(oc, texture(u_bgt, src).rgb, u_alpha);\n"
    "        }\n"
    "    }\n"
    "    o_color = vec4(oc, 1.0);\n"
    "}\n";

/* Passo 3D-3c: água pré-calculada, 1 ponto por ponto da tela (largura base x 144).
 * r = bandeiras: 1 faixa das ondas (até 16 pontos abaixo do céu, só água no
 * caminho), 2 abaixo das ondas (véu), 4 corpo da água (cor própria), 8 algo dentro
 * da água (bloco, porta, sprite): no fundo vai a cor da água da linha g.
 * g = linha: do céu acima da onda (para tirar os pontos de céu; 255 = nenhum) ou
 * da água que dá a cor. */
const char* kWaterPrepFragment =
    "uniform sampler2D u_clsmap;\n"
    "uniform sampler2D u_col;\n"   /* 3D-3e: cores do fundo (só o fundo), para achar a crista das ondas */
    "uniform ivec2 u_boff;\n"   /* 3D-4d: onde começam os blocos de 16x16 (mundo) na imagem */
    "uniform float u_bw;\n"
    "uniform float u_ch;\n"
    "out vec4 o_color;\n"
    "ivec2 g_sz;\n"
    "vec3 col(int x, int y) {\n"
    "    ivec2 cs = textureSize(u_col, 0);\n"
    "    ivec2 p = clamp(ivec2((vec2(float(x), float(y)) + 0.5) * vec2(cs) / vec2(u_bw, u_ch)), ivec2(0), cs - 1);\n"
    "    return texelFetch(u_col, p, 0).rgb;\n"
    "}\n"
    "bool same(vec3 a, vec3 b) { vec3 d = abs(a - b); return d.r + d.g + d.b < 0.06; }\n"
    "int code_at(int x, int y) {\n"
    "    ivec2 p = clamp(ivec2((vec2(float(x), float(y)) + 0.5) * vec2(g_sz) / vec2(u_bw, u_ch)), ivec2(0), g_sz - 1);\n"
    "    return int(round(texelFetch(u_clsmap, p, 0).r * 255.0));\n"
    "}\n"
    "int rk(int x, int y) {\n"
    "    int code = code_at(x, y);\n"
    "    int c = code & 7, u = (code >> 3) & 3;\n"
    "    if (c == 3) c = u == 0 ? 1 : (u == 1 ? 4 : (u == 2 ? 5 : 6));\n"
    "    if (c == 1) return 0;\n"
    "    if (c == 5) return 1;\n"
    "    if (c == 6) return 2;\n"
    "    if (c == 4) return 3;\n"
    "    if (c == 2) return 4;\n"
    "    return -1;\n"
    "}\n"
    /* 3D-3h: a faixa das ondas é achada de baixo para cima: B = cor do corpo da
     * água (até 32 pontos abaixo do começo da água na coluna); wb = onde começa a
     * corrida de B; faixa = 10 pontos acima de wb e 2 abaixo; acima da faixa é ar
     * (céu pintado, ar da caverna). Sem ar (água começa já no corpo) não há faixa.
     * Devolve 0 ar, 1 faixa (ref = linha do céu/ar para tirar os pontos de céu, 255
     * = nenhuma), 4 corpo. */
    "int water_kind(int x, int y, out int ref) {\n"
    "    ref = 255;\n"
    "    int k = 1, q = 1;\n"
    /* 3D-4l: a subida atravessa objetos dentro da água (moedas, blocos) quando há
     * água logo acima deles (até 32 pontos): a superfície é a de verdade. Antes uma
     * fileira de moedas dentro da água virava \"superfície\" para o que estava
     * embaixo (as plantas viravam faixa das ondas, copiadas em profundidade) */
    "    for (int n = 0; n < 160 && k <= 128; ++n) {\n"
    "        if (y - k < 0) { q = -2; break; }\n"
    "        q = rk(x, y - k);\n"
    "        if (q == 1) { ++k; continue; }\n"
    "        if (q == 2 || q == 3) {\n"
    "            int j = k + 1;\n"
    "            while (j <= k + 32 && y - j >= 0 && (rk(x, y - j) == 2 || rk(x, y - j) == 3)) ++j;\n"
    "            if (j <= k + 32 && y - j >= 0 && rk(x, y - j) == 1) { k = j; continue; }\n"
    "        }\n"
    "        break;\n"
    "    }\n"
    "    if (k > 128) return 4;\n"
    "    int top = y - k + 1, yr = y - k;\n"
    "    int bot = y;\n"
    "    for (int yy = y + 1; float(yy) < u_ch && yy <= y + 64; ++yy) { if (rk(x, yy) != 1) break; bot = yy; }\n"
    "    int yb = min(bot, top + 32);\n"
    /* cor do corpo num ponto de água (se caiu num objeto, sobe até a água acima dele) */
    "    for (int i = 0; i < 32 && yb > top && rk(x, yb) != 1; ++i) --yb;\n"
    /* 3D-6f: água parada com fileiras de pontos no corpo (fase da floresta): B = a
     * cor que mais aparece em até 16 pontos de água a partir de yb (antes, a do
     * próprio yb, que podia cair num ponto), e a subida aceita 1 ponto diferente
     * no meio (para só em 2 seguidos) */
    "    vec3 cv[16]; int nv = 0;\n"
    "    for (int yy = yb; yy <= bot && nv < 16; ++yy) { if (rk(x, yy) == 1) { cv[nv] = col(x, yy); ++nv; } }\n"
    "    vec3 B = col(x, yb); int bn = 0;\n"
    "    for (int i = 0; i < nv; ++i) {\n"
    "        int n = 0;\n"
    "        for (int j = 0; j < nv; ++j) if (same(cv[i], cv[j])) ++n;\n"
    "        if (n > bn) { bn = n; B = cv[i]; }\n"
    "    }\n"
    "    while (yb > top && !same(col(x, yb), B)) --yb;\n"
    "    int wb = yb, miss = 0;\n"
    "    for (int yy = yb - 1; yy >= top; --yy) {\n"
    "        if (same(col(x, yy), B)) { wb = yy; miss = 0; }\n"
    "        else if (++miss >= 2) break;\n"
    "    }\n"
    "    if (wb - top < 2) return 4;\n"
    "    int bs = max(top, wb - 10);\n"
    "    if (y < bs) return 0;\n"
    "    if (y < wb + 2) { ref = bs > top ? bs - 1 : (q == 0 ? yr : 255); return 1; }\n"
    "    return 4;\n"
    "}\n"
    /* Saída: r = bandeiras (1 faixa das ondas, 2 véu, 4 corpo da água, 8 água atrás
     * de algo, 16 buraco, 32 chão sólido, 64 porta sólida), g = linha da cor da água
     * (ou a própria), b = coluna dela, relativa (+128; 255 = a própria), a = linha do céu da faixa. */
    "void main() {\n"
    "    g_sz = textureSize(u_clsmap, 0);\n"
    "    int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);\n"
    "    int r = rk(x, y);\n"
    "    int fl = 0, row = y, sx = -1, ref = 255;\n"
    "    if (r == 1) {\n"
    "        int w = water_kind(x, y, ref);\n"
    "        fl = w == 0 ? 0 : (w == 1 ? 1 : (4 | 2));\n"
    "    }\n"
    /* 3D-3g: buraco = ponto de bloco/porta/moeda com a cor do que está do lado de
     * fora e ligado a ele por uma linha reta da mesma cor (cantos de barril, a água
     * em volta da moeda): o plano não desenha, o de trás aparece */
    "    bool hole = false;\n"
    "    if (r == 2 || r == 3) {\n"
    "        vec3 c = col(x, y);\n"
    /* 3D-3j: na porta só para os lados (o escuro da caveira em cima abria um risco
     * no meio dela) */
    "        int nd = (r == 2 && (code_at(x, y) & 32) != 0) ? 2 : 4;\n"
    "        for (int dd = 0; dd < nd && !hole; ++dd) {\n"
    "            ivec2 st = dd == 0 ? ivec2(-1, 0) : (dd == 1 ? ivec2(1, 0) : (dd == 2 ? ivec2(0, -1) : ivec2(0, 1)));\n"
    /* 3D-4n: o caminho até o lado de fora atravessa objetos de outro tipo (uma
     * moeda logo acima de um bloco: antes ela fechava o caminho, e o azul da água
     * pintado em volta das plantas ficava sólido) e vai até 32 pontos */
    "            for (int i = 1; i <= 32; ++i) {\n"
    "                int xx = x + st.x * i, yy = y + st.y * i;\n"
    "                if (xx < 0 || yy < 0 || float(xx) >= u_bw || float(yy) >= u_ch) break;\n"
    "                int q = rk(xx, yy);\n"
    "                bool sm = same(col(xx, yy), c);\n"
    "                if (q == r) { if (!sm) break; continue; }\n"
    "                if (q == 2 || q == 3) continue;\n"
    "                if (sm && (q == 0 || q == 1)) hole = true;\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    if (r == 2 || r == 3 || r == 4) {\n"   /* 3D-3d: o HUD também (água atrás dele) */
    "        int ya = -1;\n"
    "        for (int yy = y - 1; yy >= 0; --yy) {\n"
    "            int q = rk(x, yy);\n"
    "            if (q == 1) { ya = yy; break; }\n"
    "            if (q < 2) break;\n"
    "        }\n"
    "        if (ya >= 0) {\n"
    "            row = ya;\n"
    "            bool below = false;\n"
    "            for (int yy = y + 1; float(yy) < u_ch; ++yy) {\n"
    "                int q = rk(x, yy);\n"
    "                if (q == 1) { row = yy; below = true; break; }\n"
    "                if (q < 2) break;\n"
    "            }\n"
    /* 3D-3e: véu só no que está solto dentro da água (água em cima e embaixo); o
     * chão do fundo da água e o HUD não */
    "            fl = (r != 4 && below) ? (8 | 2) : 8;\n"
    /* 3D-4l: a cor da água atrás do objeto vem de cima quando lá é o corpo da água
     * (embaixo pode haver enfeites pintados na água, como as plantas da caverna) */
    "            { int rfa; if (water_kind(x, ya, rfa) == 4) row = ya; }\n"
    "        } else {\n"
    /* 3D-3k (no lugar do 3D-3i/3j): atrás de um objeto (chão, barril, bloco) a água
     * e as ondas continuam só 1 tile (8 pontos) a partir da beira da água, copiando
     * o tile vizinho (o ponto 8 à frente, do lado da água) */
    "            int best = 999, side = 0;\n"
    "            for (int sd = -1; sd <= 1; sd += 2) {\n"
    "                for (int i = 1; i <= 8; ++i) {\n"
    "                    int xx = x + sd * i;\n"
    "                    if (xx < 0 || float(xx) >= u_bw) break;\n"
    "                    int q = rk(xx, y);\n"
    "                    if (q == 1) { if (i < best) { best = i; side = sd; } break; }\n"
    "                    if (q < 2) break;\n"
    "                }\n"
    "            }\n"
    "            int s2 = x + side * 8;\n"
    "            if (side != 0 && s2 >= 0 && float(s2) < u_bw && rk(s2, y) == 1) {\n"
    "                int rf;\n"
    "                int w = water_kind(s2, y, rf);\n"
    "                if (w == 4) { fl = 8; sx = s2; }\n"
    "                else if (w == 1) { fl = 8 | 1; sx = s2; ref = rf; }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    /* 3D-3k: enfeite da porta (a caveira da torre): ponto de fundo até 16 pontos
     * acima de uma porta (só por fundo no caminho) com cor diferente da parede dos
     * dois lados da porta, na mesma linha: vai junto com a porta */
    "    if (r == 0) {\n"
    "        int yd = -1;\n"
    "        for (int i = 1; i <= 16; ++i) {\n"
    "            int yy = y + i;\n"
    "            if (float(yy) >= u_ch) break;\n"
    "            int q = rk(x, yy);\n"
    "            if (q == 2 && (code_at(x, yy) & 32) != 0) { yd = yy; break; }\n"
    "            if (q != 0) break;\n"
    "        }\n"
    "        if (yd >= 0) {\n"
    "            int xl = x, xr = x;\n"
    "            for (int i = 1; i <= 32; ++i) { int xx = x - i; if (xx < 0 || rk(xx, yd) != 2 || (code_at(xx, yd) & 32) == 0) break; xl = xx; }\n"
    "            for (int i = 1; i <= 32; ++i) { int xx = x + i; if (float(xx) >= u_bw || rk(xx, yd) != 2 || (code_at(xx, yd) & 32) == 0) break; xr = xx; }\n"
    "            vec3 c = col(x, y);\n"
    "            vec3 w1 = col(max(xl - 2, 0), y), w2 = col(min(xr + 2, int(u_bw) - 1), y);\n"
    "            if (!same(c, w1) && !same(c, w2)) fl |= 128 | 64;\n"
    "        }\n"
    "    }\n"
    /* 3D-4d: moeda (bit 7 da classe): o fundo pintado em volta dela é a cor do canto
     * de fora do bloco de 16x16 (o canto do quarto onde o ponto está); ponto com essa
     * cor vira buraco (no lugar do 3D-4c, que usava a cor 0 e deixava o contrário) */
    "    if (r == 2) {\n"
    "        int cc = code_at(x, y);\n"
    "        if ((cc & 64) != 0) hole = true;\n"   /* 3D-4j: bloco desenhado como vazio */
    "        if ((cc & 128) != 0) {\n"
    "            int ax = x - u_boff.x, ay = y - u_boff.y;\n"
    "            int qx = ax - 16 * int(floor(float(ax) / 16.0)), qy = ay - 16 * int(floor(float(ay) / 16.0));\n"
    "            int kx = qx < 8 ? x - qx : x - qx + 15, ky = qy < 8 ? y - qy : y - qy + 15;\n"
    "            if (kx >= 0 && ky >= 0 && float(kx) < u_bw && float(ky) < u_ch && rk(kx, ky) == 2 &&\n"
    "                (code_at(kx, ky) & 128) != 0 && same(col(kx, ky), col(x, y))) hole = true;\n"
    "        }\n"
    "    }\n"
    "    if (hole) fl |= 16;\n"
    "    bool spr = (code_at(x, y) & 7) == 3;\n"   /* 3D-4o: sprite não ganha volume de bloco */
    "    if (r == 3 && !hole && !spr) fl |= 32;\n"   /* 3D-3h: ponto sólido (chão) para a extrusão por ponto */
    "    if (r == 2 && !hole && !spr && (code_at(x, y) & 32) != 0) fl |= 64;\n"   /* 3D-3i: porta sólida */
    /* 3D-4a: coluna guardada relativa ao ponto (+128), a imagem pode ter mais de 255 */
    "    o_color = vec4(float(fl) / 255.0, float(row) / 255.0, float(sx < 0 ? 255 : sx - x + 128) / 255.0, float(ref) / 255.0);\n"
    "}\n";
/* Passo 3D-4i: origem do fundo atrás dos objetos, 1 ponto por ponto 1x da imagem
 * (mesma regra do 3D-4g/4h: primeiro de lado na mesma linha, até 32 pontos e
 * deslocado de 16 em 16; senão subindo, e ao passar por um objeto olha os lados
 * dessa linha, até 24 pontos). Saída: r,g = x (lo, hi); b = y lo; a = y hi (bits
 * 0-1) | 64 achou. Só para os pontos que não são fundo. */
const char* kBgSrcFragment =
    "uniform sampler2D u_clsmap;\n"
    "uniform sampler2D u_wp;\n"
    "uniform ivec2 u_cx;\n"   /* 3D-2m: colunas onde vale procurar o fundo (fora das fases: só o meio da tela) */
    "out vec4 o_color;\n"
    "ivec2 g_sz;\n"
    "ivec2 g_cs;\n"
    "int rank_at(int x, int y) {\n"
    "    if (x < u_cx.x || x >= u_cx.y) return -1;\n"   /* 3D-2m: as laterais (fundo ambiente) não servem de fundo */
    "    ivec2 p = clamp(ivec2((vec2(float(x), float(y)) + 0.5) * vec2(g_cs) / vec2(g_sz)), ivec2(0), g_cs - 1);\n"
    "    int code = int(round(texelFetch(u_clsmap, p, 0).r * 255.0));\n"
    "    int c = code & 7, u = (code >> 3) & 3;\n"
    "    if (c == 3) c = u == 0 ? 1 : (u == 1 ? 4 : (u == 2 ? 5 : 6));\n"
    "    int r = -1;\n"
    "    if (c == 1) r = 0; else if (c == 5) r = 1; else if (c == 6) r = 2; else if (c == 4) r = 3; else if (c == 2) r = 4;\n"
    "    if (r == 0 && (int(round(texelFetch(u_wp, ivec2(x, y), 0).r * 255.0)) & 128) != 0) r = 2;\n"   /* enfeite da porta */
    "    return r;\n"
    "}\n"
    "void main() {\n"
    "    g_sz = textureSize(u_wp, 0);\n"
    "    g_cs = textureSize(u_clsmap, 0);\n"
    "    int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);\n"
    "    int sx = x, sy = y;\n"
    "    bool found = false;\n"
    "    int r0 = rank_at(x, y);\n"
    "    if (r0 > 0) {\n"
    "        int hbest = 999, hside = 0;\n"
    /* 3D-4j: de lado só atrás de objetos (moeda, bloco, porta); atrás da água e do
     * HUD a busca é para cima, como antes do 3D-4g (de lado, no começo da fase, a
     * água pegava o preto da lateral fora da fase) */
    "        if (r0 == 2 || r0 == 3) for (int sd = -1; sd <= 1; sd += 2) {\n"
    "            for (int i = 1; i <= 32 && i < hbest; ++i) {\n"
    "                int hx = x + sd * i;\n"
    "                if (hx < 0 || hx >= g_sz.x) break;\n"
    "                int rh = rank_at(hx, y);\n"
    "                if (rh == 0) { hbest = i; hside = sd; break; }\n"
    "                if (rh < 0) break;\n"
    "            }\n"
    "        }\n"
    "        if (hside != 0) {\n"
    "            int off = 16 * ((hbest + 15) / 16);\n"
    "            sx = x + hside * off;\n"
    "            if (sx < 0 || sx >= g_sz.x || rank_at(sx, y) != 0) sx = x + hside * hbest;\n"
    "            found = true;\n"
    "        } else {\n"
    "            int gy = -1;\n"   /* 3D-4k: primeira linha de chão acima (teto da caverna) */
    /* 3D-4k: limites (caverna: quase tudo é chão, e a busca de lado em cada linha
     * ficava pesada): sobe até 96 linhas e olha os lados só nas 16 primeiras
     * linhas de objeto */
    "            int ob = 0;\n"
    "            for (int yy = y - 1; yy >= 0 && yy >= y - 96 && !found; --yy) {\n"
    "                int rq = rank_at(x, yy);\n"
    "                if (rq == 0) { sy = yy; found = true; break; }\n"
    "                if (rq < 0) break;\n"
    "                if (rq == 3 && gy < 0) gy = yy;\n"
    "                if (rq >= 2 && rq <= 3 && ob++ < 16) {\n"
    "                    for (int i = 1; i <= 24 && !found; ++i) {\n"
    "                        for (int sd = -1; sd <= 1; sd += 2) {\n"
    "                            int hx = x + sd * i;\n"
    "                            if (hx < 0 || hx >= g_sz.x) continue;\n"
    "                            if (rank_at(hx, yy) == 0) { sx = hx; sy = yy; found = true; break; }\n"
    "                        }\n"
    "                    }\n"
    "                }\n"
    "            }\n"
    /* 3D-4k: sem fundo acima nem dos lados (debaixo de um teto, caverna): atrás vai
     * o próprio teto, repetido de 16 em 16 (o desenho dos blocos), em vez da cópia
     * escurecida do ponto (que mostrava a água, as moedas e o HUD de novo lá atrás) */
    "            if (!found && gy >= 0) {\n"
    "                sy = gy - 15 + (((y - gy) % 16 + 16) % 16);\n"
    "                if (sy < 0 || rank_at(x, sy) != 3) sy = gy;\n"
    "                found = true;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    o_color = vec4(float(sx & 255) / 255.0, float(sx >> 8) / 255.0, float(sy & 255) / 255.0,\n"
    "                   float((sy >> 8) | (found ? 64 : 0)) / 255.0);\n"
    "}\n";
GLuint g_bsprog = 0, g_fbo_bs = 0, g_out_bs = 0;
int g_bs_w = 0, g_bs_h = 0;

GLuint g_wprog = 0, g_fbo_w = 0, g_out_w = 0;
int g_w_w = 0;
GLint g_wloc_bw = -1, g_dloc_alpha = -1, g_dloc_lshade = -1;
GLint g_wloc_ch = -1, g_bloc_ch = -1, g_dloc_uvr = -1, g_dloc_chi = -1;   /* 3D-4a */
GLint g_wloc_boff = -1, g_dloc_nospr = -1, g_dloc_halfp = -1;
int g_3d_boffx = 0, g_3d_boffy = 0;   /* 3D-4d: canto dos blocos de 16x16 na imagem */
int g_w_h = 0;

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
    /* 3D-2h: laterais com o fundo ambiente (desfoque) ficam no plano do fundo, não longe */
    "        else if (u_cls != 0) c = uvec3((u_blackfill != 0 && u_ambon == 0) ? 0u : 1u, 0u, 0u);\n"
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
    "uniform int u_mono;\n"         /* modo de cor original (P&B): paleta do sprite pelo bit 4 do OAM */
    "uniform ivec4 u_mono_ramp[4];\n"  /* passo 31a: os 4 tons P&B (R, G, B) para os tiles embutidos */
    "uniform int u_out;\n"   /* passo 3D-1: 0 = cor, 1 = só o fundo (sem sprites), 2 = classe */
    "uniform int u_winfloor;\n"   /* passo 3D-2g: janela = chão (g00) */
    "uniform int u_marks;\n"   /* passo 3D-2b: fora das fases, tipo marcado à mão por tile (linha 16 de u_slot) */
    "uniform int u_ext;\n"   /* passo 3D-4a: desenha a fase além da tela (tiles do mundo da CPU) */
    "uniform usampler2D u_extt;\n"   /* R16UI: tile | atributo << 8 (bit 12 = há tile) */
    "uniform ivec4 u_extg;\n"   /* tile do mundo do canto (x, y), largura, altura em tiles */
    "uniform ivec4 u_gh[48];\n"   /* passo 18b: inimigos além da tela só como imagem (x, y, tile, atributos) */
    "uniform int u_ngh;\n"
    "uniform ivec4 u_spr[40];\n"   /* passo 16d: por sprite: X verdadeiro (-32768 = não sabe), Y da tela (17a: verdadeiro), tile, atributos do OAM */
    "uniform ivec3 u_exto;\n"   /* EX, EY (pontos além da tela), lateral (48 com widescreen) */
    "out vec4 o_color;\n"
    "int g_y;\n"
    "int g_fid = -1;\n"
    "bool g_half = false;\n"   /* 3D-2k: chão com meia extrusão (marca 4) */   /* 3D-6c: bloco da fase deste ponto (para o plano da frente) */
    "int g_kind = 0;\n"   /* 3D-2: o fundo deste ponto é 0 fundo, 1 chão (bloco sólido), 2 água, 3 porta */
    /* classe do ponto (3D-1): 1 = fundo, 2 = janela (HUD), 3 = sprite; 3D-2: 4 = chão,
     * e +8 no sprite quando o que está atrás dele é chão */
    "bool front_block(int id);\n"   /* 3D-6c (abaixo) */
    "void emit(uvec3 col, uvec3 bgc, int cls) {\n"
    "    int code = cls;\n"
    "    if (cls == 1 && g_kind == 1) code = 4;\n"
    "    if (cls == 1 && g_kind == 2) code = 5;\n"
    "    if (cls == 1 && g_kind == 3) code = 6;\n"   /* 3D-2c: porta */
    "    if (cls == 1 && g_kind == 4) code = 6 + 32;\n"
    "    if (cls == 1 && g_kind == 5) code = 6 + 128;\n"   /* 3D-3i: porta de verdade (bit 5: extrudida até o fundo) */
    /* 3D-4j: tipo 6 = bloco desenhado como vazio enquanto a fase diz outra coisa
     * (caixa batida: o jogo desenha o bloco $7F enquanto o sprite sobe e desce):
     * plano da porta com o bit 6 = transparente */
    "    if (cls == 1 && g_kind == 6) code = 6 + 64;\n"
    "    int gk = g_kind == 6 ? 0 : g_kind;\n"
    "    if (cls == 3) code = 3 + 8 * (gk >= 4 ? 3 : gk) + (gk == 4 ? 32 : 0);\n"
    /* 3D-4l: sprite sobre o bloco desenhado como vazio (a caixa subindo): \"atrás\"
     * dele é transparente também (antes o fundo mostrava a peça azul do bloco vazio) */
    "    if (cls == 3 && g_kind == 6) code = 3 + 24 + 64;\n"
    /* 3D-6c: verde = bloco de terreno que vai para o plano da frente (sem caixas,
     * quebráveis, gelo, interruptores etc.) */
    /* 3D-2k: azul = chão com meia extrusão */
    "    if (u_out == 2) o_color = vec4(float(code) / 255.0, (cls == 1 && g_kind == 1 && front_block(g_fid)) ? 1.0 : 0.0, (cls == 1 && g_kind == 1 && g_half) ? 1.0 : 0.0, 1.0);\n"
    "    else o_color = vec4(vec3(u_out == 1 ? bgc : col) / 255.0, 1.0);\n"
    "}\n"
    /* Blocos da fase que viram chão no 3D (disassembly, bank02 ActBGColi_IsSolid):
     * $00-$27 sólidos; $28-$3F caixas, quebráveis, gelo, areia etc. (menos $37 vazio e
     * $3D sem uso); $40-$43 plataformas; quebráveis na água; espinhos $5D-$5F. */
    /* 3D-6c: blocos do plano da frente: sólidos comuns $00-$27, areia $3E/$3F e
     * plataformas $40-$43 (fora: caixa de item $28, quebráveis $29-$2E, mola $2F,
     * esteiras, interruptores, gelo, temporizados, caixa usada $3C) */
    "bool front_block(int id) { return id >= 0 && (id < 0x28 || id == 0x3E || id == 0x3F || (id >= 0x40 && id < 0x44)); }\n"
    "bool ground_block(int id) {\n"
    "    if (id < 0x28) return true;\n"
    "    if (id < 0x40) return id != 0x37 && id != 0x3D;\n"
    "    if (id < 0x44) return true;\n"
    "    if (id == 0x50 || id == 0x51 || id == 0x52 || id == 0x54) return true;\n"
    "    if (id == 0x59 || id == 0x5A) return true;\n"   /* 3D-4m: espinho na água (as plantas que matam) */
    "    return id >= 0x5D && id <= 0x5F;\n"
    "}\n"
    /* 3D-2c/2d: no plano do chão, sem ser chão: porta ($48, $4B, $5B) e escada ($44, $45) */
    "bool door_block(int id) { return id == 0x48 || id == 0x4B || id == 0x5B || id == 0x44 || id == 0x45; }\n"
    /* 3D-3d: moedas ($46, $47, $53 na água): objetos no plano da porta, não fundo nem água */
    "bool coin_block(int id) { return id == 0x46 || id == 0x47 || id == 0x53; }\n"
    "bool water_block(int id) { return id >= 0x4A && id <= 0x5B && !ground_block(id) && !door_block(id); }\n"
    /* 3D-2b: tipo pelo bloco e pelo de baixo (as ondas ficam num bloco vazio logo acima da água) */
    "int kind_of(int id, int down) {\n"
    "    if (ground_block(id)) return 1;\n"
    "    if (id == 0x48 || id == 0x4B || id == 0x5B) return 4;\n"   /* 3D-3i: porta (não escada/moeda) */
    "    if (coin_block(id)) return 5;\n"   /* 3D-4d: moeda (plano da porta, bit 7) */
    "    if (door_block(id)) return 3;\n"
    "    if (water_block(id)) return 2;\n"
    /* bloco vazio sobre a porta (o alto dela) e sobre a água (as ondas) */
    "    if (id >= 0x60 && (down == 0x48 || down == 0x4B)) return 4;\n"   /* alto da porta (não da escada) */
    "    if (id >= 0x60 && down >= 0 && (water_block(down) || down == 0x59 || down == 0x5A)) return 2;\n"   /* 3D-4m: onda sobre o espinho */
    "    return 0;\n"
    "}\n"
    "int s16(uint lo, uint hi) { int v = int(lo | (hi << 8u)); return v >= 32768 ? v - 65536 : v; }\n"
    "int lay(int bx, int by) {\n"
    "    if (bx < 0 || by < 0 || bx >= 256 || by >= 32) return -1;\n"
    "    return int(texelFetch(u_layout, ivec2(bx, by), 0).r & 0x7Fu);\n"
    "}\n"
    /* 3D-4j: a CPU marca com 255 o bloco que está desenhado como vazio ($7F) */
    "bool lay_gone(int bx, int by) {\n"
    "    if (bx < 0 || by < 0 || bx >= 256 || by >= 32) return false;\n"
    "    return texelFetch(u_layout, ivec2(bx, by), 0).r == 255u;\n"
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
    /* 3D-3l: plataforma ($40-$43, sólida só por cima): o tile de cima é chão, o de
     * baixo (pernas e a sombra pintada na parede) vai para o plano da porta, atrás
     * dos sprites e sem volume */
    "int plat_split_w(int k, int id, int wy) {\n"
    "    if (k != 1 || id < 0x40 || id > 0x43) return k;\n"
    "    return (wy & 15) >= 8 ? 3 : 1;\n"
    "}\n"
    "int plat_split(int k, int id, int y) {\n"
    "    uvec4 l2 = texelFetch(u_line, ivec2(2, y), 0);\n"
    "    return plat_split_w(k, id, s16(l2.b, l2.a));\n"
    "}\n"
    "int kind_at(int x, int y) {\n"
    "    if (u_level == 0) return 0;\n"
    "    ivec2 b = blk_at(x, y);\n"
    "    if (b.x < 0) return 0;\n"
    "    if (lay_gone(b.x, b.y)) return 6;\n"
    "    int id = lay(b.x, b.y);\n"
    "    g_fid = id;\n"
    "    int k = kind_of(id, lay(b.x, b.y + 1));\n"
    "    if (k == 0 && id >= 0x60 && wave_over(b.x, b.y)) k = 2;\n"
    "    return plat_split(k, id, y);\n"
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
    "    return texelFetch(u_atlas, ivec3(int(m.r) + hx, int(m.g) + hy, int(m.b) & 0x3FFF), 0);\n"
    "}\n"
    /* Recoloração proporcional (hd_recolor_setup/apply) */
    "struct Rc { bool on; uvec3 mul; uvec3 add; };\n"
    "Rc rc_setup(uint s1, int idx, uvec3 cur) {\n"
    "    Rc r; r.on = false; r.mul = uvec3(65536u); r.add = uvec3(0u);\n"
    "    if (u_mono != 0) return r;\n"   /* passo 31a: no P&B os mods não são recoloridos pela paleta */
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
    /* Modo original (P&B), igual a ppu_mono_tint_rgba: no tile HD embutido (texturas
     * originais, bit 0x4000 do meta) a cor do PNG cai num dos 4 tons da rampa; os
     * mods da pasta ficam com as cores deles (passo 31a). ocol: paleta do sprite na
     * tabela da linha (no P&B, 0 = OBP0 e 1 = OBP1 pelo bit 4 do OAM; senão, a CGB). */
    "uvec3 hdm(uint s1, uvec3 c) {\n"
    "    if (u_mono == 0) return c;\n"
    "    int s = int(s1) - 1;\n"
    "    uvec4 m = texelFetch(u_meta, ivec2(s & 255, s >> 8), 0);\n"
    "    if ((m.b & 0x4000u) == 0u) return c;\n"
    "    uint l = (c.r * 77u + c.g * 150u + c.b * 29u) >> 8u;\n"
    "    int k = l >= 213u ? 0 : (l >= 128u ? 1 : (l >= 43u ? 2 : 3));\n"
    "    return uvec3(u_mono_ramp[k].rgb);\n"
    "}\n"
    "int ocol(int fl) { return u_mono != 0 ? ((fl & 0x10) != 0 ? 1 : 0) : (fl & 7); }\n"
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
    /* 3D-4a: com a fase além da tela, as laterais sem o escurecimento da borda (senão
     * fica uma faixa escura entre a tela e o resto da fase) */
    "    if (u_ext == 2) { fade = false; rgba = obj_raw != 0 ? obj_rgba : bg_rgba; }\n"
    /* 3D-1: sem HD o ponto é o rgba; só o fundo = rgba sem sprite, ou o fundo escurecido */
    "    uvec3 bg1 = obj_raw != 0 ? fade3(fade, m, bg_rgba) : rgba;\n"
    "    bool hudl = (texelFetch(u_line, ivec2(1, y), 0).a & 1u) != 0u;\n"   /* 3D-1b: faixa do mapa regional */
    "    g_kind = (u_level != 0 && !hudl && hs.z != 0u) ? kind_of(int(hs.z) - 1, int(hs.w) - 1) : 0;\n"
    "    g_fid = (u_level != 0 && !hudl && hs.z != 0u) ? int(hs.z) - 1 : -1;\n"   /* 3D-2 */
    "    if (u_level != 0 && !hudl && hs.z != 0u) g_kind = plat_split(g_kind, int(hs.z) - 1, y);\n"   /* 3D-3l */
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
    "        if (h.a != 0u) c = fade3(fade, m, hdm(bs, rc_apply(rc_setup(bs, bg_raw, bg_rgba), h)));\n"
    "    }\n"
    "    uvec3 cbg = c;\n"
    "    int cls = clsb;\n"
    "    if (ot) {\n"
    "        if (os != 0u) {\n"
    "            uvec4 h = hd_texel(os, int(obj_uvf & 7u), int((obj_uvf >> 3) & 7u), uvf_flags(obj_uvf), sx, sy, n);\n"
    "            if (h.a >= 128u) { c = hdm(os, rc_apply(rc_setup(os, obj_raw, obj_rgba), h)); cls = 3; }\n"
    "        } else {\n"
    "            c = obj_rgba; cls = 3;\n"
    "        }\n"
    "    }\n"
    "    emit(c, cbg, cls);\n"
    "}\n"
    /* Passo 3D-4a: ponto da fase além da tela (o 3D mostra mais do que a tela). A
     * posição no mundo vem da linha mais perto (dentro da tela, a própria linha);
     * o tile e o atributo vêm da CPU (mesma cor das laterais); 3D-4p: com os
     * tiles HD do pacote, como no centro (mesmo tile/paleta = mesmo PNG). */
    "void ext_main() {\n"
    "    int n = u_scale;\n"
    "    int X = int(gl_FragCoord.x), Y = int(gl_FragCoord.y);\n"
    "    int cx = X / n, cy = Y / n;\n"
    "    int sy = cy - u_exto.y;\n"
    "    int ly = clamp(sy, 0, 143);\n"
    "    g_y = ly;\n"
    "    uvec4 l2 = texelFetch(u_line, ivec2(2, ly), 0);\n"
    "    int wx = s16(l2.r, l2.g) - u_exto.z - u_exto.x + cx;\n"
    "    int wy = s16(l2.b, l2.a) + (sy - ly);\n"
    "    int tx = (wx >> 3) - u_extg.x, ty = (wy >> 3) - u_extg.y;\n"
    "    uint e = 0u;\n"
    "    if (wx >= 0 && wy >= 0 && tx >= 0 && ty >= 0 && tx < u_extg.z && ty < u_extg.w) e = texelFetch(u_extt, ivec2(tx, ty), 0).r;\n"
    "    if ((e & 0x1000u) == 0u) {\n"
    "        o_color = vec4(0.0, 0.0, 0.0, 1.0);\n"   /* nada: preto / classe 0 (longe) */
    "        return;\n"
    "    }\n"
    "    int tile = int(e & 255u), attr = int(e >> 8u);\n"
    "    int lcdc = int(texelFetch(u_line, ivec2(0, ly), 0).r);\n"
    "    int px = wx & 7, py = wy & 7;\n"
    "    if ((attr & 0x20) != 0) px = 7 - px;\n"
    "    if ((attr & 0x40) != 0) py = 7 - py;\n"
    "    int tidx = (lcdc & 0x10) != 0 ? tile : (tile < 128 ? 256 + tile : tile);\n"
    "    int raw = tpix((attr & 0x08) != 0 ? 1 : 0, tidx, px, py);\n"
    "    uvec3 c = pal((attr & 7) * 4 + raw, ly);\n"
    "    if (u_hd != 0) {\n"
    "        uint s1 = slot_of(0, attr & 7, ((attr & 0x08) != 0 ? 384 : 0) + tidx);\n"
    "        if (s1 != 0u) {\n"
    "            uvec4 h = hd_texel(s1, wx & 7, wy & 7, attr & 0x60, X - cx * n, Y - cy * n, n);\n"
    "            if (h.a != 0u) c = hdm(s1, rc_apply(rc_setup(s1, raw, c), h));\n"
    "        }\n"
    "    }\n"
    "    g_kind = 0;\n"
    "    if (u_level != 0) {\n"
    "        int bx = wx >> 4, by = wy >> 4;\n"
    "        int id = lay(bx, by);\n"
    "        g_kind = kind_of(id, lay(bx, by + 1));\n"
    "        g_fid = lay_gone(bx, by) ? -1 : id;\n"
    "        if (g_kind == 0 && id >= 0x60 && wave_over(bx, by)) g_kind = 2;\n"
    "        g_kind = plat_split_w(g_kind, id, wy);\n"
    "        if (lay_gone(bx, by)) g_kind = 6;\n"
    "    }\n"
    /* Passo 16d: sprites dos atores além da tela (o jogo agora desenha os atores
     * até o fim da imagem 3D; o X verdadeiro vem do 16a). Só fora da tela larga
     * (dentro dela vale a cópia da tela). */
    "    int scr = cx - u_exto.x - u_exto.z;\n"
    "    bool outscr = sy < 0 || sy >= 144 || scr < -u_exto.z || scr >= 160 + u_exto.z;\n"   /* 17a: também acima/abaixo */
    "    if (outscr && (lcdc & 0x02) != 0) {\n"
    "        int h = (lcdc & 0x04) != 0 ? 16 : 8;\n"
    "        int oraw = 0, opal = 0, odm = 0, ou = 0, ov = 0, ofl = 0, ogi = 0;\n"
    "        bool obehind = false;\n"
    "        for (int i = 0; i < 40; ++i) {\n"
    "            int tx = u_spr[i].x;\n"
    "            if (tx == -32768) continue;\n"
    "            int p = scr - tx;\n"
    "            if (p < 0 || p >= 8) continue;\n"
    "            int spy = u_spr[i].y;\n"
    "            if (sy < spy || sy >= spy + h) continue;\n"
    "            int fl = u_spr[i].w;\n"
    "            int line = sy - spy;\n"
    "            if ((fl & 0x40) != 0) line = h - 1 - line;\n"
    "            int t0 = u_spr[i].z;\n"
    "            int t = h == 16 ? ((t0 & 0xFE) + (line >> 3)) : t0;\n"
    "            int bank = (fl & 0x08) != 0 ? 1 : 0;\n"
    "            int r = tpix(bank, t, (fl & 0x20) != 0 ? 7 - p : p, line & 7);\n"
    "            if (r == 0) continue;\n"
    "            oraw = r; opal = fl & 7; odm = ocol(fl); obehind = (fl & 0x80) != 0;\n"
    "            ou = p; ov = (sy - spy) & 7; ofl = fl & 0x60; ogi = bank * 384 + t;\n"
    "            break;\n"   /* o de índice menor fica por cima */
    "        }\n"
    /* 18b: sem sprite do jogo, os fantasmas (atrás dos de verdade) */
    "        for (int i = 0; i < 48 && oraw == 0; ++i) {\n"
    "            if (i >= u_ngh) break;\n"
    "            ivec4 g = u_gh[i];\n"
    "            int p = scr - g.x;\n"
    "            if (p < 0 || p >= 8) continue;\n"
    "            if (sy < g.y || sy >= g.y + h) continue;\n"
    "            int fl = g.w;\n"
    "            int line = sy - g.y;\n"
    "            if ((fl & 0x40) != 0) line = h - 1 - line;\n"
    "            int t = h == 16 ? ((g.z & 0xFE) + (line >> 3)) : g.z;\n"
    "            int bank = (fl & 0x08) != 0 ? 1 : 0;\n"
    "            int r = tpix(bank, t, (fl & 0x20) != 0 ? 7 - p : p, line & 7);\n"
    "            if (r == 0) continue;\n"
    "            oraw = r; opal = fl & 7; odm = ocol(fl); obehind = (fl & 0x80) != 0;\n"
    "            ou = p; ov = (sy - g.y) & 7; ofl = fl & 0x60; ogi = bank * 384 + t;\n"
    "        }\n"
    "        if (oraw != 0 && !(raw != 0 && (lcdc & 0x01) != 0 && ((attr & 0x80) != 0 || obehind))) {\n"
    "            uvec3 oc = pal(32 + odm * 4 + oraw, ly);\n"
    "            bool show = true;\n"
    "            if (u_hd != 0) {\n"
    "                uint os = slot_of(1, opal, ogi);\n"
    "                if (os != 0u) {\n"
    "                    uvec4 ho = hd_texel(os, ou, ov, ofl, X - cx * n, Y - cy * n, n);\n"
    "                    if (ho.a >= 128u) oc = hdm(os, rc_apply(rc_setup(os, oraw, oc), ho)); else show = false;\n"
    "                }\n"
    "            }\n"
    "            if (show) { emit(oc, c, 3); return; }\n"
    "        }\n"
    "    }\n"
    "    emit(c, c, 1);\n"
    "}\n"
    "void main() {\n"
    "    if (u_ext == 1) { ext_main(); return; }\n"
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
    /* 3D-2b; 3D-2f: também na janela (as ondas do g00 são janela): o tile marcado
     * deixa de ser HUD */
    "    bool mkwin = false;\n"
    "    if (u_marks != 0 && !hudl) { int mk = int(texelFetch(u_slot, ivec2(bgi, 16), 0).r); if (mk >= 1 && mk <= 4) { g_kind = mk == 4 ? 1 : mk; g_half = mk == 4; mkwin = inwin; } }\n"
    /* 3D-2g; 3D-2i: na janela do g00, o céu (cor 0 do tile) fica no plano do fundo,
     * junto das laterais; só o desenhado (ondas, nuvens) vira chão */
    "    if (u_winfloor != 0 && inwin && !mkwin && !hudl) { g_kind = bgraw != 0 ? 1 : 0; mkwin = true; }\n"
    "    bool hudpix = (inwin && !mkwin) || hudl;\n"
    /* sprites: todos os 40; vencedor e a primeira caixa transparente (HD) */
    "    int oraw = 0, opal = 0, odm = 0, osx = 999, oidx = 99, ou = 0, ov = 0, ofl = 0, ogi = 0;\n"
    "    bool obehind = false;\n"
    "    bool box = false;\n"
    "    int xu = 0, xv = 0, xfl = 0, xpal = 0, xdm = 0, xgi = 0;\n"
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
    "                if (!box) { box = true; xu = p; xv = (y - spy) & 7; xfl = fl & 0x60; xpal = fl & 7; xdm = ocol(fl);\n"
    "                            xbehind = (fl & 0x80) != 0; xgi = bank * 384 + t; }\n"
    "                continue;\n"
    "            }\n"
    "            if (oraw != 0) {\n"
    "                bool wins = opri ? (spx < osx || (spx == osx && i < oidx)) : (i < oidx);\n"
    "                if (!wins) continue;\n"
    "            }\n"
    "            oraw = raw; opal = fl & 7; odm = ocol(fl); obehind = (fl & 0x80) != 0; osx = spx; oidx = i;\n"
    "            ou = p; ov = (y - spy) & 7; ofl = fl & 0x60; ogi = bank * 384 + t;\n"
    "        }\n"
    "    }\n"
    "    bool useobj = oraw != 0;\n"
    "    if (useobj && bgraw != 0 && (lcdc & 0x01) != 0 && (bgpri || obehind)) useobj = false;\n"
    "    uvec3 bgc1 = pal(bgpal * 4 + bgraw, y);\n"
    "    uvec3 c1 = useobj ? pal(32 + odm * 4 + oraw, y) : bgc1;\n"
    "    int cls1 = useobj ? 3 : (hudpix ? 2 : 1);\n"
    "    if (u_hd == 0) { emit(c1, bgc1, cls1); return; }\n"
    /* HD (render_line com g_hd_scale > 1) */
    "    uint bs = slot_of(0, bgpal, bgi);\n"
    "    uint os = 0u;\n"
    "    bool ot = false;\n"
    "    int cu = 0, cv = 0, cfl = 0, cpal = 0, cdm = 0;\n"
    "    bool cbehind = false;\n"
    "    if (oraw != 0) {\n"
    "        ot = true; cu = ou; cv = ov; cfl = ofl; cpal = opal; cdm = odm; cbehind = obehind;\n"
    "        os = slot_of(1, opal, ogi);\n"
    "    } else if (box) {\n"
    "        os = slot_of(1, xpal, xgi);\n"
    "        if (os != 0u) { ot = true; cu = xu; cv = xv; cfl = xfl; cpal = xpal; cdm = xdm; cbehind = xbehind; }\n"
    "    }\n"
    "    if (bs == 0u && os == 0u) { emit(c1, bgc1, cls1); return; }\n"
    "    uvec3 bg_orig = pal(bgpal * 4 + bgraw, y);\n"
    "    uvec3 obj_orig = oraw != 0 ? pal(32 + cdm * 4 + oraw, y) : uvec3(0u);\n"
    "    bool blocked = bgraw != 0 && (lcdc & 0x01) != 0 && (bgpri || cbehind);\n"
    "    bool can_win = ot && !blocked;\n"
    "    uvec3 c = bg_orig;\n"
    "    if (bs != 0u) {\n"
    "        uvec4 hb = hd_texel(bs, bu, bv, attr & 0x60, sx, sy, n);\n"
    "        if (hb.a != 0u) c = hdm(bs, rc_apply(rc_setup(bs, bgraw, bg_orig), hb));\n"
    "    }\n"
    "    uvec3 cbg = c;\n"
    "    int cls = hudpix ? 2 : 1;\n"
    "    if (can_win) {\n"
    "        if (os != 0u) {\n"
    "            uvec4 ho = hd_texel(os, cu, cv, cfl, sx, sy, n);\n"
    "            if (ho.a >= 128u) { c = hdm(os, rc_apply(rc_setup(os, oraw, pal(32 + cdm * 4 + oraw, y)), ho)); cls = 3; }\n"
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
    struct Item { uint32_t slot; int size; const uint32_t* px; const uint32_t* ref; bool builtin; };
    std::vector<Item> items;
    for (uint32_t i = 0; i < cap; ++i) {
        Item it{i, 0, nullptr, nullptr, false};
        if (ppu_native_hd_table_entry(i, &it.size, &it.px, &it.ref)) {
            it.builtin = ppu_native_hd_table_entry_builtin(i);   /* passo 31a */
            items.push_back(it);
        }
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
        m[2] = (uint16_t)(pos[k].layer | (it.ref ? 0x8000 : 0) | (it.builtin ? 0x4000 : 0));   /* passo 31a */
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
    if (g_marks_dirty && g_tex_slot) {   /* passo 3D-2b: linha 16 da tabela de slots */
        gl.PixelStorei(kUnpackAlignment, 2);
        gl.PixelStorei(kUnpackRowLength, 0);
        gl.ActiveTexture(kTexture0);
        gl.BindTexture(kTexture2D, g_tex_slot);
        gl.TexSubImage2D(kTexture2D, 0, 0, 16, 768, 1, kRedInteger, kUnsignedShort, g_marks);
        gl.PixelStorei(kUnpackAlignment, 1);
        g_marks_dirty = false;
    }
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
    /* Modo de cor original (P&B). As cores da tabela da linha já vêm em P&B do
     * ppu_native; aqui só a escolha OBP0/OBP1 dos sprites e o HD (a GPU só
     * monta quadros do modo CGB). */
    const bool mono = ppu_get_color_mode() != PPU_COLOR_DX;
    gl.Uniform1i(g_loc_mono, mono ? 1 : 0);
    GLint ramp[16];
    for (int k = 0; k < 4; ++k) {   /* passo 31a: os 4 tons, como ppu_mono_tint_rgba */
        const uint32_t v = 255u - (uint32_t)k * 85u;
        const uint32_t c = ppu_mono_tint_rgba(0xFF000000u | (v << 16) | (v << 8) | v);
        ramp[k * 4 + 0] = (GLint)((c >> 16) & 255u);
        ramp[k * 4 + 1] = (GLint)((c >> 8) & 255u);
        ramp[k * 4 + 2] = (GLint)(c & 255u);
        ramp[k * 4 + 3] = 255;
    }
    gl.Uniform4iv(g_loc_mono_ramp, 4, ramp);
}

bool g_nofade = false;   /* 3D-4a: laterais sem escurecimento (u_ext = 2 no margin_main) */
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
    gl.Uniform1i(g_loc_marks, (out == 2 && !g_level && g_marks_any && !margin) ? 1 : 0);   /* passo 3D-2b */
    gl.Uniform1i(g_loc_winfloor, (out == 2 && !g_level && g_winfloor && !margin) ? 1 : 0);   /* passo 3D-2g */
    gl.Uniform1i(g_loc_ext, (margin && g_nofade) ? 2 : 0);
    gl.BindVertexArray(g_vao);
    gl.DrawArrays(kTriangles, 0, 3);
    gl.BindFramebuffer(kFramebuffer, 0);
    return gl.GetError() == 0;
}

/* Passo 3D-4a: a fase além da tela, na imagem k (0 cor, 1 só o fundo, 2 classe). */
bool draw_ext(GLuint fbo, int w, int h, int scale, int out, int ex, int ey, int side, const PPUNativeGpuFrame* f,
              bool hd) {
    gl.BindFramebuffer(kFramebuffer, fbo);
    gl.Viewport(0, 0, w, h);
    gl.Disable(kBlend);
    gl.Disable(kScissorTest);
    gl.Disable(kDepthTest);
    gl.UseProgram(g_prog);
    bind_inputs();
    gl.ActiveTexture(kTexture0 + 15);
    gl.BindTexture(kTexture2D, g_tex_ext);
    gl.ActiveTexture(kTexture0);
    gl.Uniform1i(g_loc_haslog, g_has_log ? 1 : 0);
    gl.Uniform1i(g_loc_scale, scale);
    gl.Uniform1i(g_loc_hd, hd ? 1 : 0);   /* 3D-4p: HD além da tela (sem as trocas no meio do quadro) */
    gl.Uniform1i(g_loc_hastl, 0);
    gl.Uniform1i(g_loc_margin, 0);
    gl.Uniform1i(g_loc_out, out);
    gl.Uniform1i(g_loc_level, (out == 2 && g_level) ? 1 : 0);
    gl.Uniform1i(g_loc_marks, 0);
    gl.Uniform1i(g_loc_winfloor, 0);
    gl.Uniform1i(g_loc_ext, 1);
    gl.Uniform4i(g_loc_extg, f->ext_tx0, f->ext_ty0, PPU_NATIVE_GPU_EXT_TW, PPU_NATIVE_GPU_EXT_TH);
    gl.Uniform3i(g_loc_exto, ex, ey, side);
    {   /* passo 16d: X verdadeiro dos sprites */
        GLint spr[160];
        for (int i = 0; i < 40; ++i) {
            spr[i * 4] = f->spr_tx[i];
            spr[i * 4 + 1] = f->spr_ty[i];   /* 17a: Y verdadeiro da tela */
            spr[i * 4 + 2] = f->spr_oam[i * 4 + 2];
            spr[i * 4 + 3] = f->spr_oam[i * 4 + 3];
        }
        gl.Uniform4iv(g_loc_tx, 40, spr);
        GLint gh[48 * 4];   /* passo 18b */
        const int ngh = f->ghost_n < 0 ? 0 : (f->ghost_n > 48 ? 48 : f->ghost_n);
        for (int i = 0; i < 48 * 4; ++i) gh[i] = i < ngh * 4 ? f->ghost[i / 4][i % 4] : 0;
        gl.Uniform4iv(g_loc_gh, 48, gh);
        gl.Uniform1i(g_loc_ngh, ngh);
    }
    gl.BindVertexArray(g_vao);
    gl.DrawArrays(kTriangles, 0, 3);
    gl.Uniform1i(g_loc_ext, 0);
    gl.BindFramebuffer(kFramebuffer, 0);
    return gl.GetError() == 0;
}

const char* kCopyFragment =
    "uniform sampler2D u_src;\n"
    "uniform sampler2D u_scls;\n"   /* 3D-6d: classes da tela */
    "uniform ivec2 u_off;\n"
    "out vec4 o_color;\n"
    /* 3D-6d: onde a tela tem o HUD, fica a fase (tiles do mundo, como além da tela):
     * o HUD é desenhado por cima, fixo, e os planos não perdem os tiles atrás dele */
    "void main() {\n"
    "    ivec2 q = ivec2(gl_FragCoord.xy) - u_off;\n"
    "    if ((int(round(texelFetch(u_scls, q, 0).r * 255.0)) & 7) == 2) discard;\n"
    "    o_color = texelFetch(u_src, q, 0);\n"
    "}\n";

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
    g_loc_marks = gl.GetUniformLocation(g_prog, "u_marks");
    g_loc_winfloor = gl.GetUniformLocation(g_prog, "u_winfloor");
    g_loc_haslog = gl.GetUniformLocation(g_prog, "u_haslog");
    g_loc_scale = gl.GetUniformLocation(g_prog, "u_scale");
    g_loc_hd = gl.GetUniformLocation(g_prog, "u_hd");
    g_loc_mono = gl.GetUniformLocation(g_prog, "u_mono");
    g_loc_mono_ramp = gl.GetUniformLocation(g_prog, "u_mono_ramp");   /* passo 31a */
    g_loc_hastl = gl.GetUniformLocation(g_prog, "u_hastl");
    g_loc_margin = gl.GetUniformLocation(g_prog, "u_margin");
    g_loc_out = gl.GetUniformLocation(g_prog, "u_out");
    gl.Uniform1i(gl.GetUniformLocation(g_prog, "u_extt"), 15);   /* passo 3D-4a */
    g_loc_ext = gl.GetUniformLocation(g_prog, "u_ext");
    g_loc_extg = gl.GetUniformLocation(g_prog, "u_extg");
    g_loc_exto = gl.GetUniformLocation(g_prog, "u_exto");
    g_loc_tx = gl.GetUniformLocation(g_prog, "u_spr");   /* passo 16d */
    g_loc_gh = gl.GetUniformLocation(g_prog, "u_gh");     /* passo 18b */
    g_loc_ngh = gl.GetUniformLocation(g_prog, "u_ngh");
    gl.GenVertexArrays(1, &g_vao);
    gl.PixelStorei(kUnpackAlignment, 1);
    gl.PixelStorei(kUnpackRowLength, 0);
    g_tex_vram = make_tex(kR8UI, 256, 64, kRedInteger, kUnsignedByte);
    g_tex_line = make_tex(kRGBA8UI, 3, kH, kRGBAInteger, kUnsignedByte);
    g_tex_layout = make_tex(kR8UI, 256, 32, kRedInteger, kUnsignedByte);   /* passo 3D-2 */
    g_tex_ext = make_tex(kR16UI, PPU_NATIVE_GPU_EXT_TW, PPU_NATIVE_GPU_EXT_TH, kRedInteger, kUnsignedShort);   /* passo 3D-4a */
    g_tex_oam = make_tex(kRGBA8UI, 40, kH, kRGBAInteger, kUnsignedByte);
    g_tex_pal = make_tex(kRGBA8UI, 64, kH, kRGBAInteger, kUnsignedByte);
    g_tex_dirty = make_tex(kRG16UI, 256, 64, kRGInteger, kUnsignedShort);
    g_tex_log = make_tex(kRG8UI, kLogW, kLogH, kRGInteger, kUnsignedByte);
    g_tex_slot = make_tex(kR16UI, 768, 17, kRedInteger, kUnsignedShort);   /* 3D-2b: + linha das marcas */
    g_marks_dirty = true;
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
        if (g_cprog) gl.DeleteProgram(g_cprog);   /* passo 3D-4a */
        if (g_tex_ext) gl.DeleteTextures(1, &g_tex_ext);
        free3(g_3e);
        if (g_bprog) gl.DeleteProgram(g_bprog);
        if (g_wprog) gl.DeleteProgram(g_wprog);
        if (g_bsprog) gl.DeleteProgram(g_bsprog);   /* 3D-4i */
        if (g_out_bs) gl.DeleteTextures(1, &g_out_bs);
        if (g_fbo_bs) gl.DeleteFramebuffers(1, &g_fbo_bs);
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
    g_tex_ext = g_cprog = 0;   /* passo 3D-4a */
    g_3e = Target3();
    g_cv = &g_3s;
    g_cw = g_ch = g_ex = g_ey = 0;
    g_tex_dirty = g_tex_log = g_tex_slot = g_tex_meta = g_tex_ref = g_tex_atlas = 0;
    g_tex_tldir = g_tex_tl = 0;
    g_tex_mrec = g_tex_fade = g_out_m = g_fbo_m = 0;
    g_sprog = g_tex_sbase = g_tex_samb = g_fbo_s = g_out_s = 0;
    g_iprog = g_tex_iruns = g_fbo_ibg = g_out_ibg = g_fbo_iobj = g_out_iobj = 0;
    g_out_i_w = g_out_i_h = 0;
    g_fprog = g_fbo_f = g_out_f = g_final = 0;
    g_dprog = g_bprog = 0;
    g_wprog = g_fbo_w = g_out_w = 0;
    g_bsprog = g_fbo_bs = g_out_bs = 0;   /* 3D-4i */
    g_bs_w = g_bs_h = 0;
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
        g_nofade = f->level_layout && f->ext_on;   /* 3D-4a */
        for (int k = 0; k < 3; ++k) {
            if (!draw(g_3m.fbo[k], kMW * n, kH * n, n, hd && g_atlas_ok, true, k)) {
                g_nofade = false;
                return false;
            }
        }
        g_nofade = false;
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
        /* 3D-2h: a classe também sabe do fundo ambiente; 3D-2i: também em 1x (sem amb_q) */
        gl.Uniform1i(g_sloc_amb, (in->amb_q || (k == 2 && in->amb_on)) ? 1 : 0);
        gl.DrawArrays(kTriangles, 0, 3);
    }
    gl.BindFramebuffer(kFramebuffer, 0);
    if (gl.GetError() != 0) return false;
    g_3d_bw = bw;
    g_3d_level = f->level_layout || g_marks_any || g_winfloor;   /* 3D-2b: tela marcada à mão também tem volume */
    /* Passo 3D-4a: nas fases, a tela dentro de uma imagem maior com a fase além
     * dela (EX de cada lado, EY em cima e embaixo) */
    g_cv = &g_3s;
    g_cw = bw;
    g_ch = kH;
    g_ex = g_ey = 0;
    if (f->level_layout && f->ext_on) {
        if (!g_cprog) {
            std::string e;
            g_cprog = link_program(std::string(shader_header()) + kVertex,
                                   std::string(shader_header()) + kCopyFragment, nullptr, 0, e);
            if (!g_cprog) {
                std::fprintf(stderr, "[GPU] shader da copia 3D: %s\n", e.c_str());
                return false;
            }
            gl.UseProgram(g_cprog);
            gl.Uniform1i(gl.GetUniformLocation(g_cprog, "u_src"), 0);
            g_cloc_off = gl.GetUniformLocation(g_cprog, "u_off");
            gl.Uniform1i(gl.GetUniformLocation(g_cprog, "u_scls"), 1);
        }
        const int ex = PPU_NATIVE_GPU_EXT_X, ey = PPU_NATIVE_GPU_EXT_Y;
        const int cw = bw + 2 * ex, ch = kH + 2 * ey;
        if (!ensure3(g_3e, cw * n, ch * n)) return false;
        gl.PixelStorei(kUnpackAlignment, 2);
        gl.PixelStorei(kUnpackRowLength, 0);
        gl.ActiveTexture(kTexture0 + 15);
        upload(g_tex_ext, PPU_NATIVE_GPU_EXT_TW, PPU_NATIVE_GPU_EXT_TH, kRedInteger, kUnsignedShort, f->ext);
        gl.ActiveTexture(kTexture0);
        gl.PixelStorei(kUnpackAlignment, 1);
        for (int k = 0; k < 3; ++k) {
            if (!draw_ext(g_3e.fbo[k], cw * n, ch * n, n, k, ex, ey, in->wide ? 48 : 0, f, hd && g_atlas_ok)) return false;
            gl.BindFramebuffer(kFramebuffer, g_3e.fbo[k]);
            gl.Viewport(ex * n, ey * n, bw * n, kH * n);
            gl.UseProgram(g_cprog);
            gl.Uniform2i(g_cloc_off, ex * n, ey * n);
            gl.ActiveTexture(kTexture0 + 1);
            gl.BindTexture(kTexture2D, g_3s.out[2]);
            gl.ActiveTexture(kTexture0);
            gl.BindTexture(kTexture2D, g_3s.out[k]);
            gl.BindVertexArray(g_vao);
            gl.DrawArrays(kTriangles, 0, 3);
        }
        gl.BindFramebuffer(kFramebuffer, 0);
        if (gl.GetError() != 0) return false;
        g_cv = &g_3e;
        g_cw = cw;
        g_ch = ch;
        g_ex = ex;
        g_ey = ey;
    }
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
        g_3d_boffx = (((ox + g_ex) % 16) + 16) % 16;   /* 3D-4d: na imagem (com a fase além) */
        g_3d_boffy = (((oy + g_ey) % 16) + 16) % 16;
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
        gl.Uniform1i(gl.GetUniformLocation(g_dprog, "u_bgt"), 3);
        gl.Uniform1i(gl.GetUniformLocation(g_dprog, "u_bs"), 4);
        g_dloc_alpha = gl.GetUniformLocation(g_dprog, "u_alpha");
        g_dloc_lshade = gl.GetUniformLocation(g_dprog, "u_lshade");
        g_dloc_uvr = gl.GetUniformLocation(g_dprog, "u_uvr");
        g_dloc_chi = gl.GetUniformLocation(g_dprog, "u_chi");
        g_dloc_nospr = gl.GetUniformLocation(g_dprog, "u_nospr");
        g_dloc_halfp = gl.GetUniformLocation(g_dprog, "u_halfp");
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
        g_bloc_alpha = gl.GetUniformLocation(g_bprog, "u_alpha");
        g_bloc_ch = gl.GetUniformLocation(g_bprog, "u_ch");
        g_bloc_light = gl.GetUniformLocation(g_bprog, "u_light");
        g_bloc_simple = gl.GetUniformLocation(g_bprog, "u_simple");
        g_bloc_front = gl.GetUniformLocation(g_bprog, "u_front");
        g_bloc_half = gl.GetUniformLocation(g_bprog, "u_half");
        g_bloc_edge = gl.GetUniformLocation(g_bprog, "u_edge");
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
        gl.Uniform1i(gl.GetUniformLocation(g_wprog, "u_col"), 1);
        g_wloc_bw = gl.GetUniformLocation(g_wprog, "u_bw");
        g_wloc_ch = gl.GetUniformLocation(g_wprog, "u_ch");
        g_wloc_boff = gl.GetUniformLocation(g_wprog, "u_boff");
    }
    if (g_w_w != g_cw || g_w_h != g_ch) {
        if (g_out_w) gl.DeleteTextures(1, &g_out_w);
        if (g_fbo_w) gl.DeleteFramebuffers(1, &g_fbo_w);
        g_out_w = g_fbo_w = 0;
        g_w_w = 0;
        if (!make_target(&g_fbo_w, &g_out_w, g_cw, g_ch)) return false;
        g_w_w = g_cw;
        g_w_h = g_ch;
    }
    /* água pré-calculada (1x) a partir das classes */
    gl.BindFramebuffer(kFramebuffer, g_fbo_w);
    gl.Viewport(0, 0, g_cw, g_ch);
    gl.Disable(kScissorTest);
    gl.Disable(kBlend);
    gl.Disable(kDepthTest);
    gl.UseProgram(g_wprog);
    gl.Uniform1f(g_wloc_bw, (float)g_cw);
    gl.Uniform1f(g_wloc_ch, (float)g_ch);
    gl.Uniform2i(g_wloc_boff, g_3d_boffx, g_3d_boffy);
    gl.ActiveTexture(kTexture0 + 1);
    gl.BindTexture(kTexture2D, g_cv->out[1]);
    gl.ActiveTexture(kTexture0);
    gl.BindTexture(kTexture2D, g_cv->out[2]);
    gl.BindVertexArray(g_vao);
    gl.DrawArrays(kTriangles, 0, 3);
    /* passo 3D-4i: origem do fundo atrás dos objetos, em 1x (depois da água) */
    if (!g_bsprog) {
        std::string e;
        g_bsprog = link_program(std::string(shader_header()) + kVertex,
                                std::string(shader_header()) + kBgSrcFragment, nullptr, 0, e);
        if (!g_bsprog) {
            std::fprintf(stderr, "[GPU] shader do fundo 3D: %s\n", e.c_str());
            return false;
        }
        gl.UseProgram(g_bsprog);
        gl.Uniform1i(gl.GetUniformLocation(g_bsprog, "u_clsmap"), 0);
        gl.Uniform1i(gl.GetUniformLocation(g_bsprog, "u_wp"), 1);
    }
    if (g_bs_w != g_cw || g_bs_h != g_ch) {
        if (g_out_bs) gl.DeleteTextures(1, &g_out_bs);
        if (g_fbo_bs) gl.DeleteFramebuffers(1, &g_fbo_bs);
        g_out_bs = g_fbo_bs = 0;
        g_bs_w = g_bs_h = 0;
        if (!make_target(&g_fbo_bs, &g_out_bs, g_cw, g_ch)) return false;
        g_bs_w = g_cw;
        g_bs_h = g_ch;
    }
    gl.BindFramebuffer(kFramebuffer, g_fbo_bs);
    gl.Viewport(0, 0, g_cw, g_ch);
    gl.UseProgram(g_bsprog);
    /* 3D-2m: fora das fases as laterais são só o fundo ambiente (desfoque): atrás do
     * chão perto da borda a busca pegava a cor delas (os quadrados pretos nos slots) */
    if (!g_level && g_cw == 256) gl.Uniform2i(gl.GetUniformLocation(g_bsprog, "u_cx"), 48, 208);
    else gl.Uniform2i(gl.GetUniformLocation(g_bsprog, "u_cx"), 0, g_cw);
    gl.ActiveTexture(kTexture0 + 1);
    gl.BindTexture(kTexture2D, g_out_w);
    gl.ActiveTexture(kTexture0);
    gl.BindTexture(kTexture2D, g_cv->out[2]);
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
    const float cw = (float)g_cw, ch = (float)g_ch;   /* 3D-4a: imagem com a fase além da tela */
    const float fov = 30.0f * 3.14159265f / 180.0f;
    const float dist = (bh * 0.5f) / std::tan(fov * 0.5f);
    const float pitch = v->tilt_deg * 3.14159265f / 180.0f;
    const float yaw = v->yaw_deg * 3.14159265f / 180.0f;
    /* 3D-4q: câmera seguindo o Wario: com inclinação/giro ela mira nele (entra aos
     * poucos até 15 graus; com 0 grau fica igual ao 2D) */
    float ax = 0.0f, ay = 0.0f;
    if (g_3d_level && v->follow > 0.0f) {
        const float ang = std::max(std::fabs(v->tilt_deg), std::fabs(v->yaw_deg));
        const float f = std::min(1.0f, v->follow) * std::min(1.0f, ang / 15.0f);
        const float side = (bw - 160.0f) * 0.5f;
        ax = f * (v->follow_x + side - bw * 0.5f);
        ay = f * (bh * 0.5f - v->follow_y);
    }
    const float eye[3] = {ax + dist * std::sin(yaw) * std::cos(pitch), ay + dist * std::sin(pitch),
                          dist * std::cos(yaw) * std::cos(pitch)};
    const float at[3] = {ax, ay, 0.0f};
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
        gl.BindTexture(kTexture2D, g_cv->out[k]);
        gl.TexParameteri(kTexture2D, kTexMinFilter, filt);
        gl.TexParameteri(kTexture2D, kTexMagFilter, filt);
    }
    gl.ActiveTexture(kTexture0 + 1);
    gl.BindTexture(kTexture2D, g_cv->out[2]);
    gl.ActiveTexture(kTexture0 + 2);
    gl.BindTexture(kTexture2D, g_out_w);
    gl.ActiveTexture(kTexture0 + 3);
    gl.BindTexture(kTexture2D, g_cv->out[1]);
    gl.ActiveTexture(kTexture0 + 4);
    gl.BindTexture(kTexture2D, g_out_bs);   /* 3D-4i */
    gl.BindVertexArray(g_vao);

    const float d = v->depth;
    /* 3D-3: o bloco tem profundidade bd; a frente fica em +bd/2, a porta/escada e
     * os sprites no meio (0), como na prévia; o fundo atrás. 3D-3c: a água é um
     * volume da frente dos blocos (+bd/2) até o fundo (zf): parede no fundo,
     * superfície com cópias das ondas e um véu translúcido na frente. */
    const float bd = g_3d_level ? 0.10f * dist * d : 0.0f;
    /* 3D-5b: o fundo fica onde termina o volume dos blocos (antes −0,3×dist×prof.) */
    const float zf = g_3d_level ? -bd * 0.5f : 0.0f;
    const float zw = zf;   /* 3D-3c: parede do fundo da água, junto do fundo */
    struct L { int layer; int tex; float z; };
    auto plane = [&](const L& l) {
        if (v->hide & (1 << l.layer)) return;   /* 3D-4f: diagnóstico */
        gl.ActiveTexture(kTexture0);
        gl.BindTexture(kTexture2D, g_cv->out[l.tex]);
        const float sc = (dist - l.z) / dist;
        gl.UniformMatrix4fv(g_dloc_mvp, 1, 0, mvp);
        gl.Uniform4f(g_dloc_rect, -cw * 0.5f * sc, ch * 0.5f * sc, cw * sc, ch * sc);
        gl.Uniform1f(g_dloc_z, l.z);
        gl.Uniform1i(g_dloc_layer, l.layer);
        gl.DrawArrays(kTriangleStrip, 0, 4);
    };
    /* 3D-3e: sprites no meio do bloco (fatias de -t a +t), a água começa logo atrás
     * deles (zl) e vai até o fundo; véu da água (30% da cor da água) em cada objeto
     * dentro dela, entrando aos poucos até 10 graus (inclinação 0 = 2D) */
    const float t = g_3d_level ? bd * 0.2f : 0.0f;
    /* 3D-5b: as ondas começam no plano da frente do chão e se repetem até o fundo */
    const float zl = g_3d_level ? bd * 0.5f : 0.0f;
    const float alpha = g_3d_level ? 0.30f * std::min(1.0f, std::fabs(v->tilt_deg) / 10.0f)   /* 3D-4t: também vendo por baixo */ : 0.0f;
    gl.UseProgram(g_dprog);
    gl.Uniform1f(g_dloc_alpha, alpha);
    gl.Uniform1f(g_dloc_lshade, 1.0f);
    gl.Uniform4f(g_dloc_uvr, 0.0f, 0.0f, 1.0f, 1.0f);
    gl.Uniform1i(g_dloc_chi, g_ch);
    gl.Uniform1i(g_dloc_nospr, 0);
    /* 3D-3k: porta logo atrás dos sprites */
    const float zd = g_3d_level ? -(t + 0.5f) : 0.0f;
    const L back[4] = {{0, 0, -0.8f * dist * d}, {1, 1, zf}, {5, 1, zw}, {6, 1, zd}};
    for (const L& l : back) plane(l);
    /* lados dos blocos do chão */
    if (bd > 0.0f) {
        gl.UseProgram(g_bprog);
        gl.ActiveTexture(kTexture0);
        gl.BindTexture(kTexture2D, g_cv->out[1]);
        const int cols = g_cw / 8 + 2, rows = g_ch / 8 + 2;
        gl.UniformMatrix4fv(g_bloc_mvp, 1, 0, mvp);
        gl.Uniform2f(g_bloc_off, g_3d_offx, g_3d_offy);
        gl.Uniform1f(g_bloc_bw, cw);
        gl.Uniform1f(g_bloc_ch, ch);
        gl.Uniform1f(g_bloc_dist, dist);
        gl.Uniform1f(g_bloc_zf, bd * 0.5f);
        gl.Uniform1f(g_bloc_zb, -bd * 0.5f);
        gl.Uniform1i(g_bloc_cols, cols);
        gl.Uniform1i(g_bloc_mode, 0);
        gl.Uniform1f(g_bloc_alpha, alpha);
        gl.Uniform1f(g_bloc_light, 0.0f);
        gl.Uniform1i(g_bloc_half, 0);
        if (!(v->hide & (1 << 11))) gl.DrawArraysInstanced(kTriangles, 0, 24, g_cw * g_ch);   /* 3D-3h: por ponto */
        /* 3D-2k/2l: chão marcado com meia extrusão: do fundo (trás do bloco) até o meio */
        gl.Uniform1f(g_bloc_zf, 0.0f);
        gl.Uniform1f(g_bloc_zb, -bd * 0.5f);
        gl.Uniform1i(g_bloc_half, 1);
        if (!(v->hide & (1 << 11)) && g_marks_any) gl.DrawArraysInstanced(kTriangles, 0, 24, g_cw * g_ch);
        gl.Uniform1i(g_bloc_half, 0);
        /* 3D-3k: portas (com o enfeite em cima) extrudidas do plano delas até o fim
         * do bloco de chão, lados mais claros */
        gl.Uniform1f(g_bloc_zf, zd);
        gl.Uniform1f(g_bloc_zb, -bd * 0.5f);
        gl.Uniform1i(g_bloc_mode, 2);
        gl.Uniform1f(g_bloc_light, 0.35f);
        if (!(v->hide & (1 << 11))) gl.DrawArraysInstanced(kTriangles, 0, 24, g_cw * g_ch);
        gl.Uniform1f(g_bloc_light, 0.0f);
        /* 3D-5c: sprites extrudidos por ponto, da frente deles (+t) até um pouco antes
         * do plano da porta; 3D-5d: cor do lado como nos blocos (a mais comum para
         * dentro, sem o contorno escuro) */
        {   /* 3D-5e: Graphics > "Espessura dos sprites": a frente fica no lugar e
             * o fundo da extrusão vem para perto dela */
            const float sp = v->spr_depth > 0.0f ? std::min(1.0f, v->spr_depth) : 1.0f;
            gl.Uniform1f(g_bloc_zf, t);
            gl.Uniform1f(g_bloc_zb, t - sp * (t - (zd + 0.3f)));
        }
        gl.Uniform1i(g_bloc_mode, 3);
        gl.Uniform1i(g_bloc_simple, 1);
        gl.ActiveTexture(kTexture0);
        gl.BindTexture(kTexture2D, g_cv->out[0]);
        if (!(v->hide & (1 << 3))) gl.DrawArraysInstanced(kTriangles, 0, 24, g_cw * g_ch);
        gl.Uniform1i(g_bloc_simple, 0);
        gl.BindTexture(kTexture2D, g_cv->out[1]);
        /* 3D-3c: chão da superfície da água, da frente da água até o fundo */
        gl.Uniform1f(g_bloc_zf, zl);
        gl.Uniform1f(g_bloc_zb, zf);
        gl.Uniform1i(g_bloc_mode, 1);
        if (!(v->hide & (1 << 12))) gl.DrawArraysInstanced(kTriangles, 0, 6, cols * rows);
        gl.UseProgram(g_dprog);
    }
    plane({4, 1, bd * 0.5f});   /* frente do chão */
    if (g_marks_any && bd > 0.0f) {   /* 3D-2l: frente do meio chão, no meio do bloco */
        gl.Uniform1i(g_dloc_halfp, 1);
        plane({4, 1, 0.0f});
        gl.Uniform1i(g_dloc_halfp, 0);
    }
    if (g_3d_level) {   /* 3D-3c: superfície da água, cópias das ondas da frente até o fundo */
        const float span = zl - zf;
        const float sv = std::max(std::fabs(std::sin(pitch)), std::fabs(std::sin(yaw)));
        int n = (int)std::ceil(span * sv / 3.0f) + 1;   /* uma cópia a cada ~3 pontos na tela (também com giro) */
        n = std::max(1, std::min(n, 48));
        for (int k = 0; k < n; ++k) {
            const float f = n == 1 ? 0.0f : (float)k / (float)(n - 1);
            gl.Uniform1f(g_dloc_lshade, 1.0f - 0.15f * f);   /* 3D-3g: o fundo mais escuro marca cada cópia */
            const float zc = zl - span * f;
            gl.Uniform1i(g_dloc_nospr, zc > -t - 0.5f ? 1 : 0);   /* 3D-5b: na frente dos sprites, não cobre o sprite */
            plane({8, 1, zc});
        }
        gl.Uniform1f(g_dloc_lshade, 1.0f);
        gl.Uniform1i(g_dloc_nospr, 0);
    }
    /* sprites com espessura: fatias de -t a +t (3D-3), todos na mesma profundidade
     * (3D-3d: partes em profundidades diferentes mostravam uma linha na divisa) */
    /* 3D-5c: um plano só, na frente do volume do sprite (os lados vêm do programa
     * dos blocos, modo 3); antes eram 5 cópias de −t a +t */
    plane({3, 0, g_3d_level ? t : 0.07f * dist * d});
    /* 3D-2h: nos mapas, sprites extrudidos como nas fases (mesmo comprimento: 0,4 bloco
     * + 0,2, vezes a espessura dos sprites), da frente deles para trás */
    if (!g_3d_level && v->map_sprites && !(v->hide & (1 << 3))) {
        const float bdm = 0.10f * dist * d, zs = 0.07f * dist * d;
        const float sp = v->spr_depth > 0.0f ? std::min(1.0f, v->spr_depth) : 1.0f;
        gl.UseProgram(g_bprog);
        gl.ActiveTexture(kTexture0);
        gl.BindTexture(kTexture2D, g_cv->out[0]);
        gl.UniformMatrix4fv(g_bloc_mvp, 1, 0, mvp);
        gl.Uniform2f(g_bloc_off, g_3d_offx, g_3d_offy);
        gl.Uniform1f(g_bloc_bw, cw);
        gl.Uniform1f(g_bloc_ch, ch);
        gl.Uniform1f(g_bloc_dist, dist);
        gl.Uniform1i(g_bloc_cols, g_cw / 8 + 2);
        gl.Uniform1f(g_bloc_alpha, 0.0f);
        gl.Uniform1f(g_bloc_light, 0.0f);
        gl.Uniform1i(g_bloc_front, 0);
        gl.Uniform1f(g_bloc_zf, zs);
        gl.Uniform1f(g_bloc_zb, zs - sp * (0.4f * bdm + 0.2f));
        gl.Uniform1i(g_bloc_mode, 3);
        gl.Uniform1i(g_bloc_simple, 1);
        gl.DrawArraysInstanced(kTriangles, 0, 24, g_cw * g_ch);
        gl.Uniform1i(g_bloc_simple, 0);
        gl.UseProgram(g_dprog);
    }
    /* 3D-6a: plano da frente, cópia do chão e da água (Graphics > "Plano da frente") */
    if (g_3d_level && v->front > 0.0f && bd > 0.0f && !(v->hide & (1 << 13))) {
        const float zfr = bd * 0.5f + bd * v->front;
        plane({11, 1, zfr});
        /* 3D-6b: os blocos do plano da frente extrudidos até a frente do chão (os
         * mesmos lados dos blocos, modo 0) */
        if (!(v->hide & (1 << 11))) {
            gl.UseProgram(g_bprog);
            gl.ActiveTexture(kTexture0);
            gl.BindTexture(kTexture2D, g_cv->out[1]);
            gl.Uniform1f(g_bloc_zf, zfr);
            gl.Uniform1f(g_bloc_zb, bd * 0.5f);
            gl.Uniform1i(g_bloc_mode, 0);
            gl.Uniform1f(g_bloc_light, 0.0f);
            gl.Uniform1i(g_bloc_front, 1);   /* 3D-6c: só terreno, contorno preto atrás */
            gl.Uniform1f(g_bloc_edge, std::min(0.5f, 1.0f / (bd * v->front)));
            gl.DrawArraysInstanced(kTriangles, 0, 24, g_cw * g_ch);
            gl.Uniform1i(g_bloc_front, 0);
            gl.UseProgram(g_dprog);
        }
    }
    /* 3D-3g: HUD fixo na tela (sem perspectiva nem giro), no lugar do 2D, por cima */
    if (!(v->hide & (1 << 2))) {
        float ortho[16] = {0};
        ortho[0] = 2.0f / bw;
        ortho[5] = -2.0f / bh;   /* linha 0 da imagem = topo, como o mvp */
        ortho[15] = 1.0f;
        gl.Disable(kDepthTest);
        /* 3D-6d: o HUD vem da tela (g_3s): na imagem com a fase além da tela, o lugar
         * dele agora tem os tiles da fase */
        gl.ActiveTexture(kTexture0 + 1);
        gl.BindTexture(kTexture2D, g_3s.out[2]);
        gl.ActiveTexture(kTexture0);
        gl.BindTexture(kTexture2D, g_3s.out[0]);
        gl.UniformMatrix4fv(g_dloc_mvp, 1, 0, ortho);
        gl.Uniform4f(g_dloc_rect, -bw * 0.5f, bh * 0.5f, bw, bh);
        gl.Uniform4f(g_dloc_uvr, 0.0f, 0.0f, 1.0f, 1.0f);   /* só a tela */
        gl.Uniform1f(g_dloc_z, 0.0f);
        gl.Uniform1i(g_dloc_layer, 2);
        gl.DrawArrays(kTriangleStrip, 0, 4);
        gl.Uniform4f(g_dloc_uvr, 0.0f, 0.0f, 1.0f, 1.0f);
        gl.ActiveTexture(kTexture0 + 1);
        gl.BindTexture(kTexture2D, g_cv->out[2]);
        gl.ActiveTexture(kTexture0);
        gl.Enable(kDepthTest);
    }
    if (v->debug) {   /* 3D-3f: classes por cima, chapadas */
        gl.Enable(kBlend);
        gl.BlendFuncSeparate(kSrcAlpha, kOneMinusSrcAlpha, 0, kOne);
        gl.Disable(kDepthTest);
        plane({10, 1, 0.0f});
        gl.Disable(kBlend);
    }
    gl.Disable(kDepthTest);
    gl.BindFramebuffer(kFramebuffer, 0);
    gl.ActiveTexture(kTexture0);
    return gl.GetError() == 0;
}

/* Passo 3D-2b: marcas por tile vindas da plataforma (fora das fases). */
void gpu_ppu_set_tile_marks(const uint8_t* m, bool any) {
    bool ch = any != g_marks_any;
    for (int i = 0; i < 768; ++i) {
        const uint16_t v = m ? m[i] : 0;
        if (g_marks[i] != v) { g_marks[i] = v; ch = true; }
    }
    g_marks_any = any;
    if (ch) g_marks_dirty = true;
}

void gpu_ppu_set_window_floor(bool on) { g_winfloor = on; }   /* passo 3D-2g */

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
