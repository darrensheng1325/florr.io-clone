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
//   - A VARIANT IS ITS BASE TILE PLUS A STYLE. The tileset holds, for every
//     skin, a `wall_*` and a `water_*` tile and fifteen `*_edge_<sides>`
//     variants of each, all carrying the same `tileId` as the plain wall or
//     water tile plus a `skin` naming the biome family and, on a variant, an
//     `edges` property naming the sides it shows; and three `floor_<skin>_<v>`
//     tiles that are AIR (tileId 0) with a `variant`. A gid therefore resolves
//     to a PAIR -- the tile id the game collides with and the STYLE byte the
//     renderer draws (constants.h: skin in the high nibble, edge mask or floor
//     variant in the low) -- and styles() is the second grid. scripts/
//     edgeTiles.js is what writes the variants into a map; nothing here
//     recomputes them, so a map authored without them simply has a grid of
//     zero styles.
//
//   - TILESETS MAY NOT OVERLAP. Each tileset owns the gids from its firstgid
//     for its tilecount; a map whose tilesets share a gid is REFUSED, because
//     Tiled resolves such a gid to whichever tileset it likes and the map
//     would draw one thing in the editor and another in the game.
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
#include "shared/game/constants.h"

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
    /// The sides this tile shows an edge on, from its `edges` property
    /// (constants.h's kEdge* bits). Zero for a plain tile and for air.
    std::uint8_t edgeMask = 0;
    /// The biome family, from its `skin` property resolved through
    /// constants.h's kTileSkinNames (sewers, computer, unknown). Zero for the
    /// default family and for a name the engine does not know -- a family
    /// that was removed, say -- which is reported, see warnings().
    std::uint8_t skin = 0;
    /// For an AIR tile only: the floor decoration its `variant` property
    /// names, 0..15. Zero otherwise.
    std::uint8_t variant = 0;
    /// The style byte a cell painted with this tile carries (constants.h).
    std::uint8_t style() const { return makeStyle(skin, id == 0 ? variant : edgeMask); }
};

/// Parses a tileset tile's `edges` property: any combination of the letters
/// n, e, s and w, in any order and with commas or spaces between them or not
/// (`ne`, `n,e`, `n e` are all north and east). Anything else is ignored, so
/// a mistyped property reads as a plain tile rather than refusing the map.
std::uint8_t parseEdgeMask(const std::string& text);

/// The name suffix a mask spells, in the tileset's fixed side order n, e, s, w
/// (`wall_edge_` + edgeMaskSuffix(mask)). Empty for zero.
std::string edgeMaskSuffix(std::uint8_t mask);

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

    /// Row-major style bytes, parallel to tiles(): for each wall or water
    /// cell the skin and the sides it shows an edge on, for each air cell the
    /// skin and floor variant, as the map's variant gids declare them
    /// (constants.h's styleSkin / styleEdgeMask / styleFloorVariant). All
    /// zero for a map that was never run through scripts/edgeTiles.js.
    const std::vector<std::uint8_t>& styles() const { return styles_; }

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

    /// The map's own custom properties, as an object. `displayName` and
    /// `defaultMobGroup` are what the game reads out of it; anything else an
    /// author puts there travels through untouched.
    const Json& properties() const { return properties_; }

    const std::vector<TiledTileType>& palette() const { return palette_; }

    /// Palette entries whose solid/water flags disagree with what constants.h
    /// says the same Tile does. Empty on a healthy map.
    std::vector<std::string> mismatchedFlags() const;

    /// What load() noticed but did not refuse the map for: a `skin` naming a
    /// family the engine has no art table for, say. Also printed to stderr as
    /// they are found; kept here so a tool or a test can read them back.
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint8_t> tiles_;
    std::vector<std::uint8_t> styles_;
    std::vector<std::int8_t> background_;
    Json elements_;
    Json properties_;
    std::vector<TiledTileType> palette_;
    std::vector<TiledGroundType> groundPalette_;
    std::vector<std::string> warnings_;
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
