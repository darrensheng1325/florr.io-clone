// The offline build: the whole game in one page, with no server to reach.
//
// The same two programs as always -- server/main.cpp and client/main.cpp --
// but linked into ONE wasm and started together, so a single HTML file opened
// from disk is a complete game. Nothing here is a third implementation of
// anything: GameServer and App are the shipping objects, and they talk to each
// other over the same Listener/Dialer pair the network build uses. What is
// different is underneath them, and it is small:
//
//   * The transport. In a page, net/web_channel.cpp keeps an in-page listener
//     per port and resolves a connect() to 127.0.0.1 straight to it, handing
//     the two ends a pair of queues instead of a socket. The server listens on
//     its port as usual, the client dials as usual, and neither can tell.
//
//   * Who owns the loop. Both halves hand their step() to the browser: one
//     requestAnimationFrame callback steps the server, then the client. The
//     server first, so the input the client sent last frame is simulated
//     before this frame is drawn, and the snapshot that tick produced is what
//     this frame shows -- one frame of latency, the same as the loopback pair
//     would have on a native machine.
//
//   * Where the state goes. The page cannot write files, so the account
//     database is mirrored to browser storage by path (see client/web/persist.h
//     for why it cannot share the client's mount), restored before the server
//     opens it, and flushed when the page goes away. The client's settings and
//     session token use their own localStorage-backed mount, as the online
//     client does, under a prefix of their own.
//
//   * The fonts. The online page loads Ubuntu from Google Fonts; this one has
//     nowhere to load from, so the faces embedded in the wasm for measuring
//     are registered with the document for drawing too, and text is set in
//     the same bytes it was measured with.
//
// A scheduled `restart` is honoured the way a process restart would be: the
// page reloads. Everything that matters is in storage by then, and a reload
// is exactly "the same build, started fresh", which is what the command means.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include <emscripten.h>
#include <sys/stat.h>

#include "client/app.h"
#include "client/web/persist.h"
#include "server/game_server.h"

namespace {

/// The server's port. Only ever dialled from inside this page, so the number
/// is a convention rather than a choice; the client's Advanced Settings field
/// shows it, and retargeting that field at a real host still works.
constexpr std::uint16_t kPort = 3000;

/// Storage keys, under a prefix the online client never uses: a page served
/// from the same origin as the game must not present the other's session
/// token to a server that has never heard of it.
constexpr const char* kStoragePrefix = "flowrix-offline/";
constexpr const char* kSessionName = "session";
constexpr const char* kDatabaseName = "inventory.json";

/// The directory the database lives in. In memory: the page keeps it alive
/// through storage, not through the filesystem.
constexpr const char* kStateDirectory = "/state";

/// How often the database file is compared against what storage holds. The
/// server writes it every thirty seconds and on demand; a second later it is
/// in storage. The compare is one small read when nothing changed.
constexpr double kMirrorIntervalMillis = 1000.0;

flix::GameServer* g_server = nullptr;
flix::App* g_app = nullptr;
std::string g_databasePath;
std::string g_databaseKey;
double g_nextMirrorMillis = 0;

/// Puts a message where the loading text was, for a start that failed. The
/// runtime hides that element when main() runs, so a failure has to unhide
/// it: stderr goes to the console, which nobody double-clicking a file reads.
EM_JS(void, offline_report, (const char* textPtr), {
    const status = document.getElementById('loadingScreen');
    if (!status) return;
    status.hidden = false;
    status.textContent = UTF8ToString(textPtr);
});

/// Registers one embedded face with the document, so the canvas draws the
/// same Ubuntu the client measures with. Loaded from the bytes directly:
/// there is no URL to give it, and none is needed.
EM_JS(void, offline_register_font, (const char* familyPtr, const char* weightPtr,
                                     const char* data, int size), {
    if (typeof FontFace === 'undefined' || !document.fonts) return;
    const family = UTF8ToString(familyPtr);
    const weight = UTF8ToString(weightPtr);
    const bytes = HEAPU8.slice(data, data + size);
    try {
        const face = new FontFace(family, bytes, { weight: weight });
        document.fonts.add(face);
        face.load().catch((e) => console.warn('[fonts] ' + family + ' ' + weight + ': ' + e));
    } catch (e) {
        console.warn('[fonts] ' + family + ' ' + weight + ': ' + e);
    }
});

EM_JS(void, offline_reload, (), { location.reload(); });

void registerFont(const std::string& path, const char* weight) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[fonts] %s is not in the embedded data\n", path.c_str());
        return;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    offline_register_font("Ubuntu", weight, bytes.data(), static_cast<int>(bytes.size()));
}

void mirrorDatabase() {
    flix::web::mirrorFile(g_databaseKey, g_databasePath);
}

/// Everything the page would lose if it went away now. Called from the
/// pagehide handler, where there is no shutdown() coming: the tab is closing
/// or reloading, and the next frame will simply never be asked for.
void flushAll() {
    if (g_server) {
        g_server->persistAll();
        mirrorDatabase();
    }
    if (g_app) g_app->persist();
}

void frame(void*) {
    if (!g_server->step()) {
        // step() says no exactly once: a scheduled restart has fired, after
        // its warnings and the second it waits for them to leave. Everything
        // that survives a restart is put in storage, and the page starts over.
        g_server->shutdown();
        mirrorDatabase();
        if (g_app) g_app->persist();
        emscripten_cancel_main_loop();
        offline_reload();
        return;
    }

    const double now = emscripten_get_now();
    if (now >= g_nextMirrorMillis) {
        g_nextMirrorMillis = now + kMirrorIntervalMillis;
        mirrorDatabase();
    }

    if (g_app && !g_app->step()) {
        g_app->shutdown();
        g_app = nullptr;
        emscripten_cancel_main_loop();
    }
}

} // namespace

int main(int argc, char** argv) {
    // The page forwards the handful of client switches the online shell does,
    // for the same scripted runs; the rest of the client's options describe a
    // window or a screenshot and mean nothing in a tab.
    flix::AppConfig clientConfig;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--stats") clientConfig.showStats = true;
        else if (arg == "--lobby") clientConfig.autoJoin = false;
        else if (arg == "--user") clientConfig.autoUsername = next("--user");
        else if (arg == "--password") clientConfig.autoPassword = next("--password");
        else if (arg == "--chat") clientConfig.seedChat.push_back(next("--chat"));
        else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            return 2;
        }
    }

    // Storage first, before either half opens a file. Without it -- a private
    // window, cookies blocked -- both still run; they just forget, exactly as
    // the online client does. The database is restored by path (see above)
    // and lands in the directory the server is then pointed at.
    const bool stored = flix::web::mountStorage(
        {kSessionName, std::string(kSessionName) + "-settings"}, kStoragePrefix);
    ::mkdir(kStateDirectory, 0777);
    g_databasePath = std::string(kStateDirectory) + "/" + kDatabaseName;
    g_databaseKey = std::string(kStoragePrefix) + kDatabaseName;
    if (!stored) {
        std::fprintf(stderr, "[persist] no browser storage: progress will not survive a reload\n");
    } else if (!flix::web::restoreFile(g_databaseKey, g_databasePath)) {
        std::printf("[persist] no saved database; starting a new world\n");
    }

    // Leaked deliberately, both of them: main() returns as soon as the frame
    // callback is registered and they have to outlive it. The tab going away
    // is the process exit here.
    flix::ServerConfig serverConfig;
    serverConfig.port = kPort;
    serverConfig.dataDir = "data";
    serverConfig.databasePath = g_databasePath;
    g_server = new flix::GameServer();
    std::string error;
    if (!g_server->start(serverConfig, error)) {
        std::fprintf(stderr, "could not start the server: %s\n", error.c_str());
        offline_report(("could not start the server: " + error).c_str());
        return 1;
    }

    // Before the client's first frame, which is set in these faces.
    registerFont("data/Ubuntu-Regular.ttf", "400");
    registerFont("data/Ubuntu-Bold.ttf", "700");

    clientConfig.host = "127.0.0.1";
    clientConfig.port = kPort;
    clientConfig.dataDir = "data";
    if (stored) {
        clientConfig.sessionFile = std::string(flix::web::kStorageDirectory) + "/" + kSessionName;
    }
    g_app = new flix::App();
    if (!g_app->start(clientConfig, error)) {
        std::fprintf(stderr, "could not start the client: %s\n", error.c_str());
        offline_report(("could not start the client: " + error).c_str());
        return 1;
    }

    flix::web::onPageHide(flushAll);
    emscripten_set_main_loop_arg(frame, nullptr,
                                 // 0: requestAnimationFrame, the browser's own vsync.
                                 0,
                                 // 0: main() has nothing left to do, so there is no
                                 // stack to preserve by simulating a loop.
                                 0);
    return 0;
}
