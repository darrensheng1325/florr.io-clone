// Account rankings.
//
// Ranked over accounts rather than over who happens to be online, so the board
// is a record of progress and not a snapshot of the lobby. The server answers
// on request; the panel asks once when it opens and again only when told to.

#include <algorithm>
#include <cstdio>
#include <string>

#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "client/ui/text.h"

namespace flix {

using namespace flix::ui;

namespace {

/// The card is a fixed 500x500 pinned under the top icon row, and every metric
/// here is measured off it rather than off the window.
constexpr double kHeaderHeight = 50.0;   ///< chrome above the scrolling body
constexpr double kRowHeight = 40.0;
constexpr double kColumnHeader = 30.0;
constexpr double kScrollbarWidth = 10.0;
/// The scrolling body and the track beside it stop this far short of the
/// card's bottom edge, clear of its rounded corners.
constexpr double kTrackBottomInset = 5.0;
/// A thumb drag maps the pointer over the card height less this, which is the
/// two insets the track sits between rather than the track's own length.
constexpr double kThumbTravelInset = 45.0;
/// A wheel notch carries no pixel count of its own here, but the browser panel
/// scrolls by the wheel event's raw delta -- ~100px a notch in Chrome -- so
/// the distance is spelled out to land on the same place.
constexpr double kWheelStep = 100.0;

/// Gold, silver, bronze. The only three rows that get a colour, because a
/// board where every row is coloured has no podium.
std::uint32_t rankColor(int rank) {
    switch (rank) {
        case 1: return 0xFFD700u;
        case 2: return 0xC0C0C0u;
        case 3: return 0xCD7F32u;
        default: return 0xFFFFFFu;
    }
}

/// Text drawn as translucent white rather than in a pre-mixed colour, so it
/// tints whatever it lands on: the header row over the bare card, the XP
/// column over a row band. TextStyle carries no alpha of its own.
void fadedText(Canvas& canvas, const std::string& s, double x, double y, const TextStyle& style,
               double alpha) {
    canvas.setGlobalAlpha(static_cast<float>(alpha));
    text(canvas, s, x, y, style);
    canvas.setGlobalAlpha(1.0f);
}

/// The board's own number format: tenths of a K or an M, and nothing above.
/// `abbreviate` is nearly this but lowercases the K, erases a trailing ".0"
/// and has B and T tiers, so the same total would read differently here than
/// it does in the browser.
std::string formatXp(double xp) {
    char buffer[32];
    if (xp >= 1e6) std::snprintf(buffer, sizeof buffer, "%.1fM", xp / 1e6);
    else if (xp >= 1e3) std::snprintf(buffer, sizeof buffer, "%.1fK", xp / 1e3);
    else std::snprintf(buffer, sizeof buffer, "%.0f", xp);
    return buffer;
}

/// Trimmed by character count, not by pixel width: a name that overflows its
/// column has to overflow it in both clients, or the two boards disagree about
/// which names are cut.
std::string displayName(const std::string& name) {
    return name.size() > 20 ? name.substr(0, 17) + "..." : name;
}

/// Where a scrollbar drag started. This belongs on LeaderboardPanel beside the
/// scroller, but menus.h is not this file's to change; there is exactly one
/// panel instance, so one copy here is that state under another name.
struct BarDrag {
    bool active = false;
    double anchorY = 0;
    double anchorScroll = 0;
};

BarDrag& barDrag() {
    static BarDrag drag;
    return drag;
}

} // namespace

double LeaderboardPanel::preferredWidth() { return 500.0; }

void LeaderboardPanel::reset() {
    scroll_ = {};
    requested_ = false;
    barDrag() = {};
}

bool LeaderboardPanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Rect panel = ctx.bounds;
    const Vec2 mouse = ctx.mouse();

    // Asked for on the way in rather than every frame: the server walks its
    // whole account table to answer, and the board does not move that fast.
    if (!requested_) {
        requested_ = true;
        ctx.net.requestLeaderboard();
    }

    const std::vector<LeaderboardRow>& rows = ctx.net.leaderboard();
    const bool pending = ctx.net.leaderboardPending();

    const Rect closeRect = closeButtonRect(panel);
    const Rect refreshRect{panel.right() - 140.0, panel.y + 10.0, 80.0, 30.0};
    const Rect view{panel.x + 5.0, panel.y + kHeaderHeight, panel.w - 10.0,
                    std::max(0.0, panel.h - kHeaderHeight - kTrackBottomInset)};
    const Rect bar{view.right() - kScrollbarWidth, view.y, kScrollbarWidth, view.h};

    // An empty board still reserves a row's worth of height, which is what
    // keeps its one line of prose from being the only thing that can scroll.
    scroll_.contentHeight = rows.empty() && !pending
                                ? kRowHeight
                                : kColumnHeader + static_cast<double>(rows.size()) * kRowHeight;
    // Clamped against the header-to-bottom span, not against the clipped view:
    // the two differ by the track inset, and at the end of a long board that
    // last 5px of content sits under the card's rounded edge.
    scroll_.viewHeight = panel.h - kHeaderHeight;
    const bool scrollable = scroll_.contentHeight > scroll_.viewHeight;

    // The reference SUBTRACTS the wheel delta from its scroll offset, so a
    // wheel-down there walks the board back toward rank 1 rather than on to
    // rank 50. Folded in here because the shared Scroller applies a wheel the
    // other way round, at its own step, and the other panels want that.
    if (panel.contains(mouse)) scroll_.offset += ctx.wheel() * kWheelStep;

    // Press, not release: the browser panel acts on mousedown, and a press
    // that reaches the track is one the two buttons did not take.
    BarDrag& drag = barDrag();
    bool closing = false;
    if (ctx.pressed()) {
        if (closeRect.contains(mouse)) {
            closing = true;
        } else if (refreshRect.contains(mouse)) {
            ctx.net.requestLeaderboard();
        } else if (scrollable && bar.contains(mouse)) {
            drag.active = true;
            drag.anchorY = mouse.y;
            drag.anchorScroll = scroll_.offset;
        }
    }
    if (!ctx.window.mouseDown(MouseButton::Left)) drag.active = false;
    if (drag.active) {
        // Anywhere in the track column starts the drag and the content follows
        // the pointer from there; the thumb is never snapped under the cursor.
        const double ratio = (mouse.y - drag.anchorY) / (panel.h - kThumbTravelInset);
        scroll_.offset = drag.anchorScroll + ratio * scroll_.maxOffset();
    }
    scroll_.offset = clamp(scroll_.offset, 0.0, scroll_.maxOffset());

    if (closing) return false;

    overlayCard(canvas, panel, kLeaderboardSkin);

    panelHeading(canvas, panel, "Leaderboard");

    // The count of every account the server holds, beside the title. It comes
    // with the board rather than being derived from it: the board is the top
    // 25 and this is the whole table. The active-today half only ever arrives
    // for an admin, which is how the browser's payload omits the field.
    if (ctx.net.totalAccounts() > 0) {
        TextStyle stats;
        stats.size = 13.0;
        stats.baseline = Baseline::Top;
        stats.strokeWidth = 0.0;
        std::string line = std::to_string(ctx.net.totalAccounts()) + " accounts";
        if (ctx.net.dailyActiveUsers() > 0) {
            // U+00B7, a middle dot, not a hyphen.
            line += " · " + std::to_string(ctx.net.dailyActiveUsers()) + " active today";
        }
        fadedText(canvas, line, panel.x + 160.0, panel.y + 25.0, stats, 0.7);
    }

    pillButton(canvas, refreshRect, "Refresh", kLeaderboardSkin.border);
    panelClose(canvas, closeRect, closeRect.contains(mouse));

    canvas.save();
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(view.x), static_cast<float>(view.y),
                     static_cast<float>(view.w), static_cast<float>(view.h), 8.0f);
    canvas.clip();

    double y = view.y - scroll_.offset;

    if (rows.empty()) {
        TextStyle empty;
        empty.size = 14.0;
        empty.align = Align::Centre;
        empty.baseline = Baseline::Top;
        empty.strokeWidth = 0.0;
        fadedText(canvas, pending ? "Loading..." : "No accounts found",
                  panel.x + panel.w * 0.5, y + 20.0, empty, 0.7);
    } else {
        const double rankX = panel.x + 15.0;
        const double nameX = panel.x + 60.0;
        const double levelX = panel.right() - 170.0;
        const double xpX = panel.right() - 30.0;

        TextStyle header;
        header.size = 14.0;
        header.bold = true;
        header.baseline = Baseline::Top;
        header.strokeWidth = 0.0;
        fadedText(canvas, "#", rankX, y + 8.0, header, 0.8);
        fadedText(canvas, "Player", nameX, y + 8.0, header, 0.8);
        fadedText(canvas, "Level", levelX, y + 8.0, header, 0.8);
        header.align = Align::Right;
        fadedText(canvas, "XP", xpX, y + 8.0, header, 0.8);
        y += kColumnHeader;

        for (std::size_t i = 0; i < rows.size(); ++i) {
            const Rect row{panel.x + 10.0, y, panel.w - 20.0, kRowHeight};
            y += kRowHeight;
            if (row.bottom() < view.y || row.y > view.bottom()) continue;

            // Alternating bands. Both are white at low alpha rather than one
            // being the panel colour, so the stripe survives any panel hue.
            setFill(canvas, kPaper, (i % 2 == 0) ? 0.15 : 0.08);
            canvas.fillRect(static_cast<float>(row.x), static_cast<float>(row.y),
                            static_cast<float>(row.w), static_cast<float>(row.h));

            const int rank = static_cast<int>(i) + 1;
            const double middle = row.y + kRowHeight * 0.5;

            // The rank carries a real outline and the other three a hairline:
            // the browser build's row is one loud number and three quiet ones.
            TextStyle cell;
            cell.size = 16.0;
            cell.bold = true;
            cell.fill = rankColor(rank);
            cell.strokeWidth = 1.0;
            text(canvas, std::to_string(rank), rankX, middle, cell);

            cell.size = 14.0;
            cell.bold = false;
            cell.strokeWidth = 0.5;
            text(canvas, displayName(rows[i].name), nameX, middle, cell);

            // The level travels with the row; the panel does not re-derive it
            // from XP with a curve that could drift from the server's.
            cell.bold = true;
            cell.fill = kPaper;
            text(canvas, std::to_string(rows[i].level), levelX, middle, cell);

            cell.align = Align::Right;
            cell.bold = false;
            cell.size = 13.0;
            fadedText(canvas, formatXp(rows[i].totalXp), xpX, middle, cell, 0.8);
        }
    }
    canvas.restore();

    if (scrollable) {
        scrollbar(canvas, view, scroll_.contentHeight, scroll_.offset, kLeaderboardSkin.accent,
                  kScrollbarWidth);
    }
    return true;
}

} // namespace flix
