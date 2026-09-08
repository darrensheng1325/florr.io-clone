#include "client/ui/menus.h"
#include "client/ui/text_select.h"

#include "shared/core/process_stats.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <SDL.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "client/ui/item_tile.h"
#include "client/ui/menu_icons.h"
#include "client/ui/menu_theme.h"
#include "client/ui/text.h"
#include "shared/game/config.h"

namespace flix {

using namespace flix::ui;

#ifdef __EMSCRIPTEN__
EM_JS(int, flix_open_external_link, (const char* url), {
    const opened = window.open(UTF8ToString(url), '_blank');
    if (!opened) return -1;
    opened.opener = null;
    return 0;
});
#endif

bool openExternalLink(const std::string& url) {
#ifdef __EMSCRIPTEN__
    if (flix_open_external_link(url.c_str()) == 0) return true;
    std::fprintf(stderr, "flowrix: browser blocked external link %s\n", url.c_str());
#else
    if (SDL_OpenURL(url.c_str()) == 0) return true;
    std::fprintf(stderr, "flowrix: could not open %s (%s)\n", url.c_str(), SDL_GetError());
#endif
    return false;
}

namespace {

struct MenuMeta {
    const char* label;
    Key defaultKey;
};

/// Indexed by MenuId. Only the label and the key live here: which strip a menu
/// hangs off, and in what order, is the strip's business and not the enum's.
const std::array<MenuMeta, kMenuCount> kMenus = {{
    {"", Key::Unknown},                 // MenuId::None
    {"Inventory", Key::Z},
    {"Craft", Key::C},
    {"Talents", Key::X},
    {"Mob Gallery", Key::V},
    // The two storefronts, like the three overlay panels below, are reached
    // from the top icon strip only: they are opened between fights with the
    // mouse already on the button, so they spend no letter a panel a player
    // opens mid-fight could use.
    {"Shop", Key::Unknown},
    {"Skins", Key::Unknown},
    {"Leaderboard", Key::L},
    {"Settings", Key::Escape},
    {"Changelog", Key::Unknown},
    {"Notifications", Key::Unknown},
    {"Guild", Key::Unknown},
    {"Debug", Key::J},
}};

/// Every rebindable action, indexed by ControlAction, in the browser's
/// DEFAULT_CONTROLS order. The four rows with a menu behind them repeat that
/// menu's key from kMenus rather than owning a second copy of it: the panel
/// reads and writes those through ClientSettings::hotkeys.
const std::array<ControlMeta, kControlCount> kControls = {{
    {"Move up",               Key::W,          MenuId::None},
    {"Move down",             Key::S,          MenuId::None},
    {"Move left",             Key::A,          MenuId::None},
    {"Move right",            Key::D,          MenuId::None},
    {"Inventory",             Key::Z,          MenuId::Inventory},
    {"Crafting",              Key::C,          MenuId::Crafting},
    {"Skills",                Key::X,          MenuId::Talents},
    {"Toggle mouse controls", Key::K,          MenuId::None},
    {"Toggle hitboxes",       Key::H,          MenuId::None},
    {"Toggle debug menu",     Key::J,          MenuId::Debug},
    {"Zoom in",               Key::Equals,     MenuId::None},
    {"Zoom out",              Key::Minus,      MenuId::None},
    {"Chat",                  Key::Enter,      MenuId::None},
    {"Extend petals",         Key::Space,      MenuId::None},
    {"Retract petals",        Key::LeftShift,  MenuId::None},
}};

/// The two icon groups, which are NOT the same button at two positions.
///
/// The top row is chrome -- settings, changelog, the guild, the way out --
/// packed tight along the edge and read left to right. The left column is the
/// game: the four panels a player opens mid-fight, off a hotkey, without
/// looking. So the column's buttons are a third larger, spaced further apart
/// and captioned with their key; the row's are smaller and crowded. One
/// `kIconButton` used to serve both at 42px, which drew the row and the column
/// as the same control and left neither at the size its job wants.
///
/// Every part of a button is a fraction of its own side: a 3px rim and a 6px
/// corner on the 48 becomes 4 and 8 on the 64, so the two read as one shape at
/// two sizes rather than as two designs.
struct IconStyle {
    double side;
    double gap;
    double inset;   ///< from the edge it hangs off, on both axes
    double border;
    double radius;
    double glyph;   ///< the artwork's fitted box, centred in the face
};

constexpr IconStyle kTopIcon{48.0, 6.0, 9.0, 3.0, 6.0, 32.0};
constexpr IconStyle kColumnIcon{64.0, 11.0, 9.0, 4.0, 8.0, 43.0};

constexpr const IconStyle& iconStyle(bool topRow) { return topRow ? kTopIcon : kColumnIcon; }

/// The hotkey printed in the bottom-right corner of a column button, as the
/// loadout bar prints its slot caps. Only the column wears one. Some top-row
/// panels are bound too, but those are opened between fights with the mouse
/// already on the button; captioning all ten would turn a quiet strip of icons
/// into a wall of text for keys nobody reaches for mid-game.
constexpr double kIconKeyCapSize = 14.0;
constexpr double kIconKeyCapInset = 4.0;

/// The strip cascades in from the left on its first frame: each button slides
/// its own distance from the edge in 50ms, 40ms apart down the table.
constexpr double kIconSlideSeconds = 0.050;
constexpr double kIconStaggerSeconds = 0.040;
/// The changelog button rocks +/-12 degrees on an 800ms sine while unread.
constexpr double kShakePeriodSeconds = 0.8;
constexpr double kShakeRadians = 12.0 * kPi / 180.0;

/// The browser's hit test is inclusive on all four edges; Rect::contains is
/// half-open. On a 42px button that is a whole row of pixels that looks
/// clickable and is not.
bool insideInclusive(Rect r, Vec2 p) {
    return r.w > 0 && r.h > 0 && p.x >= r.x && p.x <= r.right() && p.y >= r.y && p.y <= r.bottom();
}

int menuIndex(MenuId id) { return static_cast<int>(id); }

/// Whether a menu's hotkey can be changed, which is exactly whether the
/// settings panel lists a row for it: `bindControl` is the only writer of
/// `hotkeys`, and it only ever reaches a menu named by a ControlAction.
///
/// Only those are persisted. A key nobody can rebind is a constant that lives
/// in kMenus, and writing it to the settings file as well would let an old
/// file pin a default this build has since moved -- which is how a player who
/// once ran the build where Settings opened on O would never get Escape.
bool menuKeyIsRebindable(int menu) {
    for (int i = 0; i < kControlCount; ++i) {
        if (menuIndex(kControls[static_cast<std::size_t>(i)].menu) == menu) return true;
    }
    return false;
}

// --- the loadout bar --------------------------------------------------------

/// The browser's two HContainers, in design px at scale 1: 70px primary slots
/// 20 apart over 50px secondary slots 15 apart, with the trash appended to the
/// second row. `kLoadoutBottomPad` is NOT scaled -- it is the phone-keyboard
/// gap, and the browser leaves it at 34 whatever the slots do.
constexpr double kLoadoutPrimarySize = 70.0;
constexpr double kLoadoutSecondarySize = 50.0;
constexpr double kLoadoutPrimaryGap = 20.0;
constexpr double kLoadoutSecondaryGap = 15.0;
constexpr double kLoadoutPrimaryMargin = 5.0;
constexpr double kLoadoutSecondaryMargin = 10.0;
constexpr double kLoadoutBottomPad = 34.0;

/// The title screen hands the bar a fixed box below centre rather than the
/// whole window, so it sits under the biome buttons instead of on the bottom
/// edge. In game it owns the viewport at three-quarter scale.
constexpr double kTitleLoadoutWidth = 900.0;
constexpr double kTitleLoadoutHeight = 210.0;
constexpr double kTitleLoadoutDrop = 50.0;
constexpr double kInGameLoadoutScale = 0.75;

/// The captions above the primary row. Bracketed, as gardn draws them, and
/// ending on [0] because the tenth slot is the zero key.
constexpr const char* kLoadoutKeyCaps[kLoadoutBarPrimary] = {"[1]", "[2]", "[3]", "[4]", "[5]",
                                                             "[6]", "[7]", "[8]", "[9]", "[0]"};

constexpr std::uint32_t kLoadoutSlotFill = 0xEEEEEEu;
constexpr std::uint32_t kLoadoutTrashFill = 0xCF8888u;

/// The petal riding the cursor mid-drag: a translucent tile in game, and on
/// the title screen the bare sprite the browser's HTML5 drag image is.
constexpr double kDragGhostSize = 50.0;
constexpr double kDragGhostAlpha = 0.85;
constexpr double kDragImageSize = 40.0;

struct LoadoutLayout {
    std::array<Rect, kLoadoutBarSlots> slots{};
    Rect trash{};
};

LoadoutLayout layoutLoadout(Rect box, double scale) {
    const double primarySize = kLoadoutPrimarySize * scale;
    const double secondarySize = kLoadoutSecondarySize * scale;
    const double primaryGap = kLoadoutPrimaryGap * scale;
    const double secondaryGap = kLoadoutSecondaryGap * scale;
    const double primaryMargin = kLoadoutPrimaryMargin * scale;
    const double secondaryMargin = kLoadoutSecondaryMargin * scale;
    const int cols = kLoadoutBarPrimary;

    const double primaryRowW = cols * primarySize + (cols - 1) * primaryGap;
    // The secondary row's width counts the trash, which is why the two rows do
    // not share a start x.
    const double secondaryRowW =
        cols * secondarySize + (cols - 1) * secondaryGap + secondaryGap + secondarySize;
    const double primaryStartX = box.x + (box.w - primaryRowW) * 0.5;
    const double secondaryStartX = box.x + (box.w - secondaryRowW) * 0.5;

    const double bottomPad = kLoadoutBottomPad + secondaryMargin;
    const double secondaryY = box.y + box.h - bottomPad - secondarySize;
    const double primaryY = secondaryY - secondaryMargin - primaryMargin - primarySize;

    LoadoutLayout out;
    for (int i = 0; i < cols; ++i) {
        out.slots[static_cast<std::size_t>(i)] = {primaryStartX + i * (primarySize + primaryGap),
                                                  primaryY, primarySize, primarySize};
        out.slots[static_cast<std::size_t>(cols + i)] = {
            secondaryStartX + i * (secondarySize + secondaryGap), secondaryY, secondarySize,
            secondarySize};
    }
    out.trash = {secondaryStartX + cols * (secondarySize + secondaryGap), secondaryY,
                 secondarySize, secondarySize};
    return out;
}

/// One slot's chrome: a darker rounded plate with a SHARP inner fill, never a
/// stroke. Both insets are proportions of the slot so the secondary row reads
/// as the same object at a smaller size.
void drawLoadoutSlot(Canvas& canvas, Rect r, std::uint32_t fill, bool highlighted) {
    const double lineW = r.w / 12.0;
    const double radius = r.w / 20.0;
    setFill(canvas, shade(fill, 0.80));
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                     static_cast<float>(r.h), static_cast<float>(radius));
    canvas.fill();

    const auto inner = [&] {
        canvas.fillRect(static_cast<float>(r.x + lineW), static_cast<float>(r.y + lineW),
                        static_cast<float>(r.w - lineW * 2), static_cast<float>(r.h - lineW * 2));
    };
    setFill(canvas, fill);
    inner();
    if (!highlighted) return;
    setFill(canvas, kPaper, 0.12);
    inner();
}

/// The bracketed key caption above a primary slot, and the [T] beside the
/// trash. Slightly translucent, which is what keeps it from competing with the
/// petal names inside the slots.
void drawKeyLabel(Canvas& canvas, const std::string& label, double x, double y, Align align) {
    TextStyle style;
    style.size = 16.0;
    style.bold = true;
    style.fill = kPaper;
    style.stroke = kInk;
    style.strokeWidth = 3.0;
    style.align = align;
    style.baseline = Baseline::Middle;
    canvas.setGlobalAlpha(0.85f);
    text(canvas, label, x, y, style);
    canvas.setGlobalAlpha(1.0f);
}

/// Advances the secondary selection to the next non-empty slot in `step`'s
/// direction, or -1 when the whole row is empty.
int cycleSecondary(const Profile& profile, int current, int step) {
    int cur = current < 0 ? -1 : current;
    for (int i = 0; i < kLoadoutBarPrimary; ++i) {
        cur = (cur + step + kLoadoutBarPrimary) % kLoadoutBarPrimary;
        const auto at = static_cast<std::size_t>(kLoadoutBarPrimary + cur);
        if (at < profile.loadout.size() && !profile.loadout[at].empty()) return cur;
    }
    return -1;
}

} // namespace

const char* menuLabel(MenuId id) {
    const int i = menuIndex(id);
    return (i >= 0 && i < kMenuCount) ? kMenus[static_cast<std::size_t>(i)].label : "";
}

const char* keyName(Key key) {
    switch (key) {
        case Key::A: return "A"; case Key::B: return "B"; case Key::C: return "C";
        case Key::D: return "D"; case Key::E: return "E"; case Key::F: return "F";
        case Key::G: return "G"; case Key::H: return "H"; case Key::I: return "I";
        case Key::J: return "J"; case Key::K: return "K"; case Key::L: return "L";
        case Key::M: return "M"; case Key::N: return "N"; case Key::O: return "O";
        case Key::P: return "P"; case Key::Q: return "Q"; case Key::R: return "R";
        case Key::S: return "S"; case Key::T: return "T"; case Key::U: return "U";
        case Key::V: return "V"; case Key::W: return "W"; case Key::X: return "X";
        case Key::Y: return "Y"; case Key::Z: return "Z";
        case Key::Num0: return "0"; case Key::Num1: return "1"; case Key::Num2: return "2";
        case Key::Num3: return "3"; case Key::Num4: return "4"; case Key::Num5: return "5";
        case Key::Num6: return "6"; case Key::Num7: return "7"; case Key::Num8: return "8";
        case Key::Num9: return "9";
        case Key::Space: return "Space"; case Key::Enter: return "Enter";
        case Key::Tab: return "Tab"; case Key::Escape: return "Esc";
        case Key::Minus: return "-"; case Key::Equals: return "=";
        case Key::Comma: return ","; case Key::Period: return ".";
        case Key::Slash: return "/"; case Key::Backslash: return "\\";
        case Key::Semicolon: return ";"; case Key::Apostrophe: return "'";
        case Key::Left: return "Left"; case Key::Right: return "Right";
        case Key::Up: return "Up"; case Key::Down: return "Down";
        case Key::F1: return "F1"; case Key::F2: return "F2"; case Key::F3: return "F3";
        case Key::F4: return "F4"; case Key::F5: return "F5"; case Key::F6: return "F6";
        default: return "Unbound";
    }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

namespace {

/// The other half of a modifier pair, or Key::Unknown for everything else.
Key pairedKey(Key key) {
    switch (key) {
        case Key::LeftShift: return Key::RightShift;
        case Key::RightShift: return Key::LeftShift;
        case Key::LeftCtrl: return Key::RightCtrl;
        case Key::RightCtrl: return Key::LeftCtrl;
        case Key::LeftAlt: return Key::RightAlt;
        case Key::RightAlt: return Key::LeftAlt;
        default: return Key::Unknown;
    }
}

} // namespace

bool boundKeyDown(const Window& window, Key key) {
    if (key == Key::Unknown) return false;
    const Key other = pairedKey(key);
    return window.keyDown(key) || (other != Key::Unknown && window.keyDown(other));
}

bool boundKeyPressed(const Window& window, Key key) {
    if (key == Key::Unknown) return false;
    const Key other = pairedKey(key);
    return window.keyPressed(key) || (other != Key::Unknown && window.keyPressed(other));
}

const ControlMeta& controlMeta(ControlAction action) {
    static constexpr ControlMeta kUnbound{"", Key::Unknown, MenuId::None};
    const int i = static_cast<int>(action);
    if (i < 0 || i >= kControlCount) return kUnbound;
    return kControls[static_cast<std::size_t>(i)];
}

ClientSettings::ClientSettings() {
    for (int i = 0; i < kMenuCount; ++i) {
        hotkeys[static_cast<std::size_t>(i)] = kMenus[static_cast<std::size_t>(i)].defaultKey;
    }
    for (int i = 0; i < kControlCount; ++i) {
        controls[static_cast<std::size_t>(i)] = kControls[static_cast<std::size_t>(i)].defaultKey;
    }
}

Key ClientSettings::controlKey(ControlAction action) const {
    const ControlMeta& meta = controlMeta(action);
    if (meta.menu != MenuId::None) return hotkeys[static_cast<std::size_t>(meta.menu)];
    const int i = static_cast<int>(action);
    if (i < 0 || i >= kControlCount) return Key::Unknown;
    return controls[static_cast<std::size_t>(i)];
}

void ClientSettings::bindControl(ControlAction action, Key key) {
    const int i = static_cast<int>(action);
    if (i < 0 || i >= kControlCount) return;
    const ControlMeta& meta = controlMeta(action);
    if (meta.menu == MenuId::None) {
        controls[static_cast<std::size_t>(i)] = key;
        return;
    }
    for (int other = 1; other < kMenuCount; ++other) {
        if (hotkeys[static_cast<std::size_t>(other)] == key) {
            hotkeys[static_cast<std::size_t>(other)] = Key::Unknown;
        }
    }
    hotkeys[static_cast<std::size_t>(meta.menu)] = key;
}

bool ClientSettings::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) return false;
    std::string key;
    std::string value;
    // A settings file is a comfort, not a contract: an unreadable line is
    // skipped and the default kept, rather than the whole file being rejected.
    while (file >> key >> value) {
        const int number = std::atoi(value.c_str());
        if (key == "names") render.names = number != 0;
        else if (key == "healthBars") render.healthBars = number != 0;
        else if (key == "damageNumbers") render.damageNumbers = number != 0;
        else if (key == "hitboxes") render.hitboxes = number != 0;
        else if (key == "chat") showChat = number != 0;
        else if (key == "menuBar") showMenuBar = number != 0;
        else if (key == "stats") showStats = number != 0;
        else if (key == "debugButton") showDebugButton = number != 0;
        else if (key == "changelogSeen") changelogSeen = number;
        else if (key == "zoom") zoom = clamp(std::atof(value.c_str()), kMinZoom, kMaxZoom);
        else if (key == "interp") interpolation = clamp(std::atof(value.c_str()), 0.05, 0.5);
        else if (key == "renderScale") renderScale = clamp(std::atof(value.c_str()), 0.25, 1.0);
        else if (key == "biome") spawnBiome = (value == "-" ? std::string() : value);
        else if (key == "mouseControls") useMouseControls = number != 0;
        else if (key == "tutorialDone") tutorialCompleted = number != 0;
        else if (key == "tutorialStep") tutorialStep = number;
        else if (key == "notifRead") readNotifications.push_back(value);
        else if (key.rfind("key.", 0) == 0) {
            const int slot = std::atoi(key.c_str() + 4);
            if (slot > 0 && slot < kMenuCount && menuKeyIsRebindable(slot) && number > 0 &&
                number < static_cast<int>(Key::Count)) {
                hotkeys[static_cast<std::size_t>(slot)] = static_cast<Key>(number);
            }
        }
        // Unknown is a real value here, unlike a menu hotkey: a row can be
        // left bound to nothing, and a zero must not fall back to the default.
        else if (key.rfind("ctl.", 0) == 0) {
            const int slot = std::atoi(key.c_str() + 4);
            if (slot >= 0 && slot < kControlCount && number >= 0 &&
                number < static_cast<int>(Key::Count)) {
                controls[static_cast<std::size_t>(slot)] = static_cast<Key>(number);
            }
        }
    }
    return true;
}

bool ClientSettings::save(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    file << "names " << (render.names ? 1 : 0) << '\n'
         << "healthBars " << (render.healthBars ? 1 : 0) << '\n'
         << "damageNumbers " << (render.damageNumbers ? 1 : 0) << '\n'
         << "hitboxes " << (render.hitboxes ? 1 : 0) << '\n'
         << "chat " << (showChat ? 1 : 0) << '\n'
         << "menuBar " << (showMenuBar ? 1 : 0) << '\n'
         << "stats " << (showStats ? 1 : 0) << '\n'
         << "debugButton " << (showDebugButton ? 1 : 0) << '\n'
         << "changelogSeen " << changelogSeen << '\n'
         << "zoom " << zoom << '\n'
         << "interp " << interpolation << '\n'
         << "renderScale " << renderScale << '\n'
         // A dash rather than an empty field: the reader splits on whitespace,
         // and an empty value would swallow the next key as its own.
         << "biome " << (spawnBiome.empty() ? std::string("-") : spawnBiome) << '\n'
         << "mouseControls " << (useMouseControls ? 1 : 0) << '\n'
         << "tutorialDone " << (tutorialCompleted ? 1 : 0) << '\n'
         << "tutorialStep " << tutorialStep << '\n';
    // Capped at the server's own retention: it keeps the last thousand
    // notifications, so a read mark older than that can never be asked about
    // again and would only grow this file forever. The tail is the newest.
    constexpr std::size_t kReadMarkCap = 1000;
    const std::size_t firstMark =
        readNotifications.size() > kReadMarkCap ? readNotifications.size() - kReadMarkCap : 0;
    for (std::size_t i = firstMark; i < readNotifications.size(); ++i) {
        file << "notifRead " << readNotifications[i] << '\n';
    }
    for (int i = 1; i < kMenuCount; ++i) {
        if (!menuKeyIsRebindable(i)) continue;
        file << "key." << i << ' ' << static_cast<int>(hotkeys[static_cast<std::size_t>(i)])
             << '\n';
    }
    // The menu-backed rows are already above as key.<menu>; writing their
    // unused slots too would leave two spellings of one binding on disk.
    for (int i = 0; i < kControlCount; ++i) {
        if (kControls[static_cast<std::size_t>(i)].menu != MenuId::None) continue;
        file << "ctl." << i << ' ' << static_cast<int>(controls[static_cast<std::size_t>(i)])
             << '\n';
    }
    return static_cast<bool>(file);
}

// ---------------------------------------------------------------------------
// MenuSystem
// ---------------------------------------------------------------------------

void MenuSystem::toggle(MenuId id) {
    // Opening a second menu closes the first. They share an anchor, so two at
    // once would be one menu with another hidden behind it.
    const bool opening = open_ != id;
    open_ = opening ? id : MenuId::None;
    drag_.clear();
    // A panel opens at the top of its list, with no search text and nothing
    // staged. Reopening onto whatever was left behind is how a player ends up
    // staring at an empty grid because a filter they forgot is still applied.
    if (!opening) return;
    // From below the viewport every time, even when the card it replaces was
    // already seated: the browser shell it mirrors starts each open from
    // `translateY(100vh)`.
    panelSlide_ = 0.0;
    switch (id) {
        case MenuId::Inventory:   inventory_.reset(); break;
        case MenuId::Crafting:    crafting_.reset(); break;
        case MenuId::Talents:     talents_.reset(); break;
        case MenuId::Gallery:     gallery_.reset(); break;
        case MenuId::Shop:        shop_.reset(); break;
        case MenuId::Skins:       skins_.reset(); break;
        case MenuId::Leaderboard: leaderboard_.reset(); break;
        case MenuId::Settings:    settings_panel_.reset(); break;
        case MenuId::Changelog:   changelog_.reset(); break;
        case MenuId::Notifications: notifications_.reset(); break;
        case MenuId::Guild:       guild_.reset(); break;
        case MenuId::Debug:       debug_.reset(); break;
        default: break;
    }
}

void MenuSystem::close() {
    open_ = MenuId::None;
    drag_.clear();
}

bool MenuSystem::handleKeys(Window& window) {
    // A settings row waiting for a key must swallow every key: binding the
    // inventory to G should not also open the bestiary on the way past.
    if (settings_panel_.capturingKey()) return true;
    if (wantsText_) return false;
    // A shortcut is not a hotkey. Every menu binding is a bare letter, so
    // without this Ctrl/Cmd+C opens the forge on its way to copying a
    // selection, and Cmd+A opens the bestiary. Not the modifier ROW being
    // rebound -- capturingKey above has already claimed the keyboard for that.
    if (window.ctrlHeld()) return false;

    // Escape gets no special case: it is the settings panel's hotkey and is
    // read from kMenus with the rest of them below, so it toggles that panel
    // exactly as Z toggles the inventory. Nothing else in the client claims
    // it while a game is running -- it no longer closes an arbitrary panel,
    // and it no longer leaves to the title screen.

    // Zoom, and further down the hitbox and mouse-control switches: the three
    // settings a key changes mid-game. All three are the game screen's alone,
    // exactly as the browser binds them inside Game's own keydown -- there is
    // no world to zoom on the title screen. Zoom is tested before the menu
    // keys and the two switches after them, which is the order the browser
    // tests them in: that order decides which action a key bound to two of
    // them reaches, and this client has to decide it the same way.
    if (inGame_) {
        if (boundKeyPressed(window, settings_.controlKey(ControlAction::ZoomOut))) {
            settings_.zoom = clamp(settings_.zoom - kZoomKeyStep, kMinZoom, kMaxZoom);
            return true;
        }
        if (boundKeyPressed(window, settings_.controlKey(ControlAction::ZoomIn))) {
            settings_.zoom = clamp(settings_.zoom + kZoomKeyStep, kMinZoom, kMaxZoom);
            return true;
        }
    }

    // The loadout keys are recorded rather than acted on: only drawLoadoutBar
    // has the network client and the account's loadout to act with.
    //
    // And they are the game screen's alone. The browser binds Q/E/T and the
    // number row inside Game's own keydown; the title screen's inventory
    // manager has no keyboard path to the bar at all, only drag and drop.
    if (inGame_) {
        if (window.keyPressed(Key::E)) { pendingCycle_ = 1; return true; }
        if (window.keyPressed(Key::Q)) { pendingCycle_ = -1; return true; }
        if (window.keyPressed(Key::T)) { pendingSecondaryDelete_ = true; return true; }
        static constexpr Key kLoadoutKeys[kLoadoutBarPrimary] = {
            Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5,
            Key::Num6, Key::Num7, Key::Num8, Key::Num9, Key::Num0,
        };
        for (int i = 0; i < kLoadoutBarPrimary; ++i) {
            if (window.keyPressed(kLoadoutKeys[i])) {
                pendingSwapSlot_ = i;
                return true;
            }
        }
    }

    for (int i = 1; i < kMenuCount; ++i) {
        const Key key = settings_.hotkeys[static_cast<std::size_t>(i)];
        if (key == Key::Unknown || !window.keyPressed(key)) continue;
        // The bug button is the only way into the debug panel in the browser,
        // and its key is gated on the same switch.
        if (static_cast<MenuId>(i) == MenuId::Debug && !settings_.showDebugButton) continue;
        toggle(static_cast<MenuId>(i));
        return true;
    }

    if (inGame_) {
        if (boundKeyPressed(window, settings_.controlKey(ControlAction::ToggleMouseControls))) {
            settings_.useMouseControls = !settings_.useMouseControls;
            return true;
        }
        if (boundKeyPressed(window, settings_.controlKey(ControlAction::ToggleHitboxes))) {
            settings_.render.hitboxes = !settings_.render.hitboxes;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Panel geometry
// ---------------------------------------------------------------------------
//
// The browser build gives every panel its own box; there is no shared anchor.
// The tall lists are `position: fixed; top: 33.33vh; left: 100px; height:
// 66.67vh` DOM shells whose width is the only thing that differs between them,
// and the overlays are canvas panels pinned at a literal (20, 72) under the
// top icon row at a fixed size. Both live here rather than in the panels so
// the twelve anchors can be read against each other.

namespace {

/// A tall list beside the bottom icon column, as wide as its content needs.
///
/// Nothing here is clamped to the window. Every one of these panels is a
/// `position: fixed; top: 33.33vh; left: 100px; height: 66.67vh` shell with a
/// literal width, and a viewport too narrow for one simply lets it overflow --
/// it does not narrow the card and re-centre it, which would move a panel a
/// player has learnt the position of.
Rect listPanel(double width, int, int viewHeight) {
    const double top = static_cast<double>(viewHeight) * kMenuListTopFraction;
    const double height =
        static_cast<double>(viewHeight) * kMenuListHeightFraction - kMenuListBottomPad;
    return {kMenuInsetX, top, width, std::max(0.0, height)};
}

/// An overlay pinned under the top icon row, at a literal size. Also unclamped:
/// these are canvas panels drawn from fixed PANEL_X/PANEL_Y/PANEL_WIDTH
/// constants, and clamping the rect would leave the mouse-capture box smaller
/// than the card the panel paints for itself.
Rect cornerPanel(double width, double height, double top, int, int) {
    return {kMenuCornerX, top, width, height};
}

} // namespace

Rect InventoryPanel::bounds(int w, int h) { return listPanel(preferredWidth(), w, h); }
Rect CraftingPanel::bounds(int w, int h) { return listPanel(preferredWidth(), w, h); }
/// The talent card is SQUARE, alone among the list panels: the tree is a fan
/// spun about its own centre, and a tall card would only add height the fan
/// never reaches while cropping the width it spreads across. It keeps the
/// family's left inset and bottom edge, so it still reads as one of the four;
/// the family's nominal width is what the square's side replaces.
Rect TalentsPanel::bounds(int w, int h) {
    const Rect list = listPanel(preferredWidth(), w, h);
    const double side = list.h;
    return {list.x, list.bottom() - side, side, side};
}

/// The bestiary left the list family: it is a corner overlay, hung under the
/// top icon row beside the other read-only cards, because it is one of them --
/// nothing is dragged into or out of it, so it does not need to stand clear of
/// the loadout the way the inventory, the forge and the tree do.
Rect GalleryPanel::bounds(int w, int h) {
    return cornerPanel(preferredWidth(), GalleryPanel::preferredHeight(), kMenuCornerY, w, h);
}

Rect ShopPanel::bounds(int w, int h) {
    return cornerPanel(preferredWidth(), ShopPanel::preferredHeight(), kMenuCornerY, w, h);
}
Rect SkinsPanel::bounds(int w, int h) {
    return cornerPanel(preferredWidth(), SkinsPanel::preferredHeight(), kMenuCornerY, w, h);
}
Rect LeaderboardPanel::bounds(int w, int h) {
    return cornerPanel(preferredWidth(), 500.0, kMenuCornerY, w, h);
}
// Settings and debug sit two pixels higher than the other overlays, which is
// what the browser does; not worth "correcting" into disagreement with it.
Rect SettingsPanel::bounds(int w, int h) {
    return cornerPanel(preferredWidth(), 500.0, kMenuCornerY - 2.0, w, h);
}
Rect DebugPanel::bounds(int w, int h) {
    return cornerPanel(preferredWidth(), 500.0, kMenuCornerY - 2.0, w, h);
}
Rect ChangelogPanel::bounds(int w, int h) {
    return cornerPanel(preferredWidth(), 500.0, kMenuCornerY, w, h);
}
Rect NotificationsPanel::bounds(int w, int h) {
    return cornerPanel(preferredWidth(), 500.0, kMenuCornerY, w, h);
}
Rect GuildPanel::bounds(int w, int h) { return cornerPanel(preferredWidth(), 500.0, kMenuCornerY, w, h); }

Rect MenuSystem::panelBounds(MenuId id, int viewWidth, int viewHeight) {
    switch (id) {
        case MenuId::Inventory:     return InventoryPanel::bounds(viewWidth, viewHeight);
        case MenuId::Crafting:      return CraftingPanel::bounds(viewWidth, viewHeight);
        case MenuId::Talents:       return TalentsPanel::bounds(viewWidth, viewHeight);
        case MenuId::Gallery:       return GalleryPanel::bounds(viewWidth, viewHeight);
        case MenuId::Shop:          return ShopPanel::bounds(viewWidth, viewHeight);
        case MenuId::Skins:         return SkinsPanel::bounds(viewWidth, viewHeight);
        case MenuId::Leaderboard:   return LeaderboardPanel::bounds(viewWidth, viewHeight);
        case MenuId::Settings:      return SettingsPanel::bounds(viewWidth, viewHeight);
        case MenuId::Changelog:     return ChangelogPanel::bounds(viewWidth, viewHeight);
        case MenuId::Notifications: return NotificationsPanel::bounds(viewWidth, viewHeight);
        case MenuId::Guild:         return GuildPanel::bounds(viewWidth, viewHeight);
        case MenuId::Debug:         return DebugPanel::bounds(viewWidth, viewHeight);
        default:                    return listPanel(380.0, viewWidth, viewHeight);
    }
}

const SvgDocument* MenuSystem::icon(int index) {
    if (index < 0 || index >= ui::kMenuIconCount) return nullptr;
    // Compiled on first use rather than in a constructor: the fonts and the
    // sprite cache are already loaded by then, and a client that never opens a
    // menu never pays for the artwork.
    if (icons_.empty()) icons_.resize(static_cast<std::size_t>(ui::kMenuIconCount));
    std::shared_ptr<SvgDocument>& slot = icons_[static_cast<std::size_t>(index)];
    if (!slot) {
        slot = std::make_shared<SvgDocument>(
            SvgDocument::fromString(ui::kMenuIcons[static_cast<std::size_t>(index)].svg));
    }
    return slot->empty() ? nullptr : slot.get();
}

const std::array<MenuSystem::StripSlot, kStripSlotCount>& MenuSystem::strip() {
    using A = StripAction;
    static const std::array<StripSlot, kStripSlotCount> kSlots = {{
        {MenuId::Settings,      A::OpenMenu, "settings",      true,  0xB3B3B3u, 0x8F8F8Fu},
        {MenuId::Changelog,     A::OpenMenu, "changelog",     true,  0x00DB3Eu, 0x00AF32u},
        {MenuId::Notifications, A::OpenMenu, "notifications", true,  0x4A90E2u, 0x3B73B5u},
        {MenuId::Leaderboard,   A::OpenMenu, "leaderboard",   true,  0xE8A023u, 0xBA801Cu},
        {MenuId::Guild,         A::OpenMenu, "guild",         true,  0x27DADEu, 0x1FB3B0u},
        {MenuId::Skins,         A::OpenMenu, "skins",         true,  0xC45CFFu, 0x9A3FD0u},
        {MenuId::Gallery,       A::OpenMenu, "mob_gallery",   true,  0xD6C206u, 0xAB9B05u},
        {MenuId::Shop,          A::OpenMenu, "stars",         true,  0x7EF16Bu, 0x64C156u},
        {MenuId::None,          A::Discord,  "discord",       true,  0x5865F2u, 0x4752C4u},
        {MenuId::Debug,         A::OpenMenu, "debug",         true,  0x666666u, 0x4D4D4Du},
        {MenuId::None,          A::Exit,     "exit_button",   true,  0xFF0000u, 0xCC0000u},
        {MenuId::Inventory,     A::OpenMenu, "inventory",     false, 0x00B3FFu, 0x008FCCu},
        {MenuId::Talents,       A::OpenMenu, "skills",        false, 0x9D4EDDu, 0x7E3EB1u},
        {MenuId::Crafting,      A::OpenMenu, "craft",         false, 0xFF9D00u, 0xCC7E00u},
    }};
    return kSlots;
}

void MenuSystem::activateStripSlot(int index) {
    const StripSlot& slot = strip()[static_cast<std::size_t>(index)];
    switch (slot.action) {
        case StripAction::Exit: exitRequested_ = true; return;
        case StripAction::Discord: openExternalLink(kDiscordInvite); return;
        case StripAction::OpenMenu: break;
    }
    // Opening the panel is what marks the changelog read, which is what stops
    // the button shaking -- the same gesture, not a separate acknowledgement.
    if (slot.menu == MenuId::Changelog) settings_.changelogSeen = changelogEntries_;
    toggle(slot.menu);
}

void MenuSystem::drawIconStrip(Canvas& canvas, Window& window, double timeSeconds) {
    stripRects_.fill(Rect{});

    const Vec2 mouse{window.mouseX(), window.mouseY()};
    const bool pressed = window.mousePressed(MouseButton::Left);
    const bool released = window.mouseReleased(MouseButton::Left);

    // Hidden slots collapse out of the layout rather than leaving a gap, so
    // the exit button lands where the row ends and not at a reserved position.
    std::array<bool, kStripSlotCount> visible{};
    for (int i = 0; i < kStripSlotCount; ++i) {
        const StripSlot& slot = strip()[static_cast<std::size_t>(i)];
        if (slot.action == StripAction::Exit) visible[static_cast<std::size_t>(i)] = inGame_;
        else if (slot.menu == MenuId::Debug)
            visible[static_cast<std::size_t>(i)] = settings_.showDebugButton;
        else visible[static_cast<std::size_t>(i)] = true;
    }

    // Lay both groups out first: the slide-in offset is a per-button distance
    // from the left edge, so it needs the final x before anything is drawn.
    double x = kTopIcon.inset;
    int columnCount = 0;
    for (int i = 0; i < kStripSlotCount; ++i) {
        const auto at = static_cast<std::size_t>(i);
        if (!visible[at]) continue;
        if (!strip()[at].topRow) { ++columnCount; continue; }
        stripRects_[at] = {x, kTopIcon.inset, kTopIcon.side, kTopIcon.side};
        x += kTopIcon.side + kTopIcon.gap;
    }
    double y = canvas.height() - kColumnIcon.inset - columnCount * kColumnIcon.side -
               std::max(0, columnCount - 1) * kColumnIcon.gap;
    for (int i = 0; i < kStripSlotCount; ++i) {
        const auto at = static_cast<std::size_t>(i);
        if (!visible[at] || strip()[at].topRow) continue;
        stripRects_[at] = {kColumnIcon.inset, y, kColumnIcon.side, kColumnIcon.side};
        y += kColumnIcon.side + kColumnIcon.gap;
    }

    // First frame: stagger every visible button so the strip cascades in.
    // After that only a false->true edge starts a slide, which is what makes
    // the exit and debug buttons sweep in when they appear.
    if (!stripSeeded_) {
        stripSeeded_ = true;
        slideStart_.fill(-1.0);
        double stagger = 0;
        for (int i = 0; i < kStripSlotCount; ++i) {
            if (!visible[static_cast<std::size_t>(i)]) continue;
            slideStart_[static_cast<std::size_t>(i)] = timeSeconds + stagger;
            stagger += kIconStaggerSeconds;
        }
    } else {
        for (int i = 0; i < kStripSlotCount; ++i) {
            const auto at = static_cast<std::size_t>(i);
            if (visible[at] && !stripVisible_[at]) slideStart_[at] = timeSeconds;
            else if (!visible[at]) slideStart_[at] = -1.0;
        }
    }
    stripVisible_ = visible;

    for (int i = 0; i < kStripSlotCount; ++i) {
        const auto at = static_cast<std::size_t>(i);
        if (!visible[at]) continue;
        const StripSlot& slot = strip()[at];
        const IconStyle& style = iconStyle(slot.topRow);
        const Rect r = stripRects_[at];
        // Against the UN-translated rect: neither the slide nor the shake may
        // move a button out from under the cursor that is already on it.
        const bool hovered = insideInclusive(r, mouse);
        if (pressed && hovered) pressedSlot_ = i;

        canvas.save();
        if (slideStart_[at] >= 0) {
            const double elapsed = timeSeconds - slideStart_[at];
            const double travel = -(r.x + style.side);
            if (elapsed < 0) {
                canvas.translate(static_cast<float>(travel), 0.0f);
            } else if (elapsed < kIconSlideSeconds) {
                const double progress = elapsed / kIconSlideSeconds;
                const double eased = 1.0 - std::pow(1.0 - progress, 3.0);
                canvas.translate(static_cast<float>(travel * (1.0 - eased)), 0.0f);
            } else {
                slideStart_[at] = -1.0;
            }
        }
        if (slot.menu == MenuId::Changelog && changelogUnread()) {
            const double phase = std::fmod(timeSeconds, kShakePeriodSeconds) / kShakePeriodSeconds;
            const double cx = r.x + r.w * 0.5;
            const double cy = r.y + r.h * 0.5;
            canvas.translate(static_cast<float>(cx), static_cast<float>(cy));
            canvas.rotate(static_cast<float>(std::sin(phase * kTau) * kShakeRadians));
            canvas.translate(static_cast<float>(-cx), static_cast<float>(-cy));
        }

        // Two filled rects rather than a stroke: the face keeps the frame's own
        // rounding one step tighter, where a centred stroke would blow the
        // outer radius out by half its width.
        setFill(canvas, slot.border);
        canvas.beginPath();
        canvas.roundRect(static_cast<float>(r.x), static_cast<float>(r.y),
                         static_cast<float>(r.w), static_cast<float>(r.h),
                         static_cast<float>(style.radius));
        canvas.fill();
        setFill(canvas, slot.fill);
        canvas.beginPath();
        canvas.roundRect(static_cast<float>(r.x + style.border),
                         static_cast<float>(r.y + style.border),
                         static_cast<float>(r.w - style.border * 2),
                         static_cast<float>(r.h - style.border * 2),
                         static_cast<float>(std::max(0.0, style.radius - style.border * 0.5)));
        canvas.fill();

        // Press wins over hover, and it follows the button the press began on
        // even once the cursor has left it.
        if (hovered || pressedSlot_ == i) {
            setFill(canvas, pressedSlot_ == i ? kInk : kPaper, 0.15);
            canvas.beginPath();
            canvas.roundRect(static_cast<float>(r.x), static_cast<float>(r.y),
                             static_cast<float>(r.w), static_cast<float>(r.h),
                             static_cast<float>(style.radius));
            canvas.fill();
        }

        // After the tint, so the glyph is never washed out by it.
        if (const SvgDocument* glyph = icon(ui::menuIconIndex(slot.icon))) {
            glyph->renderFitted(canvas, static_cast<float>(r.x + (r.w - style.glyph) * 0.5),
                                static_cast<float>(r.y + (r.h - style.glyph) * 0.5),
                                static_cast<float>(style.glyph), static_cast<float>(timeSeconds));
        }

        // The hotkey, over the glyph in the corner it is least in the way of.
        // Read live off the settings rather than off kMenus' default, so a key
        // rebound in the settings panel relabels the button it belongs to.
        if (!slot.topRow && slot.menu != MenuId::None) {
            const Key bound = settings_.hotkeys[static_cast<std::size_t>(menuIndex(slot.menu))];
            if (bound != Key::Unknown) {
                TextStyle cap;
                cap.size = kIconKeyCapSize;
                cap.bold = true;
                cap.fill = kPaper;
                cap.stroke = kInk;
                cap.strokeWidth = 3.0;
                cap.roundJoin = true;
                cap.align = Align::Right;
                cap.baseline = Baseline::Bottom;
                text(canvas, std::string("[") + keyName(bound) + "]",
                     r.right() - kIconKeyCapInset, r.bottom() - kIconKeyCapInset, cap);
            }
        }
        canvas.restore();

        // A release only counts when the press landed on the same button; a
        // drag that started on empty canvas must not activate what it ends on.
        if (released && hovered && pressedSlot_ == i) activateStripSlot(i);
    }
    if (released) pressedSlot_ = -1;
}

namespace {

/// How far through its reload the flower's slot `index` is: 0 the moment it
/// broke, 1 once it is back. The wedge is drawn from this, so a slot that is
/// not reloading has to read as 1 rather than as 0.
///
/// The server streams what is LEFT of a reload and not how long the whole one
/// was, because the total is already here: both sides size a reload from the
/// same petal config.
double slotReloadProgress(const NetClient& net, int index) {
    if (index < 0 || index >= kLoadoutActiveSlots) return 1.0;
    const double remaining = net.view().self().slotReloadRemainingMillis[
        static_cast<std::size_t>(index)];
    if (remaining <= 0.0) return 1.0;
    const Profile& profile = net.profile();
    const auto at = static_cast<std::size_t>(index);
    if (at >= profile.loadout.size() || profile.loadout[at].empty()) return 1.0;
    const PetalStats stats = content().petalStats(profile.loadout[at].petalIndex,
                                                  profile.loadout[at].rarity);
    const double total = stats.reloadMillis > 0.0 ? stats.reloadMillis : kDefaultPetalReloadMillis;
    // A remaining that outruns the total is a slot whose petal has just been
    // swapped for a slower one: clamped, it sweeps from the start rather than
    // showing an inverted wedge for a frame.
    return std::clamp(1.0 - remaining / total, 0.0, 1.0);
}

/// How much of the flower's slot `index` is still standing, for the tile fill.
/// A slot the server has said nothing about -- a storage row, or the frames
/// before the first snapshot -- reads as untouched.
double slotHealthFraction(const NetClient& net, int index) {
    if (index < 0 || index >= kLoadoutActiveSlots) return 1.0;
    return net.view().self().slotHealthFraction[static_cast<std::size_t>(index)];
}

}  // namespace

void MenuSystem::drawLoadoutBar(Canvas& canvas, Window& window, NetClient& net,
                                const SpriteCache& sprites, double timeSeconds) {
    const Profile& profile = net.profile();
    const int owned = static_cast<int>(profile.loadout.size());

    // Rises into place over its first frames and sinks back the same way. A
    // plain per-frame lerp with no time term, as in the browser: the ratio is
    // the feel, and a dt-corrected version overshoots at the frame rates this
    // has to survive. The target is whether there is a loadout to show at all,
    // which is the browser's own show()/hide() rule.
    const double target = owned > 0 ? 1.0 : 0.0;
    loadoutSlide_ += (target - loadoutSlide_) * 0.2;
    if (std::fabs(loadoutSlide_ - target) < 0.005) loadoutSlide_ = target;
    if (loadoutSlide_ <= 0.005) {
        // Nothing painted means nothing to hit: a bar that has sunk off-screen
        // must not go on answering for clicks where it used to be, and it has
        // no selection or queued keystroke to carry into the next screen.
        loadoutHovered_ = -1;
        loadoutGrabbable_ = false;
        selectedSecondary_ = -1;
        pendingCycle_ = 0;
        pendingSwapSlot_ = -1;
        pendingSecondaryDelete_ = false;
        return;
    }

    // The title screen gives the bar a fixed box below centre; in game it owns
    // the viewport and every metric shrinks to three quarters.
    const double scale = inGame_ ? kInGameLoadoutScale : 1.0;
    const Rect box = inGame_
        ? Rect{0.0, 0.0, static_cast<double>(canvas.width()),
               static_cast<double>(canvas.height())}
        : Rect{(canvas.width() - kTitleLoadoutWidth) * 0.5,
               canvas.height() * 0.5 + kTitleLoadoutDrop, kTitleLoadoutWidth,
               kTitleLoadoutHeight};
    const LoadoutLayout layout = layoutLoadout(box, scale);

    // Q/E/T and the number keys were recorded by handleKeys, which has no
    // network client; this is the first place that can act on them.
    if (selectedSecondary_ >= 0 && timeSeconds - lastSelectTime_ > 5.0) selectedSecondary_ = -1;
    if (pendingCycle_ != 0) {
        // Q with nothing selected behaves as E: there is no "previous" to step
        // back to, and doing nothing would read as a dead key.
        const int step = (pendingCycle_ < 0 && selectedSecondary_ < 0) ? 1 : pendingCycle_;
        selectedSecondary_ = cycleSecondary(profile, selectedSecondary_, step);
        lastSelectTime_ = timeSeconds;
        pendingCycle_ = 0;
    }
    if (pendingSecondaryDelete_) {
        pendingSecondaryDelete_ = false;
        if (selectedSecondary_ >= 0) {
            const int slot = kLoadoutBarPrimary + selectedSecondary_;
            if (slot < owned) net.setLoadoutSlot(slot, kNoPetal, Rarity::Common);
            selectedSecondary_ = cycleSecondary(profile, selectedSecondary_, 1);
            lastSelectTime_ = timeSeconds;
        }
    }
    if (pendingSwapSlot_ >= 0) {
        const int primary = pendingSwapSlot_;
        pendingSwapSlot_ = -1;
        // With a secondary armed the number key swaps into THAT slot and
        // advances the selection; otherwise it swaps with the slot below.
        const int secondary =
            kLoadoutBarPrimary + (selectedSecondary_ >= 0 ? selectedSecondary_ : primary);
        if (primary < owned && secondary < owned) net.swapLoadoutSlots(primary, secondary);
        if (selectedSecondary_ >= 0) {
            selectedSecondary_ = cycleSecondary(profile, selectedSecondary_, 1);
            lastSelectTime_ = timeSeconds;
        }
    }

    const Vec2 mouse{window.mouseX(), window.mouseY()};
    int hovered = -1;
    for (int i = 0; i < kLoadoutBarSlots; ++i) {
        if (insideInclusive(layout.slots[static_cast<std::size_t>(i)], mouse)) hovered = i;
    }
    if (hovered < 0 && insideInclusive(layout.trash, mouse)) hovered = kLoadoutTrashSlot;
    loadoutHovered_ = hovered;
    // The browser intercepts a press on the bar only to begin a drag, so this
    // is exactly the condition under which the click is the bar's at all.
    loadoutGrabbable_ = hovered >= 0 && hovered < kLoadoutBarSlots && hovered < owned &&
                        !profile.loadout[static_cast<std::size_t>(hovered)].empty();

    canvas.save();
    canvas.translate(0.0f, static_cast<float>((1.0 - loadoutSlide_) * 120.0));

    drawLoadoutSlot(canvas, layout.trash, kLoadoutTrashFill, hovered == kLoadoutTrashSlot);
    drawKeyLabel(canvas, "[T]", layout.trash.right() + 16.0,
                 layout.trash.y + layout.trash.h * 0.5, Align::Left);
    if (drag_.active()) {
        TextStyle del;
        del.size = std::round(layout.trash.h / 4.0);
        del.bold = true;
        del.fill = kPaper;
        del.stroke = kInk;
        del.strokeWidth = 3.0;
        del.align = Align::Centre;
        del.baseline = Baseline::Middle;
        text(canvas, "Delete", layout.trash.x + layout.trash.w * 0.5,
             layout.trash.y + layout.trash.h * 0.5, del);
    }

    for (int i = 0; i < kLoadoutBarSlots; ++i) {
        const Rect slot = layout.slots[static_cast<std::size_t>(i)];
        const bool selected = i >= kLoadoutBarPrimary &&
                              i - kLoadoutBarPrimary == selectedSecondary_;
        drawLoadoutSlot(canvas, slot, kLoadoutSlotFill, hovered == i || selected);
        // The bracketed captions belong to the primary row only; the second
        // row is reached with Q/E, not with a key of its own.
        if (i < kLoadoutBarPrimary) {
            drawKeyLabel(canvas, kLoadoutKeyCaps[i], slot.x + slot.w * 0.5, slot.y - 15.0,
                         Align::Centre);
        }
    }

    if (selectedSecondary_ >= 0) {
        const Rect slot = layout.slots[static_cast<std::size_t>(kLoadoutBarPrimary +
                                                                selectedSecondary_)];
        const double cx = slot.x + slot.w * 0.5;
        const double cy = slot.y + slot.h * 0.5;
        canvas.save();
        canvas.translate(static_cast<float>(cx), static_cast<float>(cy));
        canvas.rotate(static_cast<float>(std::sin(timeSeconds * 1000.0 / 150.0) * 0.06));
        setStroke(canvas, kPaper);
        canvas.setLineWidth(4.0f);
        canvas.beginPath();
        canvas.roundRect(static_cast<float>(-slot.w * 0.5 - 6.0),
                         static_cast<float>(-slot.h * 0.5 - 6.0),
                         static_cast<float>(slot.w + 12.0), static_cast<float>(slot.h + 12.0),
                         static_cast<float>(slot.w / 20.0 + 2.0));
        canvas.stroke();
        canvas.restore();
    }

    for (int i = 0; i < kLoadoutBarSlots; ++i) {
        if (i >= owned) break;
        const auto at = static_cast<std::size_t>(i);
        if (profile.loadout[at].empty()) continue;
        // The slot a petal is being dragged out of renders as a plain empty
        // slot: only its icon is lifted, not the chrome.
        if (drag_.source == DragState::Source::LoadoutSlot && drag_.slot == i) continue;
        ItemTile tile;
        tile.petalIndex = profile.loadout[at].petalIndex;
        tile.rarity = profile.loadout[at].rarity;
        tile.timeSeconds = timeSeconds;
        tile.reload = slotReloadProgress(net, i);
        tile.health = slotHealthFraction(net, i);
        drawItemTile(canvas, sprites, layout.slots[at], tile);
    }
    canvas.restore();
}

void MenuSystem::updateLoadoutInput(Window& window, NetClient& net) {
    const Profile& profile = net.profile();
    const int owned = static_cast<int>(profile.loadout.size());
    const int hovered = loadoutHovered_;

    // Picking a petal up off the bar. Nothing is sent yet: a drag that ends
    // back where it started must not have unequipped anything on the way.
    if (window.mousePressed(MouseButton::Left) && hovered >= 0 && hovered < kLoadoutBarSlots &&
        !drag_.active()) {
        const auto at = static_cast<std::size_t>(hovered);
        if (hovered < owned && !profile.loadout[at].empty()) {
            drag_.source = DragState::Source::LoadoutSlot;
            drag_.petalIndex = profile.loadout[at].petalIndex;
            drag_.rarity = profile.loadout[at].rarity;
            drag_.slot = hovered;
        }
    }

    if (!window.mouseReleased(MouseButton::Left) || !drag_.active()) return;

    if (hovered >= 0 && hovered < kLoadoutBarSlots) {
        if (hovered < owned) {
            if (drag_.source == DragState::Source::Inventory) {
                net.setLoadoutSlot(hovered, drag_.petalIndex, drag_.rarity);
            } else if (drag_.slot != hovered) {
                net.swapLoadoutSlots(drag_.slot, hovered);
            }
        }
        drag_.clear();
        return;
    }

    // Anywhere that is not a slot sends the petal back to the inventory --
    // including the trash, and including a drop over an open panel.
    if (drag_.source == DragState::Source::LoadoutSlot && drag_.slot < owned) {
        net.setLoadoutSlot(drag_.slot, kNoPetal, Rarity::Common);
    }
    drag_.clear();
}

void MenuSystem::drawDragged(Canvas& canvas, Window& window, const SpriteCache& sprites,
                             double timeSeconds) {
    if (!drag_.active() || drag_.petalIndex == kNoPetal) return;
    if (!inGame_) {
        // The title screen's drag image is the sprite alone -- no plate, no
        // outline -- with the cursor at its centre.
        const PetalStats stats = content().petalStats(drag_.petalIndex, drag_.rarity);
        drawPetalCluster(canvas, sprites, drag_.petalIndex, stats.size, stats.count,
                         window.mouseX(), window.mouseY(), kDragImageSize, timeSeconds);
        return;
    }

    // In game the petal rides the cursor as the same tile it was picked up
    // from, just translucent: half a drag showing a different object is how
    // the player loses track of what they are holding.
    const Rect ghost{window.mouseX() - kDragGhostSize * 0.5,
                     window.mouseY() - kDragGhostSize * 0.5, kDragGhostSize, kDragGhostSize};
    ItemTile tile;
    tile.petalIndex = drag_.petalIndex;
    tile.rarity = drag_.rarity;
    tile.showName = false;
    tile.alpha = kDragGhostAlpha;
    tile.timeSeconds = timeSeconds;
    drawItemTile(canvas, sprites, ghost, tile);
}

// ---------------------------------------------------------------------------
// DebugPanel
// ---------------------------------------------------------------------------
//
// Lives here rather than in a file of its own: it is the one panel with no
// game content behind it, and it exists mainly so the strip's bug button has
// somewhere to go.

double DebugPanel::preferredWidth() { return 420.0; }

void DebugPanel::reset() {
    // Only the aggregation window is cleared. The series are the panel's whole
    // point -- reopening it to an empty graph would throw away the two minutes
    // of history it was collecting in the background for exactly this moment.
    sampleAge_ = 0;
    sampleTotal_ = 0;
    sampleCount_ = 0;
}

namespace {

void pushSample(std::vector<double>& series, double value, int limit) {
    series.push_back(value);
    if (static_cast<int>(series.size()) > limit) series.erase(series.begin());
}

double lastOr(const std::vector<double>& series, double fallback) {
    return series.empty() ? fallback : series.back();
}

std::string fixed(double value, int decimals) {
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    return buffer;
}

constexpr double kBytesPerMB = 1024.0 * 1024.0;

} // namespace

void DebugPanel::recordFrame(double dtSeconds, NetClient& net) {
    // Averaged over a whole second before it is kept: a per-frame trace is all
    // scheduler noise and says nothing about where the time went.
    sampleTotal_ += dtSeconds * 1000.0;
    ++sampleCount_;
    sampleAge_ += dtSeconds;
    if (sampleAge_ >= 1.0) {
        if (sampleCount_ > 0) {
            pushSample(clientFrameMillis_, sampleTotal_ / sampleCount_, kHistory);
        }
        // The browser reads performance.memory.usedJSHeapSize here, which is a
        // Chrome-only figure and empty everywhere else. There is no JS heap in
        // this build; the resident set is the number that means the same thing
        // about the same process, so the graph is live here rather than blank.
        const double resident = static_cast<double>(residentBytes());
        if (resident > 0) pushSample(clientMemoryMB_, resident / kBytesPerMB, kHistory);
        sampleAge_ = 0;
        sampleTotal_ = 0;
        sampleCount_ = 0;
    }

    // One graph sample per packet, not per frame: the server sends these once
    // a second, and resampling the last one between packets would draw a
    // plateau that says the server was steady when nothing was measured.
    NetClient::ServerDebugStats stats;
    if (net.takeServerDebugStats(stats)) {
        haveServerStats_ = true;
        pushSample(serverHeapMB_, stats.heapBytes / kBytesPerMB, kHistory);
        pushSample(serverResidentMB_, stats.residentBytes / kBytesPerMB, kHistory);
        pushSample(serverTickAvgMillis_, stats.tickAvgMillis, kHistory);
        pushSample(serverTickMaxMillis_, stats.tickMaxMillis, kHistory);
    }
}

void DebugPanel::drawGraph(Canvas& canvas, Rect plot, const std::string& label,
                           const std::string& value, const std::vector<Series>& lines,
                           const char* unit) {
    constexpr double kLabelHeight = 18.0;
    const double labelY = plot.y - kLabelHeight * 0.5;

    TextStyle caption;
    caption.size = 13.0;
    caption.bold = true;
    caption.strokeWidth = 2.0;
    caption.baseline = Baseline::Middle;
    text(canvas, label, plot.x, labelY, caption);

    TextStyle reading;
    reading.size = 12.0;
    reading.bold = true;
    reading.strokeWidth = 2.0;
    reading.align = Align::Right;
    reading.baseline = Baseline::Middle;
    // The last series' colour, so a two-line graph's headline number says
    // which line it belongs to without a legend.
    reading.fill = lines.empty() ? kPaper : lines.back().colour;
    text(canvas, value, plot.right(), labelY, reading);

    setFill(canvas, kInk, 0.3);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(plot.x), static_cast<float>(plot.y),
                     static_cast<float>(plot.w), static_cast<float>(plot.h), 3.0f);
    canvas.fill();

    double peak = 0;
    for (const Series& line : lines) {
        for (const double sample : *line.values) peak = std::max(peak, sample);
    }
    if (peak <= 0) {
        TextStyle empty;
        empty.size = 11.0;
        empty.strokeWidth = 0;
        empty.fill = 0x888888u;
        empty.align = Align::Centre;
        empty.baseline = Baseline::Middle;
        text(canvas, "no data yet", plot.x + plot.w * 0.5, plot.y + plot.h * 0.5, empty);
        return;
    }
    // Padded, so the tallest sample does not kiss the top edge and read as
    // clipped.
    const double scaleMax = peak * 1.15;

    const double midY = plot.y + plot.h * 0.5;
    setStroke(canvas, kPaper, 0.15);
    canvas.setLineWidth(1.0f);
    canvas.beginPath();
    canvas.moveTo(static_cast<float>(plot.x + 2.0), static_cast<float>(midY));
    canvas.lineTo(static_cast<float>(plot.right() - 2.0), static_cast<float>(midY));
    canvas.stroke();

    TextStyle axis;
    axis.size = 9.0;
    axis.strokeWidth = 0;
    axis.fill = kPaper;
    axis.baseline = Baseline::Middle;
    canvas.save();
    canvas.setGlobalAlpha(0.5f);
    text(canvas, fixed(scaleMax * 0.5, 1) + " " + unit, plot.x + 4.0, midY - 5.0, axis);
    canvas.restore();

    canvas.save();
    canvas.beginPath();
    canvas.rect(static_cast<float>(plot.x), static_cast<float>(plot.y),
                static_cast<float>(plot.w), static_cast<float>(plot.h));
    canvas.clip();
    const double step = plot.w / static_cast<double>(kHistory - 1);
    for (const Series& line : lines) {
        const std::vector<double>& values = *line.values;
        if (values.size() < 2) continue;
        setStroke(canvas, line.colour);
        canvas.setLineWidth(1.5f);
        canvas.beginPath();
        for (std::size_t i = 0; i < values.size(); ++i) {
            // Anchored to the RIGHT edge: the newest sample is always against
            // the wall and the history scrolls off the left, so a glance at
            // the same place always shows now.
            const double px =
                plot.right() - static_cast<double>(values.size() - 1 - i) * step;
            const double py = plot.bottom() - 2.0 - (values[i] / scaleMax) * (plot.h - 4.0);
            if (i == 0) canvas.moveTo(static_cast<float>(px), static_cast<float>(py));
            else canvas.lineTo(static_cast<float>(px), static_cast<float>(py));
        }
        canvas.stroke();
    }
    canvas.restore();
}

bool DebugPanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Rect panel = ctx.bounds;
    const Vec2 mouse = ctx.mouse();
    constexpr double kPad = 15.0;
    constexpr double kHeader = 30.0;

    overlayCard(canvas, panel, kDebugSkin);

    TextStyle heading;
    heading.size = 20.0;
    heading.bold = true;
    heading.strokeWidth = 3.0;
    text(canvas, "Debug", panel.x + kPad, panel.y + kPad + kHeader * 0.5, heading);

    const Rect closeRect = closeButtonRect(panel);
    panelClose(canvas, closeRect, closeRect.contains(mouse));

    constexpr double kGraphHeight = 74.0;
    constexpr double kLabelHeight = 18.0;
    constexpr double kGap = 12.0;
    const double left = panel.x + kPad;
    const double width = panel.w - kPad * 2;
    double cy = panel.y + kHeader + kPad + 5.0;
    const auto block = [&](const std::string& label, const std::string& value,
                           const std::vector<Series>& lines, const char* unit) {
        drawGraph(canvas, Rect{left, cy + kLabelHeight, width, kGraphHeight}, label, value, lines,
                  unit);
        cy += kLabelHeight + kGraphHeight + kGap;
    };

    const double frameMillis = lastOr(clientFrameMillis_, 0.0);
    block("Client Frame Time",
          clientFrameMillis_.empty()
              ? "collecting\xE2\x80\xA6"
              : fixed(frameMillis, 1) + " ms (" +
                    std::to_string(static_cast<long>(std::lround(1000.0 /
                                                                 std::max(frameMillis, 0.01)))) +
                    " FPS)",
          {{&clientFrameMillis_, 0x5A9FDBu}}, "ms");

    block("Client Memory (resident)",
          clientMemoryMB_.empty() ? "unavailable on this platform"
                                  : fixed(lastOr(clientMemoryMB_, 0.0), 1) + " MB",
          {{&clientMemoryMB_, 0x7FDB7Fu}}, "MB");

    // The two server graphs say what is missing rather than drawing zeroes:
    // until a DebugStats packet arrives there is no server to report on, which
    // on the title screen is the truth.
    const std::string noServer = "no data \xE2\x80\x94 join a game";
    block("Server Tick Time",
          haveServerStats_ ? "avg " + fixed(lastOr(serverTickAvgMillis_, 0.0), 1) + " / max " +
                                 fixed(lastOr(serverTickMaxMillis_, 0.0), 1) + " ms"
                           : noServer,
          {{&serverTickMaxMillis_, 0xE07070u}, {&serverTickAvgMillis_, 0xFFDD66u}}, "ms");

    block("Server Memory",
          haveServerStats_ ? "heap " + fixed(lastOr(serverHeapMB_, 0.0), 1) + " / rss " +
                                 fixed(lastOr(serverResidentMB_, 0.0), 1) + " MB"
                           : noServer,
          {{&serverResidentMB_, 0xC9A0E8u}, {&serverHeapMB_, 0xE8A023u}}, "MB");

    return !ctx.clicked(closeRect);
}

namespace {

/// The tall list panels are DOM shells that rise from `translateY(100vh)` over
/// 300ms; the corner overlays are canvas panels drawn straight at the corner
/// anchor with no transition at all, so only these three animate.
bool slidesUp(MenuId id) {
    return id == MenuId::Inventory || id == MenuId::Crafting || id == MenuId::Talents;
}

constexpr double kPanelSlideSeconds = 0.30;

} // namespace

MenuSystem::PanelLayer MenuSystem::panelLayer(MenuId id, bool inGame) {
    // On the title screen the two screens' odd pair still applies: settings and
    // the debug panel are painted inside renderCanvasUI, before
    // drawTitleLoadout and the strip, while every other card comes after both.
    //
    // In game every card is Over, which is a deliberate departure from the
    // reference. There graphics.render() paints the cards, then the strip, and
    // Game paints the loadout bar afterwards -- so the hotbar cut across the
    // bottom of an open inventory or crafting card. The bar belongs behind the
    // menus, so the cards are painted last on this screen whatever they are.
    if (inGame) return PanelLayer::Over;
    const bool oddPair = id == MenuId::Settings || id == MenuId::Debug;
    return oddPair ? PanelLayer::Under : PanelLayer::Over;
}

void MenuSystem::renderOpenPanel(Canvas& canvas, Window& window, NetClient& net,
                                 const SpriteCache& sprites, const WorldRenderer& renderer,
                                 double timeSeconds, double dt) {
    if (drawn_ == MenuId::None) return;
    MenuContext ctx{canvas,    window,      net, sprites,    renderer, settings_,
                    drag_,     timeSeconds, dt,  panelRect_, false};
    bool keepOpen = true;
    // The setting is the single source of truth for the debug panel: unchecking
    // "Enable Debug Menu" while it is open closes it on the next frame rather
    // than leaving a panel up with no way back to it, which is what the
    // reference's own render() does first thing.
    if (drawn_ == MenuId::Debug && !settings_.showDebugButton) {
        close();
        return;
    }
    // Everything a panel paints is selectable text unless the widget painting
    // it says otherwise -- the buttons, chips, tiles, fields and tooltips all
    // turn this back off around their own labels.
    ui::TextCaptureScope capture(true);
    switch (drawn_) {
        case MenuId::Inventory:   keepOpen = inventory_.render(ctx); break;
        case MenuId::Crafting:    keepOpen = crafting_.render(ctx); break;
        case MenuId::Talents:     keepOpen = talents_.render(ctx); break;
        case MenuId::Gallery:     keepOpen = gallery_.render(ctx); break;
        case MenuId::Shop:        keepOpen = shop_.render(ctx); break;
        case MenuId::Skins:       keepOpen = skins_.render(ctx); break;
        case MenuId::Leaderboard: keepOpen = leaderboard_.render(ctx); break;
        case MenuId::Settings:    keepOpen = settings_panel_.render(ctx); break;
        case MenuId::Changelog:   keepOpen = changelog_.render(ctx); break;
        case MenuId::Notifications: keepOpen = notifications_.render(ctx); break;
        case MenuId::Guild:       keepOpen = guild_.render(ctx); break;
        case MenuId::Debug:       keepOpen = debug_.render(ctx); break;
        default: break;
    }
    // Latched before the closing-card check below: the Log Out button closes
    // its own panel on the same click, so the request would never survive to
    // be read if it were picked up after that early return.
    if (ctx.logoutRequested) logoutRequested_ = true;

    // A card on its way out still paints, but it has no say any more: it is
    // already closed, and a search field it happened to hold would go on
    // eating the chat box's keystrokes all the way down.
    if (open_ == MenuId::None) return;
    wantsText_ = ctx.wantsText;
    if (!keepOpen) close();
}

void MenuSystem::render(Canvas& canvas, Window& window, NetClient& net, const SpriteCache& sprites,
                        const WorldRenderer& renderer, double timeSeconds, double dt,
                        const OverlayFn& overStripUnderBar) {
    wantsText_ = false;
    panelRect_ = Rect{};

    // A guild invitation raises the guild panel over whatever was open. It is
    // the one thing in this build that opens a menu without a click, so the
    // flag is consumed here -- once per invitation, not once per frame it is
    // still unanswered.
    if (net.guildInvite().justArrived) {
        net.guildInvite().justArrived = false;
        if (open_ != MenuId::Guild) toggle(MenuId::Guild);
    }

    // What is painted is `drawn_`, which outlives `open_` for as long as the
    // card takes to slide back down.
    if (open_ != MenuId::None) {
        drawn_ = open_;
        panelSlide_ = slidesUp(open_) ? std::min(1.0, panelSlide_ + dt / kPanelSlideSeconds) : 1.0;
    } else if (drawn_ != MenuId::None) {
        panelSlide_ = slidesUp(drawn_) ? std::max(0.0, panelSlide_ - dt / kPanelSlideSeconds) : 0.0;
        if (panelSlide_ <= 0.0) drawn_ = MenuId::None;
    }

    if (drawn_ != MenuId::None) {
        panelRect_ = panelBounds(drawn_, canvas.width(), canvas.height());
        // `transform: translateY(100vh)` -> `translateY(0)` over 300ms
        // `ease-out`, which is cubic-bezier(0, 0, 0.58, 1). 1-(1-t)^1.7 tracks
        // that curve to within a percent the whole way along, where the cubic
        // the strip's slide-in uses would be a fifth too far ahead at the
        // halfway point.
        const double eased = 1.0 - std::pow(1.0 - panelSlide_, 1.7);
        panelRect_.y += (1.0 - eased) * canvas.height();
    }
    const PanelLayer layer = panelLayer(drawn_, inGame_);

    if (layer == PanelLayer::Under) {
        renderOpenPanel(canvas, window, net, sprites, renderer, timeSeconds, dt);
    }
    // The bar and the strip trade places between the screens. The title screen
    // runs drawTitleLoadout and then canvasButtons.draw; in game the strip is
    // the last thing graphics.render() paints and the bar is the first thing
    // Game paints after it, which keeps the hotbar over the death scrim -- the
    // one thing that ever falls in between the two. The open card is no longer
    // in that sandwich: in game it is always painted after the bar, so the
    // hotbar sits behind the menus instead of cutting across them.
    if (inGame_) {
        drawIconStrip(canvas, window, timeSeconds);
        if (overStripUnderBar) overStripUnderBar();
        drawLoadoutBar(canvas, window, net, sprites, timeSeconds);
    } else {
        drawLoadoutBar(canvas, window, net, sprites, timeSeconds);
        drawIconStrip(canvas, window, timeSeconds);
    }
    if (layer == PanelLayer::Over) {
        renderOpenPanel(canvas, window, net, sprites, renderer, timeSeconds, dt);
    }

    // Input last, and after the panel has had the same click whichever layer it
    // was painted on: a press the card is standing on stays the card's, and a
    // drop into the card must reach the card before the bar decides it landed
    // on nothing.
    updateLoadoutInput(window, net);
    drawDragged(canvas, window, sprites, timeSeconds);
}

void MenuSystem::renderStripOnly(Canvas& canvas, Window& window, double timeSeconds) {
    // The login screen has the strip and nothing else. Whatever the last
    // screen left in the panel and bar state is cleared rather than carried
    // over, or a card nobody is painting would still be swallowing clicks.
    wantsText_ = false;
    panelRect_ = Rect{};
    loadoutHovered_ = -1;
    loadoutGrabbable_ = false;
    drawIconStrip(canvas, window, timeSeconds);
}

double MenuSystem::reservedTop() const {
    double right = 0;
    for (int i = 0; i < kStripSlotCount; ++i) {
        const auto at = static_cast<std::size_t>(i);
        if (strip()[at].topRow && stripRects_[at].w > 0) {
            right = std::max(right, stripRects_[at].right());
        }
    }
    return right > 0 ? right + kTopIcon.gap : 0.0;
}

double MenuSystem::reservedLeft() const {
    double right = 0;
    for (int i = 0; i < kStripSlotCount; ++i) {
        const auto at = static_cast<std::size_t>(i);
        if (!strip()[at].topRow && stripRects_[at].w > 0) {
            right = std::max(right, stripRects_[at].right());
        }
    }
    return right > 0 ? right + kColumnIcon.gap : 0.0;
}

double MenuSystem::stripBottom() const {
    return kTopIcon.inset + kTopIcon.side + kTopIcon.gap;
}

bool MenuSystem::capturesMouse(Vec2 mouse) const {
    if (panelRect_.w > 0 && panelRect_.contains(mouse)) return true;
    // The bar takes the click for one thing only: lifting a petal out of a
    // filled slot. An empty slot, the trash, the gaps between the rows and the
    // caption strip above them all fall through and fire an attack, so no
    // bounding box around the bar answers here. Read off the hover scan rather
    // than `mouse`, because only that pass has the account's loadout to say
    // whether the slot holds anything.
    if (loadoutGrabbable_) return true;
    for (const Rect& button : stripRects_) {
        if (insideInclusive(button, mouse)) return true;
    }
    return false;
}

} // namespace flix
