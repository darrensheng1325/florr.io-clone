#pragma once
// The one way an item is drawn, anywhere in the game.
//
// Ported from rysteria_gardn's `draw_loadout_background` and `draw_static_petal`
// (Client/Assets/Petal.cc). gardn has exactly one item renderer: the loadout
// bar, the inventory, the crafting ring, the gallery drop tables and the drops
// lying on the ground are all the same 60x60 cell drawn at different sizes, so
// a petal is instantly recognisable as the same object wherever it appears.
//
// This client used to have four of them -- the loadout bar's gardn-shaped one,
// `itemCell` for the grid panels, the shop's card and the world renderer's
// ground drop -- each with its own plate geometry, its own icon size and its
// own idea of how a multi-petal stack previews. They are all this now.
//
// Everything is laid out in gardn's 60x60 design space centred on the origin
// and scaled by `side / 60`, which is what lets one call serve a 34px crafting
// ring slot and a 70px inventory cell without either being a different design.

#include <cstdint>
#include <string>

#include "canvas.h"

#include "client/render/sprites.h"
#include "client/ui/theme.h"
#include "shared/core/types.h"
#include "shared/game/components.h"
#include "shared/game/rarity.h"

namespace flix::ui {

/// gardn's design cell. A tile's contents are written in these units.
inline constexpr double kItemTileDesign = 60.0;

/// World units of DIAMETER per unit of a petal's `size` stat -- a size-2 basic
/// petal is a 40-unit disc. The WORLD's scale, and the world's alone.
///
/// Ten units of radius per unit of size, which is gardn's petal `radius` and
/// the same figure kPetalIconSize states a tile's icon in: gardn draws a petal
/// at its collision radius (RenderPetal.cc scales the artwork by
/// `ent.radius / PETAL_DATA[...].radius`), so the two games agree about how
/// big a petal LOOKS even where they disagree about what it hits.
///
/// The browser build's 12 is what this was ported at, and it drew every petal
/// at 0.6x of gardn's, and a petal noticeably smaller in the world than the
/// same petal in its own inventory tile. The server's petal body was brought
/// to this same figure (kPetalRadiusPerSize), so the two now agree: a petal
/// hits what it looks like it hits. A petal that should still read small says
/// so with `visual_scale` in petals.json rather than by shrinking the whole
/// roster here -- but note that visual_scale moves the ARTWORK only, so a
/// scaled petal is once again drawn at a size it does not collide at.
inline constexpr double kPetalArtSize = 20.0;

/// Design units of icon DIAMETER per unit of `size`, for a tile whose petal
/// gardn does not have.
///
/// The same figure as kPetalArtSize, and not the same constant: a tile is
/// written in its own 60-unit design cell and the world in world units, so the
/// two are free to move apart again. They agree today because both are stated
/// against gardn's petal radius.
///
/// Ten units of radius per unit of size is what lines this game's `size` stat
/// up with gardn's `radius` field ON AVERAGE: it is exact for 19 of the 43
/// petals both games have and within a fifth for a dozen more. It is only the
/// FALLBACK though, because on average is not good enough -- see
/// kGardnIconRadius in the .cpp for the petals the two games genuinely
/// disagree about.
inline constexpr double kPetalIconSize = 20.0;

/// gardn lifts the icon off centre to leave room for the name along the bottom
/// edge, and draws it at 50/60 of the design size.
inline constexpr double kItemTileIconRise = 5.0;
inline constexpr double kItemTileIconScale = 0.833;

/// gardn clamps a petal whose radius exceeds 20 design units, so a giant petal
/// stays inside its plate instead of overflowing it. Expressed as a diameter
/// because the clamp applies to a whole cluster here, not just a lone petal.
///
/// A CAP, not a box: anything already inside it is left at its natural size,
/// which is what makes a moon read as a moon and a dahlia as a dahlia.
inline constexpr double kItemTileIconCap = 40.0;

/// The plate's border is the rarity colour at 0.8 HSV value -- gardn's
/// `Renderer::HSV(RARITY_COLORS[rarity], 0.8)`.
inline constexpr double kItemTilePlateShade = 0.8;

/// Draws a petal the way gardn does: one icon at its NATURAL size, or `count`
/// of them spaced evenly on a ring and each turned to face outward.
///
/// Natural size is the point. A basic petal is a small disc inside its plate
/// and a giant one nearly fills it, which is how a player reads size at a
/// glance; fitting every petal to the same box throws that away. `maxDiameter`
/// caps the cluster for the few that would otherwise overflow -- anything that
/// already fits is left alone.
///
/// Natural means gardn's own icon radius for this petal, falling back to
/// kPetalIconSize x `sizeStat` -- the tile's own constant, in the tile's own
/// design units, even where it currently holds the same number the world does.
///
/// `sizeStat` and `count` are the RARITY-scaled values (`petalStats`), not the
/// base ones: a mythic light is five icons where a common one is a single icon.
void drawPetalCluster(Canvas&, const SpriteCache&, std::uint16_t petalIndex, double sizeStat,
                      int count, double cx, double cy, double maxDiameter, double timeSeconds);

/// One item, and the states a surface needs to show it in.
struct ItemTile {
    std::uint16_t petalIndex = kNoPetal;  ///< an empty slot: the plate alone
    Rarity rarity = Rarity::Common;

    /// An empty cell: the panel's own colours instead of a rarity, and no
    /// contents. How the crafting grid shows a tier the account does not own.
    bool empty = false;
    std::uint32_t emptyFill = 0x9A8B70u;
    std::uint32_t emptyBorder = 0x9A8B70u;

    /// gardn always names the petal inside the plate. Off for the surfaces
    /// that caption a tile themselves -- the shop's price bar, the gallery's
    /// drop chance, a tile riding the cursor.
    bool showName = true;
    /// Empty takes the petal's own name.
    std::string nameOverride;
    /// Top-right, over the icon. gardn has no badge; the inventory needs one.
    std::string badge;

    /// gardn's cooldown wedge, swept as the petal reloads. 1.0 draws nothing.
    /// The owner's own bar is the only surface the server tells about a reload;
    /// everywhere else leaves this alone and gets no sweep.
    double reload = 1.0;

    /// How much of the petal is still standing, 1.0 for untouched. The face
    /// drains from the top as it falls, until a dead slot is nothing but the
    /// plate colour. Like `reload`, only the owner's bar is told.
    double health = 1.0;

    bool hovered = false;
    bool selected = false;
    bool disabled = false;  ///< greyed out; the caller also blocks the click

    /// The ground drop's backdrop: a larger, softer square under the plate,
    /// which is what lifts a drop off the terrain.
    bool shadow = false;

    double alpha = 1.0;
    double timeSeconds = 0.0;
};

/// Draws `tile` centred in `rect`, scaled from the 60x60 design cell to the
/// shorter of the rect's sides.
void drawItemTile(Canvas&, const SpriteCache&, Rect rect, const ItemTile& tile);

} // namespace flix::ui
