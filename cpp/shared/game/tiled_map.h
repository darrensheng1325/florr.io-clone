#pragma once
// A map, as Tiled writes it.
//
// `maps/*.tmj` are the maps. Tiled (mapeditor.org) is what edits them, and this
// is what reads one: a `.tmj` is JSON, and the three things the game wants out
// of one are the ART the author painted, the COLLISION that art implies, and a
// list of annotation rectangles.
//
// A few conventions carry the whole file, and each is checked on load rather
// than assumed:
//
//   - ONE TILED PIXEL IS ONE WORLD UNIT. The map's tile size is kTileSize, so
//     an object's x/y/width/height in the file is already a world rectangle and
//     nothing is scaled here. A map saved with a different tile size is
//     REFUSED, because the alternative is every rectangle silently landing
//     somewhere else.
//
//   - A LAYER DECIDES WHETHER A CELL COLLIDES; THE TILE DECIDES THE SHAPE.
//     A tile layer carries a custom boolean `has_collision`, and the tileset
//     carries, per tile, the shapes the author drew in Tiled's Tile Collision
//     Editor. Together:
//
//         shapes  <- for each layer with has_collision, the SHAPES of the tile
//                    it has in that cell, placed at the cell, turned by the
//                    cell's flip bits and scaled from the tileset's tile size
//                    to the map's
//         blocked <- the cell holds at least one such shape, or a neighbouring
//                    cell's shape reaches into it (shapeReach)
//         kind    <- Water if the TOPMOST contributing tile there is tagged
//                    `water` in the tileset, otherwise Wall
//         else       Ground
//
//     A layer with `has_collision` false, or absent, NEVER blocks, whatever
//     art it holds -- a painted pond on a background layer is scenery a body
//     walks straight through. A layer with it ticked contributes the shapes of
//     every tile it paints, and A TILE WITH NO SHAPES CONTRIBUTES NOTHING,
//     even there: that is Tiled's own semantic, and it is what lets an author
//     paint a walkable footpath tile onto the dirt layer. It is also the one
//     authoring mistake this rule can hide, so the number of cells it happens
//     in is counted (unshapedBlockingCells()) and the load report prints it.
//
//     So the author decides WHICH cells can collide by the layer they paint
//     on, and WHERE inside those cells by the shapes they drew on the tile.
//
//   - A LAYER MAY ALSO *REMOVE* COLLISION. A tile layer carrying the boolean
//     `negate_collision` cancels, where it has a tile, the collision the
//     layers BELOW it contributed. That is what a bridge is: a deck painted
//     over a river, on which the flower walks on planks and not in water.
//
//         cancelled over the negating tile's OWN shapes when it has any,
//         and over the WHOLE CELL when it has none
//
//     THAT ASYMMETRY WITH THE BLOCKING RULE IS DELIBERATE. An unshaped tile
//     on a COLLIDING layer is nearly always a mistake -- it looks solid and is
//     not -- so it contributes nothing and is counted as a warning. An
//     unshaped tile on a NEGATING layer is the ordinary way to say "this whole
//     square is decked over", because a deck covers its cell; asking every
//     bridge tile in the tileset to carry a 256-square rectangle nobody would
//     ever draw differently would be ceremony, not safety.
//
//     BELOW IT, NOT EVERYWHERE. The layers are a stack: a wall layer added
//     ABOVE a bridge still blocks on it. The shipped garden's `bridge` layer
//     happens to be the topmost one, so today it cancels water, dirt and
//     castle alike, but the rule is written as the stack it is.
//
//     A negated cell is GROUND: not Wall, not Water, so inWater() is false on
//     a bridge deck. `has_collision` and `negate_collision` are independent
//     properties and a layer carrying BOTH is incoherent -- the load report
//     says so loudly and negation wins for that layer's cells.
//
//     WHOLE-CELL NEGATION IS THE SUPPORTED CASE, and tileDecksWholeCell()
//     below is what decides whether a deck tile is one: no shapes at all, or
//     shapes that cover the cell (Tiled's "whole tile" rectangle). Either of
//     those is resolved away when the map loads, so the coarse grid, the
//     shapes, the minimap, the wire and every query agree without any of them
//     knowing negation exists. A deck tile whose shapes cover only PART of its
//     cell cannot be: cancelling half a cell means subtracting one authored
//     ring from another, which nothing here does. Such a tile still cancels
//     the POINT tests over its own shape, the coarse grid keeps the cell
//     blocked, and the load report warns about it by name -- see
//     Terrain::ShapeGrid::Ref::negates for exactly what it does and does not
//     reach.
//
//     The Tile grid below is the COARSE view of that: one value per cell,
//     saying only whether the cell holds any blocking shape and what kind. It
//     is what the minimap paints, what the bots' flow field walks, and what
//     goes on the wire; the shapes are the exact answer, and Terrain keeps
//     them beside the grid.
//
//     The tileset's `water` tag says only what KIND of blocker a shape is --
//     the minimap colour and tileIsWater(). It never decides whether anything
//     blocks. Only three Tile values ever come out of here (Ground, Wall,
//     Water).
//
//   - THE LAYERS ARE DRAWN BOTTOM TO TOP, IN FILE ORDER. Their NAMES mean
//     nothing to the game; only their order and their `has_collision` do.
//     There is no layer that "is" the terrain, and a map may have as many as
//     the author wants.
//
//   - TILED CHOOSES THE EDGE ART, NOT US. The author paints with Tiled's Wang
//     (terrain) brushes, so which corner or edge tile lands in a cell is
//     already decided in the file, flip bits and all. Every cell carries its
//     tile's three flip bits through untouched (kTileFlip*) because one edge
//     tile serves all four rotations -- and collision turns its shapes by the
//     same bits, so a rotated edge blocks along the edge the art draws.
//
//   - TILESETS MAY NOT OVERLAP. Each tileset owns the gids from its firstgid
//     for its tilecount; a map whose tilesets share a gid is REFUSED, because
//     Tiled resolves such a gid to whichever tileset it likes and the map
//     would draw one thing in the editor and another in the game.
//
// The annotations come back out as a Json array of elements, which is the one
// shape MapData parses.

#include <cstdint>
#include <string>
#include <vector>

#include "shared/core/json.h"
#include "shared/game/constants.h"

namespace flix {

/// Tiled's flip bits, as a cell carries them.
///
/// Tiled packs them into the top three bits of a gid; they are moved down to
/// these three so a cell is a small struct rather than a raw gid nobody but
/// this file can resolve. The transform they name is applied in Tiled's own
/// order: the ANTI-DIAGONAL flip first (transpose), then horizontal, then
/// vertical. Any other order draws three of the four rotations wrong -- and,
/// since the collision shapes are turned by the same transform, collides three
/// of them somewhere other than where the art is. tileOrientation() below is
/// the one place that order is written down.
inline constexpr std::uint8_t kTileFlipHorizontal = 1;
inline constexpr std::uint8_t kTileFlipVertical = 2;
inline constexpr std::uint8_t kTileFlipDiagonal = 4;

/// Set on a cell whose tile declares `covers_everything`: the art fills its
/// whole 300-unit square opaquely, so nothing painted under it can show
/// through and the renderer may stop there instead of drawing the layers
/// below. A drawing hint, carried per cell so reading it costs no lookup;
/// nothing about the game depends on it.
inline constexpr std::uint8_t kTileCoversEverything = 8;

/// The layer property that makes a layer a wall.
///
/// Named here because it is the map format's one collision knob, and an author
/// who mistypes it gets a layer that silently stops blocking. See the load
/// report in Terrain::loadTiledMap, which prints what it resolved to.
inline constexpr const char* kLayerCollisionProperty = "has_collision";

/// The layer property that makes a layer a bridge: it REMOVES the collision
/// the layers below it contributed, over its own tiles' shapes or, for a tile
/// with no shapes, over the whole cell. See the header note.
///
/// Named here for the same reason as its opposite: a mistyped property is a
/// bridge you cannot walk on, and the load report prints what it resolved to.
inline constexpr const char* kLayerNegateProperty = "negate_collision";

/// One map cell's artwork orientation: the rotation to apply about the cell's
/// centre, and whether the result is then mirrored across its own vertical
/// axis. A canvas composes translate * rotate * scale, so this pair IS the
/// matrix R(radians) * diag(mirror ? -1 : 1, 1).
struct TileOrientation {
    double radians = 0;
    bool mirror = false;
};

/// The orientation Tiled's three flip bits name, for a cell's `flags` (the
/// kTileFlip* bits; anything above them is ignored).
///
/// Tiled defines the ORDER these compose in: the ANTI-DIAGONAL flip first -- a
/// transpose, (u,v) -> (v,u) -- then horizontal, then vertical. So the matrix
/// a cell wants is V^v * H^h * D^d, and the eight of them are the eight
/// symmetries of the square. Applying them in any other order draws three of
/// the four rotations mirrored, and an edge tile that serves all four
/// rotations of one corner is exactly where that shows.
///
/// THE ONE PLACE that transform is derived. The renderer turns a cell's ART by
/// it and the map reader turns that tile's COLLISION SHAPES by it (through
/// orientInTile below), so the two cannot drift: a wrong row here would put a
/// wall exactly where it is drawn, which is at least honest.
TileOrientation tileOrientation(std::uint8_t flags);

/// Where a point inside an UNFLIPPED cell ends up once the cell's flip bits
/// are applied. `local` and the result are both in the cell's own space, y
/// down, (0,0) at its top-left corner and `side` units across.
///
/// Written on top of tileOrientation() rather than as its own eight-way table,
/// because the whole point is that the shapes land where the art does: this is
/// the renderer's matrix (translate to the centre, rotate, mirror x) applied to
/// a point instead of to a bitmap. The angles are all multiples of a quarter
/// turn, so the sine and cosine are snapped to the integers they are within a
/// rounding error of and the corners of a shape land exactly on the corners of
/// the cell.
Vec2 orientInTile(Vec2 local, std::uint8_t flags, double side = kTileSize);

/// One collision shape a tile carries: a closed ring of at least three points,
/// in CELL-LOCAL WORLD units -- (0,0) at the cell's top-left corner, kTileSize
/// across, y down -- with the cell's flip bits ALREADY applied.
///
/// Tiled draws these in the Tile Collision Editor, in the tileset's own tile
/// space (256 square here), and writes them as an `objectgroup` on the tile.
/// The reader scales that space onto the map's cell (300 square here, per axis)
/// and turns the result, so what comes out needs nothing but the cell's origin
/// added to it. A rectangle arrives as its four corners and an ellipse as a
/// polygonised ring, so everything downstream has one shape kind to handle.
///
/// The ring is wound so that its signed area is POSITIVE, which is what makes
/// (dy, -dx) of an edge point out of the shape. A mirroring flip reverses a
/// ring, so the winding is re-normalised after the transform, not before.
struct TiledShape {
    std::vector<Vec2> points;
};

/// A tile's shapes AS ONE CELL HOLDS THEM: each ring turned by the cell's flip
/// bits and wound positive again.
///
/// The one place a cell's collision geometry is produced, so the server and the
/// client cannot build it differently. Called once per (tile, orientation) the
/// map actually paints -- 86 times for the shipped garden -- and never per
/// query; see Terrain's shape store.
std::vector<TiledShape> orientTileShapes(const std::vector<TiledShape>& shapes,
                                         std::uint8_t flags);

/// The box a tile's shapes span once turned by `flags`, in cell-local world
/// units. A zero-size box for a tile with no shapes.
Rect orientedShapeBounds(const std::vector<TiledShape>& shapes, std::uint8_t flags);

/// Does this tile, turned by `flags`, cover its WHOLE cell?
///
/// The question a `negate_collision` layer asks of every tile it paints, and
/// the only two answers a deck can have that are resolvable at load (see the
/// header note):
///
///   - NO SHAPES AT ALL is a whole-cell deck. That is the asymmetry with the
///     blocking rule: an unshaped tile on a colliding layer contributes
///     nothing, an unshaped tile on a negating layer decks its square.
///   - SHAPES THAT COVER THE CELL are the same thing said explicitly -- the
///     rectangle Tiled draws when an author picks "whole tile" in the Tile
///     Collision Editor, in any of the eight orientations. Folded into the
///     whole-cell case so the two spellings of one deck behave identically.
///
/// Anything else is false: a plank across a corner covers part of its cell,
/// and no amount of bookkeeping here can subtract it from the ring below.
///
/// Covering is tested per ring, not as a union: a single ring that IS its own
/// bounding box (which is what a rectangle arrives as) and whose box contains
/// the cell. Two half-cell rectangles that happen to tile the cell between
/// them read as partial, which is the safe direction -- it warns rather than
/// silently opening a cell.
bool tileDecksWholeCell(const std::vector<TiledShape>& shapes, std::uint8_t flags);

/// WHICH CELLS a shape box touches, as offsets from the cell that owns it.
///
/// All zero for a shape drawn inside its tile, which is nearly every shape
/// there will ever be -- but Tiled lets an author drag a shape past the tile's
/// edge, and a rotation can sweep one clean into the next cell, so a shape is
/// not necessarily confined to the cell it is painted in. Both the coarse grid
/// and Terrain's shape store fan a shape out over exactly this range, so the
/// two views agree about which cells hold geometry and no query has to widen
/// itself "just in case".
///
/// A box whose far edge lands EXACTLY on a cell boundary stops at the cell
/// before it: the edge touches the next cell with zero area and the owning
/// cell already answers for the boundary line itself. Without that, every
/// full-cell shape on the map would claim its right and bottom neighbours.
struct ShapeReach {
    int dxMin = 0;
    int dyMin = 0;
    int dxMax = 0;
    int dyMax = 0;
    /// True when the shape stays in the cell that owns it.
    bool ownCellOnly() const { return dxMin == 0 && dyMin == 0 && dxMax == 0 && dyMax == 0; }
};
ShapeReach shapeReach(const Rect& bounds);

/// One entry of the map's tile palette, as the tileset declares it.
///
/// One per gid the map's tilesets span, in gid order, so `palette()[i]` is the
/// tile with gid `palette()[i].gid` -- a tileset that names no properties for
/// a tile still has an entry, it is simply empty.
struct TiledTileType {
    /// The GLOBAL tile id, i.e. what a map cell stores. Unique across the
    /// map's tilesets, which is what the overlap check guarantees.
    int gid = 0;
    /// What to call this tile in a message: its art's file name, or its Tiled
    /// class, or its gid. Never empty.
    std::string name;
    /// The tile's `image`, BASENAME ONLY -- the data directory is staged flat,
    /// and this is the name the renderer's sprite cache looks art up by.
    /// Empty for a tile with no image of its own.
    std::string art;
    /// Index into TiledMap::artFiles(), or -1 for a tile with no image.
    int artIndex = -1;
    /// What KIND of blocker this tile is where a colliding layer paints it --
    /// Tile::Water rather than Tile::Wall. Never decides WHETHER a cell
    /// blocks: on a layer that does not collide this tag does nothing at all.
    bool water = false;
    bool coversEverything = false;  ///< see kTileCoversEverything
    /// The shapes the author drew on this tile, UNFLIPPED, already scaled from
    /// the tileset's tile space into one cell of world units. Empty for a tile
    /// nobody drew a shape on, which then blocks nothing anywhere -- see the
    /// header note.
    std::vector<TiledShape> shapes;
};

/// One cell of one layer: which artwork to draw there, and how to orient it.
///
/// `art` is an index into TiledMap::artFiles(), or -1 for an empty cell --
/// empty is the common case in the upper layers, and it is not the same as a
/// tile with no image. A cell is EMPTY or it is not; a layer that collides
/// contributes the shapes of every non-empty cell's tile, which for a tile
/// carrying no shapes is nothing at all.
struct TiledCell {
    std::int16_t art = -1;
    std::uint8_t flags = 0;   ///< kTileFlip* and kTileCoversEverything
    /// Index into TiledMap::palette(), or -1 for an empty cell. What a reader
    /// building collision looks the cell's SHAPES up through; `art` cannot
    /// serve, because several tiles may share one artwork and a tile with no
    /// image has no art index at all.
    std::int32_t type = -1;
};

/// One tile layer: whether it is a wall, its name, and its cells in row-major
/// order.
///
/// Layers are stored in FILE order, which is bottom to top -- the order to
/// draw them in. The name is carried for tooling and the load report and is
/// never what decides anything.
struct TiledLayer {
    std::string name;
    /// The layer's `has_collision` property, AS IT TOOK EFFECT. False when the
    /// property is absent, so a layer nobody ticked is scenery -- and false on
    /// a layer that also negates, because negation wins (see `conflicting`).
    bool collides = false;
    /// The layer's `negate_collision` property: where this layer has a tile,
    /// the collision from the layers below it is cancelled. See the header.
    bool negates = false;
    /// The file ticked BOTH properties on this layer. They are opposites, so
    /// `collides` was forced false and `negates` kept; carried so the load
    /// report can name the layer rather than silently picking one.
    bool conflicting = false;
    /// How many of this layer's cells hold a tile at all.
    int paintedCells = 0;
    /// For a negating layer: how many of its painted cells it ACTUALLY took
    /// collision away from -- cells where this layer decks the whole cell
    /// (tileDecksWholeCell) and there was something below it to cancel.
    ///
    /// Counted from what was cancelled, never from what was there: a cell that
    /// was already open is not "cleared", and neither is one whose deck tile
    /// covers only part of it (those are `partialDeckCells`). The load report
    /// prints this number, and an author reads it to find out whether ticking
    /// the box did anything -- so it must never say yes when the answer is no.
    int clearedCells = 0;
    /// For a negating layer: how many of its painted cells hold a tile whose
    /// own shapes cover only PART of the cell. The half-supported case -- it
    /// cancels the point tests over its shape and nothing else -- so the load
    /// report warns with this count rather than folding it into `clearedCells`.
    int partialDeckCells = 0;
    std::vector<TiledCell> cells;
};

class TiledMap {
public:
    /// Reads a `.tmj`. External tilesets are resolved relative to the map file,
    /// as Tiled itself resolves them.
    bool load(const std::string& path, std::string& errorOut);

    int width() const { return width_; }
    int height() const { return height_; }

    /// Every distinct artwork the map's tiles name, in palette order, by bare
    /// file name. A TiledCell's `art` indexes into this.
    const std::vector<std::string>& artFiles() const { return artFiles_; }

    /// The tile layers, bottom to top. Each holds width()*height() cells.
    const std::vector<TiledLayer>& layers() const { return layers_; }

    /// The COARSE collision grid: row-major Tile values, width()*height() of
    /// them, derived from the layers and their tiles' shapes by the rule in the
    /// header note -- a cell is non-Ground exactly when some colliding layer
    /// paints a tile there that HAS shapes. Only ever Tile::Ground, Tile::Wall
    /// or Tile::Water.
    ///
    /// The exact answer is the shapes themselves (palette()[cell.type].shapes,
    /// turned by the cell's flags); this is the one-value-per-cell view of it
    /// that the minimap, the flow field and the wire use.
    const std::vector<std::uint8_t>& tiles() const { return tiles_; }

    /// How many cells the rule resolved to each Tile. Sums to width()*height();
    /// what the load report prints.
    int wallCells() const { return wallCells_; }
    int waterCells() const { return waterCells_; }
    int groundCells() const { return groundCells_; }

    /// Cells where a COLLIDING layer paints a tile that carries no collision
    /// shape, and which therefore block nothing.
    ///
    /// Zero on a finished map. It is not an error -- an author may well paint a
    /// walkable footpath onto the dirt layer on purpose -- but it is the only
    /// way a map can look solid in the editor and be walkable in the game, so
    /// the count and the tiles it happened with are reported at load.
    int unshapedBlockingCells() const { return unshapedBlockingCells_; }
    std::vector<std::string> unshapedBlockingTiles() const;

    /// Cells a `negate_collision` layer cleared out of the COARSE grid: cells
    /// that some layer below had blocked and that are Ground because a layer
    /// above decked them over. The 14 bridge cells of the shipped garden.
    ///
    /// Only whole-cell negation can show up here -- a negating tile that
    /// carries its own shapes cancels part of a cell, which one Tile per cell
    /// cannot express, so the coarse view keeps such a cell blocked. That is
    /// the coarse grid being conservative, which is what it is for; the exact
    /// shape store answers the partial case exactly.
    int negatedCells() const { return negatedCells_; }

    /// How many cells hold at least one collision shape. The coarse grid's
    /// wall + water count, by another route, and what tells a reader whether a
    /// map has authored shapes at all.
    int shapedBlockingCells() const { return wallCells_ + waterCells_; }

    /// The annotations, as an array of elements. See the header note.
    const Json& elements() const { return elements_; }

    /// The map's own custom properties, as an object. `displayName`, `biome`
    /// and `defaultMobGroup` are what the game reads out of it; anything else
    /// an author puts there travels through untouched.
    const Json& properties() const { return properties_; }

    const std::vector<TiledTileType>& palette() const { return palette_; }

    /// Water-tagged tiles THIS MAP paints, but only on layers that do not
    /// collide -- so not one cell of them will ever be Tile::Water.
    ///
    /// The one honest complaint left about the `water` tag. Since a layer
    /// decides blocking, a tile can no longer contradict itself; what it can
    /// still do is sit on the wrong layer, which looks like a river in the
    /// editor and is walkable grass in the game. A water tile the map never
    /// paints is not reported: a tileset is shared between maps, and a map is
    /// not wrong for leaving part of it unused.
    std::vector<std::string> strandedWaterTiles() const;

private:
    int width_ = 0;
    int height_ = 0;
    int wallCells_ = 0;
    int waterCells_ = 0;
    int groundCells_ = 0;
    std::vector<std::string> artFiles_;
    std::vector<TiledLayer> layers_;
    std::vector<std::uint8_t> tiles_;
    Json elements_;
    Json properties_;
    std::vector<TiledTileType> palette_;
    /// Per palette entry: painted by some colliding layer / by some
    /// non-colliding layer. Only strandedWaterTiles() reads them.
    std::vector<std::uint8_t> paintedOnBlocker_;
    std::vector<std::uint8_t> paintedOnScenery_;
    int unshapedBlockingCells_ = 0;
    int negatedCells_ = 0;
};

} // namespace flix
