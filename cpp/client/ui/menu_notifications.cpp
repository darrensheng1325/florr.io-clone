// The notifications overlay: server notices, invites and claimed rewards.
//
// Sized and anchored like the changelog -- a fixed 600x500 under the top icon
// row -- because in the browser build the two are the same panel with a
// different body.
//
// The feed comes from NetClient, which asks for it a page at a time. The
// browser fetches GET /api/notifications; this client has no HTTP, so the same
// query is a socket opcode with the same two parameters, and the answer lands
// in the same shape. What the panel owns is only whether THIS opening has
// already asked for its first page -- the entries, the in-flight flag and
// whether an older page exists all live on the connection.
//
// Read marks are the one piece of state with nowhere else to go: the browser
// keeps them in localStorage, and the nearest thing here is the settings file,
// so ClientSettings carries them and this panel is their only writer.

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "client/ui/text.h"

namespace flix {

using namespace flix::ui;

namespace {

constexpr double kPadding = 20.0;
/// Chrome above the scrolling body: the title row and the header buttons.
constexpr double kHeaderHeight = 40.0;
constexpr double kScrollbarWidth = 10.0;
constexpr double kMessageSize = 14.0;
/// 14px of type on an 18px pitch, and 50px of card around however many lines
/// that comes to.
constexpr double kLinePitch = 18.0;
constexpr double kCardChrome = 50.0;
constexpr double kMinCardHeight = 70.0;
constexpr double kCardGap = 10.0;
/// Text is inset this far from the card's left edge, and the same amount is
/// taken off the right, which is where the "- 20" in the wrap width comes from.
constexpr double kTextInset = 10.0;
/// One wheel notch is ~100px of deltaY in a browser, and this panel consumes
/// that unscaled.
constexpr double kWheelStep = 100.0;

/// The card's own colours come from kNotificationsSkin; this is the border
/// shade reused for the header pill, a stripe and the scrollbar thumb.
constexpr std::uint32_t kBodyBorder = kNotificationsSkin.border;

/// How many entries one page asks for. The browser's page size, and the value
/// the server compares against to decide whether there is more.
constexpr int kPageSize = 50;

/// How close to the end a scroll has to come before the next page is asked
/// for. In pixels of remaining scroll, as the reference measures it.
constexpr double kPagePrefetch = 100.0;

/// What is genuinely the panel's own between frames.
///
/// The entries are not here: they belong to the connection, and a second copy
/// would be a second thing to keep true. `read` is a mirror of the settings
/// file's list, kept as a set so a card's read test is a lookup rather than a
/// scan of the whole history once per card per frame.
struct FeedState {
    bool requested = false;
    std::set<std::string> read;
    /// How many ids the mirror was built from. This panel is the only writer
    /// of that list, so a change in its length is the only way it can differ.
    std::size_t mirrored = 0;
};

FeedState& feedState() {
    static FeedState state;
    return state;
}

/// Thumb-drag state for the scrollbar.
///
/// At file scope because it lives entirely between one press and the release
/// that ends it, and because the panel's own declaration sits in a header
/// twelve panels share.
struct ThumbDrag {
    bool active = false;
    double startY = 0;
    double startOffset = 0;
};

ThumbDrag& thumbDrag() {
    static ThumbDrag drag;
    return drag;
}

std::uint32_t stripeColor(const NotificationEntry& notice, bool isRead) {
    // Assignment order matters and is the reference's: unread paints gold, and
    // then the kind overrides it unconditionally -- so a READ super craft is
    // still green, and an unread generic notice is the only gold one.
    std::uint32_t color = kBodyBorder;
    if (!isRead) color = 0xFFD700u;
    switch (notice.kind) {
        case net::NotificationKind::SuperCraft: color = 0x2BFFA4u; break;
        case net::NotificationKind::UniqueCraft: color = 0xBF00FFu; break;
        case net::NotificationKind::ApexCraft: color = 0xFF00FFu; break;
        case net::NotificationKind::StarCode: color = 0xFFD700u; break;
        case net::NotificationKind::Generic: break;
    }
    return color;
}

/// Splits one wrapped line on any newline it still contains.
///
/// The reference wraps on SPACES only and then joins its lines with '\n', and
/// the caller splits that string again -- so an embedded newline the wrap
/// never looked at still ends up starting a line, and still costs the card
/// 18px of height. Reproducing that means splitting after the wrap, not before
/// it: splitting first would let each fragment fill the line width on its own
/// and give a different line count.
void appendSplitOnNewlines(std::vector<std::string>& lines, const std::string& line) {
    std::size_t at = 0;
    for (;;) {
        const std::size_t end = line.find('\n', at);
        if (end == std::string::npos) {
            lines.push_back(line.substr(at));
            return;
        }
        lines.push_back(line.substr(at, end - at));
        at = end + 1;
    }
}

/// Greedy word wrap at 14px. A first word wider than the line is emitted on
/// its own and allowed to overflow, which is what the reference does rather
/// than breaking mid-word.
std::vector<std::string> wrapMessage(const std::string& message, double maxWidth) {
    std::vector<std::string> lines;
    if (message.find_first_not_of(" \t\r\n") == std::string::npos) {
        // A blank message still occupies one line's worth of card.
        lines.emplace_back();
        return lines;
    }
    if (maxWidth <= 0 || measure(message, kMessageSize) <= maxWidth) {
        appendSplitOnNewlines(lines, message);
        return lines;
    }

    std::vector<std::string> words;
    for (std::size_t at = 0; at < message.size();) {
        const std::size_t end = message.find(' ', at);
        const std::string word =
            message.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!word.empty()) words.push_back(word);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    if (words.empty()) {
        lines.emplace_back();
        return lines;
    }
    if (measure(words.front(), kMessageSize) > maxWidth) {
        appendSplitOnNewlines(lines, words.front());
        return lines;
    }

    std::string current = words.front();
    for (std::size_t i = 1; i < words.size(); ++i) {
        const std::string candidate = current + " " + words[i];
        if (measure(candidate, kMessageSize) <= maxWidth) {
            current = candidate;
        } else {
            appendSplitOnNewlines(lines, current);
            current = words[i];
        }
    }
    appendSplitOnNewlines(lines, current);
    return lines;
}

double cardHeight(std::size_t lineCount) {
    return std::max(kMinCardHeight,
                    kCardChrome + static_cast<double>(lineCount) * kLinePitch);
}

double cardHeight(const std::string& message, double maxTextWidth) {
    return cardHeight(wrapMessage(message, maxTextWidth).size());
}

/// "3 days ago" / "Just now". Plural only past one, as the reference's
/// `days > 1 ? 's' : ''` gives.
std::string timeAgo(double stampMillis, double nowMillis) {
    const long long seconds = static_cast<long long>((nowMillis - stampMillis) / 1000.0);
    const long long minutes = seconds / 60;
    const long long hours = minutes / 60;
    const long long days = hours / 24;
    const auto phrase = [](long long value, const char* unit) {
        return std::to_string(value) + " " + unit + (value > 1 ? "s ago" : " ago");
    };
    if (days > 0) return phrase(days, "day");
    if (hours > 0) return phrase(hours, "hour");
    if (minutes > 0) return phrase(minutes, "minute");
    return "Just now";
}

double nowMillis() {
    using namespace std::chrono;
    return static_cast<double>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

/// White at an alpha, which TextStyle cannot express -- it carries a colour,
/// not a coverage -- and which a pre-mixed grey would get wrong over a
/// saturated panel.
void dimText(Canvas& canvas, const std::string& s, double x, double y, const TextStyle& style,
             double alpha) {
    canvas.setGlobalAlpha(static_cast<float>(alpha));
    text(canvas, s, x, y, style);
    canvas.setGlobalAlpha(1.0f);
}

} // namespace

double NotificationsPanel::preferredWidth() { return 600.0; }

void NotificationsPanel::reset() {
    scroll_ = {};
    thumbDrag() = {};
    // The reference's show() re-fetches every time the panel opens, so an
    // opening is what arms the first request rather than a first frame.
    feedState().requested = false;
}

bool NotificationsPanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Rect panel = ctx.bounds;
    const Vec2 mouse = ctx.mouse();
    FeedState& state = feedState();

    if (!state.requested) {
        state.requested = true;
        ctx.net.requestNotifications(kPageSize, 0);
    }

    const std::vector<NotificationEntry>& entries = ctx.net.notifications();
    const bool loading = ctx.net.notificationsPending();
    const bool hasMore = ctx.net.notificationsHaveMore();

    // The settings file's list is the record; this set is only a lookup over
    // it, rebuilt when the list has grown.
    std::vector<std::string>& readIds = ctx.settings.readNotifications;
    if (state.mirrored != readIds.size()) {
        state.read.clear();
        state.read.insert(readIds.begin(), readIds.end());
        state.mirrored = readIds.size();
    }
    const auto markRead = [&](const std::string& id) {
        if (!state.read.insert(id).second) return;
        readIds.push_back(id);
        state.mirrored = readIds.size();
    };

    const double viewport = panel.h - kHeaderHeight;
    const double wideText = panel.w - kPadding * 2 - kTextInset * 2;
    const double narrowText = wideText - (kScrollbarWidth + 5.0);

    // Measured twice, exactly as the reference measures it: a list that needs
    // a scrollbar wraps against a narrower card, which can add lines and so
    // change the answer the first pass gave.
    double contentHeight = 40.0;
    if (!entries.empty() || loading) {
        const auto sum = [&](double textWidth) {
            double total = 0;
            for (const NotificationEntry& notice : entries) {
                // The measuring pass counts the card and NOT the 10px between
                // two of them, while the draw pass advances by both. The list
                // therefore stops 10px per entry short of its own end. That is
                // the reference's arithmetic and the panel is compared against
                // it, so it is reproduced rather than corrected.
                total += cardHeight(notice.message, textWidth);
            }
            if (hasMore) total += 40.0;
            return total;
        };
        contentHeight = sum(wideText);
        if (contentHeight > viewport) contentHeight = sum(narrowText);
    }

    scroll_.contentHeight = contentHeight;
    scroll_.viewHeight = viewport;
    const double maxScroll = scroll_.maxOffset();
    const bool scrollable = contentHeight > viewport;

    const Rect track{panel.right() - kScrollbarWidth - 5.0, panel.y + kHeaderHeight,
                     kScrollbarWidth, panel.h - kHeaderHeight - 5.0};

    // The wheel is INVERTED here, as it is in the changelog: a notch DOWN
    // walks the list back toward the newest notice.
    const bool wheeled = ctx.wheel() != 0.0f && panel.contains(mouse);
    if (wheeled) scroll_.offset += ctx.wheel() * kWheelStep;

    ThumbDrag& drag = thumbDrag();
    if (ctx.pressed() && scrollable && track.contains(mouse)) {
        drag.active = true;
        drag.startY = mouse.y;
        drag.startOffset = scroll_.offset;
    }
    if (!ctx.window.mouseDown(MouseButton::Left)) drag.active = false;
    if (drag.active) {
        const double ratio = (mouse.y - drag.startY) / (panel.h - 45.0);
        scroll_.offset = drag.startOffset + ratio * maxScroll;
    }
    scroll_.offset = clamp(scroll_.offset, 0.0, maxScroll);

    // Paging happens on the wheel and only on the wheel, as it does in the
    // reference: dragging the thumb to the foot of the list loads nothing.
    if (wheeled && hasMore && !loading && !entries.empty() &&
        scroll_.offset >= maxScroll - kPagePrefetch) {
        ctx.net.requestNotifications(kPageSize, entries.back().timestampMillis);
    }

    overlayCard(canvas, panel, kNotificationsSkin);

    TextStyle heading;
    heading.size = 20.0;
    heading.bold = true;
    heading.fill = kPaper;
    heading.strokeWidth = 2.0;
    heading.baseline = Baseline::Top;
    text(canvas, "Notifications", panel.x + kPadding, panel.y + kPadding, heading);

    // Neither header button has a hover or a press state in the reference.
    const Rect markAllRect{panel.right() - 180.0, panel.y + 10.0, 120.0, 30.0};
    pillButton(canvas, markAllRect, "Mark All Read", kBodyBorder, 14.0);
    const Rect closeRect = closeButtonRect(panel);
    panelClose(canvas, closeRect, closeRect.contains(mouse));

    const Rect view{panel.x + kPadding, panel.y + kHeaderHeight, panel.w - kPadding * 2,
                    panel.h - kHeaderHeight - kPadding};

    canvas.save();
    roundPath(canvas, view, 8.0);
    canvas.clip();

    const double entryWidth = view.w - (scrollable ? kScrollbarWidth + 5.0 : 0.0);
    const double maxTextWidth = entryWidth - kTextInset * 2;
    double contentY = view.y + kPadding - scroll_.offset;
    std::vector<std::pair<Rect, std::string>> cards;

    TextStyle faint;
    faint.size = kMessageSize;
    faint.fill = kPaper;
    faint.strokeWidth = 0;

    if (entries.empty() && !loading) {
        TextStyle empty = faint;
        empty.align = Align::Centre;
        dimText(canvas, "No notifications yet", panel.x + panel.w * 0.5, contentY + 20.0, empty,
                0.7);
    } else {
        const double now = nowMillis();
        for (const NotificationEntry& notice : entries) {
            const bool isRead = state.read.count(notice.id) != 0;
            const std::vector<std::string> lines = wrapMessage(notice.message, maxTextWidth);
            const double height = cardHeight(lines.size());
            const Rect card{view.x, contentY, entryWidth, height};
            cards.emplace_back(card, notice.id);

            if (card.bottom() >= view.y && card.y <= view.bottom()) {
                setFill(canvas, kPaper, isRead ? 0.1 : 0.15);
                roundPath(canvas, card, 8.0);
                canvas.fill();

                // A square-cornered stripe laid over the rounded card, so the
                // colour reaches the card's true top and bottom edges.
                setFill(canvas, stripeColor(notice, isRead));
                canvas.fillRect(static_cast<float>(card.x), static_cast<float>(card.y), 4.0f,
                                static_cast<float>(card.h));

                TextStyle body;
                body.size = kMessageSize;
                body.fill = kPaper;
                body.strokeWidth = 0.5;
                double lineY = card.y + kTextInset;
                for (const std::string& line : lines) {
                    // A wrap that produced an empty line still costs its pitch.
                    if (line.find_first_not_of(" \t\r\n") != std::string::npos) {
                        text(canvas, line, card.x + kTextInset, lineY, body);
                    }
                    lineY += kLinePitch;
                }

                TextStyle stamp = faint;
                stamp.size = 12.0;
                dimText(canvas, timeAgo(notice.timestampMillis, now), card.x + kTextInset,
                        card.bottom() - 20.0, stamp, 0.7);
            }
            contentY += height + kCardGap;
        }

        if (hasMore) {
            TextStyle footer = faint;
            footer.align = Align::Centre;
            dimText(canvas, loading ? "Loading..." : "Scroll for more",
                    panel.x + panel.w * 0.5, contentY + 10.0, footer, 0.7);
        }
    }
    canvas.restore();

    if (scrollable) {
        setFill(canvas, kPaper, 0.1);
        roundPath(canvas, track, 5.0);
        canvas.fill();
        // No minimum thumb height, as the reference clamps none.
        const double thumbHeight = (viewport - 5.0) * viewport / contentHeight;
        const double travelled = maxScroll > 0 ? scroll_.offset / maxScroll : 0.0;
        setFill(canvas, kBodyBorder);
        roundPath(canvas,
                      Rect{track.x, track.y + travelled * (track.h - thumbHeight), track.w,
                           thumbHeight},
                      5.0);
        canvas.fill();
    }

    if (!ctx.released()) return true;
    if (closeRect.contains(mouse)) return false;
    if (markAllRect.contains(mouse)) {
        // Every LOADED id, not every id the server holds: the reference marks
        // what it has, and a page never fetched was never shown as unread.
        for (const NotificationEntry& notice : entries) markRead(notice.id);
        return true;
    }
    // Card rects are recorded unclipped, exactly as the reference records
    // them, so a card scrolled out of the body still answers for a click.
    for (const std::pair<Rect, std::string>& card : cards) {
        if (card.first.contains(mouse)) {
            markRead(card.second);
            break;
        }
    }
    return true;
}

} // namespace flix
