#include "window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef __EMSCRIPTEN__
// The browser backend. Drawing already goes to a real
// CanvasRenderingContext2D (see canvas.cpp), so what is left for a Window
// here is the geometry and the input -- and DOM events, unlike SDL's queue,
// are delivered by callback rather than polled. They land in the same Impl
// fields the native path fills, so everything above this file is unaware.
#include <emscripten.h>
#include <emscripten/html5.h>
#else
#include <SDL.h>
#endif

namespace {

#ifndef __EMSCRIPTEN__
Key fromScancode(SDL_Scancode code) {
  switch (code) {
    case SDL_SCANCODE_A: return Key::A; case SDL_SCANCODE_B: return Key::B;
    case SDL_SCANCODE_C: return Key::C; case SDL_SCANCODE_D: return Key::D;
    case SDL_SCANCODE_E: return Key::E; case SDL_SCANCODE_F: return Key::F;
    case SDL_SCANCODE_G: return Key::G; case SDL_SCANCODE_H: return Key::H;
    case SDL_SCANCODE_I: return Key::I; case SDL_SCANCODE_J: return Key::J;
    case SDL_SCANCODE_K: return Key::K; case SDL_SCANCODE_L: return Key::L;
    case SDL_SCANCODE_M: return Key::M; case SDL_SCANCODE_N: return Key::N;
    case SDL_SCANCODE_O: return Key::O; case SDL_SCANCODE_P: return Key::P;
    case SDL_SCANCODE_Q: return Key::Q; case SDL_SCANCODE_R: return Key::R;
    case SDL_SCANCODE_S: return Key::S; case SDL_SCANCODE_T: return Key::T;
    case SDL_SCANCODE_U: return Key::U; case SDL_SCANCODE_V: return Key::V;
    case SDL_SCANCODE_W: return Key::W; case SDL_SCANCODE_X: return Key::X;
    case SDL_SCANCODE_Y: return Key::Y; case SDL_SCANCODE_Z: return Key::Z;
    case SDL_SCANCODE_0: return Key::Num0; case SDL_SCANCODE_1: return Key::Num1;
    case SDL_SCANCODE_2: return Key::Num2; case SDL_SCANCODE_3: return Key::Num3;
    case SDL_SCANCODE_4: return Key::Num4; case SDL_SCANCODE_5: return Key::Num5;
    case SDL_SCANCODE_6: return Key::Num6; case SDL_SCANCODE_7: return Key::Num7;
    case SDL_SCANCODE_8: return Key::Num8; case SDL_SCANCODE_9: return Key::Num9;
    case SDL_SCANCODE_SPACE: return Key::Space;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return Key::Enter;
    case SDL_SCANCODE_ESCAPE: return Key::Escape;
    case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
    case SDL_SCANCODE_TAB: return Key::Tab;
    case SDL_SCANCODE_DELETE: return Key::Delete;
    case SDL_SCANCODE_HOME: return Key::Home; case SDL_SCANCODE_END: return Key::End;
    case SDL_SCANCODE_LEFT: return Key::Left; case SDL_SCANCODE_RIGHT: return Key::Right;
    case SDL_SCANCODE_UP: return Key::Up; case SDL_SCANCODE_DOWN: return Key::Down;
    case SDL_SCANCODE_LSHIFT: return Key::LeftShift; case SDL_SCANCODE_RSHIFT: return Key::RightShift;
    case SDL_SCANCODE_LCTRL: return Key::LeftCtrl; case SDL_SCANCODE_RCTRL: return Key::RightCtrl;
    case SDL_SCANCODE_LALT: return Key::LeftAlt; case SDL_SCANCODE_RALT: return Key::RightAlt;
    case SDL_SCANCODE_MINUS: return Key::Minus; case SDL_SCANCODE_EQUALS: return Key::Equals;
    case SDL_SCANCODE_COMMA: return Key::Comma; case SDL_SCANCODE_PERIOD: return Key::Period;
    case SDL_SCANCODE_SLASH: return Key::Slash; case SDL_SCANCODE_BACKSLASH: return Key::Backslash;
    case SDL_SCANCODE_SEMICOLON: return Key::Semicolon; case SDL_SCANCODE_APOSTROPHE: return Key::Apostrophe;
    case SDL_SCANCODE_F1: return Key::F1; case SDL_SCANCODE_F2: return Key::F2;
    case SDL_SCANCODE_F3: return Key::F3; case SDL_SCANCODE_F4: return Key::F4;
    case SDL_SCANCODE_F5: return Key::F5; case SDL_SCANCODE_F6: return Key::F6;
    case SDL_SCANCODE_F7: return Key::F7; case SDL_SCANCODE_F8: return Key::F8;
    case SDL_SCANCODE_F9: return Key::F9; case SDL_SCANCODE_F10: return Key::F10;
    case SDL_SCANCODE_F11: return Key::F11; case SDL_SCANCODE_F12: return Key::F12;
    default: return Key::Unknown;
  }
}
#endif

constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);
constexpr std::size_t kButtonCount = static_cast<std::size_t>(MouseButton::Count);
constexpr std::size_t kCursorCount = static_cast<std::size_t>(CursorShape::Count);

} // namespace

#ifdef __EMSCRIPTEN__
// KeyboardEvent.code -> Key. `code` is the PHYSICAL key, which is what Key
// names, so this is a rename and not a layout decision: a Dvorak typist's
// "KeyW" is the same key a QWERTY typist's is, and both walk forward.
Key fromDomCode(const char* code) {
  const std::string c = code;
  if (c.size() == 4 && c.compare(0, 3, "Key") == 0 && c[3] >= 'A' && c[3] <= 'Z') {
    return static_cast<Key>(static_cast<int>(Key::A) + (c[3] - 'A'));
  }
  if (c.size() == 6 && c.compare(0, 5, "Digit") == 0 && c[5] >= '0' && c[5] <= '9') {
    return static_cast<Key>(static_cast<int>(Key::Num0) + (c[5] - '0'));
  }
  if (c.size() == 7 && c.compare(0, 6, "Numpad") == 0 && c[6] >= '0' && c[6] <= '9') {
    return static_cast<Key>(static_cast<int>(Key::Num0) + (c[6] - '0'));
  }
  if (c.size() >= 2 && c[0] == 'F' && std::isdigit(static_cast<unsigned char>(c[1]))) {
    const int n = std::atoi(c.c_str() + 1);
    if (n >= 1 && n <= 12) return static_cast<Key>(static_cast<int>(Key::F1) + (n - 1));
  }
  struct Named { const char* code; Key key; };
  static const Named kNamed[] = {
    {"Space", Key::Space},           {"Enter", Key::Enter},
    {"NumpadEnter", Key::Enter},     {"Escape", Key::Escape},
    {"Backspace", Key::Backspace},   {"Tab", Key::Tab},
    {"Delete", Key::Delete},         {"Home", Key::Home},
    {"End", Key::End},               {"ArrowLeft", Key::Left},
    {"ArrowRight", Key::Right},      {"ArrowUp", Key::Up},
    {"ArrowDown", Key::Down},        {"ShiftLeft", Key::LeftShift},
    {"ShiftRight", Key::RightShift}, {"ControlLeft", Key::LeftCtrl},
    {"ControlRight", Key::RightCtrl},{"AltLeft", Key::LeftAlt},
    {"AltRight", Key::RightAlt},     {"MetaLeft", Key::LeftCtrl},
    {"MetaRight", Key::RightCtrl},   {"Minus", Key::Minus},
    {"NumpadSubtract", Key::Minus},  {"Equal", Key::Equals},
    {"NumpadAdd", Key::Equals},      {"Comma", Key::Comma},
    {"Period", Key::Period},         {"NumpadDecimal", Key::Period},
    {"Slash", Key::Slash},           {"NumpadDivide", Key::Slash},
    {"Backslash", Key::Backslash},   {"Semicolon", Key::Semicolon},
    {"Quote", Key::Apostrophe},
  };
  for (const Named& named : kNamed) {
    if (c == named.code) return named.key;
  }
  return Key::Unknown;
}

// The clipboard a page is allowed to read without asking is the one the user
// just pasted into it, so that is what is kept. navigator.clipboard.readText()
// would need a permission prompt mid-game; a paste event needs none.
EM_JS(void, web_clipboard_init, (), {
  Module.cppCanvasClipboard = '';
  // Bumped on every paste, never reset. pump() watches it for a change rather
  // than for a flag it would have to clear, so a paste that lands while the
  // page is between frames is still delivered to the frame that follows.
  Module.cppCanvasPasteSeq = 0;
  addEventListener('paste', (event) => {
    Module.cppCanvasClipboard = (event.clipboardData || window.clipboardData).getData('text') || '';
    Module.cppCanvasPasteSeq = (Module.cppCanvasPasteSeq | 0) + 1;
  });
});
EM_JS(int, web_clipboard_paste_seq, (), {
  return Module.cppCanvasPasteSeq | 0;
});
// Into a caller-owned buffer rather than a malloc'd string: _malloc is not
// exported to JavaScript by default, and needing it would make this file
// impose a link setting on every program that draws a window.
EM_JS(void, web_clipboard_get, (char* out, int capacity), {
  stringToUTF8(Module.cppCanvasClipboard || '', out, capacity);
});
EM_JS(void, web_clipboard_set, (const char* text), {
  const value = UTF8ToString(text);
  Module.cppCanvasClipboard = value;
  if (navigator.clipboard) navigator.clipboard.writeText(value).catch(() => {});
});
EM_JS(void, web_set_cursor, (const char* element, const char* shape), {
  const node = document.getElementById(UTF8ToString(element));
  if (node) node.style.cursor = UTF8ToString(shape);
});
// CSS pixels, which are this backend's "points". Read every pump for the same
// reason the native path re-reads the window size: a resize, a zoom and a
// drag onto another display are three different events and only one of them
// is reliably delivered.
EM_JS(double, web_css_width, (const char* element), {
  const node = document.getElementById(UTF8ToString(element));
  return node ? node.clientWidth : 0;
});
EM_JS(double, web_css_height, (const char* element), {
  const node = document.getElementById(UTF8ToString(element));
  return node ? node.clientHeight : 0;
});

// Whether the primary pointing device is a finger. The same query the browser
// client asks, and the only thing a page can say about it before anything has
// been touched.
EM_JS(int, web_coarse_pointer, (), {
  return (window.matchMedia && window.matchMedia('(pointer: coarse)').matches) ? 1 : 0;
});

// The invisible <input> that stands in for a focusable canvas.
//
// Invisible rather than hidden: display:none and visibility:hidden both refuse
// focus, and a field that cannot be focused cannot summon a keyboard. The 16px
// font is not decoration either -- iOS zooms the whole page when a smaller
// field takes focus, and the zoom does not come back.
//
// Nothing is ever read off this element. It exists to be focused; the
// keystrokes it produces bubble to the window, where the key handler above
// already listens, and that is the whole of the path text takes.
EM_JS(void, web_soft_keyboard, (int wanted), {
  let node = document.getElementById('__flix_soft_keyboard');
  if (!node) {
    if (!wanted) return;
    node = document.createElement('input');
    node.id = '__flix_soft_keyboard';
    node.type = 'text';
    node.setAttribute('autocomplete', 'off');
    node.setAttribute('autocapitalize', 'off');
    node.setAttribute('autocorrect', 'off');
    node.setAttribute('spellcheck', 'false');
    node.setAttribute('enterkeyhint', 'go');
    node.style.cssText =
        'position:fixed;top:35%;left:50%;width:2px;height:2px;opacity:0;' +
        'font-size:16px;border:none;padding:0;background:transparent;' +
        'pointer-events:none;z-index:-1;';
    // Never allowed to hold text: the value is not what the client reads, and
    // a growing one would let a soft keyboard's autocorrect rewrite history
    // the client already consumed.
    node.addEventListener('input', () => { node.value = ''; });
    document.body.appendChild(node);
  }
  if (wanted) node.focus({ preventScroll: true });
  else node.blur();
});
#endif

struct Window::Impl {
#ifdef __EMSCRIPTEN__
  // The element every frame is drawn into, and the two clocks the native path
  // keeps in SDL performance counters.
  std::string elementId = "canvas";
  double startMillis = 0, lastFrameMillis = 0;

  // Edge events, waiting for a frame to observe them.
  //
  // The native backend clears the edges at the top of pump() because the
  // events that refill them are still sitting in SDL`s queue, and the drain
  // immediately below re-applies them. DOM events have no queue: they were
  // delivered by callback at some point between the last frame and this one,
  // so clearing on entry throws every one of them away. A click that goes down
  // and up inside one frame gap -- which at 16ms a frame is most clicks --
  // would then never be seen at all, and no button in the game would work.
  //
  // So the callbacks record here and pump() moves the set across in one go.
  // Accumulating rather than overwriting is what lets a press and its release
  // arrive on the same frame, exactly as they do natively when both come out
  // of a single drain.
  std::array<bool, kKeyCount> pendingPressed{}, pendingReleased{};
  std::array<bool, kButtonCount> pendingDownEdge{}, pendingUpEdge{};
  float pendingWheel = 0;
  std::string pendingTyped;
  // The paste counter as of the last frame, and whether it moved since.
  int pasteSeq = 0;
  bool pasted = false;

  void takePendingEvents() {
    pressed = pendingPressed;
    released = pendingReleased;
    mouseDownEdge = pendingDownEdge;
    mouseUpEdge = pendingUpEdge;
    wheel = pendingWheel;
    typed = pendingTyped;

    pendingPressed.fill(false);
    pendingReleased.fill(false);
    pendingDownEdge.fill(false);
    pendingUpEdge.fill(false);
    pendingWheel = 0;
    pendingTyped.clear();

    // A paste is a page event rather than a key: the browser fires it after
    // the keystroke's own default action, and fires it for the context menu
    // and the touch keyboard too. Watching the counter is what catches all
    // three without the page ever reading the clipboard unprompted.
    const int seq = web_clipboard_paste_seq();
    pasted = seq != pasteSeq;
    pasteSeq = seq;
  }
#else
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  SDL_Texture* texture = nullptr;
  std::vector<std::uint8_t> rgba;
  Uint64 startCounter = 0, lastFrameCounter = 0;
#endif
  std::unique_ptr<Canvas> canvas;
  // The window as the OS sees it, in points.
  int pointWidth = 0, pointHeight = 0;
  // The canvas's backing store, in pixels.
  int pixelWidth = 0, pixelHeight = 0;
  // The design space, in design units. Zero until setDesignSize().
  int designWidth = 0, designHeight = 0;
  // The reported size, in design units. Ceiled, never floored: a full-screen
  // fill written as fillRect(0, 0, width(), height()) has to cover the last
  // pixel column, and half a design unit of overdraw is cheaper than a seam.
  int viewWidth = 0, viewHeight = 0;
  double devicePixelRatio = 1.0;
  double renderScale = 1.0;
  // Points per design unit, and canvas pixels per design unit. `fit` is what
  // the mouse is divided by; `uiScale` is the caller's base transform.
  double fit = 1.0;
  double uiScale = 1.0;
  bool shouldClose = false;

  std::array<bool, kKeyCount> down{}, pressed{}, released{};
  std::array<bool, kButtonCount> mouseHeld{}, mouseDownEdge{}, mouseUpEdge{};
  float mouseX = 0, mouseY = 0, wheel = 0;
  std::string typed;
  bool shift = false, ctrl = false, alt = false;

  // -- touch ----------------------------------------------------------------
  // Contacts arrive from a DOM callback or the SDL queue, are parked here, and
  // are turned into this frame's stream -- and into the mirrored mouse -- by
  // drainTouches() at the top of the frame that will read them. The two-stage
  // shape is the browser's requirement (see Impl::takePendingEvents) and the
  // native path uses it too so the mirror rule below exists exactly once.
  std::vector<TouchEvent> pendingTouches, touchEvents;
  std::vector<TouchPoint> touches;
  std::vector<std::int64_t> claimed;
  std::int64_t mirrored = 0;
  bool mirroring = false;
  bool touchSeen = false;
  /// Whether a finger has put the mirrored pointer over the window. A touch
  /// device fires no mouseenter and has no OS pointer to ask about, so without
  /// this every "is the pointer even in the window" test -- which is what the
  /// title screen gates its buttons on -- answers no, forever.
  bool touchInside = false;
  Window::TouchClaimHandler touchClaim;
  std::vector<WindowRect> keyboardRegions;

  void recordTouch(TouchPhase phase, std::int64_t id, float x, float y) {
    touchSeen = true;
    pendingTouches.push_back(TouchEvent{phase, TouchPoint{id, x, y}});
  }

  /// Moves the parked contacts into this frame's stream, keeps the live set,
  /// and mirrors the one unclaimed contact onto the left mouse button.
  void drainTouches() {
    touchEvents.swap(pendingTouches);
    pendingTouches.clear();
    for (const TouchEvent& event : touchEvents) {
      const TouchPoint& point = event.point;
      switch (event.phase) {
        case TouchPhase::Began: {
          touches.push_back(point);
          // Asked once, on the way in: a control that wants this contact says
          // so now, and never sees the mouse move under it afterwards.
          if (touchClaim && touchClaim(point)) {
            claimed.push_back(point.id);
            break;
          }
          // A second unclaimed finger does NOT take the pointer from the first.
          // Two mirrored contacts would be one mouse teleporting between them,
          // which is worse than ignoring the second.
          if (mirroring) break;
          mirroring = true;
          mirrored = point.id;
          mouseX = point.x;
          mouseY = point.y;
          touchInside = true;
          const auto left = static_cast<std::size_t>(MouseButton::Left);
          mouseHeld[left] = true;
          mouseDownEdge[left] = true;
          break;
        }
        case TouchPhase::Moved: {
          for (TouchPoint& live : touches) {
            if (live.id == point.id) { live.x = point.x; live.y = point.y; }
          }
          if (mirroring && point.id == mirrored) {
            mouseX = point.x;
            mouseY = point.y;
            touchInside = true;
          }
          break;
        }
        case TouchPhase::Ended: {
          touches.erase(std::remove_if(touches.begin(), touches.end(),
                                       [&](const TouchPoint& live) {
                                         return live.id == point.id;
                                       }),
                        touches.end());
          claimed.erase(std::remove(claimed.begin(), claimed.end(), point.id), claimed.end());
          if (mirroring && point.id == mirrored) {
            mirroring = false;
            // The release lands where the finger left, not where it landed:
            // a UI that dispatches its click on mouseup tests that position.
            mouseX = point.x;
            mouseY = point.y;
            const auto left = static_cast<std::size_t>(MouseButton::Left);
            mouseHeld[left] = false;
            mouseUpEdge[left] = true;
          }
          break;
        }
      }
    }
  }
#ifdef __EMSCRIPTEN__
  bool pointerInside = false;
#endif

#ifndef __EMSCRIPTEN__
  // Built on first use and kept: SDL_CreateSystemCursor allocates, and a
  // window that asks for a shape asks for it every frame.
  std::array<SDL_Cursor*, kCursorCount> cursors{};
#endif
  CursorShape cursorRequested = CursorShape::Arrow;
  // Count means "nothing has been pushed to the OS yet".
  CursorShape cursorApplied = CursorShape::Count;

#ifdef __EMSCRIPTEN__
  void applyCursor() {
    if (cursorRequested == cursorApplied) return;
    const char* shape = "default";
    if (cursorRequested == CursorShape::Hand) shape = "pointer";
    else if (cursorRequested == CursorShape::Text) shape = "text";
    web_set_cursor(elementId.c_str(), shape);
    cursorApplied = cursorRequested;
  }
#else
  void applyCursor() {
    if (cursorRequested == cursorApplied) return;
    const auto slot = static_cast<std::size_t>(cursorRequested);
    if (slot >= kCursorCount) return;
    if (!cursors[slot]) {
      SDL_SystemCursor system = SDL_SYSTEM_CURSOR_ARROW;
      if (cursorRequested == CursorShape::Hand) system = SDL_SYSTEM_CURSOR_HAND;
      else if (cursorRequested == CursorShape::Text) system = SDL_SYSTEM_CURSOR_IBEAM;
      cursors[slot] = SDL_CreateSystemCursor(system);
      // A shape the platform will not make is not retried every frame; the
      // pointer keeps whatever it already had.
      if (!cursors[slot]) { cursorApplied = cursorRequested; return; }
    }
    SDL_SetCursor(cursors[slot]);
    cursorApplied = cursorRequested;
  }
#endif

  // The half of the geometry that is the same in a window and in a page:
  // point size, device pixel ratio and backing-store size in, design-space
  // viewport out. Both backends fill the inputs their own way and then call
  // this, so the rule below is stated once.
  void recomputeScales() {
    // The fit is measured in POINTS, not pixels: it decides how big the design
    // space looks to a person, and a Retina display does not make a window
    // physically smaller. Measuring it in pixels would halve the design space
    // on exactly the displays this exists to fix.
    //
    // MAX, not min. Both cover the window; the difference is what a window
    // that is not the design aspect ratio does with the leftover.
    //
    //   min covers by revealing extra on the long axis, and pays for it by
    //       letting the SHORT axis set the scale. A window dragged narrow
    //       then shrinks everything -- 400x1000 points comes out at 0.21x
    //       with 1920x4800 units of world on screen. That is a zoom control
    //       made out of the window's edge, and it hands whoever finds it a
    //       view four times the size of everyone else's.
    //   max covers by cropping the short axis, and lets the LONG axis set the
    //       scale. The viewport is then never larger than the design size on
    //       either axis -- exactly it at the design ratio, less on the odd
    //       one -- so no window shape reveals more world than any other, and
    //       none of them makes anything smaller.
    //
    // The cost is that a very lopsided window has very little room: at
    // 3000x300 the viewport is 1920x192 and the HUD is squeezed. That is the
    // right way to lose. Shrinking the world to make room would be the zoom
    // this exists to refuse.
    if (designWidth > 0 && designHeight > 0) {
      fit = std::max(static_cast<double>(pointWidth) / designWidth,
                     static_cast<double>(pointHeight) / designHeight);
      if (!(fit > 0)) fit = 1.0;
    } else {
      fit = 1.0;
    }
    uiScale = fit * devicePixelRatio * renderScale;
    if (!(uiScale > 0)) uiScale = 1.0;

    // Ceiled so a full-screen fill written as fillRect(0, 0, width(), height())
    // reaches the last pixel, and nudged first because it otherwise does not:
    // a window at exactly the design aspect ratio divides out to
    // 1920.0000000000002, and a bare ceil turns that into a 1921-unit viewport
    // that flickers back to 1920 at the next size. The nudge is thirteen
    // orders of magnitude below the coverage it gives up.
    constexpr double kSnap = 1e-6;
    viewWidth = std::max(1, static_cast<int>(std::ceil(pixelWidth / uiScale - kSnap)));
    viewHeight = std::max(1, static_cast<int>(std::ceil(pixelHeight / uiScale - kSnap)));
    canvas->setLogicalSize(viewWidth, viewHeight);
  }

#ifdef __EMSCRIPTEN__
  // The page's answer to the same question. CSS pixels are the points, the
  // browser owns the device pixel ratio, and the canvas element's backing
  // store is ours to set -- which is what constructing a Canvas on it does.
  //
  // Called every pump for the reason the native one is: a resize, a browser
  // zoom and a drag onto a second display all change these numbers, and only
  // some of them fire an event.
  void refreshGeometry() {
    const double cssWidth = web_css_width(elementId.c_str());
    const double cssHeight = web_css_height(elementId.c_str());
    if (!(cssWidth > 0) || !(cssHeight > 0)) return;

    pointWidth = static_cast<int>(std::lround(cssWidth));
    pointHeight = static_cast<int>(std::lround(cssHeight));
    devicePixelRatio = emscripten_get_device_pixel_ratio();
    if (!(devicePixelRatio > 0)) devicePixelRatio = 1.0;

    const int wantPixelW =
        std::max(1, static_cast<int>(std::lround(cssWidth * devicePixelRatio * renderScale)));
    const int wantPixelH =
        std::max(1, static_cast<int>(std::lround(cssHeight * devicePixelRatio * renderScale)));
    if (wantPixelW != pixelWidth || wantPixelH != pixelHeight || !canvas) {
      pixelWidth = wantPixelW;
      pixelHeight = wantPixelH;
      // Constructing a Canvas on the element resizes its backing store and
      // takes a fresh 2D context, so this is the resize -- which is also why
      // callers must not hold canvas() across pump().
      canvas = std::make_unique<Canvas>(pixelWidth, pixelHeight, elementId);
    }

    recomputeScales();
  }

  // DOM event handlers. Static members rather than free functions because
  // Impl is private to Window, and they write straight into the same fields
  // the native pump() fills from the SDL queue.
  static Impl* implOf(void* userData) { return static_cast<Impl*>(userData); }

  // The pointer arrives in CSS pixels relative to the canvas; the caller draws
  // and hit-tests in design units, so the point-to-design fit is all that has to
  // be divided out -- exactly as in the native SDL_MOUSEMOTION case.
  static void recordPointer(Impl* impl, double targetX, double targetY) {
    const float toDesign = impl->fit > 0 ? static_cast<float>(1.0 / impl->fit) : 1.0f;
    impl->mouseX = static_cast<float>(targetX) * toDesign;
    impl->mouseY = static_cast<float>(targetY) * toDesign;
  }

  static void recordModifiers(Impl* impl, bool shift, bool ctrl, bool alt, bool meta) {
    impl->shift = shift;
    // Meta counts as ctrl, so a Mac's Cmd shortcuts reach code written for Ctrl
    // -- which is what SDL's KMOD_GUI does natively.
    impl->ctrl = ctrl || meta;
    impl->alt = alt;
  }

  static EM_BOOL onMouse(int type, const EmscriptenMouseEvent* event, void* userData) {
    Impl* impl = implOf(userData);
    // The page's own mousedown/mouseup echo of a touch that was deliberately
    // left unconsumed (see onTouch). That contact was already mirrored onto
    // this same button as it happened; letting the echo through would press
    // whatever it landed on a second time.
    if (emscripten_get_now() < impl->ghostUntilMillis) return EM_TRUE;
    recordPointer(impl, event->targetX, event->targetY);
    recordModifiers(impl, event->shiftKey, event->ctrlKey, event->altKey, event->metaKey);
    if (type == EMSCRIPTEN_EVENT_MOUSEMOVE || type == EMSCRIPTEN_EVENT_MOUSEDOWN) {
      // A pointer event targeted at the canvas is also authoritative when the
      // pointer was already there as callbacks were installed and no enter
      // event was delivered afterwards.
      impl->pointerInside = true;
    }
    if (type == EMSCRIPTEN_EVENT_MOUSEMOVE) return EM_TRUE;

    std::size_t index = kButtonCount;
    if (event->button == 0) index = static_cast<std::size_t>(MouseButton::Left);
    else if (event->button == 1) index = static_cast<std::size_t>(MouseButton::Middle);
    else if (event->button == 2) index = static_cast<std::size_t>(MouseButton::Right);
    if (index < kButtonCount) {
      const bool downNow = type == EMSCRIPTEN_EVENT_MOUSEDOWN;
      impl->mouseHeld[index] = downNow;
      (downNow ? impl->pendingDownEdge : impl->pendingUpEdge)[index] = true;
    }
    return EM_TRUE;
  }

  static EM_BOOL onPointerBoundary(int type, const EmscriptenMouseEvent* event,
                                   void* userData) {
    Impl* impl = implOf(userData);
    recordPointer(impl, event->targetX, event->targetY);
    impl->pointerInside = type == EMSCRIPTEN_EVENT_MOUSEENTER;
    if (impl->pointerInside) return EM_TRUE;

    // Match the page client's mouseleave rule: a drag that leaves the canvas
    // releases its latched buttons instead of leaving gameplay stuck down.
    for (std::size_t i = 0; i < kButtonCount; ++i) {
      if (impl->mouseHeld[i]) {
        impl->mouseHeld[i] = false;
        impl->pendingUpEdge[i] = true;
      }
    }
    return EM_TRUE;
  }

  static EM_BOOL onTouch(int type, const EmscriptenTouchEvent* event, void* userData) {
    Impl* impl = implOf(userData);
    const float toDesign = impl->fit > 0 ? static_cast<float>(1.0 / impl->fit) : 1.0f;
    TouchPhase phase = TouchPhase::Began;
    if (type == EMSCRIPTEN_EVENT_TOUCHMOVE) phase = TouchPhase::Moved;
    else if (type != EMSCRIPTEN_EVENT_TOUCHSTART) phase = TouchPhase::Ended;

    bool leaveToThePage = false;
    for (int i = 0; i < event->numTouches; ++i) {
      const EmscriptenTouchPoint& point = event->touches[i];
      // Only the contacts this event is ABOUT. A DOM touch event carries every
      // finger on the screen, changed or not, and treating them all as moves
      // would replay a stationary finger's position on every frame.
      if (!point.isChanged) continue;
      const float x = static_cast<float>(point.targetX) * toDesign;
      const float y = static_cast<float>(point.targetY) * toDesign;
      if (phase == TouchPhase::Began) {
        // A tap on a field is the one gesture this window does NOT consume.
        // Every browser refuses to raise its keyboard for a focus() made
        // during a touch it has been told to ignore, and a consumed touch
        // produces no click either -- so consuming it would leave no moment
        // at all where the keyboard could be asked for. See onClick.
        if (impl->onKeyboardField(x, y)) {
          impl->passthrough = point.identifier;
          impl->passthroughLive = true;
        } else {
          // Anywhere else takes the keyboard away, and needs no gesture to
          // do it.
          impl->lowerKeyboard();
        }
      }
      if (impl->passthroughLive && point.identifier == impl->passthrough) {
        leaveToThePage = true;
        if (phase == TouchPhase::Ended) {
          impl->passthroughLive = false;
          // The page is about to replay this gesture as mousedown, mouseup
          // and click. The first two are already in this frame's stream from
          // the mirror, so they are dropped -- see onMouse.
          impl->ghostUntilMillis = emscripten_get_now() + kGhostWindowMillis;
        }
      }
      impl->recordTouch(phase, point.identifier, x, y);
    }
    // A finger on the canvas is the game's, whole: consumed so the page does
    // not scroll, zoom, select, or -- the one that would double every tap --
    // follow the touch with a synthesised mouse click of its own. The one
    // exception is the tap that is asking for a keyboard, above.
    return leaveToThePage ? EM_FALSE : EM_TRUE;
  }

  /// The keyboard, raised from the one event every mobile browser honours.
  ///
  /// It has to be a real gesture -- a browser will not open its keyboard for a
  /// focus() made from a timer or an animation frame -- and of the events a
  /// gesture produces, `click` is the one that works everywhere. The browser
  /// build focuses its own hidden input from exactly here, which is the
  /// evidence this follows: raising it from `touchstart` instead looks right,
  /// passes a desktop browser's touch emulation, and does nothing at all on a
  /// phone.
  static EM_BOOL onClick(int type, const EmscriptenMouseEvent* event, void* userData) {
    Impl* impl = implOf(userData);
    const float toDesign = impl->fit > 0 ? static_cast<float>(1.0 / impl->fit) : 1.0f;
    const float x = static_cast<float>(event->targetX) * toDesign;
    const float y = static_cast<float>(event->targetY) * toDesign;
    if (impl->onKeyboardField(x, y)) {
      web_soft_keyboard(1);
      impl->softKeyboardUp = true;
    }
    return EM_TRUE;
  }

  /// Whether a point is on one of the text fields the client last painted. See
  /// the note on Window::setSoftKeyboardRegions for why the window is the one
  /// holding this list at all.
  bool onKeyboardField(float x, float y) const {
    for (const WindowRect& region : keyboardRegions) {
      if (x >= region.x && x <= region.x + region.w && y >= region.y &&
          y <= region.y + region.h) {
        return true;
      }
    }
    return false;
  }

  void lowerKeyboard() {
    if (!softKeyboardUp) return;
    web_soft_keyboard(0);
    softKeyboardUp = false;
  }

  bool softKeyboardUp = false;
  /// The contact whose gesture is being left to the page, so that a click
  /// comes out of it.
  std::int64_t passthrough = 0;
  bool passthroughLive = false;
  /// Until when the page's own mouse events are its replay of a touch this
  /// window already mirrored, rather than a mouse. Long enough to cover the
  /// mousedown/mouseup pair a browser synthesises after a touchend, which is
  /// immediate on a canvas with `touch-action: none` and up to a third of a
  /// second without one.
  static constexpr double kGhostWindowMillis = 700.0;
  double ghostUntilMillis = 0;

  static EM_BOOL onWheel(int type, const EmscriptenWheelEvent* event, void* userData) {
    // Sign flipped and normalised to notches: a browser's deltaY grows
    // downward and is reported in pixels, lines or pages depending on the
    // device, while wheelDelta() is the notches-up SDL reports.
    const double perNotch = event->deltaMode == DOM_DELTA_PIXEL ? 100.0 : 1.0;
    implOf(userData)->pendingWheel += static_cast<float>(-event->deltaY / perNotch);
    return EM_TRUE;
  }

  static EM_BOOL onKey(int type, const EmscriptenKeyboardEvent* event, void* userData) {
    Impl* impl = implOf(userData);
    recordModifiers(impl, event->shiftKey, event->ctrlKey, event->altKey, event->metaKey);

    const Key key = fromDomCode(event->code);
    const std::size_t i = static_cast<std::size_t>(key);
    if (type == EMSCRIPTEN_EVENT_KEYUP) {
      if (i < kKeyCount) { impl->down[i] = false; impl->pendingReleased[i] = true; }
      return EM_TRUE;
    }

    if (i < kKeyCount) {
      // `repeat` is the browser auto-repeating a held key: it must feed text
      // fields but must not read as a fresh press to game logic -- except for
      // the two erase keys, which a field has to see repeat on.
      if (!event->repeat) impl->pendingPressed[i] = true;
      else if (key == Key::Backspace || key == Key::Delete) impl->pendingPressed[i] = true;
      impl->down[i] = true;
    }

    // KeyboardEvent.key is the character the layout produced when it is one, and
    // a name like "ArrowLeft" when it is not. One code point -- however many
    // UTF-8 bytes that is -- is the test for the difference.
    if (!event->ctrlKey && !event->metaKey) {
      const char* text = event->key;
      const std::size_t bytes = std::strlen(text);
      const bool oneCodePoint =
          bytes > 0 && bytes <= 4 &&
          (bytes == 1 ? static_cast<unsigned char>(text[0]) >= 0x20 &&
                            static_cast<unsigned char>(text[0]) != 0x7F
                      : (static_cast<unsigned char>(text[0]) & 0x80) != 0);
      if (oneCodePoint) impl->pendingTyped += text;
    }

    // Consumed, so the page does not also scroll on Space, tab away, or open a
    // quick-find on '/'. Ctrl and Cmd combinations are left to the browser:
    // reload and the developer tools are not the game's to swallow.
    return (event->ctrlKey || event->metaKey || key == Key::F5 || key == Key::F12) ? EM_FALSE
                                                                                  : EM_TRUE;
  }

  // The page went away, or the canvas lost the pointer mid-drag. Either way the
  // held state has to be dropped: a button whose release happened somewhere else
  // would otherwise stay down forever.
  static EM_BOOL onBlur(int type, const EmscriptenFocusEvent* event, void* userData) {
    Impl* impl = implOf(userData);
    for (std::size_t i = 0; i < kButtonCount; ++i) {
      if (impl->mouseHeld[i]) { impl->mouseHeld[i] = false; impl->pendingUpEdge[i] = true; }
    }
    for (std::size_t i = 0; i < kKeyCount; ++i) {
      if (impl->down[i]) { impl->down[i] = false; impl->pendingReleased[i] = true; }
    }
    impl->shift = impl->ctrl = impl->alt = false;
    // A contact whose lift happens somewhere the page cannot see is a stick
    // held forever. Dropped whole rather than replayed as an end: whoever owns
    // them reads the live set, and there is nothing left in it.
    impl->touches.clear();
    impl->claimed.clear();
    impl->mirroring = false;
    impl->touchInside = false;
    return EM_TRUE;
  }
#endif

#ifndef __EMSCRIPTEN__
  // Recomputes every derived size from the window's current point size, the
  // display's scale factor and the render-resolution setting, rebuilding the
  // canvas and the upload texture only when the pixel count actually changed.
  //
  // Called every pump rather than only on a resize event: dragging a window
  // between a Retina and a non-Retina monitor changes the drawable size
  // WITHOUT changing the window's point size, and SDL reports that as neither
  // a RESIZED nor a SIZE_CHANGED event on every platform. Two integer queries
  // a frame is a cheaper way to be right than a table of per-platform events.
  void refreshGeometry() {
    if (!window || !renderer) return;

    int points[2] = {0, 0};
    SDL_GetWindowSize(window, &points[0], &points[1]);
    int drawable[2] = {0, 0};
    SDL_GetRendererOutputSize(renderer, &drawable[0], &drawable[1]);
    if (points[0] <= 0 || points[1] <= 0 || drawable[0] <= 0 || drawable[1] <= 0) return;

    pointWidth = points[0];
    pointHeight = points[1];
    devicePixelRatio = static_cast<double>(drawable[0]) / points[0];

    const int wantPixelW = std::max(1, static_cast<int>(std::lround(drawable[0] * renderScale)));
    const int wantPixelH = std::max(1, static_cast<int>(std::lround(drawable[1] * renderScale)));
    if (wantPixelW != pixelWidth || wantPixelH != pixelHeight || !canvas) {
      pixelWidth = wantPixelW;
      pixelHeight = wantPixelH;
      // Canvas cannot be resized in place, so the backing surfaces are
      // rebuilt. This is why callers must not hold canvas() across pump().
      canvas = std::make_unique<Canvas>(pixelWidth, pixelHeight);
      rgba.assign(static_cast<std::size_t>(pixelWidth) * pixelHeight * 4, 0);
      if (texture) SDL_DestroyTexture(texture);
      texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                  SDL_TEXTUREACCESS_STREAMING, pixelWidth, pixelHeight);
    }

    recomputeScales();
  }
#endif

  void clearEdges() {
    pressed.fill(false);
    released.fill(false);
    mouseDownEdge.fill(false);
    mouseUpEdge.fill(false);
    wheel = 0;
    typed.clear();
  }
};

Window::Window() : impl_(std::make_unique<Impl>()) {}
Window::~Window() { close(); }

bool Window::open(int width, int height, const std::string& title, std::string& errorOut) {
#ifdef __EMSCRIPTEN__
  // width/height are the native window's opening size; in a page the element
  // is already laid out by CSS and its size is the answer. The title belongs
  // to the document, which the shell page owns.
  (void)width; (void)height; (void)title;
  close();

  impl_->refreshGeometry();
  if (!impl_->canvas) {
    errorOut = "no element with id '" + impl_->elementId + "' to draw into";
    return false;
  }

  const char* canvasTarget = "#canvas";
  emscripten_set_mousemove_callback(canvasTarget, impl_.get(), EM_FALSE, Impl::onMouse);
  emscripten_set_mousedown_callback(canvasTarget, impl_.get(), EM_FALSE, Impl::onMouse);
  emscripten_set_mouseenter_callback(canvasTarget, impl_.get(), EM_FALSE, Impl::onPointerBoundary);
  emscripten_set_mouseleave_callback(canvasTarget, impl_.get(), EM_FALSE, Impl::onPointerBoundary);
  // Release is watched on the WINDOW, not the canvas: a drag that ends off the
  // element would otherwise never report its mouseup and the button would
  // stay held.
  emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl_.get(), EM_FALSE, Impl::onMouse);
  emscripten_set_wheel_callback(canvasTarget, impl_.get(), EM_FALSE, Impl::onWheel);
  // Down and move on the canvas; up and cancel on the window, for the same
  // reason mouseup is: a finger that leaves the element still has to release
  // whatever it was holding.
  emscripten_set_touchstart_callback(canvasTarget, impl_.get(), EM_FALSE, Impl::onTouch);
  emscripten_set_touchmove_callback(canvasTarget, impl_.get(), EM_FALSE, Impl::onTouch);
  emscripten_set_touchend_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl_.get(), EM_FALSE,
                                   Impl::onTouch);
  emscripten_set_touchcancel_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl_.get(), EM_FALSE,
                                      Impl::onTouch);
  // Not an input path: the only thing this listens for is the moment a
  // browser will let the on-screen keyboard be raised. See Impl::onClick.
  emscripten_set_click_callback(canvasTarget, impl_.get(), EM_FALSE, Impl::onClick);
  emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl_.get(), EM_FALSE, Impl::onKey);
  emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl_.get(), EM_FALSE, Impl::onKey);
  emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl_.get(), EM_FALSE, Impl::onBlur);
  web_clipboard_init();

  impl_->startMillis = emscripten_get_now();
  impl_->lastFrameMillis = impl_->startMillis;
  open_ = true;
  return true;
#else
  close();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) { errorOut = SDL_GetError(); return false; }

  // SDL turns a finger into a mouse of its own by default. This window mirrors
  // touch onto the mouse itself, and has to -- the browser backend has no SDL
  // to do it -- so the platform's copy is switched off rather than left to
  // double every tap.
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

  // ALLOW_HIGHDPI is what makes the drawable bigger than the window on a
  // Retina display. Without it the OS hands the renderer a 1x surface and
  // stretches it, so every pixel drawn is a blurry pair of pixels shown. With
  // it the caller has to deal in three sizes -- see the header -- which is
  // what uiScale() and the design space are for.
  impl_->window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   width, height,
                                   SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                                       SDL_WINDOW_ALLOW_HIGHDPI);
  if (!impl_->window) { errorOut = SDL_GetError(); SDL_Quit(); return false; }

  // The canvas is stretched to the drawable whenever renderScale() < 1, and
  // nearest -- SDL's default -- makes that stretch look like a mistake rather
  // than a setting. The browser build's equivalent is `image-rendering: auto`.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

  impl_->renderer = SDL_CreateRenderer(impl_->window, -1,
                                       SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!impl_->renderer) {
    // Software is slow but correct; a missing GPU path must not be fatal.
    impl_->renderer = SDL_CreateRenderer(impl_->window, -1, SDL_RENDERER_SOFTWARE);
  }
  if (!impl_->renderer) {
    errorOut = SDL_GetError();
    SDL_DestroyWindow(impl_->window); impl_->window = nullptr; SDL_Quit();
    return false;
  }

  // Builds the canvas, the texture and every derived scale from what the OS
  // actually gave us, which on a HiDPI display is not what was asked for.
  impl_->refreshGeometry();
  if (!impl_->canvas || !impl_->texture) {
    errorOut = SDL_GetError();
    close();
    return false;
  }

  impl_->startCounter = SDL_GetPerformanceCounter();
  impl_->lastFrameCounter = impl_->startCounter;

  SDL_StartTextInput();
  open_ = true;
  return true;
#endif
}

void Window::close() {
#ifdef __EMSCRIPTEN__
  if (open_) {
    const char* canvasTarget = "#canvas";
    emscripten_set_mousemove_callback(canvasTarget, nullptr, EM_FALSE, nullptr);
    emscripten_set_mousedown_callback(canvasTarget, nullptr, EM_FALSE, nullptr);
    emscripten_set_mouseenter_callback(canvasTarget, nullptr, EM_FALSE, nullptr);
    emscripten_set_mouseleave_callback(canvasTarget, nullptr, EM_FALSE, nullptr);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, nullptr);
    emscripten_set_wheel_callback(canvasTarget, nullptr, EM_FALSE, nullptr);
    emscripten_set_touchstart_callback(canvasTarget, nullptr, EM_FALSE, nullptr);
    emscripten_set_touchmove_callback(canvasTarget, nullptr, EM_FALSE, nullptr);
    emscripten_set_touchend_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, nullptr);
    emscripten_set_touchcancel_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE,
                                        nullptr);
    emscripten_set_click_callback(canvasTarget, nullptr, EM_FALSE, nullptr);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, nullptr);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, nullptr);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, nullptr);
  }
  impl_->cursorRequested = CursorShape::Arrow;
  impl_->cursorApplied = CursorShape::Count;
  impl_->pendingTouches.clear();
  impl_->touchEvents.clear();
  impl_->touches.clear();
  impl_->claimed.clear();
  impl_->mirroring = false;
  impl_->touchInside = false;
#ifdef __EMSCRIPTEN__
  impl_->pointerInside = false;
#endif
#else
  if (impl_->texture) { SDL_DestroyTexture(impl_->texture); impl_->texture = nullptr; }
  if (impl_->renderer) { SDL_DestroyRenderer(impl_->renderer); impl_->renderer = nullptr; }
  for (SDL_Cursor*& cursor : impl_->cursors) {
    if (cursor) { SDL_FreeCursor(cursor); cursor = nullptr; }
  }
  impl_->cursorRequested = CursorShape::Arrow;
  impl_->cursorApplied = CursorShape::Count;
  if (impl_->window) { SDL_DestroyWindow(impl_->window); impl_->window = nullptr; SDL_Quit(); }
#endif
  impl_->canvas.reset();
  open_ = false;
}

bool Window::pump() {
#ifdef __EMSCRIPTEN__
  if (!open_) return false;
  // No queue to drain: the DOM already delivered every event by callback while
  // the page was between frames. Taking the pending set IS the drain, and it
  // must NOT be preceded by a clear -- see Impl::takePendingEvents.
  impl_->takePendingEvents();
  // After the edge state has been moved across, never before: the mirrored
  // contact writes a mouse edge, and takePendingEvents ASSIGNS that array
  // rather than merging into it.
  impl_->drainTouches();
  // After that, so this frame's pointer positions are converted with the
  // geometry this frame will be drawn with, as the native path does too.
  impl_->refreshGeometry();
  return !impl_->shouldClose;
#else
  if (!open_) return false;
  impl_->clearEdges();
  // Before the events are read, so this frame's mouse positions are converted
  // with the geometry this frame will be drawn with.
  impl_->refreshGeometry();

  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_QUIT:
        impl_->shouldClose = true;
        break;

      case SDL_KEYDOWN: {
        // repeat != 0 is the OS auto-repeating a held key: it must feed text
        // fields but must not read as a fresh press to game logic.
        const Key k = fromScancode(event.key.keysym.scancode);
        const std::size_t i = static_cast<std::size_t>(k);
        if (i < kKeyCount) {
          if (!event.key.repeat) impl_->pressed[i] = true;
          impl_->down[i] = true;
        }
        // The two erase keys are the ones a text field must see repeat on:
        // holding either has to keep deleting, as it does in the browser.
        if (event.key.repeat && (k == Key::Backspace || k == Key::Delete)) {
          impl_->pressed[i] = true;
        }
        break;
      }

      case SDL_KEYUP: {
        const std::size_t i = static_cast<std::size_t>(fromScancode(event.key.keysym.scancode));
        if (i < kKeyCount) { impl_->down[i] = false; impl_->released[i] = true; }
        break;
      }

      case SDL_TEXTINPUT:
        impl_->typed += event.text.text;
        break;

      case SDL_MOUSEMOTION: {
        // Event coordinates are in points. The caller draws and hit-tests in
        // design units, so points are all that has to be divided out -- the
        // device pixel ratio and the render scale are the base transform's
        // business, not the pointer's.
        const float toDesign = impl_->fit > 0 ? static_cast<float>(1.0 / impl_->fit) : 1.0f;
        impl_->mouseX = event.motion.x * toDesign;
        impl_->mouseY = event.motion.y * toDesign;
        break;
      }

      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        std::size_t index = kButtonCount;
        if (event.button.button == SDL_BUTTON_LEFT) index = static_cast<std::size_t>(MouseButton::Left);
        else if (event.button.button == SDL_BUTTON_MIDDLE) index = static_cast<std::size_t>(MouseButton::Middle);
        else if (event.button.button == SDL_BUTTON_RIGHT) index = static_cast<std::size_t>(MouseButton::Right);
        if (index < kButtonCount) {
          const bool downNow = event.type == SDL_MOUSEBUTTONDOWN;
          impl_->mouseHeld[index] = downNow;
          (downNow ? impl_->mouseDownEdge : impl_->mouseUpEdge)[index] = true;
        }
        break;
      }

      case SDL_MOUSEWHEEL:
        impl_->wheel += event.wheel.preciseY != 0 ? event.wheel.preciseY
                                                  : static_cast<float>(event.wheel.y);
        break;

      case SDL_FINGERDOWN:
      case SDL_FINGERMOTION:
      case SDL_FINGERUP: {
        // SDL reports a finger as a FRACTION of the window, not in points, so
        // this multiplies by the point size before dividing by the same fit
        // the mouse is divided by.
        const double toDesign = impl_->fit > 0 ? 1.0 / impl_->fit : 1.0;
        const float x = static_cast<float>(event.tfinger.x * impl_->pointWidth * toDesign);
        const float y = static_cast<float>(event.tfinger.y * impl_->pointHeight * toDesign);
        const TouchPhase phase = event.type == SDL_FINGERDOWN  ? TouchPhase::Began
                                 : event.type == SDL_FINGERUP ? TouchPhase::Ended
                                                              : TouchPhase::Moved;
        impl_->recordTouch(phase, static_cast<std::int64_t>(event.tfinger.fingerId), x, y);
        break;
      }

      default:
        break;
    }
  }

  // After the queue, for the same reason the web path drains after taking its
  // pending set: the mirror writes this frame's mouse edges.
  impl_->drainTouches();

  const SDL_Keymod mods = SDL_GetModState();
  impl_->shift = (mods & KMOD_SHIFT) != 0;
  impl_->ctrl = (mods & (KMOD_CTRL | KMOD_GUI)) != 0;
  impl_->alt = (mods & KMOD_ALT) != 0;

  return !impl_->shouldClose;
#endif
}

Canvas& Window::canvas() { return *impl_->canvas; }
int Window::width() const { return impl_->viewWidth; }
int Window::height() const { return impl_->viewHeight; }
int Window::pixelWidth() const { return impl_->pixelWidth; }
int Window::pixelHeight() const { return impl_->pixelHeight; }
double Window::uiScale() const { return impl_->uiScale; }
double Window::devicePixelRatio() const { return impl_->devicePixelRatio; }
double Window::renderScale() const { return impl_->renderScale; }

void Window::setDesignSize(int width, int height) {
  impl_->designWidth = std::max(0, width);
  impl_->designHeight = std::max(0, height);
  impl_->refreshGeometry();
}

void Window::setRenderScale(double scale) {
  // Clamped rather than rejected: this comes straight off a settings slider,
  // and a canvas of zero pixels is not a preference anyone can hold.
  const double clamped = std::min(1.0, std::max(0.25, scale));
  if (clamped == impl_->renderScale) return;
  impl_->renderScale = clamped;
  impl_->refreshGeometry();
}

void Window::present() {
#ifdef __EMSCRIPTEN__
  if (!open_) return;
  // There is nothing to upload: the frame was drawn INTO the page's canvas
  // element, through its own 2D context, so it is already on screen. The
  // cursor is the only thing this still owes the host, on the same terms as
  // the native path -- last word of the frame wins.
  impl_->applyCursor();
#else
  if (!open_) return;
  // Here rather than in setCursorShape: the shape is asserted by whatever is
  // under the pointer during the frame, and only the last word of the frame
  // should reach the OS.
  impl_->applyCursor();
  if (!impl_->canvas || !impl_->texture) return;

  // getImageData is the Canvas API's only pixel accessor, and it already
  // composites onto opaque; taking the whole surface once per frame is one
  // copy, which the upload would cost anyway. It is handed straight to
  // SDL_UpdateTexture -- copying it into impl_->rgba first was a second pass
  // over three and a half megabytes for nothing. The member buffer stays as
  // the fallback for a size the canvas could not satisfy.
  const std::vector<std::uint8_t> pixels =
      impl_->canvas->getImageData(0, 0, impl_->pixelWidth, impl_->pixelHeight);
  const std::uint8_t* upload = impl_->rgba.data();
  if (pixels.size() == impl_->rgba.size()) upload = pixels.data();

  SDL_UpdateTexture(impl_->texture, nullptr, upload, impl_->pixelWidth * 4);
  SDL_RenderClear(impl_->renderer);
  SDL_RenderCopy(impl_->renderer, impl_->texture, nullptr, nullptr);
  SDL_RenderPresent(impl_->renderer);
#endif
}

double Window::frameDelay(double targetFps) {
#ifdef __EMSCRIPTEN__
  // The rate is requestAnimationFrame's to set and there is no thread to
  // sleep, so this only measures. Clamped like the native path: a backgrounded
  // tab stops being called and comes back with a dt of minutes.
  (void)targetFps;
  const double now = emscripten_get_now();
  const double dt = (now - impl_->lastFrameMillis) / 1000.0;
  impl_->lastFrameMillis = now;
  return std::min(std::max(dt, 0.0), 0.25);
#else
  const Uint64 frequency = SDL_GetPerformanceFrequency();
  const Uint64 now = SDL_GetPerformanceCounter();
  const double elapsed = static_cast<double>(now - impl_->lastFrameCounter) / frequency;

  if (targetFps > 0) {
    const double target = 1.0 / targetFps;
    if (elapsed < target) {
      const double remaining = target - elapsed;
      // Sleep the bulk and spin the last millisecond: SDL_Delay's resolution
      // is coarse enough that sleeping the whole remainder overshoots and
      // makes the frame rate visibly uneven.
      if (remaining > 0.002) SDL_Delay(static_cast<Uint32>((remaining - 0.001) * 1000.0));
      while (static_cast<double>(SDL_GetPerformanceCounter() - impl_->lastFrameCounter) / frequency < target) {}
    }
  }

  const Uint64 frameEnd = SDL_GetPerformanceCounter();
  const double dt = static_cast<double>(frameEnd - impl_->lastFrameCounter) / frequency;
  impl_->lastFrameCounter = frameEnd;
  // Clamp: a breakpoint or a paused window otherwise returns a dt of minutes,
  // which teleports everything the caller integrates.
  return std::min(dt, 0.25);
#endif
}

double Window::timeSeconds() const {
#ifdef __EMSCRIPTEN__
  return (emscripten_get_now() - impl_->startMillis) / 1000.0;
#else
  return static_cast<double>(SDL_GetPerformanceCounter() - impl_->startCounter) /
         SDL_GetPerformanceFrequency();
#endif
}

bool Window::keyDown(Key k) const {
  const std::size_t i = static_cast<std::size_t>(k);
  return i < kKeyCount && impl_->down[i];
}
bool Window::keyPressed(Key k) const {
  const std::size_t i = static_cast<std::size_t>(k);
  return i < kKeyCount && impl_->pressed[i];
}
bool Window::keyReleased(Key k) const {
  const std::size_t i = static_cast<std::size_t>(k);
  return i < kKeyCount && impl_->released[i];
}
bool Window::mouseDown(MouseButton b) const {
  const std::size_t i = static_cast<std::size_t>(b);
  return i < kButtonCount && impl_->mouseHeld[i];
}
bool Window::mousePressed(MouseButton b) const {
  const std::size_t i = static_cast<std::size_t>(b);
  return i < kButtonCount && impl_->mouseDownEdge[i];
}
bool Window::mouseReleased(MouseButton b) const {
  const std::size_t i = static_cast<std::size_t>(b);
  return i < kButtonCount && impl_->mouseUpEdge[i];
}
float Window::mouseX() const { return impl_->mouseX; }
float Window::mouseY() const { return impl_->mouseY; }
bool Window::pointerInside() const {
  // A mirrored finger counts: it is where the pointer is, and it is the only
  // pointer a touch-only device has. It stays counted after the lift, as a
  // mouse left where it was released would.
  if (impl_->touchInside) return true;
#ifdef __EMSCRIPTEN__
  return impl_->pointerInside;
#else
  return SDL_GetMouseFocus() != nullptr;
#endif
}
float Window::wheelDelta() const { return impl_->wheel; }

const std::vector<TouchEvent>& Window::touchEvents() const { return impl_->touchEvents; }
const std::vector<TouchPoint>& Window::touches() const { return impl_->touches; }
bool Window::touchSeen() const { return impl_->touchSeen; }

bool Window::coarsePointer() const {
#ifdef __EMSCRIPTEN__
  return web_coarse_pointer() != 0;
#else
  // A desktop window is driven by a mouse. A touchscreen laptop can still
  // touch it -- touchSeen() is what answers that -- but "the primary pointing
  // device is a finger" is a thing only a page can be asked.
  return false;
#endif
}

void Window::setTouchClaimHandler(TouchClaimHandler handler) {
  impl_->touchClaim = std::move(handler);
}

void Window::setSoftKeyboardRegions(std::vector<WindowRect> regions) {
#ifdef __EMSCRIPTEN__
  impl_->keyboardRegions = std::move(regions);
#else
  (void)regions;
#endif
}

void Window::dismissSoftKeyboard() {
#ifdef __EMSCRIPTEN__
  if (!impl_->softKeyboardUp) return;
  // Never while a finger is still down, and never while the click that a
  // field tap is waiting for has yet to arrive. The caller polls this every
  // frame on "is anything holding the caret", and a tap does not focus the
  // field it landed on until it is released -- so for those few frames the
  // answer is honestly "nothing", and acting on it would take away the
  // keyboard that same tap is in the middle of asking for.
  if (!impl_->touches.empty()) return;
  if (emscripten_get_now() < impl_->ghostUntilMillis) return;
  impl_->lowerKeyboard();
#endif
}
const std::string& Window::typedText() const { return impl_->typed; }
bool Window::shiftHeld() const { return impl_->shift; }
bool Window::ctrlHeld() const { return impl_->ctrl; }
bool Window::altHeld() const { return impl_->alt; }

std::string Window::clipboardText() const {
#ifdef __EMSCRIPTEN__
  // What the user last pasted into the page or copied out of it. A page may
  // not read the system clipboard unprompted, so this is as much as there is
  // -- which is why a field asks `pastedText()` instead: that reports it only
  // on the frame a paste event actually delivered it.
  // Bounded: a clipboard is whatever the user last copied anywhere, and a
  // chat field has no use for a megabyte of it.
  char buffer[4096] = {0};
  web_clipboard_get(buffer, static_cast<int>(sizeof buffer));
  return std::string(buffer);
#else
  if (!SDL_HasClipboardText()) return {};
  // SDL hands over a buffer it allocated; it is the caller's to free, and
  // leaking one per paste would be a slow leak in a long session.
  char* text = SDL_GetClipboardText();
  if (!text) return {};
  std::string out(text);
  SDL_free(text);
  return out;
#endif
}

std::string Window::pastedText() const {
#ifdef __EMSCRIPTEN__
  if (!impl_->pasted) return {};
  return clipboardText();
#else
  // The desktop has no paste event, so the shortcut is the event. Cmd folds
  // into ctrl through KMOD_GUI above, which is what makes the Mac combination
  // land here as well.
  const std::size_t v = static_cast<std::size_t>(Key::V);
  if (!impl_->ctrl || v >= kKeyCount || !impl_->pressed[v]) return {};
  return clipboardText();
#endif
}

void Window::setClipboardText(const std::string& text) {
#ifdef __EMSCRIPTEN__
  web_clipboard_set(text.c_str());
#else
  SDL_SetClipboardText(text.c_str());
#endif
}

void Window::setCursorVisible(bool visible) {
#ifdef __EMSCRIPTEN__
  web_set_cursor(impl_->elementId.c_str(), visible ? "default" : "none");
  // The shape cache no longer describes what is showing.
  impl_->cursorApplied = CursorShape::Count;
#else
  SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
#endif
}

void Window::setCursorShape(CursorShape shape) {
  if (shape < CursorShape::Count) impl_->cursorRequested = shape;
}
