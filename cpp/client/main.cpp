// The game client.
//
// Every frame is described through cpp_canvas. The desktop build consumes
// those calls with its software canvas and presents through SDL; Emscripten
// forwards them to the page's native Canvas2D contexts, including text,
// offscreen blits and post-process compositing. The browser build never owns a
// Wasm software framebuffer.
//
// The two builds differ in who owns the frame loop. Natively this program
// does, in App::run(). In the browser the event loop belongs to the page, so
// App::step() is registered as a requestAnimationFrame callback instead;
// returning from main() there is normal and must not tear the runtime down,
// which is what -sEXIT_RUNTIME=0 in the emscripten link flags is for.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "client/app.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include "client/web/persist.h"
#endif

namespace {

#ifdef __EMSCRIPTEN__
/// The running app, so the page's unload handler can reach it. main() has
/// returned by the time that fires, and a capture-less lambda is what the
/// handler takes.
flix::App* g_app = nullptr;

/// The name the session file is stored under. settingsPath() appends
/// "-settings" to the path, so the settings file's name follows from it.
constexpr const char* kSessionName = "session";
#endif

void usage(const char* program) {
    std::printf(
        "usage: %s [options]\n"
        "  --host <address>   server to connect to (default 127.0.0.1)\n"
        "  --port <number>    server port (default 3000)\n"
        "  --data <dir>       directory holding mobs.json and petals.json (default data)\n"
        "  --width <px>       window width (default 1280)\n"
        "  --height <px>      window height (default 720)\n"
        "  --user <name>      log in (or register) automatically\n"
        "  --password <pw>    password for --user\n"
        "  --frames <n>       render n frames then exit\n"
        "  --screenshot <f>   write the last frame to f as a PPM\n"
        "  --menu <name>      open a menu on startup: inventory, craft, talents,\n"
        "                     mobgallery, shop, skins, leaderboard, settings\n"
        "  --lobby            log in but stay on the title screen\n"
        "  --spawn <where>    where to join: a biome name, or pvp / maze\n"
        "  --login            ignore any stored session and show the login form\n"
        "  --dead             join, then show the death card straight away\n"
        "  --tutorial         join with the onboarding tutorial card up\n"
        "  --stats            show the frame/ping/position counters\n"
        "  --chat <line>      seed a transcript line (repeatable); markup is\n"
        "                     parsed exactly as a server line's would be\n",
        program);
}

} // namespace

int main(int argc, char** argv) {
    flix::AppConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--host") config.host = next("--host");
        else if (arg == "--port") config.port = static_cast<std::uint16_t>(std::atoi(next("--port")));
        else if (arg == "--data") config.dataDir = next("--data");
        else if (arg == "--width") config.windowWidth = std::atoi(next("--width"));
        else if (arg == "--height") config.windowHeight = std::atoi(next("--height"));
        else if (arg == "--screenshot") config.screenshotPath = next("--screenshot");
        else if (arg == "--frames") config.screenshotAfterFrames = std::atoi(next("--frames"));
        else if (arg == "--menu") {
            // Matched against the menu's own label, lowercased and with the
            // spaces taken out, so "mobgallery" reaches "Mob Gallery" without
            // a second table of names to keep in step with the first.
            const auto slug = [](std::string text) {
                std::string out;
                for (const char c : text) {
                    if (c != ' ') out += static_cast<char>(std::tolower(c));
                }
                return out;
            };
            const std::string name = slug(next("--menu"));
            for (int id = 1; id < flix::kMenuCount; ++id) {
                if (slug(flix::menuLabel(static_cast<flix::MenuId>(id))) == name) {
                    config.autoMenu = static_cast<flix::MenuId>(id);
                }
            }
            if (config.autoMenu == flix::MenuId::None) {
                std::fprintf(stderr, "unknown menu: %s\n", name.c_str());
                return 2;
            }
        }
        else if (arg == "--lobby") config.autoJoin = false;
        else if (arg == "--spawn") config.autoSpawn = next("--spawn");
        else if (arg == "--login") config.forceLogin = true;
        else if (arg == "--dead") config.autoDead = true;
        else if (arg == "--tutorial") config.autoTutorial = true;
        else if (arg == "--stats") config.showStats = true;
        else if (arg == "--chat") config.seedChat.push_back(next("--chat"));
        else if (arg == "--user") config.autoUsername = next("--user");
        else if (arg == "--password") config.autoPassword = next("--password");
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage(argv[0]);
            return 2;
        }
    }

#ifdef __EMSCRIPTEN__
    // Before start(), which reads both files. MEMFS is gone the moment the tab
    // is, so the settings and the session token are moved onto storage the
    // browser keeps; everything else the client reads is embedded and
    // read-only, and stays in memory where it belongs.
    if (flix::web::mountStorage({kSessionName, std::string(kSessionName) + "-settings"})) {
        config.sessionFile = std::string(flix::web::kStorageDirectory) + "/" + kSessionName;
    }

    // Leaked deliberately: main() returns straight after registering the
    // callback and the app has to outlive it. The browser tab's teardown is
    // the process exit here.
    auto* app = new flix::App();
    std::string error;
    if (!app->start(config, error)) {
        std::fprintf(stderr, "could not start: %s\n", error.c_str());
        return 1;
    }

    // The native client saves from run(), on its way out. A tab has no way
    // out, so the page's own teardown is what stands in for it.
    g_app = app;
    flix::web::onPageHide([] {
        if (g_app) g_app->persist();
    });
    emscripten_set_main_loop_arg(
        [](void* handle) {
            auto* running = static_cast<flix::App*>(handle);
            if (running->step()) return;
            running->shutdown();
            emscripten_cancel_main_loop();
        },
        app,
        // 0: driven by requestAnimationFrame, which is the browser's own
        // vsync. A fixed rate here would fight it.
        0,
        // 0: do not simulate an infinite loop by unwinding the stack. main()
        // has nothing left to do, so there is nothing to preserve.
        0);
    return 0;
#else
    flix::App app;
    std::string error;
    if (!app.start(config, error)) {
        std::fprintf(stderr, "could not start: %s\n", error.c_str());
        return 1;
    }
    app.run();
    return 0;
#endif
}
