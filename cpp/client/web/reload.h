#pragma once

// Replacing a stale browser client with the one the server is now serving.
//
// The client and the server ship in one archive and are updated together, so a
// server that has just restarted onto a new build is talking to pages that are
// still running the old one. The handshake catches that -- the protocol
// version and the content hash both have to match -- but catching it is only
// half an answer: the bytes that would agree are sitting on the server, and
// nothing but a page load fetches them.
//
// The browser build reloads. A native build cannot: its client is a binary
// somebody installed, and there is no version of "reload" that replaces it.

namespace flix::web {

/// Reloads the page so a stale build is replaced by the served one.
///
/// False means nothing happened, and there are two ways for that: a native
/// build, and a second call within one page session. If the freshly loaded
/// bundle STILL disagrees with the server then the mismatch is not staleness
/// -- a half-finished deploy, a proxy serving yesterday's wasm -- and a client
/// that reloads on every refusal would spin on it forever. The caller shows
/// its refusal message in that case, which is the only useful thing left.
bool reloadForStaleBuild();

/// Forgets that a reload was tried, so the NEXT stale build gets one too.
///
/// Called whenever a handshake succeeds. Without it the guard above is a
/// one-per-tab budget rather than a one-per-mismatch one: a page that reloaded
/// through this morning's deploy would sit stale through this afternoon's.
void clearStaleBuildGuard();

} // namespace flix::web
