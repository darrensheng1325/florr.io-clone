#include "client/app.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <utility>

#include "client/interpolation.h"
#include "client/ui/draw.h"
#include "client/ui/markup.h"
#include "client/ui/text.h"
#include "client/web/reload.h"
#include "shared/game/config.h"
#include "shared/game/constants.h"

namespace flix {

using namespace flix::ui;

namespace {

/// The design space every draw call in this client is written in.
///
/// Not a window size: it is the fixed extent the frame is scaled to fill, so
/// a 900x600 window and a 3840x2160 one on a Retina panel both show exactly
/// this much of the world and this much HUD, at the same relative size. Only
/// the sharpness differs. A window that is not 16:9 shows LESS than this on
/// its short axis, never more -- the window's edge is not a zoom control. See
/// Window::setDesignSize for the machinery, and client/camera.h for why the
/// world's own zoom is left flat.
///
/// 1920x900 rather than the 1280x720 the window opens at, because it is the
/// resolution the browser build's layout numbers were authored against: at
/// this size uiScale is 1 on an ordinary display and the frame is identical,
/// pixel for pixel, to what this client drew before the design space existed.
constexpr int kDesignWidth = 1920;
constexpr int kDesignHeight = 930;

/// Layout constants for the shell, in design units like everything else. Kept
/// local because nothing outside this file positions these; the shared values
/// that widgets derive from live in theme.h.
constexpr double kLoginFormWidth = 400;
constexpr double kLoginFormHeight = 500;
constexpr double kRegisterFormHeight = 600;
constexpr double kFieldHeight = 40;
constexpr double kButtonHeight = 40;
constexpr double kAdvancedHeight = 35;
/// The backdrop's petals are drawn at 0.5x to 2x of this.
constexpr double kTitlePetalPixels = 32.0;
/// The daily-login card's body; its border is the same colour at 0.7 value.
constexpr std::uint32_t kStreakPanel = 0x66FFFFu;
/// How long its star wobbles after a fresh claim.
constexpr double kStreakPulseSeconds = 3.0;

// --- HUD ------------------------------------------------------------------
//
// Absolute pixel anchors, exactly as the reference's are. Nothing in this
// block is derived from the viewport or from the icon strip: the browser lays
// it out at fixed coordinates and never reflows it, so deriving it here would
// put it somewhere the reference never has it.
constexpr double kFlowerCentreX = 50.0;
constexpr double kFlowerCentreY = 120.0;
constexpr double kHudBarWidth = 200.0;
constexpr double kHudBarHeight = 20.0;
constexpr double kHudBarX = kFlowerCentreX + 12.0;    // 62, tucked under the flower
constexpr double kHudHealthY = 97.5;
constexpr double kHudXpY = kHudHealthY + kHudBarHeight + 5.0;   // 122.5
constexpr double kHudTextX = kFlowerCentreX + 35.0;   // 85, clear of the flower
/// How long the health bar takes to fade from the invulnerable colour back to
/// green once invulnerability ends.
constexpr double kInvulFadeSeconds = 0.5;

// --- minimap ---------------------------------------------------------------
constexpr double kMinimapSize = 200.0;
constexpr double kMinimapPadding = 10.0;
/// The spawn bands' tints, one per rarity, drawn at 0.4 alpha while ALT is
/// held. MINIMAP_SPAWN_COLORS (src/graphics/minimap.ts:11-22) is its own table
/// and not the item palette: it ends violet and cyan where kRarityColors ends
/// white and magenta, and a band read against the wrong one names the wrong
/// tier.
constexpr std::array<std::uint32_t, kRarityCount> kMinimapSpawnColors = {
    0x7EEF6Du,  // common
    0xFFE65Du,  // uncommon
    0x4D52E3u,  // rare
    0x861FDEu,  // epic
    0xDE1F1Fu,  // legendary
    0x1FDBDEu,  // mythic
    0xDE1F65u,  // ultra
    0x2BFFA4u,  // super
    0xBF00FFu,  // unique
    0x00FFFFu,  // apex
};

// --- chat ------------------------------------------------------------------
//
// The reference's chat is a fixed 300x200 box 10px off the bottom-left corner.
// These are its resolved content boxes, measured from the BOTTOM of the window
// because that is the edge it is pinned to.
constexpr double kChatX = 115.0;              ///< left edge of the text column
constexpr double kChatColumnWidth = 270.0;
constexpr double kChatColumnUp = 195.0;       ///< top of the column, from the bottom edge
constexpr double kChatColumnDown = 65.0;      ///< bottom of the column, from the bottom edge
/// One 14px row of Ubuntu at the browser's `normal` line-height.
constexpr double kChatLineHeight = 16.0;
/// A message div's `margin: 2px 0`; adjacent siblings collapse to one gap.
constexpr double kChatMessageGap = 2.0;
constexpr double kChatFieldUp = 44.0;         ///< top of the input, from the bottom edge
constexpr double kChatFieldHeight = 20.0;
/// The size a browser gives an unstyled <input>, which is what the field is.
constexpr double kChatFieldTextSize = 13.333;
/// The suggestion list replaces the message column; these are its own metrics.
constexpr double kChatSuggestionRowHeight = 23.0;   // 4px padding, a 15px line, 4px
constexpr double kChatSuggestionSize = 13.0;

/// The slash commands the reference offers, in its order.
///
/// Admin rows are carried so this is the reference's table rather than an
/// edited copy of it, and filtered at draw time by matchChatCommands: they are
/// offered only to a client the server has told is admin.
struct ChatCommand {
    const char* command;
    const char* description;
    bool admin;
};

constexpr ChatCommand kChatCommands[] = {
    {"/help", "Show available commands", false},
    {"/biome", "Show the most populated biome", false},
    {"/create-api-key", "Issue an API key tied to your account: /create-api-key [label]", false},
    {"/delete-api-key", "Revoke one of your API keys: /delete-api-key <key-or-prefix>", false},
    {"/admin save", "Save player progress", true},
    {"/admin list-players", "List online players", true},
    {"/admin list-sockets", "List connected sockets", true},
    {"/admin set_max_enemies", "Set max enemy count", true},
    {"/admin set_bot_count", "Set bot count (0-100, or \"default\")", true},
    {"/admin spawn_special_mobs", "Spawn special mobs", true},
    {"/admin spawn", "Spawn a mob: /admin spawn <mob> <rarity> [x y] [amount] [stack]", true},
    {"/admin killall", "Kill all wild mobs (pets left intact)", true},
    {"/admin teleport", "Teleport a player", true},
    {"/admin tp", "Teleport a player (shorthand)", true},
    {"/admin teleport_all", "Teleport every player and bot: /admin teleport_all <x> <y>", true},
    {"/admin tpall", "Teleport every player (shorthand)", true},
    {"/admin teleport_bots", "Teleport every bot only: /admin teleport_bots <x> <y>", true},
    {"/admin tpbots", "Teleport every bot only (shorthand)", true},
    {"/admin corrupt", "Toggle corruption (fights players anywhere): /admin corrupt <player> [on|off|toggle]", true},
    {"/admin generate_code", "Generate a star code", true},
    {"/admin gen_code", "Generate a star code (shorthand)", true},
    {"/admin list_codes", "List all generated codes", true},
    {"/admin delete_code", "Delete a code", true},
    {"/admin notification", "Create a notification", true},
    {"/admin notify", "Create a notification (shorthand)", true},
    {"/admin clear_notifications", "Clear all notifications", true},
    {"/admin clear_notifs", "Clear notifications (shorthand)", true},
    {"/admin give", "Give item(s) to a player: /admin give <player> <item> <rarity> [amount]", true},
    {"/admin grant_admin", "Lend a player the admin console until they respawn: /admin grant_admin <player>", true},
    {"/admin revoke_admin", "Take back a temporary admin grant: /admin revoke_admin <player>", true},
    {"/admin list_admins", "List active temporary admin grants", true},
    {"/admin mute", "Bar a player from chat until unmuted: /admin mute <player>", true},
    {"/admin unmute", "Let a muted player chat again: /admin unmute <player>", true},
    {"/admin delete_guests", "Delete default guest accounts", true},
    {"/admin list_today_logins", "List accounts active in last 24h", true},
    {"/admin list_active", "List accounts active in last 24h (shorthand)", true},
    {"/cmd", "Execute server command (alias)", true},
    {"/forcelocalplayerflags", "Set local player face/equip flags (client-only)", false},
    {"/squad-create", "Create a new squad ([public|private])", false},
    {"/squad-invite", "Invite a player to your squad", false},
    {"/squad-find-public", "List joinable public squads", false},
    {"/squad-join", "Join a public squad by its ID", false},
    {"/squad-public", "Make your squad public (leader only)", false},
    {"/squad-private", "Make your squad private (leader only)", false},
    {"/squad-accept", "Accept a squad invite", false},
    {"/squad-decline", "Decline a squad invite", false},
    {"/squad-leave", "Leave your current squad", false},
    {"/squad-info", "Show squad members", false},
    {"/s", "Send a message to your squad", false},
    {"/guild-create", "Create a new guild: /guild-create <name>", false},
    {"/guild-invite", "Invite a player to your guild (leader only)", false},
    {"/guild-accept", "Accept a guild invite", false},
    {"/guild-decline", "Decline a guild invite", false},
    {"/guild-leave", "Leave your current guild", false},
    {"/guild-kick", "Kick a member from the guild (leader only)", false},
    {"/guild-info", "Show guild members and status", false},
    {"/guild-squad", "Invite online guildmates into a squad", false},
    {"/guild-list", "List all guilds (id, name, members)", false},
    {"/guild-menu", "Toggle the guild menu panel", false},
    {"/g", "Send a message to your guild", false},
    {"/admin guild_force_join", "Force a player into a guild", true},
    {"/admin guild_list", "List all guilds", true},
    {"/admin guild_info", "Show info for a guild by id", true},
    {"/admin restart", "Schedule server restart: restart [<N>(s|m|h)|cancel|status]", true},
    {"/admin backup_db", "Back up the database: backup_db [list]", true},
    {"/admin update", "Back up DB, install latest build from GitHub, restart: update [now|<N>(s|m|h)|status|cancel]", true},
    {"/admin change-maze", "Change the maze: change-maze [next|garden|desert|ocean|<dayNumber>]", true},
    {"/level-from-string", "Show what level a player named <name> would roll", false},
    {"/loadout-from-string", "Show the loadout a player named <name> would roll", false},
    {"/admin remove_petal ", "remove petal from a player", true},
};

Rect centred(double width, double height, int viewW, int viewH, double yOffset = 0) {
    return {(viewW - width) * 0.5, (viewH - height) * 0.5 + yOffset, width, height};
}

/// Mirrors the canvas AuthForm's vertical rhythm in title_screen/auth_form.ts.
/// Keeping the rectangles together prevents the interaction pass and the draw
/// pass from drifting apart when this form changes again.
struct AuthLayout {
    Rect username;
    Rect password;
    Rect confirmation;      ///< registering only
    Rect advanced;          ///< the Advanced Settings disclosure
    Rect serverIp;          ///< only while the disclosure is open
    Rect action;            ///< Login, or Register while registering
    Rect secondary;         ///< Register (switches mode), or Register Offline
    Rect guest;             ///< login only
    /// Register only: the "Already have an account? Login" band. Unbounded in
    /// x, because the text it covers is centred and the browser's hit test
    /// only looks at y.
    Rect modeLink;
    double headingY = 0;
    /// The guest hint in login mode, the mode link's text in register mode.
    double hintY = 0;
    double bottomY = 0;     ///< below every control, for the message line
};

AuthLayout authLayout(int viewW, int viewH, bool registering, bool advancedOpen) {
    const double formHeight = registering ? kRegisterFormHeight : kLoginFormHeight;
    const Rect form = centred(kLoginFormWidth, formHeight, viewW, viewH);
    const double x = form.x + 20;
    const double w = form.w - 40;

    // A single walk down the form, as auth_form.ts does it: every offset here
    // is one of its `currentY +=` steps, in the same order. Deriving the
    // rectangles from absolute constants instead is how the two drift apart
    // the first time a row is inserted.
    AuthLayout layout;
    double y = form.y + 30;
    layout.headingY = y;
    y += 50;
    // The browser adds a "not secure" warning and 30px here when it is served
    // over http:. A raw TCP socket has no scheme to be insecure about, so this
    // client is always on the https path.
    y += 10;

    layout.username = {x, y, w, kFieldHeight};
    y += kFieldHeight + 15;
    layout.password = {x, y, w, kFieldHeight};
    y += kFieldHeight + 15;
    if (registering) {
        layout.confirmation = {x, y, w, kFieldHeight};
        y += kFieldHeight + 15;
    }

    layout.advanced = {x, y, w, kAdvancedHeight};
    y += kAdvancedHeight + 10;
    if (advancedOpen) {
        layout.serverIp = {x, y, w, kFieldHeight};
        y += kFieldHeight + 15;
    }
    y += 10;

    layout.action = {x, y, w, kButtonHeight};
    y += kButtonHeight + 10;
    layout.secondary = {x, y, w, kButtonHeight};
    y += kButtonHeight + 10;

    if (registering) {
        layout.modeLink = {0, y, static_cast<double>(viewW), 20};
        layout.hintY = y + 10;
        layout.bottomY = y + 20;
    } else {
        const double guestWidth = w * 0.5;
        const double guestHeight = kButtonHeight * 0.8;
        layout.guest = {x + (w - guestWidth) * 0.5, y, guestWidth, guestHeight};
        y += guestHeight + 4;
        layout.hintY = y + 6;
        layout.bottomY = y + 12;
    }
    return layout;
}

/// The auth control under the pointer, by the browser build's own button ids.
/// One function for hover, for the press latch and for activation, so the
/// three can never disagree about which button is which.
std::string authControlAt(const AuthLayout& layout, bool registering, Vec2 mouse) {
    if (hit(layout.advanced, mouse)) return "toggleAdvanced";
    if (registering) {
        if (hit(layout.action, mouse)) return "register";
        if (hit(layout.secondary, mouse)) return "offline";
        if (hit(layout.modeLink, mouse)) return "showLogin";
        return {};
    }
    if (hit(layout.action, mouse)) return "login";
    if (hit(layout.secondary, mouse)) return "showRegister";
    if (hit(layout.guest, mouse)) return "guest";
    return {};
}

/// The spawn picker's label and colour.
///
/// PVP Arena and the Maze are title-screen destinations rather than map
/// annotations, so `biomeDisplay` -- which reads the map -- has nothing to say
/// about them. Their two rows live here, beside the picker that is their only
/// consumer.
BiomeDisplay titleBiomeDisplay(const std::string& id) {
    if (id == "pvp") return {"PVP Arena", 0xDC3C3Cu};
    if (id == "maze") return {"Maze", 0x573D80u};
    return biomeDisplay(id);
}

/// True while the pointer is over this client's window. Window gets the answer
/// from DOM enter/leave callbacks in a page and SDL focus on the desktop; a
/// bounds test against the last position cannot answer after the pointer left.
bool pointerInWindow(const Window& window) { return window.pointerInside(); }

/// The title screen's own hit test, inclusive on all four edges. The browser
/// writes every one of these as `x >= left && x <= left + width` (index.ts:877,
/// 900, 915-916, 955-956), so its rightmost column and bottom row are live
/// where ui::hit -- half-open, because the world's collision code needs it that
/// way -- leaves them dead.
bool hitInclusive(Rect r, Vec2 p) {
    return p.x >= r.x && p.x <= r.x + r.w && p.y >= r.y && p.y <= r.y + r.h;
}

/// The picker's fallback label: the id with its first character upper-cased
/// and nothing else touched. Deliberately not `titleCase` -- the browser
/// build's getBiomeConfig capitalises one character, so "foo_bar" reads
/// "Foo_bar" there and must here.
std::string capitaliseFirst(std::string id) {
    if (!id.empty()) id[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(id[0])));
    return id;
}

/// The name field's overflow rule: drop trailing characters until the string
/// plus an ellipsis fits, then append one. Measured in the same bold 18px face
/// the field draws in, because a narrower measure would cut too much.
std::string ellipsised(Canvas& canvas, std::string value, double maxWidth) {
    if (textWidth(canvas, value, 18.0, true) <= maxWidth) return value;
    while (!value.empty() && textWidth(canvas, value + "...", 18.0, true) > maxWidth) {
        // Whole UTF-8 sequences, so a cut never leaves a broken character.
        std::size_t at = value.size() - 1;
        while (at > 0 && (static_cast<unsigned char>(value[at]) & 0xC0) == 0x80) --at;
        value.erase(at);
    }
    return value + "...";
}

/// Unix milliseconds. The daily-streak card counts down to timestamps the
/// server minted from the same clock, so this cannot be the app's uptime.
std::int64_t wallClockMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// "17h 34m" / "4m 12s" / "9s", as the streak widget formats a countdown.
std::string formatDuration(std::int64_t millis) {
    if (millis <= 0) return "0s";
    const std::int64_t total = millis / 1000;
    const std::int64_t h = total / 3600;
    const std::int64_t m = (total % 3600) / 60;
    const std::int64_t sec = total % 60;
    if (h > 0) return std::to_string(h) + "h " + std::to_string(m) + "m";
    if (m > 0) return std::to_string(m) + "m " + std::to_string(sec) + "s";
    return std::to_string(sec) + "s";
}

/// JavaScript's `toFixed(2)`, for the stats overlay's frame time.
std::string twoDecimals(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.2f", value);
    return buffer;
}

std::string oneDecimal(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", value);
    return buffer;
}

/// The reference's `formatBytes`: whole bytes below a kilobyte, one decimal
/// above. Used for both the throughput totals and the per-event breakdown.
std::string formatBytes(double bytes) {
    char buffer[32];
    if (bytes < 1024.0) {
        std::snprintf(buffer, sizeof buffer, "%d B", static_cast<int>(bytes));
    } else if (bytes < 1024.0 * 1024.0) {
        std::snprintf(buffer, sizeof buffer, "%.1f KB", bytes / 1024.0);
    } else {
        std::snprintf(buffer, sizeof buffer, "%.1f MB", bytes / (1024.0 * 1024.0));
    }
    return buffer;
}

/// The reference's `formatNumber`: one decimal place and a suffix past a
/// thousand, and the exact integer while ALT is held.
std::string formatNumber(double value, bool raw) {
    if (!raw) {
        static constexpr struct { double scale; const char* suffix; } kSteps[] = {
            {1e12, "T"}, {1e9, "B"}, {1e6, "M"}, {1e3, "K"},
        };
        for (const auto& step : kSteps) {
            if (value < step.scale) continue;
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%.1f%s", value / step.scale, step.suffix);
            return buffer;
        }
    }
    return std::to_string(static_cast<long long>(std::llround(value)));
}

/// One HUD bar: an oversized black pill with the fill sitting flush inside it.
///
/// Deliberately NOT ui::bar. That one insets the fill by its own outline width
/// and clamps the fraction to the bar; the reference does neither, so a bar
/// that is somehow over-full overhangs its plate there and has to here, and a
/// one-pixel sliver of XP still shows as a rounded cap.
void hudBar(Canvas& canvas, double x, double y, double w, double h, double fillWidth,
            std::uint32_t colour) {
    const float radius = static_cast<float>(h * 0.5);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(x - 2.0), static_cast<float>(y - 2.0),
                     static_cast<float>(w + 4.0), static_cast<float>(h + 4.0), radius);
    setFill(canvas, kInk);
    canvas.fill();
    if (fillWidth <= 0) return;
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(x), static_cast<float>(y),
                     static_cast<float>(fillWidth), static_cast<float>(h), radius);
    setFill(canvas, colour);
    canvas.fill();
}

/// The reference's `drawFlower` with its default face, in a space whose origin
/// is the flower's centre.
///
/// Local to this file on purpose: the HUD's avatar is a fixed picture. It does
/// not mirror the player's own colour, skin, face flags or equipment -- the
/// reference draws the same yellow flower whatever the player looks like -- so
/// there is nothing here for a general painter to be parameterised by.
void drawFlowerFace(Canvas& canvas, std::uint32_t colour, double radius, double eyeX,
                    double eyeY, double mouth) {
    setFill(canvas, shade(colour, 0.8));
    canvas.fillCircle(0, 0, static_cast<float>(radius * (26.5 / 25.0)));
    setFill(canvas, colour);
    canvas.fillCircle(0, 0, static_cast<float>(radius * (23.5 / 25.0)));

    canvas.save();
    // The face is authored against a radius-25 flower; everything below is in
    // that space, which is why the eye and mouth numbers can be the
    // reference's own literals.
    const float scale = static_cast<float>(radius / 25.0);
    canvas.scale(scale, scale);

    // The eye whites are filled, then used as the clip for the pupils AND for
    // their own outline, so only the inner half of that outline survives --
    // which is what gives the eye its heavy upper lid.
    Path2D eyes;
    eyes.ellipse(-7.0f, -4.8f, 3.2f, 6.5f, 0, 0, static_cast<float>(kTau));
    eyes.moveTo(10.2f, -4.8f);
    eyes.ellipse(7.0f, -4.8f, 3.2f, 6.5f, 0, 0, static_cast<float>(kTau));
    canvas.save();
    setFill(canvas, kInk);
    canvas.fill(eyes);
    canvas.clip(eyes);
    setFill(canvas, kPaper);
    canvas.beginPath();
    canvas.arc(static_cast<float>(-7.0 + eyeX), static_cast<float>(-4.8 + eyeY), 3.0f, 0,
               static_cast<float>(kTau));
    canvas.fill();
    canvas.beginPath();
    canvas.arc(static_cast<float>(7.0 + eyeX), static_cast<float>(-4.8 + eyeY), 3.0f, 0,
               static_cast<float>(kTau));
    canvas.fill();
    canvas.setLineWidth(1.0f);
    setStroke(canvas, kInk);
    canvas.stroke(eyes);
    canvas.restore();

    canvas.save();
    setStroke(canvas, 0x222222u);
    canvas.setLineWidth(1.5f);
    canvas.setLineCap("round");
    canvas.beginPath();
    canvas.moveTo(-6.0f, 10.0f);
    canvas.quadraticCurveTo(0.0f, static_cast<float>(mouth), 6.0f, 10.0f);
    canvas.stroke();
    canvas.restore();

    canvas.restore();
}

/// "11:31:44 PM" in the machine's own timezone, which is what
/// `Date#toLocaleTimeString` gives the reference on an en-US browser.
std::string clockTime(std::int64_t unixMillis) {
    const std::time_t seconds = static_cast<std::time_t>(unixMillis / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    const int hour12 = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%d:%02d:%02d %s", hour12, local.tm_min, local.tm_sec,
                  local.tm_hour < 12 ? "AM" : "PM");
    return buffer;
}

/// The commands whose names start with what has been typed, case-blind.
/// Shared by the key handler and the draw pass so the highlighted row and the
/// completed text can never come from two different lists.
std::vector<const ChatCommand*> matchChatCommands(const std::string& typed, bool includeAdmin) {
    std::string needle = typed;
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::vector<const ChatCommand*> matches;
    for (const ChatCommand& command : kChatCommands) {
        // The browser gates the admin rows on a `showAdminCommands`
        // localStorage flag; this client has no such store, so it gates them
        // on what the server actually said -- the admin flag that arrives with
        // the skin catalog, which a temporary grant re-sends. Listing them to
        // everyone would advertise a console most players cannot open.
        if (command.admin && !includeAdmin) continue;
        std::string name = command.command;
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name.size() >= needle.size() && name.compare(0, needle.size(), needle) == 0) {
            matches.push_back(&command);
        }
    }
    return matches;
}

/// One styled run of a transcript line, before it is laid out.
///
/// `italic`, `underline` and `blink` come from the line's markup; everything
/// in the transcript is already bold, so <b> -- which is what wraps every boss
/// announcement -- has no field of its own. That matches the reference, whose
/// chat box is font-weight:700 to begin with.
struct ChatToken {
    std::string text;
    double size = 14.0;
    std::uint32_t fill = kPaper;
    double alpha = 1.0;
    bool italic = false;
    bool underline = false;
    bool blink = false;
    /// A <br>: end the row here and start the next one.
    bool lineBreak = false;
    /// Set when this run continues the previous one's word rather than
    /// starting a new one, as the "lo" of `<b>Hel</b>lo` does. Without it the
    /// layout would insert a space at every style change.
    bool joinsPrevious = false;
};

/// The same run once it knows which row it is on and where along it.
struct ChatPlacedRun {
    std::string text;
    double x = 0;
    double size = 14.0;
    std::uint32_t fill = kPaper;
    double alpha = 1.0;
    bool italic = false;
    bool underline = false;
    bool blink = false;
};

using ChatRow = std::vector<ChatPlacedRun>;

/// Erases the last whole UTF-8 sequence, so a cut never leaves half a
/// character behind.
void popCodepoint(std::string& value) {
    if (value.empty()) return;
    std::size_t at = value.size() - 1;
    while (at > 0 && (static_cast<unsigned char>(value[at]) & 0xC0) == 0x80) --at;
    value.erase(at);
}

/// Flows a message's runs into rows no wider than `width`.
///
/// The reference's message is a block of inline spans in a 270px column, so it
/// breaks between words; `word-wrap: break-word` then breaks INSIDE a word
/// that will not fit a line of its own, which is what the inner loop is for.
std::vector<ChatRow> layoutChatMessage(const std::vector<ChatToken>& tokens, double width) {
    std::vector<ChatRow> rows;
    rows.emplace_back();
    double pen = 0;

    for (const ChatToken& token : tokens) {
        // A <br> ends the row wherever it stands, including on an empty one --
        // "Public squads:<br/><br/>" is meant to leave a blank line.
        if (token.lineBreak) {
            rows.emplace_back();
            pen = 0;
            continue;
        }
        std::string word = token.text;
        if (word.empty()) continue;
        const auto place = [&](const std::string& run, double x) {
            rows.back().push_back({run, x, token.size, token.fill, token.alpha, token.italic,
                                   token.underline, token.blink});
        };
        double gap = (rows.back().empty() || token.joinsPrevious)
                         ? 0.0 : measure(" ", token.size, true);
        if (!rows.back().empty() && pen + gap + measure(word, token.size, true) > width) {
            rows.emplace_back();
            pen = 0;
            gap = 0;
        }
        // A word too long for a whole row is cut at the last character that
        // fits and the remainder starts the next row.
        while (measure(word, token.size, true) > width) {
            std::string head = word;
            while (!head.empty() && measure(head, token.size, true) > width) popCodepoint(head);
            if (head.empty()) break;
            place(head, pen);
            word.erase(0, head.size());
            rows.emplace_back();
            pen = 0;
            gap = 0;
        }
        if (word.empty()) continue;
        place(word, pen + gap);
        pen += gap + measure(word, token.size, true);
    }
    return rows;
}

/// The slant a synthetic italic gets. There is one face in this build -- bold
/// is a real Ubuntu-Bold, italic is not shipped at all -- so <i> is drawn the
/// way a browser draws a missing italic: by shearing the upright glyphs.
constexpr double kItalicShear = 0.21;   // ~12 degrees

/// One run of chat text.
///
/// The reference styles the whole box with a four-way one-pixel black
/// text-shadow plus a soft 3px glow, which is not a thing a stroke can be; two
/// black passes under the fill are what it comes out as. The fill is drawn on
/// its own so a translucent span (the timestamp) does not also thin its
/// outline, which is opaque in the reference.
void chatRun(Canvas& canvas, const std::string& s, double x, double baseline, double size,
             std::uint32_t fill, double alpha, bool italic = false, bool underline = false) {
    if (italic) {
        // Sheared about the baseline, so the run keeps its origin and the row
        // below is not walked into.
        canvas.save();
        canvas.translate(static_cast<float>(x), static_cast<float>(baseline));
        canvas.transform(1.0f, 0.0f, static_cast<float>(-kItalicShear), 1.0f, 0.0f, 0.0f);
        canvas.translate(static_cast<float>(-x), static_cast<float>(-baseline));
    }

    TextStyle style;
    style.size = size;
    style.bold = true;
    style.baseline = Baseline::Alphabetic;
    style.fill = kInk;
    style.stroke = kInk;

    style.strokeWidth = 3.0;
    canvas.setGlobalAlpha(0.8f);
    text(canvas, s, x, baseline, style);
    canvas.setGlobalAlpha(1.0f);

    style.strokeWidth = 2.0;
    if (alpha >= 1.0) {
        style.fill = fill;
        text(canvas, s, x, baseline, style);
    } else {
        text(canvas, s, x, baseline, style);
        style.strokeWidth = 0;
        style.fill = fill;
        canvas.setGlobalAlpha(static_cast<float>(alpha));
        text(canvas, s, x, baseline, style);
        canvas.setGlobalAlpha(1.0f);
    }

    if (underline) {
        // A hairline a tenth of the point size below the baseline, outlined
        // like the glyphs so it stays readable over the world behind it.
        const double width = measure(s, size, true);
        const double y = baseline + size * 0.1;
        canvas.beginPath();
        canvas.moveTo(static_cast<float>(x), static_cast<float>(y));
        canvas.lineTo(static_cast<float>(x + width), static_cast<float>(y));
        canvas.setLineWidth(static_cast<float>(std::max(1.0, size * 0.07) + 2.0));
        setStroke(canvas, kInk, 0.8);
        canvas.stroke();
        canvas.setLineWidth(static_cast<float>(std::max(1.0, size * 0.07)));
        setStroke(canvas, fill, alpha);
        canvas.stroke();
    }

    if (italic) canvas.restore();
}

} // namespace

App::App() = default;
App::~App() = default;

bool App::start(const AppConfig& config, std::string& errorOut) {
    config_ = config;

    // Without this every text call silently draws nothing, which looks like a
    // layout bug rather than a missing font.
    if (!Fonts::init(config.dataDir, errorOut)) {
        errorOut = "no usable font: " + errorOut;
        return false;
    }

    if (!loadContent(config.dataDir, errorOut)) return false;
    if (!sprites_.build(content(), config.dataDir)) {
        errorOut = "could not compile sprite artwork";
        return false;
    }
    for (const std::string& warning : sprites_.warnings()) {
        std::fprintf(stderr, "[sprites] %s\n", warning.c_str());
    }


    if (!window_.open(config.windowWidth, config.windowHeight, "florr", errorOut)) return false;
    // Everything below draws in design units, not pixels. Set before the
    // first frame, because the camera's viewport and every panel's layout are
    // read straight off window_.width()/height().
    window_.setDesignSize(kDesignWidth, kDesignHeight);

    renderer_.setContent(&content());
    renderer_.setSprites(&sprites_);
    // NetClient keeps this object alive for the entire connection and replaces
    // its grid with the authoritative TypeScript map when a game is joined.
    renderer_.setTerrain(&net_.terrain());
    net_.contentHash = content().contentHash();

    // The client needs one thing from the map's annotation layer: which biomes
    // the spawn picker may offer. A bundle it cannot read costs the picker its
    // choices, not the client its start.
    std::string mapWarning;
    if (!mapData_.load(config.dataDir + "/map_bundle.ts", mapWarning)) {
        std::fprintf(stderr, "[map] %s; the spawn picker will offer the garden only\n",
                     mapWarning.c_str());
    }
    // The browser build's picker, in its order: the garden, the two special
    // destinations, then every biome the map names.
    spawnChoices_.push_back("default");
    spawnChoices_.push_back("pvp");
    spawnChoices_.push_back("maze");
    for (const std::string& biome : mapData_.pickableBiomes()) spawnChoices_.push_back(biome);

    // The backdrop's petals are drawn from every petal a player could actually
    // own: admin-only art and the runtime egg petals are excluded, exactly as
    // the browser build's pickPetal filters them.
    for (std::uint16_t i = 0; i < content().petalCount(); ++i) {
        const PetalConfig& petal = content().petal(i);
        if (petal.isAdminPetal) continue;
        const std::string& id = petal.id;
        if (id.size() >= 4 && id.compare(id.size() - 4, 4, "_egg") == 0) continue;
        titlePetalTypes_.push_back(i);
    }

    // The endpoint the Advanced Settings drawer starts on: where this client
    // was pointed. The browser seeds the same field with its own origin.
    serverField_ = config_.host + ":" + std::to_string(config_.port);

    // Seeded from the clock, not a constant: this stream also mints guest
    // credentials, and a fixed seed would hand two clients started together
    // the same account name and the same password.
    titleRng_.reseed(static_cast<std::uint64_t>(wallClockMillis()));

    loadSession();
    // A missing settings file is a first run, not a failure: the defaults in
    // ClientSettings are already the shipped configuration.
    menus_.settings().load(settingsPath());
    // Before the first frame rather than only from frame(): a --frames run
    // short enough to be one screenshot would otherwise photograph the
    // default resolution whatever the file says.
    window_.setRenderScale(menus_.settings().renderScale);
    if (!net_.connect(config.host, config.port)) {
        errorOut = net_.lastError();
        return false;
    }

    if (config.autoMenu != MenuId::None) menus_.toggle(config.autoMenu);

    // Seeded before the first frame rather than after the join, so a
    // --frames run short enough to be one screenshot still photographs them.
    // No author: these stand in for the server's own announcements, which are
    // the lines that carry markup.
    for (const std::string& line : config.seedChat) net_.addLocalChat({}, line);

    screen_ = Screen::Connecting;
    running_ = true;
    return true;
}

void App::run() {
    while (step()) {
    }
    shutdown();
}

void App::persist() {
    // Written on the way out rather than on every toggle: this is a handful of
    // switches, and a file write per click would be absurd. The flower's name
    // is remembered on the same terms.
    menus_.settings().save(settingsPath());
    saveSession();
}

void App::shutdown() {
    persist();
}

bool App::step() {
    if (!running_ || !window_.pump()) return false;

    const double dt = window_.frameDelay(60.0);
    timeSeconds_ = window_.timeSeconds();
    frame(dt);
    // Measured around frame() and not off `dt`: dt includes the sleep that
    // frameDelay just took, so it reports the cap rather than the cost.
    const double frameMillis = (window_.timeSeconds() - timeSeconds_) * 1000.0;
    frameTimeAccum_ += frameMillis;
    ++frameTimeSamples_;
    runFrameAccum_ += frameMillis;
    ++runFrameSamples_;
    const WorldRenderer::SectionTiming& section = renderer_.sectionTiming();
    sectionMobs_.accumMillis += section.mobsMillis;
    sectionItems_.accumMillis += section.itemsMillis;
    sectionProjectiles_.accumMillis += section.projectilesMillis;
    sectionMobs_.windowPeakMillis =
        std::max(sectionMobs_.windowPeakMillis, section.mobsMillis);
    sectionItems_.windowPeakMillis =
        std::max(sectionItems_.windowPeakMillis, section.itemsMillis);
    sectionProjectiles_.windowPeakMillis =
        std::max(sectionProjectiles_.windowPeakMillis, section.projectilesMillis);
    sectionItemCount_ = section.itemCount;

    if (config_.screenshotAfterFrames > 0 &&
        ++framesDrawn_ >= config_.screenshotAfterFrames) {
        if (!config_.screenshotPath.empty()) {
            window_.canvas().savePPM(config_.screenshotPath);
            std::fprintf(stderr, "wrote %s\n", config_.screenshotPath.c_str());
        }
        // The cost of the work, with the 60Hz sleep excluded -- see
        // runFrameAccum_. This is the number to compare between two builds.
        if (runFrameSamples_ > 0) {
            const double avg = runFrameAccum_ / runFrameSamples_;
            std::fprintf(stderr, "frames=%d avg=%.2fms/frame (%.1f fps uncapped)\n",
                         runFrameSamples_, avg, avg > 0 ? 1000.0 / avg : 0.0);
        }
        running_ = false;
    }

    return running_;
}

void App::pollNetwork() {
    // Zero timeout: the frame loop sets the cadence, and blocking here would
    // couple frame rate to packet arrival.
    net_.poll(0);

    // The server is serving a build this one is not. Nothing here can talk to
    // it, and the bytes that could are on the server: on the web that is a
    // page load away, so take it. A native client has no such move and falls
    // through to the refusal message the handshake already left in lastError().
    if (net_.staleBuild) {
        net_.staleBuild = false;
        web::reloadForStaleBuild();
    }

    // The socket came back -- see onReconnected for why that is not simply
    // carrying on where the drop happened.
    if (net_.reconnected) {
        net_.reconnected = false;
        onReconnected();
    }

    // A drop mid-game does NOT take the game off the screen: the world, the
    // HUD and the panels keep drawing and a banner says what happened. Only a
    // failure before the player ever had a body replaces the screen.
    if (net_.status() == NetClient::Status::Failed && screen_ != Screen::Disconnected &&
        screen_ != Screen::Playing && screen_ != Screen::Dead) {
        screen_ = Screen::Disconnected;
    }
    if (net_.authAnswered) {
        net_.authAnswered = false;
        loginMessage_ = net_.authMessage;
        if (net_.authStatus == net::AuthStatus::Ok) {
            passwordField_.clear();
            confirmPasswordField_.clear();
            loginMessage_.clear();
            autoLogin_ = AutoLogin::Done;
            focusedField_ = -1;
            saveSession();
            screen_ = Screen::Lobby;
        }
    }
    if (net_.dead() && screen_ == Screen::Playing) {
        // Nothing is closed on death. An open panel and the icon strip keep
        // rendering under the scrim, exactly as they do in the reference.
        deathCardVisible_ = true;
        screen_ = Screen::Dead;
    }
}

void App::frame(double dt) {
    pollNetwork();

    // The pointer starts every frame as an arrow and whatever is under it says
    // otherwise, which is how the reference works: `canvas.style.cursor` is
    // reassigned on each move and falls back to the sheet's `cursor: default`.
    // Without the reset a panel that closed under a hand cursor would leave it.
    window_.setCursorShape(CursorShape::Arrow);

    // A once-a-second frame count, which is what the counters report -- an
    // instantaneous 1/dt jitters too much to read.
    ++frameCounter_;
    if (timeSeconds_ - fpsWindowStart_ >= 1.0) {
        framesPerSecond_ = frameCounter_;
        frameCounter_ = 0;
        fpsWindowStart_ = timeSeconds_;
        // Rolled over with the frame count, as the reference rolls its own
        // (src/game.ts:1252-1265): one number a second, not sixty.
        frameTimeAvgMs_ = frameTimeSamples_ > 0 ? frameTimeAccum_ / frameTimeSamples_ : 0.0;
        // The per-layer figures roll on the same boundary and over the same
        // sample count, so "Render avg/peak" always adds up against the frame
        // time printed beside it.
        for (SectionStats* section : {&sectionMobs_, &sectionItems_, &sectionProjectiles_}) {
            section->avgMillis =
                frameTimeSamples_ > 0 ? section->accumMillis / frameTimeSamples_ : 0.0;
            section->peakMillis = section->windowPeakMillis;
            section->accumMillis = 0;
            section->windowPeakMillis = 0;
        }
        frameTimeAccum_ = 0;
        frameTimeSamples_ = 0;
        // Counters are drained here and nowhere else, which is what makes the
        // figures bytes per SECOND rather than bytes since some other event.
        net_.takeWireStats(incomingBytesPerSecond_, outgoingBytesPerSecond_, topWireEvents_);
    }

    // The render-resolution setting reaches the window here rather than from
    // the settings panel, for the same reason the renderer's switches do
    // below: one place copies the settings out, and the panel never reaches
    // into anything. setRenderScale ignores a value it already has, so this
    // costs nothing on the frames where nothing moved.
    window_.setRenderScale(menus_.settings().renderScale);

    Canvas& canvas = window_.canvas();
    // The frame's base transform: design units -> canvas pixels. Every draw
    // call below is in design units, and this is the only place that knows
    // how big a design unit is. resetTransform first because the canvas
    // persists across frames and a save() a screen forgot to balance would
    // otherwise compound frame after frame.
    canvas.resetTransform();
    const float uiScale = static_cast<float>(window_.uiScale());
    canvas.scale(uiScale, uiScale);
    camera_.setViewport(window_.width(), window_.height());

    // Before any screen sees this frame's click. The browser's tutorial box is
    // a DOM element over the canvas: it takes the press first and the game
    // never hears about it, which is what the capturesMouse() guards below
    // stand in for.
    if (screen_ == Screen::Playing || screen_ == Screen::Dead) {
        tutorial_.update(window_, menus_.settings(), net_.profile(), timeSeconds_);
    }

    // Every drawn position advances here, BEFORE the screens see the frame.
    // The cursor control law measures from the flower's drawn position and the
    // camera pins to it, so easing after them would steer and frame the world
    // from a position one frame stale.
    if (screen_ == Screen::Playing || screen_ == Screen::Dead) {
        net_.view().easeRatePerSecond = easeRateFromAmount(menus_.settings().interpolation);
        // renderClockMillis(), not the window's clock: snapshot arrivals are
        // stamped against this one, and mob playback has to be measured on
        // the same timeline it is stamped on.
        net_.view().interpolate(renderClockMillis(), dt);
    }

    switch (screen_) {
        case Screen::Connecting:   updateConnecting(); break;
        case Screen::Login:        updateLogin(dt); break;
        case Screen::Lobby:        updateLobby(dt); break;
        case Screen::Playing:      updatePlaying(dt); break;
        case Screen::Dead:         updateDead(dt); break;
        case Screen::Disconnected: break;
    }

    // One heartbeat a second while the socket is up, as the reference's own
    // interval does. It is what makes the ping readout a number.
    // Every status the socket can be in once the handshake has passed: Ready
    // is only the gap before login, and a heartbeat that stopped there would
    // leave the readout on "--" for the whole session -- which is exactly what
    // it did.
    const NetClient::Status status = net_.status();
    const bool socketUp = status == NetClient::Status::Ready ||
                          status == NetClient::Status::LoggedIn ||
                          status == NetClient::Status::Playing;
    if (socketUp && timeSeconds_ >= nextPingSeconds_) {
        net_.sendPing();
        nextPingSeconds_ = timeSeconds_ + 1.0;
    }

    // Every frame, on every screen, open or not: the debug panel's graphs are
    // meant to already hold history when it is opened. See
    // DebugPanel::recordFrame.
    menus_.recordDebugSample(dt, net_);

    // The renderer's switches live in the settings menu, so they are copied
    // across every frame rather than the menu reaching into the renderer.
    renderer_.options = menus_.settings().render;
    camera_.userZoom = menus_.settings().zoom;
    const bool inWorld = screen_ == Screen::Playing || screen_ == Screen::Dead;
    menus_.setInGame(inWorld);
    if (menus_.takeExitRequest() && inWorld) leaveToTitle();
    // No inWorld guard: Settings' Log Out is offered on the title screen too,
    // and it is the one action that has to work from either of them.
    if (menus_.takeLogoutRequest()) logout();

    // --- draw -------------------------------------------------------------
    if (inWorld) {
        renderer_.ingestEvents(net_.view());
        renderer_.update(dt);
        // Pinned, not eased: the reference keeps the flower exactly on the
        // screen centre, which is what the cursor-relative control law reads.
        // The EASE is on the flower itself, one frame earlier -- see frame().
        const Vec2 selfDrawn = net_.view().selfDrawnPosition();
        camera_.snapTo(selfDrawn);
        renderer_.draw(canvas, net_.view(), camera_, selfDrawn, timeSeconds_);
        drawHud(canvas, timeSeconds_);
        // The reference hides the whole chat box while one of the three
        // petal-handling panels is up, rather than letting it poke out beside
        // the card.
        const MenuId open = menus_.open();
        const bool panelHidesChat = open == MenuId::Inventory || open == MenuId::Crafting ||
                                    open == MenuId::Gallery;
        if (menus_.settings().showChat && !panelHidesChat) drawChat(canvas, timeSeconds_);
        // Panels and the icon strip keep drawing while dead, and the scrim goes
        // over both -- but NOT over the loadout bar, which the reference paints
        // in Game once graphics.render() has already laid the death screen
        // down. Handing the card to the menus as their between-strip-and-bar
        // slot is the only way to land it there. Only the paint moves: the
        // card's buttons are still answered by updateDead, before any of this.
        menus_.render(canvas, window_, net_, sprites_, renderer_, timeSeconds_, dt, [&] {
            if (screen_ == Screen::Dead && deathCardVisible_) drawDeathCard(canvas, timeSeconds_);
        });
        if (net_.status() == NetClient::Status::Failed) drawDisconnectBanner(canvas);
        // Ping and the rest live here and nowhere else: the reference has no
        // always-on latency readout, only this opt-in corner.
        if (statsVisible()) drawStatsCounters(canvas, false);
        // Over every other layer but the wipe: the tutorial box is z-index
        // 9999, above the panels, the strip, the loadout bar and the death
        // card. Its one anchored step rings the crafting panel, which is the
        // only element in the reference that `.tutorial-highlight` can ever
        // find -- see the note in Tutorial::draw.
        tutorial_.draw(canvas, timeSeconds_,
                       menus_.open() == MenuId::Crafting
                           ? CraftingPanel::bounds(window_.width(), window_.height())
                           : Rect{});
    } else {
        drawTitleBackground(canvas, timeSeconds_);
        if (screen_ == Screen::Login) {
            drawLogin(canvas, timeSeconds_);
            // The strip paints OVER the form, after it, exactly as
            // canvasButtons.draw runs after authForm.render. Only the strip:
            // the browser hides the loadout bar for as long as the auth form
            // is up, and there is nothing logged in yet for a panel to show.
            menus_.renderStripOnly(canvas, window_, timeSeconds_);
        }
        else if (screen_ == Screen::Lobby) {
            drawLobby(canvas, timeSeconds_);
            // The same menus, on the title screen. The panels read the account
            // rather than the world, so there is nothing for them to miss here.
            menus_.render(canvas, window_, net_, sprites_, renderer_, timeSeconds_, dt);
            // Over the panels, as the reference's own always-on-top widget is.
            drawDailyStreak(canvas, timeSeconds_);
        }
        else drawConnectionState(canvas, timeSeconds_);
    }

    // Last of all, over every other layer: the wipe is what hides the seam
    // between two scenes, so nothing may paint on top of it.
    drawSceneWipe(canvas);
    window_.present();
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

void App::updateConnecting() {
    if (net_.status() != NetClient::Status::Ready) return;

    // --login is the only way to photograph the auth form on a machine that
    // has logged in before: a stored token would otherwise resume past it.
    if (config_.forceLogin) storedToken_.clear();

    if (!config_.forceLogin && !config_.autoUsername.empty()) {
        // A scripted run has no one to type. It also must not race a stored
        // token: the two answers would arrive interleaved and whichever lost
        // would look like a rejection.
        storedToken_.clear();
        usernameField_ = config_.autoUsername;
        passwordField_ = config_.autoPassword;
        autoLogin_ = AutoLogin::Registering;
        net_.requestRegister(config_.autoUsername, config_.autoPassword);
    } else if (!storedToken_.empty()) {
        // A returning player skips the form entirely. If the server rejects
        // the token, handleAuthResult clears its own copy and the login form
        // is already the screen we are on, so there is nothing to unwind.
        net_.resumeSession(storedToken_);
        storedToken_.clear();
    }
    screen_ = Screen::Login;
}

bool App::keyboardCaptured() const {
    // The three things that can hold the caret, in the order updateLobby and
    // updatePlaying resolve them: a panel's field, the chat line, and the
    // lobby's name / auth fields.
    return menus_.wantsText() || chatOpen_ || nameFocused_ || focusedField_ >= 0;
}

void App::editText(std::string& target, std::size_t maxLength) {
    const std::string& typed = window_.typedText();
    for (std::size_t i = 0; i < typed.size(); ++i) {
        if (target.size() >= maxLength) break;
        const unsigned char c = static_cast<unsigned char>(typed[i]);
        if (c >= 0x20) target += typed[i];
    }
    if (window_.keyPressed(Key::Backspace) && !target.empty()) {
        // Erase a whole UTF-8 sequence, not a byte: deleting one byte of a
        // multi-byte character leaves an invalid string behind.
        std::size_t at = target.size() - 1;
        while (at > 0 && (static_cast<unsigned char>(target[at]) & 0xC0) == 0x80) --at;
        target.erase(at);
    }
}

void App::editChatLine() {
    const std::string before = chatDraft_;
    editText(chatDraft_, 180);
    // The list opens on the first '/' and re-selects its top row whenever the
    // filter changes, which is what the reference's `input` handler does.
    if (chatDraft_ != before) {
        chatSuggestion_ = (!chatDraft_.empty() && chatDraft_[0] == '/') ? 0 : -1;
    }

    if (chatSuggestion_ >= 0) {
        const std::vector<const ChatCommand*> matches = matchChatCommands(chatDraft_, net_.isSkinAdmin());
        if (matches.empty()) {
            chatSuggestion_ = -1;
        } else {
            const int last = static_cast<int>(matches.size()) - 1;
            if (chatSuggestion_ > last) chatSuggestion_ = last;
            if (window_.keyPressed(Key::Down)) {
                chatSuggestion_ = std::min(chatSuggestion_ + 1, last);
            }
            if (window_.keyPressed(Key::Up)) chatSuggestion_ = std::max(chatSuggestion_ - 1, 0);
            // Completing wins over sending: with a row highlighted, Enter fills
            // the command in rather than posting a half-typed one.
            if (window_.keyPressed(Key::Tab) || window_.keyPressed(Key::Enter)) {
                const ChatCommand* chosen = matches[static_cast<std::size_t>(chatSuggestion_)];
                chatDraft_ = std::string(chosen->command) + " ";
                chatSuggestion_ = -1;
                return;
            }
            if (window_.keyPressed(Key::Escape)) {
                chatSuggestion_ = -1;
                return;
            }
        }
    }

    if (window_.keyPressed(Key::Enter)) {
        if (!chatDraft_.empty() && !handleClientCommand(chatDraft_)) net_.sendChat(chatDraft_);
        chatDraft_.clear();
        chatOpen_ = false;
    } else if (window_.keyPressed(Key::Escape)) {
        chatDraft_.clear();
        chatOpen_ = false;
    }
}

void App::updateLogin(double dt) {
    (void)dt;
    if (net_.status() == NetClient::Status::LoggedIn) { screen_ = Screen::Lobby; return; }

    if (!config_.autoUsername.empty()) {
        // A name already taken is the normal case on a second run against the
        // same database, so fall back to logging in exactly once. Anything
        // else is a real refusal and must not be retried in a loop.
        if (autoLogin_ == AutoLogin::Registering &&
            net_.authStatus != net::AuthStatus::Ok) {
            if (net_.authStatus == net::AuthStatus::UsernameTaken) {
                autoLogin_ = AutoLogin::LoggingIn;
                net_.requestLogin(config_.autoUsername, config_.autoPassword);
            } else {
                autoLogin_ = AutoLogin::Failed;
            }
        }
        return;
    }

    // An action deferred while the socket was pointed at a new endpoint. The
    // request itself could not travel: it would have gone out on the socket
    // that was being replaced.
    if (!pendingAuth_.empty() && net_.status() == NetClient::Status::Ready) {
        const std::string action = pendingAuth_;
        pendingAuth_.clear();
        submitAuth(action);
        return;
    }

    const AuthLayout layout = authLayout(window_.width(), window_.height(), registering_,
                                         advancedOpen_);
    const Vec2 mouse{window_.mouseX(), window_.mouseY()};

    // Tab cycles the visible fields only, so it never parks the caret on a
    // field the disclosure has folded away. With nothing focused it does
    // nothing at all: the reference's handler opens `if (!this.focusedField)
    // return false;` (auth_form.ts:389-390), so no key reaches the form until
    // the player has clicked into it.
    const int fieldCount = (registering_ ? 3 : 2) + (advancedOpen_ ? 1 : 0);
    if (focusedField_ >= 0 && window_.keyPressed(Key::Tab)) {
        focusedField_ = (focusedField_ + 1) % fieldCount;
    }

    // The browser's per-field caps. A field the reference cuts at fifty must
    // not accept sixty-four here and then be refused by the server.
    const int serverField = registering_ ? 3 : 2;
    if (focusedField_ == 0) editText(usernameField_, 50);
    else if (focusedField_ == 1) editText(passwordField_, 100);
    else if (registering_ && focusedField_ == 2) editText(confirmPasswordField_, 100);
    // The endpoint field alone is uncapped: auth_form.ts:414 falls through to a
    // bare `this.serverIP += e.key`, and only the three credential fields carry
    // a maxlength.
    else if (advancedOpen_ && focusedField_ == serverField) {
        editText(serverField_, std::numeric_limits<std::size_t>::max());
    }

    // Enter submits only while a field has the caret: the browser's form is a
    // set of inputs, and a key event never reaches it otherwise.
    if (focusedField_ >= 0 && window_.keyPressed(Key::Enter)) {
        submitAuth(registering_ ? "register" : "login");
        return;
    }

    // The icon strip is painted over this form and answers for its own
    // buttons. A click it takes must not also press a control behind it, nor
    // blur the field the player was typing in.
    if (menus_.capturesMouse(mouse)) {
        pressedControl_.clear();
        return;
    }

    if (window_.mousePressed(MouseButton::Left)) {
        pressedControl_ = authControlAt(layout, registering_, mouse);
    }
    if (!window_.mouseReleased(MouseButton::Left)) return;
    pressedControl_.clear();

    const std::string control = authControlAt(layout, registering_, mouse);
    if (!control.empty()) {
        submitAuth(control);
        return;
    }

    // Anything that is not a control and not a field blurs, which restores
    // both placeholders and takes the caret away.
    if (hit(layout.username, mouse)) focusedField_ = 0;
    else if (hit(layout.password, mouse)) focusedField_ = 1;
    else if (registering_ && hit(layout.confirmation, mouse)) focusedField_ = 2;
    else if (advancedOpen_ && hit(layout.serverIp, mouse)) focusedField_ = serverField;
    else focusedField_ = -1;
}

void App::submitAuth(const std::string& action) {
    if (action == "toggleAdvanced") {
        advancedOpen_ = !advancedOpen_;
        // Folding the drawer away must not leave the caret on a field nobody
        // can see any more.
        if (!advancedOpen_ && focusedField_ >= (registering_ ? 3 : 2)) focusedField_ = -1;
        return;
    }
    if (action == "showRegister" || action == "showLogin") {
        registering_ = (action == "showRegister");
        focusedField_ = -1;
        loginMessage_.clear();
        return;
    }
    if (action == "offline") {
        // There is no offline simulation to register against: this client is
        // a network client and nothing else. The button is drawn because the
        // reference draws it, and says so rather than doing nothing.
        loginMessage_ = "Offline play is not available in this client";
        return;
    }

    // A changed endpoint has to be reached before anything can be asked of it,
    // and the request cannot ride the socket that is being replaced.
    if (retargetServer()) {
        pendingAuth_ = action;
        return;
    }

    loginMessage_.clear();
    if (action == "guest") {
        // The browser mints User<8 digits> / password<10 digits> and registers
        // them. Registering here also logs in, so there is no second step.
        const std::string name = "User" + std::to_string(titleRng_.below(100000000u));
        const std::string password =
            "password" + std::to_string(titleRng_.next() % 10000000000ull);
        usernameField_ = name;
        passwordField_ = password;
        net_.addSystemMessage("Guest account " + name + " / " + password +
                              " -- write it down, it is not stored");
        net_.requestRegister(name, password);
        return;
    }
    if (action == "register") {
        if (passwordField_ != confirmPasswordField_) {
            loginMessage_ = "Passwords do not match";
            return;
        }
        net_.requestRegister(usernameField_, passwordField_);
        return;
    }
    net_.requestLogin(usernameField_, passwordField_);
}

bool App::retargetServer() {
    if (serverField_.empty()) return false;

    std::string host = serverField_;
    std::uint16_t port = config_.port;
    // "host", "host:port". A bracketed IPv6 literal is not offered here, and
    // the last colon rule would misread one, so only a single colon counts.
    const std::size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) {
        const int parsed = std::atoi(host.c_str() + colon + 1);
        if (parsed > 0 && parsed <= 65535) port = static_cast<std::uint16_t>(parsed);
        host.erase(colon);
    }
    if (host.empty() || (host == config_.host && port == config_.port)) return false;

    config_.host = host;
    config_.port = port;
    loginMessage_.clear();
    net_.connect(host, port);
    return true;
}

void App::updateLobby(double dt) {
    (void)dt;
    if (config_.autoJoin && !config_.autoUsername.empty() &&
        net_.status() != NetClient::Status::Playing) {
        startGame();
    }

    const Vec2 mouse{window_.mouseX(), window_.mouseY()};
    const LobbyLayout layout = lobbyLayout(window_.width(), window_.height());

    // Exactly one thing owns the keyboard at a time. A panel's own search box
    // outranks the chat line, which outranks the name field, which outranks
    // the menu hotkeys -- otherwise typing a name opens half the menus.
    if (menus_.wantsText()) {
        // A panel is taking the keystrokes; nothing here may also read them.
    } else if (chatOpen_) {
        editChatLine();
    } else if (nameFocused_) {
        editText(playerName_, 20);
        // Enter starts the game only from here, which is the one place the
        // reference accepts it: with nothing focused, Enter opens chat.
        if (window_.keyPressed(Key::Enter)) {
            nameFocused_ = false;
            startGame();
        } else if (window_.keyPressed(Key::Escape)) {
            nameFocused_ = false;
        }
    } else if (!menus_.handleKeys(window_)) {
        if (window_.keyPressed(Key::Enter)) chatOpen_ = true;
    }

    // A release anywhere ends the press, including one a panel swallowed --
    // otherwise a button stays lit after a click that went somewhere else.
    if (window_.mouseReleased(MouseButton::Left)) pressedControl_.clear();

    // A click that landed on a panel or on the icon strip was for the menus,
    // and must not also start a game behind them.
    if (!menus_.capturesMouse(mouse)) {
        if (window_.mousePressed(MouseButton::Left)) {
            pressedControl_.clear();
            if (hitInclusive(layout.ready, mouse)) pressedControl_ = "start";
            for (std::size_t i = 0; i < layout.biomes.size(); ++i) {
                if (hitInclusive(layout.biomes[i], mouse)) {
                    pressedControl_ = "biome_" + std::to_string(i);
                }
            }
        }
        if (window_.mouseReleased(MouseButton::Left)) {
            bool onBiome = false;
            for (std::size_t i = 0; i < layout.biomes.size(); ++i) {
                if (!hitInclusive(layout.biomes[i], mouse)) continue;
                onBiome = true;
                // "default" is stored as no choice at all, so a player who
                // never touches this row is not pinned to a biome that may be
                // edited out of the map later.
                menus_.settings().spawnBiome =
                    spawnChoices_[i] == "default" ? std::string() : spawnChoices_[i];
            }

            // Focus follows the click. Ready and the biome row are the two
            // things the reference lets you click WITHOUT losing the caret in
            // the name field; everything else blurs it.
            if (hitInclusive(layout.name, mouse)) nameFocused_ = true;
            else if (!onBiome && !hitInclusive(layout.ready, mouse)) nameFocused_ = false;
            chatOpen_ = hit(titleChatBox(window_.width(), window_.height()), mouse);

            if (hitInclusive(layout.ready, mouse)) startGame();
        }
    }

    if (net_.status() == NetClient::Status::Playing) {
        net_.view().snapAll();
        camera_.snapTo(net_.view().selfDrawnPosition());
        beginSceneWipe(true);
        // --dead is the only route a scripted run has to the death card: being
        // killed for real is not something `--frames` can arrange.
        screen_ = config_.autoDead ? Screen::Dead : Screen::Playing;
        deathCardVisible_ = true;
        // The browser starts the tutorial a second after the game's socket
        // authenticates, which is this moment: Game builds the Tutorial, and
        // reaching the world is what a Game exists for.
        //
        // A scripted login is not a player, and the card would cover a quarter
        // of every other capture -- which is precisely why the browser's own
        // harness writes `tutorial_completed` before those joins and clears it
        // for the one shot that wants the card. --tutorial is that shot.
        if (config_.autoTutorial || config_.autoUsername.empty()) {
            tutorial_.beginGame(menus_.settings(), timeSeconds_, config_.autoTutorial);
        }
    }
}

void App::startGame() {
    net_.joinGame(window_.width(), window_.height(), menus_.settings().spawnBiome, playerName_);
}

void App::sendInputFrame(double dt) {
    net::InputFrame input;
    input.sequence = ++inputSequence_;

    // What the camera actually draws, in world units rather than pixels: the
    // render scales by userZoom, so zooming out shows more world through the
    // same window and the server has to widen what it streams to match. It
    // rides every frame, including the menu-open one below, so a resize or a
    // wheel zoom takes effect on the next tick rather than at the next join.
    const double zoom = camera_.zoom() > 1e-6 ? camera_.zoom() : 1.0;
    input.viewportWidth = static_cast<std::uint16_t>(
        std::min(65535.0, std::round(window_.width() / zoom)));
    input.viewportHeight = static_cast<std::uint16_t>(
        std::min(65535.0, std::round(window_.height() / zoom)));

    // An open menu owns the pointer. Keep sending zero movement so the flower
    // stops while an item is being dragged, rather than steering toward the
    // panel under the mouse.
    if (menus_.anyOpen()) {
        net_.sendInput(input);
        return;
    }

    // Movement follows the cursor, which is the control scheme this game is
    // built around: the flower runs toward the pointer, at a speed set by how
    // far away it is. The movement keys are offered as an alternative rather
    // than a supplement, and win when held so the two cannot fight -- and
    // "Use Mouse Controls", which the K binding toggles in game, is what says
    // whether the cursor half is there at all. The arrows are not a binding:
    // the reference reads them beside whatever the four keys are bound to.
    const ClientSettings& settings = menus_.settings();
    Vec2 keyboard{0, 0};
    // A typed key is a character, not a direction. The reference's key handler
    // returns before it records anything once an <input> has focus, so while
    // the chat line -- or a panel's own field -- is taking keystrokes, none of
    // them reach the movement set. The cursor half keeps working: only the
    // keyboard half goes quiet.
    if (!keyboardCaptured()) {
        if (boundKeyDown(window_, settings.controlKey(ControlAction::MoveUp)) ||
            window_.keyDown(Key::Up)) {
            keyboard.y -= 1;
        }
        if (boundKeyDown(window_, settings.controlKey(ControlAction::MoveDown)) ||
            window_.keyDown(Key::Down)) {
            keyboard.y += 1;
        }
        if (boundKeyDown(window_, settings.controlKey(ControlAction::MoveLeft)) ||
            window_.keyDown(Key::Left)) {
            keyboard.x -= 1;
        }
        if (boundKeyDown(window_, settings.controlKey(ControlAction::MoveRight)) ||
            window_.keyDown(Key::Right)) {
            keyboard.x += 1;
        }
    }

    const Vec2 cursorWorld = camera_.screenToWorld({window_.mouseX(), window_.mouseY()});
    const Vec2 toCursor = cursorWorld - net_.view().selfDrawnPosition();

    if (keyboard.lengthSq() > 0) {
        const Vec2 direction = keyboard.normalized();
        input.moveAngle = direction.angle();
        input.moveStrength = 1.0;
    } else if (settings.useMouseControls) {
        const double distance = toCursor.length();
        input.moveAngle = distance > 1e-6 ? toCursor.angle() : 0.0;
        input.moveStrength = std::min(1.0, distance / kFullSpeedCursorDistance);
    } else {
        // Standing still is a decision, not a gap: with the cursor half off,
        // a frame with no key held has to say "no movement" rather than leave
        // the last angle to be read as one.
        input.moveAngle = 0.0;
        input.moveStrength = 0.0;
    }

    // Aim always follows the cursor, even under keyboard movement: where the
    // petals point and where you walk are separate decisions.
    input.aimAngle = toCursor.lengthSq() > 1e-12 ? toCursor.angle() : 0.0;

    if (!chatOpen_ && !menus_.capturesMouse({window_.mouseX(), window_.mouseY()}) &&
        !tutorial_.capturesMouse({window_.mouseX(), window_.mouseY()})) {
        if (window_.mouseDown(MouseButton::Left) ||
            boundKeyDown(window_, settings.controlKey(ControlAction::ExtendPetals))) {
            input.flags |= net::InputAttack;
        }
        if (window_.mouseDown(MouseButton::Right) ||
            boundKeyDown(window_, settings.controlKey(ControlAction::RetractPetals))) {
            input.flags |= net::InputDefend;
        }
    }

    net_.sendInput(input);
}

void App::updatePlaying(double dt) {
    // Chat swallows the keyboard while open, or typing would also drive the
    // flower and trip every hotkey.
    if (chatOpen_ && !menus_.wantsText()) {
        editChatLine();
    } else {
        // The menus get first refusal on the keyboard: a hotkey they claim is
        // not also a chat key, and Escape closes a panel before it leaves the
        // game.
        const bool consumed = menus_.handleKeys(window_);
        if (!consumed) {
            if (boundKeyPressed(window_, menus_.settings().controlKey(ControlAction::Chat))) {
                chatOpen_ = true;
            }
            if (window_.keyPressed(Key::Escape)) {
                leaveToTitle();
                return;
            }
        }
    }

    // Input is produced at the simulation rate rather than per rendered frame:
    // a 144 Hz client must not get six times the inputs of a 30 Hz one.
    inputAccumulator_ += dt;
    const double step = net::kTickSeconds;
    int produced = 0;
    while (inputAccumulator_ >= step && produced < 4) {
        inputAccumulator_ -= step;
        sendInputFrame(step);
        ++produced;
    }
    // A long stall must not queue a burst of catch-up input.
    if (inputAccumulator_ > step * 4) inputAccumulator_ = 0;

    // The wheel zooms the camera unless a panel -- or the tutorial box, which
    // is a DOM element and eats the event before the canvas -- is over it.
    if (!menus_.capturesMouse({window_.mouseX(), window_.mouseY()}) &&
        !tutorial_.capturesMouse({window_.mouseX(), window_.mouseY()})) {
        menus_.settings().zoom =
            clamp(menus_.settings().zoom + window_.wheelDelta() * 0.05, kMinZoom, kMaxZoom);
    }
}

void App::updateDead(double dt) {
    (void)dt;
    // ENTER is the Continue button by another name, and stays live after the
    // card has been dismissed: it is gated on being dead, not on the card.
    if (window_.keyPressed(Key::Enter)) {
        leaveToTitle();
        return;
    }
    if (!deathCardVisible_) return;

    // The reference acts on the press, not the release, so the card is gone by
    // the time the button comes back up and a pressed state is never seen.
    if (!window_.mousePressed(MouseButton::Left)) return;

    const Vec2 mouse{window_.mouseX(), window_.mouseY()};
    // The tutorial box is painted over the death card and swallows the click.
    if (tutorial_.capturesMouse(mouse)) return;
    const double centreX = window_.width() * 0.5;
    const double centreY = window_.height() * 0.5;
    if (hit(Rect{centreX - 100, centreY + 30, 200, 50}, mouse)) {
        leaveToTitle();
        return;
    }
    // Close only takes the card away. The player stays dead, and the world,
    // the HUD and the minimap keep drawing behind where it was.
    if (hit(Rect{centreX - 70, centreY + 95, 140, 36}, mouse)) deathCardVisible_ = false;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void App::drawConnectionState(Canvas& canvas, double time) {
    TextStyle style;
    style.size = kHeadingSize;
    style.align = Align::Centre;

    if (screen_ == Screen::Disconnected) {
        style.fill = kDanger;
        text(canvas,
             net_.lastError().empty() ? "Disconnected" : ("Disconnected: " + net_.lastError()),
             canvas.width() * 0.5, canvas.height() * 0.5, style);
        // Said only while it is true. A handshake the server REFUSED is not
        // redialled -- see NetClient::handleWelcome -- and promising a
        // reconnection that is never coming is worse than the bare refusal.
        if (net_.reconnecting()) {
            style.size = 18.0;
            text(canvas, "Reconnecting...", canvas.width() * 0.5, canvas.height() * 0.5 + 34.0,
                 style);
        }
        return;
    }

    // The reference's connecting screen is the title with one static line
    // under it -- deliberately not an animated ellipsis, which reads as a
    // progress bar for something that has no progress to report.
    TextStyle title;
    title.size = 48.0;
    title.align = Align::Centre;
    title.bold = true;
    title.strokeWidth = 6.0;
    text(canvas, "flowrix beta", canvas.width() * 0.5, canvas.height() * 0.5 - 200.0, title);

    // Half the title's point size and a heavier outline than it: the line
    // under the heading is a status, not a second heading
    // (src/title_screen/index.ts:1264-1265).
    style.size = 24.0;
    style.bold = true;
    style.strokeWidth = 4.0;
    text(canvas, "Connecting...", canvas.width() * 0.5, canvas.height() * 0.5, style);
    if (statsVisible()) drawStatsCounters(canvas, true);
    (void)time;
}

void App::drawLogin(Canvas& canvas, double time) {
    const AuthLayout layout = authLayout(canvas.width(), canvas.height(), registering_,
                                         advancedOpen_);
    const double centreX = canvas.width() * 0.5;
    const Vec2 mouse{window_.mouseX(), window_.mouseY()};
    // Nothing is hovered while the pointer is outside the window: the
    // reference's `mouseleave` calls authForm.clearHover() and drops the
    // latched press with it.
    const std::string hovered =
        pointerInWindow(window_) ? authControlAt(layout, registering_, mouse) : std::string{};
    const std::string pressed = pointerInWindow(window_) ? pressedControl_ : std::string{};

    TextStyle title;
    title.size = 48;
    title.align = Align::Centre;
    title.bold = true;
    title.strokeWidth = 6;
    // Four hundred above centre, unclamped: on a short window the reference
    // lets it run off the top, and pinning it to the edge instead would put a
    // second heading beside the form's own.
    text(canvas, "flowrix beta", centreX, canvas.height() * 0.5 - 400.0, title);

    TextStyle heading;
    heading.size = 28;
    heading.align = Align::Centre;
    heading.bold = true;
    heading.strokeWidth = 3;
    text(canvas, registering_ ? "Register" : "Login", centreX, layout.headingY, heading);

    // Every default of TextFieldStyle is already this form's: the green plate,
    // its own 0.8-value outline, radius 3, 4px slack / 5px focused, 18px text.
    const TextFieldStyle authField;
    const int serverField = registering_ ? 3 : 2;
    textField(canvas, layout.username,
              usernameField_, "Username", focusedField_ == 0, false, time, authField);
    textField(canvas, layout.password,
              passwordField_, "Password", focusedField_ == 1, true, time, authField);
    if (registering_) {
        textField(canvas, layout.confirmation, confirmPasswordField_, "Confirm Password",
                  focusedField_ == 2, true, time, authField);
    }
    if (advancedOpen_) {
        textField(canvas, layout.serverIp, serverField_, "Server IP",
                  focusedField_ == serverField, false, time, authField);
    }

    ButtonStyle advancedStyle;
    advancedStyle.fill = 0x7B2FA0u;
    advancedStyle.outlineWidth = 4.0;
    advancedStyle.radius = 5.0;
    advancedStyle.textSize = 14.0;
    button(canvas, layout.advanced,
           advancedOpen_ ? "Advanced Settings [^]" : "Advanced Settings [v]",
           hovered == "toggleAdvanced", pressed == "toggleAdvanced", advancedStyle);

    // No disabled state: the reference's buttons are full-colour with both
    // fields empty, and the refusal comes from the server, not from the paint.
    ButtonStyle actionStyle;
    actionStyle.fill = 0x8A2BE2u;
    actionStyle.radius = 5.0;
    actionStyle.textSize = 18.0;
    ButtonStyle quietStyle = actionStyle;
    quietStyle.fill = 0x6A1B9Au;

    if (registering_) {
        button(canvas, layout.action, "Register", hovered == "register",
               pressed == "register", actionStyle);
        button(canvas, layout.secondary, "Register Offline", hovered == "offline",
               pressed == "offline", quietStyle);

        TextStyle link;
        link.size = 14.0;
        link.align = Align::Centre;
        link.strokeWidth = 0;
        link.fill = hovered == "showLogin" ? kPaper : 0xE0B0FFu;
        text(canvas, "Already have an account? Login", centreX, layout.hintY, link);
    } else {
        button(canvas, layout.action, "Login", hovered == "login",
               pressed == "login", actionStyle);
        button(canvas, layout.secondary, "Register", hovered == "showRegister",
               pressed == "showRegister", actionStyle);
        button(canvas, layout.guest, "Guest", hovered == "guest",
               pressed == "guest", quietStyle);

        TextStyle hint;
        hint.size = 11.0;
        hint.align = Align::Centre;
        hint.strokeWidth = 0;
        hint.fill = 0xFF9800u;
        text(canvas, "Guest accounts do not keep progress", centreX, layout.hintY, hint);
    }

    // The reference raises its failures as browser dialogs, which this client
    // has no equivalent of; the wording is the reference's so the two say the
    // same thing, and the line sits below every control rather than over one.
    if (!loginMessage_.empty()) {
        TextStyle error;
        error.size = 14;
        error.align = Align::Centre;
        error.fill = kDanger;
        error.strokeWidth = 2;
        text(canvas, loginMessage_, centreX, layout.bottomY + 24, error);
    }
    if (statsVisible()) drawStatsCounters(canvas, true);
}

const SvgDocument* App::titleBackground(const std::string& biomeName) {
    // The browser build's BIOME_SVG_MAP. A biome without art of its own tiles
    // the garden's, which is what it does there too.
    static const std::unordered_map<std::string, std::string> kFiles = {
        {"default", "land.svg"},  {"land", "land.svg"},     {"desert", "desert.svg"},
        {"ocean", "ocean.svg"},   {"hel", "hel.svg"},       {"ant_hell", "ant_hell.svg"},
        {"sewers", "sewers.svg"}, {"jungle", "jungle.svg"},
    };
    const auto entry = kFiles.find(biomeName);
    const std::string file = entry == kFiles.end() ? "land.svg" : entry->second;

    // Compiled on first use and kept: a player flicking along the picker would
    // otherwise re-parse an SVG per frame.
    auto cached = titleBackgrounds_.find(file);
    if (cached == titleBackgrounds_.end()) {
        auto document = std::make_shared<SvgDocument>(
            SvgDocument::fromFile(config_.dataDir + "/" + file));
        cached = titleBackgrounds_.emplace(file, std::move(document)).first;
    }
    // A missing or unparseable backdrop is optional art, never a failure: the
    // painted fallback below still reads as ground.
    return cached->second->empty() ? nullptr : cached->second.get();
}

void App::drawTitleBackground(Canvas& canvas, double time) {
    const std::string& biome = menus_.settings().spawnBiome;
    const SvgDocument* texture = titleBackground(biome.empty() ? "default" : biome);

    // A fixed step per rendered frame rather than elapsed seconds: this is the
    // reference's own clock, and matching it is what keeps the two scrolling
    // at the same rate for the same number of frames.
    titleBackgroundTime_ += 16.0;

    if (texture) {
        // The camera runs a 2000px circle about the screen centre, so the
        // drift sweeps a full turn every five and a half minutes instead of
        // sliding down one diagonal forever. Speed is constant at r*omega.
        const double a = titleBackgroundTime_ * 0.00002;
        const double cameraX = canvas.width() * 0.5 + std::cos(a) * 2000.0;
        const double cameraY = canvas.height() * 0.5 + std::sin(a) * 2000.0;

        const double tileW = texture->width() > 0 ? texture->width() : 400.0;
        const double tileH = texture->height() > 0 ? texture->height() : 400.0;
        const double startX = std::floor(cameraX / tileW) * tileW;
        const double startY = std::floor(cameraY / tileH) * tileH;
        const int tilesX = static_cast<int>(std::ceil(canvas.width() / tileW)) + 2;
        const int tilesY = static_cast<int>(std::ceil(canvas.height() / tileH)) + 2;

        for (int i = 0; i <= tilesX; ++i) {
            for (int j = 0; j <= tilesY; ++j) {
                // Each tile is drawn two pixels oversized with the overlap
                // centred, and snapped to a whole pixel: a fractional origin
                // makes the seams crawl as the camera moves.
                const double x = std::floor(startX + i * tileW - cameraX - 1.0);
                const double y = std::floor(startY + j * tileH - cameraY - 1.0);
                // The counts above are the reference's, and they overshoot the
                // window by a tile in each direction; a tile that lands wholly
                // outside it is a whole SVG rasterized into nothing. On a
                // 1280x720 window that is twenty of the thirty-five.
                if (x + tileW + 2 <= 0 || y + tileH + 2 <= 0 ||
                    x >= canvas.width() || y >= canvas.height()) {
                    continue;
                }
                texture->renderFitted(canvas, static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(tileW + 2),
                                      static_cast<float>(tileH + 2),
                                      static_cast<float>(time));
            }
        }
    } else {
        // No art: a flat field, which is what the reference falls back to. A
        // pattern here would be a second design nobody asked for.
        setFill(canvas, kBackdrop);
        canvas.fillRect(0, 0, static_cast<float>(canvas.width()),
                        static_cast<float>(canvas.height()));
    }

    drawTitlePetals(canvas, time);
}

void App::drawTitlePetals(Canvas& canvas, double time) {
    if (titlePetalTypes_.empty()) return;

    // A 2% chance per frame with no cap, which settles at about twenty petals
    // on a 1280-wide screen. The population is meant to build up, so a screen
    // opened a second ago is legitimately emptier than one left running.
    if (titleRng_.chance(0.02)) {
        TitlePetal petal;
        petal.x = -50.0;
        petal.y = titleRng_.unit() * canvas.height();
        petal.speedX = 0.5 + titleRng_.unit() * 2.0;
        petal.size = (0.5 + titleRng_.unit() * 1.5) * kTitlePetalPixels;
        petal.rotation = titleRng_.unit() * 360.0;
        petal.rotationSpeed = (titleRng_.unit() - 0.5) * 4.0;
        petal.petal = titlePetalTypes_[titleRng_.below(
            static_cast<std::uint32_t>(titlePetalTypes_.size()))];
        titlePetals_.push_back(petal);
    }

    // Purely horizontal: a petal's y is set once and never touched again, so
    // nothing bobs and nothing wraps. The only way off screen is the right.
    const double exit = canvas.width() + 50.0;
    for (TitlePetal& petal : titlePetals_) {
        petal.x += petal.speedX;
        petal.rotation += petal.rotationSpeed;
    }
    titlePetals_.erase(std::remove_if(titlePetals_.begin(), titlePetals_.end(),
                                      [exit](const TitlePetal& p) { return p.x > exit; }),
                       titlePetals_.end());

    // Animated petal art is baked at ~24fps in the reference, so quantise the
    // clock the SVG animations read rather than letting them run smooth.
    const double artTime = std::floor(time * 1000.0 / 42.0) * 0.042;
    // Oldest first: a new petal lands on top of the ones already flying.
    for (const TitlePetal& petal : titlePetals_) {
        sprites_.drawPetal(canvas, petal.petal, petal.x + petal.size * 0.5,
                           petal.y + petal.size * 0.5, petal.size,
                           petal.rotation * kPi / 180.0, artTime);
    }
}

App::LobbyLayout App::lobbyLayout(int viewWidth, int viewHeight) const {
    // The browser title screen's rhythm, measured from the centre: the name
    // field and the Ready button side by side a hundred above it, the biome
    // label at fifty, the row of biome buttons at twenty.
    const double centreX = viewWidth * 0.5;
    const double centreY = viewHeight * 0.5;

    LobbyLayout layout;
    layout.name = {centreX - 200.0, centreY - 100.0, 280.0, 42.0};
    layout.ready = {centreX + 120.0, centreY - 100.0, 120.0, 42.0};

    constexpr double kBiomeWidth = 90.0;
    constexpr double kBiomeHeight = 35.0;
    constexpr double kBiomeGap = 10.0;
    const std::size_t count = spawnChoices_.size();
    if (count == 0) return layout;

    // Always one row, even when it does not fit. Wrapping would put the picker
    // on top of the loadout bar, and the reference simply lets a long row run
    // off both edges of a narrow window.
    const double rowWidth = count * (kBiomeWidth + kBiomeGap) - kBiomeGap;
    for (std::size_t i = 0; i < count; ++i) {
        layout.biomes.push_back({centreX - rowWidth * 0.5 + i * (kBiomeWidth + kBiomeGap),
                                 centreY - 20.0, kBiomeWidth, kBiomeHeight});
    }
    return layout;
}

Rect App::titleChatBox(int viewWidth, int viewHeight) {
    (void)viewWidth;
    // Bottom-left, clear of the icon column, where the reference's chat input
    // sits. Fixed rather than derived: it is a fixed-position element there,
    // and it is the same slot the game draws, so both read it from here.
    return {kChatX, viewHeight - kChatFieldUp, kChatColumnWidth, kChatFieldHeight};
}

void App::drawLobby(Canvas& canvas, double time) {
    const double centreX = canvas.width() * 0.5;
    const double centreY = canvas.height() * 0.5;
    const Vec2 mouse{window_.mouseX(), window_.mouseY()};
    // A panel under the pointer owns it: the reference stops updating the
    // title screen's hover as soon as one of its menus is open, so a button
    // does not light up through the card standing on it. A pointer that has
    // left the window owns nothing at all -- `mouseleave` drops the hovered
    // biome, the hovered Ready button and the latched press together.
    const bool freeMouse = pointerInWindow(window_) && !menus_.capturesMouse(mouse);

    TextStyle title;
    title.size = 48.0;
    title.align = Align::Centre;
    title.bold = true;
    title.strokeWidth = 6.0;
    text(canvas, "flowrix beta", centreX, centreY - 200.0, title);

    const LobbyLayout layout = lobbyLayout(canvas.width(), canvas.height());

    // Drawn by hand rather than through ui::textField: this one is a pale
    // plate with a grey edge and a BLACK caret, which is a different control
    // from the game's green fields and not a restyling of them.
    TextFieldStyle nameStyle;
    nameStyle.fill = kPaper;
    nameStyle.fillAlpha = nameFocused_ ? 0.95 : 0.9;
    nameStyle.outline = 0xB4B4B4u;
    nameStyle.focusedOutline = 0xB4B4B4u;
    nameStyle.outlineAlpha = 0.8;
    nameStyle.outlineWidth = 4.0;
    nameStyle.focusedOutlineWidth = 4.0;
    nameStyle.radius = 3.0;
    nameStyle.textSize = 18.0;
    nameStyle.textStrokeWidth = 3.0;
    nameStyle.bold = true;
    nameStyle.caret = kInk;
    textField(canvas, layout.name, ellipsised(canvas, playerName_, 260.0),
              "This flower is called...", nameFocused_, false, time, nameStyle);

    ButtonStyle readyStyle;
    readyStyle.fill = 0x1DD129u;
    readyStyle.textSize = 18.0;
    button(canvas, layout.ready, "Ready", freeMouse && hitInclusive(layout.ready, mouse),
           freeMouse && pressedControl_ == "start", readyStyle);

    TextStyle label;
    label.size = 18.0;
    label.align = Align::Centre;
    label.bold = true;
    label.strokeWidth = 4.0;
    text(canvas, "Spawn Biome:", centreX, centreY - 50.0, label);

    for (std::size_t i = 0; i < layout.biomes.size(); ++i) {
        const std::string& id = spawnChoices_[i];
        const BiomeDisplay display = titleBiomeDisplay(id);
        // The chosen one is drawn darker with a heavier outline, rather than
        // brighter: hover already means brighter, and two states that both
        // brighten are two states nobody can tell apart.
        const bool chosen = id == menus_.settings().spawnBiome ||
                            (id == "default" && menus_.settings().spawnBiome.empty());
        ButtonStyle style;
        style.fill = chosen ? hsvScale(display.color, 0.85) : display.color;
        style.outlineWidth = chosen ? 5.0 : 4.0;
        style.textSize = 14.0;
        button(canvas, layout.biomes[i], display.label ? display.label : capitaliseFirst(id),
               freeMouse && !chosen && hitInclusive(layout.biomes[i], mouse),
               freeMouse && pressedControl_ == "biome_" + std::to_string(i), style);
    }

    TextStyle hint;
    hint.size = 14.0;
    hint.align = Align::Centre;
    hint.baseline = Baseline::Top;
    hint.bold = true;
    hint.strokeWidth = 3.0;
    const char* lines[] = {
        "Controls:",
        "Arrow keys to move",
        "Hold space to extend petals",
        "Press Z to open the inventory.",
        "Press U + number keys 1-9 to use items.",
        "Press number keys 1-9 to swap items with secondary loadout",
        "Press K to switch between mouse and keyboard controls",
        "Use Q and E to swap petals",
        "Use T to unequip the selected petal",
    };
    // Below the loadout bar, which occupies centreY+50 to centreY+260. The
    // block starts inside its lower edge on purpose; that is where the
    // reference puts it.
    double y = centreY + 225.0;
    for (const char* line : lines) {
        text(canvas, line, centreX, y, hint);
        y += 20.0;
    }

    drawTitleChat(canvas, time);
    if (statsVisible()) drawStatsCounters(canvas, true);
}

void App::drawTitleChat(Canvas& canvas, double time) {
    // Only the input slot: the title screen has no transcript above it.
    drawChatField(canvas, titleChatBox(canvas.width(), canvas.height()), time);
}

void App::drawHud(Canvas& canvas, double time) {
    const SelfState& self = net_.view().self();
    // ALT swaps every abbreviated number on this surface for its exact value,
    // and reveals the other players on the minimap.
    const bool altHeld = window_.keyDown(Key::LeftAlt) || window_.keyDown(Key::RightAlt);

    // Invulnerability rides in the replicated state bits of the player's own
    // body rather than in SelfState, so the flag is read back off the entity.
    bool invulnerable = false;
    const auto selfEntity = net_.view().entities().find(self.netId);
    if (selfEntity != net_.view().entities().end()) {
        invulnerable = (selfEntity->second.state & net::StateInvulnerable) != 0;
    }
    if (wasInvulnerable_ && !invulnerable) invulEndedAt_ = time;
    wasInvulnerable_ = invulnerable;

    constexpr std::uint32_t kInvulnerableHealth = 0xFAFFC9u;
    std::uint32_t healthColour = kHealth;
    if (invulnerable) {
        healthColour = kInvulnerableHealth;
    } else if (invulEndedAt_ >= 0 && time - invulEndedAt_ < kInvulFadeSeconds) {
        // Linear, per channel. The reference eases this not at all, and a
        // curve here would be visible against an in-world bar that does not.
        const double t = clamp((time - invulEndedAt_) / kInvulFadeSeconds, 0.0, 1.0);
        const auto blend = [t](double from, double to) {
            return static_cast<std::uint32_t>(std::lround(from + (to - from) * t)) & 0xFFu;
        };
        healthColour = (blend(0xFA, 0x73) << 16) | (blend(0xFF, 0xFF) << 8) | blend(0xC9, 0x54);
    }

    TextStyle label;
    label.size = 14.0;
    label.strokeWidth = 3.0;
    // The reference never touches textBaseline on this surface, so every HUD
    // string sits on the alphabetic baseline and its y is the pen's own.
    label.baseline = Baseline::Alphabetic;

    const double health = std::max(0.0, self.health);
    hudBar(canvas, kHudBarX, kHudHealthY, kHudBarWidth, kHudBarHeight,
           self.maxHealth > 0 ? health / self.maxHealth * kHudBarWidth : 0.0, healthColour);
    text(canvas,
         formatNumber(std::round(health), altHeld) + "/" + formatNumber(self.maxHealth, altHeld),
         kHudTextX, kHudHealthY + 15.0, label);

    const LevelProgress progress = levelFromTotalXp(self.totalXp);
    hudBar(canvas, kHudBarX, kHudXpY, kHudBarWidth, kHudBarHeight,
           progress.xpForNext > 0 ? progress.xpIntoLevel / progress.xpForNext * kHudBarWidth : 0.0,
           kXpBar);
    text(canvas,
         "LVL " + std::to_string(progress.level) + " - " +
             formatNumber(progress.xpIntoLevel, altHeld) + "/" +
             formatNumber(progress.xpForNext, altHeld),
         kHudTextX, kHudXpY + 15.0, label);

    // The flower goes down LAST, so it covers the rounded left caps of both
    // bars. Drawing it first would leave two black stubs poking out of it.
    canvas.save();
    canvas.translate(static_cast<float>(kFlowerCentreX), static_cast<float>(kFlowerCentreY));
    canvas.save();
    setStroke(canvas, kInk);
    canvas.setLineWidth(4.0f);
    canvas.strokeCircle(0, 0, 27.0f);
    canvas.restore();
    drawFlowerFace(canvas, 0xFFE763u, 25.0, 2.0, 0.0, 14.5);
    canvas.restore();

    drawSquadHud(canvas);

    drawMinimap(canvas);

    drawBossBars(canvas, altHeld);

    // The loadout is NOT drawn here. The menu system's strip is the same set of
    // slots and is a live drop target; a second, inert copy of it a few pixels
    // away was two things that looked like one.
}

namespace {

/// The three flag families `/forcelocalplayerflags` can set, by the names the
/// reference's own enums give them -- src/player.ts is what an operator has
/// been typing at, so the same words have to work here.
struct NamedFlag {
    const char* name;
    std::uint32_t value;
};
constexpr NamedFlag kFaceFlagNames[] = {
    {"Poisoned", FacePoisoned},       {"Dandelioned", FaceDandelioned},
    {"DeadEyes", FaceDeadEyes},       {"SquareEyes", FaceSquareEyes},
    {"Attacking", FaceAttacking},     {"Defending", FaceDefending},
    {"HasCorruption", FaceHasCorruption},
};
constexpr NamedFlag kEquipFlagNames[] = {
    {"Cutter", EquipCutter},     {"ThirdEye", EquipThirdEye}, {"Observer", EquipObserver},
    {"Antennae", EquipAntennae}, {"Test1", EquipTest1},
};
constexpr NamedFlag kRenderFlagNames[] = {
    {"Pumpkin", PlayerRenderPumpkin},
    {"Robot", PlayerRenderRobot},
    {"Glitch", PlayerRenderGlitch},
};

template <std::size_t N>
std::string flagNameList(const NamedFlag (&flags)[N]) {
    std::string out;
    for (const NamedFlag& flag : flags) {
        if (!out.empty()) out += ", ";
        out += flag.name;
    }
    return out;
}

/// Folds a run of flag names into one mask. A bare NUMBER anywhere in the run
/// takes over outright, which is how the reference reads it: it is an escape
/// hatch for a bit the table does not name yet.
template <std::size_t N>
bool foldFlags(const NamedFlag (&table)[N], const std::vector<std::string>& names,
               std::uint32_t& out, std::string& unknownOut) {
    out = 0;
    for (const std::string& name : names) {
        const NamedFlag* found = nullptr;
        for (const NamedFlag& flag : table) {
            std::string candidate = flag.name;
            if (candidate.size() != name.size()) continue;
            bool same = true;
            for (std::size_t i = 0; i < name.size() && same; ++i) {
                same = std::tolower(static_cast<unsigned char>(candidate[i])) ==
                       std::tolower(static_cast<unsigned char>(name[i]));
            }
            if (same) found = &flag;
        }
        if (found != nullptr) {
            out |= found->value;
            continue;
        }
        bool numeric = !name.empty();
        for (const char c : name) {
            if (!std::isdigit(static_cast<unsigned char>(c))) numeric = false;
        }
        if (!numeric) {
            unknownOut = name;
            return false;
        }
        out = static_cast<std::uint32_t>(std::strtoul(name.c_str(), nullptr, 10));
        return true;
    }
    return true;
}

} // namespace

void App::onReconnected() {
    // Whatever this client was holding about the last session is about a
    // process that no longer exists. The death card especially: it is a card
    // about a body that is not coming back.
    deathCardVisible_ = false;
    loginMessage_.clear();
    screen_ = Screen::Login;

    // The account outlives the socket, so it is presented again rather than
    // asked for. A token the new server refuses lands on the login form, which
    // is the screen already showing.
    const std::string token =
        net_.sessionToken().empty() ? storedToken_ : net_.sessionToken();
    if (!token.empty()) {
        net_.resumeSession(token);
        net_.addSystemMessage("Reconnected to the server.");
    } else {
        net_.addSystemMessage("Reconnected to the server. Please log in again.");
    }
    storedToken_.clear();
}

bool App::handleClientCommand(const std::string& message) {
    if (message == "/guild-menu" || message == "/guild menu") {
        // The reference refuses this outside a running game because its panel
        // lives on the game object. This client's panel is a lobby menu too,
        // so it opens wherever the icon strip is up -- and says the
        // reference's line only where there is genuinely nothing to open.
        if (screen_ == Screen::Lobby || screen_ == Screen::Playing || screen_ == Screen::Dead) {
            menus_.toggle(MenuId::Guild);
        } else {
            net_.addSystemMessage("Guild menu is only available in-game.");
        }
        return true;
    }

    if (message.rfind("/forcelocalplayerflags", 0) != 0) return false;

    std::vector<std::string> words;
    std::string word;
    for (const char c : message.substr(std::string("/forcelocalplayerflags").size())) {
        if (c == ' ' || c == '\t') {
            if (!word.empty()) words.push_back(std::exchange(word, std::string()));
        } else {
            word.push_back(c);
        }
    }
    if (!word.empty()) words.push_back(word);

    WorldView::LocalFlagOverride& override = net_.view().localFlags;
    if (words.empty()) {
        const auto mine = net_.view().entities().find(net_.view().self().netId);
        const std::uint32_t face = mine == net_.view().entities().end() ? 0 : mine->second.faceFlags;
        const std::uint32_t equip =
            mine == net_.view().entities().end() ? 0 : mine->second.equipFlags;
        const std::uint32_t render =
            mine == net_.view().entities().end() ? 0 : mine->second.renderFlags;
        net_.addSystemMessage(
            "Usage: /forcelocalplayerflags &lt;face|equip|render&gt; &lt;flag1&gt; "
            "[flag2] ...<br/>Face flags: " + flagNameList(kFaceFlagNames) +
            "<br/>Equip flags: " + flagNameList(kEquipFlagNames) +
            "<br/>Render flags (skins): " + flagNameList(kRenderFlagNames) +
            "<br/>Current: faceFlags=" + std::to_string(face) + ", equipFlags=" +
            std::to_string(equip) + ", renderFlags=" + std::to_string(render));
        return true;
    }

    std::string family = words.front();
    for (char& c : family) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::vector<std::string> names(words.begin() + 1, words.end());

    std::uint32_t value = 0;
    std::string unknown;
    if (family == "face") {
        if (!foldFlags(kFaceFlagNames, names, value, unknown)) {
            net_.addSystemMessage("Unknown face flag: " + unknown);
            return true;
        }
        override.overridesFace = true;
        override.faceFlags = static_cast<std::uint8_t>(value);
        net_.addSystemMessage("Set local faceFlags to " + std::to_string(value));
    } else if (family == "equip") {
        if (!foldFlags(kEquipFlagNames, names, value, unknown)) {
            net_.addSystemMessage("Unknown equip flag: " + unknown);
            return true;
        }
        override.overridesEquip = true;
        override.equipFlags = static_cast<std::uint8_t>(value);
        net_.addSystemMessage("Set local equipFlags to " + std::to_string(value));
    } else if (family == "render") {
        if (!foldFlags(kRenderFlagNames, names, value, unknown)) {
            net_.addSystemMessage("Unknown render flag: " + unknown);
            return true;
        }
        override.overridesRender = true;
        override.renderFlags = value;
        net_.addSystemMessage("Set local renderFlags to " + std::to_string(value));
    } else {
        net_.addSystemMessage("Usage: /forcelocalplayerflags &lt;face|equip|render&gt; "
                              "&lt;flag1&gt; [flag2] ...");
    }
    return true;
}

void App::drawSquadHud(Canvas& canvas) {
    const SquadState& squad = net_.squad();
    if (!squad.inSquad) return;

    // Uniformly 80% of the main HUD and laid out the same way, which is what
    // the reference scales its own squad block to.
    constexpr double kScale = 0.8;
    constexpr double kMemberGap = 12.0;
    const double barWidth = kHudBarWidth * kScale;
    const double barHeight = kHudBarHeight * kScale;
    const double barX = kFlowerCentreX + 12.0 * kScale;
    const double textX = kFlowerCentreX + 35.0 * kScale;
    const double fontSize = 14.0 * kScale;
    const double flowerRadius = 25.0 * kScale;
    const double outlineRadius = 27.0 * kScale;

    TextStyle label;
    label.size = fontSize;
    label.strokeWidth = 3.0 * kScale;
    label.baseline = Baseline::Alphabetic;

    // Where the main HUD actually ends: the lower of its two bars, or the
    // bottom of the flower, whichever hangs further down.
    double top = std::max(kHudXpY + kHudBarHeight, kFlowerCentreY + 27.0) + 10.0;

    for (const SquadState::Member& member : squad.members) {
        if (member.netId == 0) continue;
        const auto found = net_.view().entities().find(member.netId);
        // A member whose body is not in this client's world has nothing to
        // draw a bar from. They are streamed however far away they are, so in
        // practice this is the moment between a death and a respawn.
        if (found == net_.view().entities().end()) continue;
        const RemoteEntity& body = found->second;
        if (body.isSelf()) continue;

        const double nameBaseline = top + fontSize;
        // The nameplate and the level, on one line: the reference puts the
        // level on an XP bar under the health bar, and this wire carries a
        // squadmate's level but not their XP, so the bar it would fill is not
        // drawn rather than drawn empty.
        text(canvas,
             (member.name.empty() ? std::string("Squadmate") : member.name) + " - LVL " +
                 std::to_string(body.level),
             barX, nameBaseline, label);

        const double healthY = nameBaseline + 4.0;
        const bool invulnerable = (body.state & net::StateInvulnerable) != 0;
        hudBar(canvas, barX, healthY, barWidth, barHeight,
               clamp(body.healthFraction, 0.0, 1.0) * barWidth,
               invulnerable ? 0xFAFFC9u : kHealth);

        canvas.save();
        canvas.translate(static_cast<float>(kFlowerCentreX),
                         static_cast<float>(healthY + barHeight * 0.5));
        canvas.save();
        setStroke(canvas, kInk);
        canvas.setLineWidth(static_cast<float>(4.0 * kScale));
        canvas.strokeCircle(0, 0, static_cast<float>(outlineRadius));
        canvas.restore();
        drawFlowerFace(canvas, 0xFFE763u, flowerRadius, 2.0 * kScale, 0.0, 14.5 * kScale);
        canvas.restore();

        top = std::max(healthY + barHeight, healthY + barHeight * 0.5 + outlineRadius) +
              kMemberGap;
    }
}

void App::drawBossBars(Canvas& canvas, bool altHeld) {
    // Super, unique and apex only. An ultra is a big mob, not a boss: it wears
    // the ordinary bar under its body, which is also where its name is.
    // A pet is somebody's summon and a target dummy is a permanent DPS-test
    // fixture, so neither earns the screen-top bar whatever tier it wears.
    std::vector<const RemoteEntity*> bosses;
    const Rect view = camera_.visibleWorld();
    for (const auto& entry : net_.view().entities()) {
        const RemoteEntity& entity = entry.second;
        if (entity.kind != net::EntityKind::Mob) continue;
        if (entity.rarity != Rarity::Super && entity.rarity != Rarity::Unique &&
            entity.rarity != Rarity::Apex) {
            continue;
        }
        if ((entity.spawnFlags & net::SpawnIsPet) != 0) continue;
        const MobConfig& config = content().mob(entity.typeIndex);
        if (config.id == "target_dummy") continue;

        // The same generous test the browser build makes: the drawn size plus
        // a buffer of at least 100 units, so a boss keeps its bar until it is
        // well clear of the edge rather than losing it mid-fight.
        const double visualScale = config.visualScale > 0 ? config.visualScale : 1.0;
        const double size = entity.radius * 2.0 * visualScale;
        const double margin = size * 0.5 + std::max(size, 100.0);
        if (entity.position.x + margin < view.left() || entity.position.x - margin > view.right() ||
            entity.position.y + margin < view.top() || entity.position.y - margin > view.bottom()) {
            continue;
        }
        bosses.push_back(&entity);
    }
    if (bosses.empty()) return;
    // The entity table is unordered, so two bosses on screen at once would
    // otherwise trade rows from frame to frame.
    std::sort(bosses.begin(), bosses.end(),
              [](const RemoteEntity* a, const RemoteEntity* b) { return a->netId < b->netId; });

    constexpr double kBarWidth = 400.0;
    constexpr double kBarHeight = 24.0;
    constexpr double kNameSize = 20.0;
    constexpr double kNameMargin = 8.0;
    constexpr double kRowSpacing = 60.0;
    constexpr double kTopMargin = 20.0;
    const double centreX = window_.width() * 0.5;

    for (std::size_t i = 0; i < bosses.size(); ++i) {
        const RemoteEntity& boss = *bosses[i];
        const double nameTop = kTopMargin + static_cast<double>(i) * kRowSpacing;
        const double barY = nameTop + kNameSize + kNameMargin;

        TextStyle name;
        name.size = kNameSize;
        name.strokeWidth = 4.0;
        name.align = Align::Centre;
        name.baseline = Baseline::Alphabetic;
        text(canvas, content().mob(boss.typeIndex).name, centreX, nameTop + kNameSize, name);

        // Max health is not on the wire -- only the fraction is -- so it is
        // recomputed from the same config the server sized the mob from. The
        // two cannot disagree: nothing scales a mob's pool after it spawns.
        const double maxHealth = content().mobStats(boss.typeIndex, boss.rarity).health;
        const double health = std::max(0.0, clamp(boss.healthFraction, 0.0, 1.0) * maxHealth);
        hudBar(canvas, centreX - kBarWidth * 0.5, barY, kBarWidth, kBarHeight,
               maxHealth > 0 ? health / maxHealth * kBarWidth : 0.0, kHealth);

        TextStyle value;
        value.size = 16.0;
        value.strokeWidth = 3.0;
        value.align = Align::Centre;
        value.baseline = Baseline::Alphabetic;
        text(canvas,
             formatNumber(std::round(health), altHeld) + "/" + formatNumber(maxHealth, altHeld),
             centreX, barY + 18.0, value);
    }
}

const Canvas* App::minimapStatic(int section, bool rarityGlow) {
    // ALT is part of the key, not just the draw: the reference's bake cache is
    // keyed `scrollX_scrollY_glow` (minimap.ts:216), so pressing ALT rebakes
    // the layer rather than tinting a stale one.
    // uiScale is the third key. See minimapDensity_: the bake is a bitmap and
    // has to be rasterised at the density it will be shown at.
    const double density = window_.uiScale();
    if (minimapStatic_ && minimapSection_ == section && minimapGlow_ == rarityGlow &&
        minimapDensity_ == density) {
        return minimapStatic_.get();
    }

    const int sectionX = section % kSectionsPerAxis;
    const int sectionY = section / kSectionsPerAxis;
    const double scrollX = sectionX * kSectionSize;
    const double scrollY = sectionY * kSectionSize;
    const double scale = kMinimapSize / kSectionSize;

    const int bakeSide =
        std::max(1, static_cast<int>(std::lround(kMinimapSize * density)));
    auto baked = std::make_unique<Canvas>(Canvas::createVirtual(bakeSide, bakeSide));
    Canvas& map = *baked;
    // Everything below is written in design units, exactly as it was when the
    // bake was always kMinimapSize pixels square. This one line is what buys
    // it the display's real resolution.
    map.scale(static_cast<float>(density), static_cast<float>(density));

    setFill(map, kPaper, 0.9);
    map.fillRect(0, 0, static_cast<float>(kMinimapSize), static_cast<float>(kMinimapSize));

    // Spawn bands, under the walls, only while ALT is held. Their own palette,
    // not kRarityColors: MINIMAP_SPAWN_COLORS (minimap.ts:11-22) gives unique a
    // violet and apex a cyan where the item tiers are white and magenta.
    if (rarityGlow) {
        for (const MapElement& element : mapData_.elements()) {
            // Every spawn zone, tier or not: the reference falls back to
            // `|| 'common'` rather than skipping an untagged one.
            if (element.kind != MapElementKind::Spawn) continue;
            const double left = (element.bounds.x - scrollX) * scale;
            const double top = (element.bounds.y - scrollY) * scale;
            const double w = element.bounds.w * scale;
            const double h = element.bounds.h * scale;
            if (left + w <= 0 || left >= kMinimapSize || top + h <= 0 || top >= kMinimapSize) {
                continue;
            }
            const Rarity tier = element.hasSpawnTier ? element.spawnTier : Rarity::Common;
            setFill(map, kMinimapSpawnColors[static_cast<std::size_t>(rarityIndex(tier))], 0.4);
            map.fillRect(static_cast<float>(left), static_cast<float>(top),
                         static_cast<float>(w), static_cast<float>(h));
        }
    }

    // One section's worth of tiles, at three pixels each. Baked rather than
    // rescanned: this is four and a half thousand cells and it only changes
    // when the player walks into another section.
    const Terrain& terrain = net_.terrain();
    const int minTileX = std::max(0, Terrain::toTileCoord(scrollX));
    const int maxTileX = std::min(kTilesPerAxis - 1, Terrain::toTileCoord(scrollX + kSectionSize));
    const int minTileY = std::max(0, Terrain::toTileCoord(scrollY));
    const int maxTileY = std::min(kTilesPerAxis - 1, Terrain::toTileCoord(scrollY + kSectionSize));
    const float tilePixels = static_cast<float>(kTileSize * scale);
    for (int ty = minTileY; ty <= maxTileY; ++ty) {
        for (int tx = minTileX; tx <= maxTileX; ++tx) {
            const Tile tile = terrain.atTile(tx, ty);
            if (tile == Tile::Ground) continue;
            // Solid ground reads as one black silhouette whatever kind of wall
            // it is; only the passable oddities keep a colour of their own.
            std::uint32_t colour = 0x000000u;
            if (!tileBlocks(tile)) colour = tile == Tile::Water ? 0x4169E1u : 0xFF5500u;
            setFill(map, colour);
            map.fillRect(static_cast<float>(tx * kTileSize * scale - scrollX * scale),
                         static_cast<float>(ty * kTileSize * scale - scrollY * scale),
                         tilePixels, tilePixels);
        }
    }

    for (const MapElement& element : mapData_.elements()) {
        if (element.kind != MapElementKind::Teleporter) continue;
        const Vec2 centre = element.centre();
        const float dotX = static_cast<float>((centre.x - scrollX) * scale);
        const float dotY = static_cast<float>((centre.y - scrollY) * scale);
        // Strictly inside, as the reference's test is: a zero-sized teleporter
        // sitting on the section's own edge is a dot the browser does not draw,
        // and a tolerance here would paint a clipped one it never shows.
        if (dotX <= 0 || dotX >= kMinimapSize || dotY <= 0 || dotY >= kMinimapSize) continue;
        // Green, never gold: gold marks a teleporter that hands the player to
        // another server, and this build has no such thing to mark.
        setFill(map, 0x00FF00u);
        map.fillCircle(dotX, dotY, 3.0f);
        setStroke(map, kInk);
        map.setLineWidth(1.0f);
        map.strokeCircle(dotX, dotY, 3.0f);
    }

    minimapStatic_ = std::move(baked);
    minimapSection_ = section;
    minimapGlow_ = rarityGlow;
    minimapDensity_ = density;
    return minimapStatic_.get();
}

void App::drawMinimap(Canvas& canvas) {
    const double x = canvas.width() - kMinimapSize - kMinimapPadding;
    const double y = kMinimapPadding;
    const double scale = kMinimapSize / kSectionSize;

    // The map always shows exactly the section the player stands in, snapped to
    // its corner. There is no zoom and no free scrolling: the reference's
    // scroll and zoom entry points are both no-ops.
    const Vec2 me = net_.view().selfDrawnPosition();
    const int sectionX = static_cast<int>(clamp(std::floor(me.x / kSectionSize), 0.0,
                                                kSectionsPerAxis - 1.0));
    const int sectionY = static_cast<int>(clamp(std::floor(me.y / kSectionSize), 0.0,
                                                kSectionsPerAxis - 1.0));
    const int section = sectionY * kSectionsPerAxis + sectionX;
    const double scrollX = sectionX * kSectionSize;
    const double scrollY = sectionY * kSectionSize;

    // ALT does two things here: it reveals the other players' dots, and it is
    // half the bake key -- the spawn bands under the tiles come and go with it.
    const bool altHeld = window_.keyDown(Key::LeftAlt) || window_.keyDown(Key::RightAlt);
    if (const Canvas* baked = minimapStatic(section, altHeld)) {
        // Sized explicitly rather than left to drawCanvas's two-argument form:
        // that one takes the source's PIXEL size as its user-space extent,
        // which would draw a bake rasterised for a Retina display at twice
        // the size the minimap is meant to be.
        canvas.drawCanvas(*baked, static_cast<float>(x), static_cast<float>(y),
                          static_cast<float>(kMinimapSize), static_cast<float>(kMinimapSize));
    }

    canvas.save();
    canvas.beginPath();
    canvas.rect(static_cast<float>(x), static_cast<float>(y), static_cast<float>(kMinimapSize),
                static_cast<float>(kMinimapSize));
    canvas.clip();
    const auto dot = [&](Vec2 world, double radius, std::uint32_t fill, bool outlined) {
        const double dx = x + (world.x - scrollX) * scale;
        const double dy = y + (world.y - scrollY) * scale;
        // Strictly inside, as the reference tests it: a dot exactly on the
        // border belongs to the neighbouring section's map, not this one.
        if (dx <= x || dx >= x + kMinimapSize || dy <= y || dy >= y + kMinimapSize) return;
        setFill(canvas, fill);
        canvas.fillCircle(static_cast<float>(dx), static_cast<float>(dy),
                          static_cast<float>(radius));
        if (!outlined) return;
        setStroke(canvas, kInk);
        canvas.setLineWidth(1.0f);
        canvas.strokeCircle(static_cast<float>(dx), static_cast<float>(dy),
                            static_cast<float>(radius));
    };
    // Squadmates are on the map whether or not ALT is held -- that is the whole
    // point of a party -- and in pink with an outline, so they stand out from
    // the crowd ALT brings up rather than joining it.
    const SquadState& squad = net_.squad();
    for (const auto& entry : net_.view().entities()) {
        const RemoteEntity& entity = entry.second;
        if (entity.kind != net::EntityKind::Player || entity.isSelf()) continue;
        const bool squadmate = squad.contains(entry.first);
        if (!squadmate && !altHeld) continue;
        if (squadmate) dot(entity.position, 4.0, 0xFF69B4u, true);
        else dot(entity.position, 4.0, kInk, false);
    }
    // Self last, so the blue dot is never hidden under someone standing on it.
    dot(me, 3.0, 0x0000FFu, true);

    // The camera's own rectangle, inside the clip, with hitboxes on. The
    // reference strokes it from cameraX/cameraY, which are the world-space TOP
    // LEFT of the view (core.ts:418-424), so it is visibleWorld() here and not
    // the camera centre.
    if (menus_.settings().render.hitboxes) {
        const Rect view = camera_.visibleWorld();
        setStroke(canvas, kInk);
        canvas.setLineWidth(2.0f);
        canvas.beginPath();
        canvas.rect(static_cast<float>(x + (view.x - scrollX) * scale),
                    static_cast<float>(y + (view.y - scrollY) * scale),
                    static_cast<float>(view.w * scale), static_cast<float>(view.h * scale));
        canvas.stroke();
    }
    canvas.restore();

    canvas.save();
    setStroke(canvas, 0xFFD700u);
    canvas.setLineWidth(2.0f);
    canvas.setLineJoin("miter");
    canvas.beginPath();
    canvas.rect(static_cast<float>(x), static_cast<float>(y), static_cast<float>(kMinimapSize),
                static_cast<float>(kMinimapSize));
    canvas.stroke();
    canvas.restore();

    TextStyle caption;
    caption.size = 14.0;
    caption.strokeWidth = 3.0;
    caption.align = Align::Centre;
    caption.baseline = Baseline::Alphabetic;
    text(canvas, biomeOf(section).name, x + kMinimapSize * 0.5, y + kMinimapSize + 18.0, caption);
}

void App::drawChat(Canvas& canvas, double time) {
    const double bottom = canvas.height();
    const Rect column{kChatX, bottom - kChatColumnUp, kChatColumnWidth,
                      kChatColumnUp - kChatColumnDown};

    // A leading slash swaps the transcript for the command list -- the
    // reference hides one element and shows the other in the same slot.
    const bool suggesting = chatOpen_ && !chatDraft_.empty() && chatDraft_[0] == '/';
    if (suggesting) {
        const std::vector<const ChatCommand*> matches = matchChatCommands(chatDraft_, net_.isSkinAdmin());
        if (!matches.empty()) {
            // Clamped locally: the key handler owns the selection, and a draw
            // pass that edited it would fight whatever the last keystroke did.
            const int selected =
                clamp(chatSuggestion_, 0, static_cast<int>(matches.size()) - 1);

            // Enough scroll to keep the highlighted row in view, which is what
            // the reference's scrollIntoView({block:'nearest'}) amounts to.
            const int visible = std::max(1, static_cast<int>(column.h / kChatSuggestionRowHeight));
            const int first = selected >= visible ? selected - visible + 1 : 0;

            canvas.save();
            canvas.beginPath();
            canvas.rect(static_cast<float>(column.x), static_cast<float>(column.y),
                        static_cast<float>(column.w), static_cast<float>(column.h));
            canvas.clip();
            double y = column.y;
            for (std::size_t i = static_cast<std::size_t>(first); i < matches.size(); ++i) {
                if (y >= column.bottom()) break;
                if (static_cast<int>(i) == selected) {
                    setFill(canvas, kPaper, 0.15);
                    canvas.fillRect(static_cast<float>(column.x), static_cast<float>(y),
                                    static_cast<float>(column.w),
                                    static_cast<float>(kChatSuggestionRowHeight));
                }
                const double baseline = y + 4.0 + ascent(kChatSuggestionSize, true);
                const double textX = column.x + 8.0;
                chatRun(canvas, matches[i]->command, textX, baseline, kChatSuggestionSize,
                        0xAADDFFu, 1.0);
                const double afterCommand =
                    textX + measure(matches[i]->command, kChatSuggestionSize, true) + 6.0;
                // The description is ellipsised rather than wrapped: its span
                // is `overflow: hidden; text-overflow: ellipsis; white-space:
                // nowrap`.
                std::string description = std::string("- ") + matches[i]->description;
                const double room = column.right() - 8.0 - afterCommand;
                if (measure(description, 12.0, true) > room) {
                    while (!description.empty() &&
                           measure(description + "...", 12.0, true) > room) {
                        popCodepoint(description);
                    }
                    description += "...";
                }
                chatRun(canvas, description, afterCommand, baseline, 12.0, kPaper, 0.5);
                y += kChatSuggestionRowHeight;
            }
            canvas.restore();
        }
    } else {
        // The transcript never expires. It flows from the TOP of its column
        // and is scrolled to the bottom only once it overflows, which is what a
        // bottom-anchored overflow:auto block does.
        const double halfLead =
            (kChatLineHeight - (ascent(14.0, true) - descent(14.0, true))) * 0.5;
        const double baselineOffset = halfLead + ascent(14.0, true);

        // Newest first, and only as far back as the column can show. Wrapping
        // a hundred-line transcript every frame to then clip all but six rows
        // of it is work nobody sees.
        std::vector<std::vector<ChatRow>> newestFirst;
        double content = kChatMessageGap;   // the last message's bottom margin
        for (auto it = net_.chat().rbegin(); it != net_.chat().rend(); ++it) {
            std::vector<ChatToken> tokens;
            tokens.push_back({"[" + clockTime(it->wallClockMillis) + "]", 12.0, kPaper, 0.6});
            if (!it->author.empty()) {
                // Every sender is the same green, whatever channel carried the
                // line: the reference has no per-channel colouring at all.
                tokens.push_back({it->author + ":", 14.0, 0x00FF00u, 1.0});
            }
            // The wire carries markup, not plain text: every boss announcement
            // is a <b style="color: ..."> and every multi-line command answer
            // is joined with <br/>. Splitting the raw string on spaces printed
            // the tags as words; parseMarkup turns them back into styling.
            bool afterWhitespace = true;
            for (const ui::MarkupSpan& span : ui::parseMarkup(it->text)) {
                if (span.lineBreak) {
                    tokens.push_back({{}, 14.0, kPaper, 1.0, false, false, false, true, false});
                    afterWhitespace = true;
                    continue;
                }
                const std::uint32_t fill = span.hasColor ? span.color : kPaper;
                std::size_t at = 0;
                while (at < span.text.size()) {
                    const std::size_t space = span.text.find_first_of(" \t\r\n", at);
                    const std::string word = span.text.substr(
                        at, space == std::string::npos ? std::string::npos : space - at);
                    if (!word.empty()) {
                        tokens.push_back({word, 14.0, fill, 1.0, span.italic, span.underline,
                                          span.blink, false, !afterWhitespace});
                        afterWhitespace = false;
                    }
                    if (space == std::string::npos) break;
                    afterWhitespace = true;
                    at = space + 1;
                }
                // A span ending mid-word ("<b>Hel</b>lo") must not gain a space
                // at the style change; one ending on a space must keep it.
                if (!span.text.empty()) {
                    const char last = span.text.back();
                    afterWhitespace = last == ' ' || last == '\t' || last == '\r' || last == '\n';
                }
            }
            newestFirst.push_back(layoutChatMessage(tokens, column.w));
            content += newestFirst.back().size() * kChatLineHeight + kChatMessageGap;
            if (content >= column.h) break;
        }

        if (!newestFirst.empty()) {
            // A short transcript sits at the TOP of its column; only once it
            // overflows does the box scroll, and then it is pinned to the
            // bottom. That is what an overflow:auto block scrolled to its end
            // does, and it is why a first message appears near the top of the
            // screen rather than just above the input.
            const double newestHeight = newestFirst.front().size() * kChatLineHeight;
            double top = (content >= column.h ? column.bottom() : column.y + content) -
                         kChatMessageGap - newestHeight;

            canvas.save();
            canvas.beginPath();
            canvas.rect(static_cast<float>(column.x), static_cast<float>(column.y),
                        static_cast<float>(column.w), static_cast<float>(column.h));
            canvas.clip();
            for (std::size_t m = 0; m < newestFirst.size(); ++m) {
                const std::vector<ChatRow>& rows = newestFirst[m];
                for (std::size_t row = 0; row < rows.size(); ++row) {
                    const double rowTop = top + row * kChatLineHeight;
                    if (rowTop + kChatLineHeight < column.y || rowTop > column.bottom()) continue;
                    for (const ChatPlacedRun& run : rows[row]) {
                        // `blink 1s step-start infinite`, which is what the
                        // reference's <blink> resolves to: shown for the first
                        // half of every second and hidden for the second.
                        if (run.blink && std::fmod(time, 1.0) >= 0.5) continue;
                        chatRun(canvas, run.text, column.x + run.x, rowTop + baselineOffset,
                                run.size, run.fill, run.alpha, run.italic, run.underline);
                    }
                }
                if (m + 1 >= newestFirst.size()) break;
                // The message above this one ends one collapsed margin higher.
                top -= kChatMessageGap + newestFirst[m + 1].size() * kChatLineHeight;
                if (top + newestFirst[m + 1].size() * kChatLineHeight < column.y) break;
            }
            canvas.restore();
        }
    }

    drawChatField(canvas, {kChatX, bottom - kChatFieldUp, kChatColumnWidth, kChatFieldHeight},
                  time);
}

void App::drawChatField(Canvas& canvas, Rect box, double time) {
    // A dark translucent slot with a hairline white edge -- the reference's
    // chat input, which is the one control in the game that is not drawn in the
    // chunky plate style everything else uses.
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(box.x), static_cast<float>(box.y),
                     static_cast<float>(box.w), static_cast<float>(box.h), 3.0f);
    setFill(canvas, kInk, chatOpen_ ? 0.5 : 0.3);
    canvas.fill();
    // Inset by half the line so the edge lands INSIDE the box, as a one-pixel
    // border does; a centred stroke would make the slot a pixel wider.
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(box.x + 0.5), static_cast<float>(box.y + 0.5),
                     static_cast<float>(box.w - 1.0), static_cast<float>(box.h - 1.0), 2.5f);
    canvas.save();
    canvas.setLineWidth(1.0f);
    setStroke(canvas, kPaper, 0.3);
    canvas.stroke();
    canvas.restore();

    TextStyle line;
    line.size = kChatFieldTextSize;
    line.align = Align::Left;
    line.strokeWidth = 0;
    // The placeholder is a property of the field being EMPTY, not of it being
    // unfocused: an <input> keeps showing it with the caret sitting in front.
    const bool empty = chatDraft_.empty();
    line.fill = empty ? 0x757575u : kPaper;
    text(canvas, empty ? "Press Enter to chat..." : chatDraft_, box.x + 6.0,
         box.y + box.h * 0.5, line);

    if (!chatOpen_) return;
    if (std::fmod(time, 1.0) >= 0.5) return;
    const double caretX = box.x + 6.0 + textWidth(canvas, chatDraft_, line.size);
    setFill(canvas, kPaper);
    canvas.fillRect(static_cast<float>(caretX), static_cast<float>(box.y + 4.0), 1.0f,
                    static_cast<float>(box.h - 8.0));
}

void App::drawDeathCard(Canvas& canvas, double time) {
    (void)time;   // static: the card's only variable is which button is hovered
    scrim(canvas, 0.65);
    const double centreX = canvas.width() * 0.5;
    const double centreY = canvas.height() * 0.5;
    const Vec2 mouse{window_.mouseX(), window_.mouseY()};

    TextStyle heading;
    heading.size = 48;
    heading.align = Align::Centre;
    heading.bold = true;
    heading.fill = kDanger;
    heading.strokeWidth = 5;
    text(canvas, "You Died!", centreX, centreY - 60, heading);

    TextStyle by;
    by.size = 22;
    by.align = Align::Centre;
    by.strokeWidth = 3;
    const std::string killer = net_.killerName().empty()
        ? "A mysterious entity"
        : net_.killerName();
    text(canvas, "You were destroyed by: " + killer, centreX, centreY - 10, by);

    // Drawn by hand rather than through ui::button: these two have exactly two
    // states, an explicit hover colour that is not a brightness step off the
    // base, and a CENTRED outline that makes the silhouette three pixels wider
    // than the box. ui::button gives none of the three.
    const auto card = [&](Rect box, const std::string& label, double textSize,
                          std::uint32_t fill, std::uint32_t hoverFill, std::uint32_t outline) {
        canvas.beginPath();
        canvas.roundRect(static_cast<float>(box.x), static_cast<float>(box.y),
                         static_cast<float>(box.w), static_cast<float>(box.h), 10.0f);
        setFill(canvas, hit(box, mouse) ? hoverFill : fill);
        canvas.fill();
        canvas.save();
        canvas.setLineWidth(3.0f);
        canvas.setLineJoin("miter");
        setStroke(canvas, outline);
        canvas.stroke();
        canvas.restore();

        TextStyle caption;
        caption.size = textSize;
        caption.align = Align::Centre;
        caption.bold = true;
        caption.strokeWidth = 3;
        text(canvas, label, box.x + box.w * 0.5, box.y + box.h * 0.5, caption);
    };

    const Rect continueBox{centreX - 100, centreY + 30, 200, 50};
    card(continueBox, "Continue", 22, 0x4A8E3Au, 0x5A9E4Au, 0x2D5A22u);
    const Rect closeBox{centreX - 70, centreY + 95, 140, 36};
    card(closeBox, "Close", 16, 0x666666u, 0x777777u, 0x444444u);

    // Translucent white rather than a flat grey, so the hint picks up the
    // colour of the scrimmed world behind it.
    TextStyle hint;
    hint.size = 14;
    hint.align = Align::Centre;
    hint.fill = kPaper;
    hint.strokeWidth = 0;
    canvas.setGlobalAlpha(0.6f);
    text(canvas, "Press ENTER to continue", centreX, closeBox.bottom() + 25, hint);
    canvas.setGlobalAlpha(1.0f);
}

void App::drawDisconnectBanner(Canvas& canvas) {
    // A strip across the very top, with the world and the HUD still drawing
    // underneath it. A dropped socket does not take the game off the screen.
    setFill(canvas, 0xC81E1Eu, 0.85);
    canvas.fillRect(0, 0, static_cast<float>(canvas.width()), 38.0f);

    TextStyle style;
    style.size = 16;
    style.align = Align::Centre;
    style.strokeWidth = 0;
    text(canvas, "Disconnected from server. Reconnecting...", canvas.width() * 0.5, 19.0, style);
}

// ---------------------------------------------------------------------------
// Scene wipe
// ---------------------------------------------------------------------------

void App::beginSceneWipe(bool toGame) {
    // Snapshot BEFORE the scene commits. Nothing has painted yet this frame, so
    // the window still holds the outgoing scene whole -- which is exactly the
    // still the wipe has to hold up while the incoming one builds itself.
    Canvas& canvas = window_.canvas();
    // PIXELS, not design units: this is a copy of the backing store, and
    // asking for a design-sized rectangle of it would photograph the top-left
    // corner of the screen and stretch that over the whole wipe. It is drawn
    // back at design size in drawSceneWipe, which is where the two spaces
    // meet. Copy canvas-to-canvas rather than reading its pixels through C++:
    // under Emscripten that stays entirely inside the browser's Canvas2D
    // backend instead of moving a full frame into and back out of Wasm.
    const int width = canvas.pixelWidth();
    const int height = canvas.pixelHeight();
    std::unique_ptr<Canvas> snapshot;
    if (width > 0 && height > 0) {
        snapshot = std::make_unique<Canvas>(Canvas::createVirtual(width, height));
        snapshot->drawCanvas(canvas, 0, 0, static_cast<float>(width),
                             static_cast<float>(height));
    }

    wipe_.snapshot = std::move(snapshot);
    wipe_.holeGrows = toGame;
    wipe_.phase = SceneWipe::Phase::Covered;
    wipe_.phaseStartSeconds = timeSeconds_;
}

bool App::wipeReadyToReveal() const {
    // Going back to the title there is nothing to wait for. Going into a game
    // the hold lasts until the player's own body has actually arrived, so the
    // reveal never opens on an empty world.
    if (!wipe_.holeGrows) return true;
    return (screen_ == Screen::Playing || screen_ == Screen::Dead) &&
           net_.view().self().netId != 0;
}

void App::drawSceneWipe(Canvas& canvas) {
    if (wipe_.phase == SceneWipe::Phase::Idle) return;
    // A scripted capture wants the finished scene, not the eight hundred
    // milliseconds of transition on the way into it. Skipped rather than
    // shortened, so a run that lands mid-wipe still photographs the screen the
    // flags asked for.
    if (config_.screenshotAfterFrames > 0) {
        wipe_.phase = SceneWipe::Phase::Idle;
        wipe_.snapshot.reset();
        return;
    }

    /// Long enough to read as a deliberate transition, short enough not to be
    /// in the way. The reference's IRIS_DURATION_MS.
    constexpr double kWipeSeconds = 0.8;
    /// A stall must not leave the screen covered forever, so the hold gives up.
    constexpr double kCoveredTimeoutSeconds = 8.0;

    const double elapsed = timeSeconds_ - wipe_.phaseStartSeconds;
    double progress = 0;
    if (wipe_.phase == SceneWipe::Phase::Covered) {
        if (wipeReadyToReveal() || elapsed > kCoveredTimeoutSeconds) {
            wipe_.phase = SceneWipe::Phase::Wiping;
            wipe_.phaseStartSeconds = timeSeconds_;
        }
    } else {
        progress = std::min(elapsed / kWipeSeconds, 1.0);
    }

    const double centreX = canvas.width() * 0.5;
    const double centreY = canvas.height() * 0.5;
    // The circle circumscribes the viewport, so a fully open hole clears the
    // corners rather than leaving four dark wedges.
    const double maxRadius = std::sqrt(centreX * centreX + centreY * centreY);
    const double inverse = 1.0 - progress;
    const double eased =
        wipe_.holeGrows ? 1.0 - inverse * inverse * inverse : inverse * inverse * inverse;
    const double radius = std::max(0.0, eased * maxRadius);

    canvas.save();
    canvas.beginPath();
    if (wipe_.holeGrows) {
        // Everything OUTSIDE the growing hole keeps the outgoing still. The
        // counter-clockwise arc punches the hole out of the rectangle by
        // winding against it.
        //
        // The angles run kTau -> 0, not 0 -> kTau. A reversed arc's sweep is
        // normalised with fmod unless it is already a whole turn NEGATIVE, so
        // asking for (0, +kTau) counter-clockwise degenerates to a zero-length
        // arc and the hole never appears.
        canvas.rect(0, 0, static_cast<float>(canvas.width()), static_cast<float>(canvas.height()));
        canvas.arc(static_cast<float>(centreX), static_cast<float>(centreY),
                   static_cast<float>(radius), static_cast<float>(kTau), 0.0f, true);
    } else {
        canvas.arc(static_cast<float>(centreX), static_cast<float>(centreY),
                   static_cast<float>(radius), 0, static_cast<float>(kTau));
    }
    canvas.clip();
    if (wipe_.snapshot) {
        canvas.drawCanvas(*wipe_.snapshot, 0, 0, static_cast<float>(canvas.width()),
                          static_cast<float>(canvas.height()));
    } else {
        // No still to hold up, so the wipe falls through black instead.
        setFill(canvas, kInk);
        canvas.fillRect(0, 0, static_cast<float>(canvas.width()),
                        static_cast<float>(canvas.height()));
    }
    canvas.restore();

    if (radius > 0) {
        canvas.save();
        setStroke(canvas, kInk);
        canvas.setLineWidth(6.0f);
        canvas.strokeCircle(static_cast<float>(centreX), static_cast<float>(centreY),
                            static_cast<float>(radius));
        canvas.restore();
    }

    if (wipe_.phase == SceneWipe::Phase::Wiping && progress >= 1.0) {
        wipe_.phase = SceneWipe::Phase::Idle;
        wipe_.snapshot.reset();
    }
}

void App::leaveToTitle() {
    net_.leaveGame();
    menus_.close();
    // Not finished, just gone: the browser destroys the Tutorial with the Game
    // that owns it, and an unfinished one comes back on the next join.
    tutorial_.endGame();
    beginSceneWipe(false);
    screen_ = Screen::Lobby;
}

void App::logout() {
    // Not leaveToTitle(): that lands on the lobby, which is the one screen a
    // logged-out client must not be on. The body itself is NetClient's to take
    // off the server -- it leaves the game before it sends the logout, because
    // the account a flower would be saved into goes away with the session.
    const bool inWorld = screen_ == Screen::Playing || screen_ == Screen::Dead;
    if (inWorld) tutorial_.endGame();
    net_.logout();
    menus_.close();

    // Forget the token on disk as well as in memory: a logout a restart undoes
    // is not a logout. The file is emptied rather than left alone because
    // saveSession() declines to write at all once there is nothing to save,
    // which would leave the old token sitting there. The name is not part of
    // the account, so it is written straight back if there is one.
    //
    // Emptied, not removed: an empty session file reads back as no session at
    // all, and on the browser build the file is the mount point for the
    // storage behind it -- deleting it would take the persistence with it.
    // See client/web/persist.cpp.
    storedToken_.clear();
    // A temporary, so the stream is closed again before saveSession() opens
    // the same path.
    std::ofstream(config_.sessionFile, std::ios::trunc);
    saveSession();

    // The form opens the way it does on a first run: blank, unfocused, and
    // with no message left over from the session that just ended.
    usernameField_.clear();
    passwordField_.clear();
    confirmPasswordField_.clear();
    loginMessage_.clear();
    focusedField_ = -1;
    registering_ = false;
    advancedOpen_ = false;
    pressedControl_.clear();
    pendingAuth_.clear();
    // Done, not Idle: a scripted run must not answer a deliberate logout by
    // registering itself straight back in.
    autoLogin_ = AutoLogin::Done;

    deathCardVisible_ = false;
    chatOpen_ = false;
    chatDraft_.clear();
    chatSuggestion_ = -1;
    nameFocused_ = false;

    // Only worth a wipe when there is a world to hide: lobby and login are the
    // same scene with a different card on it, and wiping between them would
    // read as a stutter rather than a transition.
    if (inWorld) beginSceneWipe(false);
    screen_ = Screen::Login;
}

// ---------------------------------------------------------------------------
// Session persistence
// ---------------------------------------------------------------------------

void App::loadSession() {
    std::ifstream in(config_.sessionFile);
    if (!in) return;
    // Two lines: the session token, then the flower's name. Only a token is
    // ever stored -- never the password -- so a stolen or shared machine leaks
    // at most one revocable, expiring handle. A file written by an older build
    // has only the first line, which reads back as an empty name.
    std::getline(in, storedToken_);
    std::getline(in, playerName_);
}

void App::saveSession() const {
    if (net_.sessionToken().empty() && playerName_.empty()) return;
    std::ofstream out(config_.sessionFile, std::ios::trunc);
    if (out) out << net_.sessionToken() << "\n" << playerName_ << "\n";
}

const SvgDocument* App::streakStar(std::uint32_t fill) {
    auto cached = streakStars_.find(fill);
    if (cached != streakStars_.end()) return cached->second->empty() ? nullptr : cached->second.get();

    // The game-icons.net 'stars' glyph, the same one the menu strip uses, but
    // stroked and tinted: the card's star has a thick black outline and a fill
    // that says whether today has been claimed, and renderFitted cannot
    // recolour a document after the fact.
    //
    // TWO paths, stroke-only then fill-only, because SVG paints fill under
    // stroke and the reference paints stroke under fill. One path would show
    // the whole 36-unit stroke instead of the outer half of it, and the star
    // would read as an outline with a small yellow centre.
    static const char* kStarPath =
        "M256 38.013c-22.458 0-66.472 110.3-84.64 123.502-18.17 13.2-136.674 20.975-143.614 "
        "42.334-6.94 21.358 84.362 97.303 91.302 118.662 6.94 21.36-22.286 136.465-4.116 149.665 "
        "18.17 13.2 118.61-50.164 141.068-50.164 22.458 0 122.9 63.365 141.068 50.164 18.17-13.2"
        "-11.056-128.306-4.116-149.665 6.94-21.36 98.242-97.304 91.302-118.663-6.94-21.36-125.444"
        "-29.134-143.613-42.335-18.168-13.2-62.182-123.502-84.64-123.502z";
    char colour[8];
    std::snprintf(colour, sizeof colour, "#%06x", fill & 0xFFFFFFu);
    const std::string svg =
        std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 512 512\">"
                    "<path fill=\"none\" stroke=\"#000000\" stroke-width=\"36\" "
                    "stroke-linejoin=\"round\" d=\"") + kStarPath +
        "\"/><path stroke=\"none\" fill=\"" + colour + "\" d=\"" + kStarPath + "\"/></svg>";
    auto document = std::make_shared<SvgDocument>(SvgDocument::fromString(svg));
    cached = streakStars_.emplace(fill, std::move(document)).first;
    return cached->second->empty() ? nullptr : cached->second.get();
}

void App::drawDailyStreak(Canvas& canvas, double time) {
    const DailyStreak& streak = net_.dailyStreak();
    if (!streak.known) return;

    constexpr double kWidth = 220.0;
    constexpr double kHeight = 150.0;
    constexpr double kBorder = 4.0;
    const Rect card{canvas.width() - kWidth - 16.0, 16.0, kWidth, kHeight};

    // Border and body as two filled rounded rects rather than a stroke: that is
    // how the reference draws it, and a centred stroke would round differently.
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(card.x), static_cast<float>(card.y),
                     static_cast<float>(card.w), static_cast<float>(card.h), 3.0f);
    setFill(canvas, shade(kStreakPanel, 0.7));
    canvas.fill();
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(card.x + kBorder), static_cast<float>(card.y + kBorder),
                     static_cast<float>(card.w - kBorder * 2),
                     static_cast<float>(card.h - kBorder * 2), 1.0f);
    setFill(canvas, kStreakPanel);
    canvas.fill();

    // The streak runs 1..5 and then starts the cycle again; the star shows
    // where in that cycle today's reward sits, not the streak's raw length.
    const int cycleDay = streak.streak > 0 ? ((streak.streak - 1) % 5) + 1 : 0;
    const double starX = card.x + card.w * 0.5;
    const double starY = card.y + 40.0;
    const std::uint32_t fill =
        cycleDay > 0 ? (streak.newDay ? 0xFFF28Au : 0xFFE65Du) : 0x8A4858u;

    // A short wobble to celebrate a fresh claim, easing out as it runs down.
    // Keyed on when the card first had something to say rather than on
    // `newDay`, which stays true for the whole session and would otherwise
    // wobble forever.
    if (streakSeenAt_ < 0) streakSeenAt_ = time;
    const double pulseLeft = streak.newDay ? kStreakPulseSeconds - (time - streakSeenAt_) : 0.0;
    const double radius =
        pulseLeft > 0
            ? 22.0 * (1.0 + std::sin(time * 1000.0 / 140.0) * 0.08 *
                                (pulseLeft / kStreakPulseSeconds))
            : 22.0;
    if (const SvgDocument* star = streakStar(fill)) {
        star->renderFitted(canvas, static_cast<float>(starX - radius),
                           static_cast<float>(starY - radius),
                           static_cast<float>(radius * 2.0), static_cast<float>(radius * 2.0),
                           0.0f);
    }
    if (cycleDay > 0) {
        TextStyle number;
        number.size = 16.0;
        number.bold = true;
        number.align = Align::Centre;
        number.strokeWidth = 3.0;
        text(canvas, std::to_string(cycleDay), starX, starY + 1.0, number);
    }

    const std::int64_t now = wallClockMillis();
    const bool claimed = now < streak.nextClaimAtMillis;

    TextStyle status;
    status.size = 13.0;
    status.bold = true;
    status.align = Align::Centre;
    status.baseline = Baseline::Top;
    status.strokeWidth = 3.0;
    status.fill = claimed ? kPaper : kWarning;
    text(canvas,
         claimed ? ("Claimed · Day " + std::to_string(streak.streak)) : "Ready to claim!",
         card.x + card.w * 0.5, card.y + 74.0, status);

    TextStyle countdown;
    countdown.size = 11.0;
    countdown.align = Align::Left;
    countdown.baseline = Baseline::Top;
    countdown.strokeWidth = 2.5;
    text(canvas,
         claimed ? ("Next: " + formatDuration(streak.nextClaimAtMillis - now)) : "Next: now",
         card.x + 12.0, card.y + 100.0, countdown);
    text(canvas, "Resets: " + formatDuration(streak.streakExpiresAtMillis - now),
         card.x + 12.0, card.y + 120.0, countdown);
}

bool App::statsVisible() const {
    return config_.showStats || menus_.settings().showStats;
}

void App::drawStatsCounters(Canvas& canvas, bool titleScreen) {
    TextStyle style;
    style.size = 11.0;
    style.bold = true;
    style.align = Align::Right;
    style.baseline = Baseline::Bottom;
    style.strokeWidth = 2.0;

    struct Line { std::string text; std::uint32_t fill; };
    std::vector<Line> lines;

    if (titleScreen) {
        // The title screen's overlay is a set of PLACEHOLDERS, not a readout:
        // renderStatsCounters (src/title_screen/index.ts:1228-1232) spells four
        // of the five lines out as literals and fills in nothing but the frame
        // count. There is no world behind this screen to report on, and
        // substituting live-looking zeroes would claim there is.
        lines = {
            {"Pos: --, --", 0xFFD700u},
            {"Ping: -- | In: 0 B/s | Out: 0 B/s", 0xA78BFAu},
            {"Players: 0", 0x4ECDC4u},
            {"Mobs: 0", 0xFF6B6Bu},
            {"FPS: " + std::to_string(framesPerSecond_) + " | Memory: 0.00 MB", 0x00FF00u},
        };
    } else {
        int players = 0;
        int mobs = 0;
        for (const auto& entry : net_.view().entities()) {
            if (entry.second.kind == net::EntityKind::Player) ++players;
            else if (entry.second.kind == net::EntityKind::Mob) ++mobs;
        }

        // Bottom-up, in the reference's order, so the frame counter is near the
        // corner and the render breakdown is the top line.

        // Position is omitted, not zeroed, until the world has placed this
        // flower -- the reference pushes the line only when it can resolve its
        // own socket's entity.
        if (net_.view().self().netId != 0) {
            const Vec2 me = net_.view().selfDrawnPosition();
            lines.push_back({"Pos: " + std::to_string(static_cast<long>(std::lround(me.x))) +
                                 ", " + std::to_string(static_cast<long>(std::lround(me.y))),
                             0xFFD700u});
        }

        // Before the first Pong there is no round trip to report, and the
        // reference prints "--" rather than a confident 0ms.
        const double ping = net_.averagePingMillis();
        const std::string pingText =
            ping > 0 ? std::to_string(static_cast<int>(std::lround(ping))) + "ms" : "--";
        lines.push_back({"Ping: " + pingText + " (" + net_.connectionQuality() + ")" +
                             " | In: " + formatBytes(incomingBytesPerSecond_) + "/s" +
                             " | Out: " + formatBytes(outgoingBytesPerSecond_) + "/s",
                         0xA78BFAu});

        // The heaviest opcodes of the last second, incoming first by size.
        // The arrow is the direction, as in the reference.
        if (!topWireEvents_.empty()) {
            std::string top = "Top: ";
            for (std::size_t i = 0; i < topWireEvents_.size(); ++i) {
                const NetClient::WireEvent& event = topWireEvents_[i];
                if (i > 0) top += " | ";
                // The reference draws these as U+2190/U+2192. The two Ubuntu
                // faces this client pins have no glyph for either -- the
                // browser only gets them from a system fallback -- so they
                // would come out as .notdef boxes. These say the same thing in
                // characters the face actually has.
                top += event.incoming ? "< " : "> ";
                top += event.name;
                top += " " + formatBytes(event.bytes) + "/s";
            }
            lines.push_back({top, 0xA78BFAu});
        }

        lines.push_back({"Players: " + std::to_string(players), 0x4ECDC4u});
        lines.push_back({"Mobs: " + std::to_string(mobs), 0xFF6B6Bu});

        // The work cost of a frame, which is the number that says whether the
        // frame rate is a budget problem or just the 60Hz cap.
        const std::string frameText =
            frameTimeAvgMs_ > 0 ? twoDecimals(frameTimeAvgMs_) + "ms" : "--";
        // Memory is the browser's offscreen-canvas tally, and that renderer
        // keeps none -- getOffscreenCanvasMemoryMB returns a hard 0, so the
        // reference prints this exact literal too. It is not a stub here.
        lines.push_back({"FPS: " + std::to_string(framesPerSecond_) + " (" + frameText +
                             "/frame) | Memory: 0.00 MB",
                         0x00FF00u});

        const auto section = [](const SectionStats& stats) {
            return twoDecimals(stats.avgMillis) + "/" + oneDecimal(stats.peakMillis) + "ms";
        };
        lines.push_back({"Render avg/peak: items " + section(sectionItems_) + " (" +
                             std::to_string(sectionItemCount_) + ") | mobs " +
                             section(sectionMobs_) + " | proj " + section(sectionProjectiles_),
                         0xFACC15u});
    }

    double y = canvas.height() - 8.0;
    for (const Line& line : lines) {
        style.fill = line.fill;
        text(canvas, line.text, canvas.width() - 8.0, y, style);
        y -= 15.0;
    }
}

std::string App::settingsPath() const {
    // Beside the session file, so one config directory holds both and a
    // portable install stays portable.
    return config_.sessionFile + "-settings";
}

} // namespace flix
