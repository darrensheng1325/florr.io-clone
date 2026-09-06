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

TEST(mouse_controls_are_on_until_something_turns_them_off) {
    const ClientSettings settings;
    CHECK(settings.useMouseControls);
}
