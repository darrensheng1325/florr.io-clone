#pragma once
// How a map is fitted into the minimap's box.
//
// The corner map shows THE WHOLE REALM, not a window onto it. There is one
// map in play at a time and every map says its own size, so the only question
// left is arithmetic: what scale takes a world rectangle into a 200-unit
// square, and where inside that square does it sit.
//
// UNIFORM SCALE, CENTRED. The smaller of the two axis ratios wins, so a map
// that is not square letterboxes -- bars above and below a wide map, either
// side of a tall one -- rather than being stretched into the box. A stretched
// minimap is worse than a small one: a player reads distance and direction off
// it, and neither survives two different scales.
//
// Split out of app.cpp because it is the part of the minimap that can be
// checked without painting anything: the fit is arithmetic over a rectangle
// and the solid walk below is a question about the map, while everything else
// in the path is pixels.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "shared/core/types.h"
#include "shared/game/terrain.h"

namespace flix {

/// The world -> box transform for one map: `scale` is box units per world
/// unit, and `offsetX`/`offsetY` are where the map's own top-left corner
/// lands, measured from the box's top-left. Exactly one of the two offsets is
/// non-zero for a map that letterboxes, and both are zero for a square one.
struct MinimapFit {
    double scale = 1.0;
    double offsetX = 0.0;
    double offsetY = 0.0;

    /// A world point in box-local units. Points outside the map land outside
    /// the box (or in its letterbox bars), which is what the callers clip
    /// against -- this does no clamping of its own, because a dot just off the
    /// edge and a dot far away have to be told apart.
    Vec2 toBox(Vec2 world) const {
        return {offsetX + world.x * scale, offsetY + world.y * scale};
    }
};

/// Fits a world rectangle spanning (0, 0)..(extent) into a `box` square.
///
/// A degenerate extent -- a realm with no map staged, which cannot happen
/// through Terrain but can through a hand-built one -- is treated as a square
/// box's worth, so the caller gets a finite transform rather than an infinity
/// that paints the whole screen.
inline MinimapFit minimapFit(Vec2 extent, double box) {
    MinimapFit fit;
    const double width = extent.x > 0.0 ? extent.x : box;
    const double height = extent.y > 0.0 ? extent.y : box;
    fit.scale = std::min(box / width, box / height);
    fit.offsetX = (box - width * fit.scale) * 0.5;
    fit.offsetY = (box - height * fit.scale) * 0.5;
    return fit;
}

/// One solid for the minimap to fill, in world units.
///
/// `ring` is an authored collision ring -- the store's own points, in the
/// cell-local units of the cell that owns the geometry, so a vertex is
/// `(*ring)[i] + origin`. It is null for the whole-cell fallback, and then the
/// solid is the `kTileSize` square whose top-left corner is `origin`.
struct MinimapSolid {
    const std::vector<Vec2>* ring = nullptr;
    Vec2 origin{0.0, 0.0};
    bool water = false;
};

/// Every solid of a realm's collision, once each, in PAINT ORDER.
///
/// This is the whole of what the minimap knows about the map's shape, kept
/// here rather than in the bake because it is the half that can be checked
/// without painting: what gets drawn, once each, in what order.
///
/// ONCE EACH. A shape drawn past its tile's edge is filed in every cell it
/// reaches (see ShapeGrid), so walking the grid meets it several times. It
/// cannot be deduplicated on `ownCell` alone: a shape authored WHOLLY outside
/// its tile -- Tiled lets an author drag one there, and the loader warns about
/// it rather than refusing it -- reaches no cell with `ownCell` set, and a
/// ring skipped in every cell it is filed in is a wall drawn nowhere. So the
/// key is the ring's identity instead: the points it was handed (the store's
/// own, never a copy) plus the cell that owns it, which `origin` names.
///
/// PAINT ORDER is layer order, bottom first, with water before wall inside one
/// layer -- the order the map paints them in, and the order the queries resolve
/// a point's KIND in (cellLayerAt gives the kind to the TOPMOST ring holding
/// the point). Painting in any other order colours a river black wherever it
/// crosses a wall layer below it. The whole-cell fallback sits at the top of
/// the stack, because the coarse Tile it comes from is itself the fold of the
/// topmost layer that contributed to the cell.
template <typename Fn>
void eachMinimapSolid(const Terrain& terrain, Realm realm, Fn&& emit) {
    const int cols = terrain.tileCols(realm);
    const int rows = terrain.tileRows(realm);
    // layer * 2 + kind, so a stable sort by it is layer order with water
    // ahead of wall inside a layer.
    const auto order = [](int layer, bool water) {
        return static_cast<std::uint16_t>(layer * 2 + (water ? 0 : 1));
    };
    std::vector<std::pair<std::uint16_t, MinimapSolid>> solids;
    std::set<std::tuple<const std::vector<Vec2>*, int, int>> drawn;
    std::vector<Terrain::CellCollisionRing> rings;
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            terrain.collisionRingsAt(tx, ty, realm, rings);
            if (rings.empty()) {
                // No authored geometry, but the coarse grid still calls the
                // cell blocked: a generated map, a grid a test wrote with
                // setTile(), a client with no local map file. There the whole
                // cell IS the collision -- it is exactly what blocked() and
                // resolveWall() fall back to for such a cell -- so the square
                // is what the minimap draws.
                const Tile tile = terrain.atTile(tx, ty, realm);
                if (!tileBlocks(tile)) continue;
                MinimapSolid solid;
                solid.origin = {tx * kTileSize, ty * kTileSize};
                solid.water = tileIsWater(tile);
                solids.emplace_back(order(255, solid.water), solid);
                continue;
            }
            for (const Terrain::CellCollisionRing& ring : rings) {
                if (ring.points == nullptr || ring.points->size() < 3) continue;
                const int ownerX =
                    static_cast<int>(std::lround(ring.origin.x / kTileSize));
                const int ownerY =
                    static_cast<int>(std::lround(ring.origin.y / kTileSize));
                if (!drawn.emplace(ring.points, ownerX, ownerY).second) continue;
                MinimapSolid solid;
                solid.ring = ring.points;
                solid.origin = ring.origin;
                solid.water = ring.water;
                solids.emplace_back(order(ring.layer, ring.water), solid);
            }
        }
    }
    std::stable_sort(solids.begin(), solids.end(),
                     [](const std::pair<std::uint16_t, MinimapSolid>& a,
                        const std::pair<std::uint16_t, MinimapSolid>& b) {
                         return a.first < b.first;
                     });
    for (const std::pair<std::uint16_t, MinimapSolid>& solid : solids) emit(solid.second);
}

} // namespace flix
