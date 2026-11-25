/**
 * JNI surface for the Android app.
 *
 * Two unrelated things live here because they are the only places the Java/Kotlin
 * side reaches into native code:
 *
 *  - the touch buttons (TouchOverlay.java), fed into SDL as one virtual game
 *    controller so they go through libultraship's normal mapping (every pad is
 *    on port 1, so they work next to a built-in controller),
 *  - whether the in-game menu is open (the Mods button and the touch buttons
 *    follow it), and
 *  - game asset generation, which runs Torch against the user's ROM.
 *
 * The asset generation entry point is deliberately independent of libultraship:
 * it is called from the launcher activity, before SDL exists, so it cannot use
 * Ship::Context to discover paths. The launcher passes them in explicitly.
 */
#ifdef __ANDROID__

#include <jni.h>
#include <android/log.h>

#include <SDL2/SDL.h>

#include <pthread.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Companion.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <yaml-cpp/yaml.h>
#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"

namespace {

constexpr const char* kLogTag = "SpaghettiKart";

std::string ToStdString(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars != nullptr ? chars : "";
    if (chars != nullptr) {
        env->ReleaseStringUTFChars(value, chars);
    }
    return result;
}

// stdout / stderr into logcat (tag SpaghettiKart-stdout): libultraship and Torch report some failures only there
// (e.g. "Log initialization failed"), and Android drops both streams. Started when the library loads.
void* PumpStdio(void* arg) {
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    char buf[1024];
    std::string line;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                __android_log_write(ANDROID_LOG_INFO, "SpaghettiKart-stdout", line.c_str());
                line.clear();
            } else {
                line += buf[i];
            }
        }
    }
    return nullptr;
}

__attribute__((constructor)) void RedirectStdio() {
    int fds[2];
    if (pipe(fds) != 0) {
        return;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    pthread_t t;
    if (pthread_create(&t, nullptr, PumpStdio, reinterpret_cast<void*>(static_cast<intptr_t>(fds[0]))) == 0) {
        pthread_detach(t);
    }
}

bool GameRunning() {
    auto* context = Ship::Context::GetRawInstance();
    return context != nullptr && context->GetWindow() != nullptr;
}

// The touch buttons as an ordinary SDL game controller (the original Android
// port's virtual joystick, izzy2lost). A virtual GAMECONTROLLER joystick gets
// SDL's default mapping: button i = SDL_GameControllerButton i, axis i =
// SDL_GameControllerAxis i.
SDL_Joystick* gTouchJoystick = nullptr;
int gTouchJoystickIndex = -1;

void AttachTouchJoystick() {
    if (gTouchJoystick != nullptr || !SDL_WasInit(SDL_INIT_JOYSTICK)) {
        return;
    }
    gTouchJoystickIndex = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                                    SDL_CONTROLLER_BUTTON_MAX, 0);
    if (gTouchJoystickIndex < 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Could not attach the touch buttons: %s", SDL_GetError());
        return;
    }
    gTouchJoystick = SDL_JoystickOpen(gTouchJoystickIndex);
    if (gTouchJoystick == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Could not open the touch buttons: %s", SDL_GetError());
        SDL_JoystickDetachVirtual(gTouchJoystickIndex);
        gTouchJoystickIndex = -1;
        return;
    }
    // a trigger at rest is the axis minimum (raw 0 would read as half pressed)
    SDL_JoystickSetVirtualAxis(gTouchJoystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_JOYSTICK_AXIS_MIN);
    SDL_JoystickSetVirtualAxis(gTouchJoystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_JOYSTICK_AXIS_MIN);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Touch buttons attached as a virtual controller");
}

} // namespace

extern "C" {

/**
 * The touch buttons' state (TouchOverlay bits, the stick in -1..1 with y down = +).
 *
 * The N64 buttons are sent the way SpaghettiKart's default controller mapping
 * (src/port/Engine.cpp) reads a pad: A = A, B = X, Z = left trigger, L = left
 * shoulder, R = right shoulder, START = Start, C-down = B, C-left = Y, C-up =
 * right stick up, C-right = right stick right, the D-pad = the D-pad.
 */
JNIEXPORT void JNICALL Java_com_izzy_kart_TouchNative_nativeTouchPad(JNIEnv*, jclass, jint bits, jfloat x, jfloat y) {
    AttachTouchJoystick();
    if (gTouchJoystick == nullptr) {
        return;
    }
    auto on = [bits](int bit) { return (bits >> bit) & 1; };
    // TouchOverlay: 0 A, 1 B, 2 Z, 3 L, 4 R, 5 START, 6-9 C up/down/left/right, 10-13 D-pad up/down/left/right
    SDL_JoystickSetVirtualButton(gTouchJoystick, SDL_CONTROLLER_BUTTON_A, on(0));
    SDL_JoystickSetVirtualButton(gTouchJoystick, SDL_CONTROLLER_BUTTON_X, on(1));
    SDL_JoystickSetVirtualButton(gTouchJoystick, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, on(3));
    SDL_JoystickSetVirtualButton(gTouchJoystick, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, on(4));
    SDL_JoystickSetVirtualButton(gTouchJoystick, SDL_CONTROLLER_BUTTON_START, on(5));
    SDL_JoystickSetVirtualButton(gTouchJoystick, SDL_CONTROLLER_BUTTON_B, on(7));
    SDL_JoystickSetVirtualButton(gTouchJoystick, SDL_CONTROLLER_BUTTON_Y, on(8));
    SDL_JoystickSetVirtualButton(gTouchJoystick, SDL_CONTROLLER_BUTTON_DPAD_UP, on(10));
    SDL_JoystickSetVirtualButton(gTouchJoystick, SDL_CONTROLLER_BUTTON_DPAD_DOWN, on(11));
    SDL_JoystickSetVirtualButton(gTouchJoystick, SDL_CONTROLLER_BUTTON_DPAD_LEFT, on(12));
    SDL_JoystickSetVirtualButton(gTouchJoystick, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, on(13));
    SDL_JoystickSetVirtualAxis(gTouchJoystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT,
                               on(2) ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    SDL_JoystickSetVirtualAxis(gTouchJoystick, SDL_CONTROLLER_AXIS_RIGHTY, on(6) ? SDL_JOYSTICK_AXIS_MIN : 0);
    SDL_JoystickSetVirtualAxis(gTouchJoystick, SDL_CONTROLLER_AXIS_RIGHTX, on(9) ? SDL_JOYSTICK_AXIS_MAX : 0);
    SDL_JoystickSetVirtualAxis(gTouchJoystick, SDL_CONTROLLER_AXIS_LEFTX, static_cast<Sint16>(x * SDL_JOYSTICK_AXIS_MAX));
    SDL_JoystickSetVirtualAxis(gTouchJoystick, SDL_CONTROLLER_AXIS_LEFTY, static_cast<Sint16>(y * SDL_JOYSTICK_AXIS_MAX));
}

/** The game runs (libultraship's window exists): the touch buttons may show. */
JNIEXPORT jboolean JNICALL Java_com_izzy_kart_TouchNative_nativeGameRunning(JNIEnv*, jclass) {
    return GameRunning() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Whether libultraship's menu is currently up.
 *
 * The Mods button follows it: the menu is opened by the controller or a
 * keyboard, and can close itself, so reading the engine's state is what keeps
 * the button from drifting out of sync.
 *
 * Called from the UI thread while the game thread renders. It reads a bool the
 * game thread may be writing, which is worth accepting here: the value only
 * drives whether the Mods button is shown, and a torn read self-corrects on the
 * next poll.
 */
JNIEXPORT jboolean JNICALL Java_com_izzy_kart_MainActivity_isMenuOpen(JNIEnv*, jobject) {
    auto* context = Ship::Context::GetRawInstance();
    if (context == nullptr) {
        return JNI_FALSE;
    }

    auto window = context->GetWindow();
    if (window == nullptr) {
        return JNI_FALSE;
    }

    auto gui = window->GetGui();
    if (gui == nullptr) {
        return JNI_FALSE;
    }

    return gui->GetMenuOrMenubarVisible() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Runs Torch over the ROM at romPath and writes mk64.o2r into destDir.
 *
 * sourceDir is the directory holding config.yml and yamls/ (unpacked from the
 * APK by the launcher). Returns null on success, or a message describing what
 * went wrong.
 */
} // extern "C"

// Progress of the extraction, for the launcher's dialog: Torch reports nothing itself, but logs one
// "- [TYPE] Processing <name>" line per asset; this sink counts them against the assets in the yamls.
namespace {
std::atomic<int> gAssetsDone{ 0 };
std::atomic<int> gAssetsTotal{ 0 };

class AssetCountSink : public spdlog::sinks::base_sink<std::mutex> {
  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        std::string_view text(msg.payload.data(), msg.payload.size());
        if (text.rfind("- [", 0) == 0 && text.find("] Processing ") != std::string_view::npos) {
            gAssetsDone++;
        }
    }
    void flush_() override {}
};

int CountAssets(const std::filesystem::path& yamls) {
    int total = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(yamls, error)) {
        const auto ext = entry.path().extension();
        if (!entry.is_regular_file() || (ext != ".yml" && ext != ".yaml")) {
            continue;
        }
        try {
            YAML::Node root = YAML::LoadFile(entry.path().string());
            for (auto it = root.begin(); it != root.end(); ++it) {
                if (it->first.as<std::string>().find(":config") == std::string::npos) {
                    total++;
                }
            }
        } catch (...) {
        }
    }
    return total;
}
} // namespace

extern "C" {

// 0..1 while the extraction runs (-1: not known).
JNIEXPORT jfloat JNICALL Java_com_izzy_kart_GameAssets_nativeExtractProgress(JNIEnv*, jobject) {
    const int total = gAssetsTotal.load();
    if (total <= 0) {
        return -1.0f;
    }
    return std::min(0.99f, static_cast<float>(gAssetsDone.load()) / static_cast<float>(total));
}

JNIEXPORT jstring JNICALL Java_com_izzy_kart_GameAssets_nativeGenerateGameArchive(JNIEnv* env, jobject,
                                                                                 jstring jRomPath, jstring jSourceDir,
                                                                                 jstring jDestDir) {
    const std::string romPath = ToStdString(env, jRomPath);
    const std::string sourceDir = ToStdString(env, jSourceDir);
    const std::string destDir = ToStdString(env, jDestDir);

    const auto fail = [env](const std::string& message) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Asset generation failed: %s", message.c_str());
        return env->NewStringUTF(message.c_str());
    };

    std::ifstream rom(romPath, std::ios::binary);
    if (!rom.is_open()) {
        return fail("Could not open the ROM at " + romPath);
    }

    std::vector<uint8_t> romData((std::istreambuf_iterator<char>(rom)), std::istreambuf_iterator<char>());
    rom.close();

    if (romData.empty()) {
        return fail("The ROM at " + romPath + " is empty.");
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Extracting %zu bytes of ROM data into %s", romData.size(),
                        destDir.c_str());

    // Companion registers itself as a singleton and is read back through that
    // pointer all over Torch, so it has to be assigned before Init runs.
    delete Companion::Instance;
    Companion::Instance = new Companion(std::move(romData), ArchiveType::O2R, false, sourceDir, destDir);
    Companion::Instance->SetAdditionalFiles({ "meta/mods.toml" });

    gAssetsDone = 0;
    gAssetsTotal = CountAssets(std::filesystem::path(sourceDir) / "yamls");
    auto countSink = std::make_shared<AssetCountSink>();
    spdlog::default_logger()->sinks().push_back(countSink);

    std::string extractError;
    try {
        Companion::Instance->Init(ExportType::Binary);
    } catch (const std::exception& error) {
        extractError = std::string("Torch could not extract the ROM: ") + error.what();
    } catch (...) {
        extractError = "Torch could not extract the ROM.";
    }

    // The game runs in this same process, and Companion is still holding the
    // ROM plus every decoded asset. Hand that memory back before it has to
    // compete with the game for it — including when the extraction threw, since
    // the launcher stays alive to let the user try again.
    delete Companion::Instance;
    Companion::Instance = nullptr;
    auto& sinks = spdlog::default_logger()->sinks();
    sinks.erase(std::remove(sinks.begin(), sinks.end(), countSink), sinks.end());
    gAssetsTotal = 0;

    if (!extractError.empty()) {
        return fail(extractError);
    }

    // Process() logs and returns rather than throwing for a few failure modes
    // (missing config, unrecognised ROM), so confirm the archive really landed.
    std::error_code error;
    const std::filesystem::path archive = std::filesystem::path(destDir) / "mk64.o2r";
    if (!std::filesystem::exists(archive, error) || std::filesystem::file_size(archive, error) == 0) {
        return fail("Torch finished without producing mk64.o2r. Check that the ROM is Mario Kart 64 (US).");
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Wrote %s", archive.c_str());
    return nullptr;
}

} // extern "C"

#endif // __ANDROID__

// The Mods tab of the settings menu (ui/Menu.cpp): MainActivity.openModsFromMenu() opens the mods and saves manager.
namespace Ship {
void Android_OpenModsManager() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || activity == nullptr) {
        return;
    }
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(cls, "openModsFromMenu", "()V");
    if (method != nullptr) {
        env->CallVoidMethod(activity, method);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}
// The card menu's view of the Back menu's pages (TouchControls' bridge* methods, MenuCards.cpp): the class is found
// through the activity's class loader (FindClass on this thread only sees the system classes) and kept.
static jclass TouchControlsClass(JNIEnv* env) {
    static jclass cls = nullptr;
    if (cls != nullptr) {
        return cls;
    }
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (activity == nullptr) {
        return nullptr;
    }
    jclass actCls = env->GetObjectClass(activity);
    jmethodID getLoader = env->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = env->CallObjectMethod(activity, getLoader);
    jclass loaderCls = env->FindClass("java/lang/ClassLoader");
    jmethodID load = env->GetMethodID(loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("com.izzy.kart.TouchControls");
    jobject found = env->CallObjectMethod(loader, load, name);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        found = nullptr;
    }
    if (found != nullptr) {
        cls = static_cast<jclass>(env->NewGlobalRef(found));
        env->DeleteLocalRef(found);
    }
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(loaderCls);
    env->DeleteLocalRef(loader);
    env->DeleteLocalRef(actCls);
    env->DeleteLocalRef(activity);
    return cls;
}

std::vector<std::string> Android_MenuPage(const char* id) {
    std::vector<std::string> out;
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env ? TouchControlsClass(env) : nullptr;
    if (cls == nullptr) {
        return out;
    }
    static jmethodID m = env->GetStaticMethodID(cls, "bridgeItems", "(Ljava/lang/String;)[Ljava/lang/String;");
    jstring jid = env->NewStringUTF(id);
    auto arr = static_cast<jobjectArray>(env->CallStaticObjectMethod(cls, m, jid));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        arr = nullptr;
    }
    if (arr != nullptr) {
        const jsize n = env->GetArrayLength(arr);
        for (jsize i = 0; i < n; i++) {
            auto js = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
            const char* c = js ? env->GetStringUTFChars(js, nullptr) : nullptr;
            out.emplace_back(c ? c : "");
            if (c) env->ReleaseStringUTFChars(js, c);
            if (js) env->DeleteLocalRef(js);
        }
        env->DeleteLocalRef(arr);
    }
    env->DeleteLocalRef(jid);
    return out;
}

void Android_MenuAct(const char* id, int index, int op) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env ? TouchControlsClass(env) : nullptr;
    if (cls == nullptr) {
        return;
    }
    static jmethodID m = env->GetStaticMethodID(cls, "bridgeAct", "(Ljava/lang/String;II)V");
    jstring jid = env->NewStringUTF(id);
    env->CallStaticVoidMethod(cls, m, jid, index, op);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(jid);
}

std::string Android_MenuTakePushed() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env ? TouchControlsClass(env) : nullptr;
    if (cls == nullptr) {
        return "";
    }
    static jmethodID m = env->GetStaticMethodID(cls, "bridgeTakePushed", "()Ljava/lang/String;");
    auto js = static_cast<jstring>(env->CallStaticObjectMethod(cls, m));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return "";
    }
    std::string r;
    if (js != nullptr) {
        const char* c = env->GetStringUTFChars(js, nullptr);
        r = c ? c : "";
        env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
    }
    return r;
}

bool Android_MenuTakeClose() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env ? TouchControlsClass(env) : nullptr;
    if (cls == nullptr) {
        return false;
    }
    static jmethodID m = env->GetStaticMethodID(cls, "bridgeTakeClose", "()Z");
    const bool r = env->CallStaticBooleanMethod(cls, m);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return r;
}
} // namespace Ship

// ---------------------------------------------------------------- FPS / frame time (the Back menu's readout)
#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <mutex>
// frame_stats.inc - frame statistics for the Back menu's FPS / frame time readout (TouchControls, FpsView): tick()
// once per frame shown; read() gives fps, average and longest frame time (ms) of the last half second.
namespace frame_stats {
struct State {
    std::mutex mutex;
    int64_t last = 0, windowStart = 0;
    int frames = 0;
    double sum = 0, longest = 0;
    float fps = 0, avg = 0, max = 0;
};
inline State& state() { static State s; return s; }
inline int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void tick() {
    State& s = state();
    const int64_t now = nowNs();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.last != 0) {
        const double dt = (now - s.last) / 1e6;
        s.frames++;
        s.sum += dt;
        s.longest = std::max(s.longest, dt);
    }
    s.last = now;
    if (s.windowStart == 0) s.windowStart = now;
    if (now - s.windowStart >= 500000000LL && s.frames > 0) {
        s.fps = static_cast<float>(s.frames * 1e9 / (now - s.windowStart));
        s.avg = static_cast<float>(s.sum / s.frames);
        s.max = static_cast<float>(s.longest);
        s.frames = 0;
        s.sum = s.longest = 0;
        s.windowStart = now;
    }
}
inline void read(float out[3]) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const bool stale = s.last == 0 || nowNs() - s.last > 1000000000LL;   // no frame for a second: 0 fps
    out[0] = stale ? 0.0f : s.fps;
    out[1] = s.avg;
    out[2] = s.max;
}
} // namespace frame_stats

// Every frame the game shows goes through SDL_GL_SwapWindow (libultraship's SDL backend). Defined here, it takes
// libultraship's calls (static, in this library) before libSDL2.so's, counts the frame and hands on to SDL.
extern "C" void SDL_GL_SwapWindow(SDL_Window* window) {
    using Swap = void (*)(SDL_Window*);
    static Swap real = reinterpret_cast<Swap>(dlsym(dlopen("libSDL2.so", RTLD_NOW | RTLD_NOLOAD), "SDL_GL_SwapWindow"));
    frame_stats::tick();
    if (real != nullptr) {
        real(window);
    }
}

extern "C" JNIEXPORT jfloatArray JNICALL Java_com_izzy_kart_TouchNative_nativeFrameStats(JNIEnv* env, jclass) {
    float s[3];
    frame_stats::read(s);
    jfloatArray a = env->NewFloatArray(3);
    env->SetFloatArrayRegion(a, 0, 3, s);
    return a;
}
