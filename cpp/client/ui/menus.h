#pragma once
// The game's menus: inventory, crafting, talents, bestiary, shop, skins,
// leaderboard and settings.
//
// One menu is open at a time. That is a deliberate rule rather than a
// limitation: every panel is anchored to the same place, they overlap, and a
// stack of them would leave the player dragging petals into a panel they
// cannot see. Opening one closes whatever was open, exactly as pressing its
// key again closes it.
//
// The panels are immediate-mode. Each one lays itself out, hit-tests, acts and
// draws in a single pass, so there is no retained widget tree to leave stale
// and no second copy of the layout for the input pass to drift away from. What
// they DO keep between frames is genuinely stateful: a scroll offset, what is
// being dragged, how far an animation has run.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <memory>

#include "canvas.h"
#include "svg.h"
#include "window.h"

#include "client/net_client.h"
#include "client/render/sprites.h"
#include "client/interpolation.h"
#include "client/render/world_renderer.h"
#include "client/ui/menu_widgets.h"
#include "shared/core/types.h"
#include "shared/game/skills.h"

namespace flix {

/// Opens an external HTTP(S) URL with the host platform. The web build uses
/// window.open directly, keeping Emscripten's SDL shim out of the browser
/// artifact; the desktop build delegates to SDL.
bool openExternalLink(const std::string& url);

/// Which panel is on screen.
///
/// Appended to, never reordered: `ClientSettings::hotkeys` is indexed by this
/// and persists as `key.<index>`, so an insertion in the middle silently
/// rebinds every menu a player has customised.
enum class MenuId : std::uint8_t {
    None = 0,
    Inventory,
    Crafting,
    Talents,
    Gallery,
    Shop,
    Skins,
    Leaderboard,
    Settings,
    Changelog,
    Notifications,
    Guild,
    Debug,
    Count,
};

inline constexpr int kMenuCount = static_cast<int>(MenuId::Count);

/// A rebindable action, in the order the settings panel lists its rows -- the
/// browser's DEFAULT_CONTROLS order.
///
/// Appended to, never reordered: a binding persists as `ctl.<index>`, so an
/// insertion in the middle silently rebinds every action after it.
enum class ControlAction : std::uint8_t {
    MoveUp = 0,
    MoveDown,
    MoveLeft,
    MoveRight,
    Inventory,
    Crafting,
    Skills,
    ToggleMouseControls,
    ToggleHitboxes,
    ToggleDebugMenu,
    ZoomIn,
    ZoomOut,
    Chat,
    ExtendPetals,
    RetractPetals,
    Count,
};

inline constexpr int kControlCount = static_cast<int>(ControlAction::Count);

/// What the settings panel shows for one action, and where its binding lives.
/// `menu` is the panel a row really opens; MenuId::None means there is no menu
/// behind it and the key is kept in ClientSettings::controls.
struct ControlMeta {
    const char* label;
    Key defaultKey;
    MenuId menu;
};

/// The label, default key and menu of an action. Indexed by ControlAction; an
/// action out of range reports an unbound, unlabelled row rather than reading
/// off the end.
const ControlMeta& controlMeta(ControlAction action);

/// keyDown/keyPressed for a BOUND key, which is not quite the same question.
/// The browser spells both shifts, both controls and both alts as one key and
/// binds that; a client that names physical keys has to answer for either half
/// of the pair, or a binding made on one side of the keyboard is dead on the
/// other. An unbound action (Key::Unknown) is never down and never pressed.
bool boundKeyDown(const Window& window, Key key);
bool boundKeyPressed(const Window& window, Key key);

/// The loadout the bar draws: ten primary slots, ten secondary slots under
/// them, and a trash slot at the end of the second row.
///
/// Layout, not capacity: `kLoadoutSlots` is what an account holds and the wire
/// carries, and `kLoadoutActiveSlots` is how many of those are in orbit. A slot
/// past what the account actually holds still draws as an empty one.
inline constexpr int kLoadoutBarPrimary = 10;
inline constexpr int kLoadoutBarSecondary = 10;
inline constexpr int kLoadoutBarSlots = kLoadoutBarPrimary + kLoadoutBarSecondary;
/// Hit-test index of the trash, one past the last real slot.
inline constexpr int kLoadoutTrashSlot = kLoadoutBarSlots;

/// The link the Discord button opens.
inline constexpr const char* kDiscordInvite = "https://discord.gg/e23DMCR7DV";

/// Slots in the icon strip: ten across the top-left corner, four down the
/// bottom-left one. Two of the top ten open no panel -- Discord is a link and
/// exit leaves the game -- so this is not `kMenuCount`.
inline constexpr int kStripSlotCount = 14;

/// The range ClientSettings::zoom is held to, and how far one press of a zoom
/// key moves it. The step is the browser's own ZOOM_STEP; the range is this
/// client's, and is the one the wheel has always scrolled through.
inline constexpr double kMinZoom = 0.6;
inline constexpr double kMaxZoom = 1.6;
inline constexpr double kZoomKeyStep = 0.1;

/// Everything the settings menu owns. Kept in one struct so it can be written
/// to disk and read back as a unit, and so nothing else has to know which of
/// these the renderer reads and which the input layer does.
struct ClientSettings {
    WorldRenderer::Options render;
    /// Player-chosen camera zoom, multiplied into whatever the loadout asks
    /// for. Persisted, because it is a comfort setting, not a game state.
    double zoom = 1.0;
    /// How much of the gap between a drawn position and the authoritative one
    /// is closed per frame at 60 fps -- the smoothness/latency trade for every
    /// flower, petal, drop and mob facing. The browser build keeps the same
    /// number in `localStorage.interpolationAmount`. See client/interpolation.h.
    double interpolation = kDefaultInterpolationAmount;
    /// The fraction of the display's native resolution the frame is
    /// rasterised at, 0.25 to 1. The browser build keeps the same number in
    /// `localStorage.renderScale`; here it reaches Window::setRenderScale,
    /// which is the whole of its effect -- nothing moves or resizes, the
    /// picture just gets softer and the rasteriser gets cheaper. It earns its
    /// place on a HiDPI display, where 1 means filling four times the pixels
    /// a non-Retina panel would ask for.
    double renderScale = 1.0;
    bool showChat = true;
    bool showMenuBar = true;
    /// The frame/ping/position readout in the bottom-right corner. Off by
    /// default, and the browser build keeps the same flag in
    /// `localStorage.showStats`.
    bool showStats = false;
    /// Shows the grey bug button in the top strip, which is the only way into
    /// the debug panel. Off by default, exactly as `debugMenuEnabled` is.
    bool showDebugButton = false;
    /// How many changelog releases the player had already seen the last time
    /// they opened the panel. The browser keeps the same number in
    /// `localStorage.lastSeenChangelogCount`, and shakes the strip's changelog
    /// button for as long as the changelog holds more entries than this.
    int changelogSeen = 0;
    /// Which notifications the player has already read. The browser keeps the
    /// same set in `localStorage['game_notifications_read']`; there is no such
    /// store here, so it rides in the settings file. Server ids carry no
    /// whitespace, which is what lets them share this file's key/value lines.
    std::vector<std::string> readNotifications;
    /// The key that opens each menu, indexed by MenuId.
    std::array<Key, kMenuCount> hotkeys{};
    /// The key bound to every other action, indexed by ControlAction. The four
    /// menu-backed actions live in `hotkeys` instead, because that is what
    /// MenuSystem opens a panel from, and their slots here are never read:
    /// controlKey/bindControl are what hide which action is kept where.
    std::array<Key, kControlCount> controls{};
    /// Whether the flower follows the cursor. Off leaves movement to the four
    /// movement keys alone; aim follows the pointer either way. The browser
    /// keeps the same flag in `localStorage.useMouseControls` and defaults it
    /// off, where this client defaults it ON: cursor-following is the control
    /// scheme it has always shipped with, and defaulting to the browser's
    /// value would take it away from every existing player.
    bool useMouseControls = true;
    /// The biome the player last chose to start in. Empty is the beginner
    /// ground. Remembered because it is a preference, not a game state.
    std::string spawnBiome;
    /// Whether the eleven-step tutorial has been finished or skipped, and how
    /// far it had got. The browser keeps the same pair in localStorage as
    /// `tutorial_completed` and `tutorial_step`; they ride here so this client
    /// has one settings file rather than a second store beside it.
    ///
    /// Only the first is ever read back. See ui::Tutorial::beginGame for why
    /// the reference's own resume is dead and this one matches it.
    bool tutorialCompleted = false;
    int tutorialStep = 0;

    ClientSettings();

    /// The key bound to an action, wherever that binding is kept.
    Key controlKey(ControlAction action) const;
    /// Binds a key to an action. A key already opening some OTHER menu is
    /// taken from it rather than the rebind being refused: two menus on one
    /// key is the only broken state, and telling the player off for it is
    /// worse than just moving it.
    void bindControl(ControlAction action, Key key);

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

/// What the player is currently dragging, if anything.
///
/// Owned by the menu system rather than by the inventory panel: a drag starts
/// in one panel and ends on the loadout bar, which is not a panel at all, and
/// the two must not each keep half of it.
struct DragState {
    enum class Source : std::uint8_t { None, Inventory, LoadoutSlot };
    Source source = Source::None;
    std::uint16_t petalIndex = kNoPetal;
    Rarity rarity = Rarity::Common;
    int slot = -1;              ///< the loadout slot a LoadoutSlot drag left
    bool active() const { return source != Source::None; }
    void clear() { *this = DragState{}; }
};

/// One frame of everything a panel is allowed to touch.
struct MenuContext {
    Canvas& canvas;
    Window& window;
    NetClient& net;
    const SpriteCache& sprites;
    /// Used only by the skins menu, to preview a cosmetic with the very same
    /// body the world renders.
    const WorldRenderer& renderer;
    ClientSettings& settings;
    DragState& drag;
    double timeSeconds = 0;
    double dt = 0;
    /// The panel's card, already anchored -- and already offset downwards by
    /// however much of its opening slide is still to run, so everything a
    /// panel derives from this rides along with it.
    Rect bounds;
    /// Set by a panel whose text field has focus, so the chat box and the
    /// login form do not also consume this frame's keystrokes.
    bool wantsText = false;
    /// Set by the settings panel's Log Out button. Ending a session is the
    /// app's business -- it owns the stored token and the screen -- so the
    /// panel only says that it was asked for.
    bool logoutRequested = false;

    Vec2 mouse() const { return {window.mouseX(), window.mouseY()}; }
    bool over(Rect r) const { return r.contains(mouse()); }
    bool pressed() const { return window.mousePressed(MouseButton::Left); }
    bool released() const { return window.mouseReleased(MouseButton::Left); }
    /// A click that both began and ended inside `r`. Press-and-release is what
    /// separates a click from the end of a drag that happens to land here.
    bool clicked(Rect r) const { return released() && over(r); }
    float wheel() const { return window.wheelDelta(); }
};

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

/// The petal inventory: every stack the account owns, grouped by tier.
class InventoryPanel {
public:
    /// Returns false when the panel asked to be closed.
    bool render(MenuContext&);
    void reset();

    /// The panel's natural width: five cells and their gaps, plus padding.
    static double preferredWidth();
    /// Where the card hangs in a view of this size. Defined in menus.cpp with
    /// the rest of the panel geometry, so the anchors can be read side by side.
    static Rect bounds(int viewWidth, int viewHeight);

private:
    ui::Scroller scroll_;
    /// One slot per item TYPE at its best tier, instead of one per tier.
    bool stacked_ = false;
    double stackLerp_ = 0;
    std::string search_;
    ui::TextFieldState searchField_;
};

/// The forge: five slots in a ring, and the grid that feeds them.
class CraftingPanel {
public:
    bool render(MenuContext&);
    void reset();
    static double preferredWidth();
    static Rect bounds(int viewWidth, int viewHeight);

private:
    enum class Phase : std::uint8_t { Idle, Spinning, Result };

    /// Adds up to one more batch of this petal to the staging area.
    void stage(const Profile&, std::uint16_t petalIndex, Rarity rarity, bool wholeStack);

    ui::Scroller scroll_;
    /// What is staged. A craft consumes five at a time; `batches` is how many
    /// fives are queued, which is what the slot badge counts.
    std::uint16_t stagedPetal_ = kNoPetal;
    Rarity stagedRarity_ = Rarity::Common;
    int batches_ = 0;

    Phase phase_ = Phase::Idle;
    double phaseStarted_ = 0;
    double spinAngle_ = 0;
    /// What the ring is animating. The whole staging area goes to the server
    /// on the click, so `staged*` is empty for the whole spin and the ring
    /// needs its own copy of what was sent -- which is also what the result
    /// card is compared against.
    std::uint16_t spinPetal_ = kNoPetal;
    Rarity spinRarity_ = Rarity::Common;
    bool lastSuccess_ = false;
    std::uint16_t resultPetal_ = kNoPetal;
    Rarity resultRarity_ = Rarity::Common;
    /// How many upgrades the pool produced, for the result caption.
    int resultCount_ = 0;
    /// How many of the five survived a failure, for the slots to keep drawing.
    int survivors_ = 0;
};

/// The bestiary: every mob at every tier it can appear at, and what the
/// account has actually killed.
class GalleryPanel {
public:
    bool render(MenuContext&);
    void reset();
    static double preferredWidth();
    static double preferredHeight();
    static Rect bounds(int viewWidth, int viewHeight);

private:
    ui::Scroller scroll_;
};

/// The talent tree.
class TalentsPanel {
public:
    bool render(MenuContext&);
    void reset();
    static double preferredWidth();
    static Rect bounds(int viewWidth, int viewHeight);

private:
    struct Node {
        SkillId skill = SkillId::Damage;
        int tier = 0;
        /// Position in the tree's own space, before rotation.
        Vec2 local;
        Vec2 screen;
    };

    void layout();

    std::vector<Node> nodes_;
    bool laidOut_ = false;
    /// How far the card has slid up into place, 0 to 1. The panel opens by
    /// animating this rather than by appearing where it belongs.
    double openLerp_ = 0;
    /// Dragging anywhere in the card spins the whole fan about the flower.
    double rotation_ = 0;
    bool dragging_ = false;
    /// Where the press landed, and which node it landed on. Both outlive the
    /// press because a press is still a click until it travels far enough to
    /// become a spin, and the click belongs to the node it started on.
    Vec2 dragPress_;
    int pressedNode_ = -1;
    /// Latched the moment the press clears the drag threshold: a gesture that
    /// has become a spin stays one, even if the cursor comes back to where it
    /// started.
    bool dragMoved_ = false;
    double rotationAtAnchor_ = 0;
    /// Guards the reset button behind a second click.
    bool confirmingReset_ = false;
};

/// The star shop: the store's ten offers, the challenges that pay the stars to
/// buy them with, and the code redeemer. An overlay under the top icon row,
/// not one of the tall lists -- its grid is a fixed two rows of five and has
/// nothing to gain from a two-thirds-of-the-viewport card.
class ShopPanel {
public:
    bool render(MenuContext&);
    void reset();
    static double preferredWidth();
    /// Fixed, like every other overlay's: the header, the tab row and two rows
    /// of offer cards come to this and no window makes them taller.
    static double preferredHeight();
    static Rect bounds(int viewWidth, int viewHeight);

private:
    enum class Tab : std::uint8_t { Offers, Challenges, Bonus };
    Tab tab_ = Tab::Offers;
    ui::Scroller scroll_;
};

/// Cosmetic flower skins.
class SkinsPanel {
public:
    bool render(MenuContext&);
    void reset();
    static double preferredWidth();
    /// Taller than the other corner overlays: this is the only one that has to
    /// show a drawing board, a shape list and that shape's properties at once.
    static double preferredHeight();
    static Rect bounds(int viewWidth, int viewHeight);
};

/// Account rankings, straight from the server.
class LeaderboardPanel {
public:
    bool render(MenuContext&);
    void reset();
    static double preferredWidth();
    static Rect bounds(int viewWidth, int viewHeight);

private:
    ui::Scroller scroll_;
    bool requested_ = false;
};

/// Display switches, camera zoom and the menu keys.
class SettingsPanel {
public:
    bool render(MenuContext&);
    void reset();
    static double preferredWidth();
    static Rect bounds(int viewWidth, int viewHeight);

    /// True while waiting for a key to bind. The app must not treat that
    /// keystroke as a hotkey.
    bool capturingKey() const { return rebinding_ >= 0; }

private:
    ui::Scroller scroll_;
    int rebinding_ = -1;
};

/// The release notes, newest first. Pinned under the top icon row rather than
/// beside the bottom column: it is an overlay, not one of the tall lists.
class ChangelogPanel {
public:
    bool render(MenuContext&);
    void reset();
    static double preferredWidth();
    static Rect bounds(int viewWidth, int viewHeight);

private:
    ui::Scroller scroll_;
};

/// Server notices, invites and rewards.
class NotificationsPanel {
public:
    bool render(MenuContext&);
    void reset();
    static double preferredWidth();
    static Rect bounds(int viewWidth, int viewHeight);

private:
    ui::Scroller scroll_;
};

/// The player's guild: its roster, and the join/create form when they have none.
class GuildPanel {
public:
    bool render(MenuContext&);
    void reset();
    static double preferredWidth();
    static Rect bounds(int viewWidth, int viewHeight);

private:
    ui::Scroller scroll_;
};

/// Frame time and memory graphs, client and server. Reachable only while the
/// settings switch that puts the bug button in the strip is on.
class DebugPanel {
public:
    bool render(MenuContext&);
    void reset();
    static double preferredWidth();
    static Rect bounds(int viewWidth, int viewHeight);

    /// One frame of client samples, and any server packet that has arrived.
    ///
    /// Called every frame whether or not the panel is open, which is the
    /// browser's own arrangement: recordClientFrame runs from the render loop
    /// and the debugStats handler from the socket, so the graphs already hold
    /// two minutes of history the moment the panel is opened.
    void recordFrame(double dtSeconds, NetClient&);

private:
    /// One sample per second, so a graph holds about two minutes -- the same
    /// window the browser panel keeps.
    static constexpr int kHistory = 120;

    std::vector<double> clientFrameMillis_;
    std::vector<double> clientMemoryMB_;
    std::vector<double> serverTickAvgMillis_;
    std::vector<double> serverTickMaxMillis_;
    std::vector<double> serverHeapMB_;
    std::vector<double> serverResidentMB_;
    /// Until the first packet the server graphs say so rather than drawing a
    /// flat zero, which would read as a server that costs nothing.
    bool haveServerStats_ = false;

    double sampleAge_ = 0;
    double sampleTotal_ = 0;
    int sampleCount_ = 0;

    /// One series on one graph.
    struct Series {
        const std::vector<double>* values;
        std::uint32_t colour;
    };
    /// One labelled block: caption left, value right, plot below. `lines` are
    /// drawn on a shared auto-scaled axis with the newest sample against the
    /// right edge, and the LAST of them tints the value text -- which is what
    /// lets a two-line graph read without a legend.
    static void drawGraph(Canvas&, Rect plot, const std::string& label,
                          const std::string& value, const std::vector<Series>& lines,
                          const char* unit);
};

// ---------------------------------------------------------------------------
// The system
// ---------------------------------------------------------------------------

class MenuSystem {
public:
    /// Handles the menu hotkeys, Escape among them. Returns true when the key
    /// was consumed, so the game does not also act on it.
    bool handleKeys(Window&);

    /// True while the client is in a game, which is the only time the exit
    /// button is offered.
    ///
    /// The screen owns the loadout bar's slide: the browser builds a fresh
    /// `CanvasLoadoutBar` per screen, so crossing between them replays the
    /// rise rather than leaving the bar already seated from last time.
    void setInGame(bool inGame) {
        if (inGame == inGame_) return;
        inGame_ = inGame;
        loadoutSlide_ = 0.0;
    }
    /// Set when the exit button was clicked. The app reads and clears it --
    /// leaving a game is the app's business, not a menu's.
    bool takeExitRequest() {
        const bool requested = exitRequested_;
        exitRequested_ = false;
        return requested;
    }

    /// Set when Settings' Log Out was clicked, read and cleared by the app the
    /// same way the exit request is.
    bool takeLogoutRequest() {
        const bool requested = logoutRequested_;
        logoutRequested_ = false;
        return requested;
    }

    /// Anything the caller needs painted between the icon strip and the
    /// loadout bar. In game that gap is the death scrim's: it dims the strip
    /// and the HUD but leaves the bar at full brightness, and there is no
    /// other seam in this call it could be dropped into.
    using OverlayFn = std::function<void()>;

    /// One frame of every menu: the open card, the loadout bar, the icon strip
    /// and the dragged petal. The four of them do not have one fixed order --
    /// see `PanelLayer` for which card is painted where, and note that the bar
    /// and the strip themselves trade places between the two screens.
    void render(Canvas&, Window&, NetClient&, const SpriteCache&, const WorldRenderer&,
                double timeSeconds, double dt, const OverlayFn& overStripUnderBar = {});

    /// The icon strip on its own, for the login screen: the browser paints the
    /// strip over the auth form, but there is no account yet for a panel to
    /// read and no loadout for the bar to draw.
    void renderStripOnly(Canvas&, Window&, double timeSeconds);

    void toggle(MenuId);
    void close();
    MenuId open() const { return open_; }
    bool anyOpen() const { return open_ != MenuId::None; }

    /// True when the cursor is over menu furniture, so the game must not treat
    /// the click as aiming or the wheel as a zoom.
    bool capturesMouse(Vec2 mouse) const;

    /// How far in from the left edge the HUD must start to clear the icon
    /// strips: the width of the top row, and of the bottom column. The HUD asks
    /// rather than the strips reserving, because only the HUD knows which of
    /// its pieces can move.
    ///
    /// The top row is nine buttons wide in game, so `reservedTop()` is most of
    /// the screen: the browser build clears it by dropping BELOW the row, not
    /// by moving right. `stripBottom()` is the y to use for that.
    double reservedTop() const;
    double reservedLeft() const;
    /// The first y below the top icon row, strip inset included.
    double stripBottom() const;

    /// How many releases the changelog holds. That, against what the player
    /// has already seen, is the whole of the browser's unread rule
    /// (`CHANGELOG.length > lastSeenChangelogCount`), and opening the panel is
    /// what writes the count back -- the same gesture, not a separate
    /// acknowledgement.
    ///
    /// One rather than zero by default, so a player who has never opened the
    /// panel is told there is something in it even before whoever owns the
    /// changelog table has reported its size.
    void setChangelogEntryCount(int count) { changelogEntries_ = count < 1 ? 1 : count; }
    /// True while the strip's changelog button should shake.
    bool changelogUnread() const { return changelogEntries_ > settings_.changelogSeen; }

    /// True while a settings row is waiting for a key.
    bool capturingKey() const { return settings_panel_.capturingKey(); }

    /// Feeds the debug panel one frame of samples. See DebugPanel::recordFrame
    /// for why this is not done inside render().
    void recordDebugSample(double dtSeconds, NetClient& net) {
        debug_.recordFrame(dtSeconds, net);
    }

    ClientSettings& settings() { return settings_; }
    const ClientSettings& settings() const { return settings_; }

    /// The chat box and the login form both take typed text; the menus must
    /// not also eat it. Panels with a text field set this while focused.
    bool wantsText() const { return wantsText_; }
    void setWantsText(bool wants) { wantsText_ = wants; }

private:
    /// One slot of the icon strip. The strip is not a projection of MenuId:
    /// two of its buttons open no panel at all, and the order on screen is the
    /// browser's, not the enum's.
    enum class StripAction : std::uint8_t { OpenMenu, Discord, Exit };
    struct StripSlot {
        MenuId menu;
        StripAction action;
        const char* icon;
        bool topRow;
        std::uint32_t fill;
        std::uint32_t border;
    };

    /// The strip's slots, in the browser build's order. That order is at once
    /// the layout order, the draw order, the hit-test order and the slide-in
    /// stagger order, so it is spelled out once rather than derived.
    static const std::array<StripSlot, kStripSlotCount>& strip();

    /// Where the open card falls in the paint order. It is always clear of the
    /// bar-and-strip pair, never between them -- the browser has no seam there
    /// for a card to sit in. The title screen keeps the browser's split:
    /// renderCanvasUI paints settings and debug under both, and every other
    /// card lands over them. In game every card is over both, so the loadout
    /// bar never paints across an open menu.
    enum class PanelLayer : std::uint8_t { Under, Over };
    static PanelLayer panelLayer(MenuId, bool inGame);

    static Rect panelBounds(MenuId, int viewWidth, int viewHeight);
    /// Draws whichever card is on screen -- which is `drawn_`, not `open_`,
    /// while one is still sliding out.
    void renderOpenPanel(Canvas&, Window&, NetClient&, const SpriteCache&, const WorldRenderer&,
                         double timeSeconds, double dt);
    void drawIconStrip(Canvas&, Window&, double timeSeconds);
    const SvgDocument* icon(int index);
    /// The bar's box and slot scale for the screen it is being drawn on. The
    /// title screen gives it a fixed 900x210 region below centre; in game it
    /// owns the whole viewport at three-quarter scale.
    void drawLoadoutBar(Canvas&, Window&, NetClient&, const SpriteCache&, double timeSeconds);
    /// Pick-up and drop, run AFTER the open panel has had the same click. The
    /// bar is painted under the panel and so must not answer for a press the
    /// panel is standing on top of.
    void updateLoadoutInput(Window&, NetClient&);
    void drawDragged(Canvas&, Window&, const SpriteCache&, double timeSeconds);
    void activateStripSlot(int slot);

    MenuId open_ = MenuId::None;
    /// The card being painted. It outlives `open_` by the length of the
    /// slide-out, which is the only reason the two are separate.
    MenuId drawn_ = MenuId::None;
    /// 0 = a full viewport height below its anchor, 1 = seated. Only the tall
    /// list panels use it: they are DOM shells with a transform transition,
    /// where the corner overlays are canvas panels drawn straight at (20, 72).
    double panelSlide_ = 0;
    ClientSettings settings_;
    DragState drag_;
    bool wantsText_ = false;

    bool inGame_ = false;
    bool exitRequested_ = false;
    bool logoutRequested_ = false;
    int changelogEntries_ = 1;

    /// The icon artwork, compiled on first use. One document per glyph, shared
    /// with nothing -- these are the only SVGs the UI layer draws.
    std::vector<std::shared_ptr<SvgDocument>> icons_;

    /// Where the strip's buttons and the loadout slots ended up last frame.
    /// Recomputed every frame; kept only so capturesMouse() can answer without
    /// laying the strip out again.
    std::array<Rect, kStripSlotCount> stripRects_{};
    /// When each slot's slide-in began, in seconds. Negative means idle.
    std::array<double, kStripSlotCount> slideStart_{};
    /// Whether a slot was visible last frame, so a late reveal (the exit and
    /// debug buttons) can slide in on the false->true edge only.
    std::array<bool, kStripSlotCount> stripVisible_{};
    bool stripSeeded_ = false;
    /// The slot the current press began on. A click only counts when the
    /// release lands on the same one, and only that slot draws the dark tint.
    int pressedSlot_ = -1;

    /// 0..1; the bar rises 120px into place on its first frames, and sinks
    /// back the same way once there is no loadout left to show.
    double loadoutSlide_ = 0;
    /// Slot under the cursor last frame: 0..19, kLoadoutTrashSlot, or -1.
    /// Recomputed by the draw pass, which is the only one that lays the bar
    /// out, and read by the input pass that runs after it.
    int loadoutHovered_ = -1;
    /// Whether that slot is one a press would actually lift a petal out of.
    /// The bar swallows a click for that and for nothing else -- an empty
    /// slot, the trash and the gaps all fall through and fire an attack.
    bool loadoutGrabbable_ = false;
    /// Which of the ten secondary slots Q/E has selected, or -1. Clears itself
    /// five seconds after the last press.
    int selectedSecondary_ = -1;
    double lastSelectTime_ = 0;
    /// Recorded by handleKeys and applied by drawLoadoutBar, which is the only
    /// place that has the network client to act on them.
    int pendingSwapSlot_ = -1;
    /// -1 back, +1 forward, 0 none: which way Q/E asked the selection to move.
    int pendingCycle_ = 0;
    bool pendingSecondaryDelete_ = false;
    Rect panelRect_{};

    InventoryPanel inventory_;
    CraftingPanel crafting_;
    TalentsPanel talents_;
    GalleryPanel gallery_;
    ShopPanel shop_;
    SkinsPanel skins_;
    LeaderboardPanel leaderboard_;
    SettingsPanel settings_panel_;
    ChangelogPanel changelog_;
    NotificationsPanel notifications_;
    GuildPanel guild_;
    DebugPanel debug_;
};

/// The label and hotkey shown on the menu bar.
const char* menuLabel(MenuId);
/// A key's name, for the settings list. "Unbound" for Key::Unknown.
const char* keyName(Key);

} // namespace flix
