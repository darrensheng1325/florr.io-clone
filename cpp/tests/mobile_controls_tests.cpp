#include "test.h"

#include "client/ui/menus.h"
#include "client/ui/mobile_controls.h"

#include <cmath>
#include <vector>

using namespace flix;
using namespace flix::ui;

// The touch HUD: which finger owns which control, what the stick reports, and
// where the three of them land.
//
// None of this needs a window. The controls take a stream of contacts and a
// viewport and answer in world terms -- which is the whole reason they are a
// class and not three blocks inside App: a stick that binds the wrong finger
// steers a flower that nobody is touching, and that is not something a
// screenshot would show.

namespace {

// A phone in landscape, as the design space sees it: the long axis fixes the
// scale, so the viewport is the full design width and rather less than its
// height. See Window's scaling note.
constexpr int kPhoneWidth = 1920;
constexpr int kPhoneHeight = 887;

TouchEvent began(std::int64_t id, double x, double y) {
    return {TouchPhase::Began, TouchPoint{id, static_cast<float>(x), static_cast<float>(y)}};
}
TouchEvent moved(std::int64_t id, double x, double y) {
    return {TouchPhase::Moved, TouchPoint{id, static_cast<float>(x), static_cast<float>(y)}};
}
TouchEvent ended(std::int64_t id, double x, double y) {
    return {TouchPhase::Ended, TouchPoint{id, static_cast<float>(x), static_cast<float>(y)}};
}

/// A laid-out set of controls, plus where each of the three of them is. The
/// centres are not published -- nothing outside the class needs them -- so
/// they are found the way a finger does, by asking what answers.
struct Fixture {
    MobileControls controls;

    Fixture() { controls.layout(kPhoneWidth, kPhoneHeight, inGameLoadoutBarHeight()); }

    void feed(std::vector<TouchEvent> events) { controls.update(events); }
};

/// Every point the controls answer for, on a coarse grid. Used by the layout
/// invariant below, which is about the region rather than the centres.
std::vector<Vec2> hitPoints(const MobileControls& controls) {
    std::vector<Vec2> points;
    for (double y = 0; y < kPhoneHeight; y += 2.0) {
        for (double x = 0; x < kPhoneWidth; x += 2.0) {
            if (controls.hits(x, y)) points.push_back({x, y});
        }
    }
    return points;
}

} // namespace

TEST(controls_that_have_never_been_laid_out_claim_nothing) {
    // The claim handler is asked about a contact before the first frame has
    // placed anything. Unplaced controls sit at the origin, and one that
    // answered from there would swallow every touch in the corner.
    const MobileControls fresh;
    CHECK(!fresh.hits(0, 0));
    CHECK(!fresh.hits(10, 10));
}

TEST(the_stick_reports_the_direction_it_is_pushed_and_how_far) {
    Fixture f;
    const Vec2 centre = f.controls.stickCentre();
    CHECK(centre.x > 0);

    f.feed({began(1, centre.x, centre.y)});
    MobileControls::Stick stick;
    // Dead centre is not a direction.
    CHECK(!f.controls.stick(stick));

    // Straight right, half the base radius. The radius is not published, so
    // the push is measured by what comes back rather than asserted against a
    // constant: a half-deflection is what "less than full speed" means.
    f.feed({moved(1, centre.x + 40.0, centre.y)});
    CHECK(f.controls.stick(stick));
    CHECK_NEAR(stick.direction.x, 1.0, 1e-5);
    CHECK_NEAR(stick.direction.y, 0.0, 1e-5);
    CHECK(stick.magnitude > 0.0);
    CHECK(stick.magnitude < 1.0);

    // Far outside the base: the direction goes on turning and the speed stops
    // at full, which is what a thumb that overshoots has to mean.
    f.feed({moved(1, centre.x, centre.y - 5000.0)});
    CHECK(f.controls.stick(stick));
    CHECK_NEAR(stick.direction.x, 0.0, 1e-5);
    CHECK_NEAR(stick.direction.y, -1.0, 1e-5);
    CHECK_NEAR(stick.magnitude, 1.0, 1e-5);
}

TEST(a_nudge_inside_the_dead_zone_is_not_a_push) {
    Fixture f;
    const Vec2 centre = f.controls.stickCentre();
    f.feed({began(1, centre.x, centre.y), moved(1, centre.x + 2.0, centre.y + 2.0)});
    MobileControls::Stick stick;
    CHECK(!f.controls.stick(stick));
}

TEST(lifting_the_stick_recentres_it) {
    Fixture f;
    const Vec2 centre = f.controls.stickCentre();
    f.feed({began(1, centre.x, centre.y), moved(1, centre.x + 60.0, centre.y)});
    MobileControls::Stick stick;
    CHECK(f.controls.stick(stick));
    f.feed({ended(1, centre.x + 60.0, centre.y)});
    // Not merely "no longer held": a stick that kept its deflection would
    // steer again the moment the next finger landed on it.
    CHECK(!f.controls.stick(stick));
    f.feed({began(2, centre.x, centre.y)});
    CHECK(!f.controls.stick(stick));
}

TEST(the_stick_follows_only_the_finger_that_took_it) {
    Fixture f;
    const Vec2 centre = f.controls.stickCentre();
    f.feed({began(1, centre.x, centre.y), moved(1, centre.x + 60.0, centre.y)});

    // Another finger dragging across the screen -- over the stick, even --
    // says nothing about where the stick is pushed.
    f.feed({moved(2, centre.x, centre.y - 60.0)});
    MobileControls::Stick stick;
    CHECK(f.controls.stick(stick));
    CHECK_NEAR(stick.direction.x, 1.0, 1e-5);

    // And its lift releases nothing.
    f.feed({ended(2, centre.x, centre.y)});
    CHECK(f.controls.stick(stick));
}

TEST(the_attack_button_is_held_for_as_long_as_the_finger_is_down) {
    Fixture f;
    const Vec2 centre = f.controls.attackCentre();
    CHECK(!f.controls.attackPressed());

    f.feed({began(7, centre.x, centre.y)});
    CHECK(f.controls.attackPressed());
    CHECK(!f.controls.retractPressed());

    // A thumb resting on a fire button drifts. It is still holding it.
    f.feed({moved(7, centre.x + 200.0, centre.y + 200.0)});
    CHECK(f.controls.attackPressed());

    f.feed({ended(7, centre.x + 200.0, centre.y + 200.0)});
    CHECK(!f.controls.attackPressed());
}

TEST(the_stick_and_a_button_are_held_by_two_fingers_at_once) {
    // The whole reason the raw touch stream exists rather than the mirrored
    // pointer: one mouse cannot be steering and firing at the same time.
    Fixture f;
    const Vec2 stickAt = f.controls.stickCentre();
    const Vec2 attackAt = f.controls.attackCentre();

    f.feed({began(1, stickAt.x, stickAt.y), began(2, attackAt.x, attackAt.y),
            moved(1, stickAt.x - 60.0, stickAt.y)});

    MobileControls::Stick stick;
    CHECK(f.controls.stick(stick));
    CHECK_NEAR(stick.direction.x, -1.0, 1e-5);
    CHECK(f.controls.attackPressed());
}

TEST(a_second_finger_on_a_held_control_is_not_a_second_press) {
    Fixture f;
    const Vec2 attackAt = f.controls.attackCentre();
    f.feed({began(1, attackAt.x, attackAt.y), began(2, attackAt.x, attackAt.y)});
    CHECK(f.controls.attackPressed());

    // The first finger's lift releases the button, because it is the one that
    // took it. If the second had also been bound to it, this would still read
    // as pressed and the petals would stay out forever.
    f.feed({ended(1, attackAt.x, attackAt.y)});
    CHECK(!f.controls.attackPressed());
}

TEST(reset_drops_every_contact) {
    Fixture f;
    const Vec2 stickAt = f.controls.stickCentre();
    const Vec2 attackAt = f.controls.attackCentre();
    f.feed({began(1, stickAt.x, stickAt.y), moved(1, stickAt.x + 60.0, stickAt.y),
            began(2, attackAt.x, attackAt.y)});

    f.controls.reset();
    MobileControls::Stick stick;
    CHECK(!f.controls.stick(stick));
    CHECK(!f.controls.attackPressed());
    CHECK(!f.controls.retractPressed());
}

TEST(the_controls_sit_clear_of_the_hud_they_share_the_bottom_of_the_screen_with) {
    // The three things already anchored to the bottom of a game: the loadout
    // bar across the middle, the icon column at x <= 73, and the chat column
    // from x = 115 to 385 in the last 195 units of height. A control that
    // overlapped any of them would take that element's taps.
    Fixture f;
    const double chatTop = kPhoneHeight - 195.0;
    const double barTop = kPhoneHeight - inGameLoadoutBarHeight();

    for (const Vec2& p : hitPoints(f.controls)) {
        CHECK(p.x > 73.0);                         // the icon column
        CHECK(!(p.y >= chatTop && p.x <= 385.0));  // the chat column
        CHECK(p.y < barTop);                       // the loadout bar
    }
}

TEST(every_point_the_controls_answer_for_belongs_to_exactly_one_of_them) {
    // The invariant a narrow viewport threatens: a phone held upright has no
    // room for the three side by side at full size, and controls that simply
    // kept their sizes would overlap -- a finger landing in the overlap would
    // drive whichever the hit test happened to check first, which is not a
    // control anybody can use. Everything shrinks together instead.
    //
    // Stated behaviourally rather than as a distance, because the radii are
    // the class's own business: press every point the controls claim, and
    // exactly one of them has to answer.
    for (const int width : {kPhoneWidth, 720, 499}) {
        MobileControls controls;
        controls.layout(width, width == kPhoneWidth ? kPhoneHeight : 1080,
                        inGameLoadoutBarHeight());
        int probed = 0;
        for (double y = 0; y < 1080; y += 6.0) {
            for (double x = 0; x < width; x += 6.0) {
                if (!controls.hits(x, y)) continue;
                ++probed;
                controls.reset();
                // Dragged well off the point after landing, so a contact the
                // stick took reads as deflected however close to its centre it
                // arrived. A button ignores the move and stays held.
                controls.update({began(1, x, y), moved(1, x + 4000.0, y)});
                MobileControls::Stick stick;
                const int answered = (controls.stick(stick) ? 1 : 0) +
                                     (controls.attackPressed() ? 1 : 0) +
                                     (controls.retractPressed() ? 1 : 0);
                CHECK(answered == 1);
            }
        }
        CHECK(probed > 100);
    }
}

TEST(a_landscape_viewport_is_laid_out_at_full_size) {
    // The shrink is for the case that cannot be laid out, and must not touch
    // the one that can: every landscape window has the room.
    MobileControls controls;
    controls.layout(kPhoneWidth, kPhoneHeight, inGameLoadoutBarHeight());
    CHECK_NEAR(controls.stickCentre().x, 150.0 * 2.4, 1e-9);
    CHECK_NEAR(controls.attackCentre().x, kPhoneWidth - (30.0 + 46.0) * 2.4, 1e-9);
}

TEST(an_unchosen_touch_setting_is_the_devices_to_answer) {
    ClientSettings settings;
    CHECK(!settings.requestMobileChosen);
    CHECK(settings.touchControlsWanted(true));
    CHECK(!settings.touchControlsWanted(false));

    // Once the player has said, the device stops being asked -- in either
    // direction. A phone player who turned them off keeps them off.
    settings.requestMobileChosen = true;
    settings.requestMobile = false;
    CHECK(!settings.touchControlsWanted(true));
    settings.requestMobile = true;
    CHECK(settings.touchControlsWanted(false));
}
