#pragma once
// The game's visual language, in one place.
//
// The look is flat and high-contrast: saturated fills, thick dark outlines,
// generous corner radii, and text that is always stroked before it is filled
// so it stays readable over any background. Every panel and widget derives
// from these values rather than picking its own, which is what makes a screen
// assembled from separate pieces read as one design.

#include <cstdint>

namespace flix::ui {

// --- palette ----------------------------------------------------------------

inline constexpr std::uint32_t kInk        = 0x000000u;  ///< every outline
inline constexpr std::uint32_t kPaper      = 0xFFFFFFu;  ///< text and highlights
inline constexpr std::uint32_t kPanel      = 0x599FDCu;  ///< inventory-blue panel body
inline constexpr std::uint32_t kPanelDark  = 0x4A8BC2u;  ///< matching panel border
inline constexpr std::uint32_t kSlot       = 0xEEEEEEu;  ///< empty loadout slot
inline constexpr std::uint32_t kAccent     = 0x1DD129u;  ///< Ready / primary action
inline constexpr std::uint32_t kDanger     = 0xFF4444u;
inline constexpr std::uint32_t kWarning    = 0xFFE65Du;
inline constexpr std::uint32_t kHealth     = 0x73FF54u;
inline constexpr std::uint32_t kHealthBack = 0x000000u;
inline constexpr std::uint32_t kXpBar      = 0xFAFFC9u;
inline constexpr std::uint32_t kShade      = 0x000000u;  ///< modal scrim, at low alpha
/// The green the browser build's auth form and chat field are made of.
inline constexpr std::uint32_t kField      = 0x18CE18u;
/// The wash behind selected text. Painted UNDER a field's own text, so it has
/// to read on a white inset and on a saturated plate alike; the fields on dark
/// plates pass `kPaper` instead and get a pale wash rather than a blue one.
inline constexpr std::uint32_t kSelection  = 0x3D7DD8u;

/// Menus and the title screen sit on this; the world has its own biome ground.
inline constexpr std::uint32_t kBackdrop   = 0x00D885u;

// --- metrics ----------------------------------------------------------------

/// Outline width as a fraction of the element it surrounds. A constant ratio
/// rather than constant pixels, so a large panel and a small button read as
/// the same design.
inline constexpr double kOutlineRatio = 0.06;
inline constexpr double kMinOutline = 2.0;
inline constexpr double kMaxOutline = 6.0;

/// Text outline width per pixel of font size.
inline constexpr double kTextStrokeRatio = 0.12;

inline constexpr double kPanelRadius = 8.0;
inline constexpr double kSlotRadius = 6.0;
inline constexpr double kButtonRadius = 6.0;

inline constexpr double kPanelPadding = 14.0;
inline constexpr double kSlotSize = 56.0;
inline constexpr double kSlotGap = 8.0;

// --- type scale -------------------------------------------------------------

inline constexpr double kTitleSize = 42.0;
inline constexpr double kHeadingSize = 22.0;
inline constexpr double kBodySize = 15.0;
/// The browser build's `drawGardnButton` default. Every button label in the
/// game is this size unless its box is too small to hold it.
inline constexpr double kButtonTextSize = 18.0;
inline constexpr double kSmallSize = 12.0;
inline constexpr double kDamageSize = 18.0;

// --- helpers ----------------------------------------------------------------

/// Multiplies a colour's channels, for pressed and disabled states.
///
/// Rounds rather than truncates: the browser build's `darken()` ends in
/// `Math.round`, and truncating instead shifts every derived border one step
/// darker -- enough that a slot's edge and the same slot's edge in a sibling
/// panel no longer match.
inline constexpr std::uint32_t shade(std::uint32_t rgb, double factor) {
    const auto channel = [factor](std::uint32_t c) -> std::uint32_t {
        const double v = static_cast<double>(c) * factor + 0.5;
        return static_cast<std::uint32_t>(v < 0 ? 0 : (v > 255.0 ? 255 : v));
    };
    return (channel((rgb >> 16) & 0xFF) << 16) |
           (channel((rgb >> 8) & 0xFF) << 8) |
           channel(rgb & 0xFF);
}

/// Scales a colour's HSV *value* -- what the browser build's buttons do to
/// derive their hover, press and outline shades.
///
/// Not the same as scaling the channels: past full brightness the value clamps
/// and hue and saturation are kept, so brightening a saturated colour moves it
/// toward its own pure form rather than washing it out toward white.
inline constexpr std::uint32_t hsvScale(std::uint32_t rgb, double factor) {
    const std::uint32_t r = (rgb >> 16) & 0xFF;
    const std::uint32_t g = (rgb >> 8) & 0xFF;
    const std::uint32_t b = rgb & 0xFF;
    const std::uint32_t peak = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (peak == 0) return rgb;
    const double value = static_cast<double>(peak) / 255.0;
    const double scaled = value * factor;
    const double clamped = scaled < 0 ? 0 : (scaled > 1 ? 1 : scaled);
    return shade(rgb, clamped / value);
}

inline constexpr std::uint32_t darken(std::uint32_t rgb, double amount = 0.25) {
    return shade(rgb, 1.0 - amount);
}

inline constexpr std::uint32_t lighten(std::uint32_t rgb, double amount = 0.25) {
    const auto channel = [amount](std::uint32_t c) -> std::uint32_t {
        const double v = c + (255.0 - c) * amount + 0.5;
        return static_cast<std::uint32_t>(v > 255.0 ? 255 : v);
    };
    return (channel((rgb >> 16) & 0xFF) << 16) |
           (channel((rgb >> 8) & 0xFF) << 8) |
           channel(rgb & 0xFF);
}

/// Outline width for an element of the given size, kept within sane bounds so
/// a tiny icon is not swallowed by its own border.
inline double outlineFor(double size) {
    const double w = size * kOutlineRatio;
    return w < kMinOutline ? kMinOutline : (w > kMaxOutline ? kMaxOutline : w);
}

} // namespace flix::ui
