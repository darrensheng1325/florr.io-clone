#include "server/auto_update.h"

#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace flix::autoupdate {

#ifdef __EMSCRIPTEN__

namespace {

/// Text handed back over a caller-owned buffer rather than a malloc'd string,
/// for the same reason net/web_channel.cpp does it: returning one would need
/// _malloc exported to JavaScript, which is a link setting nothing else here
/// asks for. Progress lines are short; a drain that would overflow this has
/// gone wrong in a way an extra kilobyte does not fix.
constexpr int kTextCapacity = 8192;

} // namespace

// The whole install, as one Node job. It is asynchronous because the download
// is, and the server must go on ticking while it runs -- players are still in
// the world until the restart at the end of it.
EM_JS(void, flix_update_start, (const char* urlPtr), {
  const url = UTF8ToString(urlPtr);
  const state = (Module.flixUpdate = {
    log: [],
    running: true,
    outcome: 0,          // 0 none, 1 installed, 2 failed
    status: 'Update in progress: downloading build...',
  });
  const say = (line) => { state.log.push(line); console.log(line); };

  (async () => {
    const fs = require('fs');
    const os = require('os');
    const path = require('path');
    const { execFileSync } = require('child_process');
    let staging = null;
    try {
      // The directory this module was loaded from: the build directory, which
      // is where an update installs. It is `__dirname`, NOT process.argv[1]:
      // under pm2 (fork mode) argv[1] is pm2's own wrapper,
      // /usr/local/lib/node_modules/pm2/lib/ProcessContainerFork.js, which
      // require()s server.js -- so the old dirname(argv[1]) pointed at pm2's
      // lib directory and every update on a pm2-managed server refused itself
      // with "not a built deployment". __dirname is the emitted server.js's
      // own directory whoever loaded it. The server.js check still refuses to
      // overlay anything that is not a build directory.
      const runtimeDir = typeof __dirname === 'string' ? __dirname
                                                       : path.dirname(process.argv[1]);
      if (!fs.existsSync(path.join(runtimeDir, 'server.js'))) {
        throw new Error('Refusing to update: ' + runtimeDir +
                        ' is not a built deployment (no server.js).');
      }

      say('[UPDATE] Step 2/4: downloading ' + url + ' ...');
      staging = fs.mkdtempSync(path.join(os.tmpdir(), 'florr-update-'));
      const response = await fetch(url);
      if (!response.ok) {
        throw new Error('Download failed: HTTP ' + response.status + ' ' + response.statusText);
      }
      const bytes = Buffer.from(await response.arrayBuffer());
      // Zip local-file-header magic. Catches an error page saved as a "zip",
      // which is what a wrong branch name gets you.
      if (bytes.length < 4 || bytes[0] !== 0x50 || bytes[1] !== 0x4B) {
        throw new Error('Downloaded file is not a zip archive.');
      }
      const zipPath = path.join(staging, 'build.zip');
      fs.writeFileSync(zipPath, bytes);
      say('[UPDATE] Downloaded ' + (bytes.length / 1024 / 1024).toFixed(2) + ' MB');

      say('[UPDATE] Step 3/4: extracting build...');
      const extracted = path.join(staging, 'extracted');
      fs.mkdirSync(extracted);
      execFileSync('unzip', ['-oq', zipPath, '-d', extracted]);

      // A GitHub branch zipball wraps everything in "<repo>-<branch>/", but a
      // bare dist/ and a flat archive of dist's contents are both things
      // somebody will hand this, so all three are looked for.
      const candidates = [path.join(extracted, 'dist')];
      for (const entry of fs.readdirSync(extracted, { withFileTypes: true })) {
        if (entry.isDirectory() && entry.name !== '__MACOSX') {
          candidates.push(path.join(extracted, entry.name, 'dist'));
        }
      }
      candidates.push(extracted);
      const build = candidates.find((c) => fs.existsSync(path.join(c, 'server.js')));
      if (!build) {
        throw new Error('Downloaded archive does not contain a server build ' +
                        '(no dist/server.js) - update aborted, running build untouched.');
      }

      // Skipped at the TOP LEVEL only: inventory.json exists only at the dist
      // root, and a deeper file with the same name is legitimate build output.
      const preserved = new Set(
          ['inventory.json', 'node_modules', 'db_backups', 'cert.key', 'cert.crt']);
      const overlay = (from, to, depth) => {
        let copied = 0;
        for (const entry of fs.readdirSync(from, { withFileTypes: true })) {
          if (depth === 0 && preserved.has(entry.name)) continue;
          const source = path.join(from, entry.name);
          const target = path.join(to, entry.name);
          if (entry.isDirectory()) {
            fs.mkdirSync(target, { recursive: true });
            copied += overlay(source, target, depth + 1);
          } else if (entry.isFile()) {
            fs.copyFileSync(source, target);
            copied++;
          }
        }
        return copied;
      };
      // Safe on a live server: Node read this build into memory at startup and
      // never looks at the files again, and the wasm module is already loaded.
      const copied = overlay(build, runtimeDir, 0);
      say('[UPDATE] Step 4/4: installed ' + copied + ' files into ' + runtimeDir +
          ' (inventory.json and node_modules preserved).');
      state.outcome = 1;
      state.status = 'Update installed; restart pending.';
    } catch (error) {
      const message = error && error.message ? error.message : String(error);
      state.outcome = 2;
      state.status = 'Last update FAILED: ' + message;
      say('[UPDATE] FAILED: ' + message);
    } finally {
      state.running = false;
      if (staging) {
        try { fs.rmSync(staging, { recursive: true, force: true }); } catch (e) { }
      }
    }
  })();
});

// Whether this module runs under Node, which is the only runtime with a
// filesystem to install into. The same wasm can also be the single-file
// offline page, where the server lives in a browser tab: there is no `require`
// there, no `process`, and nothing an update could overlay, so every entry
// point below answers for that case too rather than throwing on the first
// `process` it touches.
EM_JS(int, flix_update_host_is_node, (), {
  return typeof process !== 'undefined' && process.versions && process.versions.node ? 1 : 0;
});

EM_JS(int, flix_update_running, (), {
  return Module.flixUpdate && Module.flixUpdate.running ? 1 : 0;
});

EM_JS(int, flix_update_take_outcome, (), {
  if (!Module.flixUpdate) return 0;
  const outcome = Module.flixUpdate.outcome;
  Module.flixUpdate.outcome = 0;
  return outcome;
});

EM_JS(void, flix_update_drain, (char* out, int capacity), {
  const lines = Module.flixUpdate ? Module.flixUpdate.log.splice(0) : [];
  stringToUTF8(lines.join('\n'), out, capacity);
});

EM_JS(void, flix_update_status, (char* out, int capacity), {
  const node = typeof process !== 'undefined' && process.versions && process.versions.node;
  const status = Module.flixUpdate ? Module.flixUpdate.status
               : !node
                   ? 'This is the offline page: the server runs inside the browser tab and ' +
                     'has no build directory to update. Download a newer offline.html instead.'
                   : 'No update has been run since this server started.';
  stringToUTF8(status, out, capacity);
});

EM_JS(void, flix_update_default_url, (char* out, int capacity), {
  const env = (typeof process !== 'undefined' && process.env) || {};
  const repo = env.FLORR_UPDATE_REPO || 'flowrix-io/florr_clone';
  const branch = env.FLORR_UPDATE_BRANCH || 'web';
  const url = env.FLORR_UPDATE_URL ||
              ('https://codeload.github.com/' + repo + '/zip/refs/heads/' + branch);
  stringToUTF8(url, out, capacity);
});

bool supported() { return flix_update_host_is_node() != 0; }
bool inProgress() { return flix_update_running() != 0; }

std::string lastStatus() {
    std::vector<char> buffer(kTextCapacity, 0);
    flix_update_status(buffer.data(), kTextCapacity);
    return std::string(buffer.data());
}

std::string defaultUrl() {
    std::vector<char> buffer(kTextCapacity, 0);
    flix_update_default_url(buffer.data(), kTextCapacity);
    return std::string(buffer.data());
}

bool start(const std::string& url) {
    if (!supported() || inProgress()) return false;
    flix_update_start(url.c_str());
    return true;
}

std::vector<std::string> drainLog() {
    std::vector<char> buffer(kTextCapacity, 0);
    flix_update_drain(buffer.data(), kTextCapacity);
    std::vector<std::string> lines;
    const std::string joined(buffer.data());
    std::size_t at = 0;
    while (at < joined.size()) {
        const std::size_t end = joined.find('\n', at);
        lines.push_back(joined.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return lines;
}

Outcome takeOutcome() {
    switch (flix_update_take_outcome()) {
        case 1: return Outcome::Installed;
        case 2: return Outcome::Failed;
        default: return Outcome::None;
    }
}

#else

// The native build. Nothing here is a stub for something that could work: the
// zipball carries a JavaScript build, and there is no version of "install it"
// that produces a native binary.

bool supported() { return false; }
bool inProgress() { return false; }

std::string lastStatus() {
    return "This server was not installed from a build archive, so it cannot update itself.";
}

std::string defaultUrl() {
    if (const char* fromEnv = std::getenv("FLORR_UPDATE_URL")) return fromEnv;
    return "https://codeload.github.com/flowrix-io/florr_clone/zip/refs/heads/web";
}

bool start(const std::string&) { return false; }
std::vector<std::string> drainLog() { return {}; }
Outcome takeOutcome() { return Outcome::None; }

#endif

} // namespace flix::autoupdate
