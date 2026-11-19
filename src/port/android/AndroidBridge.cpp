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
} // namespace Ship
