#include "client/ui/menu_widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "client/ui/text.h"

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

/// The gap the overlay panels leave between the bottom of the scrollbar track
/// and the bottom of the card. Mirrors menu_leaderboard.cpp's own constant;
/// scrollbar() needs it to recover the viewport height from the track.
constexpr double kTrackBottomInset = 5.0;

} // namespace

// ---------------------------------------------------------------------------
// Interactive chrome
// ---------------------------------------------------------------------------
//
// The card, its heading and every button live in client/ui/menu_style.h. What
// is left here is the chrome that carries state a panel has to feed it: a
// toggle's animation, a field's caret, a scrollbar's offset.

void toggleBox(Canvas& canvas, Rect r, double lerpAmount) {
    setFill(canvas, kControlDark);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                     static_cast<float>(r.h), 5.0f);
    canvas.fill();

    const double t = clamp(lerpAmount, 0.0, 1.0);
    const auto mix = [t](std::uint32_t shift) {
        const double off = static_cast<double>((kControlMid >> shift) & 0xFF);
        const double on = static_cast<double>((kControlLit >> shift) & 0xFF);
        return static_cast<std::uint32_t>(off + (on - off) * t + 0.5);
    };
    setFill(canvas, (mix(16) << 16) | (mix(8) << 8) | mix(0));
    const double inset = 4.0;
    canvas.fillRect(static_cast<float>(r.x + inset), static_cast<float>(r.y + inset),
                    static_cast<float>(r.w - inset * 2), static_cast<float>(r.h - inset * 2));
}

void inputField(Canvas& canvas, Rect r, const std::string& value, const std::string& placeholder,
                bool focused, double timeSeconds) {
    setFill(canvas, kControlDark);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                     static_cast<float>(r.h), 5.0f);
    canvas.fill();
    setFill(canvas, kControlField);
    canvas.fillRect(static_cast<float>(r.x + 4), static_cast<float>(r.y + 4),
                    static_cast<float>(r.w - 8), static_cast<float>(r.h - 8));

    const double size = 13.0;
    // The browser floats a real <input> over this plate at r.x + 4 and gives it
    // `padding: 0 8px`, so its first glyph starts 12px in, not 8.
    const double inset = 12.0;
    // Show the TAIL of an overlong value: the caret is at the end, and a field
    // that scrolls its own start out of view is the one people expect.
    std::string shown = value;
    while (!shown.empty() && measure(shown, size, false) > r.w - inset * 2) {
        shown.erase(shown.begin());
    }

    TextStyle style = labelStyle(size, false, value.empty() ? 0x8A8A8Au : kInk, 0.0);
    text(canvas, value.empty() ? placeholder : shown, r.x + inset, r.y + r.h * 0.5, style);

    if (!focused) return;
    // A one-second cycle at an even duty and a one-pixel bar: this stands in
    // for the native <input> caret the browser shows here, and the platform
    // blink is a second, not the 1.06 an earlier pass guessed.
    if (std::fmod(timeSeconds, 1.0) < 0.5) {
        const double caretX = r.x + inset + measure(shown, size, false) + 1.0;
        setFill(canvas, kInk);
        canvas.fillRect(static_cast<float>(caretX), static_cast<float>(r.y + 6),
                        1.0f, static_cast<float>(r.h - 12));
    }
}

void scrollbar(Canvas& canvas, Rect view, double contentHeight, double scroll,
               std::uint32_t thumb, double width) {
    // The track stops 5px short of the panel's bottom edge, but the VIEWPORT
    // -- what decides both the thumb's length and how far it may travel -- is
    // the full body height. `view` is the track, so the viewport is 5px taller
    // than it; using the track for both makes the thumb short and leaves it a
    // pixel clear of the bottom at full scroll.
    const double viewport = view.h + kTrackBottomInset;
    if (contentHeight <= viewport || view.h <= 0) return;
    const double travel = contentHeight - viewport;
    // Proportional, and deliberately unfloored: the reference has no minimum,
    // and one would only bite on a list far longer than any panel shows.
    const double thumbHeight = view.h * (viewport / contentHeight);
    const double thumbY = view.y + clamp(scroll / travel, 0.0, 1.0) * (view.h - thumbHeight);
    // A 5px radius on the browser's 10px track: a full pill instead reads as
    // a different control at any other width.
    const double radius = std::min(5.0, width * 0.5);

    // A white track rather than a black one: it has to read on the saturated
    // panel colours, and a dark groove disappears into the settings grey.
    setFill(canvas, kPaper, 0.1);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(view.right() - width), static_cast<float>(view.y),
                     static_cast<float>(width), static_cast<float>(view.h),
                     static_cast<float>(radius));
    canvas.fill();

    setFill(canvas, thumb);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(view.right() - width), static_cast<float>(thumbY),
                     static_cast<float>(width), static_cast<float>(thumbHeight),
                     static_cast<float>(radius));
    canvas.fill();
}

// ---------------------------------------------------------------------------
// Tooltips
// ---------------------------------------------------------------------------

namespace {

constexpr double kTooltipPadX = 10.0;
constexpr double kTooltipPadY = 8.0;
constexpr double kTooltipLineGap = 2.0;
constexpr double kTooltipRadius = 6.0;

double rowHeight(double size) { return std::ceil(size * 1.2); }

/// One laid-out row: a tooltip line after alt substitution and word wrapping,
/// with its top offset from the content's top edge.
struct TooltipRow {
    const TooltipLine* line;
    std::string text;
    double y;
};

struct TooltipLayout {
    std::vector<TooltipRow> rows;
    double textWidth = 0;
    double textHeight = 0;
};

/// One tooltip row: an OPAQUE outline under a fill that carries the row's
/// alpha.
///
/// ui::text() fills at whatever alpha the canvas is on, so dimming a stat row
/// with globalAlpha would fade its black outline too. The browser passes the
/// alpha in the fill colour alone -- TOOLTIP_STAT_COLOR is
/// rgba(255,255,255,0.56) -- while drawText strokes with solid #000000, which
/// is why those rows stay crisply outlined over the half-black box.
///
/// It builds the path itself because that is the only seam where the two
/// passes can differ. Every row goes through here, dimmed or not, so one
/// anchor governs the whole box; `x, y` is the top-left corner, the only
/// alignment the painter ever asks for, resolved exactly as draw.cpp resolves
/// Baseline::Top.
void paintRow(Canvas& canvas, const std::string& s, double x, double y, const TextStyle& style,
              double fillAlpha) {
    paintRun(canvas, s, x, y + ascent(style.size, style.bold), style, 1.0, fillAlpha);
}

/// Resolves alt variants, wraps long lines and stacks the result top-down.
///
/// Measurement and painting both run this, rather than each doing its own
/// arithmetic: a wrap that the box did not account for is a tooltip whose text
/// hangs out of its own background.
TooltipLayout layoutRows(const std::vector<TooltipLine>& lines, bool alt) {
    TooltipLayout out;
    double y = 0;
    for (const TooltipLine& line : lines) {
        const std::string& body = (alt && !line.altText.empty()) ? line.altText : line.text;
        y += line.gapBefore;

        // Greedy wrap on spaces, as the browser does. A single word wider than
        // the limit still gets its own row and overflows -- breaking mid-word
        // would be worse to read than a slightly wide box.
        std::vector<std::string> pieces;
        if (line.maxWidth > 0 && measure(body, line.size, line.bold) > line.maxWidth) {
            std::string current;
            std::size_t at = 0;
            while (at <= body.size()) {
                const std::size_t space = body.find(' ', at);
                const std::string word = body.substr(at, space == std::string::npos
                                                             ? std::string::npos
                                                             : space - at);
                const std::string candidate = current.empty() ? word : current + " " + word;
                if (!current.empty() && measure(candidate, line.size, line.bold) > line.maxWidth) {
                    pieces.push_back(current);
                    current = word;
                } else {
                    current = candidate;
                }
                if (space == std::string::npos) break;
                at = space + 1;
            }
            if (!current.empty()) pieces.push_back(current);
        }
        if (pieces.empty()) pieces.push_back(body);

        for (std::string& piece : pieces) {
            out.textWidth = std::max(out.textWidth, measure(piece, line.size, line.bold));
            out.rows.push_back({&line, std::move(piece), y});
            y += rowHeight(line.size) + kTooltipLineGap;
        }
    }
    out.textHeight = std::max(0.0, y - kTooltipLineGap);
    return out;
}

} // namespace

bool TooltipDelay::update(int index, double timeSeconds, bool pointerDown) {
    if (index != hovered) {
        hovered = index;
        since = timeSeconds;
        suppressed = false;
    }
    // A press cancels the tooltip and keeps it cancelled until the pointer
    // moves on: the browser clears the pending timer on mousedown and never
    // re-arms it for the cell being clicked or dragged.
    if (pointerDown) suppressed = true;
    if (index < 0 || suppressed) return false;
    return timeSeconds - since >= kDelaySeconds;
}

Vec2 measureTooltip(const std::vector<TooltipLine>& lines, double minWidth, double extraHeight,
                    bool alt) {
    const TooltipLayout layout = layoutRows(lines, alt);
    return {std::max(layout.textWidth, minWidth) + kTooltipPadX * 2,
            layout.textHeight + extraHeight + kTooltipPadY * 2};
}

Rect paintTooltip(Canvas& canvas, double x, double y, const std::vector<TooltipLine>& lines,
                  double minWidth, double extraHeight, bool alt) {
    const TooltipLayout layout = layoutRows(lines, alt);
    const Vec2 size{std::max(layout.textWidth, minWidth) + kTooltipPadX * 2,
                    layout.textHeight + extraHeight + kTooltipPadY * 2};

    // Scoped, because the painter is called from the middle of a panel's own
    // draw: leaving the half-black fill and the round join set behind would
    // restyle whatever the panel draws next.
    canvas.save();

    // Half-black, no border. The box has to sit over arbitrary panel colours
    // and over the world; a border would fight whichever it lands on.
    setFill(canvas, kInk, 0.5);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(x), static_cast<float>(y), static_cast<float>(size.x),
                     static_cast<float>(size.y), static_cast<float>(kTooltipRadius));
    canvas.fill();

    for (const TooltipRow& row : layout.rows) {
        // Outline width tracks the font size (2.4px at 20, 1.44px at 12); a
        // flat 3px swallows the body rows and reads as a different typeface.
        // No align or baseline: paintRow only knows top-left, which is the
        // only anchor a tooltip row has ever been drawn at.
        TextStyle style = labelStyle(row.line->size, row.line->bold, row.line->color, -1.0);
        style.roundJoin = true;
        paintRow(canvas, row.text, x + kTooltipPadX, y + kTooltipPadY + row.y, style,
                 row.line->alpha);
    }
    canvas.restore();

    const double textBottom = y + kTooltipPadY + layout.textHeight;
    return {x + kTooltipPadX, textBottom, size.x - kTooltipPadX * 2,
            y + size.y - kTooltipPadY - textBottom};
}

Vec2 tooltipAnchor(Vec2 cursor, Vec2 size, double viewWidth, double viewHeight) {
    double x = cursor.x + 16.0;
    double y = cursor.y + 16.0;
    // Flip rather than clamp: a box pinned against the right edge covers the
    // cell it is describing, which is the one thing it must not do.
    if (x + size.x > viewWidth - 8.0) x = cursor.x - size.x - 12.0;
    if (y + size.y > viewHeight - 8.0) y = viewHeight - size.y - 8.0;
    return {std::max(8.0, x), std::max(8.0, y)};
}

Vec2 tooltipAnchor(Rect anchor, Vec2 size, double viewWidth, double viewHeight) {
    double x = anchor.right() + 10.0;
    if (x + size.x > viewWidth) x = anchor.x - size.x - 10.0;
    double y = anchor.y;
    if (y + size.y > viewHeight) y = viewHeight - size.y - 10.0;
    if (y < 0) y = 10.0;
    return {x, y};
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::string titleCase(const std::string& id) {
    std::string out;
    bool capitalise = true;
    for (const unsigned char raw : id) {
        if (raw == '_' || raw == ' ' || raw == '-') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            capitalise = true;
            continue;
        }
        out += static_cast<char>(capitalise ? std::toupper(raw) : std::tolower(raw));
        capitalise = false;
    }
    return out;
}

std::string abbreviate(double value) {
    if (!std::isfinite(value)) return "inf";
    const double magnitude = std::fabs(value);
    char buffer[32];
    if (magnitude < 1000.0) {
        std::snprintf(buffer, sizeof buffer, "%.0f", value);
    } else if (magnitude < 1e6) {
        std::snprintf(buffer, sizeof buffer, "%.1fK", value / 1e3);
    } else if (magnitude < 1e9) {
        std::snprintf(buffer, sizeof buffer, "%.1fM", value / 1e6);
    } else if (magnitude < 1e12) {
        std::snprintf(buffer, sizeof buffer, "%.1fB", value / 1e9);
    } else {
        std::snprintf(buffer, sizeof buffer, "%.1eT", value / 1e12);
    }
    std::string out = buffer;
    // "1.0k" reads worse than "1k", and the trailing zero is never news.
    const std::size_t dot = out.find(".0");
    if (dot != std::string::npos) out.erase(dot, 2);
    return out;
}

std::string withSeparators(double value) {
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.0f", std::fabs(value));
    std::string digits = buffer;
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return value < 0 ? "-" + out : out;
}

std::string ellipsize(const std::string& text, double size, bool bold, double width) {
    if (measure(text, size, bold) <= width) return text;
    std::string out = text;
    while (!out.empty() && measure(out + "...", size, bold) > width) out.pop_back();
    return out + "...";
}

} // namespace flix::ui
