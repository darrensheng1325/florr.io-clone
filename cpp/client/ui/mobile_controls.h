#pragma once
// The touch HUD: a virtual stick for movement and aim, and the two buttons
// that stand in for the keys a phone does not have.
//
// A port of the browser build's src/graphics/mobile-controls.ts, control for
// control, with one thing done differently and one thing added.
//
// Differently: the sizes. The reference is written in CSS pixels on a canvas
// the size of the viewport, so its 55px stick radius is a thumb on a phone and
// a dot on a desktop monitor. This client draws into a FIXED 1920x1080 design
// space (see Window's header), where a size is a fraction of the screen rather
// than a length -- so every measurement below is the reference's, multiplied
// by the design units a CSS pixel is worth on a phone-sized viewport. The
// controls then land at the same fraction of the screen a phone player sees
// there, which is what the reference's numbers were chosen for.
//
// Added: aim. The reference aims at the mouse even on a phone, where "the
// mouse" is wherever the last tap happened to land. Here a deflected stick
// aims as well as moves, which is the only thing a thumb can say.

#include <cstdint>
#include <vector>

#include "canvas.h"
#include "window.h"

#include "shared/core/types.h"

namespace flix::ui {

/// Design units per CSS pixel on a phone-sized viewport.
///
/// A phone in landscape is about 800 CSS pixels wide, and the design space is
/// 1920 units across whatever the window's real width is -- so a control that
/// should cover a phone's thumb has to be this much larger here than the
/// number the reference writes for the same control.
inline constexpr double kTouchScale = 2.4;

class MobileControls {
public:
    /// Anchors the three controls for a viewport of this size.
    ///
    /// `loadoutBarHeight` is what the loadout bar occupies from the bottom
    /// edge: the bar is centred and reaches nearly to the edge on its own, so
    /// everything here hangs ABOVE it rather than below.
    void layout(int viewWidth, int viewHeight, double loadoutBarHeight);

    /// Whether a finger landing here belongs to these controls.
    ///
    /// Pure, and deliberately so: it is the answer Window's claim handler
    /// needs the instant a contact arrives, and the state it would otherwise
    /// change is changed by update() on the same event a moment later.
    bool hits(double x, double y) const;

    /// One frame of the touch stream. Contacts that landed on a control are
    /// bound to it here and followed until they lift.
    void update(const std::vector<TouchEvent>& events);

    /// Drops every contact -- what a screen change, a panel opening over the
    /// controls or the setting being switched off has to do, or a stick that
    /// was deflected when it vanished goes on steering forever.
    void reset();

    /// The stick's direction and how far it is pushed, 0 to 1, or nothing at
    /// all when it is centred or untouched.
    struct Stick {
        Vec2 direction{0, 0};
        double magnitude = 0;
    };
    bool stick(Stick& out) const;

    /// Where each control ended up. Published for the sake of anything that
    /// has to reason about the space they occupy -- and for the tests, which
    /// need a point on a control to press without knowing its size.
    Vec2 stickCentre() const { return stickCentre_; }
    Vec2 attackCentre() const { return attackCentre_; }
    Vec2 retractCentre() const { return retractCentre_; }

    bool attackPressed() const { return attack_.held; }
    bool retractPressed() const { return retract_.held; }

    void draw(Canvas&) const;

private:
    /// The contact bound to one control, if any. An id and a flag rather than
    /// a sentinel id: nothing promises a platform's finger ids avoid any
    /// particular value, and "no finger" is not a finger number.
    struct Contact {
        std::int64_t id = 0;
        bool held = false;
        bool is(std::int64_t other) const { return held && id == other; }
    };

    /// Where the finger holding the stick is, relative to the stick's centre,
    /// clamped to the base. The knob is drawn at it and the direction is read
    /// off it, exactly as the reference's `knobOffset` is.
    /// False until layout() has run. A control that has never been placed sits
    /// at the origin, and would otherwise claim every touch in the top-left
    /// corner of the first frame it is asked about.
    bool placed_ = false;
    /// Everything shrinks by this together, and it is 1 on any viewport wide
    /// enough to lay the three controls out side by side -- which is every
    /// landscape one. See layout().
    double scale_ = 1.0;
    Vec2 knob_{0, 0};
    Vec2 stickCentre_{0, 0};
    Vec2 attackCentre_{0, 0};
    Vec2 retractCentre_{0, 0};

    // Three fingers can be down at once, and only their ids tell the streams
    // apart -- a position cannot, once two of them are moving.
    Contact stick_;
    Contact attack_;
    Contact retract_;

    void moveKnob(double x, double y);
};

} // namespace flix::ui
