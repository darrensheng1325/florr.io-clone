// The Skin Studio: a two-tab authoring surface for data-driven player skins.
//
// Not a picker. The player assembles a skin out of drawing primitives, names
// it and publishes it; the Browse tab lists what everyone else published. The
// built-in renderFlags cosmetics are deliberately unreachable from here -- the
// server's set_skin command is the only way to those, exactly as in the
// reference client, which has no in-client picker for them at all.
//
// This is an OVERLAY PANEL and is built from the same pieces as the other
// eleven: overlayCard for the frame, panelHeading and panelClose for the
// chrome, ui::button for the tabs and the actions, ui::chip for the small
// controls, ui::inputField for the name box and ui::scrollbar for both lists.
// It used to carry a private dialect of all of those -- its own card, its own
// two-tone button with a flat white hover wash, twelve file-local colours, a
// header well no other panel has, and a name prompt floated at the middle of
// the VIEWPORT because the browser build put a real <input> there. None of
// that survives. What is still local is what is genuinely this panel's: the
// shape model, the drag handles, the palette and the text-mode parser.
//
// The Browse tab owns none of what it shows. Publishing, equipping and taking
// a skin down are all requests to the server, which sanitizes, assigns the id
// and broadcasts -- the catalog this panel lists is NetClient's, the same one
// the world renderer resolves a wearer's skin through.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "client/net_client.h"
#include "client/ui/draw.h"
#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "client/ui/text.h"
#include "client/ui/text_input.h"
#include "client/ui/text_select.h"
#include "shared/game/skin_format.h"

namespace flix {

using namespace flix::ui;

namespace {

// --- palette ---------------------------------------------------------------
//
// Three shades of the card's own purple and the shared greys, rather than a
// private swatch table. kSkinsSkin is what the strip's skins button is painted
// in, so the panel a player opens is visibly the button they pressed.

/// The selection colour: the strip button's purple, which is also the skin's
/// border. Bright enough to read against the body it sits on.
constexpr std::uint32_t kAccentPurple = kSkinsSkin.border;
/// Sunken surfaces -- the shape list, the text editor, a browse card.
constexpr std::uint32_t kWell = hsvScale(kSkinsSkin.fill, 0.66);
/// One row on a sunken surface.
constexpr std::uint32_t kRowFill = hsvScale(kSkinsSkin.fill, 1.12);
/// The settings panel's own neutral and danger fills, so a grey button and a
/// red one mean the same thing on every card.
constexpr std::uint32_t kNeutralFill = 0xA3A3A3u;
constexpr std::uint32_t kDangerFill = 0xCC4444u;
/// The preview board. Grass, because that is what a skin will be seen on.
constexpr std::uint32_t kBoard = 0x3B7D4Fu;
/// The "no colour" swatch: a dark cell with a slash through it.
constexpr std::uint32_t kSwatchNone = 0x1A1D20u;
constexpr std::uint32_t kSwatchNoneEdge = 0x777777u;
constexpr std::uint32_t kSwatchSlash = 0xD05A5Au;
/// A curve's two bezier handles, told apart from its anchors by colour.
constexpr std::uint32_t kHandleControl = 0xFFE763u;
/// A parse error under the command help.
constexpr std::uint32_t kErrorText = 0xFFE65Du;

// --- metrics ---------------------------------------------------------------
//
// The settings card's, which is the overlay family's: a 15px margin, a 30px
// heading band and a 32px tab row under it.

constexpr double kPad = 15.0;
constexpr double kHeadingBand = 30.0;
constexpr double kTabHeight = 32.0;
constexpr double kTabWidth = 104.0;
constexpr double kTabGap = 5.0;
constexpr double kFooterHeight = 32.0;
/// The left column, and the board at the top of it. The column is the wider
/// of the two so a shape row still has room for its name beside the three
/// buttons that reorder and remove it.
constexpr double kLeftWidth = 196.0;
constexpr double kPreviewSize = 176.0;
constexpr double kColumnGap = 16.0;
/// One shape row's pitch, and the card behind it.
constexpr double kRowPitch = 28.0;
constexpr double kRowCard = kRowPitch - 4.0;
constexpr double kScrollbarWidth = 10.0;
/// One wheel notch is ~100 CSS px of deltaY in a browser; the window reports
/// notches, so the step is spelled out as every other list panel spells it.
constexpr double kWheelStep = 100.0;

/// The type scale, matching the other overlay panels: 15 for a section
/// heading, 13 for body and buttons, 11 for a caption under a control.
constexpr double kSectionSize = 15.0;
constexpr double kBodySize = 13.0;
constexpr double kCaptionSize = 11.0;

/// The fourteen swatch colours, in the order they are laid out.
const char* const kPalette[] = {
    "#ffe763", "#ff9d00", "#e8731f", "#d01c1d", "#e85cc0", "#c45cff", "#3a86ff",
    "#27dade", "#2bd14f", "#7d5a3a", "#ffffff", "#bfc6cc", "#5a6670", "#111111",
};
constexpr int kPaletteCount = 14;
/// Fifteen cells -- the fourteen colours and "none" -- laid out as ONE row
/// fitted to the column, rather than a fixed cell size that wrapped thirteen
/// onto the first row and two onto a second.
constexpr double kSwatchGap = 4.0;
constexpr double kSwatchMax = 26.0;

double round1(double v) { return std::round(v * 10.0) / 10.0; }

/// `String(round1(v))`: an integral value prints without a decimal point, and
/// a negative zero prints as "0", both of which JS does and printf does not.
std::string numberText(double v) {
    const double r = round1(v);
    char buf[32];
    if (std::fabs(r) < 0.05) return "0";
    if (std::fabs(r - std::round(r)) < 1e-9) {
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(r)));
    } else {
        std::snprintf(buf, sizeof buf, "%.1f", r);
    }
    return buf;
}

bool isHexColor(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (std::size_t i = 1; i < 7; ++i) {
        const char c = s[i];
        const bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!digit) return false;
    }
    return true;
}

std::uint32_t hexColor(const std::string& s, std::uint32_t fallback = kInk) {
    if (!isHexColor(s)) return fallback;
    return static_cast<std::uint32_t>(std::strtoul(s.c_str() + 1, nullptr, 16));
}

std::size_t utf8Length(const std::string& s) {
    std::size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

/// Catalog names and authors are cut to a character count, not a width.
std::string clipChars(const std::string& s, std::size_t n) {
    if (utf8Length(s) <= n) return s;
    std::size_t at = 0;
    for (std::size_t i = 0; i + 1 < n && at < s.size(); ++i) at = utf8Next(s, at);
    return s.substr(0, at) + "\xe2\x80\xa6";
}

void clipRect(Canvas& canvas, Rect r) {
    canvas.beginPath();
    canvas.rect(static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                static_cast<float>(r.h));
    canvas.clip();
}

void clipRound(Canvas& canvas, Rect r, double radius) {
    roundPath(canvas, r, radius);
    canvas.clip();
}

/// Panel text: white, outlined so it reads over the card and over the board,
/// and vertically centred on the y it is given -- which is what lets a row's
/// contents be laid out against the row's own middle.
TextStyle label(double size, bool bold = false, Align align = Align::Left,
                std::uint32_t fill = kPaper) {
    TextStyle style;
    style.size = size;
    style.bold = bold;
    style.align = align;
    style.baseline = Baseline::Middle;
    style.fill = fill;
    style.stroke = kInk;
    style.strokeWidth = size >= kSectionSize ? 3.0 : 2.0;
    style.roundJoin = true;
    return style;
}

/// The overlay family's button, at whatever size the caller's box is.
ButtonStyle buttonStyle(std::uint32_t fill, double textSize = kBodySize) {
    ButtonStyle style;
    style.fill = fill;
    style.outlineWidth = 3.0;
    style.radius = 3.0;
    style.textSize = textSize;
    style.textStrokeWidth = 3.0;
    style.shrinkToFit = true;
    return style;
}

bool insideInclusive(Rect r, Vec2 p) {
    return p.x >= r.x && p.x <= r.x + r.w && p.y >= r.y && p.y <= r.y + r.h;
}

// --- the skin data model ---------------------------------------------------
//
// Deliberately the SHARED model, not a studio-local one: the same structs and
// the same validator cross the wire and are stored by the server, so a skin
// this panel accepts is exactly a skin the catalog will accept back.

using ShapeType = SkinShapeType;
using Shape = SkinShape;

const char* shortType(ShapeType t) {
    switch (t) {
        case ShapeType::Circle: return "Circle";
        case ShapeType::Ellipse: return "Ellipse";
        case ShapeType::Rect: return "Rect";
        case ShapeType::Polygon: return "Polygon";
        case ShapeType::Curve: return "Curve";
        default: return "Line";
    }
}

Shape defaultShape(ShapeType t) {
    Shape s;
    s.t = t;
    switch (t) {
        case ShapeType::Circle:
            s.r = 10; s.fill = "#3a86ff"; break;
        case ShapeType::Ellipse:
            s.rx = 10; s.ry = 6; s.fill = "#3a86ff"; break;
        case ShapeType::Rect:
            s.rx = 8; s.ry = 8; s.fill = "#3a86ff"; break;
        case ShapeType::Line:
            s.x = -10; s.x2 = 10; s.stroke = "#111111"; s.sw = 2; break;
        case ShapeType::Curve:
            s.x = -12; s.y = 4; s.x2 = 12; s.y2 = 4;
            s.cx1 = -8; s.cy1 = -14; s.cx2 = 8; s.cy2 = -14;
            s.stroke = "#111111"; s.sw = 2; break;
        case ShapeType::Polygon:
            s.points = {0, -12, 11, 8, -11, 8}; s.fill = "#3a86ff"; break;
    }
    return s;
}

std::vector<Shape> starterShapes() {
    std::vector<Shape> out;
    Shape body;
    body.t = ShapeType::Circle;
    body.r = 25;
    body.fill = "#ffe763";
    body.stroke = "#cdb74f";
    body.sw = 3;
    out.push_back(body);
    for (const double ex : {-7.0, 7.0}) {
        Shape eye;
        eye.t = ShapeType::Ellipse;
        eye.x = ex;
        eye.y = -5;
        eye.rx = 3.2;
        eye.ry = 6.5;
        eye.fill = "#111111";
        out.push_back(eye);
    }
    return out;
}

/// Draws a skin's shapes about the current origin. The editor authors against
/// a body radius of 25, so `radius` is what scales an authored skin onto
/// whatever circle it is being shown in.
void renderSkinShapes(Canvas& canvas, const std::vector<Shape>& shapes, double radius) {
    const double scale = radius / 25.0;
    const float tau = static_cast<float>(kPi * 2.0);
    canvas.save();
    canvas.scale(static_cast<float>(scale), static_cast<float>(scale));
    canvas.beginPath();
    canvas.arc(0.0f, 0.0f, 100.0f, 0.0f, tau);
    canvas.clip();
    for (const Shape& s : shapes) {
        canvas.save();
        canvas.translate(static_cast<float>(s.x), static_cast<float>(s.y));
        if (s.rot != 0) canvas.rotate(static_cast<float>(s.rot * kPi / 180.0));
        canvas.beginPath();
        switch (s.t) {
            case ShapeType::Circle:
                canvas.arc(0.0f, 0.0f, static_cast<float>(s.r != 0 ? s.r : 1.0), 0.0f, tau);
                break;
            case ShapeType::Ellipse:
                canvas.ellipse(0.0f, 0.0f, static_cast<float>(s.rx != 0 ? s.rx : 1.0),
                               static_cast<float>(s.ry != 0 ? s.ry : 1.0), 0.0f, 0.0f, tau);
                break;
            case ShapeType::Rect: {
                const double rx = s.rx != 0 ? s.rx : 1.0;
                const double ry = s.ry != 0 ? s.ry : 1.0;
                canvas.rect(static_cast<float>(-rx), static_cast<float>(-ry),
                            static_cast<float>(rx * 2), static_cast<float>(ry * 2));
                break;
            }
            case ShapeType::Line:
                // The endpoint is absolute local space; translate() already
                // moved the origin onto (x, y).
                canvas.moveTo(0.0f, 0.0f);
                canvas.lineTo(static_cast<float>(s.x2 - s.x), static_cast<float>(s.y2 - s.y));
                break;
            case ShapeType::Curve:
                // Endpoint and both control points are absolute local space
                // too. A fill closes the path implicitly, chord from end back
                // to start, which is what turns a curve into a blob.
                canvas.moveTo(0.0f, 0.0f);
                canvas.bezierCurveTo(
                    static_cast<float>(s.cx1 - s.x), static_cast<float>(s.cy1 - s.y),
                    static_cast<float>(s.cx2 - s.x), static_cast<float>(s.cy2 - s.y),
                    static_cast<float>(s.x2 - s.x), static_cast<float>(s.y2 - s.y));
                break;
            case ShapeType::Polygon:
                if (s.points.size() >= 6) {
                    canvas.moveTo(static_cast<float>(s.points[0]), static_cast<float>(s.points[1]));
                    for (std::size_t j = 2; j + 1 < s.points.size(); j += 2) {
                        canvas.lineTo(static_cast<float>(s.points[j]),
                                      static_cast<float>(s.points[j + 1]));
                    }
                    canvas.closePath();
                }
                break;
        }
        if (!s.fill.empty() && s.t != ShapeType::Line) {
            setFill(canvas, hexColor(s.fill));
            canvas.fill();
        }
        if (!s.stroke.empty() && s.sw > 0) {
            setStroke(canvas, hexColor(s.stroke));
            canvas.setLineWidth(static_cast<float>(s.sw));
            canvas.setLineJoin("round");
            canvas.setLineCap("round");
            canvas.stroke();
        }
        canvas.restore();
    }
    canvas.restore();
}

/// One shape as one editable command line; round-trips through parseShapes.
std::string serializeShape(const Shape& s) {
    std::string out = skinShapeTypeName(s.t);
    out += " x=" + numberText(s.x) + " y=" + numberText(s.y);
    switch (s.t) {
        case ShapeType::Circle:
            out += " r=" + numberText(s.r);
            break;
        case ShapeType::Ellipse:
        case ShapeType::Rect:
            out += " rx=" + numberText(s.rx) + " ry=" + numberText(s.ry);
            break;
        case ShapeType::Line:
            out += " x2=" + numberText(s.x2) + " y2=" + numberText(s.y2);
            break;
        case ShapeType::Curve:
            out += " x2=" + numberText(s.x2) + " y2=" + numberText(s.y2);
            out += " cx1=" + numberText(s.cx1) + " cy1=" + numberText(s.cy1);
            out += " cx2=" + numberText(s.cx2) + " cy2=" + numberText(s.cy2);
            break;
        case ShapeType::Polygon:
            if (!s.points.empty()) {
                out += " points=";
                for (std::size_t i = 0; i < s.points.size(); ++i) {
                    if (i) out += ",";
                    out += numberText(s.points[i]);
                }
            }
            break;
    }
    if (s.t != ShapeType::Line && s.t != ShapeType::Curve && s.rot != 0) {
        out += " rot=" + numberText(s.rot);
    }
    if (!s.fill.empty()) out += " fill=" + s.fill;
    if (!s.stroke.empty()) out += " stroke=" + s.stroke;
    if (s.sw > 0) out += " sw=" + numberText(s.sw);
    return out;
}

// --- actions ---------------------------------------------------------------

enum class Act : std::uint8_t {
    None, Close, Tab, AddShape, SelectShape, MoveShape, DelShape, Step, Fill, Stroke,
    AddVertex, DelVertex, TextMode, Publish, Reset, Equip, Unequip, Delete,
    ConfirmDelete, CancelDelete,
};

enum class Field : std::uint8_t { X, Y, R, Rx, Ry, Rot, Sw };

struct Action {
    Act k = Act::None;
    int i = 0;              ///< shape index, tab, or shape type
    int dir = 0;
    Field field = Field::X;
    double delta = 0;
    std::string color;
    std::string id;
    std::string name;

    /// Identity for hover: two regions hover as one only when every field that
    /// distinguishes them agrees.
    bool operator==(const Action& o) const {
        return k == o.k && i == o.i && dir == o.dir && field == o.field && delta == o.delta &&
               color == o.color && id == o.id;
    }
};

struct HitRegion {
    Rect r;
    Action action;
};

enum class HandleKind : std::uint8_t { Anchor, End, Control1, Control2, Vertex };

struct Handle {
    HandleKind kind = HandleKind::Anchor;
    int vertex = 0;
    double lx = 0, ly = 0;
};

/// Where everything on the card sits, derived from the card. Nothing in this
/// panel carries an absolute coordinate any more: the studio used to be pinned
/// at a literal (20, 72) while MenuSystem hit-tested it at the corner anchor
/// every other overlay uses, so the card and its own mouse-capture box were
/// fourteen pixels apart.
struct Layout {
    Rect panel;
    Rect tabs;      ///< the tab row, full content width
    Rect body;      ///< everything between the tabs and the footer
    Rect footer;    ///< name field and the two actions
    Rect preview;   ///< the drawing board, top of the left column
    Rect left;      ///< board, shape palette and shape list
    Rect right;     ///< the selected shape's properties, or the text editor
};

Layout layoutOf(Rect panel) {
    Layout out;
    out.panel = panel;
    const double x = panel.x + kPad;
    const double w = panel.w - kPad * 2;
    out.tabs = {x, panel.y + kPad + kHeadingBand + 5.0, w, kTabHeight};
    out.footer = {x, panel.bottom() - kPad - kFooterHeight, w, kFooterHeight};

    const double top = out.tabs.bottom() + 12.0;
    out.body = {x, top, w, std::max(0.0, out.footer.y - 10.0 - top)};
    out.left = {x, top, kLeftWidth, out.body.h};
    out.preview = {x + (kLeftWidth - kPreviewSize) * 0.5, top, kPreviewSize, kPreviewSize};
    out.right = {x + kLeftWidth + kColumnGap, top, w - kLeftWidth - kColumnGap, out.body.h};
    return out;
}

/// The Browse tab has no footer: its cards run to the card's own bottom pad.
Rect browseBody(const Layout& l) {
    const double top = l.tabs.bottom() + 30.0;
    return {l.body.x, top, l.body.w, std::max(0.0, l.panel.bottom() - kPad - top)};
}

/// Everything the studio keeps between frames.
struct Studio {
    enum class Tab : std::uint8_t { Create, Browse };

    Tab tab = Tab::Create;
    std::vector<Shape> shapes = starterShapes();
    int selected = 0;
    std::string skinName;

    /// The published catalog, borrowed from NetClient rather than copied.
    ///
    /// The reference keeps its own array and refills it from applyCatalog /
    /// applySkinPublished / applySkinDeleted; here the connection already
    /// maintains exactly that list -- and has to, because the world renderer
    /// resolves a wearer's skin through the same registry. Two copies of it
    /// would be two things to keep in step. Null before the first render, and
    /// whenever the panel draws without a connection.
    const NetClient* net = nullptr;

    static const std::vector<CustomSkin>& noSkins() {
        static const std::vector<CustomSkin> none;
        return none;
    }
    const std::vector<CustomSkin>& catalog() const {
        return net ? net->skinCatalog() : noSkins();
    }
    bool isAdmin() const { return net && net->isSkinAdmin(); }
    const std::string& equippedId() const {
        static const std::string none;
        return net ? net->equippedSkinId() : none;
    }

    /// This frame's geometry, set before input runs so the hit tests and the
    /// paint share one answer.
    Layout layout;

    /// Regions from the frame just drawn. The panel hit-tests and hovers
    /// against the last completed render's list.
    std::vector<HitRegion> regions;
    /// What a region is cut to. The panel while laying out its chrome, and the
    /// viewport while laying out a scrolled list: a row or a card at the edge
    /// of one is painted in part, and must answer for clicks on that part
    /// only. Without this, the Equip button of a card half-scrolled off the
    /// bottom went on taking clicks below the card entirely.
    Rect regionClip;
    Action hover;
    bool hovering = false;

    Scroller shapeScroll;
    Scroller browseScroll;

    bool dragging = false;
    HandleKind dragHandle = HandleKind::Anchor;
    int dragVertex = 0;

    bool textMode = false;
    std::string textError;
    std::string textBuffer;
    /// Caret, selection and focus for the two editors. The name box is an
    /// ordinary field on the footer, so "naming" is just where the caret is.
    TextFieldState textField;
    TextFieldState nameField;

    bool confirming = false;
    std::string confirmId;
    std::string confirmName;

    Shape* selectedShape() {
        if (selected < 0 || static_cast<std::size_t>(selected) >= shapes.size()) return nullptr;
        return &shapes[static_cast<std::size_t>(selected)];
    }

    std::string serializeShapes() const {
        std::string out;
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            if (i) out += "\n";
            out += serializeShape(shapes[i]);
        }
        return out;
    }

    /// The board shows +-40 local units.
    double previewScale() const { return (layout.preview.w / 2) / 40.0; }
    Vec2 toPx(double lx, double ly) const {
        const Rect pr = layout.preview;
        const double s = previewScale();
        return {pr.x + pr.w / 2 + lx * s, pr.y + pr.h / 2 + ly * s};
    }
    Vec2 toLocal(Vec2 p) const {
        const Rect pr = layout.preview;
        const double s = previewScale();
        return {(p.x - (pr.x + pr.w / 2)) / s, (p.y - (pr.y + pr.h / 2)) / s};
    }

    std::vector<Handle> handles();

    void addRegion(Rect, const Action&);

    // draw
    void draw(MenuContext&);
    void drawTabs(Canvas&, Vec2 mouse);
    void drawPreview(Canvas&);
    void drawShapeList(Canvas&, Rect);
    void drawProps(Canvas&, Rect);
    double drawPalette(Canvas&, Rect, const char* caption, const std::string& current,
                       bool isFill);
    void stepper(Canvas&, Rect, const char* caption, Field, double value, double step);
    void drawTextEditor(Canvas&, Rect, double timeSeconds);
    void drawCommandHelp(Canvas&, Rect);
    void drawFooter(Canvas&, double timeSeconds);
    void drawBrowse(Canvas&, const std::string& me);
    void drawConfirm(Canvas&, Vec2 mouse);
    /// A button that registers its own hit region, so the input pass that runs
    /// before the next paint knows what was under the cursor.
    void actionButton(Canvas&, Rect, const std::string& label, const Action&,
                      std::uint32_t fill, double textSize = kBodySize);
    void actionChip(Canvas&, Rect, const std::string& label, const Action&,
                    std::uint32_t fill = kRowFill, double textSize = kCaptionSize);
    void iconBtn(Canvas&, Rect, int kind, const Action&);

    // input
    bool handleInput(MenuContext&);
    bool dispatch(MenuContext&, const Action&);
    void step(Field, double delta);
    void applyDrag(Vec2 mouse);
    void enterTextMode();
    void applyTextBuffer();
    std::string parseShapes(const std::string& text, std::vector<Shape>& out) const;
};

/// The studio's state belongs on SkinsPanel, but that class is declared in
/// menus.h, which this file does not own. Exactly one panel instance exists,
/// so a file-local singleton is that same object with a longer reach.
Studio& studio() {
    static Studio state;
    return state;
}

std::vector<Handle> Studio::handles() {
    std::vector<Handle> out;
    const Shape* s = selectedShape();
    if (!s) return out;
    out.push_back({HandleKind::Anchor, 0, s->x, s->y});
    if (s->t == ShapeType::Line) out.push_back({HandleKind::End, 0, s->x2, s->y2});
    if (s->t == ShapeType::Curve) {
        out.push_back({HandleKind::End, 0, s->x2, s->y2});
        out.push_back({HandleKind::Control1, 0, s->cx1, s->cy1});
        out.push_back({HandleKind::Control2, 0, s->cx2, s->cy2});
    }
    if (s->t == ShapeType::Polygon) {
        for (std::size_t i = 0; i + 1 < s->points.size(); i += 2) {
            out.push_back({HandleKind::Vertex, static_cast<int>(i / 2), s->x + s->points[i],
                           s->y + s->points[i + 1]});
        }
    }
    return out;
}

// --- widgets ---------------------------------------------------------------

void Studio::addRegion(Rect r, const Action& action) {
    const double x0 = std::max(r.x, regionClip.x);
    const double y0 = std::max(r.y, regionClip.y);
    const double x1 = std::min(r.right(), regionClip.right());
    const double y1 = std::min(r.bottom(), regionClip.bottom());
    if (x1 <= x0 || y1 <= y0) return;
    regions.push_back({{x0, y0, x1 - x0, y1 - y0}, action});
}

void Studio::actionButton(Canvas& canvas, Rect r, const std::string& caption,
                          const Action& action, std::uint32_t fill, double textSize) {
    ui::button(canvas, r, caption, hovering && hover == action, false, buttonStyle(fill, textSize));
    addRegion(r, action);
}

void Studio::actionChip(Canvas& canvas, Rect r, const std::string& caption, const Action& action,
                        std::uint32_t fill, double textSize) {
    ChipStyle style;
    style.fill = fill;
    style.border = kWell;
    style.textSize = textSize;
    ui::chip(canvas, r, caption, hovering && hover == action, style);
    addRegion(r, action);
}

/// The three glyph buttons on a shape row: raise, lower, delete. Drawn rather
/// than typed -- the bundled face has no coverage for the arrows or the cross.
void Studio::iconBtn(Canvas& canvas, Rect r, int kind, const Action& action) {
    const bool hovered = hovering && hover == action;
    const std::uint32_t fill = kind == 2 ? kDangerFill : kRowFill;
    inlaid(canvas, r, hovered ? lighten(fill, 0.15) : fill, kWell, 2.0, 4.0);

    const double cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    canvas.save();
    canvas.setLineWidth(1.8f);
    canvas.setLineCap("round");
    setStroke(canvas, kPaper);
    canvas.beginPath();
    if (kind == 0) {
        canvas.moveTo(static_cast<float>(cx - 4), static_cast<float>(cy + 2));
        canvas.lineTo(static_cast<float>(cx), static_cast<float>(cy - 3));
        canvas.lineTo(static_cast<float>(cx + 4), static_cast<float>(cy + 2));
    } else if (kind == 1) {
        canvas.moveTo(static_cast<float>(cx - 4), static_cast<float>(cy - 2));
        canvas.lineTo(static_cast<float>(cx), static_cast<float>(cy + 3));
        canvas.lineTo(static_cast<float>(cx + 4), static_cast<float>(cy - 2));
    } else {
        canvas.moveTo(static_cast<float>(cx - 3.5), static_cast<float>(cy - 3.5));
        canvas.lineTo(static_cast<float>(cx + 3.5), static_cast<float>(cy + 3.5));
        canvas.moveTo(static_cast<float>(cx + 3.5), static_cast<float>(cy - 3.5));
        canvas.lineTo(static_cast<float>(cx - 3.5), static_cast<float>(cy + 3.5));
    }
    canvas.stroke();
    canvas.restore();
    addRegion(r, action);
}

/// A captioned number: the caption over a [-][value][+] row.
void Studio::stepper(Canvas& canvas, Rect r, const char* caption, Field field, double value,
                     double stepBy) {
    ui::text(canvas, caption, r.x, r.y + 7.0, label(kCaptionSize));

    const double by = r.y + 16.0, bh = r.h - 16.0, bw = 22.0;
    const double valW = std::max(20.0, r.w - bw * 2 - 8.0);

    Action down;
    down.k = Act::Step;
    down.field = field;
    down.delta = -stepBy;
    actionChip(canvas, {r.x, by, bw, bh}, "-", down, kRowFill, kBodySize);

    const Rect well{r.x + bw + 4.0, by, valW, bh};
    fillRound(canvas, well, 3.0, kWell);
    ui::text(canvas, numberText(value), well.x + well.w / 2, well.y + well.h / 2,
             label(kBodySize, false, Align::Centre));

    Action up;
    up.k = Act::Step;
    up.field = field;
    up.delta = stepBy;
    actionChip(canvas, {r.x + bw + valW + 8.0, by, bw, bh}, "+", up, kRowFill, kBodySize);
}

/// The swatch grid. Returns the y the next block may start at.
double Studio::drawPalette(Canvas& canvas, Rect r, const char* caption,
                           const std::string& current, bool isFill) {
    ui::text(canvas, caption, r.x, r.y + 7.0, label(kCaptionSize));
    // The first cell is "none": no fill / no outline, which is a real choice.
    const int cells = kPaletteCount + 1;
    const double size = std::min(kSwatchMax, (r.w - (cells - 1) * kSwatchGap) / cells);
    const double sy = r.y + 18.0;
    for (int i = 0; i < cells; ++i) {
        const std::string colour = i == 0 ? std::string() : std::string(kPalette[i - 1]);
        const Rect cell{r.x + i * (size + kSwatchGap), sy, size, size};
        const bool isCurrent = current == colour;

        if (colour.empty()) {
            fillRound(canvas, cell, 3.0, kSwatchNone);
            strokeRound(canvas, {cell.x + 0.5, cell.y + 0.5, cell.w - 1, cell.h - 1}, 3.0,
                        kSwatchNoneEdge, 1.0);
            canvas.save();
            setStroke(canvas, kSwatchSlash);
            canvas.setLineWidth(1.5f);
            canvas.beginPath();
            canvas.moveTo(static_cast<float>(cell.x + 4), static_cast<float>(cell.bottom() - 4));
            canvas.lineTo(static_cast<float>(cell.right() - 4), static_cast<float>(cell.y + 4));
            canvas.stroke();
            canvas.restore();
        } else {
            fillRound(canvas, cell, 3.0, hexColor(colour));
        }
        // The selected swatch wears the accent ring; every other one keeps the
        // hairline that separates a white or near-black chip from the card.
        strokeRound(canvas, {cell.x + 1, cell.y + 1, cell.w - 2, cell.h - 2}, 3.0,
                    isCurrent ? kAccentPurple : kInk, isCurrent ? 2.5 : 1.0);

        Action pick;
        pick.k = isFill ? Act::Fill : Act::Stroke;
        pick.color = colour;
        addRegion(cell, pick);
    }
    return sy + size + 14.0;
}

// --- the panel -------------------------------------------------------------

void Studio::drawTabs(Canvas& canvas, Vec2 mouse) {
    const Rect row = layout.tabs;
    static const char* const kLabels[] = {"Create", "Browse"};
    for (int i = 0; i < 2; ++i) {
        const Rect r{row.x + i * (kTabWidth + kTabGap), row.y, kTabWidth, row.h};
        const bool active = static_cast<int>(tab) == i;
        Action pick;
        pick.k = Act::Tab;
        pick.i = i;
        ui::button(canvas, r, kLabels[i], !active && r.contains(mouse), false,
                   buttonStyle(active ? kAccentPurple : kNeutralFill, kBodySize));
        addRegion(r, pick);
    }

    // The row's right end carries the tab's own one control: the editing mode
    // while creating, and the way out of a skin while browsing.
    if (tab == Tab::Create) {
        Action mode;
        mode.k = Act::TextMode;
        const Rect r{row.right() - 110.0, row.y, 110.0, row.h};
        actionButton(canvas, r, textMode ? "Visual editor" : "Text editor", mode,
                     textMode ? kAccentPurple : kNeutralFill);
    } else {
        Action unequip;
        unequip.k = Act::Unequip;
        const Rect r{row.right() - 110.0, row.y, 110.0, row.h};
        actionButton(canvas, r, "Unequip", unequip, kNeutralFill);
    }
}

void Studio::drawPreview(Canvas& canvas) {
    const Rect pr = layout.preview;
    canvas.save();
    // Sunk into the card the way every other surface on it is: the well's
    // colour as a frame, the board inside it.
    inlaid(canvas, pr, kBoard, kWell, 3.0, 6.0);
    canvas.save();
    clipRound(canvas, {pr.x + 3, pr.y + 3, pr.w - 6, pr.h - 6}, 4.0);
    canvas.translate(static_cast<float>(pr.x + pr.w / 2), static_cast<float>(pr.y + pr.h / 2));
    const double s = previewScale();

    setStroke(canvas, kPaper, 0.25);
    canvas.setLineDash({4.0f, 4.0f});
    canvas.setLineWidth(1.0f);
    canvas.beginPath();
    canvas.arc(0.0f, 0.0f, static_cast<float>(25 * s), 0.0f, static_cast<float>(kPi * 2));
    canvas.stroke();
    canvas.setLineDash({});

    renderSkinShapes(canvas, shapes, 25 * s);
    canvas.restore();

    // Handles are drawn AFTER the clip is released, in panel space, so a
    // handle dragged past the edge stays grabbable outside the board.
    if (!textMode) {
        const Shape* sel = selectedShape();
        if (sel && sel->t == ShapeType::Curve) {
            const Vec2 a = toPx(sel->x, sel->y);
            const Vec2 b = toPx(sel->x2, sel->y2);
            const Vec2 g1 = toPx(sel->cx1, sel->cy1);
            const Vec2 g2 = toPx(sel->cx2, sel->cy2);
            canvas.save();
            setStroke(canvas, kPaper, 0.55);
            canvas.setLineWidth(1.0f);
            canvas.setLineDash({3.0f, 3.0f});
            canvas.beginPath();
            canvas.moveTo(static_cast<float>(a.x), static_cast<float>(a.y));
            canvas.lineTo(static_cast<float>(g1.x), static_cast<float>(g1.y));
            canvas.moveTo(static_cast<float>(b.x), static_cast<float>(b.y));
            canvas.lineTo(static_cast<float>(g2.x), static_cast<float>(g2.y));
            canvas.stroke();
            canvas.restore();
        }
        for (const Handle& h : handles()) {
            const Vec2 p = toPx(h.lx, h.ly);
            const bool control = h.kind == HandleKind::Control1 || h.kind == HandleKind::Control2;
            setFill(canvas, control ? kHandleControl
                                    : h.kind == HandleKind::Anchor ? kAccentPurple : kPaper);
            setStroke(canvas, kInk);
            canvas.setLineWidth(1.0f);
            canvas.beginPath();
            // Control points read as circles; anchors and vertices stay square.
            if (control) {
                canvas.arc(static_cast<float>(p.x), static_cast<float>(p.y), 3.5f, 0.0f,
                           static_cast<float>(kPi * 2));
            } else {
                canvas.rect(static_cast<float>(p.x - 3), static_cast<float>(p.y - 3), 6.0f, 6.0f);
            }
            canvas.fill();
            canvas.stroke();
        }
    }
    canvas.restore();

    ui::text(canvas, textMode ? "Live preview" : "Drag the handles to shape it",
             pr.x + pr.w / 2, pr.bottom() + 12.0, label(kCaptionSize, false, Align::Centre));
}

void Studio::drawShapeList(Canvas& canvas, Rect view) {
    if (view.h <= 0) return;
    inlaid(canvas, view, kWell, kSkinsSkin.border, 2.0, 6.0);

    shapeScroll.contentHeight = static_cast<double>(shapes.size()) * kRowPitch + 8.0;
    shapeScroll.viewHeight = view.h;
    shapeScroll.offset = clamp(shapeScroll.offset, 0.0, shapeScroll.maxOffset());

    canvas.save();
    clipRound(canvas, {view.x + 2, view.y + 2, view.w - 4, view.h - 4}, 5.0);
    const Rect outerClip = regionClip;
    regionClip = view;

    const bool scrolls = shapeScroll.contentHeight > view.h;
    const double rowW = view.w - 12.0 - (scrolls ? kScrollbarWidth + 2.0 : 0.0);
    double ry = view.y + 4.0 - shapeScroll.offset;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const Shape& s = shapes[i];
        if (ry + kRowCard > view.y && ry < view.bottom()) {
            const bool sel = static_cast<int>(i) == selected;
            const Rect row{view.x + 6.0, ry, rowW, kRowCard};
            if (sel) inlaid(canvas, row, kAccentPurple, kSkinsSkin.border, 2.0, 4.0);

            // The shape's own colour, as the swatch that identifies its row.
            const Rect swatch{row.x + 7.0, row.y + (kRowCard - 12.0) * 0.5, 12.0, 12.0};
            fillRound(canvas, swatch, 2.0, hexColor(!s.fill.empty() ? s.fill : s.stroke, kInk));
            strokeRound(canvas, swatch, 2.0, kInk, 1.0);

            // Not selectable: the row IS the control that picks this shape, so
            // its name belongs to the button rather than to the page.
            {
                TextCaptureScope off(false);
                ui::text(canvas, std::to_string(i + 1) + ". " + shortType(s.t),
                         swatch.right() + 8.0, row.y + kRowCard * 0.5, label(kBodySize, sel));
            }

            const double bx = row.right() - 3.0 * 22.0 - 2.0 * 3.0 - 4.0;
            const double by = row.y + (kRowCard - 20.0) * 0.5;
            Action up;
            up.k = Act::MoveShape;
            up.i = static_cast<int>(i);
            up.dir = -1;
            iconBtn(canvas, {bx, by, 22.0, 20.0}, 0, up);
            Action down = up;
            down.dir = 1;
            iconBtn(canvas, {bx + 25.0, by, 22.0, 20.0}, 1, down);
            Action del;
            del.k = Act::DelShape;
            del.i = static_cast<int>(i);
            iconBtn(canvas, {bx + 50.0, by, 22.0, 20.0}, 2, del);

            Action pick;
            pick.k = Act::SelectShape;
            pick.i = static_cast<int>(i);
            addRegion({row.x, row.y, bx - row.x - 4.0, kRowCard}, pick);
        }
        ry += kRowPitch;
    }
    canvas.restore();
    regionClip = outerClip;

    scrollbar(canvas, {view.x + 2, view.y + 5, view.w - 7, view.h - 10},
              shapeScroll.contentHeight, shapeScroll.offset, kAccentPurple, kScrollbarWidth);
}

void Studio::drawProps(Canvas& canvas, Rect r) {
    ui::text(canvas, "Selected shape", r.x, r.y + 7.0, label(kSectionSize, true));
    const Shape* s = selectedShape();
    if (!s) {
        ui::text(canvas, "Nothing selected \xe2\x80\x94 add a shape below.", r.x, r.y + 30.0,
                 label(kBodySize));
        return;
    }

    struct Row {
        const char* caption;
        Field field;
        double value;
        double step;
    };
    std::vector<Row> rows;
    rows.push_back({"X", Field::X, s->x, 1});
    rows.push_back({"Y", Field::Y, s->y, 1});
    if (s->t == ShapeType::Circle) rows.push_back({"Radius", Field::R, s->r, 1});
    if (s->t == ShapeType::Ellipse || s->t == ShapeType::Rect) {
        rows.push_back({"Width", Field::Rx, s->rx, 1});
        rows.push_back({"Height", Field::Ry, s->ry, 1});
    }
    // Lines and curves are authored by dragging absolute endpoints, which the
    // handles report unrotated -- so no rotation control for those.
    if (s->t != ShapeType::Line && s->t != ShapeType::Curve) {
        rows.push_back({"Rotation", Field::Rot, s->rot, 15});
    }
    rows.push_back({"Outline width", Field::Sw, s->sw, 1});

    constexpr double kStepperH = 40.0;
    constexpr double kStepperGap = 8.0;
    double cy = r.y + 22.0;
    const double colW = (r.w - 12.0) / 2;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::size_t col = i % 2, row = i / 2;
        stepper(canvas,
                {r.x + static_cast<double>(col) * (colW + 12.0),
                 cy + static_cast<double>(row) * (kStepperH + kStepperGap), colW, kStepperH},
                rows[i].caption, rows[i].field, rows[i].value, rows[i].step);
    }
    cy += static_cast<double>((rows.size() + 1) / 2) * (kStepperH + kStepperGap) + 2.0;

    if (s->t == ShapeType::Polygon) {
        Action add;
        add.k = Act::AddVertex;
        actionChip(canvas, {r.x, cy, 96.0, 24.0}, "Add point", add, kRowFill, kBodySize);
        Action del;
        del.k = Act::DelVertex;
        actionChip(canvas, {r.x + 104.0, cy, 96.0, 24.0}, "Remove point", del, kRowFill,
                   kBodySize);
        cy += 32.0;
    }

    if (s->t == ShapeType::Curve) {
        ui::text(canvas, "Drag the two round handles to bend it;", r.x, cy + 8.0,
                 label(kCaptionSize));
        ui::text(canvas, "a fill closes the curve into a blob.", r.x, cy + 22.0,
                 label(kCaptionSize));
        cy += 34.0;
    }

    if (s->t != ShapeType::Line) {
        cy = drawPalette(canvas, {r.x, cy, r.w, 0}, "Fill", s->fill, true);
    }
    drawPalette(canvas, {r.x, cy, r.w, 0}, "Outline", s->stroke, false);
}

void Studio::drawTextEditor(Canvas& canvas, Rect r, double timeSeconds) {
    inlaid(canvas, r, kWell, kSkinsSkin.border, 2.0, 6.0);

    canvas.save();
    clipRound(canvas, {r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 5.0);
    constexpr double fontPx = 12.5;
    constexpr double lineH = fontPx * 1.5;
    TextStyle body = label(fontPx);
    body.bold = false;
    body.strokeWidth = 0;

    const TextSelection& sel = textField.selection;
    std::size_t lineStart = 0;
    int lineIndex = 0;
    for (std::size_t i = 0; i <= textBuffer.size(); ++i) {
        if (i != textBuffer.size() && textBuffer[i] != '\n') continue;
        const std::string line = textBuffer.substr(lineStart, i - lineStart);
        const double baseY = r.y + 10.0 + lineH * (lineIndex + 0.5);
        TextRun run;
        run.text = line;
        run.originX = r.x + 10.0;
        run.size = fontPx;

        // One highlight per line, cut to the part of that line the selection
        // actually covers -- a multi-line selection is a stack of these.
        if (sel.end() > lineStart && sel.begin() <= i) {
            TextSelection onLine;
            onLine.anchor = sel.begin() > lineStart ? sel.begin() - lineStart : 0;
            onLine.caret = std::min(sel.end(), i) - lineStart;
            selectionHighlight(canvas, run, onLine,
                               Rect{r.x + 4.0, baseY - lineH * 0.5, r.w - 8.0, lineH}, kPaper,
                               0.30);
        }
        if (baseY < r.bottom()) ui::text(canvas, line, run.originX, baseY, body);

        if (sel.caret >= lineStart && sel.caret <= i && caretVisible(textField, timeSeconds)) {
            const double caretX = xOfIndex(run, sel.caret - lineStart);
            setFill(canvas, kPaper);
            canvas.fillRect(static_cast<float>(caretX), static_cast<float>(baseY - fontPx * 0.6),
                            1.0f, static_cast<float>(fontPx * 1.2));
        }
        lineStart = i + 1;
        ++lineIndex;
    }
    canvas.restore();
}

void Studio::drawCommandHelp(Canvas& canvas, Rect r) {
    double hy = r.y + 8.0;
    ui::text(canvas, "Canvas commands", r.x, hy, label(kSectionSize, true));
    hy += 20.0;
    static const char* const kHelp[] = {
        "One shape per line:",
        "type x=.. y=.. fill=#rrggbb",
        "types: circle ellipse rect",
        "       line polygon curve",
        "circle: r=",
        "ellipse / rect: rx= ry=",
        "line: x2= y2=",
        "polygon: points=x,y,x,y",
        "curve: x2= y2= cx1= cy1=",
        "       cx2= cy2=",
        "optional: rot= stroke= sw=",
    };
    for (const char* line : kHelp) {
        if (hy > r.bottom()) break;
        ui::text(canvas, line, r.x, hy, label(kCaptionSize));
        hy += 15.0;
    }
    if (textError.empty()) return;
    hy += 6.0;
    // Wrapped by hand at the column width: a parse error names the line and
    // the key, and both together overrun a 170px column.
    std::string rest = textError;
    while (!rest.empty() && hy <= r.bottom()) {
        std::size_t take = rest.size();
        while (take > 1 && measure(rest.substr(0, take), kCaptionSize, true) > r.w) --take;
        if (take < rest.size()) {
            const std::size_t space = rest.rfind(' ', take);
            if (space != std::string::npos && space > 0) take = space;
        }
        ui::text(canvas, rest.substr(0, take), r.x, hy, label(kCaptionSize, true, Align::Left,
                                                              kErrorText));
        rest.erase(0, take);
        while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
        hy += 15.0;
    }
}

void Studio::drawFooter(Canvas& canvas, double timeSeconds) {
    const Rect r = layout.footer;
    constexpr double kPublishW = 106.0;
    constexpr double kResetW = 84.0;

    const Rect field{r.x, r.y, std::max(120.0, r.w - kPublishW - kResetW - 24.0), r.h};
    inputField(canvas, field, skinName, "Skin name", nameField.focused, timeSeconds, &nameField);

    Action reset;
    reset.k = Act::Reset;
    actionButton(canvas, {r.right() - kPublishW - kResetW - 8.0, r.y, kResetW, r.h}, "Reset",
                 reset, kNeutralFill);
    Action publish;
    publish.k = Act::Publish;
    actionButton(canvas, {r.right() - kPublishW, r.y, kPublishW, r.h}, "Publish", publish,
                 kAccent);
}

void Studio::drawBrowse(Canvas& canvas, const std::string& me) {
    ui::text(canvas, "Published skins \xe2\x80\x94 equip one and everyone sees it.",
             layout.body.x, layout.tabs.bottom() + 16.0, label(kBodySize));

    const Rect view = browseBody(layout);
    const std::vector<CustomSkin>& skins = catalog();
    if (skins.empty()) {
        ui::text(canvas, "Nothing published yet. Make one in the Create tab.", view.x,
                 view.y + 24.0, label(kBodySize));
        return;
    }

    // Four to a row rather than three: a 135px card fits its own preview,
    // its name and a full-width Equip button, which the old three-up card did
    // not -- its preview was derived from the card WIDTH and overran the
    // bottom edge, so the buttons painted on top of the artwork.
    constexpr int cols = 4;
    constexpr double kGap = 10.0;
    constexpr double kCardH = 180.0;
    const double cardW = (view.w - (cols - 1) * kGap - kScrollbarWidth - 4.0) / cols;
    const double preview = cardW - 30.0;

    std::vector<const CustomSkin*> sorted;
    sorted.reserve(skins.size());
    for (const CustomSkin& skin : skins) sorted.push_back(&skin);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const CustomSkin* a, const CustomSkin* b) {
                         return a->createdAt > b->createdAt;
                     });

    const double rows = std::ceil(static_cast<double>(sorted.size()) / cols);
    browseScroll.contentHeight = rows * (kCardH + kGap) - kGap;
    browseScroll.viewHeight = view.h;
    browseScroll.offset = clamp(browseScroll.offset, 0.0, browseScroll.maxOffset());

    canvas.save();
    clipRect(canvas, view);
    const Rect outerClip = regionClip;
    regionClip = view;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const CustomSkin& skin = *sorted[i];
        const std::size_t col = i % cols, row = i / cols;
        const Rect card{view.x + static_cast<double>(col) * (cardW + kGap),
                        view.y + static_cast<double>(row) * (kCardH + kGap) - browseScroll.offset,
                        cardW, kCardH};
        if (card.bottom() < view.y || card.y > view.bottom()) continue;
        const bool equipped = skin.id == equippedId();
        inlaid(canvas, card, kWell, equipped ? kAccentPurple : kSkinsSkin.border, 2.0, 6.0);

        const Rect board{card.x + (cardW - preview) * 0.5, card.y + 10.0, preview, preview};
        canvas.save();
        inlaid(canvas, board, kBoard, kWell, 2.0, 4.0);
        clipRound(canvas, {board.x + 2, board.y + 2, board.w - 4, board.h - 4}, 3.0);
        canvas.translate(static_cast<float>(board.x + board.w / 2),
                         static_cast<float>(board.y + board.h / 2));
        renderSkinShapes(canvas, skin.shapes, board.w * 0.36);
        canvas.restore();

        ui::text(canvas, clipChars(skin.name, 14), card.x + cardW * 0.5, board.bottom() + 14.0,
                 label(kBodySize, true, Align::Centre));
        ui::text(canvas, "by " + clipChars(skin.author, 14), card.x + cardW * 0.5,
                 board.bottom() + 30.0, label(kCaptionSize, false, Align::Centre));

        std::string author = skin.author;
        for (char& c : author) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool canDelete = isAdmin() || author == me;

        // A takedown is a glyph beside the Equip button, not a second labelled
        // one: at this card width two labelled buttons side by side left
        // neither of them legible, and a cross laid over the card's corner
        // covered the artwork it belongs to.
        const double actionsY = card.bottom() - 32.0;
        const double equipW = cardW - 16.0 - (canDelete ? 28.0 : 0.0);
        Action equip;
        equip.k = Act::Equip;
        equip.id = skin.id;
        actionButton(canvas, {card.x + 8.0, actionsY, equipW, 24.0},
                     equipped ? "Equipped" : "Equip", equip,
                     equipped ? kAccent : kAccentPurple, kBodySize);
        if (canDelete) {
            Action del;
            del.k = Act::Delete;
            del.id = skin.id;
            del.name = skin.name;
            iconBtn(canvas, {card.x + 8.0 + equipW + 4.0, actionsY, 24.0, 24.0}, 2, del);
        }
    }
    canvas.restore();
    regionClip = outerClip;

    scrollbar(canvas, {view.x, view.y, view.w, view.h - 5.0}, browseScroll.contentHeight,
              browseScroll.offset, kAccentPurple, kScrollbarWidth);
}

void Studio::drawConfirm(Canvas& canvas, Vec2 mouse) {
    // A confirm is modal: nothing behind it can be clicked, so the regions
    // drawn so far are dropped rather than merely painted over. Built like the
    // guild panel's, which stands in for the same browser confirm().
    regions.clear();
    regionClip = layout.panel;
    const Rect box{layout.panel.x + (layout.panel.w - 320.0) * 0.5,
                   layout.panel.y + (layout.panel.h - 116.0) * 0.5, 320.0, 116.0};
    overlayCard(canvas, box, kSkinsSkin);
    ui::text(canvas, "Delete \"" + clipChars(confirmName, 22) + "\"?", box.x + box.w * 0.5,
             box.y + 38.0, label(kSectionSize, true, Align::Centre));

    constexpr double kBtnW = 116.0, kBtnH = 30.0;
    const double by = box.bottom() - kBtnH - 18.0;
    const Rect cancel{box.x + box.w * 0.5 - kBtnW - 8.0, by, kBtnW, kBtnH};
    const Rect accept{box.x + box.w * 0.5 + 8.0, by, kBtnW, kBtnH};

    Action no;
    no.k = Act::CancelDelete;
    ui::button(canvas, cancel, "Cancel", cancel.contains(mouse), false,
               buttonStyle(kNeutralFill));
    addRegion(cancel, no);

    Action yes;
    yes.k = Act::ConfirmDelete;
    ui::button(canvas, accept, "Delete", accept.contains(mouse), false,
               buttonStyle(kDangerFill));
    addRegion(accept, yes);
}

void Studio::draw(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Vec2 mouse = ctx.mouse();
    regions.clear();

    const Rect panel = layout.panel;
    regionClip = panel;
    overlayCard(canvas, panel, kSkinsSkin);
    panelHeading(canvas, panel, "Skin Studio");

    const Rect closeRect = closeButtonRect(panel);
    ui::panelClose(canvas, closeRect, closeRect.contains(mouse));
    Action close;
    close.k = Act::Close;
    addRegion(closeRect, close);

    drawTabs(canvas, mouse);

    if (tab == Tab::Create) {
        drawPreview(canvas);
        if (textMode) {
            drawCommandHelp(canvas, {layout.left.x, layout.preview.bottom() + 22.0,
                                     layout.left.w, layout.body.bottom() -
                                                        (layout.preview.bottom() + 22.0)});
            drawTextEditor(canvas, layout.right, ctx.timeSeconds);
        } else {
            const double listTop = layout.preview.bottom() + 22.0;
            ui::text(canvas, "Add shape", layout.left.x, listTop + 7.0,
                     label(kSectionSize, true));

            // Two rows of three; six types do not fit legibly on one.
            static const ShapeType kTypes[] = {ShapeType::Circle,  ShapeType::Ellipse,
                                               ShapeType::Rect,    ShapeType::Polygon,
                                               ShapeType::Line,    ShapeType::Curve};
            constexpr int perRow = 3;
            constexpr double gap = 4.0;
            const double bw = (layout.left.w - (perRow - 1) * gap) / perRow;
            const double by = listTop + 20.0;
            for (int i = 0; i < 6; ++i) {
                const int col = i % perRow, row = i / perRow;
                Action add;
                add.k = Act::AddShape;
                add.i = static_cast<int>(kTypes[i]);
                actionChip(canvas,
                           {layout.left.x + col * (bw + gap), by + row * 26.0, bw, 22.0},
                           shortType(kTypes[i]), add);
            }
            const double afterChips = by + 2 * 26.0 + 6.0;
            drawShapeList(canvas, {layout.left.x, afterChips, layout.left.w,
                                   layout.body.bottom() - afterChips});
            drawProps(canvas, layout.right);
        }
        drawFooter(canvas, ctx.timeSeconds);
    } else {
        std::string me = ctx.net.profile().username;
        for (char& c : me) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        drawBrowse(canvas, me);
    }

    if (confirming) drawConfirm(canvas, mouse);
}

// --- input -----------------------------------------------------------------

void Studio::step(Field field, double delta) {
    Shape* s = selectedShape();
    if (!s) return;
    const auto set = [delta](double v, double lo, double hi) {
        return round1(clamp(v + delta, lo, hi));
    };
    switch (field) {
        case Field::X: s->x = set(s->x, -kSkinCoordLimit, kSkinCoordLimit); break;
        case Field::Y: s->y = set(s->y, -kSkinCoordLimit, kSkinCoordLimit); break;
        case Field::R: s->r = set(s->r, 0.5, kSkinRadiusLimit); break;
        case Field::Rx: s->rx = set(s->rx, 0.5, kSkinRadiusLimit); break;
        case Field::Ry: s->ry = set(s->ry, 0.5, kSkinRadiusLimit); break;
        case Field::Rot: s->rot = set(s->rot, -180.0, 180.0); break;
        case Field::Sw: s->sw = set(s->sw, 0.0, kMaxSkinStrokeWidth); break;
    }
}

void Studio::applyDrag(Vec2 mouse) {
    Shape* s = selectedShape();
    if (!s) return;
    const Vec2 l = toLocal(mouse);
    const double cx = clamp(l.x, -kSkinCoordLimit, kSkinCoordLimit);
    const double cy = clamp(l.y, -kSkinCoordLimit, kSkinCoordLimit);
    switch (dragHandle) {
        case HandleKind::Anchor: s->x = round1(cx); s->y = round1(cy); break;
        case HandleKind::End: s->x2 = round1(cx); s->y2 = round1(cy); break;
        case HandleKind::Control1: s->cx1 = round1(cx); s->cy1 = round1(cy); break;
        case HandleKind::Control2: s->cx2 = round1(cx); s->cy2 = round1(cy); break;
        case HandleKind::Vertex: {
            const std::size_t i = static_cast<std::size_t>(dragVertex) * 2;
            if (i + 1 < s->points.size()) {
                s->points[i] = round1(cx - s->x);
                s->points[i + 1] = round1(cy - s->y);
            }
            break;
        }
    }
}

void Studio::enterTextMode() {
    textBuffer = serializeShapes();
    textField.focusAtEnd(textBuffer, 0.0);
    textError.clear();
}

void Studio::applyTextBuffer() {
    std::vector<Shape> parsed;
    const std::string error = parseShapes(textBuffer, parsed);
    if (!error.empty()) {
        textError = error;
        return;
    }
    textError.clear();
    shapes = parsed;
    if (selected >= static_cast<int>(shapes.size())) selected = static_cast<int>(shapes.size()) - 1;
    if (selected < 0) selected = 0;
}

std::string Studio::parseShapes(const std::string& source, std::vector<Shape>& out) const {
    out.clear();
    std::size_t at = 0;
    int lineNo = 0;
    while (at <= source.size()) {
        const std::size_t end = std::min(source.find('\n', at), source.size());
        std::string raw = source.substr(at, end - at);
        at = end + 1;
        ++lineNo;

        const std::size_t first = raw.find_first_not_of(" \t\r");
        if (first == std::string::npos) {
            if (end >= source.size()) break;
            continue;
        }
        raw = raw.substr(first, raw.find_last_not_of(" \t\r") - first + 1);
        if (raw[0] == '#') {
            if (end >= source.size()) break;
            continue;
        }
        const std::string where = "Line " + std::to_string(lineNo) + ": ";

        std::vector<std::string> toks;
        for (std::size_t i = 0; i < raw.size();) {
            const std::size_t start = raw.find_first_not_of(" \t\r", i);
            if (start == std::string::npos) break;
            std::size_t stop = raw.find_first_of(" \t\r", start);
            if (stop == std::string::npos) stop = raw.size();
            toks.push_back(raw.substr(start, stop - start));
            i = stop;
        }

        ShapeType t = ShapeType::Circle;
        if (!parseSkinShapeType(toks[0], t)) return where + "unknown shape \"" + toks[0] + "\"";

        std::vector<std::pair<std::string, std::string>> kv;
        for (std::size_t i = 1; i < toks.size(); ++i) {
            const std::size_t eq = toks[i].find('=');
            if (eq == std::string::npos || eq == 0) {
                return where + "expected key=value near \"" + toks[i] + "\"";
            }
            kv.emplace_back(toks[i].substr(0, eq), toks[i].substr(eq + 1));
        }
        const auto find = [&kv](const char* key) -> const std::string* {
            for (const auto& pair : kv) {
                if (pair.first == key) return &pair.second;
            }
            return nullptr;
        };
        // parseFloat takes the leading numeric prefix, so "12px" is 12 and only
        // a value with no numeric prefix at all is an error.
        const auto numeric = [](const std::string& v, double& value) {
            char* stop = nullptr;
            value = std::strtod(v.c_str(), &stop);
            return stop != v.c_str() && std::isfinite(value);
        };

        static const char* const kNumKeys[] = {"x",  "y",  "rot", "sw",  "r",   "rx", "ry",
                                              "x2", "y2", "cx1", "cy1", "cx2", "cy2"};
        for (const char* key : kNumKeys) {
            const std::string* v = find(key);
            double parsedValue = 0;
            if (v && !numeric(*v, parsedValue)) {
                return where + "\"" + key + "\" must be a number";
            }
        }
        for (const char* key : {"fill", "stroke"}) {
            const std::string* v = find(key);
            if (v && !v->empty() && !isHexColor(*v)) {
                return where + "\"" + key + "\" must be a #rrggbb color";
            }
        }
        const auto num = [&](const char* key, double fallback) {
            const std::string* v = find(key);
            double value = 0;
            return v && numeric(*v, value) ? value : fallback;
        };

        Shape s;
        s.t = t;
        s.x = num("x", 0);
        s.y = num("y", 0);
        s.rot = num("rot", 0);
        const std::string* fill = find("fill");
        const std::string* stroke = find("stroke");
        s.fill = fill ? *fill : std::string();
        s.stroke = stroke ? *stroke : std::string();
        s.sw = num("sw", stroke && !stroke->empty() ? 2.0 : 0.0);
        // Mirror sanitizeSkinShape's outline defaults for the open shapes, so the
        // live preview matches what publishing would produce.
        if ((t == ShapeType::Line || t == ShapeType::Curve) && s.stroke.empty() && s.fill.empty()) {
            s.stroke = "#000000";
            if (s.sw == 0) s.sw = 2;
        }
        if (t == ShapeType::Circle) {
            s.r = num("r", 10);
        } else if (t == ShapeType::Ellipse || t == ShapeType::Rect) {
            s.rx = num("rx", 10);
            s.ry = num("ry", 6);
        } else if (t == ShapeType::Line) {
            s.x2 = num("x2", 0);
            s.y2 = num("y2", 0);
        } else if (t == ShapeType::Curve) {
            s.x2 = num("x2", 0);
            s.y2 = num("y2", 0);
            // Control points default onto their own endpoint, so a curve
            // written with only x/y/x2/y2 draws as a straight segment.
            s.cx1 = num("cx1", s.x);
            s.cy1 = num("cy1", s.y);
            s.cx2 = num("cx2", s.x2);
            s.cy2 = num("cy2", s.y2);
        } else if (t == ShapeType::Polygon) {
            const std::string* raw_points = find("points");
            std::vector<double> pts;
            if (raw_points && !raw_points->empty()) {
                std::size_t p = 0;
                while (p <= raw_points->size()) {
                    const std::size_t comma = std::min(raw_points->find(',', p), raw_points->size());
                    double value = 0;
                    if (!numeric(raw_points->substr(p, comma - p), value)) {
                        return where + "\"points\" must be a comma list of numbers";
                    }
                    pts.push_back(value);
                    if (comma >= raw_points->size()) break;
                    p = comma + 1;
                }
            }
            if (pts.size() < 6) {
                return where + "polygon needs points=x,y,x,y,x,y (\xe2\x89\xa5" "3 points)";
            }
            if (pts.size() > static_cast<std::size_t>(kMaxPolyPoints) * 2) {
                return where + "polygon allows at most " + std::to_string(kMaxPolyPoints) +
                       " points";
            }
            s.points = pts;
        }
        out.push_back(s);
        if (end >= source.size()) break;
    }
    if (out.empty()) return "Add at least one shape line.";
    if (out.size() > static_cast<std::size_t>(kMaxSkinShapes)) {
        return "Too many shapes (max " + std::to_string(kMaxSkinShapes) + ").";
    }
    return {};
}

bool Studio::dispatch(MenuContext& ctx, const Action& a) {
    // Every control takes the caret away from the name box, which is what
    // clicking off a DOM input does. The box itself never reaches here: it
    // answers for its own press in trackTextMouse.
    nameField.blur();

    switch (a.k) {
        case Act::Close:
            return false;
        case Act::Tab:
            tab = a.i == 0 ? Tab::Create : Tab::Browse;
            break;
        case Act::AddShape:
            // Silently capped: the button stays fully lit, exactly as it does
            // in the reference. A greyed-out control would be a new state.
            if (static_cast<int>(shapes.size()) >= kMaxSkinShapes) break;
            shapes.push_back(defaultShape(static_cast<ShapeType>(a.i)));
            selected = static_cast<int>(shapes.size()) - 1;
            break;
        case Act::SelectShape:
            selected = a.i;
            break;
        case Act::MoveShape: {
            const int j = a.i + a.dir;
            if (j < 0 || j >= static_cast<int>(shapes.size())) break;
            std::swap(shapes[static_cast<std::size_t>(a.i)], shapes[static_cast<std::size_t>(j)]);
            if (selected == a.i) selected = j;
            else if (selected == j) selected = a.i;
            break;
        }
        case Act::DelShape:
            if (a.i < 0 || a.i >= static_cast<int>(shapes.size())) break;
            shapes.erase(shapes.begin() + a.i);
            if (selected >= static_cast<int>(shapes.size())) {
                selected = static_cast<int>(shapes.size()) - 1;
            }
            break;
        case Act::Step:
            step(a.field, a.delta);
            break;
        case Act::Fill:
            if (Shape* s = selectedShape()) s->fill = a.color;
            break;
        case Act::Stroke:
            if (Shape* s = selectedShape()) {
                s->stroke = a.color;
                // An outline colour with no width would draw nothing, so
                // picking one turns the outline on.
                if (!a.color.empty() && s->sw <= 0) s->sw = 2;
            }
            break;
        case Act::AddVertex: {
            Shape* s = selectedShape();
            if (s && s->t == ShapeType::Polygon &&
                s->points.size() < static_cast<std::size_t>(kMaxPolyPoints) * 2) {
                const std::size_t n = s->points.size();
                const double ax = s->points[n - 2], ay = s->points[n - 1];
                const double bx = s->points[0], by = s->points[1];
                s->points.push_back(round1((ax + bx) / 2));
                s->points.push_back(round1((ay + by) / 2));
            }
            break;
        }
        case Act::DelVertex: {
            Shape* s = selectedShape();
            if (s && s->t == ShapeType::Polygon && s->points.size() > 6) {
                s->points.resize(s->points.size() - 2);
            }
            break;
        }
        case Act::TextMode:
            textMode = !textMode;
            textError.clear();
            if (textMode) enterTextMode();
            break;
        case Act::Publish: {
            // Sanitized locally FIRST, so a rejection is instant and reads the
            // same as the server's: the name box takes the caret when the name
            // is what is missing, and the reason goes to the transcript as a
            // line from "Skins". The server runs the same validator on arrival
            // -- this pass is courtesy, not the gate.
            skinName = sanitizeSkinName(skinName);
            const SkinCheck check = sanitizeSkin(skinName, shapes);
            if (!check.ok()) {
                if (skinName.empty()) nameField.focusAtEnd(skinName, ctx.timeSeconds);
                ctx.net.addLocalChat("Skins", check.error);
                break;
            }
            ctx.net.publishSkin(skinName, shapes);
            break;
        }
        case Act::Reset:
            shapes = starterShapes();
            selected = 0;
            textError.clear();
            if (textMode) enterTextMode();
            break;
        case Act::Equip:
            ctx.net.equipSkin(a.id);
            break;
        case Act::Unequip:
            ctx.net.equipSkin("");
            break;
        case Act::Delete:
            confirming = true;
            confirmId = a.id;
            confirmName = a.name;
            break;
        case Act::ConfirmDelete:
            // The card does NOT disappear here: the catalog is the server's,
            // and it drops out when the broadcast comes back. Erasing it
            // locally would hide a takedown the server refused.
            ctx.net.deleteSkin(confirmId);
            confirming = false;
            break;
        case Act::CancelDelete:
            confirming = false;
            break;
        case Act::None:
            break;
    }
    return true;
}

bool Studio::handleInput(MenuContext& ctx) {
    const Vec2 mouse = ctx.mouse();
    const Rect panel = layout.panel;

    // The name box and the text editor take the keyboard while they have the
    // caret, the way the DOM controls they replace do.
    const bool naming = tab == Tab::Create && !textMode && nameField.focused;
    const bool editing = tab == Tab::Create && textMode;
    if (naming || editing) ctx.wantsText = true;

    if (naming) {
        TextEditOptions typing;
        typing.maxBytes = kMaxSkinNameLen;
        // The server's own filter is ASCII word characters, spaces and dashes,
        // so anything wider would only be dropped by sanitizeSkinName.
        typing.asciiOnly = true;
        editText(ctx.window, skinName, nameField, ctx.timeSeconds, typing);
        if (ctx.window.keyPressed(Key::Enter) || ctx.window.keyPressed(Key::Escape)) {
            skinName = sanitizeSkinName(skinName);
            nameField.blur();
        }
    } else if (editing) {
        TextEditOptions typing;
        // A skin is 24 shapes of a couple of dozen characters each; the cap is
        // what stops a pasted file rather than what a real skin runs into.
        typing.maxBytes = 8192;
        typing.multiline = true;
        if (editText(ctx.window, textBuffer, textField, ctx.timeSeconds, typing)) {
            applyTextBuffer();
        }
        // The editor's own lines: 10 in from its left edge, the first centred
        // a line-height and a half down, which is what drawTextEditor paints.
        constexpr double fontPx = 12.5;
        constexpr double lineH = fontPx * 1.5;
        const Rect editor = layout.right;
        trackTextMouseMultiline(ctx.window, textField, editor, textBuffer, editor.x + 10.0,
                                editor.y + 10.0 + lineH * 0.5, lineH, fontPx, false,
                                ctx.timeSeconds);
        if (ctx.window.keyPressed(Key::Escape)) textMode = false;
    }
    if (tab == Tab::Create && !textMode) {
        // The name box takes its own press; the region list below would only
        // ever put the caret at one end of it.
        const Rect box = layout.footer;
        const Rect field{box.x, box.y, std::max(120.0, box.w - 106.0 - 84.0 - 24.0), box.h};
        trackTextMouse(ctx.window, nameField, field,
                       inputFieldRun(field, skinName, nameField), skinName, ctx.timeSeconds);
        if (insideInclusive(field, mouse)) ctx.window.setCursorShape(CursorShape::Text);
    }

    // Only inside the panel, and raw: one notch is the browser's 100px of
    // deltaY, which is what the other list panels consume too.
    const float wheel = ctx.wheel();
    if (wheel != 0 && insideInclusive(panel, mouse)) {
        Scroller& scroller = tab == Tab::Browse ? browseScroll : shapeScroll;
        scroller.offset =
            clamp(scroller.offset - static_cast<double>(wheel) * kWheelStep, 0.0,
                  scroller.maxOffset());
    }

    if (dragging) {
        if (ctx.window.mouseDown(MouseButton::Left)) applyDrag(mouse);
        else dragging = false;
    }

    // Hover tracks the last completed render's regions, which is where the
    // panel reads it from too.
    hovering = false;
    for (std::size_t i = regions.size(); i-- > 0;) {
        if (insideInclusive(regions[i].r, mouse)) {
            hover = regions[i].action;
            hovering = true;
            break;
        }
    }

    if (!ctx.pressed()) return true;

    // Regions win over the board: later-drawn regions are hit first.
    for (std::size_t i = regions.size(); i-- > 0;) {
        if (insideInclusive(regions[i].r, mouse)) return dispatch(ctx, regions[i].action);
    }

    // A press anywhere else has already blurred the box through
    // trackTextMouse; committing the name is what is left.
    if (!nameField.focused && !skinName.empty()) skinName = sanitizeSkinName(skinName);

    if (tab == Tab::Create && !textMode && !confirming && selectedShape()) {
        if (insideInclusive(layout.preview, mouse)) {
            double best = 9.0;
            dragHandle = HandleKind::Anchor;
            dragVertex = 0;
            for (const Handle& h : handles()) {
                const Vec2 p = toPx(h.lx, h.ly);
                const double d = distance(p, mouse);
                if (d < best) {
                    best = d;
                    dragHandle = h.kind;
                    dragVertex = h.vertex;
                }
            }
            dragging = true;
            applyDrag(mouse);
        }
    }
    return true;
}

} // namespace

double SkinsPanel::preferredWidth() { return 600.0; }
double SkinsPanel::preferredHeight() { return 580.0; }

void SkinsPanel::reset() {
    // Deliberately NOT a fresh studio: hiding the panel drops the two overlays
    // and the drag and keeps the authored shapes, so reopening it must not
    // throw away the skin being drawn.
    Studio& state = studio();
    state.nameField.blur();
    state.textField.dragging = false;
    state.dragging = false;
    state.confirming = false;
    state.regions.clear();
    state.hovering = false;
    if (state.textMode) state.enterTextMode();
}

bool SkinsPanel::render(MenuContext& ctx) {
    Studio& state = studio();
    // Rebound every frame rather than once: the studio is a file-local
    // singleton and the connection it reads is not, so a reconnect must not
    // leave it pointed at a dead one.
    state.net = &ctx.net;
    // From the shared anchor, so the card, its hit regions and the box
    // MenuSystem captures the mouse in are one rect.
    state.layout = layoutOf(ctx.bounds);
    // Input runs first and against the previous frame's regions: the panel's
    // handlers fire between renders, and every button, tab, swatch and stepper
    // acts on PRESS rather than release.
    if (!state.handleInput(ctx)) return false;
    state.draw(ctx);
    return true;
}

} // namespace flix
