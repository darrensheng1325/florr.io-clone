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

## Layers

| layer | holds |
| --- | --- |
| `background` | the ground: which artwork each cell is painted with |
| `terrain` | the tile grid — collision. Air is left empty, so the background shows through |
| `spawns` | `spawn` rectangles, with a `spawnType` property |
| `biomes` | `biome` rectangles, with `biomeName`, `backgroundTexture` and `spawnTable` |
| `teleporters` | `teleporter` **points**, with `teleportToX` / `teleportToY` and an optional `serverPort` |

Objects are grouped by kind so each set can be hidden while you work on
another. The game never sees the grouping: every reader filters by kind before
it looks at order, so only the order *within* a layer is observable, and that
is preserved.

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
