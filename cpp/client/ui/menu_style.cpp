#include "client/ui/menu_style.h"

#include <algorithm>
#include <cmath>

#include "client/ui/text.h"
#include "client/ui/text_select.h"

namespace flix::ui {

namespace {

/// Body text on a panel: white, outlined hard enough to read over a saturated
/// fill or a mob sprite, and never over-stroked at small sizes.
TextStyle labelStyle(double size, bool bold, std::uint32_t fill, double stroke) {
    TextStyle style;
    style.size = size;
    style.bold = bold;
    style.fill = fill;
    style.stroke = kInk;
    style.strokeWidth = stroke;
    return style;
}

/// The close button's cross is inset by this fraction of the button's width on
/// every side. 0.27 of the overlay panels' 30px box is the reference's literal
/// 8px pad, and expressing it as a ratio is what lets the 26px button in the
/// tall list panels wear the same cross.
constexpr double kCrossPadRatio = 0.27;

/// The rim, and the cross's ink, as fractions of the button's own width.
///
/// Both used to be literals -- a 2px rim and a 2.5px cross whatever the button
/// -- which drew a 29px close button with the rim of a 20px one. A close
/// control is one shape at several sizes, so its parts scale with it.
constexpr double kCloseRadius = 4.0;
constexpr double kCloseRimRatio = 0.14;
constexpr double kCloseCrossRatio = 0.105;

/// The cross's ink. Off-white, not `kPaper`: over a red face a pure white
/// cross reads a weight heavier than the reference's does, and both reference
/// shots measure it at exactly this.
constexpr std::uint32_t kCloseCross = 0xCCCCCCu;

/// How far the hover face is lightened off the skin's own close colour.
///
/// Derived rather than a literal swatch: the panels no longer share one pink,
/// and a fixed rose over the inventory's brick close read as a different
/// button rather than as the same one lit.
constexpr double kCloseHoverLift = 0.18;

} // namespace

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------

void roundPath(Canvas& canvas, Rect r, double radius) {
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                     static_cast<float>(r.h), static_cast<float>(radius));
}

void fillRound(Canvas& canvas, Rect r, double radius, std::uint32_t rgb, double alpha) {
    if (r.w <= 0 || r.h <= 0) return;
    setFill(canvas, rgb, alpha);
    roundPath(canvas, r, radius);
    canvas.fill();
}

void strokeRound(Canvas& canvas, Rect r, double radius, std::uint32_t rgb, double width) {
    if (r.w <= 0 || r.h <= 0) return;
    canvas.save();
    setStroke(canvas, rgb);
    canvas.setLineWidth(static_cast<float>(width));
    roundPath(canvas, r, radius);
    canvas.stroke();
    canvas.restore();
}

// ---------------------------------------------------------------------------
// The card
// ---------------------------------------------------------------------------

void overlayCard(Canvas& canvas, Rect r, std::uint32_t fill, std::uint32_t border) {
    if (r.w <= 0 || r.h <= 0) return;
    fillRound(canvas, r, kOverlayRadius, border);
    fillRound(canvas, overlayBody(r), kOverlayInnerRadius, fill);
}

void overlayCard(Canvas& canvas, Rect r, const PanelSkin& skin) {
    overlayCard(canvas, r, skin.fill, skin.border);
}

void inlaid(Canvas& canvas, Rect r, std::uint32_t fill, std::uint32_t border, double borderWidth,
            double radius, double alpha) {
    if (r.w <= 0 || r.h <= 0) return;
    canvas.setGlobalAlpha(static_cast<float>(clamp(alpha, 0.0, 1.0)));
    setFill(canvas, border);
    roundPath(canvas, r, radius);
    canvas.fill();

    const double inset = std::min(borderWidth, std::min(r.w, r.h) * 0.5);
    setFill(canvas, fill);
    roundPath(canvas, Rect{r.x + inset, r.y + inset, r.w - inset * 2, r.h - inset * 2},
              std::max(0.0, radius - 2.0));
    canvas.fill();
    canvas.setGlobalAlpha(1.0f);
}

void panelCard(Canvas& canvas, Rect r, const PanelSkin& skin, double borderWidth, double radius) {
    inlaid(canvas, r, skin.fill, skin.border, borderWidth, radius);
}

void panelTitle(Canvas& canvas, Rect panel, const std::string& title,
                const std::string& subtitle) {
    // Round-joined: every panel sets ctx.lineJoin = 'round' before its title
    // and drawText inherits it. At a 4px stroke a miter grows spikes off the
    // sharp corners of 'v' and 'y'.
    TextStyle heading = labelStyle(kMenuTitleSize, true, kPaper, 4.0);
    heading.align = Align::Centre;
    heading.baseline = Baseline::Top;
    heading.roundJoin = true;
    text(canvas, title, panel.x + panel.w * 0.5, panel.y + kMenuTitleTop, heading);

    if (subtitle.empty()) return;
    TextStyle sub = labelStyle(kMenuSubtitleSize, true, kPaper, 3.0);
    sub.align = Align::Centre;
    sub.baseline = Baseline::Top;
    sub.roundJoin = true;
    text(canvas, subtitle, panel.x + panel.w * 0.5, panel.y + kMenuTitleTop + kMenuSubtitleDrop,
         sub);
}

void panelHeading(Canvas& canvas, Rect panel, const std::string& title) {
    TextStyle heading = labelStyle(20.0, true, kPaper, 2.0);
    heading.baseline = Baseline::Top;
    text(canvas, title, panel.x + kMenuPadding + 6.0, panel.y + kMenuPadding + 6.0, heading);
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

Rect closeButtonRect(Rect panel) {
    return {panel.right() - kMenuPadding - kCloseSize, panel.y + kMenuPadding + 2.0, kCloseSize,
            kCloseSize};
}

void closeCross(Canvas& canvas, Rect r, double arm, double width, bool roundCap) {
    const double cx = r.x + r.w * 0.5;
    const double cy = r.y + r.h * 0.5;
    canvas.save();
    setStroke(canvas, kCloseCross);
    canvas.setLineWidth(static_cast<float>(width));
    canvas.setLineCap(roundCap ? "round" : "butt");
    canvas.beginPath();
    canvas.moveTo(static_cast<float>(cx - arm), static_cast<float>(cy - arm));
    canvas.lineTo(static_cast<float>(cx + arm), static_cast<float>(cy + arm));
    canvas.moveTo(static_cast<float>(cx + arm), static_cast<float>(cy - arm));
    canvas.lineTo(static_cast<float>(cx - arm), static_cast<float>(cy + arm));
    canvas.stroke();
    canvas.restore();
}

void panelClose(Canvas& canvas, Rect r, bool hovered) {
    // Not inlaid(): that derives the inner corner as radius - 2, where this
    // button's face is one step inside its outer 4 rather than two.
    const double rim = std::max(2.0, r.w * kCloseRimRatio);
    fillRound(canvas, r, kCloseRadius, kCloseRim);
    fillRound(canvas, Rect{r.x + rim, r.y + rim, r.w - rim * 2, r.h - rim * 2},
              kCloseRadius - 1.0, hovered ? lighten(kCloseFace, kCloseHoverLift) : kCloseFace);
    closeCross(canvas, r, r.w * (0.5 - kCrossPadRatio), std::max(2.0, r.w * kCloseCrossRatio),
               true);
}

void pillButton(Canvas& canvas, Rect r, const std::string& label, std::uint32_t fill,
                double textSize) {
    TextCaptureScope off(false);
    fillRound(canvas, r, 5.0, fill);

    TextStyle caption = labelStyle(textSize, false, kPaper, 0.0);
    caption.align = Align::Centre;
    text(canvas, label, r.x + r.w * 0.5, r.y + r.h * 0.5, caption);
}

void framedButton(Canvas& canvas, Rect r, const std::string& label, std::uint32_t labelColor,
                  std::uint32_t frame, bool hovered) {
    TextCaptureScope off(false);
    fillRound(canvas, r, 4.0, frame);
    // The interior is a wash rather than a colour, so the frame stays the only
    // thing that says what the button is.
    fillRound(canvas, Rect{r.x + 2.0, r.y + 2.0, r.w - 4.0, r.h - 4.0}, 3.0,
              hovered ? kPaper : kInk, hovered ? 0.22 : 0.25);

    TextStyle caption = labelStyle(13.0, true, labelColor, 3.0);
    caption.align = Align::Centre;
    caption.roundJoin = true;
    // The label sits a pixel below the button's middle: at 13px bold the
    // stroked cap height reads high without it.
    text(canvas, label, r.x + r.w * 0.5, r.y + r.h * 0.5 + 1.0, caption);
}

void chip(Canvas& canvas, Rect r, const std::string& label, bool hovered, const ChipStyle& style) {
    TextCaptureScope off(false);
    const std::uint32_t hoverFill =
        style.hoverFill == 0xFFFFFFFFu ? lighten(style.fill, 0.15) : style.hoverFill;
    const std::uint32_t fill = style.enabled ? (hovered ? hoverFill : style.fill) : 0x8A8A8Au;
    const std::uint32_t border = style.enabled ? style.border : 0x5A5A5Au;
    inlaid(canvas, r, fill, border, 2.0, style.radius, style.enabled ? 1.0 : 0.45);

    TextStyle caption = labelStyle(style.textSize, true, kPaper, 3.0);
    caption.align = Align::Centre;
    text(canvas, label, r.x + r.w * 0.5, r.y + r.h * 0.5, caption);
}

} // namespace flix::ui
