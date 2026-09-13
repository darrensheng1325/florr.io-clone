// The star shop.
//
// Three tabs over one card: the store's ten rotating offers, the challenges
// that pay the stars to buy them with, and the code redeemer. The store is the
// panel -- a fixed two rows of five cards that change on the hour, each a
// petal tile over a gold price pill, some of them wearing a discount ribbon --
// and the other two tabs are what feeds it.
//
// Laid out against a reference screenshot rather than against the browser
// build's CSS, which is why the numbers here are literal pixel offsets from
// the card's own edges: they were measured off that shot at its own size, and
// the panel is drawn at that size.
//
// The offers themselves are not this file's: `shopOffers` in shared/game/shop.h
// derives them from the clock, and the server derives the same ten to price
// what is clicked. This panel only draws them and names which one was pressed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "svg.h"

#include "client/ui/item_tile.h"
#include "client/ui/menu_icons.h"
#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "client/ui/text.h"
#include "client/ui/text_input.h"
#include "shared/game/config.h"
#include "shared/game/shop.h"

namespace flix {

using namespace flix::ui;

namespace {

// --- the card ---------------------------------------------------------------

/// The card wears the overlay frame every other corner panel does -- a rounded
/// rect in the skin's border green with a square body inset into it. The
/// reference shot has no frame here, but its card is the only one in the game
/// without one, and a frameless green slab beside the framed settings and
/// bestiary cards reads as a panel that failed to draw.
constexpr double kCardRadius = kOverlayRadius;

/// The inset both light plates keep from that edge, top and bottom alike.
constexpr double kPlateInset = 8.0;
constexpr double kPlateRadius = 6.0;

constexpr double kHeaderHeight = 50.0;

/// The tab row sits on the card's own body between the two plates.
constexpr double kTabTop = 65.0;
constexpr double kTabHeight = 35.0;
constexpr double kTabWidth = 104.0;
constexpr double kTabGap = 6.0;
constexpr double kTabRadius = 7.0;
constexpr double kContentTop = 108.0;

// --- the store's grid -------------------------------------------------------

/// One offer's dark backing block, and the tile and price pill on it. Every
/// offset here is from the block's own top-left corner.
constexpr double kBlockWidth = 120.0;
constexpr double kBlockHeight = 140.0;
constexpr double kBlockGap = 20.0;
constexpr double kBlockRadius = 8.0;
constexpr double kGridTop = 174.0;
constexpr double kTileWidth = 82.0;
constexpr double kTileHeight = 80.0;
constexpr double kTileTop = 9.0;
constexpr double kPricePillWidth = 104.0;
constexpr double kPricePillHeight = 34.0;
constexpr double kPricePillTop = 98.0;

/// The discount ribbon, centred on the tile's top-right corner and tipped
/// clockwise so it reads as a tag stuck on the card rather than part of it.
constexpr double kRibbonWidth = 57.0;
constexpr double kRibbonHeight = 24.0;
constexpr double kRibbonRadians = 24.0 * kPi / 180.0;

/// The star balance in the bottom-right corner, and the star glyphs on it.
constexpr double kBalanceWidth = 83.0;
constexpr double kBalanceHeight = 38.0;
constexpr double kBalanceStar = 26.0;
constexpr double kPriceStar = 24.0;

/// The store tab's own heading and the rotation line under it, measured off
/// the reference: "Today's offers:" is 137x20 there and came out 127x18 at 18,
/// and the line under it a pixel short of the shot's cap height at 13. Every
/// other label on the panel already matched, so these two are the sizes that
/// were guessed rather than measured the first time round.
constexpr double kOffersHeadingSize = 19.5;
constexpr double kOffersSubSize = 14.0;

/// Glyph outlines on this panel, as a fraction of the font size. Lighter than
/// the client's own kTextStrokeRatio: every label here sits on a flat green
/// plate and never has to survive over game content, so it is outlined only
/// enough to separate it from the plate.
constexpr double kShopStrokeRatio = 0.10;

constexpr double kScrollbarWidth = 6.0;

constexpr std::size_t kCodeMaxLength = 64;

/// One wheel notch in a browser is 100 CSS px of `deltaY`, and the shop adds
/// that delta raw. Anything else scrolls a different distance per notch than
/// the panel this one is copying.
constexpr double kWheelPixels = 100.0;

/// The greens the shot is made of, beside the card's own pair in
/// menu_theme.h: the light plate the header and the content sit on, the tab
/// frame, and the Lock pill.
constexpr std::uint32_t kShopPlate = 0x7fef6du;
constexpr std::uint32_t kTabBorder = 0x539d47u;
constexpr std::uint32_t kLockFill = 0x5baf50u;
constexpr std::uint32_t kLockBorder = 0x519d44u;

/// The price pill under every offer, which is also its buy button.
constexpr std::uint32_t kPriceFill = 0xF7E04Bu;
constexpr std::uint32_t kPriceBorder = 0xC8B63Bu;

/// The card's one hover/press treatment: a wash of black over a control's
/// FACE, inside its frame, never a recolour or a lift. The tabs wear the light
/// one on hover and the heavy one while selected; the buy pill wears the heavy
/// one on hover, so "this is the thing that acts" reads the same in both
/// places.
constexpr double kTintHover = 0.05;
constexpr double kTintSelected = 0.10;
/// The inset of that face, and the corner it takes -- `inlaid`'s own.
constexpr double kControlBorder = 4.0;

constexpr std::uint32_t kCodeBlue = 0x4A90E2u;
constexpr std::uint32_t kCodeBlueHover = 0x5FA1EDu;
constexpr std::uint32_t kCodeBlueLit = 0x7EB9F7u;
constexpr std::uint32_t kGold = 0xFFD700u;
constexpr std::uint32_t kAlert = 0xE74C3Cu;

// --- the confirm box --------------------------------------------------------

/// How far the card dims behind it.
constexpr double kModalScrim = 0.39;
/// Centred on the card, at the size the shot measures. Only an alert departs
/// from the height, and only to fit its message.
constexpr double kModalWidth = 280.0;
constexpr double kModalHeight = 288.0;
constexpr double kModalBorder = 6.0;
constexpr double kModalRadius = 4.0;
constexpr double kModalBodySize = 14.0;
/// An alert's message: where the first line sits and how far apart they are.
constexpr double kModalTextTop = 34.0;
constexpr double kModalLine = 22.0;
/// The row along the bottom: the gold buy pill, then Cancel.
constexpr double kModalButtonHeight = 32.0;
constexpr double kModalBuyWidth = 84.0;
constexpr double kModalCancelWidth = 89.0;
constexpr double kModalButtonGap = 7.0;
constexpr double kModalBottomPad = 22.0;
constexpr double kModalStar = 22.0;
constexpr std::uint32_t kModalGrey = 0x666666u;
constexpr std::uint32_t kModalGreyBorder = 0x535353u;

/// The challenge tiers, with their card colour stored rather than derived:
/// the shop overrides the rarity palette for unique, which is white in the
/// tier table and purple here.
struct StarChallenge {
    Rarity tier;
    int stars;
    std::uint32_t color;
};

const std::array<StarChallenge, 5> kChallenges = {{
    {Rarity::Mythic, 1, 0x1FDBDEu},
    {Rarity::Ultra, 5, 0xDE1F65u},
    {Rarity::Super, 25, 0x2BFFA4u},
    {Rarity::Unique, 100, 0xBF00FFu},
    {Rarity::Apex, 250, 0xFF00FFu},
}};

/// One of the stars that rain down the screen when a code is redeemed.
///
/// The reference drops these on the GAME canvas, underneath the shop's own, so
/// they pass behind the card. Drawn here before anything else in the panel for
/// the same reason.
struct FallingStar {
    Vec2 at;
    double vy = 0;          ///< pixels per frame at 60fps, as the reference stores it
    double rotation = 0;
    double spin = 0;
    double size = 0;
    double alpha = 0;
    double life = 0;        ///< seconds left
    double maxLife = 0;
};

constexpr std::size_t kFallingStars = 20;

// ---------------------------------------------------------------------------
// State the panel cannot hold
// ---------------------------------------------------------------------------

/// The code field, the modal and the per-tab scroll offsets.
///
/// `ShopPanel` is declared in menus.h, which this file does not own, so this
/// state lives beside the panel and is keyed on the instance rather than being
/// a file-level singleton -- two shops would otherwise share one code field.
/// It belongs in the class the moment the header can take it.
struct ShopState {
    enum class Modal : std::uint8_t { None, Confirm, Alert };

    std::string code;
    /// Caret, selection, focus and the blink's phase. The phase is anchored to
    /// the last edit rather than taken modulo wall time, so the caret is solid
    /// the instant a key lands instead of blinking out mid-keystroke.
    ui::TextFieldState field;

    Modal modal = Modal::None;
    std::string message;
    std::uint32_t messageColor = kPaper;
    std::uint16_t pendingPetal = kNoPetal;
    Rarity pendingRarity = Rarity::Common;
    /// Which card of the rotation the confirm modal is holding, so the buy
    /// carries the offer's own discounted price rather than the ladder's --
    /// and what that card costs, which the box prints on its buy button.
    int pendingSlot = -1;
    double pendingPrice = 0;

    /// Indexed by tab. Kept across a tab switch and across a close, as the
    /// browser does: a player who scrolled down to the mythic prices does not
    /// want to find the top of the list again on the way back.
    std::array<double, 3> scroll{{0.0, 0.0, 0.0}};

    /// The rotation the cards below were generated for, so the store is built
    /// once an hour rather than once a frame. `INT64_MIN` is "nothing yet",
    /// which a rotation index cannot otherwise be -- an empty vector is not,
    /// because a client with no petals loaded would rebuild it every frame.
    std::int64_t offersRotation = INT64_MIN;
    std::vector<ShopOffer> offers;

    std::vector<FallingStar> stars;
    Rng rng;
};

/// The run the code field paints, scrolled so the caret stays inside the box.
/// The paint and the hit test both take it from here, or a click would land on
/// a glyph other than the one under the pointer.
TextRun codeRun(Rect box, const std::string& code, const ui::TextFieldState& state) {
    TextRun run;
    run.text = code;
    run.size = 16.0;
    const double toCaret =
        measure(code.substr(0, std::min(state.selection.caret, code.size())), run.size, false);
    run.originX = box.x + 8.0 - std::max(0.0, toCaret - (box.w - 16.0));
    return run;
}

ShopState& stateFor(const ShopPanel* panel) {
    static std::unordered_map<const ShopPanel*, ShopState> states;
    return states[panel];
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

void roundedPath(Canvas& canvas, Rect r, double radius) {
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                     static_cast<float>(r.h),
                     static_cast<float>(std::min(radius, std::min(r.w, r.h) * 0.5)));
}

void fillRounded(Canvas& canvas, Rect r, double radius, std::uint32_t rgb, double alpha = 1.0) {
    if (r.w <= 0 || r.h <= 0) return;
    setFill(canvas, rgb, alpha);
    roundedPath(canvas, r, radius);
    canvas.fill();
}

/// A stroke CENTRED on the box, which is what `ctx.stroke()` after `roundRect`
/// does in the reference -- half of it hangs outside `r`. The shared `plate()`
/// insets instead, and every border here would land a pixel in from where the
/// browser puts it.
void strokeRounded(Canvas& canvas, Rect r, double radius, std::uint32_t rgb, double width,
                   double alpha = 1.0) {
    if (r.w <= 0 || r.h <= 0) return;
    canvas.save();
    canvas.setLineJoin("round");
    canvas.setLineWidth(static_cast<float>(width));
    setStroke(canvas, rgb, alpha);
    roundedPath(canvas, r, radius);
    canvas.stroke();
    canvas.restore();
}

/// The hover/press wash: `alpha` of black over the face `inlaid` left inside
/// this control's frame, so the frame itself keeps its colour.
void tintFace(Canvas& canvas, Rect control, double radius, double alpha) {
    fillRounded(canvas,
                {control.x + kControlBorder, control.y + kControlBorder,
                 control.w - kControlBorder * 2, control.h - kControlBorder * 2},
                radius - 2.0, kInk, alpha);
}

/// The browser sets `lineJoin = 'round'` once in drawHeader and never puts it
/// back, so every glyph outline on this panel is round-joined.
///
/// A negative `strokeWidth` asks for the panel's own proportional outline;
/// 0 turns it off. Proportional and not the flat 3px this used to pass at
/// every size: solved against the reference, the outline that matches is a
/// tenth of the font size at 24px, at 19.5, at 16 and at 14 alike, and one
/// flat width across that range is half again too heavy on the small labels
/// while being too light on the headings. Over-outlined text reads as a
/// heavier, greyer typeface rather than as a thicker outline, which is what
/// "the shop's colours are off" turned out to be.
TextStyle shopText(double size, bool bold, std::uint32_t fill, double strokeWidth) {
    TextStyle style;
    style.size = size;
    style.bold = bold;
    style.fill = fill;
    style.strokeWidth = strokeWidth < 0 ? size * kShopStrokeRatio : strokeWidth;
    style.roundJoin = true;
    return style;
}

/// `text()` with an alpha on the FILL alone.
///
/// The reference fades a label by filling it `rgba(255,255,255,a)` over an
/// OPAQUE black outline. Wrapping the whole of `text()` in a globalAlpha takes
/// the outline with it, and a faded outline over this card's green is not
/// black any more -- an inactive tab's darkest pixel measures #184a21 that way
/// against the reference's #000000, and the label reads green-edged.
void fadedText(Canvas& canvas, const std::string& s, double x, double y, const TextStyle& style,
               double fillAlpha) {
    if (s.empty() || !Fonts::ready()) return;

    double penX = x;
    if (style.align != Align::Left) {
        const double width = measure(s, style.size, style.bold);
        penX -= style.align == Align::Centre ? width * 0.5 : width;
    }
    double penY = y;
    switch (style.baseline) {
        case Baseline::Top: penY += ascent(style.size, style.bold); break;
        case Baseline::Bottom: penY += descent(style.size, style.bold); break;
        case Baseline::Alphabetic: break;
        default:
            penY += (ascent(style.size, style.bold) + descent(style.size, style.bold)) * 0.5;
            break;
    }

    paintRun(canvas, s, penX, penY, style, 1.0, fillAlpha);
}

/// The star glyph, from the same game-icons.net document the browser recolours
/// and draws as an image. Compiled once; the glyph is a fat five-point star
/// with concave limbs that a regular polygon does not resemble.
///
/// Two of them, because the shop needs both: gold where the star is the
/// currency against a neutral ground, and the artwork's own white where it
/// sits INSIDE something already gold -- a gold star on a gold pill is a
/// silhouette of nothing.
const SvgDocument* starDocument(bool gold) {
    const auto compile = [](bool asGold) {
        const int index = menuIconIndex("stars");
        if (index < 0) return SvgDocument::fromString(std::string());
        std::string svg = kMenuIcons[index].svg;
        if (asGold) {
            const std::string white = "fill=\"#fff\"";
            const std::string amber = "fill=\"#ffd700\"";
            for (std::size_t at = svg.find(white); at != std::string::npos;
                 at = svg.find(white, at + amber.size())) {
                svg.replace(at, white.size(), amber);
            }
        }
        return SvgDocument::fromString(svg);
    };
    static const SvgDocument goldDoc = compile(true);
    static const SvgDocument paperDoc = compile(false);
    const SvgDocument& doc = gold ? goldDoc : paperDoc;
    return doc.empty() ? nullptr : &doc;
}

/// A star, drawn rather than typed: the glyph is not in the shipped face, and
/// the shop is the one screen where the currency has to be unmistakable. Only
/// reached when the SVG failed to compile.
void drawStar(Canvas& canvas, Vec2 at, double radius, std::uint32_t rgb) {
    setFill(canvas, rgb);
    canvas.beginPath();
    for (int i = 0; i < 10; ++i) {
        const double r = (i % 2 == 0) ? radius : radius * 0.45;
        const Vec2 p = at + Vec2::fromAngle(-kPi * 0.5 + i * kPi / 5.0, r);
        if (i == 0) canvas.moveTo(static_cast<float>(p.x), static_cast<float>(p.y));
        else canvas.lineTo(static_cast<float>(p.x), static_cast<float>(p.y));
    }
    canvas.closePath();
    canvas.fill();
}

/// Fills the sky with the reward shower a redeemed code sets off: twenty
/// stars, dropped in above the top edge, spinning as they fall.
void spawnFallingStars(ShopState& state, double viewWidth) {
    if (state.stars.size() >= kFallingStars) return;
    const std::size_t room = kFallingStars - state.stars.size();
    for (std::size_t i = 0; i < room; ++i) {
        FallingStar star;
        star.at = {state.rng.unit() * viewWidth, -20.0 - state.rng.unit() * 50.0};
        star.vy = state.rng.range(2.0, 5.0);
        star.rotation = state.rng.angle();
        star.spin = (state.rng.unit() - 0.5) * 0.1;
        star.size = state.rng.range(8.0, 20.0);
        star.alpha = state.rng.range(0.8, 1.0);
        // Two independent rolls in the reference, so a star can begin part-way
        // through its own fade.
        star.life = state.rng.range(2.0, 3.0);
        star.maxLife = state.rng.range(2.0, 3.0);
        state.stars.push_back(star);
    }
}

/// Steps and draws the shower, dropping stars that ran out of screen or life.
void drawFallingStars(Canvas& canvas, ShopState& state, double dt, double viewHeight) {
    // The reference integrates per FRAME at 60fps -- `y += vy`, `lifetime -=
    // 16` -- so its speeds are converted here rather than reused raw, or the
    // shower runs at whatever rate this client happens to hit.
    const double frames = dt * 60.0;
    std::vector<FallingStar> alive;
    alive.reserve(state.stars.size());
    for (FallingStar star : state.stars) {
        star.at.y += star.vy * frames;
        star.rotation += star.spin * frames;
        star.life -= dt;
        const double progress = star.maxLife > 0 ? star.life / star.maxLife : 0.0;
        if (star.at.y > viewHeight + 50.0 || progress <= 0.0) continue;

        canvas.save();
        canvas.setGlobalAlpha(static_cast<float>(star.alpha * progress));
        canvas.translate(static_cast<float>(star.at.x), static_cast<float>(star.at.y));
        canvas.rotate(static_cast<float>(star.rotation));
        setFill(canvas, kGold);
        setStroke(canvas, kPaper);
        canvas.setLineWidth(1.0f);
        canvas.beginPath();
        const double outer = star.size * 0.5;
        for (int i = 0; i < 10; ++i) {
            const double angle = i * kPi / 5.0 - kPi * 0.5;
            const double radius = (i % 2 == 0) ? outer : outer * 0.4;
            const auto px = static_cast<float>(std::cos(angle) * radius);
            const auto py = static_cast<float>(std::sin(angle) * radius);
            if (i == 0) canvas.moveTo(px, py);
            else canvas.lineTo(px, py);
        }
        canvas.closePath();
        canvas.fill();
        canvas.stroke();
        canvas.restore();
        alive.push_back(star);
    }
    state.stars = std::move(alive);
}

/// The star icon in a `size` square whose top-left corner is (x, y), matching
/// the reference's `drawImage(icon, x, y, size, size)`. Gold by default; the
/// price and balance pills ask for the white one.
void drawStarIcon(Canvas& canvas, double x, double y, double size, bool gold = true) {
    if (const SvgDocument* doc = starDocument(gold)) {
        doc->renderFitted(canvas, static_cast<float>(x), static_cast<float>(y),
                          static_cast<float>(size), 0.0f);
        return;
    }
    drawStar(canvas, {x + size * 0.5, y + size * 0.5}, size * 0.5, gold ? kGold : kPaper);
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

/// The browser's `formatPrice`. Deliberately not `abbreviate()`: the shared
/// helper keeps a decimal from 10k up, and the shop drops it -- 18382 reads
/// "18k" on a 44px card, not "18.4k".
std::string formatPrice(double price) {
    char buffer[32];
    double value = price;
    int decimals = 1;
    const char* suffix = "";
    if (price >= 1e9) { value = price / 1e9; suffix = "B"; }
    else if (price >= 1e6) { value = price / 1e6; suffix = "M"; }
    else if (price >= 1e4) { value = price / 1e3; decimals = 0; suffix = "k"; }
    else if (price >= 1e3) { value = price / 1e3; suffix = "k"; }
    else {
        std::snprintf(buffer, sizeof buffer, "%.0f", price);
        return buffer;
    }
    // Rounded before it is printed: `toFixed` breaks a tie AWAY from zero and
    // `%.*f` breaks it to even, so a 2,250-star card reads "2.3k" in the
    // reference and would read "2.2k" here.
    const double scale = std::pow(10.0, decimals);
    value = std::floor(value * scale + 0.5) / scale;
    std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    std::string out = buffer;
    // "1.0k" reads worse than "1k", and the trailing zero is never news.
    if (out.size() > 2 && out.compare(out.size() - 2, 2, ".0") == 0) out.erase(out.size() - 2);
    return out + suffix;
}

/// Greedy word wrap for the modal message. A single word wider than the limit
/// keeps its own line and overflows, rather than being broken mid-word.
std::vector<std::string> wrapMessage(const std::string& body, double size, double maxWidth) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t at = 0;
    while (at <= body.size()) {
        const std::size_t space = body.find(' ', at);
        const std::string word =
            body.substr(at, space == std::string::npos ? std::string::npos : space - at);
        if (!word.empty()) {
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && measure(candidate, size, false) > maxWidth) {
                lines.push_back(line);
                line = word;
            } else {
                line = candidate;
            }
        }
        if (space == std::string::npos) break;
        at = space + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

std::string trimmed(const std::string& s) {
    std::size_t first = 0;
    while (first < s.size() && static_cast<unsigned char>(s[first]) <= ' ') ++first;
    std::size_t last = s.size();
    while (last > first && static_cast<unsigned char>(s[last - 1]) <= ' ') --last;
    return s.substr(first, last - first);
}

/// The line under "Today's offers:".
///
/// Rounded UP, so the top of an hour reads "in 1 hour" rather than "in 59
/// minutes" -- the caption is a promise about when the cards change, and a
/// countdown that starts a minute short of the wait reads as a stale panel.
std::string rotationCaption(std::int64_t secondsLeft) {
    const std::int64_t minutes = (secondsLeft + 59) / 60;
    if (minutes >= 60) return "Store will change in 1 hour";
    if (minutes > 1) return "Store will change in " + std::to_string(minutes) + " minutes";
    if (minutes == 1) return "Store will change in 1 minute";
    return "Store will change in " + std::to_string(secondsLeft) + " seconds";
}

/// The discount tag, centred on `corner` and tipped clockwise: a black rounded
/// rect with the saving on it, stuck over the tile's top-right corner.
void drawDiscountRibbon(Canvas& canvas, Vec2 corner, int percent) {
    canvas.save();
    canvas.translate(static_cast<float>(corner.x), static_cast<float>(corner.y));
    canvas.rotate(static_cast<float>(kRibbonRadians));
    fillRounded(canvas, {-kRibbonWidth * 0.5, -kRibbonHeight * 0.5, kRibbonWidth, kRibbonHeight},
                4.0, kInk);
    TextStyle label = shopText(16.0, true, kPaper, 0.0);
    label.align = Align::Centre;
    text(canvas, "-" + std::to_string(percent) + "%", 0.0, 0.0, label);
    canvas.restore();
}

} // namespace

double ShopPanel::preferredWidth() { return 753.0; }
double ShopPanel::preferredHeight() { return 549.0; }

void ShopPanel::reset() {
    // The browser keeps its scroll offsets and whatever was typed into the
    // code field for the manager's whole lifetime; closing the panel drops
    // only the focus and any open modal.
    ShopState& state = stateFor(this);
    state.field.blur();
    state.modal = ShopState::Modal::None;
    state.message.clear();
    state.pendingPetal = kNoPetal;
    // The shower belongs to the moment a code was redeemed. It rains on the
    // game canvas in the reference and so outlives the card there; this one
    // can only be drawn while the card is up, and a shower that resumed
    // mid-air an hour later would be a ghost of a reward already given.
    state.stars.clear();
}

bool ShopPanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Profile& profile = ctx.net.profile();
    const Rect panel = ctx.bounds;
    const Vec2 mouse = ctx.mouse();
    ShopState& state = stateFor(this);

    // The server's answer to a purchase or a code. Read before the modal is
    // latched, so a refusal raises its alert on the frame it arrived rather
    // than the one after.
    ShopOutcome& answer = ctx.net.shopOutcome();
    if (answer.pending) {
        answer.pending = false;
        if (!answer.ok) {
            state.modal = ShopState::Modal::Alert;
            state.message =
                (answer.redeem ? "Code redemption failed: " : "Purchase failed: ") + answer.message;
            state.messageColor = kAlert;
        } else if (answer.redeem) {
            // The field was already cleared when the code was sent, as the
            // reference clears it; the stars are the whole confirmation.
            spawnFallingStars(state, ctx.window.width());
        }
    }

    // Behind everything, including the card: the reference drops these on the
    // game canvas and the shop's own canvas sits over it.
    if (!state.stars.empty()) {
        drawFallingStars(canvas, state, ctx.dt, ctx.window.height());
    }

    // Input answers the frame that was drawn, so the modal's presence is
    // latched here and used by both passes.
    const bool modalUp = state.modal != ShopState::Modal::None;

    // What the pointer is over. Assigned as the regions are laid out and
    // pushed to the window once, at the end of the draw pass, exactly as the
    // reference assigns `canvas.style.cursor` once per move.
    CursorShape cursor = CursorShape::Arrow;

    // A focused field or an open modal owns the keyboard. Without this the
    // menu system reads every letter typed into the code field as a hotkey and
    // Escape as "close the shop".
    if (modalUp || state.field.focused) ctx.wantsText = true;

    // Read the balance from the live entity when there is one, so a mythic
    // kill's payout shows up before the next profile arrives.
    const int stars = ctx.net.status() == NetClient::Status::Playing
                          ? ctx.net.view().self().stars
                          : profile.stars;

    // --- card ---------------------------------------------------------------
    // No shadow: the reference card sits on its page with nothing between it
    // and the game behind it. The frame is the shared one.
    overlayCard(canvas, panel, kShopSkin);

    // --- header -------------------------------------------------------------
    const Rect header{panel.x + kPlateInset, panel.y + kPlateInset, panel.w - kPlateInset * 2,
                      kHeaderHeight};
    fillRounded(canvas, header, kPlateRadius, kShopPlate);

    TextStyle title = shopText(24.0, true, kPaper, -1.0);
    title.align = Align::Centre;
    text(canvas, "Shop", panel.x + panel.w * 0.5, header.y + header.h * 0.5, title);

    // The one panel whose close does NOT hang off the card's corner: the shop's
    // header is a plate inset from it, and a button outside that plate would
    // read as belonging to the card behind it rather than to the header.
    const Rect closeRect{header.right() - 8.0 - kCloseSize,
                         header.y + (header.h - kCloseSize) * 0.5, kCloseSize, kCloseSize};
    const bool closeHover = !modalUp && closeRect.contains(mouse);
    if (closeHover) cursor = CursorShape::Hand;
    panelClose(canvas, closeRect, closeHover);

    // --- tabs ---------------------------------------------------------------
    // Three of them, centred on the card's own body between the two plates.
    // The active one is pressed INTO the body rather than lit: the frame is
    // what says "tab", so lighting the face would make it read as a button.
    static constexpr std::array<const char*, 3> kTabLabels{"Shop", "Challenge", "Bonus"};
    const double tabsWidth = 3.0 * kTabWidth + 2.0 * kTabGap;
    std::array<Rect, 3> tabRects{};
    for (int i = 0; i < 3; ++i) {
        const Rect rect{panel.x + (panel.w - tabsWidth) * 0.5 + i * (kTabWidth + kTabGap),
                        panel.y + kTabTop, kTabWidth, kTabHeight};
        tabRects[static_cast<std::size_t>(i)] = rect;
        const bool active = static_cast<int>(tab_) == i;
        const bool hovered = !modalUp && rect.contains(mouse);
        if (hovered) cursor = CursorShape::Hand;

        inlaid(canvas, rect, kShopSkin.fill, kTabBorder, kControlBorder, kTabRadius);
        if (active || hovered) {
            tintFace(canvas, rect, kTabRadius, active ? kTintSelected : kTintHover);
        }

        TextStyle label = shopText(16.0, true, kPaper, -1.0);
        label.align = Align::Centre;
        // Every label is full white, the active one included. Which tab is
        // selected is said by the FACE -- kTintSelected presses it into the
        // body -- and dimming the other two said it a second time, in a way
        // that read as "disabled" rather than as "not the current tab".
        text(canvas, kTabLabels[static_cast<std::size_t>(i)], rect.x + rect.w * 0.5,
             rect.y + rect.h * 0.5, label);
    }

    // --- content plate ------------------------------------------------------
    const Rect contentPlate{panel.x + kPlateInset, panel.y + kContentTop,
                            panel.w - kPlateInset * 2, panel.h - kContentTop - kPlateInset};
    fillRounded(canvas, contentPlate, kPlateRadius, kShopPlate);

    // --- the footer, on every tab -------------------------------------------
    // The balance is the one number all three tabs are about, so it is the
    // card's footer rather than the store's.
    const Rect balancePill{contentPlate.right() - 9.0 - kBalanceWidth,
                           contentPlate.bottom() - 10.0 - kBalanceHeight, kBalanceWidth,
                           kBalanceHeight};
    fillRounded(canvas, balancePill, kBlockRadius, kShopSkin.fill);
    {
        const std::string balance = withSeparators(stars);
        const double group = kBalanceStar + 6.0 + measure(balance, 18.0, true);
        const double groupX = balancePill.x + (balancePill.w - group) * 0.5;
        drawStarIcon(canvas, groupX, balancePill.y + (balancePill.h - kBalanceStar) * 0.5,
                     kBalanceStar, false);
        text(canvas, balance, groupX + kBalanceStar + 6.0, balancePill.y + balancePill.h * 0.5,
             shopText(18.0, true, kPaper, -1.0));
    }

    TextStyle hint = shopText(13.0, true, kPaper, -1.0);
    hint.align = Align::Right;
    const double hintRight = balancePill.x - 9.0;
    text(canvas, "You gain stars by completing challenges, or by", hintRight,
         balancePill.y + 7.0, hint);
    text(canvas, "redeeming a code on the Bonus tab.", hintRight, balancePill.y + 31.0, hint);

    // What a tab has to itself: the plate above the footer.
    const Rect body{contentPlate.x, contentPlate.y, contentPlate.w,
                    balancePill.y - 10.0 - contentPlate.y};

    // --- tab contents -------------------------------------------------------
    // Laid out by the draw pass and answered for at the bottom of the frame.
    // A rect left empty is a region this tab does not have, and an empty rect
    // contains no point, so the hit tests need no second look at `tab_`.
    std::array<Rect, kShopOfferCount> offerBlocks{};
    int hoveredOffer = -1;
    Rect lockRect{};
    Rect codeField{};
    Rect redeemButton{};

    const std::size_t tabIndex = static_cast<std::size_t>(tab_);
    double contentHeight = 0;

    if (tab_ == Tab::Offers) {
        const std::int64_t rotation = shopRotation(shopClockNow());
        if (state.offersRotation != rotation) {
            state.offersRotation = rotation;
            state.offers = shopOffers(rotation);
        }

        TextStyle heading = shopText(kOffersHeadingSize, true, kPaper, -1.0);
        heading.align = Align::Centre;
        text(canvas, "Today's offers:", panel.x + panel.w * 0.5, contentPlate.y + 24.0, heading);

        const std::int64_t remaining =
            std::max<std::int64_t>(0, shopRotationEnd(rotation) - shopClockNow());
        TextStyle sub = shopText(kOffersSubSize, true, kPaper, -1.0);
        sub.align = Align::Centre;
        text(canvas, rotationCaption(remaining), panel.x + panel.w * 0.5, contentPlate.y + 46.0,
             sub);

        // Locking an offer through a rotation is the reference's; nothing on
        // this server keeps per-account offers, so the control is drawn where
        // it belongs and left inert rather than made to lie about what it did.
        // Drawn at full strength, as the reference draws it: the pill is
        // kLockFill and the label is white. The 0.55 wash this used to wear
        // was our own way of saying "inert", and it made the one green control
        // on the panel the only washed-out thing on it.
        lockRect = Rect{contentPlate.right() - 27.0 - 67.0, contentPlate.y + 17.0, 67.0, 29.0};
        inlaid(canvas, lockRect, kLockFill, kLockBorder, kControlBorder, 6.0);
        TextStyle lockLabel = shopText(14.0, true, kPaper, -1.0);
        lockLabel.align = Align::Centre;
        text(canvas, "Lock", lockRect.x + lockRect.w * 0.5, lockRect.y + lockRect.h * 0.5,
             lockLabel);

        const double gridWidth =
            kShopOfferColumns * kBlockWidth + (kShopOfferColumns - 1) * kBlockGap;
        const double gridX = panel.x + (panel.w - gridWidth) * 0.5;

        for (std::size_t i = 0; i < state.offers.size() && i < kShopOfferCount; ++i) {
            const ShopOffer& offer = state.offers[i];
            const int column = static_cast<int>(i) % kShopOfferColumns;
            const int row = static_cast<int>(i) / kShopOfferColumns;
            const Rect block{gridX + column * (kBlockWidth + kBlockGap),
                             panel.y + kGridTop + row * (kBlockHeight + kBlockGap), kBlockWidth,
                             kBlockHeight};
            offerBlocks[i] = block;

            // The pill is the button. The block and the tile above it are the
            // card's picture, not a control, so nothing about them answers to
            // the pointer -- and every pill is live whatever the balance: the
            // reference neither greys a card out nor reddens its price, and a
            // player who cannot afford one is told so by the server's refusal
            // rather than by a card that quietly does nothing.
            const Rect pill{block.x + (kBlockWidth - kPricePillWidth) * 0.5,
                            block.y + kPricePillTop, kPricePillWidth, kPricePillHeight};
            const bool hot = !modalUp && pill.contains(mouse);
            if (hot) {
                hoveredOffer = static_cast<int>(i);
                cursor = CursorShape::Hand;
            }

            fillRounded(canvas, block, kBlockRadius, kShopSkin.fill);

            const Rect tileRect{block.x + (kBlockWidth - kTileWidth) * 0.5, block.y + kTileTop,
                                kTileWidth, kTileHeight};
            ItemTile tile;
            tile.petalIndex = offer.petalIndex;
            tile.rarity = offer.rarity;
            tile.timeSeconds = ctx.timeSeconds;
            drawItemTile(canvas, ctx.sprites, tileRect, tile);

            inlaid(canvas, pill, kPriceFill, kPriceBorder, kControlBorder, kBlockRadius);
            if (hot) tintFace(canvas, pill, kBlockRadius, kTintSelected);

            const std::string price = formatPrice(offer.price);
            const double group = kPriceStar + 4.0 + measure(price, 16.0, true);
            const double groupX = pill.x + (pill.w - group) * 0.5;
            drawStarIcon(canvas, groupX, pill.y + (pill.h - kPriceStar) * 0.5, kPriceStar, false);
            text(canvas, price, groupX + kPriceStar + 4.0, pill.y + pill.h * 0.5,
                 shopText(16.0, true, kPaper, -1.0));
        }

        // The ribbons last, in their own pass: one hangs off the corner of its
        // card and would otherwise be painted over by the next card's block.
        for (std::size_t i = 0; i < state.offers.size() && i < kShopOfferCount; ++i) {
            if (state.offers[i].discountPercent <= 0) continue;
            const Rect block = offerBlocks[i];
            drawDiscountRibbon(canvas,
                               {block.x + (kBlockWidth + kTileWidth) * 0.5, block.y + kTileTop},
                               state.offers[i].discountPercent);
        }
    } else if (tab_ == Tab::Bonus) {
        const Rect codePlate{body.x + 30.0, body.y + 26.0, body.w - 60.0, 112.0};
        fillRounded(canvas, codePlate, 10.0, kCodeBlue, 0.15);
        strokeRounded(canvas, codePlate, 10.0, kCodeBlue, 2.0);

        TextStyle codeTitle = shopText(18.0, true, kPaper, -1.0);
        codeTitle.baseline = Baseline::Top;
        text(canvas, "Redeem Code", codePlate.x + 15.0, codePlate.y + 12.0, codeTitle);

        codeField = Rect{codePlate.x + 15.0, codePlate.y + 45.0,
                         codePlate.w - 30.0 - 10.0 - 110.0, 40.0};
        redeemButton = Rect{codeField.right() + 10.0, codeField.y, 110.0, 40.0};

        const bool fieldHover = !modalUp && codeField.contains(mouse);
        if (fieldHover) cursor = CursorShape::Text;
        fillRounded(canvas, codeField, 5.0, kPaper, 0.10);
        strokeRounded(canvas, codeField, 5.0,
                      state.field.focused ? kPaper : (fieldHover ? kCodeBlueLit : kCodeBlue), 2.0);

        canvas.save();
        canvas.beginPath();
        canvas.rect(static_cast<float>(codeField.x + 4.0), static_cast<float>(codeField.y),
                    static_cast<float>(codeField.w - 8.0), static_cast<float>(codeField.h));
        canvas.clip();
        {
            const TextRun run = codeRun(codeField, state.code, state.field);
            const bool placeholder = state.code.empty();
            const std::string shown =
                placeholder ? (state.field.focused ? std::string() : "Enter code...") : state.code;

            // A pale wash rather than the blue one: this field is a translucent
            // white inset on a green card, and a blue block on it reads as a
            // different control rather than as selected text.
            if (state.field.focused) {
                selectionHighlight(canvas, run, state.field.selection,
                                   Rect{codeField.x + 4.0, codeField.y + 6.0, codeField.w - 8.0,
                                        codeField.h - 12.0},
                                   kPaper, 0.35);
            }

            if (placeholder) canvas.setGlobalAlpha(0.45f);
            text(canvas, shown, run.originX, codeField.y + codeField.h * 0.5,
                 shopText(16.0, false, kPaper, 0.0));
            if (placeholder) canvas.setGlobalAlpha(1.0f);

            if (state.field.focused && caretVisible(state.field, ctx.timeSeconds)) {
                const double caretX = xOfIndex(run, state.field.selection.caret);
                setStroke(canvas, kPaper);
                canvas.setLineWidth(1.5f);
                canvas.beginPath();
                canvas.moveTo(static_cast<float>(caretX), static_cast<float>(codeField.y + 8.0));
                canvas.lineTo(static_cast<float>(caretX),
                              static_cast<float>(codeField.bottom() - 8.0));
                canvas.stroke();
            }
        }
        canvas.restore();

        const bool redeemHover = !modalUp && redeemButton.contains(mouse);
        if (redeemHover) cursor = CursorShape::Hand;
        fillRounded(canvas, redeemButton, 5.0, redeemHover ? kCodeBlueHover : kCodeBlue);
        TextStyle redeemLabel = shopText(16.0, true, kPaper, -1.0);
        redeemLabel.align = Align::Centre;
        text(canvas, "Redeem", redeemButton.x + redeemButton.w * 0.5,
             redeemButton.y + redeemButton.h * 0.5, redeemLabel);

        TextStyle note = shopText(14.0, false, kPaper, -1.0);
        note.align = Align::Centre;
        note.baseline = Baseline::Top;
        text(canvas, "Codes are handed out on the Discord, and each pays its stars once.",
             body.x + body.w * 0.5, codePlate.bottom() + 22.0, note);
    } else {
        contentHeight = 32.0 + kChallenges.size() * 104.0 + 10.0;
        scroll_.contentHeight = contentHeight;
        scroll_.viewHeight = body.h;
        scroll_.offset = state.scroll[tabIndex];
        // The wheel works anywhere over the card, not just over the list, and
        // is dead while a modal is up.
        if (!modalUp && panel.contains(mouse)) scroll_.offset -= ctx.wheel() * kWheelPixels;
        scroll_.offset = clamp(scroll_.offset, 0.0, scroll_.maxOffset());
        state.scroll[tabIndex] = scroll_.offset;

        canvas.save();
        canvas.beginPath();
        canvas.rect(static_cast<float>(body.x), static_cast<float>(body.y),
                    static_cast<float>(body.w), static_cast<float>(body.h));
        canvas.clip();

        double y = body.y + 10.0 - scroll_.offset;
        TextStyle heading = shopText(20.0, true, kPaper, -1.0);
        heading.align = Align::Centre;
        heading.baseline = Baseline::Top;
        text(canvas, "Earn Stars by Defeating Mythic+ Mobs", body.x + body.w * 0.5, y, heading);
        y += 32.0;

        for (const StarChallenge& challenge : kChallenges) {
            const Rect card{body.x + 20.0, y, body.w - 40.0, 92.0};
            y += card.h + 12.0;
            if (card.bottom() <= body.y || card.y >= body.bottom()) continue;

            fillRounded(canvas, card, 10.0, challenge.color);
            strokeRounded(canvas, card, 10.0, kInk, 2.0, 0.3);

            TextStyle name = shopText(18.0, true, kPaper, -1.0);
            name.baseline = Baseline::Top;
            text(canvas, std::string(rarityLabel(challenge.tier)) + " Challenge", card.x + 12.0,
                 card.y + 12.0, name);

            TextStyle detail = shopText(14.0, false, kPaper, -1.0);
            detail.baseline = Baseline::Top;
            fadedText(canvas, std::string("Defeat any ") + rarityLabel(challenge.tier) + " tier mob",
                      card.x + 12.0, card.y + 36.0, detail, 0.95);

            drawStarIcon(canvas, card.x + 12.0, card.y + 60.0, 18.0);
            const std::string reward =
                std::to_string(challenge.stars) + (challenge.stars == 1 ? " Star" : " Stars");
            text(canvas, reward, card.x + 38.0, card.y + 69.0, shopText(16.0, true, kGold, -1.0));
        }
        canvas.restore();

        // --- scrollbar ------------------------------------------------------
        if (contentHeight > body.h && body.h > 0) {
            const Rect track{body.right() - kScrollbarWidth - 6.0, body.y, kScrollbarWidth, body.h};
            const double thumbHeight = std::max(20.0, (body.h / contentHeight) * body.h);
            const double thumbY =
                body.y + (scroll_.offset / (contentHeight - body.h)) * (body.h - thumbHeight);
            fillRounded(canvas, track, 3.0, kInk, 0.15);
            fillRounded(canvas, {track.x, thumbY, track.w, thumbHeight}, 3.0, kPaper, 0.55);
        }
    }

    // --- modal --------------------------------------------------------------
    // The card dims and one green box stands on it: a title, the question, the
    // petal itself, and the two things that can be done about it. The buy
    // button IS the price -- the same gold pill the offer card wears -- so
    // what is being agreed to is spelled out on the control that agrees.
    Rect confirmRect{};
    Rect cancelRect{};
    if (modalUp) {
        fillRounded(canvas, panel, kCardRadius, kInk, kModalScrim);

        const bool confirming = state.modal == ShopState::Modal::Confirm;
        const std::vector<std::string> lines =
            confirming ? std::vector<std::string>{}
                       : wrapMessage(state.message, kModalBodySize, kModalWidth - 40.0);
        // A confirm is the measured box; an alert is as tall as its message
        // needs, which is the only thing that ever varies here.
        const double boxH =
            confirming ? kModalHeight
                       : kModalTextTop + static_cast<double>(lines.size()) * kModalLine + 24.0 +
                             kModalButtonHeight + kModalBottomPad;
        const Rect box{panel.x + (panel.w - kModalWidth) * 0.5,
                       panel.y + (panel.h - boxH) * 0.5, kModalWidth, boxH};
        inlaid(canvas, box, kShopPlate, kShopSkin.fill, kModalBorder, kModalRadius);

        const double centreX = box.x + box.w * 0.5;
        if (confirming) {
            TextStyle heading = shopText(24.0, true, kPaper, -1.0);
            heading.align = Align::Centre;
            text(canvas, "Confirm", centreX, box.y + 39.0, heading);

            TextStyle question = shopText(kModalBodySize, true, kPaper, -1.0);
            question.align = Align::Centre;
            text(canvas, "Are you sure you want to buy this?", centreX, box.y + 87.0, question);
            TextStyle warning = question;
            warning.fill = kAlert;
            text(canvas, "Petal purchases are not refundable", centreX, box.y + 107.0, warning);

            ItemTile tile;
            tile.petalIndex = state.pendingPetal;
            tile.rarity = state.pendingRarity;
            tile.timeSeconds = ctx.timeSeconds;
            drawItemTile(canvas, ctx.sprites,
                         {centreX - kTileWidth * 0.5, box.y + 135.0, kTileWidth, kTileWidth},
                         tile);
        } else {
            TextStyle message = shopText(kModalBodySize, true, state.messageColor, -1.0);
            message.align = Align::Centre;
            message.baseline = Baseline::Top;
            double lineY = box.y + kModalTextTop;
            for (const std::string& piece : lines) {
                text(canvas, piece, centreX, lineY, message);
                lineY += kModalLine;
            }
        }

        // Buy sits left of Cancel, as the reference has it: the affirmative is
        // the one that carries a price, and reading order puts the price first.
        const double buttonsWidth =
            confirming ? kModalBuyWidth + kModalButtonGap + kModalCancelWidth : kModalCancelWidth;
        double bx = centreX - buttonsWidth * 0.5;
        const double by = box.bottom() - kModalBottomPad - kModalButtonHeight;
        if (confirming) {
            confirmRect = {bx, by, kModalBuyWidth, kModalButtonHeight};
            bx += kModalBuyWidth + kModalButtonGap;

            const bool hovered = confirmRect.contains(mouse);
            if (hovered) cursor = CursorShape::Hand;
            inlaid(canvas, confirmRect, kPriceFill, kPriceBorder, kControlBorder, kBlockRadius);
            if (hovered) tintFace(canvas, confirmRect, kBlockRadius, kTintSelected);

            const std::string price = formatPrice(state.pendingPrice);
            const double group = kModalStar + 4.0 + measure(price, 16.0, true);
            const double groupX = confirmRect.x + (confirmRect.w - group) * 0.5;
            drawStarIcon(canvas, groupX, confirmRect.y + (confirmRect.h - kModalStar) * 0.5,
                         kModalStar, false);
            text(canvas, price, groupX + kModalStar + 4.0,
                 confirmRect.y + confirmRect.h * 0.5, shopText(16.0, true, kPaper, -1.0));
        }

        cancelRect = {bx, by, kModalCancelWidth, kModalButtonHeight};
        const bool cancelHover = cancelRect.contains(mouse);
        if (cancelHover) cursor = CursorShape::Hand;
        inlaid(canvas, cancelRect, kModalGrey, kModalGreyBorder, kControlBorder, kBlockRadius);
        if (cancelHover) tintFace(canvas, cancelRect, kBlockRadius, kTintSelected);
        TextStyle label = shopText(16.0, true, kPaper, -1.0);
        label.align = Align::Centre;
        text(canvas, confirming ? "Cancel" : "OK", cancelRect.x + cancelRect.w * 0.5,
             cancelRect.y + cancelRect.h * 0.5, label);
    }

    // The whole card has now been laid out, so this is the last word on what
    // the pointer is over. A modal's scrim swallows every region under it, and
    // `cursor` was only ever set by a region the scrim does not cover.
    ctx.window.setCursorShape(cursor);

    // --- actions ------------------------------------------------------------
    const auto clearModal = [&state] {
        state.modal = ShopState::Modal::None;
        state.message.clear();
        state.pendingPetal = kNoPetal;
    };
    const auto resolveModal = [&](bool confirmed) {
        if (confirmed && state.modal == ShopState::Modal::Confirm &&
            state.pendingPetal != kNoPetal) {
            ctx.net.requestBuyPetal(state.pendingPetal, state.pendingRarity, state.pendingSlot);
        }
        clearModal();
    };
    const auto submitCode = [&] {
        const std::string code = trimmed(state.code);
        if (code.empty()) {
            state.modal = ShopState::Modal::Alert;
            state.message = "Please enter a code";
            state.messageColor = kAlert;
            return;
        }
        ctx.net.requestRedeemCode(code);
        // Cleared on SEND, not on the answer -- the reference empties the field
        // the moment it emits, and the reply only decides between the shower
        // and the red alert.
        state.code.clear();
        state.field.selection.collapse(0);
        state.field.caretSeconds = ctx.timeSeconds;
    };

    // --- keyboard -----------------------------------------------------------
    if (modalUp) {
        if (ctx.window.keyPressed(Key::Enter)) {
            resolveModal(state.modal == ShopState::Modal::Confirm);
        } else if (ctx.window.keyPressed(Key::Escape)) {
            clearModal();
        }
    } else if (state.field.focused) {
        // Printable ASCII only: codes are, and it keeps the caret's byte index
        // and its character index the same thing.
        TextEditOptions typing;
        typing.maxBytes = kCodeMaxLength;
        typing.asciiOnly = true;
        editText(ctx.window, state.code, state.field, ctx.timeSeconds, typing);
        if (ctx.window.keyPressed(Key::Enter)) submitCode();
        // Escape leaves the field rather than the panel; a second press then
        // closes the shop, which is the browser's behaviour exactly.
        if (ctx.window.keyPressed(Key::Escape)) state.field.blur();
    }
    // The drag half of the pointer, before the press pass below claims it.
    if (!modalUp) {
        trackTextMouse(ctx.window, state.field, codeField,
                       codeRun(codeField, state.code, state.field), state.code,
                       ctx.timeSeconds);
    }

    // --- mouse --------------------------------------------------------------
    // On the press edge, as the reference's mousedown handler is: a tab or a
    // card responds the instant the button goes down.
    if (!ctx.pressed()) return true;

    if (modalUp) {
        if (confirmRect.contains(mouse)) resolveModal(true);
        else if (cancelRect.contains(mouse)) resolveModal(false);
        // The scrim swallows everything else, including the close button.
        return true;
    }

    if (closeRect.contains(mouse)) return false;

    for (int i = 0; i < 3; ++i) {
        if (!tabRects[static_cast<std::size_t>(i)].contains(mouse)) continue;
        // Leaving the code field behind clears its focus, or the shop would go
        // on eating keystrokes from a tab with no field on it.
        if (static_cast<int>(tab_) != i) state.field.blur();
        tab_ = static_cast<Tab>(i);
        return true;
    }

    // The field answered for its own press in trackTextMouse above, caret
    // placement and selection drag included.
    if (codeField.contains(mouse)) return true;

    if (redeemButton.contains(mouse)) {
        submitCode();
        return true;
    }

    if (hoveredOffer >= 0 && static_cast<std::size_t>(hoveredOffer) < state.offers.size()) {
        const ShopOffer& offer = state.offers[static_cast<std::size_t>(hoveredOffer)];
        // Some of these prices are a whole session's stars; a stray click must
        // not spend one, so the purchase goes through a confirmation. The box
        // shows the petal itself rather than naming it -- the tile is the same
        // one that was just clicked, which is a clearer answer to "this?" than
        // a sentence repeating its name and tier.
        state.modal = ShopState::Modal::Confirm;
        state.messageColor = kPaper;
        state.pendingPetal = offer.petalIndex;
        state.pendingRarity = offer.rarity;
        state.pendingSlot = hoveredOffer;
        state.pendingPrice = offer.price;
    }
    return true;
}

} // namespace flix
