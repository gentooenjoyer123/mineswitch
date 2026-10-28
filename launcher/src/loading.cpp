#include "mineswitch/loading.h"
#include "mineswitch/drawing.hpp"
#include "mineswitch/i18n.hpp"
#include <array>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>

namespace {
using namespace mineswitch;

struct Phase {
    const char *title;
    const char *detail;
};
constexpr std::array<Phase, 4> phases{{
    {"Preparing the environment", "Getting the files needed for launch ready."},
    {"Starting Java", "Creating the game environment."},
    {"Loading the client", "Starting Minecraft."},
    {"Loading the game and mods", "Please wait for the game window to open."},
}};

std::string display_title(const char *title) {
    std::string result;
    if (title) {
        for (std::size_t i = 0; i < 512 && title[i]; ++i) {
            const auto ch = static_cast<unsigned char>(title[i]);
            result += ch < 32 || ch == 127 ? ' ' : title[i];
        }
    }
    if (result.find_first_not_of(' ') == std::string::npos)
        return "Minecraft Java Edition";
    return result;
}

class Loading {
  public:
    // Own video/events only. Drawing releases these exact subsystem references,
    // preserving audio/timers acquired by Java while this screen is visible.
    Drawing drawing{SDL_INIT_VIDEO | SDL_INIT_EVENTS};
    std::string title;
    Uint32 started = SDL_GetTicks();
    int phase = 1;
    bool failed = false;

    explicit Loading(const char *name) : title(display_title(name)) {}

    void frame(int stage, const std::string &screenshot = {}) {
        if (stage >= 1 && stage <= static_cast<int>(phases.size()))
            phase = stage;
        SDL_PumpEvents();
        drawing.clear();

        drawing.box({72, 54, 12, 12}, accent);
        drawing.box({88, 70, 8, 8}, Color{91, 153, 121});
        drawing.text("MineSwitch", 113, 47, 25, foreground);

        drawing.box({160, 174, 960, 370}, panel);
        drawing.outline({160, 174, 960, 370}, border, 1);
        drawing.text("MINECRAFT JAVA", 208, 215, 17, muted);
        drawing.text(tr("Launching Minecraft"), 208, 256, 46, foreground, 864);
        drawing.text(title, 210, 323, 23, muted, 860);
        drawing.box({208, 378, 864, 1}, border);

        const auto &current = phases[static_cast<std::size_t>(phase - 1)];
        drawing.box({210, 419, 8, 8}, accent);
        drawing.text(tr(current.title), 235, 405, 26, foreground, 680);
        drawing.text(tr(current.detail), 235, 450, 20, muted, 820);

        // Moving dots indicate activity, not measured completion or a percent.
        const auto active = ((SDL_GetTicks() - started) / 240u) % 4u;
        for (unsigned i = 0; i < 4; ++i) {
            const Color color = i == active ? accent : Color{61, 86, 81};
            drawing.box({992 + static_cast<int>(i) * 20, 420, 8, 8}, color);
        }
        drawing.text(tr("The first launch may take longer."), 160, 581, 19, muted, 960);

        if (!screenshot.empty())
            drawing.screenshot(screenshot);
        drawing.present();
    }
};

std::unique_ptr<Loading> loading;
} // namespace

extern "C" int mineswitch_loading_begin(const char *title) {
    mineswitch_loading_end();
    try {
        auto next = std::make_unique<Loading>(title);
        next->frame(1);
        loading = std::move(next);
        return 1;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "MINESWITCH_LOADING_BEGIN_ERROR %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "MINESWITCH_LOADING_BEGIN_ERROR unknown\n");
    }
    return 0;
}

extern "C" void mineswitch_loading_frame(int stage) {
    if (!loading || loading->failed)
        return;
    try {
        loading->frame(stage);
    } catch (const std::exception &error) {
        // Preserve the already presented frame until the caller ends loading.
        loading->failed = true;
        std::fprintf(stderr, "MINESWITCH_LOADING_FRAME_ERROR %s\n", error.what());
    } catch (...) {
        loading->failed = true;
        std::fprintf(stderr, "MINESWITCH_LOADING_FRAME_ERROR unknown\n");
    }
}

extern "C" void mineswitch_loading_end(void) { loading.reset(); }

#if defined(MINESWITCH_LOADING_TEST) && !defined(__SWITCH__)
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace

// Host-only lifecycle check and screenshot; compile this translation unit with
// MINESWITCH_LOADING_TEST and link Drawing, SDL2 and FreeType. No game is run.
int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: loading-smoke SCREENSHOT.bmp\n");
        return 2;
    }
    SDL_AudioDeviceID audio = 0;
    try {
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
        require(SDL_InitSubSystem(SDL_INIT_TIMER) == 0, "Cannot initialize caller timer");
        mineswitch_loading_frame(1);
        mineswitch_loading_end();
        require(mineswitch_loading_begin("Minecraft 1.21.1 · Fabric"), "Loading screen did not initialize");
        const Uint32 window = SDL_GetWindowID(loading->drawing.window());
        require((SDL_WasInit(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK)) == 0,
                "Loading initialized input subsystems owned by the caller");

        // Simulate a Java audio subsystem becoming active after loading begins.
        require(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0, "Cannot initialize concurrent audio");
        SDL_AudioSpec wanted{}, actual{};
        wanted.freq = 48000; wanted.format = AUDIO_S16SYS; wanted.channels = 2; wanted.samples = 512;
        audio = SDL_OpenAudioDevice(nullptr, 0, &wanted, &actual, 0);
        require(audio != 0, "Cannot open concurrent audio device");
        SDL_PauseAudioDevice(audio, 0);
        for (int stage = 1; stage <= 4; ++stage)
            mineswitch_loading_frame(stage);
        require(!loading->failed, "Loading frame failed");
        loading->frame(4, argv[1]);
        mineswitch_loading_end();
        mineswitch_loading_end();
        mineswitch_loading_frame(4);
        require(SDL_GetWindowFromID(window) == nullptr, "Loading window survived end");
        require(SDL_WasInit(SDL_INIT_VIDEO) == 0, "Loading video subsystem survived end");
        require((SDL_WasInit(SDL_INIT_AUDIO | SDL_INIT_TIMER)) == (SDL_INIT_AUDIO | SDL_INIT_TIMER),
                "Loading ended a caller-owned subsystem");
        require(SDL_GetAudioDeviceStatus(audio) == SDL_AUDIO_PLAYING, "Loading interrupted concurrent audio");
        const std::array<unsigned char, 128> silence{};
        require(SDL_QueueAudio(audio, silence.data(), silence.size()) == 0, "Audio handle invalid after loading end");

        require(mineswitch_loading_begin(nullptr), "Second loading lifecycle failed");
        mineswitch_loading_frame(0);
        mineswitch_loading_end();
        require(SDL_GetAudioDeviceStatus(audio) == SDL_AUDIO_PLAYING, "Repeated loading interrupted concurrent audio");
        SDL_CloseAudioDevice(audio); audio = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO | SDL_INIT_TIMER);
        require(SDL_WasInit(0) == 0, "Loading left an initialized subsystem");

        require(SDL_InitSubSystem(SDL_INIT_AUDIO | SDL_INIT_TIMER) == 0, "Cannot initialize failure fixture subsystems");
        audio = SDL_OpenAudioDevice(nullptr, 0, &wanted, &actual, 0);
        require(audio != 0, "Cannot open failure fixture audio");
        SDL_PauseAudioDevice(audio, 0);
        const char *configured_driver = SDL_getenv("SDL_VIDEODRIVER");
        const std::string saved_driver = configured_driver ? configured_driver : "";
        SDL_setenv("SDL_VIDEODRIVER", "mineswitch-missing-test-driver", 1);
        const int failed_begin = mineswitch_loading_begin("Initialization failure fixture");
        SDL_setenv("SDL_VIDEODRIVER", saved_driver.c_str(), 1);
        require(failed_begin == 0, "Failed video initialization did not return the fallback status");
        require(SDL_GetAudioDeviceStatus(audio) == SDL_AUDIO_PLAYING && SDL_WasInit(SDL_INIT_TIMER),
                "Failed loading initialization interrupted caller subsystems");
        SDL_CloseAudioDevice(audio); audio = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO | SDL_INIT_TIMER);
        // SDL2.32 can retain its internal EVENTS ref when VIDEO init fails;
        // Switch SDL2.28 rolls it back. This standalone fixture owns the process
        // and may finish SDL globally; production loading never does so.
        SDL_Quit();
        std::puts("LOADING_SMOKE_PASS phases, screenshot, window release, idempotent cleanup, concurrent audio preservation, init failure");
        return 0;
    } catch (const std::exception &error) {
        mineswitch_loading_end();
        if (audio)
            SDL_CloseAudioDevice(audio);
        SDL_QuitSubSystem(SDL_INIT_AUDIO | SDL_INIT_TIMER);
        SDL_Quit();
        std::fprintf(stderr, "LOADING_SMOKE_FAIL %s\n", error.what());
        return 1;
    }
}
#endif
