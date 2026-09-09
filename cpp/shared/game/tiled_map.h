#pragma once
// The world map, as Tiled writes it.
//
// `maps/world.tmj` is the map. Tiled (mapeditor.org) is what edits it, and this
// is what reads it: a `.tmj` is JSON, and the two things the game wants out of
// one are a grid of tile ids and a list of annotation rectangles.
//
// Two conventions carry the whole file, and both are checked on load rather
// than assumed:
//
//   - ONE TILED PIXEL IS ONE WORLD UNIT. The map's tile size is kTileSize, so
//     an object's x/y/width/height in the file is already a world rectangle and
//     nothing is scaled here. A map saved with a different tile size is
//     REFUSED, because the alternative is every rectangle silently landing
//     somewhere else.
//
//   - THE TILESET IS THE TILE PALETTE. Each tile in `terrain.tsj` carries the
//     game's tile id as a custom property, so the mapping from Tiled's global
//     tile ids to Tile is read out of the map rather than assumed to be the
//     identity. That is what lets the palette gain a tile without every gid in
//     the file shifting underneath the grid.
//
//   - THE BACKGROUND LAYER IS THE GROUND. What the ground is painted with used
//     to be sectionAt(): which third of the map you stood in. The map says it
//     per cell now, in the `background` layer, over a second tileset whose
//     tiles carry a `groundId` where the terrain tileset's carry a `tileId`.
//     A tile painted into the wrong layer is therefore a gid that layer cannot
//     resolve, and is reported rather than silently becoming a wall.
//
// The annotations come back out as a Json array in the SAME shape the old
// TypeScript bundle's MAP_ELEMENTS had, so MapData has one element parser
// rather than two that must agree.

#include <cstdint>
#include <string>
#include <vector>

#include "shared/core/json.h"

namespace flix {

/// One entry of the map's tile palette, as the tileset declares it.
///
/// `solid` and `water` are carried for completeness and for tools; the engine
/// itself reads the flags off Tile via tileBlocks(), because collision runs per
/// tile per tick and cannot afford a lookup. A tileset that disagrees with
/// constants.h about what blocks is a map bug, and mismatchedFlags() below is
/// how a caller finds out.
struct TiledTileType {
    int id = 0;
    std::string name;
    bool solid = false;
    bool water = false;
};

/// One entry of the ground palette: an artwork the background layer can paint
/// a cell with. `art` is the file name the renderer loads, which is also what
/// the section grid used to hard-code.
struct TiledGroundType {
    int id = 0;
    std::string name;
    std::string art;
};

class TiledMap {
public:
    /// Reads a `.tmj`. External tilesets are resolved relative to the map file,
    /// as Tiled itself resolves them.
    bool load(const std::string& path, std::string& errorOut);

    int width() const { return width_; }
    int height() const { return height_; }

    /// Row-major game tile ids, width()*height() of them.
    const std::vector<std::uint8_t>& tiles() const { return tiles_; }

    /// Row-major ground ids, one per cell, or EMPTY when the map has no
    /// background layer. -1 in a cell means bare void: no ground at all.
    ///
    /// Empty and all-void are different answers, and the renderer needs both:
    /// a map without the layer falls back to the section grid, a map with a
    /// void cell paints nothing there.
    const std::vector<std::int8_t>& background() const { return background_; }

    const std::vector<TiledGroundType>& groundPalette() const { return groundPalette_; }

    /// The annotations, in the bundle's element shape. See the header note.
    const Json& elements() const { return elements_; }

    const std::vector<TiledTileType>& palette() const { return palette_; }

    /// Palette entries whose solid/water flags disagree with what constants.h
    /// says the same Tile does. Empty on a healthy map.
    std::vector<std::string> mismatchedFlags() const;

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint8_t> tiles_;
    std::vector<std::int8_t> background_;
    Json elements_;
    std::vector<TiledTileType> palette_;
    std::vector<TiledGroundType> groundPalette_;
};

/// The world map inside a staged data directory.
///
/// Prefers `world.tmj`; falls back to `map_bundle.ts` when the Tiled map is not
/// there. The fallback is not legacy tolerance for its own sake — the offline
/// build and the test harnesses stage their content in several shapes, and a
/// missing map should read as "this directory has the old bundle", not as a
/// server that will not boot.
std::string worldMapPath(const std::string& dataDir);

/// True when a path names a Tiled map rather than the TypeScript bundle.
bool isTiledMapPath(const std::string& path);

} // namespace flix
