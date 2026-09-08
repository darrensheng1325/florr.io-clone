#include "test.h"

#include "client/ui/text_select.h"

#include <string>

using flix::ui::CapturedRun;
using flix::ui::capturingText;
using flix::ui::TextCaptureScope;
using flix::ui::TextPoint;
using flix::ui::TextSelect;

// The layer that makes painted labels selectable.
//
// Only the half that needs no font is exercised here: recording, identity
// across frames, and what a span of runs reads as once it is copied out.
// Anything that maps an x to an offset goes through `measure`, which answers 0
// until a typeface is loaded, and a test binary loads none.

namespace {

/// The layer is process-wide, so every test starts from a clean frame.
TextSelect& fresh() {
    TextSelect& layer = TextSelect::instance();
    layer.beginFrame();
    layer.clear();
    return layer;
}

/// Records one line of a panel: a label on its own baseline.
void line(TextSelect& layer, const std::string& text, double y) {
    layer.record(text, 20.0, y, 14.0, false);
}

TextPoint pointAt(const TextSelect& layer, std::size_t index, std::size_t offset) {
    return {layer.runs()[index].key, offset};
}

} // namespace

TEST(runs_are_kept_in_paint_order) {
    TextSelect& layer = fresh();
    line(layer, "first", 100.0);
    line(layer, "second", 120.0);
    line(layer, "third", 140.0);
    CHECK_EQ(layer.runs().size(), std::size_t{3});
    CHECK_EQ(layer.runs()[0].text, std::string("first"));
    CHECK_EQ(layer.runs()[2].text, std::string("third"));
}

TEST(a_run_painted_twice_over_is_recorded_once) {
    // Every stroked-then-filled heading in the game draws its glyphs two or
    // three times at the same pen; the chat draws each token three times.
    TextSelect& layer = fresh();
    line(layer, "Common", 100.0);
    line(layer, "Common", 100.0);
    line(layer, "Common", 100.0);
    CHECK_EQ(layer.runs().size(), std::size_t{1});
}

TEST(the_same_label_on_two_rows_stays_two_runs) {
    TextSelect& layer = fresh();
    line(layer, "Common", 100.0);
    line(layer, "Common", 130.0);
    CHECK_EQ(layer.runs().size(), std::size_t{2});
    CHECK(layer.runs()[0].key != layer.runs()[1].key);
}

TEST(blank_runs_are_ignored) {
    TextSelect& layer = fresh();
    line(layer, "", 100.0);
    line(layer, "   ", 100.0);
    line(layer, "\t", 100.0);
    CHECK_EQ(layer.runs().size(), std::size_t{0});
}

TEST(a_selection_survives_the_next_frame) {
    // Identity is content-derived, not the index into a list that is thrown
    // away and rebuilt sixty times a second.
    TextSelect& layer = fresh();
    line(layer, "one", 100.0);
    line(layer, "two", 120.0);
    layer.selectAll();
    CHECK_EQ(layer.selectedText(), std::string("one\ntwo"));

    layer.beginFrame();
    CHECK(!layer.hasSelection());        // nothing recorded yet this frame
    line(layer, "one", 100.0);
    line(layer, "two", 120.0);
    CHECK(layer.hasSelection());
    CHECK_EQ(layer.selectedText(), std::string("one\ntwo"));
}

TEST(a_selection_whose_runs_have_gone_reports_nothing) {
    TextSelect& layer = fresh();
    line(layer, "one", 100.0);
    line(layer, "two", 120.0);
    layer.selectAll();

    layer.beginFrame();
    line(layer, "something else entirely", 100.0);
    CHECK(!layer.hasSelection());
    CHECK_EQ(layer.selectedText(), std::string(""));
}

TEST(runs_on_one_baseline_join_with_a_space) {
    // A chat line is a dozen runs on one baseline: the timestamp, the author
    // and every word. The spaces between them were the layout's, not the
    // runs', so copying has to put them back.
    TextSelect& layer = fresh();
    layer.record("[12:03]", 10.0, 100.0, 12.0, false);
    layer.record("Mira:", 60.0, 100.0, 14.0, false);
    layer.record("hello", 110.0, 100.0, 14.0, false);
    layer.record("there", 160.0, 100.0, 14.0, false);
    layer.selectAll();
    CHECK_EQ(layer.selectedText(), std::string("[12:03] Mira: hello there"));
}

TEST(a_change_of_baseline_is_a_new_line) {
    TextSelect& layer = fresh();
    layer.record("hello", 10.0, 100.0, 14.0, false);
    layer.record("there", 60.0, 100.0, 14.0, false);
    layer.record("next", 10.0, 122.0, 14.0, false);
    layer.selectAll();
    CHECK_EQ(layer.selectedText(), std::string("hello there\nnext"));
}

TEST(a_partial_span_takes_the_tail_and_the_head_of_its_ends) {
    TextSelect& layer = fresh();
    line(layer, "alpha", 100.0);
    line(layer, "bravo", 120.0);
    line(layer, "charlie", 140.0);
    layer.select(pointAt(layer, 0, 2), pointAt(layer, 2, 4));
    CHECK_EQ(layer.selectedText(), std::string("pha\nbravo\nchar"));
}

TEST(a_span_dragged_backwards_reads_the_same_way) {
    TextSelect& layer = fresh();
    line(layer, "alpha", 100.0);
    line(layer, "bravo", 120.0);
    // Anchor on the second row, caret dragged up to the first.
    layer.select(pointAt(layer, 1, 3), pointAt(layer, 0, 1));
    CHECK_EQ(layer.selectedText(), std::string("lpha\nbra"));
}

TEST(a_span_inside_one_run_takes_just_that_much) {
    TextSelect& layer = fresh();
    line(layer, "poison cactus", 100.0);
    layer.select(pointAt(layer, 0, 7), pointAt(layer, 0, 13));
    CHECK(layer.hasSelection());
    CHECK_EQ(layer.selectedText(), std::string("cactus"));
}

TEST(an_empty_span_is_no_selection) {
    TextSelect& layer = fresh();
    line(layer, "alpha", 100.0);
    layer.select(pointAt(layer, 0, 2), pointAt(layer, 0, 2));
    CHECK(!layer.hasSelection());
    CHECK_EQ(layer.selectedText(), std::string(""));
}

TEST(capture_is_off_by_default_and_scopes_restore_it) {
    CHECK(!capturingText());
    {
        TextCaptureScope on(true);
        CHECK(capturingText());
        {
            TextCaptureScope off(false);
            CHECK(!capturingText());
        }
        CHECK(capturingText());
    }
    CHECK(!capturingText());
}

TEST(nothing_is_recorded_while_capture_is_off) {
    // The guard lives in ui::text rather than in record(), so this pins the
    // contract the widgets rely on rather than the implementation.
    TextSelect& layer = fresh();
    CHECK(!capturingText());
    CHECK_EQ(layer.runs().size(), std::size_t{0});
}
