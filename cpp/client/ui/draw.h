#pragma once
// The UI drawing toolkit.
//
// Every widget in the game is built from these few primitives, so the look
// stays consistent without each screen re-deriving it. Nothing here holds
// state -- these are pure draw calls against a Canvas, and layout is the
// caller's business.

#include <cstdint>
#include <string>

#include "canvas.h"

#include "client/ui/theme.h"
#include "shared/core/types.h"

namespace flix::ui {

/// Where text sits relative to the position given.
enum class Align { Left, Centre, Right };
/// `Alphabetic` puts the pen ON the baseline -- the canvas default the browser
/// build leaves in place whenever a call site names no baseline of its own.
enum class Baseline { Top, Middle, Bottom, Alphabetic };

struct TextStyle {
    /// The browser's `drawText` default, not `kBodySize`: a call site ported
    /// from the reference that names no size must land on the same point size
    /// there as here. kBodySize stays what layouts measure against.
    double size = 14.0;
    std::uint32_t fill = kPaper;
    /// The outline is what makes text legible over arbitrary game content.
    /// Setting `strokeWidth` to 0 opts out, for text on a known flat panel.
    std::uint32_t stroke = kInk;
    double strokeWidth = -1;    ///< negative means size * kTextStrokeRatio
    bool bold = false;
    Align align = Align::Left;
    Baseline baseline = Baseline::Middle;
    /// Join for the glyph outline. The browser's drawText deliberately leaves
    /// the ambient join alone; only the tooltip painter asks for round, and it
    /// is the one surface whose outlines visibly differ because of it.
    bool roundJoin = false;
};

/// Applies `rgb` at `alpha` to the canvas fill.
void setFill(Canvas&, std::uint32_t rgb, double alpha = 1.0);
void setStroke(Canvas&, std::uint32_t rgb, double alpha = 1.0);

/// Stroke-then-fill text, which is the whole reason this exists: doing it in
/// the other order eats the glyph with its own outline.
void text(Canvas&, const std::string& s, double x, double y, const TextStyle& style = {});
double textWidth(Canvas&, const std::string& s, double size, bool bold = false);

/// Paints one text run whose pen is already resolved: `penX` is the run's left
/// edge and `baseline` its alphabetic baseline. `text()` is this plus the
/// alignment arithmetic; the painters that need a per-run alpha, a per-glyph
/// pen or the reverse paint order call it directly.
///
/// This is the one seam where the web build stops being a path renderer. A
/// glyph outline is an ordinary path, so the native rasterizer draws text the
/// way it draws any other shape -- but a browser keeps a cache of rasterized
/// glyphs, and handing it a freshly built Path2D of contours every frame
/// throws that cache away and re-rasterizes every letter on screen. Measured
/// on the title screen, that was ~6,000 of the frame's ~8,700 canvas
/// operations. The web build hands the run to the page's own text engine
/// instead, which is what the reference client did; the native build keeps the
/// outlines, having no other text engine to hand it to.
///
/// `fillFirst` puts the fill under the outline instead of over it -- the
/// changelog bullet is the one glyph in the build drawn that way.
void paintRun(Canvas&, const std::string& s, double penX, double baseline,
              const TextStyle& style, double strokeAlpha = 1.0, double fillAlpha = 1.0,
              bool fillFirst = false);

/// A filled, outlined, rounded rectangle -- the basis of every panel, slot and
/// button in the game.
void plate(Canvas&, Rect r, std::uint32_t fill, double radius = kPanelRadius,
           std::uint32_t outline = kInk, double outlineWidth = -1, double alpha = 1.0);

/// A panel with the standard body colour and a darker inset edge.
void panel(Canvas&, Rect r, double alpha = 1.0);

/// A button in the browser build's `gardn` style: a rounded rect with a thick
/// stroke in the fill's own darker shade, and chunky outlined white text.
/// Hover and press are brightness changes on the fill, never a different hue.
struct ButtonStyle {
    std::uint32_t fill = kAccent;
    /// 0xffffffff derives the outline as the fill at 0.8 HSV value.
    std::uint32_t outline = 0xFFFFFFFFu;
    double outlineWidth = 5.0;
    /// The browser build's buttons are nearly square-cornered; a rounder one
    /// reads as a different control.
    double radius = 3.0;
    double textSize = kButtonTextSize;
    double textStrokeWidth = 3.0;
    bool enabled = true;
    /// Off, because `drawGardnButton` never measures its label: a name too
    /// long for its box overflows both ends of it there, and a button that
    /// quietly shrank instead would read as a different type scale beside its
    /// siblings ("Computer Lab" in the 90px biome row). Opt in per call site
    /// only where a panel is measured to need it.
    bool shrinkToFit = false;
};

/// Draws a button. Hover and press are passed in rather than tracked here so
/// that hit-testing stays with the screen that owns the layout.
void button(Canvas&, Rect r, const std::string& label, bool hovered, bool pressed,
            const ButtonStyle& style = {});

/// A horizontal bar with an outline, used for health, XP and reload sweeps.
/// `fraction` is clamped, so a caller need not sanitise it.
void bar(Canvas&, Rect r, double fraction, std::uint32_t fill,
         std::uint32_t back = kHealthBack, double radius = -1);

/// A circle with the standard outline. Flowers, petals and drops are all this.
void disc(Canvas&, Vec2 centre, double radius, std::uint32_t fill,
          std::uint32_t outline = kInk, double outlineWidth = -1, double alpha = 1.0);

/// A full-screen scrim behind a modal.
void scrim(Canvas&, double alpha = 0.45);

/// A text field's box and contents, with a blinking caret when focused.
/// `masked` renders the value as bullets, for passwords.
///
/// `timeSeconds` is the caller's frame clock and the caret does NOT read it:
/// the browser blinks on `Date.now()`, so the phase is taken from the wall
/// clock instead. Two clients launched a second apart would otherwise pulse
/// against each other and against the browser build.
///
/// The defaults are the browser build's auth-form input: a saturated green
/// plate with its own 0.8-value shade as a thick round-joined outline, and a
/// focused state that widens the outline rather than recolouring it. The
/// outline is CENTRED on the box, as `ctx.stroke()` draws it there, so the
/// painted field is `outlineWidth / 2` larger than `r` on every side.
struct TextFieldStyle {
    std::uint32_t fill = kField;
    /// 0xffffffff derives the outline as the fill at 0.8 HSV value, which is
    /// what `hsvAdjust(color, 0.8)` does for every gardn-style control.
    std::uint32_t outline = 0xFFFFFFFFu;
    std::uint32_t focusedOutline = 0xFFFFFFFFu;
    double fillAlpha = 1.0;
    double outlineAlpha = 1.0;
    double radius = 3.0;
    double outlineWidth = 4.0;
    double focusedOutlineWidth = 5.0;
    double textSize = 18.0;
    std::uint32_t textFill = kPaper;
    double textStrokeWidth = 2.0;
    bool bold = false;
    std::uint32_t caret = kPaper;
    double padding = 10.0;
    /// On, because in the reference the field's own plate is what sets the
    /// join: `drawInput` and the lobby name field both leave `lineJoin =
    /// 'round'` ambient across the `drawText` that follows, and `drawText`
    /// deliberately never resets it. Every field in the browser build is
    /// therefore round-joined.
    bool roundJoin = true;
};

void textField(Canvas&, Rect r, const std::string& value, const std::string& placeholder,
               bool focused, bool masked, double timeSeconds,
               const TextFieldStyle& style = {});

/// True when `point` is inside `r`. Here so every screen hit-tests the same way.
inline bool hit(Rect r, Vec2 point) { return r.contains(point); }

} // namespace flix::ui
