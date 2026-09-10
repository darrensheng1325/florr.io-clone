#include "test.h"

#include "client/ui/menus.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace flix;

// The client's rebindable controls: where a binding is kept, and whether it
// survives the settings file.
//
// The keys themselves are pressed in App and MenuSystem, which need a real
// window and are not reachable from here. What IS reachable is the half that
// used to be missing: a binding the panel writes has to be readable by the
// action that consumes it, and has to still be there on the next run.

namespace {

std::string tempPath(const char* name) {
    const char* env = std::getenv("TMPDIR");
    std::string base = (env != nullptr && *env != '\0') ? env : "/tmp";
    if (base.back() != '/') base.push_back('/');
    base += "flix_settings_tests";
    mkdir(base.c_str(), 0755);   // already exists is fine
    return base + "/" + name;
}

ControlAction action(int i) { return static_cast<ControlAction>(i); }

} // namespace

TEST(every_control_starts_on_the_default_its_row_advertises) {
    const ClientSettings settings;
    for (int i = 0; i < kControlCount; ++i) {
        CHECK(settings.controlKey(action(i)) == controlMeta(action(i)).defaultKey);
    }
    // The four menu-backed rows read the menu's own key, so the panel and the
    // hotkey MenuSystem opens the panel from can never disagree.
    CHECK(settings.controlKey(ControlAction::Inventory) ==
          settings.hotkeys[static_cast<std::size_t>(MenuId::Inventory)]);
}

TEST(binding_a_menu_row_moves_the_menu_key_and_takes_it_off_the_other_menu) {
    ClientSettings settings;
    settings.bindControl(ControlAction::Crafting, Key::Z);   // the inventory's key
    CHECK(settings.controlKey(ControlAction::Crafting) == Key::Z);
    CHECK(settings.hotkeys[static_cast<std::size_t>(MenuId::Crafting)] == Key::Z);
    CHECK(settings.controlKey(ControlAction::Inventory) == Key::Unknown);
}

TEST(binding_a_plain_row_leaves_every_menu_key_alone) {
    ClientSettings settings;
    settings.bindControl(ControlAction::ToggleHitboxes, Key::Z);
    CHECK(settings.controlKey(ControlAction::ToggleHitboxes) == Key::Z);
    CHECK(settings.controlKey(ControlAction::Inventory) == Key::Z);
}

TEST(a_rebound_control_survives_the_settings_file) {
    const std::string path = tempPath("controls.txt");
    std::remove(path.c_str());

    ClientSettings written;
    written.bindControl(ControlAction::MoveUp, Key::I);
    written.bindControl(ControlAction::ExtendPetals, Key::F);
    written.bindControl(ControlAction::ZoomIn, Key::Period);
    written.bindControl(ControlAction::Skills, Key::Y);
    // A row can be left bound to nothing, and must not come back as a default.
    written.bindControl(ControlAction::ToggleMouseControls, Key::Unknown);
    written.useMouseControls = false;
    CHECK(written.save(path));

    ClientSettings read;
    CHECK(read.load(path));
    CHECK(read.controlKey(ControlAction::MoveUp) == Key::I);
    CHECK(read.controlKey(ControlAction::ExtendPetals) == Key::F);
    CHECK(read.controlKey(ControlAction::ZoomIn) == Key::Period);
    CHECK(read.controlKey(ControlAction::Skills) == Key::Y);
    CHECK(read.controlKey(ControlAction::ToggleMouseControls) == Key::Unknown);
    CHECK(!read.useMouseControls);
    // Untouched rows are still their defaults, not whatever the file's last
    // line happened to leave behind.
    CHECK(read.controlKey(ControlAction::MoveDown) == Key::S);
    CHECK(read.controlKey(ControlAction::Chat) == Key::Enter);

    std::remove(path.c_str());
}

TEST(the_wheel_cannot_scroll_out_past_the_default_view) {
    // kMinZoom is the floor the wheel, the zoom keys and the settings file all
    // land on. It is 100%: a file written by a build that let the wheel reach
    // 0.6 -- or by the browser, which reached 0.5 -- comes back at the default
    // view. Seeing more of the world than that is the loadout's to grant, not
    // the wheel's; see loadoutCameraZoom.
    CHECK_NEAR(kMinZoom, 1.0, 1e-12);
    const std::string path = tempPath("zoom.txt");
    std::remove(path.c_str());

    ClientSettings written;
    written.zoom = 0.5;
    CHECK(written.save(path));
    ClientSettings read;
    CHECK(read.load(path));
    CHECK_NEAR(read.zoom, kMinZoom, 1e-12);

    // Zooming IN is still the player's: the ceiling is untouched.
    written.zoom = kMaxZoom;
    CHECK(written.save(path));
    CHECK(read.load(path));
    CHECK_NEAR(read.zoom, kMaxZoom, 1e-12);
    CHECK(kMaxZoom > kMinZoom);

    std::remove(path.c_str());
}

TEST(mouse_controls_are_on_until_something_turns_them_off) {
    const ClientSettings settings;
    CHECK(settings.useMouseControls);
}

TEST(a_menu_key_no_row_can_rebind_is_not_pinned_by_an_old_settings_file) {
    // Settings, the gallery and the two storefronts have no row in the
    // controls panel, so their keys are constants of the build rather than
    // preferences. An old file that still carries the binding a previous
    // build shipped -- Settings on O, before Escape took it -- must not hold
    // this build to it.
    const std::string path = tempPath("stale_menu_keys.txt");
    std::remove(path.c_str());
    {
        std::FILE* f = std::fopen(path.c_str(), "w");
        CHECK(f != nullptr);
        std::fprintf(f, "key.%d %d\n", static_cast<int>(MenuId::Settings),
                     static_cast<int>(Key::O));
        std::fprintf(f, "key.%d %d\n", static_cast<int>(MenuId::Gallery),
                     static_cast<int>(Key::G));
        // The inventory's, on the other hand, is a real preference.
        std::fprintf(f, "key.%d %d\n", static_cast<int>(MenuId::Inventory),
                     static_cast<int>(Key::Y));
        std::fclose(f);
    }

    ClientSettings read;
    CHECK(read.load(path));
    const ClientSettings fresh;
    CHECK(read.hotkeys[static_cast<std::size_t>(MenuId::Settings)] ==
          fresh.hotkeys[static_cast<std::size_t>(MenuId::Settings)]);
    CHECK(read.hotkeys[static_cast<std::size_t>(MenuId::Gallery)] ==
          fresh.hotkeys[static_cast<std::size_t>(MenuId::Gallery)]);
    CHECK(read.controlKey(ControlAction::Inventory) == Key::Y);

    // And saving does not write them back out for the next run to read.
    CHECK(read.save(path));
    ClientSettings again;
    CHECK(again.load(path));
    CHECK(again.hotkeys[static_cast<std::size_t>(MenuId::Settings)] == Key::Escape);
    CHECK(again.controlKey(ControlAction::Inventory) == Key::Y);

    std::remove(path.c_str());
}

TEST(the_touch_control_choice_survives_the_settings_file_and_an_unmade_one_does_not) {
    const std::string path = tempPath("touch.cfg");

    // Nothing chosen: nothing written, so the next run still asks the device.
    ClientSettings fresh;
    CHECK(fresh.save(path));
    ClientSettings reloaded;
    CHECK(reloaded.load(path));
    CHECK(!reloaded.requestMobileChosen);
    CHECK(reloaded.touchControlsWanted(true));
    CHECK(!reloaded.touchControlsWanted(false));

    // A player who turned them OFF is the case a written value has to survive:
    // resolving from the device again would hand a phone the stick back every
    // single run.
    ClientSettings chosen;
    chosen.requestMobile = false;
    chosen.requestMobileChosen = true;
    CHECK(chosen.save(path));
    ClientSettings after;
    CHECK(after.load(path));
    CHECK(after.requestMobileChosen);
    CHECK(!after.touchControlsWanted(true));

    std::remove(path.c_str());
}
