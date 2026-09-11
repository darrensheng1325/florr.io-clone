# The world maps

Every world the server runs lives here, in [Tiled](https://www.mapeditor.org)'s
own format. Open a `.tmj` in Tiled and edit it; there is no other map source and
there is no build step.

```
maps/
  maps.json      the manifest: which maps exist, in the order that fixes realms
  garden.tmj     a map: its art layers, its collision and every annotation
  tileset.tsj    the tile palette: one entry per tile, and its art
  tiles/*.svg    one artwork per tile
  ground/*.svg   the nine title-screen backdrops (not map data any more)
  README.md      this
```

## The manifest

`maps.json` is the authority on which maps exist, and its **order is the
contract**: entry 0 is realm 0, entry 1 is realm 1, and so on. A client and a
server that read this file agree about which realm is which map, which is what
makes a teleporter's `targetMap` and a saved spawn choice mean the same thing on
both ends. Reorder it and everyone standing in a map moves to another one.

A map's **id is its file stem** — `garden.tmj` is the map `garden` — and that is
what a teleporter aims at and what qualifies a spawn point's id. At most
`kMaxWorldMaps` (62) maps can be loaded at once. The directory is never scanned:
a map nobody listed is not a realm, and a listed map that is missing fails the
build rather than the server.

The manifest and every map's bytes are covered by the content hash, so a client
running a different map than the server it dials is refused at the handshake
rather than discovered by walking into a wall nobody else can see.

## A map is a Tiled map, in Tiled's idiom

There is no house format layered over Tiled's. A map has as many tile layers as
the author wants, drawn bottom to top in file order; it may name any number of
tilesets; tiles may be flipped and rotated; and the edge and corner art is
chosen by Tiled's **terrain (Wang) brushes**, not by a script of ours. Two
conventions carry the rest: one about the grid, which is checked on load rather
than assumed, and one about what a layer means.

**One Tiled pixel is one world unit.** The tile size is 300×300, which is
`kTileSize` in `cpp/shared/game/constants.h`. Because they match, an object's
`x`/`y`/`width`/`height` in the file is already a world rectangle and nothing is
scaled on the way in. A map saved at a different tile size is refused rather
than guessed at — there is no scale factor that is right for the grid *and* the
objects. An infinite or non-orthogonal map is refused for the same reason.

**A layer's name is a note to the author; its `has_collision` is not.**
`background`, `water`, `dirt`, `castle` mean nothing to the game, there is no
layer that "is" the terrain, and nothing reads a layer by name. The one thing
the game does read off a layer is a custom boolean, `has_collision`, and that
boolean is the whole of collision — see below. Otherwise the layers are drawn
bottom to top and that is all they do.

### Flips

Tiled packs three flip bits into the top of a gid — horizontal `0x80000000`,
vertical `0x40000000`, anti-diagonal `0x20000000` — because one edge tile serves
all four rotations of itself. The reader carries them through per cell
(`kTileFlipHorizontal` / `Vertical` / `Diagonal`) and the renderer applies them
in **Tiled's order: the anti-diagonal flip first (transpose), then horizontal,
then vertical**. Any other order draws three of the four rotations wrong.

Collision masks the bits off: a rotated wall is still a wall.

## Collision: the layer's `has_collision`

**Layers with collision enabled are walls; layers with it disabled are not.**
A tile layer may carry one custom boolean in Tiled, under *Layer → Custom
Properties*:

| property | meaning |
| --- | --- |
| `has_collision` | anything painted on this layer blocks movement |

Per cell, over every layer of the map:

```
blocked <- ANY layer with has_collision = true has a NON-EMPTY tile here
kind    <- the TOPMOST blocking tile here is tagged `water` ? water : wall
else       ground
```

A layer with `has_collision` absent or false **never** blocks, whatever art it
holds; a map that sets it nowhere has no walls at all. The art is not consulted
about whether a cell blocks — the same grass tile is solid on a colliding layer
and walkable on a layer below it.

This is a layer property rather than a tile property because a layer is a thing
the author can see, name and toggle in Tiled, and paints a whole region of
wall in one go. Collision used to be a `solid` boolean on each tile in the
tileset, and a Wang brush is exactly the thing that defeats that: a brush paints a
family's centre, edge and corner tiles interchangeably, so one corner tile
nobody remembered to tag was a hole in a wall that no amount of repainting
would close, and finding it meant walking into it. A layer has one switch and
the author has already decided which layer the walls go on.

The cost is that **stacking no longer subtracts.** A bridge drawn over a pond
does not make the cell walkable if the pond's layer collides: the pond cell is
still non-empty and still blocks. To open a hole through a colliding layer you
erase the cell on that layer — the brush's eraser, not a tile painted on top of
it. Sequencing is a drawing question now and a collision question never.

Only three tile values ever come out of the reader — ground, wall and water —
and the engine's `Tile` enum, `tileBlocks()` and `tileIsWater()` are unchanged.
Water blocks movement, as it always has.

### The tileset's `water`, which is a kind and not a verdict

A tile in `tileset.tsj` may carry a `water` boolean. It answers **what kind of
blocker this cell is**, never **whether it blocks**:

| property | meaning |
| --- | --- |
| `water` | a blocking cell whose topmost blocker is tagged this reads as water rather than wall |
| `covers_everything` | a drawing hint, not collision: see below |

The difference is visible rather than physical — the minimap paints water its
own colour, and `tileIsWater()` is what anything asking "is this thing in the
drink" reads — because water already blocked before any of this. Tagging a
tile `water` and painting it on a non-colliding layer produces plain ground:
the kind is only ever asked about a cell that is already blocked.

With `solid` gone there is nothing left in the tileset that can contradict the
engine: `water` is a label on a decision the layer already made.

### What is tagged today

`tileset.tsj`'s 77 tiles carry two tags between them:

| tag | tiles |
| --- | --- |
| `water` | `ocean_c_0`…`ocean_c_3`, `water_c_0`, `water_l_0`, `water_tl_0`, `water_tri_0`, `sewage_c_0`, `sewage_l_0`, `sewage_tl_0`, `sewage_tri_0` — 12 |
| `covers_everything` | the full-square centre tiles: `desert_c_0`…`desert_c_4`, `grass_c_0`…`grass_c_3`, `ocean_c_0`…`ocean_c_3`, `pvp_c_0`…`pvp_c_3`, `castle_c_0`, `dirt_c_0`, `dirt2_c_0`, `dirt2_c_1`, `water_c_0`, `sewage_c_0` — 23 |

Everything else is untagged, which now costs nothing: an untagged tile on a
colliding layer is a wall like any other.

`garden.tmj` puts its four layers to work as `background` (no collision) under
`water`, `dirt` and `castle` (all three colliding), which makes about **55% of
the map solid**. That is deliberate: it is a castle and its grounds, not an
open field. The one door sits in the open corner at the bottom left, where 42
of the 56 cells it covers are ground.

### `covers_everything`

A tile with `covers_everything` fills its whole 300-unit square opaquely, so
nothing painted under it can show through. The renderer uses it to stop drawing
a cell's stack early. It is a drawing hint and nothing about the game depends on
it; a tile that claims it wrongly shows as art missing under a translucent edge,
never as a collision bug.

## The object layers

Annotations are grouped by kind, one Tiled object layer each, so a set can be
hidden while another is worked on. **These three layer names are read**, unlike
the tile layers':

| layer | holds |
| --- | --- |
| `spawns` | mob bands and mob regions — **polygons** |
| `player_spawns` | doors: where a player arrives — rectangles |
| `teleporters` | pads: where a player leaves — points |

The game never sees the grouping beyond the kind: every reader filters by kind
before it looks at order, so only the order *within* a layer is observable, and
that is preserved.

### `spawns` — bands and regions

A `spawn` object answers up to two independent questions, and which properties
it carries decides which kind of object it is:

| property | meaning |
| --- | --- |
| `spawnType` | the **tier**: `common` … `ultra`. Makes this a **band**. |
| `mobs` | the **distribution**: what actually appears here |

- A shape with `spawnType` is a **band**. It owns a population of its own,
  stocked to a density scaled by the outline's area, and the ambient fill stays
  out of it. This is where the map's difficulty progression lives.
- A shape with only `mobs` is a **region**: it says what grows on this ground
  and owns nothing. The ambient fill spawns inside it freely, at its own natural
  tier spread, and asks the region only *what*.

The two have different shapes on purpose: danger runs in bands along a
coastline, while "this is the desert" covers a quarter of the map.

Bands are **polygons**. Draw one with Tiled's polygon tool and it can follow a
coastline or a canyon; a rectangle over the same ground either spills mobs onto
the next tier's territory or leaves a wedge of its own permanently empty. Every
broadphase question still goes to the bounding box — is this zone near a
viewport, is it worth looking at — and only three go to the outline: is this
point inside, how large is it, where inside should this mob go. **The boundary
is inside**, as it was when these were rectangles. A rectangle object still
loads and stays a rectangle.

### `player_spawns` — doors

A door is where a player arrives: from the title screen's picker, from a
teleporter, or on respawn.

| property | meaning |
| --- | --- |
| `spawnId` | what a teleporter or a saved preference names this door by. Unique within its map; the server qualifies it as `<map id>:<spawn id>` |
| `label` | what the picker's button says. Empty falls back to the id, title cased |
| `color` | the button's colour, `#rrggbb` |
| `order` | where the button sits in its row; ties break by map order |
| `backdrop` | the artwork tiled behind the picker while this button is chosen, by file name. Empty falls back to the spawn id, so a door called `desert` gets `desert.svg` |
| `biome` | which tab of the picker files this door. Empty falls back to the map's `biome`, then to the map's id |
| `pickable` | whether the title screen **offers** this door. Default `true` |

**A door needs no properties at all.** Three places an author might have put the
id are tried in order, so a door drawn with Tiled's default fields still works:

1. the `spawnId` property, or the object's **name** (Tiled's name field);
2. a slug of the `label` — lower-cased, every run of punctuation or space
   collapsed to one underscore, so `"Garden"` becomes `garden`;
3. the **map's id**, which every map has.

That is why the one nameless door in `garden.tmj`, whose only distinguishing
property is `label: "Garden"`, is the pickable door `garden`.

Only a map with several unnamed doors can now collide, and that shows up as a
duplicate id rather than as a door that silently vanished.

`pickable` is the main-area rule: a biome's sublevels are entered from its main
area through a pad, so their doors are arrival points and nothing more, and only
the main area's door is a button. A non-pickable door is still joinable — by an
admin, by name.

### `teleporters` — pads

| property | meaning |
| --- | --- |
| `targetMap` | the id of the map this pad leads to |
| `targetSpawn` | the door in that map to arrive at. Empty means its default |
| `teleportToX`, `teleportToY` | an explicit arrival point in the target map's coordinates, for a pad that wants somewhere no door covers. `targetSpawn` wins when both are given |

A pad naming no map is scenery: it charges up and goes nowhere, and that is
reported at load rather than at the moment a player stands on it. Stepping
through one is a `RealmChange`, because each map is its own coordinate space —
as is a respawn that crosses maps.

## The map's own properties

Set these in Tiled under *Map → Map Properties → Custom Properties*.

| property | meaning | default |
| --- | --- | --- |
| `displayName` | what the map is called in a message | the map's id |
| `biome` | which tab of the spawn picker this map's doors file under | **the map's id** |
| `defaultMobGroup` | the mob group a band with no `mobs` of its own spawns from | **`biome`** |

Both defaults exist so that a one-biome map does not have to say its own name
three times. `garden.tmj` declares none of them and is therefore the map
`garden`, in biome `garden`, growing `garden` mobs.

## Mob groups

A distribution row is a **name and a weight**:

```
mobs = garden 50% hornet 50%
mobs = ocean 20% jellyfish 80%
mobs = hornet
```

The name is a **mob group** when `src/mobs.json` defines one by that name, and a
**mob id** otherwise; groups win, so naming a group is never ambiguous. The
groups today are `garden`, `desert`, `ocean`, `hel`, `ant_hell`, `jungle`,
`sewers` and `computer` — there is no separate list of them, they are the union
of the names the mobs claim:

```json
"groups": ["garden", "jungle"]           // in both, at spawn_weight
"groups": {"garden": 1, "jungle": 0.4}   // weighted per group
```

A weight of zero is meaningful and kept: a centipede's body segments belong to
the garden — tools should say so — but are only ever spawned by the head.

Weights are relative and need not sum to 100: `ocean 1 jellyfish 4` is the same
distribution as `ocean 20% jellyfish 80%`. Commas, percent signs, `=` and
newlines are all just separators. A bare name takes weight 1. A name the content
does not define is reported once on stderr rather than silently spawning nothing
forever.

Whether a name is a group or a mob is resolved **at spawn time**, by the
spawner, never here: the map layer has no view of the content registry and must
not grow one. Resolving it at load is what made the old nine hard-coded section
presets impossible to add to.

Naming a mob directly also bypasses the group's exclusions, which is how a
`neverAmbient` mob reaches the world at all.

## Art and staging

The build stages this directory **flat, by bare file name**, into the data
directory beside the binaries (`cpp/CMakeLists.txt`). That flatness is what
makes the references inside the files resolve: a map names its tilesets as
siblings, a tileset names its art under `tiles/`, and the client's sprite cache
looks a tile's art up by bare name. `maps/tileset.tsj` and
`maps/tiles/grass_c_0.svg` both land directly in the data directory.

- The **maps** come out of `maps.json`, never globbed.
- The **tilesets** are read out of the maps at configure time, so a map that
  starts naming a second `.tsj` stages it on the next build.
- The **tile art** and the **ground art** are globbed, because the tileset — not
  a list anyone maintains — decides what exists.

So **adding a tile is two things**: drop a `.svg` in `tiles/`, add a tile to
`tileset.tsj` in Tiled (and tag it `water` if that is what it is). Nothing is
generated and nothing is regenerated. Whether it blocks is decided later, by
which layer it gets painted on.

Art files are 256×256 drawn at the 300-unit grid size (`tilerendersize: grid`),
but the size is for Tiled's benefit alone: `SvgDocument::renderFitted` maps a
viewBox into whatever box it is handed, so the client fits every tile to its
300-unit cell whatever the art's own dimensions say. A file the tileset names
but the directory lacks is one warning in the client, not a failure — the cell
simply does not draw. Outside the map is black void.

The nine `ground/*.svg` are **no longer map data**: a map's own bottom layer is
its ground now. They are still staged because the title screen paints its
backdrop with them, by bare name.

## Layer data format

Save with **CSV or uncompressed** layer data (*Map → Map Properties → Tile Layer
Format*). The reader takes a plain array or uncompressed base64; it deliberately
does not decompress, because adding zlib to the shared library for a map file
would put a decompressor in the wasm build too. A compressed or chunked
(infinite-map) layer is refused by name, with the fix in the message.

## What is not here any more

The **generated-map machinery is gone**. There used to be a tile-art generator
(`scripts/lib/tileArt.js`, `scripts/tileArt/*`), an edge-mask solver that chose
a `wall_edge_<sides>` variant for every cell (`scripts/edgeTiles.js`), a biome
map generator (`scripts/generateBiomeMaps.js`), a converter from the retired
`MapData` literal (`scripts/mapToTiled.js`) and the two palettes those wrote
(`terrain.tsj`, `ground.tsj`). Tiled's Wang brushes do the edge work now, and
the author draws the art, so all of it has been deleted along with the engine's
tile-skin and edge-mask system.

`src/map_bundle.ts` is **frozen where it stands**. It was the TypeScript
server's copy of the map, compiled by `scripts/encodeMap.js` from the one map
that then existed; the new format cannot produce one and there is no
`npm run build:map` any more. The file stays on disk untouched because
`src/map_data.ts` imports it and the frozen TypeScript tree has to keep
typechecking — it is simply never regenerated again. Nothing in the C++ engine
reads it.

`maps_old/` is the author's backup of the 47 generated maps this replaced. It is
untracked, it is not staged, and nothing reads it. Leave it alone.

## What guards this

> **Provisional.** The reader, the renderer and their tests are being rewritten
> in the same change as this document, and the layer collision rule landed after
> the first pass at them. Treat the list below as what *should* guard the format
> rather than as a roll call of tests that exist today; check it against
> `cpp/tests/` before trusting a name in it.

What should guard it, and where:

- **`cpp/tests/tiled_map_tests.cpp`** — the reader: the per-cell collision rule
  in each of its interesting cases (a colliding layer's tile over a
  non-colliding one, art on a non-colliding layer blocking nothing, a `water`
  tile on a colliding layer reading as water and the same tile on a
  non-colliding layer reading as ground, a wall drawn over water staying wall,
  an empty cell), a layer with `has_collision` absent defaulting to false, the
  three flip bits composed in Tiled's order, overlapping-tileset and
  wrong-tile-size refusals, a compressed layer refused by name, and the art
  list and per-layer cells of the real `garden.tmj`.
- **`cpp/tests/spawn_tests.cpp`** — the object layers on the shipped map: the
  bands, the regions, the doors and their id fallbacks (name → label slug → map
  id), and `biome`/`defaultMobGroup` defaulting with nothing authored.
- **`cpp/tests/realm_tests.cpp`** — the manifest order fixing realms, and a
  teleporter's cross-realm arrival.
- **`cpp/tests/art_cache_tests.cpp`** — the tile art cache: a missing file
  warns once and draws nothing.
- The **content hash** over `maps.json` and every map's bytes, which is what
  refuses a client holding a different map than the server.

And the real thing, which is the check that matters: `flowrix_server` boots on
the staged map with no `[map]`, `[spawn]` or `[tiled]` line on stderr, and the
native client draws it.
