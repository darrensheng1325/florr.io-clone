// The changelog overlay: what changed, newest release first.
//
// An overlay rather than one of the tall list panels -- it hangs under the top
// icon row at a fixed 600x500, which is why its bounds come from
// `ChangelogPanel::bounds` and not from the shared list anchor.
//
// The table below is the browser build's CHANGELOG array, verbatim and in its
// own order -- oldest first -- and is walked backwards to paint. Keeping the
// source order means a new release is appended here exactly as it is appended
// there, with nothing to re-sort and no way to get the order wrong twice.
//
// Row pitch is written as literals rather than derived from lineHeight(): the
// reference advances 25px under a date and 24px under a bullet whatever the
// face's own leading is, and deriving it would put every row of a 7896px list
// a little further out of place than the row above it.

#include <algorithm>
#include <cctype>
#include <string>
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
/// A date row advances 25, a bullet 24, and 15 separates two entries.
constexpr double kDatePitch = 25.0;
constexpr double kBulletPitch = 24.0;
constexpr double kEntryGap = 15.0;
constexpr double kChangeSize = 14.0;
/// Bullet glyph to change text.
constexpr double kBulletIndent = 20.0;
/// One wheel notch is ~100px of deltaY in a browser, and this panel consumes
/// that unscaled. The shared Scroller's 42 would make the same gesture cover
/// well under half the ground it covers in the reference.
constexpr double kWheelStep = 100.0;

/// The card's own colours come from kChangelogSkin; this is the border shade
/// reused as a rule and a scrollbar thumb on the body.
constexpr std::uint32_t kBodyBorder = kChangelogSkin.border;
constexpr std::uint32_t kLinkFill = 0xD8F7FFu;

/// The longest entry holds 17 bullets; the spare slot keeps every row's list
/// null-terminated, so the count is walked rather than stored beside it.
constexpr int kMaxChanges = 18;

struct ChangelogEntry {
    const char* date;
    const char* changes[kMaxChanges];
};

constexpr ChangelogEntry kChangelog[] = {
    {"October 18, 2025",
     {"Added changelog"}},
    {"October 19, 2025",
     {"Added 3 new biomes",
      "New admin petal: Sparkle",
      "New admin command: spawn_special_mobs"}},
    {"October 22, 2025",
     {"New petal: Lightning",
      "New petal: Iris",
      "It is discovered that mobs have round hitboxes, so the setting is now more accurate"}},
    {"October 24, 2025",
     {"Overhauled the settings menu",
      "Ultra+ petals now have particle effects"}},
    {"November 20, 2025",
     {"New biome: Jungle"}},
    {"November 21, 2025",
     {"Some recolors and tweaks to the graphics",
      "New petal: Jelly",
      "fixes"}},
    {"November 22, 2025",
     {"New petal: Yucca",
      "New petal: Leaf",
      "Target dummy now shows DPS"}},
    {"November 23, 2025",
     {"Added skills system"}},
    {"November 29, 2025",
     {"New mob: Item Spawner",
      "New petal: Cutter",
      "New petal: Lightning Cutter",
      "New petal: Wing",
      "New biome: Sewers"}},
    {"December 3, 2025",
     {"Some UI changes",
      "Recolored some biomes",
      "New petal: Square",
      "A global notification is now shown when a rare petal is crafted"}},
    {"December 10, 2025",
     {"New petal: Blood Leaf, it damages the player when exploding",
      "Healing and self-damage now scales by rarity",
      "Cactus and Poison Cactus now give max health to the player"}},
    {"December 11, 2025",
     {"New petal: Ant Egg",
      "New petal: Fire Ant Egg",
      "Some rebalancing of mobs"}},
    {"December 15, 2025",
     {"All mobs now have eggs",
      "Added mob gallery",
      "Some optimizations"}},
    {"December 18, 2025",
     {"New ant hell map"}},
    {"December 20, 2025",
     {"Added shop, note that you cannot buy petals with real money, only stars",
      "Stars are awarded for killing mobs or claiming codes",
      "New admin command: generate_code"}},
    {"December 23, 2025",
     {"Added notifications system, check it to see when there might be an event",
      "Fixed a negative petal health bug",
      "New admin command: clear_notifications",
      "New admin command: notification"}},
    {"December 26, 2025",
     {"Changed title screen UI",
      "New admin command: give"}},
    {"December 31, 2025",
     {"New petal: Splitter",
      "New petal: Gas",
      "New petal: Bulb",
      "Petals now have physics and are attracted to mobs"}},
    {"January 19, 2026",
     {"Changed the map of most biomes",
      "Some biomes are temporarily disabled for now"}},
    {"January 22, 2026",
     {"Ocean is back!",
      "New petal: Starfish",
      "New petal: Sponge",
      "New mob: Starfish",
      "New mob: Sponge"}},
    {"January 31, 2026",
     {"New biome: MATRIX",
      "˜´∑ µø∫Ú øßå",
      "˜ªº˜˚¬ˆçß••",
      "New petal: ∆å√åßç®ˆπ†"}},
    {"February 16, 2026",
     {"New petal: Glass",
      "New petal: Third Eye",
      "New petal: Corn",
      "Commands are now suggested in the chat"}},
    {"February 20, 2026",
     {"Buffed droprates of ultras by 20x",
      "Changed the UI of mobs"}},
    {"February 21, 2026",
     {"Bugfix"}},
    {"February 22, 2026",
     {"Fixed an animation loop bug"}},
    {"February 22-March 25, 2026",
     {"Fixed performance issues",
      "Your settings now save",
      "Fixed teleporter system",
      "Backend changes",
      "FPS should be higher now, and ping should be lower"}},
    {"March 27, 2026",
     {"New petal: Faster",
      "More bug fixes and UI improvements"}},
    {"April 2, 2026",
     {"New mob: Sun",
      "New petal: Pollen",
      "Bulb now emits light",
      "Nerfed blood leaf",
      "Fixed some bugs"}},
    {"April 5, 2026",
     {"You now have a second row of loadout slots"}},
    {"April 12, 2026",
     {"Changed the UI of the skills panel",
      "Changed the UI of the inventory",
      "Changed the UI of the crafting panel",
      "Added second chance skill",
      "Fixed players getting damaged during invulnerability"}},
    {"April 13, 2026",
     {"Added apex rarity",
      "Recolored unique and apex petals"}},
    {"April 17, 2026",
     {"New petal: Powder",
      "New petal: Peas",
      "New mob: Desert Centipede",
      "New mob: Centipede"}},
    {"April 18, 2026",
     {"Faster now stacks additively instead of multiplicatively",
      "Fixed players glitching into walls when going very fast",
      "Added squads",
      "Added guilds",
      "New mob: Worker Ant",
      "New mob: Baby Ant",
      "New mob: Worker Fire Ant",
      "New mob: Baby Fire Ant",
      "A new portal has opened in desert",
      "Ant hell background looks much nicer now"}},
    {"April 19, 2026",
     {"Sun no longer drops eggs (to fix an exploit)",
      "Sun now drops glass, rock, sand, pollen, speed boost, and shield",
      "Added PVP mode",
      "Added Daily Streak",
      "Any obtained sun egg is now deleted"}},
    {"April 22, 2026",
     {"New mob: Ant Hole",
      "New mob: Fire Ant Hole"}},
    {"April 23, 2026",
     {"New petal: Magnet",
      "New petal: Air",
      "New petal: Soil",
      "Changed Ant Hole and Fire Ant Hole drops"}},
    {"April 24, 2026",
     {"Optimize the game",
      "Added API keys(you can now create discord bot)",
      "Secured the server",
      "Roach now looks better",
      "Added ultra zones"}},
    {"April 26, 2026",
     {"Changed keybinds for using items from 1-10 to U+1-10",
      "New petal: Yin Yang",
      "New petal: Lentil",
      "New petal: Bubble",
      "Clover now gives luck, and increases the rarity of mobs around you",
      "Changed petal attraction system",
      "Special petals no longer work in inactive loadout slots"}},
    {"April 30, 2026",
     {"Server now restarts every 24 hours",
      "Optimizations and bug fixes",
      "The keybinds for using items can now be swapped between U+1-10 and 1-10 in the settings menu",
      "New setting: Show Admin Commands",
      "Fixed show hitboxes not working",
      "Fixed moth rendering bug",
      "Ant Hell can now spawn bosses",
      "Added a bridge to the Mythic zone in Garden",
      "Changed sewers wall textures"}},
    {"May 3, 2026",
     {"New petal: Antennae",
      "New petal: Observer"}},
    {"May 4, 2026",
     {"Pollen drops on the ground now",
      "Changed mob spawning algorithm",
      "Bee now drops pollen"}},
    {"May 20, 2026",
     {"New petal: Bomb",
      "New petal: Flower",
      "New petal: Raindrop",
      "Optimize game"}},
    {"June 2, 2026",
     {"Added discord link"}},
    {"June 19, 2026",
     {"Server crashes should now be fixed(hopefully)",
      "Admin names are no longer shown on leaderboard",
      "Added skins menu"}},
    {"June 30, 2026",
     {"Fixed unobtainable petals being in the shop",
      "Bugfix(from discord/youtube bug reports)",
      "New link: link:https://flowrix.sussybite.dev"}},
    {"July 7, 2026",
     {"Patched exploits to get sun egg",
      "Changed bee AI",
      "Changed most mob speeds",
      "Clover now increases craft chance",
      "Added debug menu",
      "Added maze mode(testing phase, if you find exploits, please bug report in discord)",
      "Petals collected in maze increase in rarity by 1 outside of maze",
      "Petals collected outside of maze decrease by 1 in maze",
      "Only Mythic- petals are allowed in maze",
      "Maze changes each day",
      "Added absorbing(only for petals collected in maze)"}},
    {"July 8, 2026",
     {"Maze now has a seperate leveling system than the main game",
      "Made maze larger",
      "Fixed some server bugs",
      "Added mobile support",
      "Added absorb talents"}},
    {"July 9, 2026",
     {"New desert background",
      "Server and client optimizations"}},
    {"July 14, 2026",
     {"Fix client FPS",
      "New setting: GPU Acceleration",
      "Patched some glitches where players could get into walls",
      "Admin give and spawn mob commands can now have amounts",
      "New admin command: killall"}},
    {"July 20, 2026",
     {"Reworked Rose",
      "New petal: Dahlia",
      "New petal: Azalea",
      "Sandstorm now spins faster",
      "Fixed mobile login",
      "Fixed common mobs dropping better petals than uncommon mobs"}},
    {"July 26, 2026",
     {"Fixed petals reloading instantly(there are no bugs)",
      "Fixed inventory getting corrupted"}},
    {"July 29, 2026",
     {"New mob: Evil Centipede",
      "New mob: Queen Ant",
      "New mob: Digger",
      "New petal: Shell",
      "New petal: Uranium",
      "New petal: Pincer",
      "New petal: Web",
      "New petal: Guided Missile",
      "New petal: Blue Iris",
      "New petal: Stick",
      "New petal: Moon",
      "New petal: Lotus",
      "New petal: Heaviest",
      "New petal: Rice",
      "Poison damage numbers are purple now",
      "Added stalling mechanics to the game",
      "Honey slows, not a lure petal"}},
    {"August 1, 2026",
     {"Target dummies now spawn less often at garden spawn",
      "Pets and target dummies no longer have bossbars"}},
    {"August 2, 2026",
     {"Made pets smaller",
      "Nerf digger egg"}},
    {"August 3, 2026",
     {"Patched duping exploit"}},
    {"August 4, 2026",
     {"Removed some petals that were duped"}},
    {"August 6, 2026",
     {"Patched a cheat",
      "Fixed apex mobs despawning",
      "Admins can now post images in chat",
      "/spawn command now announces boss mobs"}},
    {"August 7, 2026",
     {"All players can post images in chat now(use <img src=\"Image Link\">)",
      "Fix crafting bugs",
      "Added curves to skin editor"}},
    {"August 10, 2026",
     {"New mob: Glitch Flower",
      "Reworked flower petal",
      "Added corruption",
      "New admin command: /corrupt"}},
    {"August 17, 2026",
     {"Apex mobs now have 100x HP and XP",
      "Server optimizations"}},
    {"August 23, 2026",
     {"Fix bugs",
      "Magnet no longer instantly attracts petals without playing animation",
      "Jungle coming soon",
      "Looting requirements changed"}},
    {"August 24, 2026",
     {"Balanced all mob HP/damage",
      "Changed some mob sizes"}},
    {"August 26, 2026",
     {"Fixed server crash bug"}},
};

constexpr int kEntryCount = static_cast<int>(sizeof(kChangelog) / sizeof(kChangelog[0]));

int changeCount(const ChangelogEntry& entry) {
    int count = 0;
    while (count < kMaxChanges && entry.changes[count] != nullptr) ++count;
    return count;
}

double entryHeight(const ChangelogEntry& entry) {
    return kDatePitch + kBulletPitch * changeCount(entry) + kEntryGap;
}

/// One clickable link, recorded while painting so the click pass has the same
/// geometry the glyphs were drawn at. Recorded UNCLIPPED, as the reference
/// does; `render` gates the hit on the content rect instead.
struct LinkHit {
    Rect rect;
    std::string url;
};

/// A run of a change line: literal text, or a link when `url` is set.
struct Segment {
    std::string text;
    std::string url;
};

/// Thumb-drag state for the scrollbar.
///
/// At file scope because it lives entirely between one press and the release
/// that ends it, and because the panel's own declaration sits in a header
/// twelve panels share -- widening it for one panel's transient would cost
/// every other panel a recompile and tell a reader nothing.
struct ThumbDrag {
    bool active = false;
    double startY = 0;
    double startOffset = 0;
};

ThumbDrag& thumbDrag() {
    static ThumbDrag drag;
    return drag;
}

bool isSpaceByte(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

bool hasScheme(const std::string& url) {
    const auto starts = [&url](const char* prefix) {
        const std::size_t n = std::char_traits<char>::length(prefix);
        if (url.size() < n) return false;
        for (std::size_t i = 0; i < n; ++i) {
            if (std::tolower(static_cast<unsigned char>(url[i])) != prefix[i]) return false;
        }
        return true;
    };
    return starts("http://") || starts("https://");
}

std::string normalizeUrl(const std::string& raw) {
    return hasScheme(raw) ? raw : "https://" + raw;
}

/// `new URL(u).host + pathname`, with a trailing slash trimmed: a link is
/// shown by where it goes, not by how it was written.
std::string linkLabel(const std::string& raw) {
    const std::string url = normalizeUrl(raw);
    const std::size_t afterScheme = url.find("//");
    std::string rest = afterScheme == std::string::npos ? url : url.substr(afterScheme + 2);
    // Neither the query nor the fragment is part of a host or a pathname.
    rest = rest.substr(0, rest.find_first_of("?#"));
    if (rest.size() > 1 && rest.back() == '/') rest.pop_back();
    return rest;
}

/// Splits a change line on `link:<token>`, exactly as the reference's regex
/// does. A bare "link:" with no token after it is not a match and stays text.
std::vector<Segment> parseChange(const std::string& change) {
    std::vector<Segment> segments;
    std::size_t last = 0;
    std::size_t from = 0;
    while (true) {
        const std::size_t at = change.find("link:", from);
        if (at == std::string::npos) break;
        std::size_t end = at + 5;
        while (end < change.size() && !isSpaceByte(change[end])) ++end;
        if (end == at + 5) {
            from = end;
            continue;
        }
        if (at > last) segments.push_back({change.substr(last, at - last), {}});
        const std::string raw = change.substr(at + 5, end - at - 5);
        segments.push_back({linkLabel(raw), normalizeUrl(raw)});
        last = from = end;
    }
    if (last < change.size()) segments.push_back({change.substr(last), {}});
    if (segments.empty()) segments.push_back({change, {}});
    return segments;
}

/// The one glyph in the build that is filled and THEN stroked.
///
/// The reference paints the bullet with fillText followed by strokeText, so
/// its black outline sits ON TOP of the white dot instead of behind it. At
/// 14px that outline is most of what the bullet looks like, which is why the
/// order is reproduced rather than routed through ui::text().
void drawBullet(Canvas& canvas, double x, double y, double strokeWidth) {
    TextStyle style;
    style.size = kChangeSize;
    style.fill = kPaper;
    style.stroke = kInk;
    // Clamped, not passed through: a negative strokeWidth means "derive one"
    // to paintRun, where here it has always meant "no outline".
    style.strokeWidth = std::max(0.0, strokeWidth);
    paintRun(canvas, "•", x, y + (ascent(kChangeSize) + descent(kChangeSize)) * 0.5, style,
             1.0, 1.0, /*fillFirst=*/true);
}

/// Paints one change line. `strokeWidth` is threaded by reference because a
/// link underline leaves it at 1 for everything after it -- including the next
/// entry's bullets -- which the reference does by leaking ctx.lineWidth.
void drawChange(Canvas& canvas, const std::string& change, double x, double y,
                double& strokeWidth, std::vector<LinkHit>& links) {
    double penX = x;
    for (const Segment& segment : parseChange(change)) {
        if (segment.text.empty()) continue;
        const double width = measure(segment.text, kChangeSize);

        TextStyle style;
        style.size = kChangeSize;
        style.fill = segment.url.empty() ? kPaper : kLinkFill;
        // Clamped, not passed through: a negative strokeWidth means "derive one"
        // to paintRun, where here it has always meant "no outline".
        style.strokeWidth = std::max(0.0, strokeWidth);
        text(canvas, segment.text, penX, y, style);

        if (!segment.url.empty()) {
            canvas.save();
            setStroke(canvas, kLinkFill);
            canvas.setLineWidth(1.0f);
            canvas.beginPath();
            canvas.moveTo(static_cast<float>(penX), static_cast<float>(y + 17.0));
            canvas.lineTo(static_cast<float>(penX + width), static_cast<float>(y + 17.0));
            canvas.stroke();
            canvas.restore();
            strokeWidth = 1.0;
            links.push_back({Rect{penX, y, width, 18.0}, segment.url});
        }
        penX += width;
    }
}

} // namespace

double ChangelogPanel::preferredWidth() { return 600.0; }

void ChangelogPanel::reset() {
    scroll_ = {};
    thumbDrag() = {};
}

bool ChangelogPanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Rect panel = ctx.bounds;
    const Vec2 mouse = ctx.mouse();

    const double viewport = panel.h - kHeaderHeight;
    double contentHeight = 0;
    for (const ChangelogEntry& entry : kChangelog) contentHeight += entryHeight(entry);
    scroll_.contentHeight = contentHeight;
    scroll_.viewHeight = viewport;
    const double maxScroll = scroll_.maxOffset();
    const bool scrollable = contentHeight > viewport;

    const Rect track{panel.right() - kScrollbarWidth - 5.0, panel.y + kHeaderHeight,
                     kScrollbarWidth, panel.h - kHeaderHeight - 5.0};

    // The wheel is INVERTED here: in the reference a notch DOWN walks the list
    // back toward the newest entry. Odd, deliberate there, kept here.
    if (panel.contains(mouse)) scroll_.offset += ctx.wheel() * kWheelStep;

    ThumbDrag& drag = thumbDrag();
    if (ctx.pressed() && scrollable && track.contains(mouse)) {
        drag.active = true;
        drag.startY = mouse.y;
        drag.startOffset = scroll_.offset;
    }
    if (!ctx.window.mouseDown(MouseButton::Left)) drag.active = false;
    if (drag.active) {
        // The reference maps the drag over panelHeight - 45, not over the
        // track: the thumb runs slightly ahead of the cursor by design.
        const double ratio = (mouse.y - drag.startY) / (panel.h - 45.0);
        scroll_.offset = drag.startOffset + ratio * maxScroll;
    }
    scroll_.offset = clamp(scroll_.offset, 0.0, maxScroll);

    overlayCard(canvas, panel, kChangelogSkin);

    TextStyle heading;
    heading.size = 20.0;
    heading.bold = true;
    heading.fill = kPaper;
    heading.strokeWidth = 2.0;
    heading.baseline = Baseline::Top;
    text(canvas, "Changelog", panel.x + kPadding, panel.y + kPadding, heading);

    // No hover and no press state, exactly as the reference pill has none.
    const Rect closeRect = closeButtonRect(panel);
    panelClose(canvas, closeRect, closeRect.contains(mouse));

    const Rect view{panel.x + kPadding, panel.y + kHeaderHeight, panel.w - kPadding * 2,
                    panel.h - kHeaderHeight - kPadding};

    canvas.save();
    roundPath(canvas, view, 8.0);
    canvas.clip();

    // The date strip yields its right edge to the scrollbar when there is one.
    const double stripWidth = view.w - (scrollable ? kScrollbarWidth + 5.0 : 0.0);
    double contentY = view.y + kPadding - scroll_.offset;
    std::vector<LinkHit> links;

    // Newest first: the table is stored oldest-first and read backwards.
    for (int i = kEntryCount - 1; i >= 0; --i) {
        const ChangelogEntry& entry = kChangelog[i];
        const double height = entryHeight(entry);
        // A row's glyphs reach past its pitch, so the cull keeps a line of
        // slack on each side rather than trusting the box.
        if (contentY + height < view.y - kBulletPitch || contentY > view.bottom() + kBulletPitch) {
            contentY += height;
            continue;
        }

        setFill(canvas, kPaper, 0.05);
        roundPath(canvas, Rect{view.x, contentY - 5.0, stripWidth, 10.0}, 8.0);
        canvas.fill();

        TextStyle date;
        date.size = 20.0;
        date.bold = true;
        date.fill = kPaper;
        date.strokeWidth = 2.0;
        text(canvas, entry.date, view.x, contentY, date);
        contentY += kDatePitch;

        // The first bullet of an entry is outlined at 1px and the rest at
        // 0.5, which is what makes the top bullet of every group read darker.
        double strokeWidth = 1.0;
        const int changes = changeCount(entry);
        for (int c = 0; c < changes; ++c) {
            drawBullet(canvas, view.x, contentY, strokeWidth);
            strokeWidth = 0.5;
            drawChange(canvas, entry.changes[c], view.x + kBulletIndent, contentY, strokeWidth,
                       links);
            contentY += kBulletPitch;
        }
        contentY += kEntryGap;
    }
    canvas.restore();

    if (scrollable) {
        setFill(canvas, kPaper, 0.1);
        roundPath(canvas, track, 5.0);
        canvas.fill();
        // No minimum thumb height: on a 7896px list the reference's thumb is
        // 26px, and clamping it to a comfortable size would move it off the
        // scroll position it is meant to report.
        const double thumbHeight = (viewport - 5.0) * viewport / contentHeight;
        const double travelled = maxScroll > 0 ? scroll_.offset / maxScroll : 0.0;
        setFill(canvas, kBodyBorder);
        roundPath(canvas,
                      Rect{track.x, track.y + travelled * (track.h - thumbHeight),
                           track.w, thumbHeight},
                      5.0);
        canvas.fill();
    }

    if (!ctx.released()) return true;
    if (closeRect.contains(mouse)) return false;
    // Link rects are recorded unclipped, so the hit is gated on the body the
    // way the reference gates it.
    if (view.contains(mouse)) {
        for (const LinkHit& link : links) {
            if (!link.rect.contains(mouse)) continue;
            openExternalLink(link.url);
            break;
        }
    }
    return true;
}

} // namespace flix
