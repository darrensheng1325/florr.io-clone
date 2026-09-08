// The guild overlay: the player's guild and its roster, or the join/create
// form when they are in none.
//
// A 500x500 card under the top icon row, in the same cyan the strip's guild
// button is -- the colour is how a player tells the overlays apart before they
// read a title.
//
// The roster and the pending invitation both come from NetClient: the server
// pushes a GuildUpdate to every member whenever one changes, and a
// GuildInviteReceived to whoever was invited. Nothing here caches either, so
// there is only ever one answer to "what guild am I in".
//
// The two dialogs the reference raises are native browser chrome -- a floating
// <input> for create/invite/kick and window.confirm() for leave/kick -- and
// this client has no DOM to raise them in. They are drawn on the canvas
// instead, at the viewport's centre, carrying the reference's own literal
// strings and field limits.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "client/ui/text.h"
#include "client/ui/text_input.h"

namespace flix {

using namespace flix::ui;

namespace {

/// Chrome above the scrolling body. Taller than the overlay panels' 40 because
/// this card carries a 22px title rather than a 20px one.
constexpr double kHeaderHeight = 54.0;
/// The card's frame, shared with every other overlay panel.
constexpr double kBorderWidth = kOverlayBorder;
constexpr double kRowHeight = 34.0;
/// The card behind one member: a row's pitch less the gap under it.
constexpr double kRowCard = kRowHeight - 4.0;
constexpr double kScrollbarWidth = 10.0;
/// One wheel notch is ~100px of deltaY in a browser, which this panel consumes
/// unscaled -- in the CONVENTIONAL direction, unlike its two neighbours.
constexpr double kWheelStep = 100.0;

constexpr std::uint32_t kBodyFill = 0x27DADEu;
constexpr std::uint32_t kBodyBorder = 0x1FB3B0u;
constexpr std::uint32_t kDangerFrame = 0xB71C1Cu;
constexpr std::uint32_t kAcceptLabel = 0x4CAF50u;
constexpr std::uint32_t kAcceptFrame = 0x2E7D32u;
constexpr std::uint32_t kDeclineLabel = 0xFF5252u;
constexpr std::uint32_t kOnlineDot = 0x7DFF7Du;
constexpr std::uint32_t kOfflineDot = 0x888888u;
/// `background: #222` on the reference's floating input, and the same body for
/// the confirm card so the two dialogs read as one piece of chrome.
constexpr std::uint32_t kDialogBody = 0x222222u;
/// Chrome's own ::placeholder colour, which is `darkgray` and not a fade of
/// the text colour -- on a #222 field it is lighter than a 50% white would be.
constexpr std::uint32_t kPlaceholder = 0xA9A9A9u;

/// The floating input's box. `width: 260px` is the CONTENT box -- the default
/// box-sizing -- so the painted box is 260 plus 10px of padding and 2px of
/// border on each side. Its height is one line of Chrome's 13.33px input type
/// inside the same padding and border.
constexpr double kPromptWidth = 284.0;
constexpr double kPromptHeight = 35.0;
constexpr double kPromptTextSize = 13.33;
/// 2px of border plus 10px of padding: where the first glyph starts.
constexpr double kPromptInset = 12.0;

/// The confirm card. window.confirm() is native chrome with no metrics worth
/// copying, so this is the prompt's own frame at a size its two buttons and
/// one line of question fit in.
constexpr double kConfirmWidth = 320.0;
constexpr double kConfirmHeight = 108.0;

/// Which name a prompt is asking for. The reference raises a DOM <input> for
/// each of these three and nothing else.
enum class PromptMode : std::uint8_t { None, Create, Invite, Kick };

/// Which question a confirm is asking. Both are window.confirm() calls in the
/// reference, and both carry a fixed string.
enum class ConfirmMode : std::uint8_t { None, Leave, Kick };

/// The run the floating prompt paints. It does not scroll: the box holds more
/// than either of its two length limits.
TextRun promptRun(Rect box, const std::string& value) {
    TextRun run;
    run.text = value;
    run.originX = box.x + kPromptInset;
    run.size = kPromptTextSize;
    return run;
}

/// The one dialog that can be up, and what it is asking for.
///
/// At file scope for the same reason the drag state below is: there is exactly
/// one panel instance, and menus.h is a header twelve panels share.
struct Dialog {
    PromptMode prompt = PromptMode::None;
    std::string typed;
    /// Caret, selection and the blink's phase.
    ui::TextFieldState field;

    ConfirmMode confirm = ConfirmMode::None;
    /// Who a kick confirm is about, so the question can name them.
    std::string member;

    bool up() const { return prompt != PromptMode::None || confirm != ConfirmMode::None; }
    void clear() { *this = Dialog{}; }
};

Dialog& dialog() {
    static Dialog state;
    return state;
}

/// The reference's own two field limits: a guild name is five characters but
/// the input allows 32, and a username 24.
std::size_t promptLimit(PromptMode mode) { return mode == PromptMode::Create ? 32 : 24; }

const char* promptPlaceholder(PromptMode mode) {
    switch (mode) {
        case PromptMode::Create: return "Guild name";
        case PromptMode::Invite: return "Username to invite";
        case PromptMode::Kick: return "Username to kick";
        case PromptMode::None: break;
    }
    return "";
}

std::string trimmedText(const std::string& s) {
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
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

enum class GuildAction : std::uint8_t {
    None,
    Close,
    Create,
    Invite,
    Leave,
    SquadAll,
    AcceptInvite,
    DeclineInvite,
    Kick,
    SquadOne,
};

struct HitRegion {
    Rect rect;
    GuildAction action = GuildAction::None;
    std::string member;
};

std::string lowered(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool sameName(const std::string& a, const std::string& b) { return lowered(a) == lowered(b); }

/// A one-second cycle at an even duty, standing in for the native caret of the
/// <input> the reference floats here. Anchored to the last edit so typing
/// never blinks the caret out mid-keystroke.
/// The leader badge. Drawn rather than typed: the reference appends U+2605 to
/// the row's label and gets it from a fallback face, and Ubuntu -- the only
/// face this client loads -- would answer with its .notdef box.
void drawStar(Canvas& canvas, double cx, double cy, double radius) {
    setFill(canvas, kPaper);
    canvas.beginPath();
    for (int i = 0; i < 10; ++i) {
        const double reach = (i % 2 == 0) ? radius : radius * 0.382;
        const double angle = -kPi * 0.5 + i * kPi / 5.0;
        const float x = static_cast<float>(cx + std::cos(angle) * reach);
        const float y = static_cast<float>(cy + std::sin(angle) * reach);
        if (i == 0) {
            canvas.moveTo(x, y);
        } else {
            canvas.lineTo(x, y);
        }
    }
    canvas.closePath();
    canvas.fill();
}

/// White at an alpha, which TextStyle cannot express -- it carries a colour,
/// not a coverage -- and which a pre-mixed grey would get wrong over cyan.
void dimText(Canvas& canvas, const std::string& s, double x, double y, const TextStyle& style,
             double alpha) {
    canvas.setGlobalAlpha(static_cast<float>(alpha));
    text(canvas, s, x, y, style);
    canvas.setGlobalAlpha(1.0f);
}

} // namespace

double GuildPanel::preferredWidth() { return 500.0; }

void GuildPanel::reset() {
    scroll_ = {};
    thumbDrag() = {};
    // A dialog belongs to the opening that raised it: the reference's hide()
    // closes any prompt on the way out, so a reopened panel never inherits one.
    dialog().clear();
}

bool GuildPanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Rect panel = ctx.bounds;
    const Vec2 mouse = ctx.mouse();
    const GuildState& guild = ctx.net.guild();
    // Not const: answering an invitation clears the banner on the click, as
    // the reference drops `pendingInvite` before the server has replied.
    GuildInvite& pending = ctx.net.guildInvite();
    Dialog& modal = dialog();
    // A dialog owns the keyboard. Without this the menu system reads every
    // letter typed into the field as a hotkey and Escape as "close the guild".
    if (modal.up()) ctx.wantsText = true;
    const std::string me = ctx.net.profile().username;
    const bool leaderIsMe = guild.joined && sameName(guild.leader, me);

    const double visible = panel.h - kHeaderHeight;
    const double bodyTop = panel.y + kHeaderHeight;
    // This panel measures its body by drawing it, so the scroll bounds are
    // last frame's -- as they are in the reference, whose pointer handlers read
    // the contentHeight the previous render left behind.
    const double maxScroll = std::max(0.0, scroll_.contentHeight - visible);

    const Rect track{panel.right() - 15.0, bodyTop, kScrollbarWidth, visible - 10.0};
    // The grab zone runs 5px past the track's foot, which is where the
    // reference stops it. Not worth reconciling: it is the hit target, and the
    // track is only what gets painted.
    const Rect grab{track.x, track.y, track.w, visible - 5.0};

    // Conventional wheel direction, unlike the changelog and notifications
    // panels next to it -- the reference adds deltaY here and subtracts it
    // there, and the three are compared against the reference, not each other.
    if (panel.contains(mouse)) scroll_.offset -= ctx.wheel() * kWheelStep;

    ThumbDrag& drag = thumbDrag();
    const bool scrollable = scroll_.contentHeight > visible;
    if (ctx.pressed() && scrollable && grab.contains(mouse)) {
        drag.active = true;
        drag.startY = mouse.y;
        drag.startOffset = scroll_.offset;
    }
    if (!ctx.window.mouseDown(MouseButton::Left)) drag.active = false;
    if (drag.active) {
        const double ratio = (mouse.y - drag.startY) / std::max(1.0, visible - 5.0);
        scroll_.offset = drag.startOffset + ratio * maxScroll;
    }
    scroll_.offset = clamp(scroll_.offset, 0.0, maxScroll);

    overlayCard(canvas, panel, kGuildSkin);

    TextStyle heading;
    heading.size = 22.0;
    heading.bold = true;
    heading.fill = kPaper;
    heading.strokeWidth = 4.0;
    heading.baseline = Baseline::Top;
    heading.roundJoin = true;
    text(canvas, "Guild", panel.x + 16.0, panel.y + 14.0, heading);

    std::vector<HitRegion> regions;
    const Rect closeRect = closeButtonRect(panel);
    panelClose(canvas, closeRect, closeRect.contains(mouse));
    regions.push_back({closeRect, GuildAction::Close, {}});

    canvas.save();
    // A PLAIN rect, not a rounded one: the body's corners are square where the
    // other overlays' are rounded, and a rounded clip would shave the first
    // and last member rows.
    canvas.beginPath();
    canvas.rect(static_cast<float>(panel.x + kBorderWidth), static_cast<float>(bodyTop),
                static_cast<float>(panel.w - kBorderWidth * 2),
                static_cast<float>(panel.h - kHeaderHeight - kBorderWidth));
    canvas.clip();

    double contentY = bodyTop + 6.0 - scroll_.offset;
    // What the body would be if nothing were scrolled away. Tracked separately
    // from contentY because contentY is already offset by the scroll.
    double drawnY = 6.0;

    TextStyle body;
    body.size = 14.0;
    body.bold = true;
    body.fill = kPaper;
    body.strokeWidth = 0;
    body.baseline = Baseline::Top;

    if (pending.waiting && !guild.joined) {
        const double bannerHeight = 70.0;
        setFill(canvas, kInk, 0.25);
        roundPath(canvas, Rect{panel.x + 12.0, contentY, panel.w - 24.0, bannerHeight}, 6.0);
        canvas.fill();

        text(canvas, "@" + pending.fromUsername + " invited you to", panel.x + 22.0,
             contentY + 8.0, body);
        TextStyle name = body;
        name.size = 16.0;
        text(canvas, pending.guildName, panel.x + 22.0, contentY + 26.0, name);

        const double buttonW = 80.0;
        const double buttonH = 26.0;
        const Rect accept{panel.x + panel.w - 24.0 - buttonW * 2 - 6.0,
                          contentY + bannerHeight - buttonH - 8.0, buttonW, buttonH};
        framedButton(canvas, accept, "Accept", kAcceptLabel, kAcceptFrame, accept.contains(mouse));
        regions.push_back({accept, GuildAction::AcceptInvite, {}});

        const Rect decline{accept.x + buttonW + 6.0, accept.y, buttonW, buttonH};
        framedButton(canvas, decline, "Decline", kDeclineLabel, kDangerFrame,
                    decline.contains(mouse));
        regions.push_back({decline, GuildAction::DeclineInvite, {}});

        contentY += bannerHeight + 10.0;
        drawnY += bannerHeight + 10.0;
    }

    if (!guild.joined) {
        text(canvas, "You are not in a guild yet.", panel.x + 16.0, contentY, body);
        contentY += 20.0;

        TextStyle note;
        note.size = 13.0;
        note.fill = kPaper;
        note.strokeWidth = 0;
        note.baseline = Baseline::Top;
        dimText(canvas, "Guilds hold up to 200 members and have a 5-character ID.",
                panel.x + 16.0, contentY, note, 0.85);
        contentY += 18.0;
        dimText(canvas, "The leader invites players; admins can force-join.", panel.x + 16.0,
                contentY, note, 0.85);
        contentY += 26.0;

        const Rect create{panel.x + 16.0, contentY, 160.0, 34.0};
        framedButton(canvas, create, "Create guild", kPaper, kBodyBorder, create.contains(mouse));
        regions.push_back({create, GuildAction::Create, {}});
        drawnY += 100.0;
    } else {
        TextStyle guildName;
        guildName.size = 16.0;
        guildName.bold = true;
        guildName.fill = kPaper;
        guildName.strokeWidth = 3.0;
        guildName.baseline = Baseline::Top;
        guildName.roundJoin = true;
        text(canvas, guild.name, panel.x + 16.0, contentY, guildName);
        contentY += 22.0;

        TextStyle leaderLine;
        leaderLine.size = 13.0;
        leaderLine.fill = kPaper;
        leaderLine.strokeWidth = 0;
        leaderLine.baseline = Baseline::Top;
        // U+2014, an em dash, not a hyphen.
        dimText(canvas,
                "Leader: @" + guild.leader + " — " + std::to_string(guild.members.size()) +
                    "/200 members",
                panel.x + 16.0, contentY, leaderLine, 0.92);
        contentY += 22.0;

        const double actionW = 100.0;
        const double actionH = 28.0;
        double actionX = panel.x + 16.0;
        const Rect squadAll{actionX, contentY, actionW, actionH};
        framedButton(canvas, squadAll, "Squad up", kPaper, kBodyBorder, squadAll.contains(mouse));
        regions.push_back({squadAll, GuildAction::SquadAll, {}});
        actionX += actionW + 8.0;

        // Only the leader can invite, and the button's absence slides Leave
        // one slot to the left rather than leaving a gap.
        if (leaderIsMe) {
            const Rect inviteRect{actionX, contentY, actionW, actionH};
            framedButton(canvas, inviteRect, "Invite...", kPaper, kBodyBorder,
                        inviteRect.contains(mouse));
            regions.push_back({inviteRect, GuildAction::Invite, {}});
            actionX += actionW + 8.0;
        }

        const Rect leave{actionX, contentY, actionW, actionH};
        framedButton(canvas, leave, "Leave", kPaper, kDangerFrame, leave.contains(mouse));
        regions.push_back({leave, GuildAction::Leave, {}});

        contentY += actionH + 14.0;
        drawnY += 22.0 + 22.0 + actionH + 14.0;

        TextStyle membersHeading;
        membersHeading.size = 13.0;
        membersHeading.bold = true;
        membersHeading.fill = kPaper;
        membersHeading.strokeWidth = 0;
        // The reference never sets a baseline before this one call and inherits
        // whatever the last panel left; on the title screen that is 'middle'.
        dimText(canvas, "Members (" + std::to_string(guild.members.size()) + ")",
                panel.x + 16.0, contentY, membersHeading, 0.95);
        contentY += 20.0;
        drawnY += 20.0;

        const auto isOnline = [&guild](const std::string& member) {
            for (const std::string& name : guild.online) {
                if (sameName(name, member)) return true;
            }
            return false;
        };

        std::vector<std::string> sorted = guild.members;
        // Online first, then the leader, then by name. Written as a strict
        // order rather than the reference's sign-returning comparator, which
        // std::sort would be entitled to trip over.
        std::stable_sort(sorted.begin(), sorted.end(),
                         [&](const std::string& a, const std::string& b) {
                             const bool aOn = isOnline(a);
                             const bool bOn = isOnline(b);
                             if (aOn != bOn) return aOn;
                             const bool aLead = sameName(a, guild.leader);
                             const bool bLead = sameName(b, guild.leader);
                             if (aLead != bLead) return aLead;
                             // localeCompare orders case-insensitively; a raw
                             // byte compare would file every capitalised name
                             // ahead of every lower-case one.
                             const std::string al = lowered(a);
                             const std::string bl = lowered(b);
                             return al != bl ? al < bl : a < b;
                         });

        for (const std::string& member : sorted) {
            const double rowY = contentY;
            contentY += kRowHeight;
            drawnY += kRowHeight;
            if (rowY + kRowCard < bodyTop || rowY > panel.bottom()) continue;

            const bool online = isOnline(member);
            const bool isSelf = sameName(member, me);
            const bool isLeader = sameName(member, guild.leader);
            const double midY = rowY + kRowCard * 0.5;

            setFill(canvas, kPaper, 0.08);
            roundPath(canvas, Rect{panel.x + 12.0, rowY, panel.w - 24.0, kRowCard}, 4.0);
            canvas.fill();

            setFill(canvas, online ? kOnlineDot : kOfflineDot);
            canvas.beginPath();
            canvas.arc(static_cast<float>(panel.x + 24.0), static_cast<float>(midY), 5.0f, 0.0f,
                       static_cast<float>(kTau));
            canvas.fill();

            TextStyle label;
            label.size = 13.0;
            label.bold = true;
            label.fill = kPaper;
            label.strokeWidth = 0;
            // Laid out with a pen rather than as one string: the leader badge
            // is a drawn star and has no advance width to inherit.
            const std::string handle = "@" + member;
            double pen = panel.x + 38.0;
            text(canvas, handle, pen, midY, label);
            pen += measure(handle, label.size, true);
            if (isLeader) {
                pen += measure("  ", label.size, true);
                drawStar(canvas, pen + 6.0, midY, 6.0);
                pen += 12.0;
            }
            if (isSelf) text(canvas, "  (you)", pen, midY, label);

            const double smallW = 56.0;
            const double smallH = 22.0;
            double buttonX = panel.x + panel.w - 16.0 - smallW;
            const double buttonY = rowY + (kRowCard - smallH) * 0.5;
            if (leaderIsMe && !isSelf) {
                const Rect kick{buttonX, buttonY, smallW, smallH};
                framedButton(canvas, kick, "Kick", kPaper, kDangerFrame, kick.contains(mouse));
                regions.push_back({kick, GuildAction::Kick, member});
                buttonX -= smallW + 6.0;
            }
            if (!isSelf && online) {
                const Rect squad{buttonX, buttonY, smallW, smallH};
                framedButton(canvas, squad, "Squad", kPaper, kBodyBorder, squad.contains(mouse));
                regions.push_back({squad, GuildAction::SquadOne, member});
            }
        }
    }

    scroll_.contentHeight = drawnY;
    scroll_.viewHeight = visible;
    canvas.restore();

    if (drawnY > visible) {
        setFill(canvas, kInk, 0.25);
        roundPath(canvas, track, 5.0);
        canvas.fill();
        const double thumbHeight = std::max(24.0, track.h * visible / drawnY);
        const double travel = std::max(1.0, drawnY - visible);
        setFill(canvas, kBodyBorder);
        roundPath(canvas,
                      Rect{track.x, track.y + (scroll_.offset / travel) * (track.h - thumbHeight),
                           track.w, thumbHeight},
                      5.0);
        canvas.fill();
    }

    // --- dialogs ------------------------------------------------------------
    //
    // Painted last so they sit over the card, and given the click before the
    // panel's own regions ever see it.
    const auto centred = [&](double w, double h) {
        return Rect{(static_cast<double>(canvas.width()) - w) * 0.5,
                    (static_cast<double>(canvas.height()) - h) * 0.5, w, h};
    };

    if (modal.confirm != ConfirmMode::None) {
        // No scrim: window.confirm() dims nothing. What it does do is block
        // the page, so every click this card does not answer is swallowed
        // rather than reaching the panel underneath.
        const Rect card = centred(kConfirmWidth, kConfirmHeight);
        setFill(canvas, kBodyFill);
        roundPath(canvas, card, 4.0);
        canvas.fill();
        setFill(canvas, kDialogBody);
        roundPath(canvas, Rect{card.x + 2.0, card.y + 2.0, card.w - 4.0, card.h - 4.0}, 3.0);
        canvas.fill();

        TextStyle question;
        question.size = 14.0;
        question.fill = kPaper;
        question.strokeWidth = 0;
        question.align = Align::Centre;
        const std::string ask = modal.confirm == ConfirmMode::Leave
                                    ? std::string("Leave this guild?")
                                    : "Kick " + modal.member + " from the guild?";
        text(canvas, ask, card.x + card.w * 0.5, card.y + 34.0, question);

        const double buttonW = 100.0;
        const double buttonH = 30.0;
        const double buttonY = card.bottom() - buttonH - 16.0;
        // Cancel left, OK right: the platform order of the dialog this stands
        // in for.
        const Rect cancel{card.x + card.w * 0.5 - buttonW - 8.0, buttonY, buttonW, buttonH};
        const Rect accept{card.x + card.w * 0.5 + 8.0, buttonY, buttonW, buttonH};
        framedButton(canvas, cancel, "Cancel", kPaper, kBodyBorder, cancel.contains(mouse));
        framedButton(canvas, accept, "OK", kPaper, kBodyBorder, accept.contains(mouse));

        const auto answer = [&](bool confirmed) {
            if (confirmed) {
                if (modal.confirm == ConfirmMode::Leave) {
                    ctx.net.requestGuildLeave();
                } else {
                    ctx.net.requestGuildKick(modal.member);
                }
            }
            modal.clear();
        };
        if (ctx.window.keyPressed(Key::Enter)) answer(true);
        else if (ctx.window.keyPressed(Key::Escape)) answer(false);
        else if (ctx.released()) {
            if (accept.contains(mouse)) answer(true);
            else if (cancel.contains(mouse)) answer(false);
        }
        return true;
    }

    if (modal.prompt != PromptMode::None) {
        const Rect box = centred(kPromptWidth, kPromptHeight);
        setFill(canvas, kBodyFill);
        roundPath(canvas, box, 4.0);
        canvas.fill();
        setFill(canvas, kDialogBody);
        roundPath(canvas, Rect{box.x + 2.0, box.y + 2.0, box.w - 4.0, box.h - 4.0}, 3.0);
        canvas.fill();

        selectionHighlight(canvas, promptRun(box, modal.typed), modal.field.selection,
                           Rect{box.x + 4.0, box.y + 7.0, box.w - 8.0, box.h - 14.0}, kPaper,
                           0.32);

        TextStyle field;
        field.size = kPromptTextSize;
        field.strokeWidth = 0;
        field.fill = modal.typed.empty() ? kPlaceholder : kPaper;
        text(canvas, modal.typed.empty() ? promptPlaceholder(modal.prompt) : modal.typed,
             box.x + kPromptInset, box.y + box.h * 0.5, field);

        if (caretVisible(modal.field, ctx.timeSeconds)) {
            const double caretX = xOfIndex(promptRun(box, modal.typed), modal.field.selection.caret)
                                  + 1.0;
            setFill(canvas, kPaper);
            canvas.fillRect(static_cast<float>(caretX), static_cast<float>(box.y + 8.0), 1.0f,
                            static_cast<float>(box.h - 16.0));
        }

        // Printable ASCII only, which is what keeps the caret's byte index and
        // its character index the same thing. Guild names and usernames are
        // both ASCII by the server's own rules.
        TextEditOptions typing;
        typing.maxBytes = promptLimit(modal.prompt);
        typing.asciiOnly = true;
        editText(ctx.window, modal.typed, modal.field, ctx.timeSeconds, typing);
        trackTextMouse(ctx.window, modal.field, box, promptRun(box, modal.typed), modal.typed,
                       ctx.timeSeconds);
        if (ctx.window.keyPressed(Key::Enter)) {
            // An empty field just closes: the reference submits the trimmed
            // value and sends nothing when there is none.
            const std::string value = trimmedText(modal.typed);
            if (!value.empty()) {
                switch (modal.prompt) {
                    case PromptMode::Create: ctx.net.requestGuildCreate(value); break;
                    case PromptMode::Invite: ctx.net.requestGuildInvite(value); break;
                    case PromptMode::Kick: ctx.net.requestGuildKick(value); break;
                    case PromptMode::None: break;
                }
            }
            modal.clear();
        } else if (ctx.window.keyPressed(Key::Escape)) {
            modal.clear();
        }
        // No scrim and no swallowed clicks: the reference's input is a floating
        // element over a canvas that goes on answering the mouse underneath it.
    }

    if (!ctx.released()) return true;
    for (const HitRegion& region : regions) {
        if (!region.rect.contains(mouse)) continue;
        switch (region.action) {
            case GuildAction::Close:
                modal.clear();
                return false;
            case GuildAction::Create:
                modal.clear();
                modal.prompt = PromptMode::Create;
                modal.field.focusAtEnd(modal.typed, ctx.timeSeconds);
                break;
            case GuildAction::Invite:
                modal.clear();
                modal.prompt = PromptMode::Invite;
                modal.field.focusAtEnd(modal.typed, ctx.timeSeconds);
                break;
            case GuildAction::Kick:
                // Named here rather than typed: the reference asks
                // `Kick ${member} from the guild?` about the row that was hit.
                modal.clear();
                modal.confirm = ConfirmMode::Kick;
                modal.member = region.member;
                break;
            case GuildAction::Leave:
                modal.clear();
                modal.confirm = ConfirmMode::Leave;
                break;
            case GuildAction::SquadAll:
                ctx.net.requestGuildSquadAll();
                break;
            case GuildAction::SquadOne:
                ctx.net.requestGuildInviteToSquad(region.member);
                break;
            case GuildAction::AcceptInvite:
                ctx.net.requestGuildAccept();
                pending = {};
                break;
            case GuildAction::DeclineInvite:
                ctx.net.requestGuildDecline();
                pending = {};
                break;
            case GuildAction::None:
                break;
        }
        break;
    }
    return true;
}

} // namespace flix
