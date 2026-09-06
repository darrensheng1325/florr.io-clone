#pragma once
// The `update` admin command's engine: fetch the latest build and install it
// over the running one.
//
// The pipeline is src/server/autoUpdate.ts, unchanged in shape because the
// thing it installs is unchanged: `dist/` is committed to the repository, so
// the branch zipball IS the build, and `dist/inventory.json` is gitignored, so
// the archive can never contain a database. Steps:
//
//   1. Back up the database. MANDATORY, and done by the caller before any of
//      this is started -- a failure there aborts before a file is touched.
//   2. Download the zipball.
//   3. Unzip into a staging directory (GitHub wraps everything in one
//      "<repo>-<branch>/" folder, which is why the build is searched for).
//   4. Overlay it onto the directory the server is running from. inventory.json
//      and node_modules are never overwritten -- the first is the live
//      database, the second is where the Pi's source-built uWebSockets.js
//      lives.
//   5. Schedule a restart, which is what actually loads the new build.
//
// Only the Node build can do any of this, and that is not a limitation: the
// zipball carries `dist/server.js` and `dist/server.wasm`, which is exactly
// what `npm start` runs. A native binary is not in it, so a native server has
// nothing to install and says so rather than pretending.

#include <string>
#include <vector>

namespace flix::autoupdate {

/// Whether this build can install anything at all.
bool supported();

/// True while a download or install is in flight. Nothing may be scheduled on
/// top of one: it takes seconds, and two overlays into one directory is how a
/// half of each build ends up installed.
bool inProgress();

/// What the last update did, for `update status`.
std::string lastStatus();

/// The build the zipball is fetched from. The same two environment variables
/// the browser build reads override it, so a fork updates from its own repo
/// without a rebuild.
std::string defaultUrl();

/// Starts an install. False when one is already running, or when this build
/// cannot install.
bool start(const std::string& url);

/// Progress lines produced since the last call, oldest first. Drained by the
/// server once a tick and forwarded to whoever asked for the update.
std::vector<std::string> drainLog();

/// How a finished install ended. `None` until it does; the finished states are
/// reported exactly once, on the first call after the fact.
enum class Outcome { None, Installed, Failed };
Outcome takeOutcome();

} // namespace flix::autoupdate
