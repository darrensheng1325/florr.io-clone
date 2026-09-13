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

**One Tiled pixel is one world unit.** The tile size is 256×256, which is
`kTileSize` in `cpp/shared/game/constants.h`, and it is the size the tile art is
drawn at, so a tile lands on its cell 1:1. Because they match, an object's
`x`/`y`/`width`/`height` in the file is already a world rectangle and nothing is
scaled on the way in. A map saved at a different tile size is refused rather
than guessed at — there is no scale factor that is right for the grid *and* the
objects. An infinite or non-orthogonal map is refused for the same reason.

**A layer's name is a note to the author; its `has_collision` is not.**
`background`, `water`, `dirt`, `castle` mean nothing to the game, there is no
layer that "is" the terrain, and nothing reads a layer by name. The one thing
the game does read off a layer is a custom boolean, `has_collision`, and that
boolean says whether the layer collides at all — see below. There is a second,
`negate_collision`, which says the layer *removes* collision; that is what the
bridge is. Otherwise the layers are drawn bottom to top and that is all they do.

### Flips

Tiled packs three flip bits into the top of a gid — horizontal `0x80000000`,
vertical `0x40000000`, anti-diagonal `0x20000000` — because one edge tile serves
all four rotations of itself. The reader carries them through per cell
(`kTileFlipHorizontal` / `Vertical` / `Diagonal`) and the renderer applies them
in **Tiled's order: the anti-diagonal flip first (transpose), then horizontal,
then vertical**. Any other order draws three of the four rotations wrong.

Collision goes through **the same transform, from the same function**
(`flix::tileOrientation`, in `cpp/shared/game/tiled_map.h`): a tile's collision
shapes are turned and mirrored exactly as its art is, so a rotated edge blocks
where the rotated picture draws. Deriving that transform twice is how collision
and art drift apart, so there is only one of it.

## Collision: the layer says *whether*, the tile says *where*

Two things decide collision, and they answer different questions.

**A layer's `has_collision` says whether this layer collides at all.** A tile
layer may carry one custom boolean in Tiled, under *Layer → Custom Properties*:

| property | meaning |
| --- | --- |
| `has_collision` | tiles painted on this layer block movement, in the shapes they draw |
| `negate_collision` | tiles painted on this layer *cancel* the collision the layers **below** contributed — see below |

They are opposites. A layer carrying **both** says nothing coherent: negation
wins for that layer's cells, the layer blocks nothing of its own, and the load
report names the layer rather than quietly picking one.

**A tile's collision shapes say where inside its cell it blocks.** Those are the
shapes you draw in Tiled's *Tile Collision Editor* (select the tile in the
tileset, then *View → Tile Collision Editor*), and they are what the game
actually collides against:

```
blocked(point) <- the point is inside a collision SHAPE of the tile painted
                  on some layer with has_collision = true, placed at that
                  cell, turned by the cell's flip bits and scaled from the
                  tile's own image size onto the map's cell size
kind           <- the TOPMOST shape containing the point belongs to a tile
                  tagged `water` ? water : wall
else              ground
```

So a cell is **not** solid corner to corner just because something is painted on
it. `castle_l` draws its wall body across the left 130.5 of its 256-unit tile
and its collision rectangle is exactly that wide, so on a 256-unit cell the wall
face lands 130.5 units in — and a flower walks right up to the edge of the
drawn stone instead of stopping half a cell short of it. An edge tile has a
walkable rim; a diagonal tile has a diagonal you can slide along.

**A tile with no collision shapes blocks nothing, even on a colliding layer.**
That is Tiled's own semantic, and it is what makes the rule above usable: the
scenery tiles carry no shapes and so can sit on any layer at all. It also means
a structural tile nobody drew a shape for is a hole in a wall, so the loader
counts those cells and names the tiles on stderr at start-up. On `garden.tmj`
that count is zero.

Shape kinds Tiled can write are all read: a rectangle (with `rotation`), a
polygon (concave included — the authored dirt edges are), and an ellipse, which
is polygonised. A **polyline is an open shape and is not collision**: it is
skipped, with a warning naming the tile.

A layer with `has_collision` absent or false **never** blocks, whatever art it
holds, and whatever shapes its tiles carry; a map that sets it nowhere has no
walls at all.

### `negate_collision`: a layer that takes collision away

Tick `negate_collision` on a tile layer (again *Layer → Custom Properties*) and,
**where that layer has a tile**, the collision the layers *below* it contributed
is cancelled. That is what a bridge is: a deck painted over a river, which the
flower crosses on planks instead of swimming.

```
negated(cell) <- a layer with negate_collision = true has a tile here, and
                 that tile decks the cell:
                     no collision shapes at all   -> the WHOLE cell
                     shapes that cover the cell   -> the whole cell (same
                                                     thing, drawn out)
                     shapes covering only part    -> only that shape, and
                                                     only for point tests
                                                     (see the warning below)
kind          <- a negated cell is GROUND. Not wall, not water: a flower on a
                 bridge over a river is on planks, and `inWater()` is false.
```

**No shapes means the whole cell here — the opposite of what it means on a
colliding layer.** That asymmetry is deliberate, and it is the one thing about
this property worth remembering:

| the tile has no collision shapes | on `has_collision` | on `negate_collision` |
| --- | --- | --- |
| what it does | nothing at all | decks the whole cell |
| why | a wall-looking tile that does not block is nearly always a mistake, so the loader refuses to guess and counts the cells | a deck covers its square; that is what a deck *is*, and asking every bridge tile in the tileset to carry a hand-drawn 256-square rectangle would be ceremony, not safety |

**It reaches down, not everywhere.** The layers are a stack. A `negate_collision`
layer cancels what is *below* it and nothing above, so a wall layer added over a
bridge still blocks on it. `garden.tmj`'s `bridge` happens to be the topmost
layer, so today it cancels `water`, `dirt` and `castle` alike — but move it down
the layer panel and it will only cancel what it now sits over. **A deck placed
*under* the thing it meant to cancel does nothing at all**, which is the most
common way to get this wrong; the load report warns about exactly that.

**It reaches everything.** Negation is resolved once, when the map loads — the
cancelled shapes are never filed in the first place — so the coarse grid, the
shapes, the minimap, the bots' flow field, spawn placement, the wire and every
query agree without any of them knowing negation exists. A bridge reads as a
walkable channel straight across the river on the minimap, and the water either
side of it still blocks.

**A deck tile that carries its own shapes.** Drawing Tiled's whole-tile
rectangle on the bridge tile is fine — it means the same whole-cell deck and is
treated identically. A shape covering only *part* of the cell is the one case
this cannot resolve: cancelling half a cell means subtracting one authored ring
from another, which the loader does not do. Such a tile cancels only the point
tests over its own shape; the coarse grid keeps the cell blocked, so the
minimap, the flow field and the swept tests still see the blocker and a body may
not be able to stand on the plank at all. The loader warns, with the count, and
the fix is one click: give the deck tile a whole-tile shape, or no shape.

**The worked example.** `garden.tmj`'s `bridge` layer is 14 cells at row
`y = 123`, `x = 83…96`, painted with `bridge_c_0`, `bridge_r_0` and
`bridge_c_1`. None of those three tiles carries a collision shape, and under
every one of them is the `water` layer with `water_tl_0` / `water_l_0` /
`water_tri_0` / `water_c_0`, each of which does. So the river blocks everywhere
except those 14 cells, where the deck cancels it: a flower walks the run end to
end, is dry the whole way (`inWater()` is false on the deck), and is stopped by
the water one cell north and one cell south of it.

The load report says what it resolved to — the negating layers by name, and how
many of each layer's painted cells it actually opened:

```
[map] data/garden.tmj: collision from water, dirt, castle; negated by bridge
(14 of 14 cells cleared); scenery background; 6495 wall, 1136 water, 8753
ground cells; 93 shape sets over 7631 shaped cells, 14 cells decked over
```

`14 of 14` is the number to read after ticking the box. It counts cells where
something was **cancelled**, never cells that merely held something, so a deck
over dry land reads `0 of 14` and earns a warning:

```
[map] WARNING data/garden.tmj: layer "bridge" has "negate_collision" set but
cancels no collision in any of its 14 painted cells; negation only reaches the
layers BELOW it, so check it is not under what it means to cancel
```

A deck tile with a part-cell shape is counted apart (`0 of 1 cells cleared, 1
partial`) and warned about in its own line, because it is the half-supported
case above.

### The coarse grid, which is still per cell

Beside the exact shapes the engine keeps the old one-value-per-cell grid, now
meaning *"some shape in this cell blocks"*. It is what the bots' flow field
walks, what spawn placement rejects conservatively, the fast reject before any
shape test, and the only collision that goes over the wire. It over-states walls
— a cell with a sliver of wall in it reads as solid — which is the safe
direction for all three of those. Anything that asks about a POINT (`blocked`,
`inWater`, the push-out, the segment tests, line of sight) goes to the shapes
and is exact.

The **minimap is not one of them**. It draws the rings themselves
(`Terrain::collisionRingsAt`), because the coarse grid's over-statement is not
safe in a picture: a third of this map's solid cells are only partly solid, and
a corridor that runs between two of them is painted shut — the minimap would
show a wall where the player can walk, and hide the way through. It falls back
to filling a cell's square only where there are no shapes to draw, which is the
same cell-wide solid `blocked()` itself falls back to there.

A client whose data directory has no map for the realm it is in falls back to
whole-cell collision from that wire grid. It believes in more wall than there
is, never less, which is the harmless way to be wrong.

**Why the split.** *Whether* is a layer property because a layer is a thing the
author can see, name and toggle, and paints a whole region of wall in one go.
Whether used to be a `solid` boolean on each tile, and a Wang brush is exactly
the thing that defeats that: a brush paints a family's centre, edge and corner
tiles interchangeably, so one corner tile nobody remembered to tag was a hole in
a wall that no amount of repainting would close, and finding it meant walking
into it. A layer has one switch and the author has already decided which layer
the walls go on.

*Where* is a tile property because it is a property of the drawing: the same
`castle_l` blocks the same shape wherever it is painted, and the shape is
already sitting in the tileset next to the picture it belongs to. The two
questions do not fail the same way either — forgetting the layer switch makes a
whole region walkable and is obvious the moment you walk it, while a missing
tile shape is one tile's worth of hole, which is why the loader counts those and
names them.

The cost used to be that **stacking did not subtract**: a tile painted on top of
a colliding cell added its own shapes and could never take the cell's existing
ones away, so opening a hole through a colliding layer meant erasing the cell on
that layer with the brush's eraser. That is still how you delete a wall — and it
is still the right tool when what you want is *no wall there*. What a layer
**can** now do is cancel the wall while keeping the art: `negate_collision`,
next.

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

The difference is visible rather than physical — the minimap paints a water
shape its own colour, and `tileIsWater()` is what anything asking "is this thing
in the drink" reads — because water already blocked before any of this. Tagging a
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

Everything else is untagged, which costs nothing: an untagged tile whose shapes
block is a wall like any other. Note that `water` is asked about the tile whose
SHAPE contains the point, so a bridge drawn over a pond reads as a bridge where
its own shape covers — and a bridge on a `negate_collision` layer is not water
at all, because there is nothing left in that cell to be water.

`garden.tmj` puts its layers to work as `background` (no collision) under
`water`, `dirt` and `castle` (all three colliding), with `bridge` on top of them
negating. Roughly half its cells hold something that blocks — but because
collision is the authored shapes rather than the cells, rather less of the map
is actually solid than the cell count suggests, and a good share of the cells
the coarse grid calls wall have walkable ground inside them. That difference is
the edge tiles, and it is what a player feels as walking along a wall rather
than along a staircase. Read the counts off your own start-up line rather than
off this one, which is only what the map happened to say the day it was written:

```
[map] data/garden.tmj: collision from water, dirt, castle; negated by bridge
(14 of 14 cells cleared); scenery background; 6495 wall, 1136 water, 8753
ground cells; 93 shape sets over 7631 shaped cells, 14 cells decked over
```

A second line appears when a collision shape reaches outside the tile it was
drawn on — which the shipped tileset does, by 0.39 of a world unit, from one
polygon vertex at x = −0.333 on `water_tl_0` and `sewage_tl_0`:

```
[map] data/garden.tmj: a collision shape reaches 0.391 world units outside its
own tile; it still blocks, in every cell it reaches, but check it was meant
```

That is legal — a shape is filed in every cell it touches, so it collides there
and the coarse grid marks those cells too — but it is nearly always a slip of
the mouse in the Tile Collision Editor, and nothing in Tiled shows it, so it is
reported.

### `covers_everything`

A tile with `covers_everything` fills its whole 256-unit square opaquely, so
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
| `difficulty` | **how dangerous** this ground is, a number from 0 up. Makes this a **band**. |
| `mobs` | the **distribution**: what actually appears here |

- A shape with `difficulty` is a **band**. It owns a population of its own,
  stocked to a density scaled by the outline's area. **Bands are the only thing
  that spawns anything.** This is where the map's difficulty progression lives.
- A shape with only `mobs` is a **region**: it says what grows on this ground
  and owns nothing. It spawns nothing by itself — a band standing on it that
  named no `mobs` asks it *what* to grow, and that is all it does.

> **If there is no band, there are no mobs.** Ground no band covers grows
> nothing, ever, and a map with no band on it is empty. That is the rule, not a
> bug: the author draws where the mobs are. A region drawn over unbanded ground
> is still empty ground — it answers a question nothing is asking. The map's
> load line says so out loud when it happens:
>
> ```
> [map] sketch: 64x64 tiles, biome "sketch", mobs "sketch", NO SPAWN BANDS --
> no mobs will spawn on this map, 1 region, 3 art files, 2 layers, doors: main
> ```
>
> **What still appears regardless**, because none of it is ground being filled:
>
> - A **child of a mob a band placed** — a nest's escorts and waves, a
>   centipede's body segments. They are laid out on a ring around their parent,
>   and that ring legitimately reaches over the band's edge onto ground nothing
>   fills. They are not spawned *on* unbanded ground, they are spawned *by*
>   something standing inside a band, so they are deliberately not gated.
> - **Pets, admin spawn commands and the drop/loot system**, none of which ask
>   the ground anything.
> - **The PVP arena and the daily maze**, the two realms that are not authored
>   maps at all: they are generated, they carry no object layer to draw a band
>   on, and `cpp/server/systems/mode_spawning.h` populates each of them whole on
>   its own terms. "No band, no mobs" is a rule about map ground, and those two
>   are deliberately exempt from it.

A band with `difficulty: 0` is still a band — it owns its population and grows
commons. It is the *presence* of the property that makes it one, not its value.

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

#### `difficulty` — which number is which rarity

One open-ended number, and it buys a *mixture* of two adjacent tiers rather than
one tier outright. That is why it replaced a band naming a tier (`spawnType:
rare`): a tier name can only ever say one of ten things, and it cannot say
"mostly ultras with the odd super in them" at all.

The number is read through four anchors — 0 is fully common, 100 is ultra with a
two-per-cent chance of super, 200 is fully super, 300 is unique with a
five-per-cent chance of apex — and it is linear between them. Past 300 it keeps
the last segment's slope toward apex rather than capping, so a bigger number
always means at least as dangerous.

**Whole tiers.** These difficulties spawn one rarity and nothing else:

| difficulty | rarity |
| ---: | --- |
| 0 | common |
| 16.6 | uncommon |
| 33.2 | rare |
| 49.8 | epic |
| 66.4 | legendary |
| 83.1 | mythic |
| 99.7 | ultra |
| 200 | super |
| 295.2 | unique |
| 390.5 | apex |

**Everything else is a blend**, including three of the four anchors themselves:

| difficulty | spawns |
| ---: | --- |
| 10 | 40% common, 60% uncommon |
| 25 | 50% uncommon, 50% rare |
| 50 | 99% epic, 1% legendary |
| 75 | 49% legendary, 51% mythic |
| **100** | **98% ultra, 2% super** |
| 150 | 49% ultra, 51% super |
| **200** | **100% super** |
| 250 | 47.5% super, 52.5% unique |
| **300** | **95% unique, 5% apex** |
| 350 | 42.5% unique, 57.5% apex |
| 390.5 and up | 100% apex |

The ladder is deliberately not evenly spaced: the first seven rarities fit in
the first hundred points and the last three take the next three hundred. The
early climb is short and the top of it is long, which is what the four anchors
say.

Luck is the only thing that moves a band off these numbers, and it only moves it
up — every point of a player's luck above neutral adds a hundredth of a tier, so
a clover buys a percentage point of the tier above wherever its owner is
standing. There is no downward drift: a difficulty-0 band is fully common for
everyone, always.

The curve lives in exactly one place, `cpp/shared/game/difficulty.h`. Its anchor
table is the design statement; move it and every number in this section moves
with it, including the threshold that keeps a door off dangerous ground. That
threshold is **tier 1 — difficulty 16.6**, the top of pure uncommon: the last
difficulty whose blend cannot contain a rare at all. Not tier 2, which is where
the ground is *entirely* rare and which would have called a band that rolls rare
98.7% of the time (difficulty 33) safe for a level-one flower. Derived from the
curve rather than written down twice.

**What the shipped map says today.** `garden.tmj` carries a handful of bands,
from 0 on the ground the `garden` door stands on up to the hardest of them, each
naming the map's own `garden` roster. Everything outside them is empty ground.
The author moves and renumbers them as the map is balanced, so the count below
is whatever the file said the day this was written; the start-up line is the
thing to read:

```
[map] garden: 128x128 tiles, biome "garden", mobs "garden", 4 bands difficulty 0
(common)..40 (rare), 0 regions, 77 art files, 4 layers, doors: garden
```

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
| `defaultMobGroup` | the mob group a band with no `mobs` of its own, and no region under it, spawns from | **`biome`** |

Both defaults exist so that a one-biome map does not have to say its own name
three times. `garden.tmj` declares none of them and is therefore the map
`garden`, in biome `garden`, growing `garden` mobs.

There is deliberately **no map-wide difficulty**. There used to be a
`defaultDifficulty` property, for "how dangerous the ground no band covers is",
and it was read by exactly one thing: the per-viewer density fill that stocked
that ground. That fill is gone, so no mob is ever rolled against such a number
and the property configured nothing. Difficulty belongs to a band.

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

Art files are 256×256, which is the grid size (`tilerendersize: grid`), but
matching it is a convenience rather than a rule: `SvgDocument::renderFitted`
maps a viewBox into whatever box it is handed, so the client fits every tile to
its 256-unit cell whatever the art's own dimensions say. **Collision** is fitted the
same way and from the same number — each tile's shapes are read in **that tile's
own image size** and scaled onto the cell — so a tile of any size may be added
without touching anything that already exists. That matters because `tileset.tsj`
is an image collection (`"columns": 0`), and for one of those Tiled rewrites the
tileset-level `tilewidth`/`tileheight` to the size of the *largest* image in it:
reading shapes in that space would silently rescale every shape in the game the
first time a bigger tile arrived. The tileset's own size is used only for a
spritesheet tileset, whose tiles have no image of their own. A file the tileset names
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

**Zone rarities are gone**, and with them the machinery that hung off them: a
band naming `spawnType: rare`, the per-section "natural" rarity spread the
density fill used to roll, the one-tier drift that nudged every spawn up or down
on a die roll, the one-in-a-hundred super an ultra band used to produce, and the
boss pass — the pass that kept exactly one ultra alive in the world and one
super per section and placed them by hand. That pass cannot coexist with a scale
on which a difficulty-100 band is *full* of ultras, so on this scale bosses come
from the ground they stand on. What survives is the announcement: a super,
unique or apex spawning is still worth telling the server about.

## What guards this

Every name below is a test in `cpp/tests` that exists and passes today, with
what it pins written after it. The list was checked against the files rather
than remembered, so a name that has drifted is a bug in one of the two. The
only exception is a name introduced as a *former* name — "(it replaced …)",
"(formerly …)" — which is deliberately a test that no longer exists, kept so
that someone searching for the old behaviour finds where it went.

**The collision rule — `cpp/tests/tiled_map_tests.cpp`** (the reader: which
shapes a cell ends up with)

- `collision_is_the_layers_and_the_topmost_blocker_names_the_kind` — a
  colliding layer's tile over a non-colliding one, and which of the stack names
  wall or water.
- `a_layer_that_does_not_collide_never_blocks_whatever_it_holds` — the same
  castle-and-water painting on `has_collision: false` and on a layer with no
  such property, blocking nothing either way, and still drawing.
- `an_empty_cell_on_a_colliding_layer_is_still_ground`.
- `a_tile_with_no_collision_shape_blocks_nothing_even_on_a_colliding_layer` —
  and the authoring warning counts such a cell **once**, not once per layer.
- `every_shape_kind_tiled_can_write_arrives_except_the_open_one` — rectangle,
  rotated rectangle, polygon, ellipse and rotated ellipse all arrive; the
  polyline is skipped with the tile named.
- `a_shape_is_read_in_its_own_tiles_image_not_the_tilesets_display_grid` —
  the image-collection case above: a 256 tile and a 512 tile in one tileset
  both fill their cell, and a spritesheet tile with no image of its own falls
  back to the tileset's size.
- `a_tiles_shapes_are_scaled_from_the_tilesets_tile_size_onto_the_cell` — the
  scale onto the 256-unit cell. (Its name predates the fix above and now says
  the wrong space; what it checks is the scaling, and it is correct.)
- `flip_bits_reach_the_art_and_turn_the_collision_shapes` and
  `all_eight_orientations_put_a_shape_where_the_art_is` — the three bits
  composed in Tiled's order, on the art and on the shapes, from the one
  `tileOrientation`.
- `the_shipped_map_loads` and
  `the_shipped_map_collides_with_authored_shapes_everywhere` — the real
  `garden.tmj`: its art list, its per-layer cells, and that every
  cell it marks blocked got there from a shape.
- `water_art_painted_only_where_it_cannot_block_is_reported`,
  `the_derived_grid_reaches_a_client_through_terrain_and_the_wire`,
  `a_map_with_no_object_layers_loads`, `the_object_layers_become_elements`.
- `a_map_the_engine_cannot_read_is_refused_with_a_reason` — two tilesets
  fighting over a gid, a compressed layer, a tile size that is not the game's,
  a gid no tileset defines, and a layer of the wrong length.

**The geometry — `cpp/tests/terrain_tests.cpp`** (what the shapes then do)

- `a_rect_shape_blocks_inside_itself_and_leaves_the_rest_of_the_cell_walkable`
  and `a_concave_shape_blocks_its_arms_and_not_its_notch` — the notch stays
  open, which whole-cell collision could not express.
- `an_unshaped_tile_on_a_colliding_layer_blocks_nothing_in_terrain` — the same
  rule as the reader's, asked of `Terrain` rather than of the file.
- `all_eight_orientations_block_where_the_art_is`.
- `a_circle_stops_on_the_diagonal_edge_a_shape_draws_not_on_the_cell_boundary`
  and `a_body_resting_against_a_wall_stops_at_the_flat_face` — the push-out
  against a turned polygon and against a flat one.
- `a_body_wedged_between_two_shapes_is_reported_unresolved_not_relocated` — the
  four-pass contract: a centre the passes cannot untangle is reported, never
  moved somewhere it did not earn.
- `the_segment_tests_agree_with_the_point_tests_along_the_same_line`.
- `water_is_the_shape_it_is_drawn_as_and_the_topmost_layer_names_the_kind`.
- `a_shape_that_leaves_its_tile_blocks_and_is_reported_in_every_cell_it_reaches`
  — the overhang line above: a shape is filed in every cell it touches and the
  coarse grid marks those cells, so the coarse view stays conservative.
- `the_shipped_garden_stops_a_body_where_its_art_does` — the real map, with
  every expected number derived from the file rather than written down.
- `a_realm_keeps_its_shapes_only_while_they_still_describe_its_grid` and
  `writing_a_tile_by_hand_drops_the_realms_authored_shapes` — shapes and grid
  never describe two different maps.

**Negation — `cpp/tests/terrain_tests.cpp`** (`negate_collision`: the bridge)

- `an_unshaped_negating_tile_clears_its_whole_cell` — the ordinary deck: the
  coarse grid, the point tests, the rings, the swept test and the push-out all
  say open.
- `a_deck_tile_that_covers_its_cell_is_the_same_deck_as_an_unshaped_one` — the
  two spellings of one deck, resolved to the same thing everywhere, including
  `nearestOpenTile` and the report's counts.
- `a_body_walks_onto_a_deck_whose_tile_carries_a_whole_cell_shape` — the same
  fixture through the real movement step, because a cell the queries call open
  that no body can reach is not a bridge.
- `a_shaped_negating_tile_clears_only_its_own_shape` and
  `a_partial_deck_that_overlaps_no_collision_is_reported_as_clearing_nothing` —
  the half-supported part-cell deck, and that the report counts what was
  **cancelled** rather than what was merely there.
- `a_negating_layer_does_not_cancel_a_colliding_layer_above_it` — negation
  reaches DOWN. The shipped bridge is the topmost layer, so only a fixture can
  say this.
- `a_negated_water_cell_is_ground_and_is_not_water` — `inWater()` is false on
  the planks.
- `the_coarse_grid_and_the_exact_queries_agree_on_a_negated_cell` — one answer
  for the minimap, the flow field, spawn placement, the wire and the shapes.
- `a_layer_with_both_collision_properties_is_reported_and_negates`.
- `a_negating_layer_over_open_ground_cancels_nothing_and_says_so` — the numbers
  the "cancels no collision" warning is printed from.
- `the_shipped_bridge_is_a_walkable_channel_and_the_river_still_blocks` — the
  real `garden.tmj`, with the run derived from the file: every decked cell open
  and dry, a clear segment end to end, the rows either side still blocking in
  every column, and the minimap drawing no solid on the deck.

**The client — `cpp/tests/client_collision_tests.cpp`**

- `a_client_collides_against_the_same_shapes_the_server_enforces`.
- `a_client_with_no_local_map_collides_with_whole_cells` — the conservative
  fallback.
- `a_realm_change_rebuilds_the_clients_collision_shapes`.

**The object layers — `cpp/tests/spawn_tests.cpp`** (read off the shipped map,
derived from the file rather than pinned, because the author is still drawing)

- `the_shipped_catalogue_loads_and_resolves_its_defaults` (formerly
  `the_shipped_map_loads_and_resolves_its_defaults`, which pinned a catalogue of
  exactly one map) — **every** map `maps/maps.json` names loads, and each
  reports the banded branch of the load line quoted above: a band count and a
  difficulty range, never `NO SPAWN BANDS`. Neither the number of maps nor the
  number of bands is pinned, because the author adds both.
- `every_shipped_door_is_named_by_its_label_and_is_offered` (formerly
  `the_shipped_door_is_named_by_its_label_and_is_pickable`) — the name → label
  slug → map id fallback asked of every door in the catalogue, each resolving to
  a real map, with the pickable ones offered on the join list; the catalogue is
  required to be non-empty and self-consistent rather than a fixed size.
  `the_shipped_door_stands_on_open_ground`.
- `an_authored_band_and_region_still_parse`,
  `a_zone_outline_excludes_what_its_bounding_box_includes`,
  `a_zone_boundary_counts_as_inside`, `a_zone_area_is_the_outlines_not_the_boxs`,
  `a_spawn_in_a_polygon_zone_lands_inside_it` — the polygon rules above.
- `a_distribution_parses_the_authored_syntax`,
  `a_distribution_accepts_the_shapes_an_author_will_type`,
  `a_broken_distribution_is_reported_not_guessed_at`.
- `the_live_server_grows_what_the_ground_under_each_mob_declares` — the same
  rule asked of a real `GameServer` on the real map, with a real client joined
  through the real door: every mob it grew is judged against the hardest band
  on the map it stands on, and a mob clear of every band may only be a borrowed
  body segment or a nest's escort.
- `every_pickable_door_stands_on_safe_open_ground`,
  `a_door_that_is_not_pickable_is_joined_only_by_an_admin`,
  `a_teleporter_carries_a_player_to_another_map`,
  `a_spawn_choice_the_maps_do_not_define_falls_back`.

**`difficulty` — `cpp/tests/spawning_tests.cpp`** (the curve, and what reads it)

- `the_difficulty_curve_hits_the_four_authored_anchors` — 0, 100, 200 and 300
  give exactly the four sentences at the head of this section.
- `difficulty_clamps_below_zero_and_ramps_past_three_hundred` — below 0 is
  common; above 300 keeps climbing and stops at apex.
- `a_higher_difficulty_never_spawns_a_lower_tier` — the curve is monotonic all
  the way up.
- `luck_shifts_the_curve_upward_and_never_down`.
- `safe_ground_is_ground_that_cannot_roll_a_rare` — the door threshold, pinned
  from both sides.
- `a_band_is_the_only_ground_that_grows_anything` — commons inside a
  difficulty-0 band, and *nothing at all* on the map beside it. (It replaced
  `a_bands_difficulty_beats_the_maps_default`, whose second half asserted that
  the ground around the band grew the map's `defaultDifficulty`.)
- `a_map_with_no_band_at_all_grows_nothing` — a map with a default group and a
  region over the whole of it, and still no mobs. It also pins the load line
  above word for word, through `MapData::bandSummary()`, because that line is
  the only warning the author gets.
- `a_harness_with_no_map_at_all_grows_nothing` — no maps means no bands means
  no mobs, stated on its own so that a harness which places its own mobs cannot
  quietly start passing for the wrong reason.
- `every_ambient_mob_on_the_shipped_map_stands_inside_a_band` — the invariant
  itself, driven over `garden.tmj` with the real spawn pass: every live mob is
  inside a band, or is the escort or body segment of one that is. There is no
  third case.
- `a_band_converges_to_the_population_its_own_area_buys` and
  `a_band_stocks_its_own_outline_and_never_the_open_ground_beside_it` — a
  band's target is its outline's area times `kTargetMobDensity`, and every mob
  it places lands inside that outline. (The two used to be
  `population_converges_to_the_target_near_a_player` and
  `ambient_mobs_spawn_inside_the_buffered_viewport`, which measured the deleted
  per-viewer fill; they now measure the band.)
- `the_shipped_map_never_spawns_above_the_difficulty_its_ground_declares` — no
  band exceeds the tier its own difficulty buys.
- `the_region_under_a_spawn_decides_its_group` and
  `the_shipped_map_grows_its_own_biomes_roster` (formerly
  `the_shipped_map_stocks_its_default_group`) — the band → region →
  `defaultMobGroup` fallback, which `spawn_tests.cpp`'s
  `the_shipped_map_falls_back_to_its_own_mob_group_wherever_nothing_says_otherwise`
  checks again off the real map.
- `a_hard_band_is_where_every_boss_in_the_world_comes_from`,
  `a_soft_band_announces_nothing`,
  `an_announced_boss_carries_the_map_it_spawned_on` — what replaced the boss
  pass, and the announcement that outlived it.
- `min_rarity_still_floors_a_named_mob_whatever_the_ground_says` and
  `a_neverambient_mob_never_comes_from_a_group_roll_however_hard_the_ground` —
  the two mob properties a zone's number does not override.

**The handshake — `cpp/tests/config_tests.cpp`**

- `the_content_hash_covers_the_staged_maps` — `maps.json`, every map's bytes
  **and every tileset's**, which is where the shapes and the `water` tag live.
  Drawing one shape on one tile changes the hash, so a client with a stale
  tileset is refused rather than left predicting against different geometry.

**Realms — `cpp/tests/realm_tests.cpp`** — `each_realm_draws_its_own_ground`
and `a_respawn_into_another_realm_sends_the_client_that_realms_map`: a map is
its own coordinate space and its own grid.

Nothing today guards the client's *missing tile art* warning
(`sprites.cpp`'s "tile …: unreadable, drawing nothing"); `art_cache_tests.cpp`
covers the SVG cache itself and not that path.

And the real thing, which is the check that matters: `flowrix_server` boots on
the staged map and prints its three `[map]` lines — the collision summary, the
overhang note and the load report — with **no `[spawn]` or `[tiled]` line at
all** on stderr, and the native client draws the map and stops where the art
says it should. A `[spawn]` line on a shipped boot means the map lost its bands.
Measured on the map as it stands, which the author is still redrawing, so read
the boot lines rather than these numbers:

```
[map] data/garden.tmj: collision from water, dirt, castle; scenery background;
6443 wall, 1150 water, 8791 ground cells; 93 shape sets over 7593 shaped cells
[map] data/garden.tmj: a collision shape reaches 0.391 world units outside its
own tile; it still blocks, in every cell it reaches, but check it was meant
[map] garden: 128x128 tiles, biome "garden", mobs "garden", 4 bands difficulty 0
(common)..40 (rare), 0 regions, 77 art files, 4 layers, doors: garden
```

and a client joining through that door meets a world every mob of which was
born inside one of those four bands, or is the brood of a hole that was. A
thirteen-second boot with one client joined counted 229 live mobs: 229 born in a
band, 0 anywhere else. (Three of them had since wandered off their band's edge,
which is why the check is against the spot a mob was *placed* — `MobAi::anchor`,
never rewritten — and not against where it is standing.)
