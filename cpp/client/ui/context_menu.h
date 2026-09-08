#pragma once
// The right-click menu.
//
// One per client, raised over whatever the pointer is on and drawn last of
// everything. Built out of the game's own dark chrome -- the charcoal the
// inventory's search surround and the Stack toggle are made of -- rather than
// a panel colour: it appears over all eight of those in turn, and a card in
// any one of their hues would read as that panel having grown a limb.

#include <cstdint>
#include <string>
#include <vector>

#include "canvas.h"
#include "window.h"

#include "shared/core/types.h"

namespace flix::ui {

/// What a row does. The app maps these onto whatever holds the caret or the
/// selection, so the menu itself knows nothing about fields or panels.
enum class ContextAction : std::uint8_t { None, Cut, Copy, Paste, SelectAll };

class ContextMenu {
public:
    struct Item {
        ContextAction action = ContextAction::None;
        std::string label;
        /// A greyed row is still drawn: a menu whose contents changed shape
        /// with the selection would be a different menu every time.
        bool enabled = true;
    };

    void open(Vec2 at, std::vector<Item> items, int viewWidth, int viewHeight);
    void close();
    bool isOpen() const { return open_; }
    /// Whether `point` is inside the card, so the frame can keep a click that
    /// lands on the menu from also reaching what is under it.
    bool contains(Vec2) const;

    /// Answers the pointer and the keyboard, then draws. Returns the action a
    /// click chose, and closes the menu when it did. Escape and a press
    /// outside close it without choosing anything.
    ContextAction update(Canvas&, Window&);

private:
    Rect bounds() const;

    bool open_ = false;
    Vec2 at_{};
    std::vector<Item> items_;
};

} // namespace flix::ui
