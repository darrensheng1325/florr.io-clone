#pragma once
// Compiled artwork: mobs, petals, and the ground the world is tiled with.
//
// Every sprite in the game is an inline SVG document inside mobs.json /
// petals.json. Those are parsed ONCE at startup into retained SvgDocuments and
// drawn straight to the canvas thereafter -- there is no bitmap bake. Baking
// mobs to bitmaps was tried in the original and cost more than it saved: a
// rarity-scaled mob needs a bitmap per size, the cache thrashes as soon as a
// crowd is on screen, and the vector path is fast enough.
//
// The biome ground and the textured map tiles are the same idea one step out:
// a document per map section, tiled across the world by the renderer. The wall
// and water tiles are a table of them: per SKIN (constants.h's kTileSkinNames:
// the plain family, then sewers, computer and unknown) a base wall, a base
// water and fifteen edge variants of each, plus three floor decorations for
// every biome skin --
// the same files Tiled shows in the palette (maps/tiles/), so what the author
// sees is what is drawn. The table is filled once at build() and read by
// index thereafter: no frame ever builds a file name.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "canvas.h"
#include "svg.h"

#include "shared/game/constants.h"
#include "shared/game/rarity.h"

namespace flix {

class ContentRegistry;

class SpriteCache {
public:
    /// Compiles every mob and petal image in `content`, and loads the biome
    /// ground artwork out of `dataDir`. Returns false only if content itself
    /// is unusable; an individual sprite that fails to parse is recorded as a
    /// warning and drawn as a coloured disc instead, so one bad document never
    /// takes the game down. Missing ground artwork costs the flat biome
    /// colour, never the frame.
    bool build(const ContentRegistry& content, const std::string& dataDir = "data");

    /// Draws mob `index` centred at (x, y), fitted to `diameter` pixels, with
    /// `rotation` radians applied about its centre. `mirrored` flips the art
    /// across its own vertical axis AFTER the rotation, which is what the
    /// browser build's `reversed` mobs do -- turning them by pi instead
    /// rotates asymmetric artwork rather than reflecting it.
    void drawMob(Canvas&, std::uint16_t index, double x, double y, double diameter,
                 double rotation, double timeSeconds, bool mirrored = false) const;

    void drawPetal(Canvas&, std::uint16_t index, double x, double y, double diameter,
                   double rotation, double timeSeconds) const;

    /// True when the sprite compiled; false when the fallback disc is used.
    bool mobDrawable(std::uint16_t index) const;
    bool petalDrawable(std::uint16_t index) const;

    /// The 400-unit ground artwork of one ground type, or null when its file
    /// could not be read. The map's `background` layer names which one every
    /// cell is painted with; ids are in the order the nine map sections used
    /// to be, which is what makes an unpainted map look unchanged.
    const SvgDocument* groundArt(int groundId) const;

    /// The repeating artwork of one tile kind, or null when the kind is a flat
    /// colour. One copy covers one 300-unit cell, the period the browser
    /// build's tile pattern repeats at.
    const SvgDocument* tileArt(Tile tile) const;

    /// The artwork of a wall or water tile of skin `skin` (constants.h's
    /// kTileSkinNames index) showing the sides in `mask` (kEdge* bits). For
    /// the default skin: `wall.svg` for a wall with no edges,
    /// `wall_edge_<sides>.svg` / `water_edge_<sides>.svg` otherwise, and null
    /// for water with no edges -- the flat fill IS its artwork. For a biome
    /// skin: `wall_<skin>.svg`, `wall_<skin>_edge_<sides>.svg`,
    /// `water_<skin>.svg`, `water_<skin>_edge_<sides>.svg`. Null for any other
    /// tile kind, for a skin past the table, and for a file that could not be
    /// read, which the renderer answers with the default skin's art or the
    /// flat colour rather than a hole.
    const SvgDocument* edgeArt(Tile tile, std::uint8_t skin, std::uint8_t mask) const;

    /// The floor decoration `floor_<skin>_<variant>.svg` an air cell of a
    /// biome skin draws over its ground. Null for the default skin (which has
    /// none), for a variant past kFloorVariantsPerSkin, and for a file that
    /// could not be read -- and null means draw nothing, which is what a
    /// plain air cell does anyway.
    const SvgDocument* floorArt(std::uint8_t skin, std::uint8_t variant) const;

    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    struct Sprite {
        std::shared_ptr<SvgDocument> document;
        std::uint32_t fallbackColor = 0xFFFFFFu;
        bool usable = false;
        /// The artwork declares nothing to draw, so neither does this: several
        /// petals and mobs ship a literally empty <svg/>, and the browser's
        /// rasterised canvas for one of those is blank. Distinct from `usable`
        /// because a document we merely failed to build still gets the
        /// coloured stand-in.
        bool blank = false;
    };

    void draw(Canvas&, const Sprite&, double x, double y, double diameter,
              double rotation, double timeSeconds, bool mirrored) const;

    /// Parses one optional document, recording a warning instead of failing.
    std::shared_ptr<SvgDocument> compileArt(const std::string& source, const std::string& label);

    std::vector<Sprite> mobs_;
    std::vector<Sprite> petals_;
    std::array<std::shared_ptr<SvgDocument>, kSectionCount> ground_{};
    std::shared_ptr<SvgDocument> bridge_;
    /// One row per skin, indexed by edge mask (or, for the floors, by
    /// variant). Slot 0 of a wall row is the skin's plain wall; slot 0 of the
    /// default water row stays empty (the flat fill), of a biome water row
    /// holds its base art; a floor row uses slots 0..kFloorVariantsPerSkin-1.
    using TileArtRow = std::array<std::shared_ptr<SvgDocument>, kEdgeMaskMax + 1>;
    std::array<TileArtRow, kTileSkinCount> wallArt_{};
    std::array<TileArtRow, kTileSkinCount> waterArt_{};
    std::array<TileArtRow, kTileSkinCount> floorArt_{};
    std::vector<std::string> warnings_;
};

} // namespace flix
