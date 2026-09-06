#include "client/web/reload.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace flix::web {

#ifdef __EMSCRIPTEN__

// The guard lives in sessionStorage rather than in a static here, because the
// thing it is guarding against survives the C++ side entirely: a reload throws
// this whole module away and starts a new one, so a flag in memory would be
// clear again by the time it was next read and the page would reload forever.
// The key is the browser build's own (src/ws_client.ts), so the two clients
// cannot both be mid-reload against one tab.
EM_JS(int, flix_reload_for_stale_build, (), {
  const key = 'florr:proto-reload';
  try {
    if (sessionStorage.getItem(key)) return 0;
    sessionStorage.setItem(key, String(Date.now()));
  } catch (e) {
    // No storage at all (a private window, blocked cookies). Treated as a
    // first attempt: one reload that might not stick is better than none.
  }
  location.reload();
  return 1;
});

EM_JS(void, flix_clear_stale_build_guard, (), {
  try { sessionStorage.removeItem('florr:proto-reload'); } catch (e) { }
});

bool reloadForStaleBuild() { return flix_reload_for_stale_build() != 0; }
void clearStaleBuildGuard() { flix_clear_stale_build_guard(); }

#else

bool reloadForStaleBuild() { return false; }
void clearStaleBuildGuard() {}

#endif

} // namespace flix::web
