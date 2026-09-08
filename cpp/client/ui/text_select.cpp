#include "client/ui/text_select.h"

#include <algorithm>
#include <cmath>

#include "client/ui/draw.h"
#include "client/ui/text.h"

namespace flix::ui {

namespace {

bool gCapturing = false;

/// FNV-1a, which is all the mixing a key over a label and a coordinate needs.
std::uint64_t hashOf(const std::string& text, double penX, double size) {
    std::uint64_t h = 1469598103934665603ull;
    const auto eat = [&h](unsigned char byte) {
        h ^= byte;
        h *= 1099511628211ull;
    };
    for (const char c : text) eat(static_cast<unsigned char>(c));
    // Quarter-pixel resolution on x and whole points on the size. The vertical
    // position is deliberately NOT part of this: a panel that scrolls moves its
    // labels down the screen without making them different labels.
    const auto quarters = static_cast<std::int64_t>(std::llround(penX * 4.0));
    for (int i = 0; i < 8; ++i) eat(static_cast<unsigned char>((quarters >> (i * 8)) & 0xFF));
    eat(static_cast<unsigned char>(std::lround(size)));
    // Never 0: that is the "nowhere" key.
    return h == 0 ? 1 : h;
}

/// A blue plate with white glyphs on it, whatever the run was painted in.
/// Selected text is conventionally shown in the SELECTION's colours, and it has
/// to be: these runs sit on eight different panel colours, and a translucent
/// wash that reads on the settings grey disappears on the inventory blue.
TextStyle selectedStyle(double size, bool bold) {
    TextStyle style;
    style.size = size;
    style.bold = bold;
    style.fill = kPaper;
    style.stroke = kInk;
    style.strokeWidth = 2.0;
    style.baseline = Baseline::Alphabetic;
    style.roundJoin = true;
    return style;
}

} // namespace

double CapturedRun::width() const { return measure(text, size, bold); }

Rect CapturedRun::band() const {
    const double top = baselineY - ascent(size, bold);
    // descent() is negative, so this is below the baseline.
    const double bottom = baselineY - descent(size, bold);
    return {penX, top, width(), std::max(1.0, bottom - top)};
}

bool capturingText() { return gCapturing; }

TextCaptureScope::TextCaptureScope(bool on) : was_(gCapturing) { gCapturing = on; }
TextCaptureScope::~TextCaptureScope() { gCapturing = was_; }

TextSelect& TextSelect::instance() {
    static TextSelect layer;
    return layer;
}

void TextSelect::beginFrame() {
    previousBands_.clear();
    previousBands_.reserve(runs_.size());
    for (const CapturedRun& run : runs_) previousBands_.push_back(run.band());
    runs_.clear();
    byKey_.clear();
    seen_.clear();
    focused_ = FocusedField{};
}

void TextSelect::record(const std::string& text, double penX, double baselineY, double size,
                        bool bold) {
    if (text.empty()) return;
    // Nothing but blanks is not text a player can want; it would also make an
    // invisible run the pointer could catch on.
    if (text.find_first_not_of(" \t") == std::string::npos) return;

    const std::uint64_t content = hashOf(text, penX, size);
    // The same run painted twice is one run: every stroked-then-filled heading
    // in the game draws its glyphs two or three times over, and the chat draws
    // each token three times to build its outline.
    for (const CapturedRun& run : runs_) {
        if (run.text == text && std::fabs(run.penX - penX) < 0.25 &&
            std::fabs(run.baselineY - baselineY) < 0.25) {
            return;
        }
    }

    CapturedRun run;
    run.text = text;
    run.penX = penX;
    run.baselineY = baselineY;
    run.size = size;
    run.bold = bold;
    // Two rows carrying the same label at the same x are told apart by which
    // of them was painted first.
    const int occurrence = seen_[content]++;
    run.key = content + static_cast<std::uint64_t>(occurrence) * 0x9E3779B97F4A7C15ull;
    if (run.key == 0) run.key = 1;

    byKey_[run.key] = runs_.size();
    runs_.push_back(std::move(run));
}

void TextSelect::setFocusedField(const FocusedField& field) { focused_ = field; }

const CapturedRun* TextSelect::find(std::uint64_t key) const {
    const auto it = byKey_.find(key);
    return it == byKey_.end() ? nullptr : &runs_[it->second];
}

TextPoint TextSelect::resolve(Vec2 point) const {
    if (runs_.empty()) return {};

    // The run under the pointer, else the nearest one: a drag that leaves the
    // line it started on has to keep extending rather than stop dead.
    const CapturedRun* best = nullptr;
    double bestScore = 0;
    for (const CapturedRun& run : runs_) {
        const Rect band = run.band();
        const double dy = point.y < band.y      ? band.y - point.y
                          : point.y > band.bottom() ? point.y - band.bottom()
                                                    : 0.0;
        const double dx = point.x < band.x       ? band.x - point.x
                          : point.x > band.right() ? point.x - band.right()
                                                   : 0.0;
        // Vertical distance dominates: the run on the pointer's own line wins
        // over a nearer one two rows up.
        const double score = dy * 1000.0 + dx;
        if (!best || score < bestScore) {
            best = &run;
            bestScore = score;
        }
    }
    if (!best) return {};

    TextRun run;
    run.text = best->text;
    run.originX = best->penX;
    run.size = best->size;
    run.bold = best->bold;
    return {best->key, indexAtX(run, point.x)};
}

bool TextSelect::ordered(std::size_t& from, std::size_t& to) const {
    const auto a = byKey_.find(anchor_.run);
    const auto b = byKey_.find(caret_.run);
    if (a == byKey_.end() || b == byKey_.end()) return false;
    from = a->second;
    to = b->second;
    return true;
}

bool TextSelect::overText(Vec2 point) const {
    for (const CapturedRun& run : runs_) {
        if (run.band().contains(point)) return true;
    }
    return false;
}

bool TextSelect::overTextLastFrame(Vec2 point) const {
    for (const Rect& band : previousBands_) {
        if (band.contains(point)) return true;
    }
    return false;
}

bool TextSelect::hasSelection() const {
    std::size_t from = 0, to = 0;
    if (!anchor_.valid() || !caret_.valid() || !ordered(from, to)) return false;
    return from != to || anchor_.offset != caret_.offset;
}

void TextSelect::clear() {
    anchor_ = {};
    caret_ = {};
    dragging_ = false;
}

void TextSelect::select(TextPoint anchor, TextPoint caret) {
    anchor_ = anchor;
    caret_ = caret;
    dragging_ = false;
}

void TextSelect::selectAll() {
    if (runs_.empty()) return;
    anchor_ = {runs_.front().key, 0};
    caret_ = {runs_.back().key, runs_.back().text.size()};
    dragging_ = false;
}

void TextSelect::trackMouse(Window& window, bool blocked) {
    if (blocked) {
        dragging_ = false;
        return;
    }
    const Vec2 mouse{window.mouseX(), window.mouseY()};
    // A scroll slides the runs out from under the highlight. Rather than let
    // it point at whatever has taken their place, the selection goes.
    if (window.wheelDelta() != 0) {
        clear();
        return;
    }

    if (dragging_) {
        if (window.mouseDown(MouseButton::Left)) {
            const TextPoint at = resolve(mouse);
            if (at.valid()) caret_ = at;
        } else {
            dragging_ = false;
        }
    }

    if (!window.mousePressed(MouseButton::Left)) return;
    // A press that is not on text drops the selection; one that is starts a
    // new drag. Either way the press is NOT consumed -- the panels answer for
    // their own controls on the same press, and a label that happens to sit on
    // a clickable row must not swallow the click.
    if (!overText(mouse)) {
        clear();
        return;
    }
    const TextPoint at = resolve(mouse);
    if (!at.valid()) return;
    if (window.shiftHeld() && anchor_.valid()) {
        caret_ = at;
    } else {
        anchor_ = at;
        caret_ = at;
    }
    dragging_ = true;
}

std::string TextSelect::selectedText() const {
    std::size_t from = 0, to = 0;
    if (!hasSelection() || !ordered(from, to)) return {};
    TextPoint head = anchor_, tail = caret_;
    if (to < from) {
        std::swap(from, to);
        std::swap(head, tail);
    }

    std::string out;
    double lastBaseline = runs_[from].baselineY;
    for (std::size_t i = from; i <= to; ++i) {
        const CapturedRun& run = runs_[i];
        const std::size_t begin = i == from ? std::min(head.offset, run.text.size()) : 0;
        const std::size_t end = i == to ? std::min(tail.offset, run.text.size()) : run.text.size();
        if (end <= begin) continue;

        if (!out.empty()) {
            // A row of a panel is one run; a chat line is a dozen of them on
            // one baseline. So a change of baseline is a new line and anything
            // else is a word break -- the runs carry no spaces of their own,
            // the layout put them there.
            out += std::fabs(run.baselineY - lastBaseline) > run.size * 0.5 ? '\n' : ' ';
        }
        out += run.text.substr(begin, end - begin);
        lastBaseline = run.baselineY;
    }
    return out;
}

void TextSelect::paint(Canvas& canvas) const {
    std::size_t from = 0, to = 0;
    if (!hasSelection() || !ordered(from, to)) return;
    TextPoint head = anchor_, tail = caret_;
    if (to < from) {
        std::swap(from, to);
        std::swap(head, tail);
    }
    // The redraw below goes through ui::text, which would otherwise record the
    // selection's own glyphs as one more run to select.
    TextCaptureScope off(false);

    for (std::size_t i = from; i <= to; ++i) {
        const CapturedRun& run = runs_[i];
        const std::size_t begin = i == from ? std::min(head.offset, run.text.size()) : 0;
        const std::size_t end = i == to ? std::min(tail.offset, run.text.size()) : run.text.size();
        if (end <= begin) continue;

        const double x0 = run.penX + measure(run.text.substr(0, begin), run.size, run.bold);
        const double x1 = run.penX + measure(run.text.substr(0, end), run.size, run.bold);
        const Rect band = run.band();
        // A pixel of bleed either side, so a row of selected runs reads as one
        // block rather than as a dashed line of boxes.
        setFill(canvas, kSelectionPlate);
        canvas.fillRect(static_cast<float>(x0 - 1.0), static_cast<float>(band.y),
                        static_cast<float>(x1 - x0 + 2.0), static_cast<float>(band.h));
        text(canvas, run.text.substr(begin, end - begin), x0, run.baselineY,
             selectedStyle(run.size, run.bold));
    }
}

} // namespace flix::ui
