#pragma once
// Text fields: the selection they hold, the keyboard that edits it and the
// pointer that drags it out.
//
// Every field in the client -- the auth form, the lobby's name box, the chat
// line, the inventory's search, the shop's code box, the guild prompts, the
// settings endpoint and the skin studio's two editors -- grew its own copy of
// "append typedText, erase on Backspace". Seven copies, seven sets of rules
// about what a printable character is, no clipboard, and no selection at all:
// there was nothing in the client a player could drag a highlight across, so
// "copy" could only ever have meant the whole box.
//
// This is the one copy. Nothing here draws -- the field's plate and its scroll
// are still the panel's business -- but everything here is what a panel needs
// to draw a selection correctly: where a byte offset lands in x, and which
// offset an x lands on.

#include <cstddef>
#include <string>

#include "window.h"

#include "shared/core/types.h"

namespace flix::ui {

struct TextEditOptions {
    /// Cap on the value's LENGTH IN BYTES. A paste is trimmed to fit it on a
    /// character boundary rather than cut mid-sequence.
    std::size_t maxBytes = 128;
    /// Printable ASCII only. The fields the server holds to ASCII anyway --
    /// usernames, guild names, redemption codes -- take this, so a pasted
    /// emoji is dropped at the field instead of being refused after a round
    /// trip.
    bool asciiOnly = false;
    /// Enter inserts a newline instead of being left for the caller to read as
    /// a submit. Also what turns Home/End into the line's ends and makes
    /// Up/Down move between lines.
    bool multiline = false;
    /// Whether the field will put its own contents on the clipboard. Off for
    /// the password boxes: pasting INTO a masked field is fine, reading one
    /// back out is the thing a mask exists to stop.
    bool copyable = true;
};

/// A field's caret and its selection.
///
/// One pair of byte offsets, not a caret plus a separate range: `anchor` is
/// where the selection was started from and `caret` is where it has been
/// dragged to, so the two are equal exactly when there is no selection. That
/// is also what makes a shift-extension in either direction fall out for free
/// -- the caret crossing the anchor reverses the range without a special case.
struct TextSelection {
    std::size_t caret = 0;
    std::size_t anchor = 0;

    bool empty() const { return caret == anchor; }
    std::size_t begin() const { return caret < anchor ? caret : anchor; }
    std::size_t end() const { return caret < anchor ? anchor : caret; }
    void collapse(std::size_t at) { caret = at; anchor = at; }
    void selectAll(const std::string& value) { anchor = 0; caret = value.size(); }
    std::string of(const std::string& value) const {
        return empty() ? std::string() : value.substr(begin(), end() - begin());
    }
};

/// One frame of keyboard input, as a text field sees it.
///
/// Split out from Window so the editing rules can be exercised without a
/// window to open. That is not tidiness: the first version of this file
/// resolved the caret through Window directly, and a bug that walked it back
/// one character per frame -- every field typing in reverse -- was unreachable
/// by any test, because the only parts a test could call were the pure helpers
/// that happened to be correct.
struct TextEditFrame {
    /// Characters the layout produced this frame.
    std::string typed;
    /// Text a paste delivered this frame.
    std::string pasted;
    /// Extends the selection instead of collapsing it, for every caret move.
    bool shift = false;
    bool copy = false;        ///< Ctrl/Cmd+C
    bool cut = false;         ///< Ctrl/Cmd+X
    bool selectAll = false;   ///< Ctrl/Cmd+A
    bool backspace = false;
    bool erase = false;       ///< the Delete key
    bool left = false, right = false, up = false, down = false;
    bool home = false, end = false;
    /// Inserts a newline in a multiline field; ignored in any other.
    bool enter = false;
};

struct TextEditResult {
    bool changed = false;
    /// What a copy or cut asked to put on the clipboard, empty when neither
    /// happened. Returned rather than written, so the core needs no window.
    std::string clipboard;
    /// Whether the caret moved or the selection changed, which is separate
    /// from `changed`: a field restarts its blink on either.
    bool caretMoved = false;
};

/// Applies one frame of editing. Offsets are kept on UTF-8 boundaries, so a
/// caller may measure `value.substr(0, selection.caret)` to place its caret and
/// `value.substr(selection.begin(), ...)` to paint its highlight.
TextEditResult editText(const TextEditFrame&, std::string& value, TextSelection&,
                        const TextEditOptions& options = {});

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

/// A single-line run of text as it is painted, for mapping between a byte
/// offset in it and an x on screen.
///
/// `originX` is where the run's FIRST glyph is drawn, so a field that scrolls
/// its own text passes its scroll through it (`box.x + pad - scrollX`) and one
/// formula covers the scrolled and unscrolled cases alike.
struct TextRun {
    std::string text;
    double originX = 0;
    double size = 14.0;
    bool bold = false;
};

double xOfIndex(const TextRun&, std::size_t at);

/// The character boundary NEAREST `px` -- nearest, not the one before it, so a
/// click on the right half of a glyph puts the caret after that glyph.
std::size_t indexAtX(const TextRun&, double px);

/// The word around `at`, for a double-click: a run of letters, digits and
/// underscores, or a run of whatever else is there.
std::size_t wordBegin(const std::string&, std::size_t at);
std::size_t wordEnd(const std::string&, std::size_t at);

// ---------------------------------------------------------------------------
// The field
// ---------------------------------------------------------------------------

/// Everything a text field keeps between frames beyond its value.
struct TextFieldState {
    TextSelection selection;
    bool focused = false;
    bool dragging = false;
    /// When the caret last moved or the value last changed. The blink is
    /// phased on it rather than on the wall clock, so a caret being typed at
    /// stays solid instead of blinking out mid-word.
    double caretSeconds = 0;
    /// The previous press, for double- and triple-click.
    double lastClickSeconds = -10;
    int clickStreak = 0;

    /// Takes the caret, with everything selected -- what tabbing or clicking a
    /// button that focuses a field should do.
    void focus(const std::string& value, double timeSeconds);
    /// Takes the caret with the cursor at the end and nothing selected.
    void focusAtEnd(const std::string& value, double timeSeconds);
    void blur();
};

/// True while the caret should be painted.
bool caretVisible(const TextFieldState&, double timeSeconds);

/// Reads this frame from the window, applies it, and posts a copy or cut to
/// the system clipboard. Returns true when `value` changed.
bool editText(Window&, std::string& value, TextFieldState&, double timeSeconds,
              const TextEditOptions& options = {});

/// One single-line field's pointer handling: a press inside `box` focuses it
/// and places the caret, a drag extends the selection, a double-click takes
/// the word under it and a third click the whole value. A press outside blurs.
///
/// Returns true when the press landed inside the field, so a panel can keep
/// the same click from also reaching whatever is behind it.
bool trackTextMouse(Window&, TextFieldState&, Rect box, const TextRun&,
                    const std::string& value, double timeSeconds);

/// The same for a multiline field, whose lines are laid out at `lineHeight`
/// starting with the first line's centre at `firstBaseline`.
bool trackTextMouseMultiline(Window&, TextFieldState&, Rect box, const std::string& value,
                             double originX, double firstBaseline, double lineHeight,
                             double size, bool bold, double timeSeconds);

/// Start of the UTF-8 sequence ending at `at`, and start of the one beginning
/// there -- one character back and one character forward. Trimming a single
/// byte off a multi-byte character leaves a string that will neither measure
/// nor draw, so every erase and every caret move in the client goes through
/// these.
std::size_t utf8Prev(const std::string&, std::size_t at);
std::size_t utf8Next(const std::string&, std::size_t at);

} // namespace flix::ui
