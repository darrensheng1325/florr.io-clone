#pragma once

// Browser-side persistence for the emscripten client.
//
// Everything the client reads at startup -- the fonts, the SVGs, mobs.json,
// petals.json -- is embedded in the wasm and never changes, so MEMFS is the
// right home for it. Two files are not like that: the settings the player
// chose and the session token that keeps them logged in. Those are written by
// the client and have to still be there after a reload, and MEMFS is gone the
// moment the tab is.
//
// So those two are moved into their own directory, and that directory is
// backed by browser storage rather than memory. The file API above this does
// not change: ClientSettings still writes with an ofstream and App still reads
// with an ifstream. Only where the bytes end up is different.
//
// The single-file offline build has a third file with the same need: the
// server's account database, which lives in the page too. It cannot use the
// mount, because the mount pairs a storage key with the file object it
// created and the database is written atomically -- a temp file renamed over
// the old one -- so every save would be a new object the mount has no name
// for. restoreFile()/mirrorFile() below persist that one by PATH instead: the
// bytes at the path are copied to storage whenever they change, and copied
// back before the server opens the file. Two mechanisms, because the two
// write patterns are different; the storage under both is the same.

#include <string>
#include <vector>

namespace flix::web {

/// The directory the persistent files live in. Nothing else is stored there:
/// the mount below only knows about the names it was given, so a file that
/// appears here later would be an ordinary in-memory one.
extern const char* const kStorageDirectory;

/// What the online client's keys are prefixed with in storage. The offline
/// page uses a prefix of its own, so that a page served from the same origin
/// as the game never presents the other's session token to the wrong server.
inline constexpr const char* kDefaultStoragePrefix = "flowrix/";

/// Mounts kStorageDirectory on a WasmFS backend whose bytes live in the
/// browser's localStorage, and creates `names` inside it, each one restored
/// from whatever was saved under `prefix` + name last time. They are created
/// up front, and empty when there is nothing saved, so that every later open
/// is a write to a file that already exists -- see the note in the .cpp about
/// why that matters.
///
/// Returns false, having changed nothing, when the browser will not give the
/// page storage at all (private windows and blocked cookies both do this). The
/// client still runs in that case; it just forgets, as it did before.
bool mountStorage(const std::vector<std::string>& names,
                  const std::string& prefix = kDefaultStoragePrefix);

/// Copies what browser storage holds under `key` into the file at `path`,
/// creating the file's directory if it is missing. False when nothing is
/// stored under that key -- a first run -- or when there is no storage.
bool restoreFile(const std::string& key, const std::string& path);

/// Copies the file at `path` into browser storage under `key`, unless its
/// bytes are exactly what the last restore or mirror of that key saw. Cheap to
/// call often for that reason: a file that has not changed costs one read and
/// a compare, never a storage write. False when the file cannot be read or
/// storage refused the write.
bool mirrorFile(const std::string& key, const std::string& path);

/// Calls `flush` when the page is going away.
///
/// The native client saves on the way out of run(). A browser tab has no way
/// out: it is closed or reloaded, and the main loop is simply never called
/// again, so a save that waits for shutdown() is a save that never happens.
/// pagehide and a hidden visibilitychange are the two events that do fire, on
/// desktop and on mobile respectively, and localStorage is synchronous, so
/// there is time to write from either one.
void onPageHide(void (*flush)());

} // namespace flix::web
