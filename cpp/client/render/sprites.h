#pragma once
// Compiled artwork: mobs, petals, and the tiles a map is painted with.
//
// Almost every sprite in the game is an inline SVG document inside mobs.json /
// petals.json. Those are parsed ONCE at startup into retained SvgDocuments and
// drawn straight to the canvas thereafter -- there is no bitmap bake. Baking
// mobs to bitmaps was tried in the original and cost more than it saved: a
// rarity-scaled mob needs a bitmap per size, the cache thrashes as soon as a
// crowd is on screen, and the vector path is fast enough.
//
// The exceptions are the mobs whose `image` names a PAINTER rather than
// declaring a document -- `$rock`, `$cactus` -- because their picture is a
// function of their radius and a document can only be magnified. This cache
// resolves that marker once at build time and dispatches to mob_art.h, so
// every call site keeps one way to draw a mob whichever kind it is.
//
// The map tiles are the same idea one step out, but their palette is the
// MAP's, not this file's: a Tiled tileset names one artwork per tile, the
// build stages `maps/tiles/` flat into the data directory, and this cache
// loads one by bare file name the first time a map asks for it. Nothing here
// ever builds a file name, and nothing here knows what a tile MEANS -- which
// edge or corner picture belongs in a cell was decided by the author's
// terrain brush and is in the map file already.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "canvas.h"
#include "client/render/mob_art.h"
#include "svg.h"

namespace flix {

class ContentRegistry;

class SpriteCache {
public:
    /// Compiles every mob and petal image in `content`, and remembers
    /// `dataDir` as where tile artwork is staged. Returns false only if
    /// content itself is unusable; an individual sprite that fails to parse is
    /// recorded as a warning and drawn as a coloured disc instead, so one bad
    /// document never takes the game down.
    bool build(const ContentRegistry& content, const std::string& dataDir = "data");

    /// Draws mob `index` centred at (x, y), fitted to `diameter` pixels, with
    /// `rotation` radians applied about its centre. `mirrored` flips the art
    /// across its own vertical axis AFTER the rotation, which is what the
    /// browser build's `reversed` mobs do -- turning them by pi instead
    /// rotates asymmetric artwork rather than reflecting it.
    ///
    /// `worldRadius` is the body's radius in WORLD units, and only the mobs
    /// drawn by code read it: a rock's facets and a cactus's spines are cut
    /// from how big the mob IS, never from how big it happens to be on this
    /// screen, so zooming out gives you a smaller rock rather than a smoother
    /// one. Zero means "the same as the drawn radius", which is what a call
    /// site with no world behind it -- a bestiary tile, a contact sheet -- has
    /// to say.
    void drawMob(Canvas&, std::uint16_t index, double x, double y, double diameter,
                 double rotation, double timeSeconds, bool mirrored = false,
                 double worldRadius = 0.0) const;

    void drawPetal(Canvas&, std::uint16_t index, double x, double y, double diameter,
                   double rotation, double timeSeconds) const;

    /// True when the sprite compiled; false when the fallback disc is used.
    bool mobDrawable(std::uint16_t index) const;
    bool petalDrawable(std::uint16_t index) const;

    /// The artwork of one map tile, by the BARE file name its tileset names
    /// (`grass_c_0.svg`). Read out of the data directory the first time it is
    /// asked for and kept thereafter, so a frame never touches the disk twice
    /// for the same tile.
    ///
    /// Null when the file is not there or will not parse -- one warning per
    /// name, and the cell simply draws nothing. A map is free to name art that
    /// has not been drawn yet, and that must cost that cell its picture, never
    /// the frame.
    const SvgDocument* tileArt(const std::string& file) const;

    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    struct Sprite {
        std::shared_ptr<SvgDocument> document;
        /// Set when the mob's `image` names a painter instead of declaring a
        /// document; `document` is then null and never consulted.
        MobArt art = MobArt::None;
        /// The mob's or petal's own `color`, which is both what a painter
        /// bodies its artwork in and what a sprite that would not compile is
        /// drawn as. One field because it is one fact about the entry.
        std::uint32_t color = 0xFFFFFFu;
        bool usable = false;
        /// The artwork declares nothing to draw, so neither does this: several
        /// petals and mobs ship a literally empty <svg/>, and the browser's
        /// rasterised canvas for one of those is blank. Distinct from `usable`
        /// because a document we merely failed to build still gets the
        /// coloured stand-in.
        bool blank = false;
    };

    void draw(Canvas&, const Sprite&, double x, double y, double diameter,
              double rotation, double timeSeconds, bool mirrored, double worldRadius) const;

    /// Parses one optional document, recording a warning instead of failing.
    std::shared_ptr<SvgDocument> compileArt(const std::string& source, const std::string& label);

    std::vector<Sprite> mobs_;
    std::vector<Sprite> petals_;
    /// Where tile artwork is staged, from build().
    std::string dataDir_ = "data";
    /// Tile artwork by file name, filled on demand. A name that failed is kept
    /// with a null document, which is what makes the warning fire exactly
    /// once. Mutable because tileArt() is a const read of a lazily filled
    /// cache -- it reports what the data directory holds, it does not change
    /// what the cache means.
    mutable std::unordered_map<std::string, std::shared_ptr<SvgDocument>> tileArt_;
    mutable std::vector<std::string> warnings_;
};

} // namespace flix
