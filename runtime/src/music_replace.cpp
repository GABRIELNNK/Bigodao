/**
 * @file music_replace.cpp
 * @brief Passo 6c: trilhas .ogg no lugar da música do Game Boy.
 *
 * Fluxo:
 *  - music_replace_rescan() procura nos mods ligados os arquivos de
 *    music/ (via hd_pack_io) e os music.json;
 *  - quando $A61D muda, music_replace_set_song() abre a trilha nova (fora da
 *    thread de áudio) e a entrega ao mixer; a antiga sai com um fade curto;
 *  - music_replace_mix() roda no callback do SDL: decodifica, faz o loop,
 *    reamostra para a taxa da placa e soma no que o GB gerou.
 * A trilha começa depois de delay_frames quadros, para entrar junto com o
 * áudio do GB que ainda está no buffer (e com o portão do audio.c).
 */
#include "music_replace.h"
#include "hd_pack_io.h"

#define STB_VORBIS_HEADER_ONLY
#include "../vendor/stb/stb_vorbis.c"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

namespace {

/* ---------------------------------------------------------------------------
 * Índice das trilhas
 * ------------------------------------------------------------------------- */
struct TrackSettings {
    bool has_loop = false;     /* veio do music.json */
    bool loop = true;
    int64_t loop_start = -1;   /* -1 = não definido */
    int64_t loop_end = -1;
    float volume = 1.0f;
};

struct TrackRef {
    bool present = false;
    int source = -1;
    std::string source_name;
    std::string path;
    TrackSettings settings;
};

TrackRef g_tracks[256];
int g_track_count = 0;
std::string g_track_list;

std::string lower_ascii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)std::tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

/* "04.ogg", "04_rice.ogg", "2B - jingle.ogg" -> número da música. */
int song_from_leaf(const std::string& leaf) {
    if (leaf.size() < 6) return -1;   /* "XX.ogg" */
    const std::string low = lower_ascii(leaf);
    if (low.compare(low.size() - 4, 4, ".ogg") != 0) return -1;
    const int hi = hex_digit(low[0]);
    const int lo = hex_digit(low[1]);
    if (hi < 0 || lo < 0) return -1;
    const char after = low[2];
    if (after != '.' && after != '_' && after != '-' && after != ' ') return -1;
    return hi * 16 + lo;
}

/* Caminho tem uma pasta "music" antes do nome do arquivo. */
bool in_music_dir(const std::string& path, std::string& leaf) {
    const std::string low = lower_ascii(path);
    const size_t cut = low.find_last_of("/\\");
    if (cut == std::string::npos) return false;
    const std::string dir = low.substr(0, cut);
    leaf = path.substr(cut + 1);
    const size_t dcut = dir.find_last_of("/\\");
    const std::string last = dcut == std::string::npos ? dir : dir.substr(dcut + 1);
    return last == "music";
}

struct ScanFound {
    int source;
    std::string source_name;
    std::string path;
    std::string leaf;
};

void scan_visitor(int source_index, const char* source_name, const char* path, void* user) {
    std::vector<ScanFound>* found = (std::vector<ScanFound>*)user;
    std::string leaf;
    if (!in_music_dir(path, leaf)) return;
    found->push_back({source_index, source_name, path, leaf});
}

void parse_music_json(const std::string& text, TrackSettings (&out)[256], bool (&touched)[256]) {
    static const std::regex entry(R"re("([0-9A-Fa-f]{2})"\s*:\s*\{([^}]*)\})re");
    static const std::regex num_start(R"re("loop_start"\s*:\s*(-?\d+))re");
    static const std::regex num_end(R"re("loop_end"\s*:\s*(-?\d+))re");
    static const std::regex loop_flag(R"re("loop"\s*:\s*(true|false))re");
    static const std::regex vol(R"re("volume"\s*:\s*([0-9]*\.?[0-9]+))re");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), entry);
         it != std::sregex_iterator(); ++it) {
        const int song = (int)std::strtol((*it)[1].str().c_str(), nullptr, 16);
        const std::string body = (*it)[2].str();
        TrackSettings& s = out[song];
        std::smatch m;
        if (std::regex_search(body, m, num_start)) s.loop_start = std::strtoll(m[1].str().c_str(), nullptr, 10);
        if (std::regex_search(body, m, num_end)) s.loop_end = std::strtoll(m[1].str().c_str(), nullptr, 10);
        if (std::regex_search(body, m, loop_flag)) { s.has_loop = true; s.loop = m[1].str() == "true"; }
        if (std::regex_search(body, m, vol)) s.volume = std::strtof(m[1].str().c_str(), nullptr);
        touched[song] = true;
    }
}

/* ---------------------------------------------------------------------------
 * Trilha tocando
 * ------------------------------------------------------------------------- */
struct Track {
    int song = -1;
    uint8_t* data = nullptr;
    stb_vorbis* vorbis = nullptr;
    int rate = 44100;
    int64_t total = 0;          /* quadros (0 = desconhecido) */
    bool loop = true;
    int64_t loop_start = 0;
    int64_t loop_end = 0;       /* 0 = fim do arquivo */
    float volume = 1.0f;

    int64_t pos = 0;            /* próximo quadro a decodificar */
    int16_t buf[2 * 1024];
    int buf_len = 0;
    int buf_pos = 0;
    bool eof = false;

    /* reamostragem linear */
    int32_t s0[2] = {0, 0};
    int32_t s1[2] = {0, 0};
    double frac = 0.0;
    bool primed = false;

    /* controle (thread de áudio) */
    uint32_t start_delay = 0;   /* quadros de silêncio antes de começar */
    int64_t stop_after = -1;    /* quadros até começar o fade de saída (-1 = não) */
    float gain = 0.0f;
    float gain_step = 0.0f;     /* por quadro */
    float fade_out_step = 0.0f;
    bool fading_out = false;
    std::atomic<bool> dead{false};

    ~Track() {
        if (vorbis) stb_vorbis_close(vorbis);
        free(data);
    }

    void refill() {
        buf_pos = 0;
        buf_len = 0;
        for (int attempt = 0; attempt < 2 && buf_len == 0; ++attempt) {
            int want = 1024;
            if (loop && loop_end > 0) {
                if (pos >= loop_end) {
                    stb_vorbis_seek(vorbis, (unsigned int)loop_start);
                    pos = loop_start;
                }
                want = (int)std::min<int64_t>(want, loop_end - pos);
                if (want <= 0) want = 1;
            }
            const int got = stb_vorbis_get_samples_short_interleaved(vorbis, 2, buf, want * 2);
            if (got > 0) {
                buf_len = got;
                pos += got;
                return;
            }
            if (!loop) {
                eof = true;
                return;
            }
            stb_vorbis_seek(vorbis, (unsigned int)loop_start);   /* fim do arquivo: volta */
            pos = loop_start;
        }
        if (buf_len == 0) eof = true;
    }

    /* Próximo quadro na taxa do .ogg; false no fim. */
    bool next_frame(int32_t out[2]) {
        if (buf_pos >= buf_len) {
            if (eof) return false;
            refill();
            if (buf_len == 0) return false;
        }
        out[0] = buf[buf_pos * 2];
        out[1] = buf[buf_pos * 2 + 1];
        buf_pos++;
        return true;
    }
};

std::mutex g_mutex;                 /* protege g_current e g_fading */
Track* g_current = nullptr;
std::vector<Track*> g_fading;
std::vector<Track*> g_graveyard;    /* liberar fora da thread de áudio */

std::atomic<bool> g_enabled{true};
bool g_log = false;   /* passo 6d */
std::atomic<float> g_volume{1.0f};
std::atomic<bool> g_active{false};
int g_song = -1;                    /* thread principal */
std::atomic<uint32_t> g_last_device_rate{44100};

const float kFadeInSeconds = 0.010f;
const float kFadeOutSeconds = 0.120f;

Track* open_track(int song) {
    const TrackRef& ref = g_tracks[song & 0xFF];
    if (!ref.present) return nullptr;
    size_t size = 0;
    uint8_t* data = hd_pack_io_read_file(ref.source, ref.path.c_str(), &size);
    if (!data || size == 0 || size > 0x7FFFFFFF) {
        fprintf(stderr, "[Musica] %s: nao consegui ler %s\n", ref.source_name.c_str(), ref.path.c_str());
        free(data);
        return nullptr;
    }
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(data, (int)size, &err, nullptr);
    if (!v) {
        fprintf(stderr, "[Musica] %s: %s nao e um OGG Vorbis valido (erro %d)\n",
                ref.source_name.c_str(), ref.path.c_str(), err);
        free(data);
        return nullptr;
    }
    Track* t = new Track();
    t->song = song;
    t->data = data;
    t->vorbis = v;
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    t->rate = info.sample_rate > 0 ? (int)info.sample_rate : 44100;
    t->total = stb_vorbis_stream_length_in_samples(v);

    /* Tags LOOPSTART / LOOPLENGTH / LOOPEND (padrão de muitos pacotes). */
    int64_t tag_start = -1, tag_len = -1, tag_end = -1;
    const stb_vorbis_comment c = stb_vorbis_get_comment(v);
    for (int i = 0; i < c.comment_list_length; ++i) {
        const std::string kv = c.comment_list[i] ? c.comment_list[i] : "";
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower_ascii(kv.substr(0, eq));
        const int64_t val = std::strtoll(kv.c_str() + eq + 1, nullptr, 10);
        if (key == "loopstart") tag_start = val;
        else if (key == "looplength") tag_len = val;
        else if (key == "loopend") tag_end = val;
    }

    const TrackSettings& s = ref.settings;
    t->loop = s.has_loop ? s.loop : true;
    t->loop_start = s.loop_start >= 0 ? s.loop_start : (tag_start >= 0 ? tag_start : 0);
    if (s.loop_end >= 0) t->loop_end = s.loop_end;
    else if (tag_end > 0) t->loop_end = tag_end;
    else if (tag_start >= 0 && tag_len > 0) t->loop_end = tag_start + tag_len;
    else t->loop_end = 0;
    if (t->total > 0) {
        if (t->loop_end > t->total) t->loop_end = 0;
        if (t->loop_start >= t->total) t->loop_start = 0;
    }
    if (t->loop_end > 0 && t->loop_end <= t->loop_start) t->loop_end = 0;
    t->volume = s.volume > 0.0f ? s.volume : 1.0f;

    if (g_log) fprintf(stderr, "[Musica] trilha %02X: %s (%s), %d Hz, %lld quadros, loop %s %lld..%lld\n",
            song, ref.path.c_str(), ref.source_name.c_str(), t->rate, (long long)t->total,
            t->loop ? "sim" : "nao", (long long)t->loop_start, (long long)t->loop_end);
    return t;
}

}  // namespace

/* ---------------------------------------------------------------------------
 * API (thread principal)
 * ------------------------------------------------------------------------- */
extern "C" void music_replace_rescan(void) {
    std::vector<ScanFound> found;
    hd_pack_io_list_files(scan_visitor, &found);

    TrackSettings settings[256];
    bool touched[256] = {false};
    for (TrackRef& r : g_tracks) r = TrackRef();
    for (const ScanFound& f : found) {   /* na ordem das fontes: a de baixo vence */
        const std::string low = lower_ascii(f.leaf);
        if (low == "music.json") {
            size_t size = 0;
            uint8_t* data = hd_pack_io_read_file(f.source, f.path.c_str(), &size);
            if (data) {
                parse_music_json(std::string((const char*)data, size), settings, touched);
                free(data);
            }
            continue;
        }
        const int song = song_from_leaf(f.leaf);
        if (song < 0) continue;
        TrackRef& r = g_tracks[song];
        r.present = true;
        r.source = f.source;
        r.source_name = f.source_name;
        r.path = f.path;
    }
    g_track_count = 0;
    g_track_list.clear();
    for (int i = 0; i < 256; ++i) {
        if (touched[i]) g_tracks[i].settings = settings[i];
        if (!g_tracks[i].present) continue;
        g_track_count++;
        char b[8];
        snprintf(b, sizeof(b), "%s%02X", g_track_list.empty() ? "" : " ", i);
        g_track_list += b;
    }
    if (g_log) fprintf(stderr, "[Musica] %d trilha(s) nos mods%s%s\n", g_track_count,
            g_track_count ? ": " : "", g_track_list.c_str());

    /* Reabre a música atual com o índice novo. */
    const int song = g_song;
    g_song = -1;
    if (song >= 0) music_replace_set_song(song, 0);
}

extern "C" int music_replace_track_count(void) {
    return g_track_count;
}

extern "C" const char* music_replace_track_list(void) {
    return g_track_list.c_str();
}

extern "C" void music_replace_set_song(int song, uint32_t delay_frames) {
    if (song == g_song) return;
    g_song = song;
    Track* next = nullptr;
    if (song >= 0 && song < 256 && g_enabled.load()) {
        next = open_track(song);   /* leitura e abertura fora do lock */
    }
    const uint32_t dev_rate = g_last_device_rate.load();
    const float rate = (float)(dev_rate ? dev_rate : 44100);
    if (next) {
        next->start_delay = delay_frames;
        next->gain = 0.0f;
        next->gain_step = 1.0f / (kFadeInSeconds * rate);
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_current) {
            g_current->stop_after = (int64_t)delay_frames;
            g_current->fade_out_step = 1.0f / (kFadeOutSeconds * rate);
            g_fading.push_back(g_current);
        }
        g_current = next;
    }
    g_active.store(next != nullptr);
}

extern "C" bool music_replace_active(void) {
    return g_active.load();
}

extern "C" void music_replace_collect(void) {
    std::vector<Track*> dead;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_fading.begin();
        while (it != g_fading.end()) {
            if ((*it)->dead.load()) {
                dead.push_back(*it);
                it = g_fading.erase(it);
            } else {
                ++it;
            }
        }
        if (g_current && g_current->dead.load()) {   /* trilha sem loop terminou */
            dead.push_back(g_current);
            g_current = nullptr;
        }
    }
    for (Track* t : dead) delete t;
}

extern "C" void music_replace_set_enabled(bool enabled) {
    if (enabled == g_enabled.load()) return;
    g_enabled.store(enabled);
    const int song = g_song;
    g_song = -1;
    music_replace_set_song(enabled ? song : -1, 0);
    if (!enabled) g_song = song;   /* lembra para quando religar */
}

extern "C" bool music_replace_get_enabled(void) {
    return g_enabled.load();
}

extern "C" void music_replace_set_volume(float volume) {
    g_volume.store(std::max(0.0f, std::min(2.0f, volume)));
}

extern "C" float music_replace_get_volume(void) {
    return g_volume.load();
}

extern "C" void music_replace_set_log(bool enabled) {
    g_log = enabled;
}

/* ---------------------------------------------------------------------------
 * Mixer (thread de áudio)
 * ------------------------------------------------------------------------- */
namespace {

/* Soma uma trilha em out. Devolve false quando ela acabou de vez. */
bool mix_track(Track* t, int16_t* out, uint32_t frames, uint32_t device_rate, float master) {
    const double step = (double)t->rate / (double)device_rate;
    for (uint32_t i = 0; i < frames; ++i) {
        if (t->start_delay > 0) {
            t->start_delay--;
            continue;
        }
        if (t->stop_after == 0) {
            t->fading_out = true;
            t->stop_after = -1;
        } else if (t->stop_after > 0) {
            t->stop_after--;
        }
        if (t->fading_out) {
            t->gain -= t->fade_out_step;
            if (t->gain <= 0.0f) return false;
        } else if (t->gain < 1.0f) {
            t->gain = std::min(1.0f, t->gain + t->gain_step);
        }
        if (!t->primed) {
            if (!t->next_frame(t->s0) || !t->next_frame(t->s1)) return false;
            t->primed = true;
        }
        const double f = t->frac;
        const float l = (float)(t->s0[0] + (t->s1[0] - t->s0[0]) * f);
        const float r = (float)(t->s0[1] + (t->s1[1] - t->s0[1]) * f);
        const float g = t->gain * t->volume * master;
        int32_t ol = out[i * 2] + (int32_t)std::lrintf(l * g);
        int32_t orr = out[i * 2 + 1] + (int32_t)std::lrintf(r * g);
        out[i * 2] = (int16_t)std::max(-32768, std::min(32767, ol));
        out[i * 2 + 1] = (int16_t)std::max(-32768, std::min(32767, orr));
        t->frac += step;
        while (t->frac >= 1.0) {
            t->frac -= 1.0;
            t->s0[0] = t->s1[0];
            t->s0[1] = t->s1[1];
            if (!t->next_frame(t->s1)) {
                if (t->fading_out || t->eof) return false;
            }
        }
    }
    return true;
}

}  // namespace

extern "C" void music_replace_mix(int16_t* out, uint32_t frames, uint32_t device_rate, float master) {
    if (!out || frames == 0 || device_rate == 0) return;
    g_last_device_rate.store(device_rate, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;   /* troca de trilha em andamento: pula este bloco */
    const float vol = master * g_volume.load(std::memory_order_relaxed);
    if (g_current && !g_current->dead.load(std::memory_order_relaxed)) {
        if (!mix_track(g_current, out, frames, device_rate, vol)) {
            g_current->dead.store(true);
        }
    }
    for (Track* t : g_fading) {
        if (t->dead.load(std::memory_order_relaxed)) continue;
        if (!mix_track(t, out, frames, device_rate, vol)) {
            t->dead.store(true);
        }
    }
}
