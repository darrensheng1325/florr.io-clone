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
//   - COLLISION IS A PROPERTY OF THE LAYER. A tile layer carries a custom
//     boolean `has_collision`, and that one tick box is the entire rule:
//
//         blocked <- ANY layer with has_collision has a tile in that cell
//         kind    <- Water if the TOPMOST blocking tile there is tagged
//                    `water` in the tileset, otherwise Wall
//         else       Ground
//
//     A layer with `has_collision` false, or absent, NEVER blocks, whatever
//     art it holds -- a painted pond on a background layer is scenery a body
//     walks straight through. A layer with it ticked blocks EVERYWHERE it has
//     a tile, and no tile can opt out. So the author decides collision by
//     which layer they paint on, which is the thing they are already choosing,
//     and the tileset never has to agree with the map about it.
//
//     The tileset's `water` tag is the one thing left that a tile says about
//     collision, and it says only what KIND of blocker the cell is -- the
//     minimap colour and tileIsWater(). It never decides whether the cell
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
//     tile serves all four rotations; collision masks them off, since a
//     rotated tile covers the same cell.
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
/// vertical. Any other order draws three of the four rotations wrong.
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
};

/// One cell of one layer: which artwork to draw there, and how to orient it.
///
/// `art` is an index into TiledMap::artFiles(), or -1 for an empty cell --
/// empty is the common case in the upper layers, and it is not the same as a
/// tile with no image. A cell is EMPTY or it is not; a layer that collides
/// blocks at every non-empty cell, including one whose tile has no art.
struct TiledCell {
    std::int16_t art = -1;
    std::uint8_t flags = 0;   ///< kTileFlip* and kTileCoversEverything
};

/// One tile layer: whether it is a wall, its name, and its cells in row-major
/// order.
///
/// Layers are stored in FILE order, which is bottom to top -- the order to
/// draw them in. The name is carried for tooling and the load report and is
/// never what decides anything.
struct TiledLayer {
    std::string name;
    /// The layer's `has_collision` property. False when the property is
    /// absent, so a layer nobody ticked is scenery.
    bool collides = false;
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

    /// The COLLISION grid: row-major Tile values, width()*height() of them,
    /// derived from the layers by the rule in the header note. Only ever
    /// Tile::Ground, Tile::Wall or Tile::Water.
    const std::vector<std::uint8_t>& tiles() const { return tiles_; }

    /// How many cells the rule resolved to each Tile. Sums to width()*height();
    /// what the load report prints.
    int wallCells() const { return wallCells_; }
    int waterCells() const { return waterCells_; }
    int groundCells() const { return groundCells_; }

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
};

} // namespace flix
