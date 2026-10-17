/**
 * @file android_bridge.cpp
 * @brief Ponte com o lado Java no Android. Stubs nas outras plataformas.
 */
#include "android_bridge.h"

#if defined(__ANDROID__)

#include "platform_sdl.h"   /* gb_platform_set_persistence_dir */

#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <android/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace android_bridge {

static const char* kTag = "Bigodao";
static bool g_pick_pending = false;
static std::string g_user_dir;   /* calculado em prepare_storage() */

static void ensure_dir(const std::string& path) {
    mkdir(path.c_str(), 0755);   /* falha silenciosa se já existe */
}

/* ---- Chamadas a métodos estáticos do MainActivity ------------------------ */

static jclass activity_class(JNIEnv** out_env) {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return nullptr;
    /* GetObjectClass evita FindClass, que falha para classes do app
     * quando chamado de uma thread nativa. */
    jclass cls = env->GetObjectClass(activity);
    env->DeleteLocalRef(activity);
    *out_env = env;
    return cls;
}

static void clear_exception(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

static std::string call_static_string(const char* name) {
    JNIEnv* env = nullptr;
    jclass cls = activity_class(&env);
    if (!cls) return std::string();
    std::string result;
    jmethodID mid = env->GetStaticMethodID(cls, name, "()Ljava/lang/String;");
    if (mid) {
        jstring s = (jstring)env->CallStaticObjectMethod(cls, mid);
        if (!env->ExceptionCheck() && s) {
            const char* utf = env->GetStringUTFChars(s, nullptr);
            if (utf) {
                result = utf;
                env->ReleaseStringUTFChars(s, utf);
            }
            env->DeleteLocalRef(s);
        }
    }
    clear_exception(env);
    env->DeleteLocalRef(cls);
    return result;
}

static int call_static_int(const char* name) {
    JNIEnv* env = nullptr;
    jclass cls = activity_class(&env);
    if (!cls) return -1;
    int result = -1;
    jmethodID mid = env->GetStaticMethodID(cls, name, "()I");
    if (mid) {
        result = (int)env->CallStaticIntMethod(cls, mid);
        if (env->ExceptionCheck()) result = -1;
    }
    clear_exception(env);
    env->DeleteLocalRef(cls);
    return result;
}

/* ---- Arquivos --------------------------------------------------------------- */

/* Lê um arquivo dos assets do APK. Caminho relativo + pasta de trabalho
 * diferente de <interno>/ garante que o SDL vá ao AssetManager. */
static bool extract_asset(const char* asset_path, const std::string& dest) {
    SDL_RWops* in = SDL_RWFromFile(asset_path, "rb");
    if (!in) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "Asset %s indisponível: %s", asset_path, SDL_GetError());
        return false;
    }
    FILE* out = fopen(dest.c_str(), "wb");
    if (!out) {
        SDL_RWclose(in);
        return false;
    }
    char buffer[16384];
    size_t n;
    bool ok = true;
    while ((n = SDL_RWread(in, buffer, 1, sizeof(buffer))) > 0) {
        if (fwrite(buffer, 1, n, out) != n) { ok = false; break; }
    }
    fclose(out);
    SDL_RWclose(in);
    return ok;
}

/*
 * Pasta "Bigodao" na raiz do armazenamento do usuário (/storage/emulated/0/Bigodao).
 * Precisa da permissão "Acesso a todos os arquivos" (Android 11+) ou de
 * armazenamento (Android 6-10). O Java pede uma vez; enquanto o usuário está
 * na tela de permissão, esperamos aqui (até 2 minutos).
 */
static std::string resolve_bigodao_folder() {
    int state = call_static_int("requestUserFolderAccess");
    /* 2 = liberado, 1 = perguntando agora, 0/3/-1 = sem acesso */
    for (int waited_ms = 0; state == 1 && waited_ms < 120000; waited_ms += 250) {
        SDL_Delay(250);
        state = call_static_int("userFolderAccessState");
    }
    if (state != 2) return std::string();
    return call_static_string("userFolderPath");   /* cria Bigodao/mods e Bigodao/log */
}

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool copy_file(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return false;
    const std::string tmp = to + ".tmp-copy";
    FILE* out = fopen(tmp.c_str(), "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buffer[16384];
    size_t n;
    bool ok = true;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (ok) ok = rename(tmp.c_str(), to.c_str()) == 0;
    if (!ok) remove(tmp.c_str());
    return ok;
}

/* Arquivos de save do runtime: <nome>.sav, <nome>.rtc, <nome>.state1 ... .state10 */
static bool is_save_file(const char* name) {
    const size_t len = strlen(name);
    auto ends_with = [&](const char* suffix) {
        const size_t sl = strlen(suffix);
        return len > sl && strcmp(name + len - sl, suffix) == 0;
    };
    return ends_with(".sav") || ends_with(".rtc") || strstr(name, ".state") != nullptr;
}

/*
 * Versões anteriores gravavam os saves no armazenamento interno do app.
 * Copia esses arquivos para a pasta nova (só os que ainda não existem lá).
 * Os originais ficam onde estavam, por segurança.
 */
static int migrate_old_saves(const std::string& saves_dir) {
    const char* internal = SDL_AndroidGetInternalStoragePath();
    if (!internal) return 0;
    DIR* dir = opendir(internal);
    if (!dir) return 0;
    int copied = 0;
    while (struct dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.' || !is_save_file(entry->d_name)) continue;
        if (strstr(entry->d_name, ".tmp")) continue;   /* gravação interrompida */
        const std::string from = std::string(internal) + "/" + entry->d_name;
        const std::string to = saves_dir + entry->d_name;
        if (!file_exists(from) || file_exists(to)) continue;
        if (copy_file(from, to)) copied++;
    }
    closedir(dir);
    return copied;
}

static std::string app_files_dir() {
    const char* ext = SDL_AndroidGetExternalStoragePath();
    if (ext && ext[0]) return std::string(ext) + "/";
    const char* internal = SDL_AndroidGetInternalStoragePath();
    return internal ? std::string(internal) + "/" : std::string();
}

std::string user_dir() {
    if (g_user_dir.empty()) g_user_dir = app_files_dir();
    return g_user_dir;
}

void prepare_storage() {
    /* 0. Driver de áudio: o AAudio do SDL 2.30.0 não espera o dispositivo
     *    (escreve com timeout de 1 ms e descarta o que não coube), então o
     *    callback roda sem parar, o som some em underruns e o ritmo do jogo
     *    se perde. O driver "android" (AudioTrack) bloqueia direito. */
    if (!SDL_GetHint(SDL_HINT_AUDIODRIVER)) {
        SDL_SetHint(SDL_HINT_AUDIODRIVER, "android,openslES");
    }

    /* 1. Pasta do usuário: /storage/emulated/0/Bigodao (mods + log) */
    const std::string folder = resolve_bigodao_folder();
    g_user_dir = folder.empty() ? app_files_dir() : folder + "/";
    const std::string udir = g_user_dir;
    const std::string log_dir = udir.empty() ? std::string() : udir + "log/";

    /* 2. Log em arquivo, acessível sem adb */
    if (!log_dir.empty()) {
        ensure_dir(udir);
        ensure_dir(log_dir);
        const std::string log_path = log_dir + "last_run.log";
        const int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
            setvbuf(stdout, nullptr, _IOLBF, 0);
            setvbuf(stderr, nullptr, _IONBF, 0);
        }
    }

    /* 3. Pasta de trabalho dentro do armazenamento interno */
    const char* internal = SDL_AndroidGetInternalStoragePath();
    if (!internal) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Sem armazenamento interno: %s", SDL_GetError());
        return;
    }
    const std::string work = std::string(internal) + "/runtime";
    ensure_dir(work);
    ensure_dir(work + "/tools");
    if (chdir(work.c_str()) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "chdir(%s) falhou", work.c_str());
    }

    /* 4. Manifesto do patch (sempre reextraído: pode mudar entre versões do app) */
    extract_asset("tools/wl_sm3_dx_v12_patch.json", work + "/tools/wl_sm3_dx_v12_patch.json");

    printf("[Android] Pasta de trabalho: %s\n", work.c_str());
    if (folder.empty()) {
        printf("[Android] Sem acesso à pasta Bigodao do usuário (permissão negada); usando a pasta do app.\n");
    }
    printf("[Android] Pasta do usuário (mods e log): %s\n", udir.c_str());

    /* 5. Saves e savestates em Bigodao/saves, para levar a outro aparelho ou sistema.
     *    Sem acesso à pasta do usuário, continuam no armazenamento interno (como antes). */
    if (!folder.empty()) {
        const std::string saves_dir = folder + "/saves/";
        ensure_dir(saves_dir);
        const int migrated = migrate_old_saves(saves_dir);
        if (gb_platform_set_persistence_dir(saves_dir.c_str())) {
            printf("[Android] Saves: %s\n", saves_dir.c_str());
            if (migrated > 0) {
                printf("[Android] %d save(s) antigo(s) copiado(s) do armazenamento interno.\n", migrated);
            }
        } else {
            printf("[Android] Não foi possível usar %s; saves no armazenamento interno.\n", saves_dir.c_str());
        }
    }
}

bool request_file_pick(PickKind kind) {
    JNIEnv* env = nullptr;
    jclass cls = activity_class(&env);
    if (!cls) return false;
    jmethodID mid = env->GetStaticMethodID(cls, "requestFilePick", "(I)V");
    bool ok = false;
    if (mid) {
        env->CallStaticVoidMethod(cls, mid, (jint)kind);
        ok = !env->ExceptionCheck();
    }
    clear_exception(env);
    env->DeleteLocalRef(cls);
    if (ok) g_pick_pending = true;
    return ok;
}

bool poll_picked_file(int& kind, std::string& path) {
    if (!g_pick_pending) return false;
    const std::string result = call_static_string("pollPickedFile");
    /* Formato: "<kind>\n<caminho>" (caminho vazio = cancelado); vazio = ainda não */
    const size_t nl = result.find('\n');
    if (nl == std::string::npos) return false;
    kind = atoi(result.c_str());
    path = result.substr(nl + 1);
    g_pick_pending = false;
    return true;
}

} // namespace android_bridge

#else  /* !__ANDROID__ : Linux / Windows */

/*
 * No PC, os saves e savestates ficam na pasta "saves" ao lado do executável,
 * em qualquer pasta onde o jogo for extraído:
 *   <pasta do jogo>/saves/wario_land_sm3_dx_v12.sav, .state1 ... .state10
 * Mesma organização do Android (Bigodao/saves): para trocar de máquina ou de
 * sistema, basta copiar o conteúdo da pasta saves.
 */
#include "platform_sdl.h"   /* gb_platform_set_persistence_dir */

#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace android_bridge {

namespace fs = std::filesystem;

static fs::path path_from_utf8(const std::string& s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

static std::string path_to_utf8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

/* <nome>.sav, <nome>.rtc, <nome>.state1 ... .state10 */
static bool is_save_file(const std::string& name) {
    auto ends_with = [&](const char* suffix) {
        const size_t sl = strlen(suffix);
        return name.size() > sl && name.compare(name.size() - sl, sl, suffix) == 0;
    };
    if (name.find(".tmp") != std::string::npos) return false;   /* gravação interrompida */
    return ends_with(".sav") || ends_with(".rtc") || name.find(".state") != std::string::npos;
}

/*
 * Versões anteriores gravavam os saves soltos ao lado do executável. Copia
 * esses arquivos para a pasta saves (só os que ainda não existem lá); os
 * originais ficam onde estavam, por segurança.
 */
static int migrate_old_saves(const fs::path& exe_dir, const fs::path& saves_dir) {
    std::error_code ec;
    int copied = 0;
    for (const auto& entry : fs::directory_iterator(exe_dir, ec)) {
        std::error_code e2;
        if (!entry.is_regular_file(e2)) continue;
        const fs::path name = entry.path().filename();
        if (!is_save_file(path_to_utf8(name))) continue;
        const fs::path dest = saves_dir / name;
        if (fs::exists(dest, e2)) continue;
        if (fs::copy_file(entry.path(), dest, fs::copy_options::skip_existing, e2) && !e2) copied++;
    }
    return copied;
}

void prepare_storage() {
    char* base = SDL_GetBasePath();   /* pasta do executável, termina com separador */
    if (!base) {
        fprintf(stderr, "[Saves] Pasta do executável desconhecida; usando o padrão do runtime.\n");
        return;
    }
    const fs::path exe_dir = path_from_utf8(base);
    SDL_free(base);

    const fs::path saves_dir = exe_dir / "saves";
    std::error_code ec;
    fs::create_directories(saves_dir, ec);
    if (ec || !fs::is_directory(saves_dir, ec)) {
        fprintf(stderr, "[Saves] Não foi possível criar %s; saves ao lado do executável.\n",
                path_to_utf8(saves_dir).c_str());
        return;
    }
    const int migrated = migrate_old_saves(exe_dir, saves_dir);
    const std::string saves_utf8 = path_to_utf8(saves_dir);
    if (!gb_platform_set_persistence_dir(saves_utf8.c_str())) {
        fprintf(stderr, "[Saves] Não foi possível usar %s; saves ao lado do executável.\n", saves_utf8.c_str());
        return;
    }
    printf("[Saves] %s\n", saves_utf8.c_str());
    if (migrated > 0) {
        printf("[Saves] %d save(s) antigo(s) copiado(s) para a pasta saves.\n", migrated);
    }
}

std::string user_dir() { return std::string(); }
bool request_file_pick(PickKind) { return false; }
bool poll_picked_file(int&, std::string&) { return false; }
} // namespace android_bridge

#endif
