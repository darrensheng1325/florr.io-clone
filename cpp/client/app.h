#pragma once
// The client application: window, screens, input, and the frame loop.
//
// One state machine drives the whole client. Each screen owns its layout and
// its hit-testing; there is no retained widget tree, because at this size an
// immediate-mode pass is simpler to follow and impossible to leave stale.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "canvas.h"
#include "svg.h"
#include "window.h"

#include "client/camera.h"
#include "client/net_client.h"
#include "client/ui/context_menu.h"
#include "client/ui/text_select.h"
#include "client/render/sprites.h"
#include "client/render/world_renderer.h"
#include "client/ui/menus.h"
#include "client/ui/mobile_controls.h"
#include "client/ui/tutorial.h"
#include "shared/game/map_elements.h"
#include "shared/core/types.h"

namespace flix {

class ContentRegistry;

enum class Screen : std::uint8_t {
    Connecting,   ///< reaching the server and handshaking
    Login,        ///< username/password, or resuming a stored session
    Lobby,        ///< logged in: loadout, inventory, and the play button
    Playing,
    Dead,         ///< the death card, over a frozen world
    Disconnected,
};

struct AppConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3000;
    std::string dataDir = "data";
    /// Where the session token and the chosen flower name are remembered
    /// between runs, so a returning player is asked for neither again. Only
    /// the token is stored -- never the password.
    std::string sessionFile = ".florr-session";
    int windowWidth = 1280;
    int windowHeight = 720;
    bool fullscreen = false;

    /// Render this many frames, write the last one to `screenshotPath`, and
    /// exit. For testing what the client actually draws without needing a
    /// display, a window server, or screen-recording permission.
    int screenshotAfterFrames = 0;
    std::string screenshotPath;
    /// Log in and join automatically, so a screenshot run reaches the game
    /// rather than sitting on the login form.
    std::string autoUsername;
    std::string autoPassword;
    /// Open this menu on the way in. Only useful with --screenshot: a panel is
    /// the one part of the client a scripted run cannot otherwise reach.
    MenuId autoMenu = MenuId::None;
    /// Whether a scripted login goes straight into a game. Cleared by --lobby,
    /// which is the only way to photograph the title screen of a client that
    /// has credentials -- otherwise it joins before the first frame is drawn.
    bool autoJoin = true;
    /// Where a scripted join lands: a biome name, or "pvp"/"maze" for the two
    /// realms that are destinations rather than places on the map. Empty means
    /// the beginner ground. The only way a --screenshot run can photograph the
    /// arena or the maze, which no scripted run can otherwise walk to.
    std::string autoSpawn;
    /// Ignore any stored session and start on the login form. The only way to
    /// photograph the auth screen once a machine has logged in once.
    bool forceLogin = false;
    /// Paint the frame/ping/position counters the browser build gates behind
    /// its `showStats` setting.
    bool showStats = false;
    /// Put the touch controls up whatever the settings file and the device
    /// say. A desktop window reports a mouse and gets no stick, so this is the
    /// only way a screenshot run can photograph them.
    bool forceTouchControls = false;
    /// Force the death card up as soon as the auto-join lands. There is no
    /// other way for a scripted run to photograph it: reaching it for real
    /// means being killed, which a `--frames` run cannot arrange.
    bool autoDead = false;
    /// Force the tutorial card up on the way in, whole and on the first frame.
    ///
    /// A scripted run gets no tutorial without this. The browser build has the
    /// same problem from the other side -- the overlay covers a quarter of
    /// every in-game shot -- and its harness answers it by writing
    /// `tutorial_completed` before the join and clearing it for the one shot
    /// that wants the card. This flag is that switch, and a scripted login is
    /// what it is off for: a real player with no settings file still meets the
    /// tutorial on their first game.
    bool autoTutorial = false;
    /// Transcript lines to seed before the first frame, newest last. The chat
    /// box is the one surface whose content a screenshot run cannot arrange --
    /// it needs a second player to say something -- and it is where the
    /// server's markup lands, so the parity rig needs a way to put a known
    /// line in it. Delivered exactly as a server line would be, markup and
    /// all, so what is photographed is the real parse and the real layout.
    std::vector<std::string> seedChat;
};

class App {
public:
    App();
    ~App();

    /// Loads content, opens the window, and connects. Returns false with
    /// `errorOut` set if any of those cannot be done.
    bool start(const AppConfig&, std::string& errorOut);

    /// Runs until the window closes.
    void run();

    /// One frame. Returns false once the app is finished -- the window closed,
    /// or a --frames run reached its count. run() is a loop over this; the
    /// emscripten build cannot own the loop (the browser does) and drives this
    /// from a requestAnimationFrame callback instead.
    bool step();

    /// Writes the settings and session files.
    ///
    /// Separate from shutdown() because the browser build has no way out to
    /// save on: the tab is closed or reloaded and step() is simply never
    /// called again, so the page's unload handler calls this instead.
    void persist();

    /// Finishes: saves, as persist() does. run() does this on the way out; the
    /// emscripten build calls it when step() first returns false.
    void shutdown();

private:
    void frame(double dt);
    void pollNetwork();

    void updateConnecting();
    void updateLogin(double dt);
    void updateLobby(double dt);
    void updatePlaying(double dt);
    void updateDead(double dt);

    void drawLogin(Canvas&, double time);
    void drawLobby(Canvas&, double time);
    /// The lobby's name field, Ready button and spawn-biome row, laid out once
    /// so the interaction pass and the draw pass cannot disagree about where
    /// they are.
    struct LobbyLayout {
        Rect name;
        Rect ready;
        std::vector<Rect> biomes;
    };
    LobbyLayout lobbyLayout(int viewWidth, int viewHeight) const;
    /// The title screen's chat line, bottom-left, under the icon column.
    static Rect titleChatBox(int viewWidth, int viewHeight);
    void drawTitleChat(Canvas&, double time);
    /// The daily-login card, top-right. Painted last, over the panels, exactly
    /// as the browser build's own always-on-top widget is.
    void drawDailyStreak(Canvas&, double time);
    /// The frame/ping/position readout in the bottom-right corner. On the
    /// title screens it is a fixed set of placeholder lines rather than a live
    /// readout, which is what the reference paints there.
    void drawStatsCounters(Canvas&, bool titleScreen);
    /// Whether that readout is up. Two switches turn it on: the Settings
    /// panel's "Show Performance Stats", which is where a player finds it and
    /// which persists, and --stats, which is how a scripted run photographs it
    /// without a settings file.
    bool statsVisible() const;
    /// Every biome the player may start in, "default" first.
    const std::vector<std::string>& spawnChoices() const { return spawnChoices_; }
    /// Draws the scrolling title texture behind all non-game screens.
    void drawTitleBackground(Canvas&, double time);
    /// Steps and draws the petals drifting over that texture.
    void drawTitlePetals(Canvas&, double time);
    void drawHud(Canvas&, double time);
    /// The screen-top health bars a super, unique or apex mob in view gets,
    /// one under the other, name above each. Ultras are not bosses: they wear
    /// the ordinary bar under the body like every other mob.
    void drawBossBars(Canvas&, bool altHeld);
    /// Puts the session back together after the socket came back.
    ///
    /// A reconnection is always to a NEW server process -- that is what a
    /// restart is -- so the body, the world and the server's idea of this
    /// connection are all gone. The account is not: the session token outlives
    /// the socket, so it is presented again and the player lands back on the
    /// title screen ready to play rather than on a dead world.
    void onReconnected();

    /// The two commands the SERVER has no say in: a panel toggle, and a local
    /// render override. True when the line was one of them and must not be
    /// sent -- everything else, squad and guild lines included, is the
    /// server's to answer.
    bool handleClientCommand(const std::string& message);

    /// The party bars: one small block per squadmate, stacked under the main
    /// HUD. Nothing at all when the player squads alone, which is the normal
    /// case and is why the block starts where the main HUD ends rather than
    /// reserving room for itself.
    void drawSquadHud(Canvas&);
    /// The 200x200 section map in the top-right corner, its gold border and
    /// its biome caption.
    void drawMinimap(Canvas&);
    /// The maze realm's map: the whole layout rasterised, corridors light and
    /// tinted by depth band, walls dark, with the same player dots the
    /// overworld map draws. Replaces the section minimap inside the maze.
    void drawMazeMinimap(Canvas&, bool altHeld);
    /// The arena realm has no map to show; the reference puts the live
    /// scoreboard in the minimap's corner instead.
    void drawArenaLeaderboard(Canvas&);
    /// The maze minimap's baked static layer, rebuilt when the maze rotates.
    const Canvas* mazeMinimapStatic();
    /// The static layers of the minimap -- background, ALT spawn bands, wall
    /// tiles, teleporter dots -- baked once per section rather than rescanned
    /// every frame. `rarityGlow` is part of the key, not just the paint: the
    /// bands appear and vanish with ALT, so the bake has to be redone.
    const Canvas* minimapStatic(int section, bool rarityGlow);
    void drawDeathCard(Canvas&, double time);
    void drawChat(Canvas&, double time);
    /// The chat input slot, shared by the title screen and the game so the two
    /// cannot drift apart.
    void drawChatField(Canvas&, Rect box, double time);
    void drawConnectionState(Canvas&, double time);
    /// The red strip across the top of a live game whose socket has dropped.
    /// The world and the HUD keep drawing underneath, as the reference's do.
    void drawDisconnectBanner(Canvas&);
    /// Leaves the world for the title screen: what Continue, ENTER and the
    /// exit button all do.
    void leaveToTitle();

    /// Builds this frame's input from the keyboard and mouse and sends it.
    ///
    /// Nothing local acts on it: the flower's drawn position is the server's,
    /// eased (see client/interpolation.h). Producing one of these is the only
    /// thing the client does with a movement key.
    void sendInputFrame(double dt);

    /// Whether a text field is taking this frame's keystrokes -- the chat
    /// line, a login field, or a panel's own search/code box. While one is,
    /// a key is a character and nothing else: no movement, no hotkey.
    bool keyboardCaptured() const;

    /// Whether this frame's press landed on the closed chat slot, which opens
    /// it. The keyboard shortcut is the other way in, and the only way there
    /// used to be -- which left the chat unreachable on a device with no
    /// keyboard to press Enter on.
    bool pressedChatBox() const;

    /// The caret and selection of the three places one can be. Exclusive by
    /// construction -- only one of the auth form, the chat line and the lobby
    /// name box holds the keyboard at a time -- so the auth form's four inputs
    /// share one, re-seated as the focus moves between them.
    ui::TextFieldState authField_;
    ui::TextFieldState nameField_;
    ui::TextFieldState chatField_;
    /// Where drawChatField last painted, so the pointer can be tracked against
    /// it on the frame that follows.
    Rect chatBox_{};
    /// Where the lobby name box last painted, for the same reason.
    Rect nameBox_{};
    /// The transcript and its input line as one box, for the press that
    /// closes the chat by landing anywhere else.
    Rect chatRegion_{};
    ui::ContextMenu contextMenu_;

    /// Text entry shared by the login fields and the chat box.
    ///
    /// `masked` pins the caret to the end and refuses to copy: a password box
    /// draws one bullet per BYTE, so a caret dragged into the middle of one
    /// would sit off its own glyph, and there is nothing worth selecting in a
    /// field that shows no text.
    void editText(std::string& target, std::size_t maxLength, ui::TextFieldState& state,
                  bool masked = false);
    /// Resolves this frame's text drag, paints the highlight and runs the
    /// right-click menu. Last of everything: the runs it selects against are
    /// recorded as the panels paint, and its own card goes over them.
    void updateTextSelection(Canvas&);

    /// The lobby name box's plate, shared by its painter and its hit test.
    static ui::TextFieldStyle nameFieldStyle();
    /// The auth form's field at `index`, or null when there is none there.
    std::string* authValue(int index);
    /// Moves the caret to another auth field, taking its contents whole.
    void focusAuthField(int index);
    /// One frame of chat editing: typing, the slash-command list's keys, and
    /// send or cancel. Shared by the lobby and the game so the box behaves
    /// identically on both.
    void editChatLine();

    /// Runs one of the auth form's actions. Named after the browser build's
    /// `AuthAction`, so the two forms answer the same set of verbs.
    void submitAuth(const std::string& action);
    /// Points the socket at whatever the Advanced Settings field says, if that
    /// is somewhere else. True when a reconnect has started, in which case the
    /// caller must wait for the handshake before asking the server anything.
    bool retargetServer();

    /// Joins with this client's viewport, biome and flower name. One place, so
    /// the auto-login path and the Ready button cannot send different things.
    void startGame();

    /// Ends the session and goes back to the auth form: the server revokes the
    /// token, the client forgets the account, and the stored token on disk
    /// goes with it so a restart does not walk straight back in.
    void logout();

    void loadSession();
    void saveSession() const;

    /// Where the display switches, the zoom and the menu keys are remembered.
    std::string settingsPath() const;

    /// A token read from disk at startup. Held rather than sent immediately:
    /// the socket is not up yet at load time, and the server refuses anything
    /// before it has answered the handshake.
    std::string storedToken_;

    /// One floating petal of the title backdrop. Position is the sprite's
    /// TOP-LEFT, as the browser build's is, so a petal enters and leaves the
    /// screen at the same moment in both.
    struct TitlePetal {
        double x = 0;
        double y = 0;
        double speedX = 0;      ///< px per FRAME, not per second
        double size = 0;        ///< drawn diameter, in px
        double rotation = 0;    ///< degrees
        double rotationSpeed = 0;   ///< degrees per frame, either sign
        std::uint16_t petal = 0;
    };

    AppConfig config_;
    Window window_;
    NetClient net_;
    Camera camera_;
    SpriteCache sprites_;
    /// The title backdrop, one document per biome, compiled on first use. The
    /// picker changes which one tiles behind the menu, exactly as choosing a
    /// biome does in the browser build.
    std::unordered_map<std::string, std::shared_ptr<SvgDocument>> titleBackgrounds_;
    const SvgDocument* titleBackground(const std::string& biomeName);
    WorldRenderer renderer_;
    /// The map's annotation layer. The client reads the same bundle the server
    /// does, and needs only one thing from it: which biomes exist to be picked.
    MapData mapData_;
    std::vector<std::string> spawnChoices_;

    Screen screen_ = Screen::Connecting;
    double timeSeconds_ = 0;
    /// Frames in the last whole second, for the stats overlay.
    int framesPerSecond_ = 0;
    int frameCounter_ = 0;
    double fpsWindowStart_ = 0;
    /// The work a frame costs, averaged over the last whole second and rolled
    /// over beside the frame count. The raw per-frame value twitches far too
    /// much to read, which is why the reference averages it too.
    double frameTimeAvgMs_ = 0;
    double frameTimeAccum_ = 0;
    int frameTimeSamples_ = 0;

    /// The same measurement over the WHOLE run rather than the last second,
    /// which is what a --frames run reports on its way out. The rolling pair
    /// above is reset every second for the on-screen readout, so it cannot
    /// answer "was that build faster than this one" -- and the wall clock
    /// cannot either, because frameDelay caps the loop at 60Hz and hides every
    /// win below 16ms a frame.
    double runFrameAccum_ = 0;
    int runFrameSamples_ = 0;

    /// Per-layer render cost, averaged over the last whole second with that
    /// second's worst frame beside it. Both halves are wanted: the average is
    /// the steady cost, and the peak is the one-frame spike that a raw
    /// last-frame readout turns into unreadable flicker.
    struct SectionStats {
        double avgMillis = 0;
        double peakMillis = 0;
        double accumMillis = 0;
        double windowPeakMillis = 0;
    };
    SectionStats sectionMobs_, sectionItems_, sectionProjectiles_;
    int sectionItemCount_ = 0;

    /// When the next heartbeat ping is due. The reference sends one a second
    /// for as long as the socket is up, and the round trip it measures is the
    /// only thing the Ping readout and the connection-quality band have to go
    /// on -- without it both are permanently "--".
    double nextPingSeconds_ = 0;

    /// Wire bytes in the last whole second, and the heaviest opcodes in it.
    /// Sampled on the same once-a-second boundary the frame counter rolls on,
    /// because both are reported as per-second figures.
    std::uint32_t incomingBytesPerSecond_ = 0;
    std::uint32_t outgoingBytesPerSecond_ = 0;
    std::vector<NetClient::WireEvent> topWireEvents_;

    // -- title backdrop ------------------------------------------------------
    /// The backdrop's own clock, advanced a fixed step per RENDERED frame. The
    /// browser build does the same; using wall time instead would make the
    /// scroll and the petals run at different speeds on a machine that cannot
    /// hold sixty frames, which is exactly the machine where it would show.
    double titleBackgroundTime_ = 0;
    std::vector<TitlePetal> titlePetals_;
    /// Petal types the backdrop may show: everything that is not an admin
    /// petal and not an egg, which is the browser build's filter.
    std::vector<std::uint16_t> titlePetalTypes_;
    Rng titleRng_{0x5DEECE66Dull};
    /// The star of the daily-login card, one document per fill colour. Built
    /// on demand, because the colour depends on the streak.
    std::unordered_map<std::uint32_t, std::shared_ptr<SvgDocument>> streakStars_;
    const SvgDocument* streakStar(std::uint32_t fill);
    /// When the streak card first had a state to draw, which is what its
    /// post-claim wobble is measured from. Negative until then.
    double streakSeenAt_ = -1;

    // -- login form --------------------------------------------------------
    std::string usernameField_;
    std::string passwordField_;
    std::string confirmPasswordField_;
    /// The endpoint the Advanced Settings drawer edits, as "host" or
    /// "host:port". Seeded from the command line and applied on the next
    /// login, register or guest attempt.
    std::string serverField_;
    /// Which field has the caret: -1 for none, then username, password,
    /// confirmation (registering only) and the server endpoint. Nothing is
    /// focused until the player clicks, so the form opens showing both
    /// placeholders rather than a caret.
    int focusedField_ = -1;
    bool registering_ = false;
    bool advancedOpen_ = false;
    std::string loginMessage_;

    /// Which control the pointer went down on, by the browser build's own
    /// button ids. Latched at press: a press that began somewhere else must
    /// not light a button the pointer is merely dragged across.
    std::string pressedControl_;
    /// An auth action waiting for a reconnect to finish.
    std::string pendingAuth_;

    // -- lobby ---------------------------------------------------------------
    /// The flower's name, as typed in the field beside Ready. Remembered
    /// between runs beside the session token, which is where the browser build
    /// keeps it too (localStorage, not the account).
    std::string playerName_;

    /// Scripted-login progress. A screenshot or smoke run has no one to type,
    /// so it registers, and falls back to logging in when the name is taken.
    enum class AutoLogin : std::uint8_t { Idle, Registering, LoggingIn, Done, Failed };
    AutoLogin autoLogin_ = AutoLogin::Idle;

    // -- chat --------------------------------------------------------------
    bool chatOpen_ = false;
    std::string chatDraft_;
    /// Which row of the slash-command list is highlighted, or -1 when the list
    /// is not up. Reset to 0 every time the list opens, as the reference's is.
    int chatSuggestion_ = -1;

    // -- death ---------------------------------------------------------------
    /// The card can be dismissed with Close while the player stays dead and
    /// the world keeps rendering, which is the whole difference between Close
    /// and Continue.
    bool deathCardVisible_ = true;

    // -- HUD -----------------------------------------------------------------
    /// When invulnerability last ended, in app seconds; negative before the
    /// first time it does. The health bar fades from the invulnerable colour
    /// back to green over half a second from here.
    double invulEndedAt_ = -1;
    bool wasInvulnerable_ = false;

    /// The minimap's baked static layer and the section it was baked for.
    /// Rebuilt only when the player crosses into another section -- the tile
    /// scan is four and a half thousand cells and does not belong in a frame.
    std::unique_ptr<Canvas> minimapStatic_;
    int minimapSection_ = -1;
    /// Whether the cached bake has the ALT spawn bands in it.
    bool minimapGlow_ = false;
    /// The canvas pixels per design unit the bake was rasterised at. Part of
    /// the key: a bitmap baked for a 1x display and shown on a Retina one is
    /// the single blocky rectangle on an otherwise crisp screen.
    double minimapDensity_ = 0.0;

    /// The maze minimap's bake, keyed on the day it was built for and the
    /// density it was rasterised at, for the same reasons as the section one.
    std::unique_ptr<Canvas> mazeMinimapStatic_;
    std::int64_t mazeMinimapDay_ = 0;
    bool mazeMinimapBaked_ = false;
    double mazeMinimapDensity_ = 0.0;

    // -- scene wipe ----------------------------------------------------------
    /// The iris that covers a scene change. `Covered` holds the outgoing frame
    /// whole until the incoming scene has something to show; `Wiping` opens or
    /// closes the hole over IRIS_DURATION_MS.
    struct SceneWipe {
        enum class Phase : std::uint8_t { Idle, Covered, Wiping };
        Phase phase = Phase::Idle;
        /// True going INTO the game: the hole grows and the outgoing still is
        /// what is left outside it. False going back to the title, where the
        /// still shrinks to a point instead.
        bool holeGrows = false;
        double phaseStartSeconds = 0;
        /// A frozen copy of the outgoing scene. Null means the copy failed, in
        /// which case the wipe falls through black exactly as the reference's
        /// snapshot-less path does.
        std::unique_ptr<Canvas> snapshot;
    };
    SceneWipe wipe_;
    void beginSceneWipe(bool toGame);
    void drawSceneWipe(Canvas&);
    /// True once the incoming scene has something worth revealing.
    bool wipeReadyToReveal() const;

    // -- menus ---------------------------------------------------------------
    /// Owns every panel, the menu bar, the loadout strip and what is being
    /// dragged between them. The app's only job is to say when it may run and
    /// to stop feeding the game input it has taken.
    MenuSystem menus_;

    // -- tutorial ------------------------------------------------------------
    /// The onboarding card, which lives and dies with a game exactly as the
    /// browser's does -- it is built by Game and destroyed with it.
    ui::Tutorial tutorial_;

    // -- touch ---------------------------------------------------------------
    /// The on-screen stick and its two buttons. Drawn and answered only in a
    /// game: there is nothing on the title screen a thumb cannot already reach
    /// through the mirrored pointer (see Window's touch section).
    ui::MobileControls mobile_;
    /// What the display said about its pointer when the window opened. Asked
    /// once: it is a media query, and the answer does not change under a
    /// running page in any way that matters.
    bool coarsePointer_ = false;
    /// Whether the touch controls are up this frame: the setting, resolved
    /// against the device, and a game with no panel standing over them.
    bool touchControlsVisible() const;
    /// Publishes where this frame's text fields are, so a touch landing on one
    /// can raise the on-screen keyboard from inside its own gesture. See
    /// Window::setSoftKeyboardRegions.
    void publishKeyboardRegions();

    // -- input -------------------------------------------------------------
    /// The last aim this client sent. Read only on touch: a centred stick has
    /// no direction to give, and the alternative -- the pointer, which on a
    /// phone is wherever the last tap landed -- would swing the petals at it.
    double lastAimAngle_ = 0;
    std::uint32_t inputSequence_ = 0;
    /// Accumulates real time so input is produced at the simulation rate rather
    /// than once per rendered frame -- a 144 Hz client must not send (and be
    /// simulated for) six times what a 25 Hz one does.
    double inputAccumulator_ = 0;

    bool running_ = false;
    /// Frames drawn since start, for --frames. A step() counter rather than a
    /// local, because the loop is no longer necessarily this object's.
    int framesDrawn_ = 0;
};

} // namespace flix
