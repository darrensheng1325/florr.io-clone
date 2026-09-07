// The game server. Headless: it draws nothing and opens no window.
//
// Two builds, one program. Natively it owns its loop and listens on a TCP
// port. Under emscripten it runs on Node, listens for WebSocket -- and, when a
// certificate is configured, WebTransport -- sessions, and hands step() to a
// timer, because blocking the Node event loop is exactly what would stop every
// message from ever being delivered.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "server/game_server.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <string.h>
#endif

namespace {

#ifndef __EMSCRIPTEN__
flix::GameServer* g_server = nullptr;

void onSignal(int) {
    // Only an atomic flag is stored here; flushing the database and telling
    // clients happens on the main thread, where it is safe to do.
    if (g_server) g_server->stop();
}
#endif

void usage(const char* program) {
    std::printf(
        "usage: %s [options]\n"
        "  --port <number>    listen port (default 4242)\n"
        "  --data <dir>       directory holding mobs.json and petals.json (default data)\n"
        "  --db <path>        account database (default inventory.json)\n"
        "  --seed <number>    simulation random seed\n"
        "  --max-players <n>  connection limit\n"
#ifdef __EMSCRIPTEN__
        "  --cert <path>      TLS certificate. Without it, cert.crt then\n"
        "                     dev-cert.crt are looked for in the working\n"
        "                     directory; https also enables WebTransport\n"
        "  --key <path>       private key for --cert\n"
        "  --web-root <dir>   directory the client is served from (default:\n"
        "                     the build directory this module is in)\n"
#endif
        ,
        program);
}

#ifdef __EMSCRIPTEN__
/// Mounts the real directory holding `databasePath` over the same path in the
/// virtual filesystem, so the accounts file the server reads and writes is an
/// ordinary file on disk. Called before start(), which loads it. Rewrites
/// `databasePath` to the absolute path it resolved to.
///
/// The resolving is the point. Node`s working directory and the emscripten
/// filesystem`s are not the same place: the latter is always `/`, so a
/// relative --db -- and the default is the bare `inventory.json` that
/// `pm2 start server.js` gets -- names a file in memory that has nothing to do
/// with the one on disk of the same name. The server then loads no accounts,
/// saves to nowhere, and says nothing about either, which is how a database
/// comes to look wiped while sitting intact beside the process that is
/// ignoring it.
bool mountDatabaseDirectory(std::string& databasePath, std::string& errorOut) {
    // The caller owns the buffer, as in net/web_channel.cpp: a path or a
    // failure message, and neither is worth exporting malloc for.
    char answer[1024] = {0};
    const int failed = EM_ASM_INT({
        const path = require("path");
        const fs = require("fs");
        const file = path.resolve(UTF8ToString($0));
        const directory = path.dirname(file);
        try {
            // The directory, not the file: --db may name one that does not
            // exist yet, which is an ordinary first run.
            fs.mkdirSync(directory, { recursive: true });
            FS.mkdirTree(directory);
            FS.mount(NODEFS, { root: directory }, directory);
        } catch (e) {
            stringToUTF8(directory + ": " + e, $1, $2);
            return 1;
        }
        stringToUTF8(file, $1, $2);
        return 0;
    }, databasePath.c_str(), answer, static_cast<int>(sizeof answer));

    if (failed) {
        errorOut = "the database directory cannot be reached from the host "
                   "filesystem (" + std::string(answer) + ")";
        return false;
    }
    databasePath = answer;
    return true;
}
#endif

} // namespace

int main(int argc, char** argv) {
    flix::ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--port") config.port = static_cast<std::uint16_t>(std::atoi(next("--port")));
        else if (arg == "--data") config.dataDir = next("--data");
        else if (arg == "--db") config.databasePath = next("--db");
        else if (arg == "--seed") config.worldSeed = std::strtoull(next("--seed"), nullptr, 10);
        else if (arg == "--max-players") config.maxPlayers = static_cast<std::size_t>(std::atoi(next("--max-players")));
        else if (arg == "--cert") config.certPath = next("--cert");
        else if (arg == "--key") config.keyPath = next("--key");
        else if (arg == "--web-root") config.webRoot = next("--web-root");
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage(argv[0]);
            return 2;
        }
    }

#ifdef __EMSCRIPTEN__
    // The embedded content is in memory and read-only, which is right for it.
    // The database is neither: it has to survive a restart, so the one
    // directory it lives in is the real filesystem, mounted at the path the
    // config already names. Everything else stays in MEMFS.
    std::string error;
    // Fatal, where it used to be a warning. Every account on this server is in
    // that one file; starting without it would serve an empty world and then
    // persist nothing, and both halves of that are silent.
    if (!mountDatabaseDirectory(config.databasePath, error)) {
        std::fprintf(stderr, "could not start: %s\n", error.c_str());
        return 1;
    }

    // Leaked deliberately: main() returns as soon as the timer is armed and
    // the server has to outlive it. Node exiting is this process's exit.
    auto* server = new flix::GameServer();
    if (!server->start(config, error)) {
        std::fprintf(stderr, "could not start: %s\n", error.c_str());
        return 1;
    }
    std::printf("listening on port %u\n", static_cast<unsigned>(config.port));
    emscripten_set_main_loop_arg(
        [](void* handle) {
            auto* running = static_cast<flix::GameServer*>(handle);
            if (running->step()) return;
            running->shutdown();
            emscripten_cancel_main_loop();
            std::printf("shut down cleanly\n");
        },
        server,
        // 0 lets the runtime pick; on Node that is a timer well above the
        // 20Hz the simulation needs, and step() ticks only when one is due.
        0, 0);
    return 0;
#else
    flix::GameServer server;
    std::string error;
    if (!server.start(config, error)) {
        std::fprintf(stderr, "could not start: %s\n", error.c_str());
        return 1;
    }

    // A write to a closed socket must not take the process down; the transport
    // reports the failure and drops that one connection instead.
    std::signal(SIGPIPE, SIG_IGN);

    g_server = &server;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::printf("listening on port %u\n", static_cast<unsigned>(config.port));
    server.run();
    std::printf("shut down cleanly\n");
    return 0;
#endif
}
