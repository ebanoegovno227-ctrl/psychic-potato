// Libka0.22 — host redirect only (TLS bypass removed)
// Hooks ported from Dobby to And64InlineHook (A64HookFunction, ARM64 only)

#include <pthread.h>
#include <jni.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <android/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include "Includes/And64InlineHook/And64InlineHook.hpp"
#include "Includes/Obfuscate.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "MoonProject", __VA_ARGS__)

// ---------------------------------------------------------------------------
// Telegram diagnostics: pushes hook status + errors to a bot so the operator
// sees failures immediately. Fully fail-safe — any JNI/HTTP error is swallowed,
// it must never crash the host process. Sends only mod diagnostics, no user data.
// ---------------------------------------------------------------------------

// TODO(operator): set the target chat id (channel/group where the bot posts).
// The bot must be a member/admin of that chat. A token alone cannot deliver.
#define TG_CHAT_ID "-1003619335506"

static JavaVM* g_vm = nullptr;

static std::mutex g_tg_mutex;
static std::condition_variable g_tg_signal;
static std::deque<std::string> g_tg_queue;
static std::atomic<bool> g_tg_running{false};

static std::string tg_url_encode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

static void tg_http_get(JNIEnv* env, const std::string& url) {
    if (env->PushLocalFrame(16) != 0) { env->ExceptionClear(); return; }

    jclass urlCls = env->FindClass("java/net/URL");
    jclass httpCls = env->FindClass("java/net/HttpURLConnection");
    if (env->ExceptionCheck() || !urlCls || !httpCls) { env->ExceptionClear(); env->PopLocalFrame(nullptr); return; }

    jmethodID ctor = env->GetMethodID(urlCls, "<init>", "(Ljava/lang/String;)V");
    jmethodID openConn = env->GetMethodID(urlCls, "openConnection", "()Ljava/net/URLConnection;");
    jmethodID setMethod = env->GetMethodID(httpCls, "setRequestMethod", "(Ljava/lang/String;)V");
    jmethodID setConnTo = env->GetMethodID(httpCls, "setConnectTimeout", "(I)V");
    jmethodID setReadTo = env->GetMethodID(httpCls, "setReadTimeout", "(I)V");
    jmethodID getCode = env->GetMethodID(httpCls, "getResponseCode", "()I");
    jmethodID disconnect = env->GetMethodID(httpCls, "disconnect", "()V");
    if (env->ExceptionCheck() || !ctor || !openConn || !setMethod || !getCode || !disconnect) {
        env->ExceptionClear(); env->PopLocalFrame(nullptr); return;
    }

    jstring jurl = env->NewStringUTF(url.c_str());
    jobject urlObj = env->NewObject(urlCls, ctor, jurl);
    if (env->ExceptionCheck() || !urlObj) { env->ExceptionClear(); env->PopLocalFrame(nullptr); return; }

    jobject conn = env->CallObjectMethod(urlObj, openConn);
    if (env->ExceptionCheck() || !conn) { env->ExceptionClear(); env->PopLocalFrame(nullptr); return; }

    jstring get = env->NewStringUTF("GET");
    env->CallVoidMethod(conn, setMethod, get);
    if (setConnTo) env->CallVoidMethod(conn, setConnTo, 7000);
    if (setReadTo) env->CallVoidMethod(conn, setReadTo, 7000);
    env->CallIntMethod(conn, getCode);   // triggers the request
    env->CallVoidMethod(conn, disconnect);
    if (env->ExceptionCheck()) env->ExceptionClear();

    env->PopLocalFrame(nullptr);
}

static void* tg_worker(void*) {
    if (!g_vm) return nullptr;
    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return nullptr;

    const std::string base = std::string("https://api.telegram.org/bot") +
                             (char*)OBFUSCATE("8125273515:AAGH-ZfbRLbTAVRwyWPqzAB0uTKg9OC8b84") +
                             "/sendMessage?chat_id=" + (char*)OBFUSCATE(TG_CHAT_ID) + "&text=";

    while (g_tg_running.load()) {
        std::string message;
        {
            std::unique_lock<std::mutex> lock(g_tg_mutex);
            g_tg_signal.wait(lock, [] { return !g_tg_queue.empty() || !g_tg_running.load(); });
            if (!g_tg_running.load() && g_tg_queue.empty()) break;
            message = std::move(g_tg_queue.front());
            g_tg_queue.pop_front();
        }
        tg_http_get(env, base + tg_url_encode(message));
    }

    g_vm->DetachCurrentThread();
    return nullptr;
}

static void tg_start() {
    bool expected = false;
    if (!g_tg_running.compare_exchange_strong(expected, true)) return;
    pthread_t t;
    if (pthread_create(&t, nullptr, tg_worker, nullptr) == 0) pthread_detach(t);
    else g_tg_running.store(false);
}

static void tg_log(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(g_tg_mutex);
        if (g_tg_queue.size() >= 128) return;  // bounded — never grow without limit
        g_tg_queue.push_back("[Moon] " + message);
    }
    g_tg_signal.notify_one();
}

#define LOGTG(fmt, ...)                                    \
    do {                                                   \
        char _buf[512];                                    \
        snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__);  \
        LOG("%s", _buf);                                   \
        tg_log(_buf);                                      \
    } while (0)




static volatile bool g_security_ok = true;
static pthread_mutex_t g_security_mutex = PTHREAD_MUTEX_INITIALIZER;

__attribute__((always_inline))
static inline void secure_exit() {
    pthread_mutex_lock(&g_security_mutex);
    g_security_ok = false;
    pthread_mutex_unlock(&g_security_mutex);
    LOG("SECURITY VIOLATION: Terminating process.");
    kill(getpid(), SIGKILL);
    _exit(0);
}




static bool is_whitelisted_lib(const char* path) {
    static const char* whitelist[] = {
        "libil2cpp.so", "libunity.so", "libmain.so", "libc.so", "libm.so",
        "liblog.so", "libdl.so", "libz.so", "libstdc++.so", "libc++.so",
        "libc++_shared.so", "libGLESv2.so", "libGLESv3.so", "libEGL.so",
        "libvulkan.so", "libandroid.so", "libOpenSLES.so", "linker", "linker64",
        "libProjectZero.so", "libnative.so", "libswappy.so", "libswappyVk.so",
        "libFirebaseCppAnalytics.so", "libFirebaseCppApp-6.0.0.so",
        "libFirebaseCppMessaging.so", "libfmod.so", "libfmodL.so",
        "libfmodstudio.so", "libfmodstudioL.so", "libnative-googlesignin.so",
        "/system/", "/vendor/", "/apex/", nullptr
    };
    for (int i = 0; whitelist[i] != nullptr; i++) {
        if (strstr(path, whitelist[i]) != nullptr) return true;
    }
    return false;
}

static bool is_suspicious_path(const char* path) {
    if (strstr(path, "com.nevergames.standnever") != nullptr) return false;
    static const char* suspicious[] = {
        "/data/data/", "/data/local/tmp/", "/sdcard/", "/storage/",
        "frida", "xposed", "substrate", "gg", "gameguardian", "cheat",
        "hack", "mod", "inject", "hook", nullptr
    };
    for (int i = 0; suspicious[i] != nullptr; i++) {
        if (strcasestr(path, suspicious[i]) != nullptr) return true;
    }
    return false;
}

static void scan_loaded_libraries() {
    char line[512];
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, ".so") == nullptr) continue;
        if (strstr(line, "r-x") == nullptr && strstr(line, "r--") == nullptr) continue;
        char* path = strchr(line, '/');
        if (!path) continue;
        char* newline = strchr(path, '\n');
        if (newline) *newline = '\0';
        if (!is_whitelisted_lib(path) && is_suspicious_path(path)) {
            LOG("THREAT DETECTED: Illegal library loaded: %s", path);
            fclose(fp);
            secure_exit();
            return;
        }
    }
    fclose(fp);
}




static bool check_function_hooked(void* func_addr) {
    if (!func_addr) return false;
    unsigned char* bytes = (unsigned char*)func_addr;
    if (bytes[0] == 0x50 && bytes[1] == 0x00 && bytes[2] == 0x00 && bytes[3] == 0x58) return true;
    if (bytes[0] == 0x51 && bytes[1] == 0x00 && bytes[2] == 0x00 && bytes[3] == 0x58) return true;
    if ((bytes[3] & 0xFC) == 0x14) return true;
    if ((bytes[3] & 0xFC) == 0x94) return true;
    if (bytes[0] == 0x04 && bytes[1] == 0xF0 && bytes[2] == 0x1F && bytes[3] == 0xE5) return true;
    if ((bytes[3] & 0x0F) == 0x0A) return true;
    if ((bytes[3] & 0x0F) == 0x0B) return true;
    return false;
}

static int open_wrapper(const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    return open(path, flags, mode);
}

static void verify_critical_functions() {
    void* funcs[] = {
        (void*)dlopen,
        (void*)dlsym,
        (void*)mmap,
        (void*)mprotect,
        (void*)open_wrapper,
        (void*)read,
        (void*)ptrace,
        nullptr
    };
    for (int i = 0; funcs[i] != nullptr; i++) {
        if (check_function_hooked(funcs[i])) { secure_exit(); return; }
    }
}




static void scan_for_cheat_tools() {
    char line[512];
    FILE* fp = fopen("/proc/self/maps", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strcasestr(line, "GameGuardian") || strcasestr(line, "gg.android") || strcasestr(line, "frida")) {
                fclose(fp); secure_exit(); return;
            }
        }
        fclose(fp);
    }
    DIR* dir = opendir("/proc");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;
            char cmdline_path[256];
            snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%s/cmdline", entry->d_name);
            FILE* fpc = fopen(cmdline_path, "r");
            if (fpc) {
                char cmdline[256] = {0};
                fread(cmdline, 1, sizeof(cmdline) - 1, fpc);
                fclose(fpc);
                if (strcasestr(cmdline, "gameguardian") || strcasestr(cmdline, "frida-server")) {
                    closedir(dir); secure_exit(); return;
                }
            }
        }
        closedir(dir);
    }
}




typedef void* monoString;
static void* (*il2cpp_string_new)(const char*) = nullptr;

uintptr_t get_lib_base(const char* lib_name) {
    char line[512];
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, lib_name)) {
            uintptr_t base = (uintptr_t)strtoull(line, NULL, 16);
            fclose(fp);
            return base;
        }
    }
    fclose(fp);
    return 0;
}

static void* make_string(const char* s) {
    if (il2cpp_string_new) return il2cpp_string_new(s);
    return nullptr;
}

// CKOFFFHGGOC — Connect(string host, int port) — RVA: 0x2F56C34
void (*orig_Connect)(void*, monoString, int);
void hook_Connect(void* self, monoString host, int port) {
    void* newHost = make_string("2.27.200.28");
    LOGTG("Connect fired -> host redirected (port %d)", port);
    orig_Connect(self, newHost ? newHost : host, port);
}

// OKMBLDNLPBF(ConnectionConfig) — RVA: 0x2F56C5C
// offset 0x10 = Host (string); offset 0x28 = UseSSL (bool) — no longer touched
void* (*orig_ConnectConfig)(void*, void*);
void* hook_ConnectConfig(void* self, void* config) {
    if (config) {
        void* newHost = make_string("2.27.200.28");
        if (newHost) {
            *(void**)((uintptr_t)config + 0x10) = newHost;
            LOGTG("ConnectConfig fired -> host redirected (TLS intact)");
        } else {
            LOGTG("ConnectConfig fired -> host unchanged (string_new not ready)");
        }
    }
    return orig_ConnectConfig(self, config);
}

void* security_thread(void*) {
    sleep(10);
    while (g_security_ok) {
        scan_loaded_libraries();
        verify_critical_functions();
        scan_for_cheat_tools();
        sleep(5);
    }
    return nullptr;
}

void* moon_thread(void*) {
    LOG("MoonProject thread started");
    uintptr_t base = 0;
    for (int i = 0; i < 30 && !base; i++) {
        base = get_lib_base("libil2cpp.so");
        if (!base) sleep(1);
    }
    if (!base) { LOGTG("ERROR: libil2cpp.so base not found after 30s"); return nullptr; }
    LOGTG("libil2cpp.so base = %p", (void*)base);

    void* handle = dlopen("libil2cpp.so", RTLD_NOW);
    if (handle) {
        il2cpp_string_new = (void* (*)(const char*))dlsym(handle, "il2cpp_string_new");
    }
    if (!il2cpp_string_new) LOGTG("WARN: il2cpp_string_new not resolved");

    // ConnectAsync(string host, int port) — host redirect
    A64HookFunction((void*)(base + 0x2F56C34), (void*)hook_Connect, (void**)&orig_Connect);
    LOGTG("hook Connect: %s", orig_Connect ? "OK" : "FAILED");

    // ConnectWithConfigAsync(ConnectionConfig) — host redirect only (TLS left intact)
    A64HookFunction((void*)(base + 0x2F56C5C), (void*)hook_ConnectConfig, (void**)&orig_ConnectConfig);
    LOGTG("hook ConnectConfig: %s", orig_ConnectConfig ? "OK" : "FAILED");

    LOGTG("hooks applied (built %s %s)", __DATE__, __TIME__);
    return nullptr;
}

__attribute__((constructor)) void lib_moon_main() {
    pthread_t t1, t2;
    pthread_create(&t1, nullptr, moon_thread, nullptr);
    pthread_detach(t1);
    pthread_create(&t2, nullptr, security_thread, nullptr);
    pthread_detach(t2);
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    tg_start();
    return JNI_VERSION_1_6;
}
