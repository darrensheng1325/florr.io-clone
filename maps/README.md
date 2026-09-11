# The world map

The map lives here, in [Tiled](https://www.mapeditor.org)'s format. Open
`world.tmj` in Tiled and edit it; there is no other map source.

```
maps/
  world.tmj      the map: the ground, the tile grid and every annotation
  terrain.tsj    the tile palette, one tile per game tile id
  tiles/*.svg    what each terrain tile is drawn as
  ground.tsj     the ground palette, one tile per ground artwork
  ground/*.svg   the ground artwork itself
```

## Three rules the format rests on

**One Tiled pixel is one world unit.** The map's tile size is 300×300, which is
`WALL_TILE_SIZE` in `src/constants.ts` and `kTileSize` in
`cpp/shared/game/constants.h`. Because they match, an object's `x`/`y`/`width`/
`height` in the file is already a world rectangle and nothing is scaled on the
way in. Both readers refuse a map saved at a different tile size rather than
guess a scale factor — there isn't one that is right for the grid *and* the
objects.

**The tileset is the palette.** Each tile in `terrain.tsj` carries the game's
tile id as a `tileId` property, so Tiled's global tile ids are translated rather
than assumed to line up. Adding a tile type means adding a tile to the tileset.
Its other properties are the ones `TileTypeConfig` has:

| property | meaning |
| --- | --- |
| `tileId` | the id stored in the grid — 0 air, 1 wall, 2 water, 3+ custom |
| `solid` / `water` | what the tile does. `constants.h` is the authority; a tileset that disagrees is reported at load, because a tile drawn walkable and collided with as wall is a lie to whoever is editing |
| `style` | `flat`, `wall` or `water` — how the browser client paints it |
| `color`, `borderColor` | the fill the renderer uses when there is no texture |
| `textureSvg` | the tile art this tile is really painted with, as a path. **Present only for tiles the game textures**; `tiles/air.svg`, `tiles/water.svg` and `tiles/block.svg` are palette art for the editor and are not map data |
| `builtin` | ids 0–2, which `constants.ts` registers itself and the bundle must not redefine |

Tile art is 300×300 so a tile fills exactly one cell in Tiled. The game never
reads those attributes — `graphics/map-drawing.ts` stretches whatever it is
handed to one tile, and `SvgDocument::renderFitted` maps the viewBox into
whatever box it is given — so the size is for the editor's benefit alone.

Palette art is exactly what the renderer draws and nothing more. `air` is an
empty image because air is where the ground shows through; `water` and `block`
are the flat fills `world_renderer.cpp`'s `kTileColor()` paints. Water's jagged
shoreline is *not* in the art: the renderer draws it as a separate pass over
every exposed edge, from an outline generated out of the tile's own coordinates
(`jaggedEdge()` in `terrain.h`), so it belongs to a pair of tiles rather than to
one and no single palette image could be honest about it.

**The background layer is the ground.** It used to be `sectionAt()`: which
third of the map you stood in decided what the ground was painted with, and the
nine artworks were nailed to a 3×3 grid nobody could move. The `background`
layer says it per cell, over `ground.tsj` — the same nine artworks, now a
palette rather than a geography, so a map can put a patch of desert inside the
garden.

Ground tiles carry `groundId` where terrain tiles carry `tileId`. That is what
keeps the two layers apart: a ground tile painted into the terrain layer is a
gid the terrain reader cannot resolve, and it says so instead of silently
becoming a wall.

The renderer still tiles ground artwork every 400 units, as it always has, and
samples the background layer once per artwork tile at that tile's centre — the
layer chooses the art, it does not chop it up. That is also why the seams still
land exactly where they did: a ground tile's centre is at least 200 units from
a section boundary, and the 300-unit cell containing it has its own centre
within 150 units, so the two never end up on opposite sides.

## Tile skins

`terrain.tsj` holds four families of wall / water art. Ids 0–35 are the
**default** family: the six builtin tiles (air, wall, water, bridge, sewage,
block) and the default wall's and water's 15 edge variants each. After that come
three biome **skins**, 35 ids apiece — a wall base, its 15 edge variants, a
water base, its 15 edge variants, and three floor decorations:

| skin | name | ids | classes |
| --- | --- | --- | --- |
| 0 | default | 0–35 | `wall`, `wall_edge_<sides>`, `water`, `water_edge_<sides>` |
| 1 | sewers | 36–70 | `wall_sewers[_edge_<sides>]`, `water_sewers[_edge_<sides>]`, `floor_sewers_<0-2>` |
| 2 | computer | 71–105 | `wall_computer[_edge_<sides>]`, … |
| 3 | unknown | 106–140 | `wall_unknown[_edge_<sides>]`, … |

`tilecount` is 141, so `ground.tsj` sits at firstgid 142 in every map. An edge
variant's `<sides>` lists its exposed sides in the fixed order n, e, s, w
(`wall_edge_ne`); `scripts/edgeTiles.js --apply` picks the variant for every
cell, so a map is authored with the base tiles and the lips follow.

A skinned tile is still plain air, wall or water to the game (`tileId` 0–2);
the skin and edge mask travel as a per-cell style byte the C++ side reads off
the tileset (`skin`, `edges`, `variant` properties). A wall or water cell of
the default family takes its look from the ground beneath it: on sewers,
computer or unknown ground it is drawn with that skin, on any other ground it is
the default brown wall / flat water (`skinForGround()` in `constants.h`). That
is how the overworld's three skinned thirds get their walls without the map
naming a skin, and why the garden, desert, hel, ocean, ant_hell and jungle
sublevels are painted with the default `wall` and `water` and carry no floor
decorations.

The art is generated, not drawn by hand. `scripts/lib/tileArt.js` owns the
geometry every family shares (lip and shoreline profiles, corner joins, the
tileset entry, the id layout) and `scripts/tileArt/<skin>.js` owns one family's
look. To add a family:

1. append its name to `SKIN_NAMES` and its colours to `SKIN_PALETTE` in
   `scripts/lib/tileArt.js` — the next 35 ids are its block;
2. write `scripts/tileArt/<name>.js` (copy `sewers.js`; it must export `name`,
   `wall`, `water` and exactly three `floors` stamps);
3. add the name to `kTileSkinNames` in `cpp/shared/game/constants.h` at the same
   index, and map its ground to it in `skinForGround()`;
4. if the generator should paint sublevels with it, add the biome to `SKINNED`
   in `scripts/generateBiomeMaps.js` and give it a `skin` and `floors`;
5. `node scripts/edgeTiles.js --art` (writes `tiles/*.svg` and `terrain.tsj`),
   then `npm run build:map` — the ground tileset's firstgid moves with
   `tilecount`, and every map is remapped for you.

`node scripts/edgeTiles.js --art --skin <name> --out <dir>` previews one family
without touching the tileset. The renderer draws a wall or water cell whose skin
has no art file with the default family's art for the same mask, so a partly
drawn family never leaves holes.

## Layers

| layer | holds |
| --- | --- |
| `background` | the ground: which artwork each cell is painted with |
| `terrain` | the tile grid — collision. Air is left empty, so the background shows through |
| `spawns` | `spawn` **polygons**, with `spawnType` and an optional `mobs` distribution |
| `biomes` | `biome` rectangles, with `biomeName`, `backgroundTexture` and `spawnTable` |
| `teleporters` | `teleporter` **points**, with `teleportToX` / `teleportToY` and an optional `serverPort` |

Objects are grouped by kind so each set can be hidden while you work on
another. The game never sees the grouping: every reader filters by kind before
it looks at order, so only the order *within* a layer is observable, and that
is preserved.

### What a spawn zone spawns

A zone says two independent things. `spawnType` is the **tier** — `common`
through `ultra` — and is still where the map's difficulty progression lives.
`mobs` is the **distribution**: what actually appears there, as weighted rows of
mob ids and presets.

```
mobs = garden 50% hornet 50%
mobs = ocean 20% jellyfish 80%
mobs = hornet
```

A row is a name and a weight. The name is a **preset** when it matches one of
the nine mob-spawn sections — `garden`, `desert`, `hel`, `ocean`, `ant_hell`,
`jungle`, `sewers`, `computer`, `unknown` — and a **mob id** otherwise. A preset
defers to that section's own ambient table, weights and all, so `ocean` in a
garden zone spawns exactly what the ocean would. A named mob is taken directly,
which also bypasses the ambient table's exclusions: naming a `neverAmbient` mob
is one of the two ways one reaches the world at all.

Weights are relative, so they need not sum to 100 — `ocean 1 jellyfish 4` is the
same zone as `ocean 20% jellyfish 80%`. Commas, percent signs and `=` are all
just separators. A bare name takes weight 1, so `hornet` alone is a zone of
nothing but hornets. A name the content does not define is reported once on
stderr rather than silently spawning nothing forever.

**Omitting `mobs` is the default and means what it always meant**: roll the
ambient table of whichever section the mob lands in. Every zone on the shipped
map still does that — six of them straddle two sections, where "the section the
mob landed in" is not a constant, so the default is deliberately not written out
as a preset.

### Spawn zones are polygons

A mob tier band is an outline, not a box. Draw one with Tiled's polygon tool
(or drag a vertex onto an existing zone) and it can follow a coastline or a
canyon; a rectangle over the same ground either spills mobs onto the next
tier's territory or leaves a wedge of its own permanently empty.

Everything the spawner does in bulk still works on the zone's **bounding box** —
which sections it touches, whether it is near anyone's viewport, whether it is
worth looking at — and only three questions go to the outline: is this point in
this zone, how large is it, and where inside it should this mob go. That split
is why the change is cheap: the box is a superset of the outline, so every
broadphase stayed exactly as it was.

- **Placement** samples the bounding box and throws away what falls outside the
  outline. A zone covering a small fraction of its box just spends more of its
  attempts; it does not spawn in the corners.
- **Population** is scaled by the outline's area, so a diagonal band is not
  packed at twice the density of a rectangular zone beside it.
- **The boundary is inside.** The rectangles these replaced were tested
  inclusively on every edge, so a mob standing exactly on a border was in that
  zone, and it still is.

Biomes and teleporters are unchanged — a rectangle and a point. A spawn zone
saved as a plain rectangle object still loads and stays a rectangle; the
converter writes the four corners instead so the shape is one you can add a
vertex to without converting it first.

One caveat worth knowing: `src/constants.ts` declares the `polygon` field so
`map_bundle.ts` typechecks, but nothing in `src/` reads it. The unmaintained
TypeScript server therefore still treats every zone as its bounding box.

A biome's spawn table is a list of records, which Tiled has no property type
for, so it travels as a JSON string in the `spawnTable` property — Tiled edits
it in its multi-line string editor:

```json
[ {"tier": "legendary", "weight": 5, "mobType": "hornet"} ]
```

A row's `mobType` is the only way a mob the ambient roll refuses ever reaches
the world. Dropping it turns a dummy biome into ordinary garden ground.

## After editing

```
npm run build:map      # maps/world.tmj -> src/map_bundle.ts
```

The C++ client and server read `maps/world.tmj` directly (see
`cpp/shared/game/tiled_map.h`); the bundle exists for the TypeScript server,
which wants the map as an importable module rather than a file to open. Both
come from this one file, so nothing is authored twice — but the bundle is a
build output, so regenerate it before you ship.

`cpp/tests/tiled_map_tests.cpp` compares the two readers tile for tile and
rectangle for rectangle, and checks that the background layer still answers
exactly what `sectionAt()` used to at every point the renderer samples. If any
of that ever drifts, that is what says so.

The bundle carries no background layer — there is nowhere in it to put one — so
a data directory holding only `map_bundle.ts` falls back to the section grid and
draws the world it always did.

## Layer data format

Save with **CSV or uncompressed** layer data (Tiled: *Map → Map Properties →
Tile Layer Format*). The C++ reader takes a plain array or uncompressed base64;
it deliberately does not decompress, because adding zlib to the shared library
for a map file would put a decompressor in the wasm build too.

## Converting an older map

`scripts/mapToTiled.js` turns the retired `MapData` literal — what
`MapEditor.html` still exports, and what `src/map_source.ts` used to hold —
into the files here, and verifies the round trip before it exits:

```
node scripts/mapToTiled.js path/to/map.json
```
