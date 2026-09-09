#include "test.h"

#include "client/camera.h"
#include "client/ui/menus.h"
#include "shared/game/config.h"

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <string>

using namespace flix;

// What the camera shows: the player's zoom setting times what the worn
// loadout asks for. Antennae and observer are the petals that ask, and the
// figure is the client's alone -- the server neither computes it nor sends
// it, exactly as the browser's getEquippedZoomMultiplier was -- so this is
// the one place it can be right or wrong.

namespace {

/// Content of its own, so the assertions ride the RULES and not the shipped
/// balance numbers: one petal that asks nothing, two that ask the figures
/// petals.json gives antennae and observer, and one authored so low that a
/// high tier would take it through zero.
const char* const kPetalsJson = R"JSON({
  "basic":    {"name":"Basic","damage":10,"health":10,"size":1,"cooldown":2500,"count":1,"color":"#FFFFFF"},
  "antennae": {"name":"Antennae","damage":0,"health":null,"size":1,"cooldown":1,"count":0,"equipFlags":"Antennae","noPhysics":true,"cameraZoom":0.92,"color":"#000000"},
  "observer": {"name":"Observer","damage":0,"health":null,"size":1,"cooldown":1,"count":0,"equipFlags":"Observer","noPhysics":true,"cameraZoom":0.85,"color":"#000000"},
  "scope":    {"name":"Scope","damage":0,"health":null,"size":1,"cooldown":1,"count":0,"noPhysics":true,"cameraZoom":0.1,"color":"#000000"}
})JSON";

const char* const kMobsJson = R"JSON({
  "critter": {"name":"Critter","health":10,"damage":1,"size":1,"speed":1,"range":300,"cooldown":500,"color":"#FF0000","section":[0],"ai_type":"hostile"}
})JSON";

std::string tempDir() {
    const char* env = std::getenv("TMPDIR");
    std::string base = (env != nullptr && *env != '\0') ? env : "/tmp";
    if (base.back() != '/') base.push_back('/');
    base += "flix_camera_tests";
    mkdir(base.c_str(), 0755);   // already there is fine
    return base;
}

bool writeText(const std::string& path, const char* text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return out.good();
}

struct Fixture {
    ContentRegistry registry;
    bool ok = false;
    std::string error;
};

const Fixture& fixture() {
    static const Fixture state = [] {
        Fixture f;
        const std::string dir = tempDir();
        const std::string mobs = dir + "/mobs.json";
        const std::string petals = dir + "/petals.json";
        if (!writeText(mobs, kMobsJson) || !writeText(petals, kPetalsJson)) {
            f.error = "could not write the fixture content into " + dir;
            return f;
        }
        f.ok = f.registry.loadFiles(mobs, petals, "", f.error);
        return f;
    }();
    return state;
}

bool contentLoaded() {
    if (!fixture().ok) {
        ::testing::reportFailure(__FILE__, __LINE__,
                                 "fixture content failed to load: " + fixture().error);
    }
    return fixture().ok;
}

Profile::Slot slot(const char* id, Rarity rarity = Rarity::Common) {
    Profile::Slot s;
    s.petalIndex = fixture().registry.petalIndex(id);
    s.rarity = rarity;
    return s;
}

/// A profile with `count` slots, every one of them empty.
Profile emptyBar(std::size_t count = static_cast<std::size_t>(kLoadoutActiveSlots) * 2) {
    Profile p;
    p.loadout.assign(count, Profile::Slot{});
    return p;
}

double zoomOf(const Profile& p) { return loadoutCameraZoom(p, fixture().registry); }

/// The multiplier a tier applies to the authored figure's distance from 1:
/// the passive-modifier curve, 1x at common to 4x at unique, the same one
/// the browser's getPetalStats used for cameraZoom.
double scaled(double authored, Rarity rarity) {
    return 1.0 + (authored - 1.0) * petalModifierScale(rarity);
}

} // namespace

TEST(a_bar_with_no_antennae_or_observer_asks_the_camera_for_nothing) {
    if (!contentLoaded()) return;
    CHECK_NEAR(zoomOf(Profile{}), 1.0, 1e-12);
    Profile p = emptyBar();
    CHECK_NEAR(zoomOf(p), 1.0, 1e-12);
    p.loadout[0] = slot("basic", Rarity::Unique);
    p.loadout[3] = slot("basic");
    CHECK_NEAR(zoomOf(p), 1.0, 1e-12);
}

TEST(antennae_and_observer_zoom_the_camera_out_by_their_authored_figure) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    p.loadout[2] = slot("antennae");
    CHECK_NEAR(zoomOf(p), 0.92, 1e-12);
    p.loadout[2] = slot("observer");
    CHECK_NEAR(zoomOf(p), 0.85, 1e-12);
}

TEST(two_of_them_show_the_better_one_and_never_stack) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    p.loadout[0] = slot("antennae");
    p.loadout[1] = slot("observer");
    CHECK_NEAR(zoomOf(p), 0.85, 1e-12);
    // Two observers are one observer, not 0.85 squared.
    p.loadout[0] = slot("observer");
    CHECK_NEAR(zoomOf(p), 0.85, 1e-12);
    // And the stronger tier wins whichever slot it sits in.
    p.loadout[5] = slot("antennae", Rarity::Unique);
    CHECK_NEAR(zoomOf(p), std::min(0.85, scaled(0.92, Rarity::Unique)), 1e-12);
}

TEST(rarity_widens_the_view_on_the_passive_modifier_curve) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    p.loadout[0] = slot("observer", Rarity::Unique);
    // 1 + (0.85 - 1) * 4 = 0.40 at unique: a unique observer shows two and a
    // half times the width a bare flower does.
    CHECK_NEAR(zoomOf(p), 0.40, 1e-12);
    CHECK_NEAR(zoomOf(p), scaled(0.85, Rarity::Unique), 1e-12);
    p.loadout[0] = slot("antennae", Rarity::Apex);
    CHECK_NEAR(zoomOf(p), scaled(0.92, Rarity::Apex), 1e-12);
    CHECK(zoomOf(p) < 0.92);
}

TEST(the_view_is_floored_where_the_browser_floored_it) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    // Authored at 0.1, an apex scope would be 1 - 0.9 * 4.375: through zero
    // and out the other side. It stops at 0.3, as getPetalStats did.
    p.loadout[0] = slot("scope", Rarity::Apex);
    CHECK(scaled(0.1, Rarity::Apex) < 0.0);
    CHECK_NEAR(zoomOf(p), 0.3, 1e-12);
    // The shipped observer never reaches the floor at any tier.
    p.loadout[0] = slot("observer", Rarity::Apex);
    CHECK(zoomOf(p) > 0.3);
}

TEST(storage_grants_no_zoom) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    // Slot ten is the first secondary slot: an observer stashed there is not
    // worn, just as its equipment bit is not set and it draws no antennae.
    p.loadout[static_cast<std::size_t>(kLoadoutActiveSlots)] = slot("observer", Rarity::Unique);
    CHECK_NEAR(zoomOf(p), 1.0, 1e-12);
    p.loadout[static_cast<std::size_t>(kLoadoutActiveSlots) - 1] = slot("observer");
    CHECK_NEAR(zoomOf(p), 0.85, 1e-12);
}

TEST(a_slot_the_content_does_not_know_is_skipped) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    p.loadout[0].petalIndex = static_cast<std::uint16_t>(fixture().registry.petalCount() + 5);
    p.loadout[1] = slot("antennae");
    CHECK_NEAR(zoomOf(p), 0.92, 1e-12);
}

TEST(the_camera_multiplies_the_setting_by_what_the_loadout_asks) {
    Camera camera;
    camera.setViewport(1920, 1080);
    camera.snapTo({1000, 1000});
    camera.userZoom = 1.0;
    camera.loadoutZoom = 1.0;
    CHECK_NEAR(camera.zoom(), 1.0, 1e-12);
    const Rect bare = camera.visibleWorld();

    // An observer alone: the world is 1/0.85 wider through the same window.
    camera.loadoutZoom = 0.85;
    CHECK_NEAR(camera.zoom(), 0.85, 1e-12);
    const Rect worn = camera.visibleWorld();
    CHECK_NEAR(worn.w, bare.w / 0.85, 1e-9);
    CHECK_NEAR(worn.h, bare.h / 0.85, 1e-9);
    // Still centred on the flower.
    CHECK_NEAR(worn.x + worn.w * 0.5, 1000.0, 1e-9);
    CHECK_NEAR(worn.y + worn.h * 0.5, 1000.0, 1e-9);

    // The player's own zoom-in rides on top of it, not instead of it.
    camera.userZoom = 1.6;
    CHECK_NEAR(camera.zoom(), 1.6 * 0.85, 1e-12);
    // And the screen centre maps back to the flower whatever the product.
    const Vec2 centre = camera.screenToWorld({960, 540});
    CHECK_NEAR(centre.x, 1000.0, 1e-9);
    CHECK_NEAR(centre.y, 1000.0, 1e-9);
}
