#pragma once
// Selecting the text a panel has already painted.
//
// The menus are immediate-mode: a label is a draw call and nothing retains
// where it landed, so there is nothing for a selection to point at. This layer
// records every run of STATIC text as it is painted and resolves a drag
// against that list, which is what lets a player highlight a petal's stats, a
// changelog line or somebody's chat message and copy it.
//
// Capture is OFF by default and is turned on for exactly two things: an open
// menu panel and the chat transcript. Inside those, the widgets that print a
// label on something you click -- a button, a chip, an item tile, a text field
// -- turn it back off, because that text belongs to the control rather than to
// the page. Everything that moves is outside both scopes to begin with: the
// world, the HUD, damage numbers and the loadout bar are never recorded.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "canvas.h"
#include "window.h"

#include "client/ui/text_input.h"
#include "shared/core/types.h"

namespace flix::ui {

/// One painted run, as the layer remembers it.
struct CapturedRun {
    std::string text;
    double penX = 0;
    double baselineY = 0;
    double size = 14.0;
    bool bold = false;
    /// Content-derived, so the same label keeps the same identity from one
    /// frame to the next even though the run list is rebuilt every frame.
    std::uint64_t key = 0;

    double width() const;
    /// The band the glyphs occupy, ascender to descender.
    Rect band() const;
};

/// Where a selection starts or ends: which run, and a byte offset into it.
struct TextPoint {
    std::uint64_t run = 0;      ///< 0 is "nowhere"
    std::size_t offset = 0;

    bool valid() const { return run != 0; }
};

/// The field that currently holds the caret, whichever panel owns it.
///
/// Registered by `editText` itself -- a field that is being edited IS the
/// focused one -- so the context menu can cut, paste into and select the box
/// under the pointer without knowing which panel drew it. The pointers are
/// good for the frame that registered them and no longer.
struct FocusedField {
    std::string* value = nullptr;
    TextFieldState* state = nullptr;
    TextEditOptions options;
    /// Where it was painted, for hit-testing a right-click against it. Zero
    /// when the field did not report one.
    Rect box{};

    bool valid() const { return value != nullptr && state != nullptr; }
};

class TextSelect {
public:
    static TextSelect& instance();

    /// Drops the previous frame's runs and the focused field. Called once, at
    /// the very top of the frame, before anything is updated or painted.
    void beginFrame();

    void record(const std::string& text, double penX, double baselineY, double size, bool bold);

    void setFocusedField(const FocusedField&);
    const FocusedField& focusedField() const { return focused_; }

    /// Resolves this frame's drag against this frame's runs. Called at the end
    /// of the frame, once everything has been painted -- which is also when the
    /// runs exist. `blocked` suppresses it while something else owns the
    /// pointer, such as an open context menu.
    void trackMouse(Window&, bool blocked);

    /// Paints the highlight. Selected text is redrawn in the selection's own
    /// colours over a solid plate rather than washed: the runs are already on
    /// screen by the time the selection is known, and a translucent wash over
    /// white-on-blue reads as a smudge rather than as a selection.
    void paint(Canvas&) const;

    bool hasSelection() const;
    std::string selectedText() const;
    void selectAll();
    void clear();
    /// Selects a known span rather than one under the pointer.
    void select(TextPoint anchor, TextPoint caret);
    /// This frame's runs, in paint order.
    const std::vector<CapturedRun>& runs() const { return runs_; }
    /// True when a run was painted under `point` this frame.
    bool overText(Vec2) const;
    /// The same question against the PREVIOUS frame's runs. The input pass
    /// runs before anything is painted, so this frame's list is still empty
    /// when it has to decide whether a press landed on text.
    bool overTextLastFrame(Vec2) const;
    /// Whether a selection drag is under way.
    bool dragging() const { return dragging_; }

private:
    const CapturedRun* find(std::uint64_t key) const;
    /// The run and offset nearest `point`, or an invalid point when nothing
    /// was captured at all.
    TextPoint resolve(Vec2 point) const;
    /// The two ends in paint order, or false when either has gone away.
    bool ordered(std::size_t& from, std::size_t& to) const;

    std::vector<CapturedRun> runs_;
    /// The bands of the frame before, kept for `overTextLastFrame`.
    std::vector<Rect> previousBands_;
    std::unordered_map<std::uint64_t, std::size_t> byKey_;
    /// How many runs of each content hash have been seen this frame, which is
    /// what separates two identical labels drawn in two different rows.
    std::unordered_map<std::uint64_t, int> seen_;

    TextPoint anchor_;
    TextPoint caret_;
    bool dragging_ = false;

    FocusedField focused_;
};

/// Whether painted text is being recorded right now.
bool capturingText();

/// Turns capture on or off for a scope, restoring whatever it was.
struct TextCaptureScope {
    explicit TextCaptureScope(bool on);
    ~TextCaptureScope();
    TextCaptureScope(const TextCaptureScope&) = delete;
    TextCaptureScope& operator=(const TextCaptureScope&) = delete;

private:
    bool was_;
};

} // namespace flix::ui
