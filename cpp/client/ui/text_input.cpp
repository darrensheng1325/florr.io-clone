#include "client/ui/text_input.h"

#include <algorithm>
#include <cmath>

#include "client/ui/text.h"
#include "client/ui/text_select.h"

namespace flix::ui {

namespace {

/// Whether `byte` starts a UTF-8 sequence rather than continuing one.
bool isLead(unsigned char byte) { return (byte & 0xC0) != 0x80; }

/// The part of `text` a field with these options will take, trimmed to `room`
/// bytes on a character boundary.
///
/// A clipboard holds whatever the user last copied ANYWHERE, so this is the
/// filter that stands between a field and a screenful of shell output: control
/// characters go, a newline survives only in a multiline field, and everything
/// past the cap is dropped rather than truncating the character it lands in.
std::string acceptable(const std::string& text, const TextEditOptions& options,
                       std::size_t room) {
    std::string out;
    if (room == 0) return out;
    out.reserve(text.size() < room ? text.size() : room);

    for (std::size_t i = 0; i < text.size();) {
        std::size_t next = i + 1;
        while (next < text.size() && !isLead(static_cast<unsigned char>(text[next]))) ++next;
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const std::size_t width = next - i;
        i = next;

        // CRLF collapses to one newline; a lone CR is one too, so text pasted
        // out of a Windows editor does not double-space itself.
        const bool newline = lead == '\n' || lead == '\r';
        if (newline && !options.multiline) continue;
        if (!newline) {
            if (lead < 0x20 || lead == 0x7F) continue;
            if (options.asciiOnly && lead >= 0x80) continue;
        }
        if (out.size() + (newline ? 1 : width) > room) break;

        if (newline) {
            if (lead == '\r' && i < text.size() && text[i] == '\n') ++i;
            out += '\n';
        } else {
            out.append(text, i - width, width);
        }
    }
    return out;
}

/// The bounds of the line `at` falls in. Only a multiline field asks: for a
/// single-line one the line and the value are the same thing.
std::size_t lineStart(const std::string& s, std::size_t at) {
    if (at == 0) return 0;
    const std::size_t nl = s.rfind('\n', at - 1);
    return nl == std::string::npos ? 0 : nl + 1;
}

std::size_t lineEnd(const std::string& s, std::size_t at) {
    const std::size_t nl = s.find('\n', at);
    return nl == std::string::npos ? s.size() : nl;
}

/// A caret left inside a multi-byte character -- the value can have been
/// replaced under it -- snapped back to that character's first byte.
///
/// Only that. A caret already on a boundary, the END of the value included,
/// must come through unmoved: an earlier version rounded every caret through
/// utf8Prev(utf8Next(caret)) instead, which is identity in the middle of a
/// string but walks back a whole character at the end of one -- so every field
/// in the game typed in reverse.
std::size_t snapToBoundary(const std::string& value, std::size_t at) {
    if (at > value.size()) return value.size();
    while (at > 0 && at < value.size() && !isLead(static_cast<unsigned char>(value[at]))) --at;
    return at;
}

bool isWordByte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c >= 0x80;
}

/// A second press within this long, and near enough, continues a click streak.
constexpr double kDoubleClickSeconds = 0.4;

} // namespace

std::size_t utf8Prev(const std::string& s, std::size_t at) {
    if (at > s.size()) at = s.size();
    if (at == 0) return 0;
    std::size_t i = at - 1;
    while (i > 0 && !isLead(static_cast<unsigned char>(s[i]))) --i;
    return i;
}

std::size_t utf8Next(const std::string& s, std::size_t at) {
    if (at >= s.size()) return s.size();
    std::size_t i = at + 1;
    while (i < s.size() && !isLead(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

TextEditResult editText(const TextEditFrame& frame, std::string& value, TextSelection& selection,
                        const TextEditOptions& options) {
    TextEditResult out;

    const TextSelection before = selection;
    selection.caret = snapToBoundary(value, selection.caret);
    selection.anchor = snapToBoundary(value, selection.anchor);

    /// Replaces the selection with nothing, and reports whether there was one.
    const auto dropSelection = [&] {
        if (selection.empty()) return false;
        const std::size_t at = selection.begin();
        value.erase(at, selection.end() - at);
        selection.collapse(at);
        out.changed = true;
        return true;
    };

    const auto insert = [&](const std::string& text) {
        if (text.empty()) return;
        // Measured against what is left AFTER the selection goes, so replacing
        // a full field is not refused for want of room.
        const std::size_t remaining = value.size() - (selection.end() - selection.begin());
        const std::size_t room = options.maxBytes > remaining ? options.maxBytes - remaining : 0;
        const std::string fit = acceptable(text, options, room);
        if (fit.empty()) return;
        dropSelection();
        value.insert(selection.caret, fit);
        selection.collapse(selection.caret + fit.size());
        out.changed = true;
    };

    /// Moves the caret, taking the anchor with it unless shift is held.
    const auto moveTo = [&](std::size_t at) {
        selection.caret = at;
        if (!frame.shift) selection.anchor = at;
    };

    if (frame.selectAll) selection.selectAll(value);

    insert(frame.typed);
    insert(frame.pasted);

    if (options.copyable && !selection.empty() && (frame.copy || frame.cut)) {
        // The SELECTION and nothing else. A field with the caret in it but
        // nothing selected must let Ctrl+C through: the chat box holds the
        // caret the whole time it is open, and copying its empty draft over
        // whatever the player had just highlighted in the transcript is the
        // one thing that must not happen.
        out.clipboard = selection.of(value);
        if (frame.cut) dropSelection();
    }

    if (options.multiline && frame.enter) insert("\n");

    if (frame.backspace && !dropSelection() && selection.caret > 0) {
        const std::size_t from = utf8Prev(value, selection.caret);
        value.erase(from, selection.caret - from);
        selection.collapse(from);
        out.changed = true;
    }
    if (frame.erase && !dropSelection() && selection.caret < value.size()) {
        value.erase(selection.caret, utf8Next(value, selection.caret) - selection.caret);
        out.changed = true;
    }

    // An unshifted arrow over a selection collapses to that selection's edge
    // rather than stepping off the caret, which is what every editor does.
    if (frame.left) {
        moveTo(!frame.shift && !selection.empty() ? selection.begin()
                                                  : utf8Prev(value, selection.caret));
    }
    if (frame.right) {
        moveTo(!frame.shift && !selection.empty() ? selection.end()
                                                  : utf8Next(value, selection.caret));
    }
    // Home and End are the LINE's ends in a multiline field and the value's in
    // a single-line one, which in a single-line field is the same answer.
    if (frame.home) moveTo(options.multiline ? lineStart(value, selection.caret) : 0);
    if (frame.end) moveTo(options.multiline ? lineEnd(value, selection.caret) : value.size());

    const bool up = options.multiline && frame.up;
    const bool down = options.multiline && frame.down;
    if (up != down) {
        // The column is counted in CHARACTERS, not bytes: a line above with a
        // multi-byte character in it would otherwise pull the caret sideways.
        const std::size_t start = lineStart(value, selection.caret);
        std::size_t column = 0;
        for (std::size_t i = start; i < selection.caret; i = utf8Next(value, i)) ++column;

        std::size_t row = start;
        if (up) {
            if (start > 0) row = lineStart(value, start - 1);
        } else {
            const std::size_t stop = lineEnd(value, selection.caret);
            if (stop < value.size()) row = stop + 1;
        }
        // Past the end of a shorter line the caret parks at that line's end,
        // which is what every editor does.
        const std::size_t stop = lineEnd(value, row);
        std::size_t at = row;
        for (std::size_t i = 0; i < column && at < stop; ++i) at = utf8Next(value, at);
        moveTo(at);
    }

    out.caretMoved = selection.caret != before.caret || selection.anchor != before.anchor;
    return out;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

double xOfIndex(const TextRun& run, std::size_t at) {
    return run.originX + measure(run.text.substr(0, std::min(at, run.text.size())), run.size,
                                 run.bold);
}

std::size_t indexAtX(const TextRun& run, double px) {
    // Walked a character at a time rather than bisected: proportional glyph
    // widths make the offset-to-x map non-uniform, and a field holds at most a
    // line of text.
    std::size_t best = 0;
    double bestDistance = std::fabs(px - run.originX);
    for (std::size_t at = utf8Next(run.text, 0); ; at = utf8Next(run.text, at)) {
        const double distance = std::fabs(px - xOfIndex(run, at));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = at;
        }
        if (at >= run.text.size()) break;
    }
    return best;
}

namespace {

/// Which CHARACTER a double-click at `at` means.
///
/// `at` is a boundary, so it is ambiguous between the character before it and
/// the one after. The one before wins whenever it is part of a word, which is
/// what makes a click at the end of a word select that word rather than the
/// space behind it -- the same bias a browser applies.
std::size_t wordProbe(const std::string& s, std::size_t at) {
    at = snapToBoundary(s, at);
    if (at >= s.size()) return utf8Prev(s, s.size());
    if (at == 0) return 0;
    const std::size_t before = utf8Prev(s, at);
    return isWordByte(static_cast<unsigned char>(s[before])) ? before : at;
}

} // namespace

std::size_t wordBegin(const std::string& s, std::size_t at) {
    if (s.empty()) return 0;
    std::size_t start = wordProbe(s, at);
    const bool word = isWordByte(static_cast<unsigned char>(s[start]));
    while (start > 0) {
        const std::size_t prev = utf8Prev(s, start);
        if (isWordByte(static_cast<unsigned char>(s[prev])) != word) break;
        start = prev;
    }
    return start;
}

std::size_t wordEnd(const std::string& s, std::size_t at) {
    if (s.empty()) return 0;
    std::size_t stop = wordProbe(s, at);
    const bool word = isWordByte(static_cast<unsigned char>(s[stop]));
    while (stop < s.size() && isWordByte(static_cast<unsigned char>(s[stop])) == word) {
        stop = utf8Next(s, stop);
    }
    return stop;
}

// ---------------------------------------------------------------------------
// The field
// ---------------------------------------------------------------------------

void TextFieldState::focus(const std::string& value, double timeSeconds) {
    focused = true;
    dragging = false;
    selection.selectAll(value);
    caretSeconds = timeSeconds;
}

void TextFieldState::focusAtEnd(const std::string& value, double timeSeconds) {
    focused = true;
    dragging = false;
    selection.collapse(value.size());
    caretSeconds = timeSeconds;
}

void TextFieldState::blur() {
    focused = false;
    dragging = false;
    selection.collapse(selection.caret);
}

bool caretVisible(const TextFieldState& state, double timeSeconds) {
    // Solid for the first half-second after any change: a caret that blinks
    // out in the middle of a word reads as a field that stopped listening.
    return std::fmod(std::max(0.0, timeSeconds - state.caretSeconds), 1.0) < 0.5;
}

bool editText(Window& window, std::string& value, TextFieldState& state, double timeSeconds,
              const TextEditOptions& options) {
    const bool ctrl = window.ctrlHeld();

    TextEditFrame frame;
    // A shortcut is not a character. Typed text is skipped under a modifier
    // because the two backends disagree about whether Ctrl+V also produces
    // one: an SDL layout may hand back a "v", the browser's keydown handler
    // filters it out, and a field that took both would paste and type on the
    // same keystroke.
    if (!ctrl && !window.altHeld()) frame.typed = window.typedText();
    frame.pasted = window.pastedText();
    frame.shift = window.shiftHeld();
    frame.copy = ctrl && window.keyPressed(Key::C);
    frame.cut = ctrl && window.keyPressed(Key::X);
    frame.selectAll = ctrl && window.keyPressed(Key::A);
    frame.backspace = window.keyPressed(Key::Backspace);
    frame.erase = window.keyPressed(Key::Delete);
    frame.left = window.keyPressed(Key::Left);
    frame.right = window.keyPressed(Key::Right);
    frame.up = window.keyPressed(Key::Up);
    frame.down = window.keyPressed(Key::Down);
    frame.home = window.keyPressed(Key::Home);
    frame.end = window.keyPressed(Key::End);
    frame.enter = window.keyPressed(Key::Enter);

    const TextEditResult result = editText(frame, value, state.selection, options);
    if (!result.clipboard.empty()) window.setClipboardText(result.clipboard);
    if (result.changed || result.caretMoved) state.caretSeconds = timeSeconds;

    // A field being edited IS the focused one, so this is the one place that
    // has to say so -- the context menu can then cut, paste into and select it
    // without knowing which panel drew it.
    if (state.focused) {
        FocusedField field;
        field.value = &value;
        field.state = &state;
        field.options = options;
        TextSelect::instance().setFocusedField(field);
    }
    return result.changed;
}

namespace {

/// The shared tail of the two pointer handlers: a press that has already been
/// resolved to a byte offset.
bool pressAt(Window& window, TextFieldState& state, std::size_t at, const std::string& value,
             double timeSeconds, bool allowWordSelect) {
    const bool repeat = timeSeconds - state.lastClickSeconds < kDoubleClickSeconds;
    state.clickStreak = repeat ? state.clickStreak + 1 : 1;
    state.lastClickSeconds = timeSeconds;
    state.focused = true;
    state.caretSeconds = timeSeconds;

    if (allowWordSelect && state.clickStreak >= 3) {
        state.selection.selectAll(value);
        state.dragging = false;
    } else if (allowWordSelect && state.clickStreak == 2) {
        state.selection.anchor = wordBegin(value, at);
        state.selection.caret = wordEnd(value, at);
        state.dragging = false;
    } else {
        // Shift-click extends from wherever the selection was anchored, which
        // is what makes "click here, shift-click there" select the span.
        if (window.shiftHeld()) state.selection.caret = at;
        else state.selection.collapse(at);
        state.dragging = true;
    }
    return true;
}

} // namespace

bool trackTextMouse(Window& window, TextFieldState& state, Rect box, const TextRun& run,
                    const std::string& value, double timeSeconds) {
    const Vec2 mouse{window.mouseX(), window.mouseY()};

    if (state.dragging) {
        if (window.mouseDown(MouseButton::Left)) {
            // Not clamped to the box: dragging past either edge should keep
            // extending to that end, which is what falls out of the nearest-
            // boundary search once the pointer is outside the run.
            state.selection.caret = indexAtX(run, mouse.x);
            state.caretSeconds = timeSeconds;
        } else {
            state.dragging = false;
        }
    }

    // Reported whether or not it is focused: a right-click on an unfocused box
    // still has to raise the field's own menu rather than the page's.
    if (state.focused) {
        FocusedField field = TextSelect::instance().focusedField();
        if (field.state == &state) {
            field.box = box;
            TextSelect::instance().setFocusedField(field);
        }
    }

    if (!window.mousePressed(MouseButton::Left)) return false;
    if (!box.contains(mouse)) {
        state.blur();
        return false;
    }
    return pressAt(window, state, indexAtX(run, mouse.x), value, timeSeconds, true);
}

namespace {

/// The byte offset a point lands on in a multiline value laid out at
/// `lineHeight` with the first line's MIDDLE at `firstBaseline`.
std::size_t indexAtPoint(const std::string& value, double originX, double firstBaseline,
                         double lineHeight, double size, bool bold, Vec2 point) {
    const double rows = lineHeight > 0 ? (point.y - (firstBaseline - lineHeight * 0.5)) / lineHeight
                                       : 0.0;
    int row = static_cast<int>(std::floor(rows));
    if (row < 0) row = 0;

    std::size_t start = 0;
    for (int i = 0; i < row; ++i) {
        const std::size_t nl = value.find('\n', start);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    const std::size_t stop = lineEnd(value, start);

    TextRun run;
    run.text = value.substr(start, stop - start);
    run.originX = originX;
    run.size = size;
    run.bold = bold;
    return start + indexAtX(run, point.x);
}

} // namespace

bool trackTextMouseMultiline(Window& window, TextFieldState& state, Rect box,
                             const std::string& value, double originX, double firstBaseline,
                             double lineHeight, double size, bool bold, double timeSeconds) {
    const Vec2 mouse{window.mouseX(), window.mouseY()};
    const auto resolve = [&] {
        return indexAtPoint(value, originX, firstBaseline, lineHeight, size, bold, mouse);
    };

    if (state.dragging) {
        if (window.mouseDown(MouseButton::Left)) {
            state.selection.caret = resolve();
            state.caretSeconds = timeSeconds;
        } else {
            state.dragging = false;
        }
    }

    if (!window.mousePressed(MouseButton::Left)) return false;
    if (!box.contains(mouse)) {
        state.dragging = false;
        return false;
    }
    return pressAt(window, state, resolve(), value, timeSeconds, true);
}

} // namespace flix::ui
