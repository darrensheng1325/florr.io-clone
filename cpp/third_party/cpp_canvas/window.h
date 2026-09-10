#pragma once

// An interactive window with input, for applications that own their own frame
// loop.
//
// Canvas::showWindow() takes the loop over and reports nothing but "closed",
// which is enough for a demo and not enough for anything interactive. A Window
// inverts that: the caller drives the loop, pumps events when it likes, draws
// into the window's Canvas, and presents when it is ready.
//
// SDL is an implementation detail and does not appear in this header.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "canvas.h"

// Physical keys, named independently of any backend's numbering.
enum class Key : std::uint16_t {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Space, Enter, Escape, Backspace, Tab,
    Left, Right, Up, Down,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt,
    Minus, Equals, Comma, Period, Slash, Backslash, Semicolon, Apostrophe,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    // APPEND ONLY, never renumber: a user's hotkeys are persisted by value, so
    // inserting a name mid-list silently rebinds every key after it.
    Delete, Home, End,
    Count
};

enum class MouseButton : std::uint8_t { Left = 0, Middle, Right, Count };

// One finger on a touchscreen, in the same design units the mouse arrives in.
//
// `id` is the contact's, stable from the moment it lands until it lifts, and
// is what a caller matches a move against -- two fingers on the screen produce
// two interleaved streams and the position alone cannot tell them apart.
struct TouchPoint {
    std::int64_t id = 0;
    float x = 0;
    float y = 0;
};

enum class TouchPhase : std::uint8_t { Began, Moved, Ended };

// A rectangle in design units. The window layer has no geometry type of its
// own and must not borrow the application's, so it carries this one.
struct WindowRect {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

struct TouchEvent {
    TouchPhase phase = TouchPhase::Began;
    TouchPoint point;
};

// The pointer's shape. `Text` is the I-beam a text field asks for.
enum class CursorShape : std::uint8_t { Arrow = 0, Hand, Text, Count };

class Window {
public:
    Window();
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Creates the window. Returns false with `errorOut` set on failure.
    bool open(int width, int height, const std::string& title, std::string& errorOut);
    void close();
    bool isOpen() const { return open_; }

    // Drains OS events into the input state below. Returns false once the user
    // has asked to close the window.
    //
    // Edge state (pressed/released this frame, wheel, typed text) is cleared at
    // the START of each pump, so it stays readable for the whole frame that
    // follows -- the alternative, clearing after the frame, loses events that
    // arrive while the frame is still being drawn.
    bool pump();

    // The canvas the frame is drawn into. Its backing store is the window's
    // drawable area (times renderScale()); its user-space size -- what
    // Canvas::width()/height() report -- is the design space below. Recreated
    // on resize, so do not hold the reference across a pump().
    Canvas& canvas();

    // Uploads the canvas and shows it.
    void present();

    // -- scaling -------------------------------------------------------------
    //
    // Three sizes exist and only one of them is the drawing code's business.
    //
    //   points   what the OS calls the window's size. Half the pixels on a
    //            Retina display, and never used for layout.
    //   pixels   the backing store: points x devicePixelRatio x renderScale.
    //            Presentation only.
    //   design   the coordinate space every draw call is written in, fixed by
    //            setDesignSize(). This is what width()/height() report and
    //            what the mouse arrives in.
    //
    // The design space is what keeps the game from zooming out, and the design
    // size is a MAXIMUM. A window is never letterboxed: it is covered, with
    // the LONG axis setting the scale, so a window at the design aspect ratio
    // reports exactly the design size however many pixels or points it has,
    // and one at any other aspect ratio reports FEWER units on its short axis
    // rather than more on its long one.
    //
    // Reporting more would be the bug. Covering the other way -- letting the
    // short axis set the scale so nothing is ever cropped -- means a window
    // dragged narrow or wide shrinks everything and reveals the world it made
    // room for, which is a zoom control made out of the window's edge.

    // Declares the design space. Until it is called the window reports points,
    // which is what a plain tool wants.
    void setDesignSize(int width, int height);

    // Design units on each axis. At the design aspect ratio these are the
    // design size exactly, at any window size and on any display.
    int width() const;
    int height() const;

    // Canvas pixels per design unit. The caller MUST apply this as the base
    // transform of every frame -- scale(uiScale(), uiScale()) -- or nothing
    // will line up with the sizes reported above.
    double uiScale() const;

    // The display's pixels per point: 2 on a Retina display, 1 elsewhere.
    // Changes when the window is dragged onto another monitor.
    double devicePixelRatio() const;

    // The fraction of the display's native resolution the canvas is rendered
    // at, 0.25 to 1. Below 1 the canvas is smaller than the drawable and the
    // host stretches it on the way to the screen. This buys CPU rasterizer
    // time on desktop and native Canvas2D fill-rate in a browser. Only the
    // sharpness moves: uiScale() absorbs the change, so nothing shifts.
    void setRenderScale(double scale);
    double renderScale() const;

    // The backing store's size, for a caller that has to reason in real
    // pixels. Layout never does.
    int pixelWidth() const;
    int pixelHeight() const;

    // Waits out the remainder of a frame at `targetFps`. Returns the seconds
    // the last frame actually took, so the caller can integrate with real dt.
    double frameDelay(double targetFps);

    double timeSeconds() const;

    // -- input ---------------------------------------------------------------

    bool keyDown(Key k) const;
    bool keyPressed(Key k) const;    ///< went down this frame
    bool keyReleased(Key k) const;   ///< came up this frame

    bool mouseDown(MouseButton b) const;
    bool mousePressed(MouseButton b) const;
    bool mouseReleased(MouseButton b) const;

    float mouseX() const;
    float mouseY() const;
    /// Whether the pointer is currently over the window/canvas. Browser
    /// builds track DOM enter/leave events; desktop builds ask SDL.
    bool pointerInside() const;
    float wheelDelta() const;

    // -- touch ---------------------------------------------------------------
    //
    // A touchscreen is not a mouse with one button, but almost everything in a
    // canvas UI wants it to be: every panel, button and drag in this client is
    // written against mousePressed/mouseDown. So one contact -- the first one
    // down -- is MIRRORED onto the left mouse button, and the rest of the
    // client never learns the difference.
    //
    // The exception is whatever wants the raw stream: an on-screen stick has
    // to track a finger while a second one holds a button, which one mirrored
    // pointer cannot express. Such a control claims its contacts through
    // setTouchClaimHandler below, and a claimed contact is kept out of the
    // mirror entirely -- otherwise dragging the stick would also drag the
    // mouse across the HUD behind it.

    /// Every touch that began, moved or ended since the last pump, in arrival
    /// order. Cleared at each pump, like the other edge state.
    const std::vector<TouchEvent>& touchEvents() const;

    /// The contacts currently down, claimed and mirrored alike.
    const std::vector<TouchPoint>& touches() const;

    /// Whether a finger has ever touched this window. The honest test for "is
    /// this a touch device", where `coarsePointer()` is only the browser's
    /// guess -- but it answers nothing until the player has touched something.
    bool touchSeen() const;

    /// Whether the primary pointing device is a coarse one -- a finger rather
    /// than a mouse. The page's `(pointer: coarse)` media query in a browser,
    /// and false in a desktop window, which is what it is there.
    bool coarsePointer() const;

    /// Consulted the instant a finger lands, with that contact's position in
    /// design units. Returning true keeps it out of the mouse mirror, because
    /// something else owns it.
    ///
    /// It must be a PURE hit test: it is asked once per contact, and the
    /// control that answers still has to act on the event stream itself.
    using TouchClaimHandler = std::function<bool(const TouchPoint&)>;
    void setTouchClaimHandler(TouchClaimHandler handler);

    // -- the on-screen keyboard ----------------------------------------------
    //
    // A canvas cannot take keyboard focus, so a phone browser never opens its
    // keyboard for one however many text fields are painted on it. The answer
    // is a real (invisible) <input> that IS focusable: focusing it summons the
    // keyboard, and the keystrokes it produces bubble to the page and reach
    // this window's own key handler exactly as a physical keyboard's do.
    //
    // The focus has to happen inside the touch that asked for it -- a browser
    // refuses to raise the keyboard from a timer or an animation frame -- so
    // the decision cannot wait for the frame that would notice the field was
    // focused. The caller instead publishes where its fields ARE, and the
    // touch handler answers for itself, in the gesture, before the frame runs.
    //
    // Native builds have a real keyboard and ignore all of this.

    /// Where this frame's text fields are, in design units. A touch landing in
    /// one raises the on-screen keyboard; a touch landing outside all of them
    /// dismisses it.
    void setSoftKeyboardRegions(std::vector<WindowRect> regions);

    /// Takes the keyboard away, for a field that was closed by something other
    /// than a touch -- Escape, a send, a panel closing under it.
    void dismissSoftKeyboard();

    /// UTF-8 typed this frame, for text fields. Distinct from keyPressed:
    /// this is what the keyboard layout produced, not which key was struck.
    const std::string& typedText() const;

    /// True while a modifier is held, for shortcuts.
    bool shiftHeld() const;
    bool ctrlHeld() const;
    bool altHeld() const;

    /// The system clipboard's text, for a field's paste. Empty when the
    /// clipboard holds no text at all; the caller still has to filter it,
    /// since what arrives is whatever the user last copied ANYWHERE.
    std::string clipboardText() const;
    void setClipboardText(const std::string& text);

    /// The text a paste asked for THIS frame, empty when nothing was pasted.
    ///
    /// Not the same question as `clipboardText()`, and deliberately not the
    /// same mechanism on the two backends. The desktop reads the clipboard on
    /// Ctrl/Cmd+V, since that keystroke IS the event there. A page may not read
    /// the clipboard on a keystroke without a permission prompt, so the web
    /// build reports what the page's own `paste` event delivered instead --
    /// which needs no prompt and also picks up a paste from the context menu
    /// or a touch keyboard, neither of which is a Ctrl+V at all.
    std::string pastedText() const;

    void setCursorVisible(bool visible);

    // The pointer's shape over this window. Records a request; the shape
    // reaches the OS at the next present(), and only when it differs from the
    // one already showing. That is what lets a frame reset the shape to Arrow
    // and let whatever is under the pointer overrule it, without the cursor
    // flickering between the two.
    void setCursorShape(CursorShape shape);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool open_ = false;
};
