# florr C++ — architecture

A clean-sheet C++ rewrite. The TypeScript tree in `src/` is a **behavioural
reference only**: it says what the game feels like and what its data files
mean. None of its structure, workarounds or defects carry across.

What is deliberately kept:

* **Gameplay style** — a flower with an orbiting ring of petals, attack/defend,
  tiered mobs, loot that drops and is absorbed into an inventory, crafting up
  the rarity ladder.
* **UI style** — the flat, rounded, high-contrast look: thick dark outlines,
  rarity-coloured panels, chunky text with a stroked outline.
* **`src/mobs.json` and `src/petals.json`** — loaded verbatim, including the
  inline SVG artwork.
* **`inventory.json`** — the account database, read and written in the same
  JSON shape.

What is deliberately *not* kept: the wire protocol, the entity model, the
rendering stack, the DOM. **There is no HTML and no CSS anywhere in this
project.** The client is a native SDL2 application and every pixel of it —
title screen, login form, HUD, inventory, chat — is drawn through the
`cpp_canvas` 2D API.

---

## Layout

```
cpp/
  shared/         code both binaries link
    core/         entity.h component.h world.{h,cpp} types.h json.{h,cpp}
    net/          bytebuffer.h protocol.h transport.{h,cpp}
                  web_channel.{h,cpp} -- WebSocket/WebTransport, emscripten
    game/         constants.h rarity.{h,cpp} components.h config.{h,cpp}
  server/         headless authoritative simulation
  client/         SDL2 window, rendering, UI, prediction
    ui/markup.*   the HTML subset chat lines arrive in
    web/          the emscripten build's shell page
  third_party/
    cpp_canvas/   the vendored Canvas2D-alike renderer
  data/           runtime copies of mobs.json / petals.json / mob_xp.json
  tests/          one binary, all tests
```

Includes are always repo-relative from `cpp/`: `#include "shared/core/world.h"`.

---

## The ECS

`shared/core/world.h`. Archetype storage: entities with the same component set
share contiguous per-component columns.

```cpp
World world;
Entity e = world.create();
world.add<Transform>(e, Transform{{100, 200}, 0.0});
world.add<Health>(e, Health{50, 100});

Query<Transform, Motion> movers{world};      // build ONCE, reuse every tick
movers.each([&](Entity e, Transform& t, Motion& m) {
    t.position += m.velocity * dt;
});
```

Rules that matter:

* **Build queries once**, as a member of the system. Constructing one per tick
  throws away the archetype cache that makes iteration free.
* **Never create or destroy an entity inside `each`.** Column pointers are live
  during iteration. Record the intent in a `CommandBuffer` and let the runtime
  flush it between phases.
* **Frequently-toggled state is a field, not a tag.** Adding or removing a
  component relocates the entity and copies all of its data. `Dead` is a tag
  because an entity dies once; "is currently poisoned" is a field on
  `Afflictions` because it changes constantly.
* Component structs live in `shared/game/components.h` and are registered with
  `FLIX_COMPONENT(...)` at global scope. Add new ones there, not locally.

## Simulation

Fixed **25 Hz** (`net::kTicksPerSecond`). The server never varies its step.

Phase order, once per tick — later phases may rely on earlier ones having run:

1. **Input** — drain client input into `PlayerInput`.
2. **Intent** — AI decides targets and headings; player input becomes a desired
   velocity.
3. **Movement** — integrate velocity under friction, resolve tiles, apply and
   decay knockback.
4. **Rings** — petals are placed on their owner's ring.
5. **Combat** — contact damage, petal hits, projectiles, afflictions.
6. **Lifecycle** — deaths, XP and loot awards, drops, despawns, spawning.
7. **Replication** — build and send each client's snapshot.
8. **Flush** — apply the tick's `CommandBuffer`.

Deaths mark `Dead` rather than destroying, so everything later in the same tick
still sees the entity. The reaper destroys them in phase 8.

## Networking

`shared/net/`. `[u32 length][u8 type][payload]`, little-endian, no type tags
inside a payload. Over TCP natively and over WebSocket or WebTransport in an
emscripten build -- see Transports below; the framing is the same either way. `protocol.h` is the single source of truth for message
ids and field order; both sides read it.

* `Listener` (server) and `Dialer` (client) both drive a `poll()` loop and hand
  whole frames to a `TransportHandler`.
* A malformed length prefix drops the connection; a truncated frame yields a
  zeroed message the handler discards. Neither can corrupt state.
* Snapshots are per-client and viewport-scoped. An entity entering view is sent
  once as a spawn record (kind, type, rarity, name); afterwards only the fields
  in its `UpdateFields` mask.
* `NetId` is a never-reused u32, distinct from `Entity`, so a client that missed
  a removal cannot apply an update to the wrong thing.

## Client prediction

The client runs `integrateVelocity` from `shared/game/constants.h` — literally
the same function the server runs. It keeps unacknowledged inputs in a ring,
and on each snapshot snaps to the authoritative position and replays the inputs
the server has not yet acknowledged. In open movement the two agree exactly and
nothing visibly corrects. Remote entities are interpolated one snapshot behind.

## Rendering

`third_party/cpp_canvas`, a Canvas2D-shaped API (`save`/`restore`,
`translate`/`rotate`/`scale`, paths, `fill`/`stroke`, `fillText`,
`drawCanvas`). `SvgDocument::fromString()` compiles the SVG strings in
`mobs.json` / `petals.json` into canvas calls — that is how mob and petal
artwork survives the rewrite intact.

Draw order per frame: terrain → ground effects → drops → mobs → petals →
players → floating damage → HUD → panels → cursor.

## The web build

`emcmake cmake` builds the client AND the server for a JavaScript runtime:

```
mkdir cpp/build-web && cd cpp/build-web
emcmake cmake .. -DFLIX_BUILD=release
cmake --build . -j8      # -> bundle.{html,js,wasm}, server.{js,wasm}
```

`npm run build` at the repository root does the same thing and then stages the
result in `dist/`, which is what ships: `scripts/build-web.js` configures and
builds both web targets and copies them out. `npm start` is that build followed
by running it, so what it serves is never a half-stale `dist/`. Both pin
`FLIX_BUILD=release` rather than letting the environment decide — they are the
commands that produce what ships. `npm run build:dev` is the dev-flavoured
build, and `npm run start_nobuild` runs whatever is already staged. Switching
flavour deletes `cpp/build-web` first: the Makefile generator does not make an
object depend on the flags it was compiled with, so reconfiguring in place would
relink the previous flavour's objects with the new flavour's name on them.
The web build's outputs carry the names the TypeScript build's did
— the page's script is `bundle.js` and the server is `server.js` — so nginx,
pm2 and the autoupdate zipball did not have to learn new ones; `bundle.html` is
staged as `dist/index.html`. Only the web build is renamed, at the link, by
`OUTPUT_NAME` in CMakeLists.txt: the native binaries are still `flowrix_client`
and `flowrix_server`.

`-DFLIX_BUILD` picks the flavour, and is the only build knob: `CMAKE_BUILD_TYPE`
follows from it rather than being set alongside it.

* **`dev`** (cmake's default; the npm scripts pin `release`) — `-O2 -g`, `assert()` live, frame pointers kept, and
  emscripten's own heap and stack checks on. The native client remains
  optimised because its CPU rasterizer is not useful at `-O0`; the web client
  always delegates drawing and compositing to the browser's Canvas2D backend.
  The web link is left at the configuration's own `-O2 -g`, because wasm-opt
  at `-O3` is most of the wait on a relink.
* **`release`** — `-O3`, `NDEBUG`, no debug info, emscripten's checks off, and
  the client's web link pinned at `-O3` whatever else is on the line. About 2MB
  of wasm against dev's 19MB.

Both are the same programs as the native ones — the same tick and the same
systems issuing the same Canvas-shaped draw list. The native build consumes
that list with its software rasterizer; Emscripten sends every draw, offscreen
blit, text run, and composite operation to browser Canvas2D. It never allocates
the software framebuffer in Wasm. The builds otherwise differ in three places.

* **Who owns the loop.** Natively `App::run()` and `GameServer::run()` do. In a
  page the event loop belongs to the browser, and under Node it belongs to
  Node; either way, blocking it is exactly what would stop every message from
  ever being delivered. So both `main.cpp`s hand `step()` to
  `emscripten_set_main_loop_arg` and return.

* **What carries the bytes.** See below. The framing does not change:
  `[u32 length][u8 type][payload]` is what finds message boundaries, and it
  does so identically whether the bytes arrived as discrete WebSocket messages
  or as a QUIC stream that split and coalesced them however it liked.

* **Where the content lives.** `--embed-file`, not `--preload-file`: mobs.json,
  petals.json, the map bundle, the biome SVGs and the fonts are inside the
  wasm. The client reads all of them synchronously during `start()`, so there
  is nothing to gain from a fetch it would have to wait for; a single artifact
  cannot half-deploy the way a `.wasm` and a stale `.data` beside it can; and
  the server has no page to run a preload from at all.

`client/web/shell.html` is the page. It does no rendering and picks no
transport: it sizes nothing, hands the wasm the canvas element and the argv
`main()` would have had natively, and gets out of the way. It defaults to its
own origin, so an untouched URL already points at the server that served it;
`?host=`/`?port=` are only for a client build hosted somewhere else.

## Serving the client

The wasm server serves it, over the same port it plays the game on:

```
node cpp/build-web/server.js --port 4242 --db inventory.json
# -> https://localhost:4242/
```

`npm start` is this, pointed at the staged build instead: `node dist/server.js
--port ${PORT:-3000} --db dist/inventory.json --web-root dist`, run from the
repository root so that the certificate lookup finds `cert.crt` / `dev-cert.crt`
where they live.

One process and one origin for the page, the WebSocket, the QUIC listener and
`/transport-info`. Nothing else has to be running, and the page is same-origin
with its server — which is what a secure context needs before WebTransport is
possible at all.

It follows the TypeScript server here, because the two share these files:

* **HTTPS when there is a certificate.** `--cert`/`--key` name it; with
  neither, `cert.crt` then `dev-cert.crt` are looked for in the working
  directory. **Validity decides, not order** — the committed `cert.crt`
  outlives its own dates while the generated dev pair is refreshed, and
  serving the dead one would cost every browser the connection and cost
  WebTransport its pinnable digest. Finding nothing serves plain HTTP, and
  WebSocket is then the only transport on offer.
* An expired certificate is **reported, not regenerated**. `npm start` owns
  that file; two writers is how they come to disagree.
* `--web-root` is the directory served, defaulting to the build directory the
  server module was loaded from — which is where the client build already is.
  `/` serves `index.html` or `bundle.html`. Requests are decoded,
  normalised and required to stay under the root, and `.wasm` is served as
  `application/wasm` so the browser will stream-compile it.

## Transports

`shared/net/web_channel.h` is the seam. Natively `transport.cpp` moves bytes
with `socket()`, `connect()`, `accept()` and `recv()`; a browser tab has none
of those, and emscripten's BSD-socket emulation offers WebSocket and nothing
else. So the emscripten build asks this layer for whatever the runtime has:

| | native | emscripten |
|---|---|---|
| client → server | TCP | WebTransport, else WebSocket |
| server listens on | TCP | WebSocket, plus WebTransport with a certificate |

The choice is made per connection, by the client, while it connects:

1. `GET /transport-info` asks the server what it offers. No answer means
   WebSocket, which is the safe assumption anyway.
2. WebTransport is attempted only if the runtime implements it, the page is a
   secure context (the API requires it), and the server said yes. A
   development certificate that no public CA vouches for is pinned by the
   `certHashes` digest the server publishes, which is what makes it work on
   localhost with no trust-store setup.
3. Anything at all going wrong — no UDP path, an untrusted certificate, a
   timeout — falls through to WebSocket. One wasted round trip is the whole
   cost of trying.

The server's QUIC listener is equally best-effort: `@fails-components/webtransport`
is an optional native dependency and needs a certificate, and failing either
costs the deployment WebTransport and nothing else. Without a certificate the
listener is plain HTTP and WebSocket only, because WebTransport is
secure-context only and there would be nothing to offer — see Serving the
client above for how one is found.

Only `Connection`'s byte movement and `poll()` differ between the two
backends. The framing, the frame loop, the backlog rule and the conditions
that end a connection are one implementation — `Listener::service()` — shared
by both, because a copy of those rules per backend is how they would come to
differ in more.

**The two pairs do not interoperate.** A native client speaks TCP and a wasm
server speaks WebSocket; neither knows the other's transport. That is a
deployment choice, not an accident: giving the native build a WebSocket client
would mean carrying an HTTP upgrade and a frame codec in C++ for a case that
has not come up.

## Chat markup

Chat content is not plain text on the wire. The server sends
`<b style="color: #2bffa4;">A super crab has spawned!</b>` and joins multi-line
answers with `<br/>`. `client/ui/markup.h` parses that subset into styled runs;
the transcript in `App::drawChat` lays those out rather than splitting the raw
string on spaces, which used to print the tags.

* `b`, `strong`, `i`, `em`, `u`, `blink`, `span`, `font`, `color` and `br` are
  honoured in every build. There is no italic face, so `<i>` is sheared, the
  way a browser synthesises a missing one.
* `<a>` needs somewhere to navigate, so it is honoured only under
  `FLIX_WEB_BUILD` and dropped — with its content, as any unknown tag is —
  natively. `<img>` is dropped everywhere: the transcript is glyph outlines,
  not elements.
* **`script` and `iframe` are dropped with their content in every build.** The
  old browser client offered a "click to run" button for one and a "click to
  show embed" button for the other. Nothing here reinstates either, and no code
  path in this client executes or embeds what a chat line asks it to. If you
  are adding a tag, that is the line not to cross.

## Style

Match the surrounding code:

* C++17. `snake_case` files, `PascalCase` types, `camelCase` functions and
  variables, `kCamelCase` constants.
* Comment *why*, not *what*. A comment earns its place by explaining a
  non-obvious decision, an invariant, or a trap. No banner comments restating
  a function's name.
* No exceptions in the tick path. Bad input is clamped or dropped, never thrown.
* Tests go in `cpp/tests/`, use `tests/test.h` (`TEST`, `CHECK`, `CHECK_EQ`,
  `CHECK_NEAR`), and must cover the edge cases — not just the happy path.
* Never introduce HTML, CSS, or a DOM dependency.
