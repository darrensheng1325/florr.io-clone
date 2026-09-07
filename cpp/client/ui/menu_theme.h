#pragma once
// Panel palettes.
//
// Every menu in the game is the same object -- a rounded card in a saturated
// colour, a darker border of the same hue, white outlined text -- and is told
// apart by that colour alone. A player learns "blue is my petals, tan is the
// forge, yellow is the bestiary" long before they read a title, so the colours
// are the identity and belong in one table rather than in eight files.

#include <cstdint>

#include "client/ui/theme.h"

namespace flix::ui {

/// The fill/border pair a panel is built from, plus the colours of the chrome
/// that sits on it.
struct PanelSkin {
    std::uint32_t fill = kPanel;
    std::uint32_t border = kPanelDark;
    /// Divider rules and the scrollbar thumb. Defaults to the border colour,
    /// which is what makes both read as part of the frame rather than content.
    std::uint32_t accent = kPanelDark;
};

/// The close button's face and rim, shared by EVERY panel rather than carried
/// per skin.
///
/// It is the one control on a card that does not belong to the card: it means
/// the same thing everywhere and it is the one a player hits without looking,
/// so tinting it per panel makes the muscle memory hunt for a different button
/// each time. These are the inventory's, which is what the rest were dragged
/// to match.
inline constexpr std::uint32_t kCloseFace = 0xAE5B58u;
inline constexpr std::uint32_t kCloseRim = 0x8D4A47u;

inline constexpr PanelSkin kInventorySkin{0x6B9DD6u, 0x5680ADu, 0x5680ADu};
inline constexpr PanelSkin kCraftingSkin{0xDB9D5Bu, 0xB17F48u, 0xB17F48u};
inline constexpr PanelSkin kGallerySkin{0xE6D64Cu, 0xA89D36u, 0xA89D36u};
/// The talent card is the one panel drawn against a reference screenshot
/// rather than the browser build's CSS, so its body is that shot's dusty red
/// rather than the pink the rest of the family was derived from. The border
/// doubles as the tree's own ink: every connector and the TP badge are drawn
/// in it, which is what makes the fan read as part of the card.
inline constexpr PanelSkin kTalentsSkin{0xCC625Eu, 0xA44F4Cu, 0xA44F4Cu};
/// The shop is the one panel drawn against a reference screenshot rather than
/// against the browser build's CSS, so its greens are that shot's -- and it is
/// also the one card with NO frame: the green ring around it in that shot is
/// the page behind the card, not a border. Border and accent are the body
/// colour deliberately, so a shared helper that draws either paints nothing.
inline constexpr PanelSkin kShopSkin{0x65c359u, 0x65c359u, 0x7DC065u};
/// The skin studio is the one panel whose border is LIGHTER than its body --
/// it borrows the strip button's own purple as the frame.
inline constexpr PanelSkin kSkinsSkin{0x8737B6u, 0x9A3FD0u, 0x9A3FD0u};
inline constexpr PanelSkin kLeaderboardSkin{0xE8A023u, 0xC4871Au, 0xC4871Au};
/// Settings and the debug panel share one grey card, its border the same grey
/// at 0.8 HSV value.
inline constexpr PanelSkin kSettingsSkin{0xAAAAAAu, 0x888888u, 0x888888u};
inline constexpr PanelSkin kDebugSkin = kSettingsSkin;
inline constexpr PanelSkin kChangelogSkin{0x49C46Fu, 0x4CAF50u, 0x4CAF50u};
inline constexpr PanelSkin kNotificationsSkin{0x4A90E2u, 0x357ABDu, 0x357ABDu};
inline constexpr PanelSkin kGuildSkin{0x27DADEu, 0x1FB3B0u, 0x1FB3B0u};

// --- shared panel metrics ---------------------------------------------------

/// Border width and corner radius. A heavy frame on a softly rounded corner:
/// the list panels read as a slab the content is sunk into, and the corner is
/// round enough to be seen past the border's own thickness.
inline constexpr double kMenuBorder = 7.0;
inline constexpr double kMenuRadius = 5.0;
inline constexpr double kMenuPadding = 14.0;

/// Title, then the line under it that says what to do with the panel. The gap
/// between the two is wide enough that they read as a heading and a caption
/// rather than as one block.
inline constexpr double kMenuTitleSize = 24.0;
inline constexpr double kMenuSubtitleSize = 16.0;

/// Where those two sit, measured from the panel's top edge rather than from
/// `kMenuPadding`: the heading is clear of the border, and the gap under it is
/// wide enough that the title and the instruction line read as two things.
inline constexpr double kMenuTitleTop = 19.0;
inline constexpr double kMenuSubtitleDrop = 43.0;

/// The square close button in every panel's top-right corner.
inline constexpr double kCloseSize = 29.0;


/// One inventory/shop/gallery cell. Five of them plus their gaps is what sets
/// the inventory panel's width, so these two are load-bearing.
///
/// 60 is `kItemTileDesign`: at exactly that size a tile is drawn 1:1 with the
/// design cell it is written in, so no icon, name or badge is resampled.
inline constexpr double kCellSize = 60.0;
inline constexpr double kCellGap = 10.0;

/// The dark chrome the toggle, the search field and the TP badge are made of.
inline constexpr std::uint32_t kControlDark = 0x3A3A3Au;
inline constexpr std::uint32_t kControlMid = 0x666666u;
inline constexpr std::uint32_t kControlLit = 0xCFCFCFu;
inline constexpr std::uint32_t kControlField = 0xEEEEEEu;

/// The two anchors the browser build hangs panels off.
///
/// The tall list panels (inventory, craft, talents) sit a third of the way
/// down and kMenuInsetX in from the left, clear of the icon column, and run
/// two thirds of the viewport tall. The corner panels (settings, changelog,
/// notifications, guild, leaderboard, skins, shop, gallery, debug) are pinned
/// directly under the top icon row instead, at their own fixed sizes.
inline constexpr double kMenuInsetX = 91.0;
inline constexpr double kMenuListTopFraction = 1.0 / 3.0;
inline constexpr double kMenuListHeightFraction = 2.0 / 3.0;
/// ...less this, so the card stops short of the bottom edge instead of being
/// clipped by it. `top + height` is exactly 1.0, so without a pad every list
/// panel runs off the screen whatever the viewport is.
inline constexpr double kMenuListBottomPad = 16.0;
/// The corner panels hang off the top-left corner, tucked in under the icon
/// row: 6 in from the edge -- a shade further out than the row's own 9, so the
/// card reads as a page under the buttons rather than as one more button --
/// and 5 below the row's bottom edge.
inline constexpr double kMenuCornerX = 6.0;
inline constexpr double kMenuCornerY = 62.0;

} // namespace flix::ui
