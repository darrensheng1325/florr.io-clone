#include "client/ui/draw.h"

#include "client/ui/text.h"
#include "client/ui/text_select.h"
#ifndef __EMSCRIPTEN__
#include "client/ui/text_cache.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace flix::ui {

namespace {

Color toColor(std::uint32_t rgb, double alpha) {
    return Color{
        static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
        static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
        static_cast<std::uint8_t>(rgb & 0xFF),
        static_cast<std::uint8_t>(clamp(alpha, 0.0, 1.0) * 255.0 + 0.5),
    };
}

/// Where the pen starts, given an alignment and the text's measured width.
double originX(double x, double width, Align align) {
    switch (align) {
        case Align::Centre: return x - width * 0.5;
        case Align::Right: return x - width;
        default: return x;
    }
}

/// The baseline for a requested vertical anchor. Canvas names these after the
/// em box; ascent is positive and descent is negative, as the font stores them.
double baselineY(double y, double size, Baseline baseline, bool bold) {
    switch (baseline) {
        case Baseline::Top: return y + ascent(size, bold);
        case Baseline::Bottom: return y + descent(size, bold);
        case Baseline::Alphabetic: return y;
        default: return y + (ascent(size, bold) + descent(size, bold)) * 0.5;
    }
}

#ifdef __EMSCRIPTEN__
/// The CSS font shorthand for a run. `Ubuntu` is the face the page loads and
/// the same one `data/Ubuntu-*.ttf` supplies to `measure()`, so a layout
/// measured against the outlines still fits what the browser draws.
std::string fontSpec(double size, bool bold) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s%.3fpx Ubuntu, sans-serif", bold ? "bold " : "", size);
    return buf;
}
#endif

/// The rounded box every gardn-style control is built from: fill the path,
/// then stroke it CENTRED, exactly as `drawGardnButton` does. The stroke sits
/// half outside `r`, which is why nothing here insets first -- a control that
/// shrank to fit its own outline would no longer line up with the browser's.
void strokedBox(Canvas& canvas, Rect r, double radius, std::uint32_t fill, double fillAlpha,
                std::uint32_t outline, double outlineWidth, double outlineAlpha) {
    if (r.w <= 0 || r.h <= 0) return;
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                     static_cast<float>(r.h),
                     static_cast<float>(std::min(radius, std::min(r.w, r.h) * 0.5)));
    setFill(canvas, fill, fillAlpha);
    canvas.fill();
    if (outlineWidth <= 0) return;
    canvas.save();
    canvas.setLineCap("round");
    canvas.setLineJoin("round");
    canvas.setLineWidth(static_cast<float>(outlineWidth));
    setStroke(canvas, outline, outlineAlpha);
    canvas.stroke();
    canvas.restore();
}

} // namespace

void setFill(Canvas& canvas, std::uint32_t rgb, double alpha) {
    canvas.setFillStyle(toColor(rgb, alpha));
}

void setStroke(Canvas& canvas, std::uint32_t rgb, double alpha) {
    canvas.setStrokeStyle(toColor(rgb, alpha));
}

void paintRun(Canvas& canvas, const std::string& s, double penX, double baseline,
              const TextStyle& style, double strokeAlpha, double fillAlpha, bool fillFirst) {
    if (s.empty() || !Fonts::ready()) return;

    const double strokeWidth =
        style.strokeWidth < 0 ? style.size * kTextStrokeRatio : style.strokeWidth;

#ifdef __EMSCRIPTEN__
    // The pen is already resolved, so the run is anchored the same way on both
    // builds: the browser is told to put the pen exactly where the outline
    // path would have started it, not to do the alignment itself.
    canvas.setFont(fontSpec(style.size, style.bold));
    canvas.setTextAlign("left");
    canvas.setTextBaseline("alphabetic");

    const auto strokePass = [&] {
        if (strokeWidth <= 0) return;
        // Scoped exactly as the outline path is: leaking a join and a cap out
        // of a text call silently restyles whatever shape is stroked next,
        // which is a bug that only ever shows up several draw calls away from
        // its cause. The fill colour still leaks, as it always has.
        canvas.save();
        canvas.setLineJoin(style.roundJoin ? "round" : "miter");
        canvas.setLineCap("butt");
        canvas.setLineWidth(static_cast<float>(strokeWidth));
        setStroke(canvas, style.stroke, strokeAlpha);
        canvas.strokeText(s, static_cast<float>(penX), static_cast<float>(baseline));
        canvas.restore();
    };
    const auto fillPass = [&] {
        setFill(canvas, style.fill, fillAlpha);
        canvas.fillText(s, static_cast<float>(penX), static_cast<float>(baseline));
    };
    if (fillFirst) { fillPass(); strokePass(); } else { strokePass(); fillPass(); }
#else
    // The raster cache is the native build's answer to the glyph cache the
    // comment on this function's declaration describes. It takes the run when
    // it can and says so; everything it turns down -- a rotated transform, an
    // extreme size -- goes the long way, which is the same code it bakes with.
    if (paintRunCached(canvas, s, penX, baseline, style, strokeWidth, strokeAlpha, fillAlpha,
                       fillFirst)) {
        return;
    }
    paintRunDirect(canvas, s, penX, baseline, style, strokeWidth, strokeAlpha, fillAlpha,
                   fillFirst);
#endif
}

void text(Canvas& canvas, const std::string& s, double x, double y, const TextStyle& style) {
    if (s.empty() || !Fonts::ready()) return;
    const double pen = originX(x, measure(s, style.size, style.bold), style.align);
    const double base = baselineY(y, style.size, style.baseline, style.bold);
    // The one place a run can be recorded from: every label, heading and chat
    // token in the game is painted through here, so the selectable-text layer
    // needs no second copy of the alignment arithmetic to agree with.
    if (capturingText()) {
        TextSelect::instance().record(s, pen, base, style.size, style.bold);
    }
    paintRun(canvas, s, pen, base, style);
}

double textWidth(Canvas&, const std::string& s, double size, bool bold) {
    return measure(s, size, bold);
}

void plate(Canvas& canvas, Rect r, std::uint32_t fill, double radius,
           std::uint32_t outline, double outlineWidth, double alpha) {
    if (r.w <= 0 || r.h <= 0) return;
    const double width = outlineWidth < 0 ? outlineFor(std::min(r.w, r.h)) : outlineWidth;
    // Inset by half the stroke so the outline sits inside the requested
    // rectangle; a centred stroke would make every panel silently larger than
    // the layout that positioned it.
    const double inset = width * 0.5;
    const double clampedRadius = std::min(radius, std::min(r.w, r.h) * 0.5);

    canvas.beginPath();
    canvas.roundRect(static_cast<float>(r.x + inset), static_cast<float>(r.y + inset),
                     static_cast<float>(std::max(0.0, r.w - width)),
                     static_cast<float>(std::max(0.0, r.h - width)),
                     static_cast<float>(std::max(0.0, clampedRadius - inset)));
    setFill(canvas, fill, alpha);
    canvas.fill();
    if (width > 0) {
        setStroke(canvas, outline, alpha);
        canvas.setLineWidth(static_cast<float>(width));
        canvas.setLineJoin("round");
        canvas.stroke();
    }
}

void panel(Canvas& canvas, Rect r, double alpha) {
    plate(canvas, r, kPanel, kPanelRadius, kPanelDark, 4.0, alpha);
}

void button(Canvas& canvas, Rect r, const std::string& label, bool hovered, bool pressed,
            const ButtonStyle& style) {
    TextCaptureScope off(false);
    // Brightness in HSV, matching the browser build exactly: press 0.9, hover
    // 1.1, outline 0.8. A linear channel scale agrees with these everywhere
    // except a clamped brighten, which is where the two visibly diverge.
    std::uint32_t fill = style.fill;
    if (!style.enabled) fill = hsvScale(fill, 0.45);
    else if (pressed) fill = hsvScale(fill, 0.9);
    else if (hovered) fill = hsvScale(fill, 1.1);

    // The outline is derived from the BASE colour, not the hover/press shade,
    // so a button's edge holds still while its face lights up.
    const std::uint32_t outline = style.outline == 0xFFFFFFFFu
        ? hsvScale(style.fill, 0.8)
        : style.outline;
    strokedBox(canvas, r, style.radius, fill, 1.0, outline, style.outlineWidth, 1.0);

    TextStyle ts;
    ts.size = style.textSize;
    // Only when the call site asks. `drawGardnButton` has no measuring step at
    // all -- a label wider than its box simply runs out of both ends of it --
    // and shrinking by default made "Computer Lab" render at 11.4px beside a
    // row of 14px siblings, which is visible without a reference to hand.
    if (style.shrinkToFit) {
        const double available = r.w - style.outlineWidth * 2 - 6.0;
        const double measured = measure(label, ts.size, true);
        if (measured > available && available > 0) {
            ts.size = std::max(8.0, ts.size * available / measured);
        }
    }
    ts.bold = true;
    ts.strokeWidth = style.textStrokeWidth;
    ts.align = Align::Centre;
    ts.baseline = Baseline::Middle;
    ts.fill = style.enabled ? kPaper : shade(kPaper, 0.65);
    text(canvas, label, r.x + r.w * 0.5, r.y + r.h * 0.5, ts);
}

void bar(Canvas& canvas, Rect r, double fraction, std::uint32_t fill,
         std::uint32_t back, double radius) {
    if (r.w <= 0 || r.h <= 0) return;
    const double rad = radius < 0 ? r.h * 0.5 : radius;
    plate(canvas, r, back, rad);

    const double clamped = clamp(fraction, 0.0, 1.0);
    if (clamped <= 0) return;

    const double outline = outlineFor(r.h);
    const double innerH = std::max(0.0, r.h - outline * 2);
    const double innerW = std::max(0.0, r.w - outline * 2) * clamped;
    // Below the corner radius the fill would render as a sliver of the wrong
    // shape; drawing nothing reads better than a stray dot.
    if (innerW < 1.0 || innerH < 1.0) return;

    canvas.beginPath();
    canvas.roundRect(static_cast<float>(r.x + outline), static_cast<float>(r.y + outline),
                     static_cast<float>(innerW), static_cast<float>(innerH),
                     static_cast<float>(std::min(rad, innerH * 0.5)));
    setFill(canvas, fill);
    canvas.fill();
}

void disc(Canvas& canvas, Vec2 centre, double radius, std::uint32_t fill,
          std::uint32_t outline, double outlineWidth, double alpha) {
    if (radius <= 0) return;
    const double width = outlineWidth < 0 ? outlineFor(radius * 2) : outlineWidth;
    setFill(canvas, fill, alpha);
    canvas.fillCircle(static_cast<float>(centre.x), static_cast<float>(centre.y),
                      static_cast<float>(radius));
    if (width > 0) {
        setStroke(canvas, outline, alpha);
        canvas.setLineWidth(static_cast<float>(width));
        canvas.strokeCircle(static_cast<float>(centre.x), static_cast<float>(centre.y),
                            static_cast<float>(radius));
    }
}

void scrim(Canvas& canvas, double alpha) {
    setFill(canvas, kShade, alpha);
    canvas.fillRect(0, 0, static_cast<float>(canvas.width()), static_cast<float>(canvas.height()));
}

void selectionHighlight(Canvas& canvas, const TextRun& run, const TextSelection& selection,
                        Rect band, std::uint32_t colour, double alpha) {
    if (selection.empty()) return;
    const double left = std::max(xOfIndex(run, selection.begin()), band.x);
    const double right = std::min(xOfIndex(run, selection.end()), band.right());
    if (right <= left) return;
    setFill(canvas, colour, alpha);
    canvas.fillRect(static_cast<float>(left), static_cast<float>(band.y),
                    static_cast<float>(right - left), static_cast<float>(band.h));
}

namespace {

/// A field shorter than its own type scale would otherwise clip its glyphs
/// against its outline; the browser never hits this because every one of its
/// fields is 42px tall.
double fieldTextSize(Rect r, const TextFieldStyle& style) {
    return std::min(style.textSize, r.h * 0.6);
}

} // namespace

TextRun textFieldRun(Rect r, const std::string& value, const TextFieldStyle& style) {
    TextRun run;
    run.text = value;
    run.originX = r.x + style.padding;
    run.size = fieldTextSize(r, style);
    run.bold = style.bold;
    return run;
}

void textField(Canvas& canvas, Rect r, const std::string& value, const std::string& placeholder,
               bool focused, bool masked, double timeSeconds,
               const TextFieldStyle& style, const TextFieldState* state) {
    TextCaptureScope off(false);
    // Every field painted through here is a field a finger can tap, focused or
    // not -- and the unfocused ones are the point: raising the on-screen
    // keyboard is the job of the touch that FOCUSES a field, so waiting for
    // one to be focused before recording where it is would be waiting for the
    // tap that has already happened. See ui::TextFieldRegions.
    TextFieldRegions::instance().record(r);
    const std::uint32_t outlineBase = focused ? style.focusedOutline : style.outline;
    const std::uint32_t outline =
        outlineBase == 0xFFFFFFFFu ? hsvScale(style.fill, 0.8) : outlineBase;
    strokedBox(canvas, r, style.radius, style.fill, style.fillAlpha, outline,
               focused ? style.focusedOutlineWidth : style.outlineWidth, style.outlineAlpha);

    const double textSize = fieldTextSize(r, style);

    std::string shown = value;
    if (masked) shown.assign(value.size(), '*');

    TextStyle ts;
    ts.size = textSize;
    ts.bold = style.bold;
    ts.baseline = Baseline::Middle;
    ts.strokeWidth = style.textStrokeWidth;
    // Set before the placeholder branch so both strings are outlined alike;
    // the reference draws them through the same call, under the join its plate
    // left ambient.
    ts.roundJoin = style.roundJoin;

    // An empty focused field shows nothing, not the placeholder: the caret is
    // already saying where the text will go.
    if (shown.empty() && !focused) {
        ts.fill = style.textStrokeWidth > 0 ? style.textFill : shade(style.textFill, 0.55);
        text(canvas, placeholder, r.x + style.padding, r.y + r.h * 0.5, ts);
        return;
    }

    // The highlight goes under the text, and only on a field whose caret this
    // painter is being told about -- a masked one keeps its old behaviour,
    // since a selection over a row of bullets is not a thing worth painting.
    const bool selectable = state && !masked;
    if (selectable && focused) {
        selectionHighlight(canvas, textFieldRun(r, value, style), state->selection,
                           Rect{r.x + style.padding * 0.5, r.y + 6.0,
                                r.w - style.padding, std::max(2.0, r.h - 12.0)});
    }

    ts.fill = style.textFill;
    text(canvas, shown, r.x + style.padding, r.y + r.h * 0.5, ts);

    if (!focused) return;
    // `Math.floor(Date.now() / 500) % 2 === 0`, to the millisecond: phasing the
    // blink on the epoch rather than on the caller's frame clock is what keeps
    // two clients -- and a client and the browser -- pulsing in step, instead
    // of each starting its cycle wherever its own process happened to launch.
    // Filled rather than stroked: a stroked line straddles its path and lands
    // half a pixel off the glyph it follows.
    // A field with a caret of its own blinks on THAT instead, so the bar stays
    // solid while it is being typed at rather than winking out mid-word.
    if (selectable) {
        if (!caretVisible(*state, timeSeconds)) return;
    } else {
        const auto epochMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if ((epochMillis / 500) % 2 != 0) return;
    }
    const double caretX =
        selectable ? xOfIndex(textFieldRun(r, value, style), state->selection.caret)
                   : r.x + style.padding + textWidth(canvas, shown, textSize, style.bold);
    setFill(canvas, style.caret);
    canvas.fillRect(static_cast<float>(caretX), static_cast<float>(r.y + 10), 2.0f,
                    static_cast<float>(std::max(2.0, r.h - 20.0)));
}

} // namespace flix::ui
