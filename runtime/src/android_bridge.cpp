/**
 * @file android_bridge.cpp
 * @brief Ponte com o lado Java no Android. Stubs nas outras plataformas.
 */
#include "android_bridge.h"

#if defined(__ANDROID__)

#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <android/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#else  /* !__ANDROID__ */

namespace android_bridge {
void prepare_storage() {}
std::string user_dir() { return std::string(); }
bool request_file_pick(PickKind) { return false; }
bool poll_picked_file(int&, std::string&) { return false; }
} // namespace android_bridge

#endif
