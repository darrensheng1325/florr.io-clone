#include "test.h"

#include "client/ui/text_input.h"

#include <string>

using flix::ui::editText;
using flix::ui::TextEditFrame;
using flix::ui::TextEditOptions;
using flix::ui::TextEditResult;
using flix::ui::TextSelection;
using flix::ui::utf8Next;
using flix::ui::utf8Prev;
using flix::ui::wordBegin;
using flix::ui::wordEnd;

// The one keyboard path every text field in the client runs through.
//
// These are frame-by-frame tests on purpose. An early version of this code
// re-seated the caret at the top of every frame with
// utf8Prev(utf8Next(caret)) -- identity in the middle of a string, but a whole
// character backwards at the END of one, which is where an append-only field's
// caret always is. Every field in the game typed in reverse, and none of the
// pure helpers was wrong, so nothing short of driving successive frames could
// have caught it.

namespace {

/// One frame carrying only typed text.
TextEditFrame typed(const std::string& text) {
    TextEditFrame frame;
    frame.typed = text;
    return frame;
}

/// Types `text` one character per frame, as a keyboard actually delivers it.
void typeOut(std::string& value, TextSelection& selection, const std::string& text,
             const TextEditOptions& options = {}) {
    for (const char c : text) editText(typed(std::string(1, c)), value, selection, options);
}

/// A selection anchored at `from` and dragged to `to`.
TextSelection spanning(std::size_t from, std::size_t to) {
    TextSelection selection;
    selection.anchor = from;
    selection.caret = to;
    return selection;
}

const std::string kEAcute = "\xc3\xa9";        // U+00E9, two bytes
const std::string kStar = "\xe2\x98\x85";      // U+2605, three bytes

} // namespace

// --- typing and the caret ---------------------------------------------------

TEST(typing_appends_in_order) {
    std::string value;
    TextSelection selection;
    typeOut(value, selection, "hello");
    CHECK_EQ(value, std::string("hello"));
    CHECK_EQ(selection.caret, value.size());
    CHECK(selection.empty());
}

TEST(an_idle_frame_leaves_the_caret_alone) {
    // The reversed-typing regression, in one assertion: with the caret at the
    // end of the value, a frame that carries nothing must not move it.
    std::string value = "abc";
    TextSelection selection;
    selection.collapse(value.size());
    editText(TextEditFrame{}, value, selection, {});
    CHECK_EQ(selection.caret, std::size_t{3});
    editText(TextEditFrame{}, value, selection, {});
    CHECK_EQ(selection.caret, std::size_t{3});
    CHECK_EQ(value, std::string("abc"));
}

TEST(typing_continues_after_a_multibyte_character) {
    std::string value;
    TextSelection selection;
    typeOut(value, selection, "a");
    editText(typed(kEAcute), value, selection, {});
    editText(typed(kStar), value, selection, {});
    editText(typed("z"), value, selection, {});
    CHECK_EQ(value, "a" + kEAcute + kStar + "z");
    CHECK_EQ(selection.caret, value.size());
}

TEST(typing_inserts_at_the_caret) {
    std::string value = "ac";
    TextSelection selection;
    selection.collapse(1);
    editText(typed("b"), value, selection, {});
    CHECK_EQ(value, std::string("abc"));
    CHECK_EQ(selection.caret, std::size_t{2});
}

TEST(a_caret_inside_a_character_snaps_to_its_start) {
    std::string value = "a" + kEAcute + "b";     // byte 2 is a continuation
    TextSelection selection;
    selection.collapse(2);
    editText(TextEditFrame{}, value, selection, {});
    CHECK_EQ(selection.caret, std::size_t{1});
}

TEST(a_caret_past_the_end_is_pulled_back) {
    std::string value = "abc";
    TextSelection selection;
    selection.collapse(99);
    editText(TextEditFrame{}, value, selection, {});
    CHECK_EQ(selection.caret, std::size_t{3});
}

TEST(backspace_erases_a_whole_character) {
    std::string value = "a" + kStar;
    TextSelection selection;
    selection.collapse(value.size());
    TextEditFrame frame;
    frame.backspace = true;
    CHECK(editText(frame, value, selection, {}).changed);
    CHECK_EQ(value, std::string("a"));
    CHECK_EQ(selection.caret, std::size_t{1});
}

TEST(delete_erases_forward) {
    std::string value = "a" + kStar + "b";
    TextSelection selection;
    selection.collapse(1);
    TextEditFrame frame;
    frame.erase = true;
    editText(frame, value, selection, {});
    CHECK_EQ(value, std::string("ab"));
    CHECK_EQ(selection.caret, std::size_t{1});
}

TEST(arrows_step_by_character_not_byte) {
    std::string value = "a" + kStar + "b";
    TextSelection selection;
    TextEditFrame right;
    right.right = true;
    editText(right, value, selection, {});
    CHECK_EQ(selection.caret, std::size_t{1});
    editText(right, value, selection, {});
    CHECK_EQ(selection.caret, std::size_t{4});     // the whole 3-byte sequence
    TextEditFrame left;
    left.left = true;
    editText(left, value, selection, {});
    CHECK_EQ(selection.caret, std::size_t{1});
}

// --- selection --------------------------------------------------------------

TEST(shift_arrows_grow_a_selection_and_a_bare_arrow_drops_it) {
    std::string value = "hello";
    TextSelection selection;

    TextEditFrame shiftRight;
    shiftRight.right = true;
    shiftRight.shift = true;
    editText(shiftRight, value, selection, {});
    editText(shiftRight, value, selection, {});
    CHECK_EQ(selection.begin(), std::size_t{0});
    CHECK_EQ(selection.end(), std::size_t{2});
    CHECK_EQ(selection.of(value), std::string("he"));

    // Unshifted, the caret goes to the selection's far edge rather than one
    // step past where it happened to be.
    TextEditFrame right;
    right.right = true;
    editText(right, value, selection, {});
    CHECK(selection.empty());
    CHECK_EQ(selection.caret, std::size_t{2});
}

TEST(a_selection_reverses_through_its_anchor) {
    std::string value = "hello";
    TextSelection selection;
    selection.collapse(3);

    TextEditFrame shiftLeft;
    shiftLeft.left = true;
    shiftLeft.shift = true;
    editText(shiftLeft, value, selection, {});
    editText(shiftLeft, value, selection, {});
    CHECK_EQ(selection.of(value), std::string("el"));
    CHECK_EQ(selection.caret, std::size_t{1});
    CHECK_EQ(selection.anchor, std::size_t{3});
}

TEST(shift_home_and_end_select_to_the_edges) {
    std::string value = "abcdef";
    TextSelection selection;
    selection.collapse(3);

    TextEditFrame shiftHome;
    shiftHome.home = true;
    shiftHome.shift = true;
    editText(shiftHome, value, selection, {});
    CHECK_EQ(selection.of(value), std::string("abc"));

    selection.collapse(3);
    TextEditFrame shiftEnd;
    shiftEnd.end = true;
    shiftEnd.shift = true;
    editText(shiftEnd, value, selection, {});
    CHECK_EQ(selection.of(value), std::string("def"));
}

TEST(select_all_takes_the_whole_value) {
    std::string value = "abcdef";
    TextSelection selection;
    TextEditFrame frame;
    frame.selectAll = true;
    editText(frame, value, selection, {});
    CHECK_EQ(selection.of(value), value);
}

TEST(typing_over_a_selection_replaces_it) {
    std::string value = "hello world";
    TextSelection selection = spanning(0, 5);
    editText(typed("bye"), value, selection, {});
    CHECK_EQ(value, std::string("bye world"));
    CHECK_EQ(selection.caret, std::size_t{3});
    CHECK(selection.empty());
}

TEST(a_paste_over_a_full_field_replaces_rather_than_being_refused) {
    // Room is measured against what is left AFTER the selection goes, or
    // selecting a full field and pasting would do nothing at all.
    TextEditOptions options;
    options.maxBytes = 5;
    std::string value = "aaaaa";
    TextSelection selection;
    selection.selectAll(value);
    TextEditFrame frame;
    frame.pasted = "bbbbb";
    editText(frame, value, selection, options);
    CHECK_EQ(value, std::string("bbbbb"));
}

TEST(backspace_and_delete_take_the_selection_whole) {
    std::string value = "hello world";
    TextSelection selection = spanning(5, 11);
    TextEditFrame frame;
    frame.backspace = true;
    editText(frame, value, selection, {});
    CHECK_EQ(value, std::string("hello"));
    CHECK_EQ(selection.caret, std::size_t{5});

    value = "hello world";
    selection = spanning(0, 6);
    TextEditFrame del;
    del.erase = true;
    editText(del, value, selection, {});
    CHECK_EQ(value, std::string("world"));
    CHECK_EQ(selection.caret, std::size_t{0});
}

TEST(copy_takes_the_selection_when_there_is_one) {
    std::string value = "hello world";
    TextSelection selection = spanning(6, 11);
    TextEditFrame frame;
    frame.copy = true;
    const TextEditResult result = editText(frame, value, selection, {});
    CHECK_EQ(result.clipboard, std::string("world"));
    CHECK(!result.changed);
    CHECK_EQ(value, std::string("hello world"));
}

TEST(copy_falls_back_to_the_whole_field_with_nothing_selected) {
    std::string value = "code-42";
    TextSelection selection;
    selection.collapse(value.size());
    TextEditFrame frame;
    frame.copy = true;
    CHECK_EQ(editText(frame, value, selection, {}).clipboard, std::string("code-42"));
}

TEST(cut_removes_what_it_copied) {
    std::string value = "hello world";
    TextSelection selection = spanning(0, 6);
    TextEditFrame frame;
    frame.cut = true;
    const TextEditResult result = editText(frame, value, selection, {});
    CHECK_EQ(result.clipboard, std::string("hello "));
    CHECK(result.changed);
    CHECK_EQ(value, std::string("world"));
    CHECK_EQ(selection.caret, std::size_t{0});

    // With nothing selected it takes the field, as copy does.
    selection.collapse(value.size());
    editText(frame, value, selection, {});
    CHECK_EQ(value, std::string(""));
}

TEST(a_masked_field_refuses_to_copy_itself) {
    TextEditOptions options;
    options.copyable = false;
    std::string value = "hunter2";
    TextSelection selection;
    selection.selectAll(value);

    TextEditFrame copy;
    copy.copy = true;
    CHECK_EQ(editText(copy, value, selection, options).clipboard, std::string(""));

    TextEditFrame cut;
    cut.cut = true;
    editText(cut, value, selection, options);
    CHECK_EQ(value, std::string("hunter2"));

    // Pasting INTO one is still allowed: reading the field back out is what a
    // mask is there to stop.
    selection.collapse(value.size());
    TextEditFrame paste;
    paste.pasted = "!";
    editText(paste, value, selection, options);
    CHECK_EQ(value, std::string("hunter2!"));
}

TEST(a_word_is_a_run_of_word_bytes_or_a_run_of_anything_else) {
    const std::string s = "one two-three";
    CHECK_EQ(wordBegin(s, 5), std::size_t{4});
    CHECK_EQ(wordEnd(s, 5), std::size_t{7});
    // A boundary is ambiguous, and the character BEFORE it wins when it is a
    // word one -- so a click at the end of "two" takes "two", not the dash.
    CHECK_EQ(wordBegin(s, 7), std::size_t{4});
    CHECK_EQ(wordEnd(s, 7), std::size_t{7});
    CHECK_EQ(wordBegin(s, 3), std::size_t{0});
    CHECK_EQ(wordEnd(s, 0), std::size_t{3});
    // A run of separators is still a "word" when nothing else is beside it.
    const std::string gaps = "a  b";
    CHECK_EQ(wordBegin(gaps, 2), std::size_t{1});
    CHECK_EQ(wordEnd(gaps, 2), std::size_t{3});
}

// --- filters and limits -----------------------------------------------------

TEST(the_byte_cap_stops_typing_and_trims_a_paste) {
    TextEditOptions options;
    options.maxBytes = 4;

    std::string value;
    TextSelection selection;
    typeOut(value, selection, "abcdef", options);
    CHECK_EQ(value, std::string("abcd"));

    // A paste is cut on a character boundary rather than mid-sequence, and it
    // is TRUNCATED rather than sieved: the first character that will not fit
    // ends the paste, instead of being skipped so a later, smaller one can
    // slip in ahead of the text it followed.
    TextEditOptions wider;
    wider.maxBytes = 6;
    std::string room4 = "ab";
    TextSelection at4;
    at4.collapse(room4.size());
    TextEditFrame frame;
    frame.pasted = kStar + "z";
    editText(frame, room4, at4, wider);
    CHECK_EQ(room4, "ab" + kStar + "z");
    CHECK_EQ(at4.caret, room4.size());

    wider.maxBytes = 5;
    std::string room3 = "ab";
    TextSelection at3;
    at3.collapse(room3.size());
    editText(frame, room3, at3, wider);
    CHECK_EQ(room3, "ab" + kStar);

    std::string room2 = "ab";
    TextSelection at2;
    at2.collapse(room2.size());
    editText(frame, room2, at2, options);
    CHECK_EQ(room2, std::string("ab"));
}

TEST(ascii_only_drops_wider_characters) {
    TextEditOptions options;
    options.asciiOnly = true;
    std::string value;
    TextSelection selection;
    TextEditFrame frame;
    frame.pasted = "na" + kEAcute + "ve!";
    editText(frame, value, selection, options);
    CHECK_EQ(value, std::string("nave!"));
    CHECK_EQ(selection.caret, value.size());
}

TEST(a_paste_is_filtered_for_the_kind_of_field) {
    TextEditFrame frame;
    frame.pasted = "one\r\ntwo\tthree";

    std::string single;
    TextSelection selection;
    editText(frame, single, selection, {});
    CHECK_EQ(single, std::string("onetwothree"));

    TextEditOptions options;
    options.multiline = true;
    std::string multi;
    TextSelection other;
    editText(frame, multi, other, options);
    CHECK_EQ(multi, std::string("one\ntwothree"));
}

TEST(control_characters_never_reach_the_value) {
    std::string value;
    TextSelection selection;
    TextEditFrame frame;
    frame.typed = std::string("a\tb\x01") + "\x7f" + "c";
    editText(frame, value, selection, {});
    CHECK_EQ(value, std::string("abc"));
}

// --- multiline --------------------------------------------------------------

TEST(enter_is_a_newline_only_in_a_multiline_field) {
    TextEditFrame frame;
    frame.enter = true;

    std::string single = "ab";
    TextSelection selection;
    selection.collapse(single.size());
    CHECK(!editText(frame, single, selection, {}).changed);
    CHECK_EQ(single, std::string("ab"));

    TextEditOptions options;
    options.multiline = true;
    std::string multi = "ab";
    TextSelection other;
    other.collapse(multi.size());
    CHECK(editText(frame, multi, other, options).changed);
    CHECK_EQ(multi, std::string("ab\n"));
}

TEST(home_and_end_are_the_lines_ends_when_multiline) {
    TextEditOptions options;
    options.multiline = true;
    std::string value = "one\ntwo";
    TextSelection selection;
    selection.collapse(5);                  // inside "two"

    TextEditFrame home;
    home.home = true;
    editText(home, value, selection, options);
    CHECK_EQ(selection.caret, std::size_t{4});

    TextEditFrame end;
    end.end = true;
    editText(end, value, selection, options);
    CHECK_EQ(selection.caret, std::size_t{7});

    // A single-line field reads both as the value's ends instead.
    selection.collapse(2);
    editText(home, value, selection, {});
    CHECK_EQ(selection.caret, std::size_t{0});
    editText(end, value, selection, {});
    CHECK_EQ(selection.caret, value.size());
}

TEST(up_and_down_keep_the_column) {
    TextEditOptions options;
    options.multiline = true;
    std::string value = "abcd\nef\nghij";
    TextSelection selection;
    selection.collapse(3);                  // "abc|d"

    TextEditFrame down;
    down.down = true;
    editText(down, value, selection, options);
    // The middle line is shorter than the column, so the caret parks at its end.
    CHECK_EQ(selection.caret, std::size_t{7});

    editText(down, value, selection, options);
    CHECK_EQ(selection.caret, std::size_t{10});       // "gh|ij", column 2

    TextEditFrame up;
    up.up = true;
    editText(up, value, selection, options);
    CHECK_EQ(selection.caret, std::size_t{7});
}

TEST(shift_down_selects_across_lines) {
    TextEditOptions options;
    options.multiline = true;
    std::string value = "one\ntwo\nthree";
    TextSelection selection;
    TextEditFrame frame;
    frame.down = true;
    frame.shift = true;
    editText(frame, value, selection, options);
    CHECK_EQ(selection.of(value), std::string("one\n"));
}

TEST(up_and_down_do_nothing_in_a_single_line_field) {
    std::string value = "abc";
    TextSelection selection;
    selection.collapse(1);
    TextEditFrame frame;
    frame.up = true;
    editText(frame, value, selection, {});
    CHECK_EQ(selection.caret, std::size_t{1});
}

TEST(utf8_steps_clamp_at_both_ends) {
    const std::string s = "a" + kEAcute + "b";     // 4 bytes
    CHECK_EQ(utf8Prev(s, 0), std::size_t{0});
    CHECK_EQ(utf8Next(s, s.size()), s.size());
    CHECK_EQ(utf8Prev(s, 99), std::size_t{3});
    CHECK_EQ(utf8Prev(s, 3), std::size_t{1});
    CHECK_EQ(utf8Next(s, 1), std::size_t{3});
}
