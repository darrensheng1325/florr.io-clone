// The bestiary.
//
// A row per mob, a column per tier, and a cell filled in only once the account
// has actually killed one. The empty cells are the point: the grid is a
// checklist of the world, and the gaps are what is left to find.
//
// Every mob is generated at every tier, so no cell is ever "this mob does not
// exist here": an unkilled cell is a bare plate in the card's own frame colour,
// which is what makes a part-filled row read at a glance. Hovering one that HAS
// been killed opens a tooltip carrying the full drop table: the same
// upgrade/downgrade pipeline the server rolls, run forwards and displayed as
// per-rarity percentages.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "client/ui/item_tile.h"
#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "client/ui/text.h"
#include "shared/core/json.h"
#include "shared/game/config.h"

namespace flix {

using namespace flix::ui;

namespace {

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

constexpr double kPad = kMenuPadding;
constexpr double kTitleHeight = 30.0;
constexpr double kTitleGap = 26.0;
/// The cell the shared item chrome is written in, and the gap the reference
/// leaves between two of them. The bestiary is a grid of SEPARATE plates, not
/// the edge-to-edge sheet the panel used to draw: neighbours never share a
/// rule, and the 4-unit rim each plate carries reaches 2 units into the gap.
constexpr double kCell = kCellSize;
constexpr double kGridGap = kCellGap;
constexpr double kCellRadius = 5.0;
constexpr double kCellRim = 4.0;
/// Reserved for the scrollbar when the grid is centred; the bar itself is
/// drawn narrower than the reserve, hard against the right padding.
constexpr double kScrollbarWidth = 12.0;
constexpr double kThumbWidth = 8.0;

/// Where the scrollable content starts, measured down from the card's top.
constexpr double kContentTop = kPad + kTitleHeight + kTitleGap;

/// The card is a fixed size however big the window is. It is a corner overlay
/// now -- pinned under the top icon row rather than standing beside the icon
/// column -- so the height is literal too instead of two thirds of a viewport.
///
/// The width is DERIVED, not the reference shot's literal 603: that card holds
/// eight tiers and this game has nine (the reference has no unique), so a
/// literal width would clip the last column against the scrollbar. It is the
/// row, the padding either side, the scrollbar reserve, and the same ~4.5
/// units of slack the reference leaves at each end of a row.
constexpr double kGridSlack = 9.0;
constexpr double kCardHeight = 607.0;

/// The kill tally, top-right, tilted the way the reference shot draws it: it
/// reads as a sticker stuck on the plate rather than as part of the artwork,
/// and it deliberately overhangs the plate's right edge.
constexpr double kTallySize = 12.0;
constexpr double kTallyStroke = 3.0;
constexpr double kTallyTilt = 19.0 * kPi / 180.0;
/// Its CENTRE, in from the plate's top-right corner. Centred rather than
/// right-aligned, which is what keeps "x9" and "x2.1k" sitting on the same
/// spot instead of growing leftwards from one edge.
constexpr double kTallyInset = 8.5;

/// A mob is drawn at its own size, as the world draws it -- a baby ant is a
/// speck inside its plate and a hornet nearly fills one, which is the one fact
/// about a mob the grid can show without being read. Common's radius, never
/// the cell's own rarity: a row would otherwise swell left to right and the
/// grid would read as a size chart.
constexpr double kIconZoom = 1.15;
constexpr double kIconCap = 48.0;

/// Apex mobs exist, but nothing in the world spawns one and no item is ever
/// graded apex, so the tenth tier is left out of both the grid and the drop
/// table's columns.
constexpr int kTierColumns = kRarityCount - 1;
constexpr int kDropTiers = kRarityCount - 1;

constexpr double kGridWidth = kTierColumns * kCell + (kTierColumns - 1) * kGridGap;
constexpr double kCardWidth = kGridWidth + kPad * 2.0 + kScrollbarWidth + 4.0 + kGridSlack;

// Drop table, in the tooltip's own space.
constexpr double kDropsGapY = 6.0;
constexpr double kDropsHeaderH = 20.0;
constexpr double kColHeaderH = 16.0;
constexpr double kCardSize = 32.0;
constexpr double kCardLabelH = 14.0;
constexpr double kDropCellW = 56.0;
constexpr double kDropRowH = kCardSize + 4.0 + kCardLabelH;
constexpr double kDropRowGapY = 4.0;

struct Cell {
    /// x is a screen coordinate; y is measured from the card's top, as the
    /// reference stores it, because the scroll cull compares against it raw.
    Rect rect;
    std::uint16_t mobIndex = 0;
    Rarity rarity = Rarity::Common;
    std::uint32_t kills = 0;
};

/// Every string on this panel is stroked with a ROUND join: the reference sets
/// `ctx.lineJoin = 'round'` before the title and never puts it back, so the
/// whole frame inherits it. All of them are bold, too -- the tooltip rows are
/// the only regular-weight text the panel draws.
TextStyle galleryStyle(double size, std::uint32_t fill, double strokeWidth) {
    TextStyle style;
    style.size = size;
    style.bold = true;
    style.fill = fill;
    style.stroke = kInk;
    style.strokeWidth = strokeWidth;
    style.roundJoin = true;
    return style;
}

/// `abbreviateNumber` from the reference. Deliberately not the shared
/// `abbreviate()`: this one stops at B rather than growing a T tier, and its
/// thousands suffix is a lowercase k.
std::string abbreviateNumber(double value) {
    if (!std::isfinite(value)) return "\xE2\x88\x9E";
    char buffer[32];
    const char* suffix = "";
    if (value < 1000.0) {
        std::snprintf(buffer, sizeof buffer, "%.0f", std::floor(value + 0.5));
        return buffer;
    }
    if (value < 1e6) {
        std::snprintf(buffer, sizeof buffer, "%.1f", value / 1e3);
        suffix = "k";
    } else if (value < 1e9) {
        std::snprintf(buffer, sizeof buffer, "%.1f", value / 1e6);
        suffix = "M";
    } else {
        std::snprintf(buffer, sizeof buffer, "%.1f", value / 1e9);
        suffix = "B";
    }
    std::string out = buffer;
    // "1.0k" reads worse than "1k", and the trailing zero is never news.
    if (out.size() > 2 && out.compare(out.size() - 2, 2, ".0") == 0) out.erase(out.size() - 2);
    return out + suffix;
}

std::string formatFixed(double value, int decimals) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    return buffer;
}

// ---------------------------------------------------------------------------
// Drop tables
// ---------------------------------------------------------------------------

/// One authored row of mob_drops.json.
///
/// The server resolves the same file into rarity OFFSETS from the mob that
/// died (server/systems/loot.h), which is all a roll needs. The gallery needs
/// the authored ABSOLUTE rarity instead: every percentage in the tooltip is
/// the upgrade/downgrade pipeline run forwards from it.
struct DropDef {
    std::string type;        ///< "petal", "consumable", ...
    std::string itemType;
    Rarity rarity = Rarity::Common;
    double probability = 0;
    int maxQuantity = 1;
    /// kInvalidIndex for anything that is not a petal, which is exactly the
    /// set of drops the browser build has no icon for either.
    std::uint16_t petalIndex = kInvalidIndex;
};

/// The staged content directory.
///
/// Nothing a panel is handed carries the client's `--data` path, and
/// mob_drops.json is staged beside the fonts, so the typeface the client
/// actually loaded is the one handle the UI layer has on it. A client that
/// fell back to a system font finds no table and the tooltip degrades to its
/// text rows rather than failing.
std::string dataDirectory() {
    const std::string& font = Fonts::path();
    const std::size_t slash = font.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : font.substr(0, slash);
}

// ---------------------------------------------------------------------------
// Authored mob order and stats
// ---------------------------------------------------------------------------

/// The two things the ContentRegistry deliberately drops on the floor.
///
/// `order` is the bestiary's row order. ContentRegistry sorts its keys so an
/// index means the same thing on both ends of the wire (config.cpp
/// usableKeys), but the browser walks `Object.keys(MOB_CONFIG)` -- mobs.json's
/// own declaration order -- so the rows have to be recovered from the file.
/// Never resort the registry to fix this: those indices cross the wire.
///
/// `speed` is the authored figure, sign included. The loader keeps only the
/// magnitude because a negative speed is a direction the flee behaviour
/// already owns (moth ships -2.4), but the tooltip prints mobs.json verbatim.
struct MobRows {
    std::vector<std::uint16_t> order;
    std::vector<double> speed;
};

const MobRows& mobRows() {
    static const MobRows rows = [] {
        MobRows out;
        out.speed.reserve(content().mobCount());
        for (std::size_t i = 0; i < content().mobCount(); ++i) {
            out.speed.push_back(content().mob(static_cast<std::uint16_t>(i)).speed);
        }

        Json root;
        std::string error;
        // Json::keys() preserves insertion order -- that is how loadDropTables
        // reads the sibling file.
        if (Json::parseFile(dataDirectory() + "/mobs.json", root, error) && root.isObject()) {
            for (const std::string& mobId : root.keys()) {
                const std::uint16_t index = content().mobIndex(mobId);
                if (index >= content().mobCount()) continue;
                out.order.push_back(index);
                const Json& speed = root[mobId]["speed"];
                if (speed.isNumber()) out.speed[index] = speed.asDouble(out.speed[index]);
            }
        }
        // A partial read would silently hide mobs; fall back to the whole
        // registry rather than to a truncated bestiary.
        if (out.order.size() != content().mobCount()) {
            out.order.clear();
            for (std::size_t i = 0; i < content().mobCount(); ++i) {
                out.order.push_back(static_cast<std::uint16_t>(i));
            }
        }
        return out;
    }();
    return rows;
}

std::vector<std::vector<DropDef>> loadDropTables() {
    std::vector<std::vector<DropDef>> byMob(content().mobCount());
    Json root;
    std::string error;
    if (!Json::parseFile(dataDirectory() + "/mob_drops.json", root, error)) return byMob;
    if (!root.isObject()) return byMob;

    for (const std::string& mobId : root.keys()) {
        const std::uint16_t index = content().mobIndex(mobId);
        if (index >= byMob.size()) continue;
        const Json& drops = root[mobId]["drops"];
        if (!drops.isArray()) continue;
        for (const Json& entry : drops.items()) {
            DropDef def;
            def.type = entry["type"].asString("petal");
            def.itemType = entry["itemType"].asString();
            if (def.itemType.empty()) continue;
            def.rarity = parseRarity(entry["rarity"].asString("common"));
            def.probability = entry["probability"].asDouble(0.0);
            def.maxQuantity = entry["maxQuantity"].asInt(1);
            if (def.type == "petal") def.petalIndex = content().petalIndex(def.itemType);
            byMob[index].push_back(std::move(def));
        }
    }
    return byMob;
}

/// Parsed once, on the first tooltip. Content is immutable after load, so the
/// resolved indices cannot go stale.
const std::vector<DropDef>& mobDropTable(std::uint16_t mobIndex) {
    static const std::vector<std::vector<DropDef>> tables = loadDropTables();
    static const std::vector<DropDef> kNone;
    return mobIndex < tables.size() ? tables[mobIndex] : kNone;
}

/// Crafting success at a tier, as a PERCENTAGE. The shared
/// `craftSuccessChance` is the same ladder expressed as a fraction; the drop
/// arithmetic below mixes it with literal 100s and needs this scale.
double craftPercent(int tier) { return 64.0 / std::pow(2.0, tier); }

/// The tier's column in the drop table, or -1 for a rarity items never take.
int dropTier(Rarity r) {
    const int i = rarityIndex(r);
    return i < kDropTiers ? i : -1;
}

double upgradePercent(Rarity r) {
    const int i = dropTier(r);
    return (i < 0 || i >= kDropTiers - 1) ? 0.0 : craftPercent(i) / 3.0;
}

double downgradePercent(Rarity r) {
    const int i = dropTier(r);
    return i <= 0 ? 0.0 : 100.0 / (1.0 + craftPercent(i - 1));
}

Rarity tierAbove(Rarity r) {
    const int i = dropTier(r);
    return (i >= 0 && i < kDropTiers - 1) ? static_cast<Rarity>(i + 1) : r;
}

Rarity tierBelow(Rarity r) {
    const int i = dropTier(r);
    return i > 0 ? static_cast<Rarity>(i - 1) : r;
}

/// One (item, rarity) square of the table.
struct DropCell {
    /// The entry whose multiplier the square shows. Later branches landing on
    /// the same square only add their probability, exactly as the reference's
    /// object spread keeps the first entry's fields.
    const DropDef* drop = nullptr;
    double probability = 0;   ///< percent, 0..100
};

struct DropRow {
    const DropDef* meta = nullptr;   ///< row identity: (type, itemType)
    std::array<DropCell, kDropTiers> cells{};
};

/// Mirrors the server drop pipeline so the tooltip shows real rates.
///
/// Common mobs roll each table entry independently at its listed probability;
/// uncommon mobs drop the whole table guaranteed; above uncommon the entries
/// are weights normalised to one drop per kill, landing a tier below the mob
/// 90% of the time. Every outcome then branches into the pickup
/// downgrade/same/upgrade split and is clamped up to the mob's rarity floor.
std::vector<DropRow> computeMobDrops(std::uint16_t mobIndex, Rarity mobRarity,
                                     std::array<bool, kDropTiers>& usedTiers) {
    std::vector<DropRow> rows;
    const std::vector<DropDef>& table = mobDropTable(mobIndex);
    if (table.empty()) return rows;

    const int tier = rarityIndex(mobRarity);
    const double ultraMultiplier = mobRarity == Rarity::Ultra ? 20.0 : 1.0;
    // Server-side floor: a rare mob never drops below one tier under it, an
    // epic or better below two.
    const int minTier = tier >= 3 ? tier - 2 : (tier == 2 ? 1 : 0);

    const auto push = [&](const DropDef& drop, Rarity rarity, double probability) {
        if (probability <= 0) return;
        int column = dropTier(rarity);
        if (column < 0) return;
        if (column < minTier) column = minTier;

        DropRow* row = nullptr;
        for (DropRow& candidate : rows) {
            if (candidate.meta->type == drop.type && candidate.meta->itemType == drop.itemType) {
                row = &candidate;
                break;
            }
        }
        if (row == nullptr) {
            rows.emplace_back();
            rows.back().meta = &drop;
            row = &rows.back();
        }
        DropCell& cell = row->cells[static_cast<std::size_t>(column)];
        if (cell.drop == nullptr) cell.drop = &drop;
        cell.probability += probability;
        usedTiers[static_cast<std::size_t>(column)] = true;
    };

    const auto outcomes = [&](Rarity base, double baseProb, const DropDef& drop) {
        const double up = std::min(100.0, upgradePercent(base) * ultraMultiplier);
        const double down = downgradePercent(base);
        const double same = std::max(0.0, 100.0 - up - down);
        push(drop, tierBelow(base), baseProb * down);
        push(drop, base, baseProb * same);
        push(drop, tierAbove(base), baseProb * up);
    };

    double totalWeight = 0;
    for (const DropDef& drop : table) totalWeight += drop.probability;

    for (const DropDef& drop : table) {
        if (tier == rarityIndex(Rarity::Uncommon)) {
            outcomes(drop.rarity, 1.0, drop);
        } else if (tier > rarityIndex(Rarity::Uncommon) && totalWeight > 0) {
            const double share = drop.probability / totalWeight;
            const Rarity lower = clampRarity(std::min(tier - 1, kDropTiers - 1));
            outcomes(lower, share * 0.9, drop);
            outcomes(drop.rarity, share * 0.1, drop);
        } else {
            outcomes(drop.rarity, drop.probability, drop);
        }
    }
    return rows;
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------

/// The card: a border-coloured rounded rect with a SHARP-cornered body inset
/// by the border width. The shared panelCard() rounds the body by radius - 2;
/// the reference passes a literal 0, and at a 3px outer radius the difference
/// is visible on all four corners.
void galleryCard(Canvas& canvas, Rect panel) {
    setFill(canvas, kGallerySkin.border);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(panel.x), static_cast<float>(panel.y),
                     static_cast<float>(panel.w), static_cast<float>(panel.h),
                     static_cast<float>(kMenuRadius));
    canvas.fill();

    setFill(canvas, kGallerySkin.fill);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(panel.x + kMenuBorder),
                     static_cast<float>(panel.y + kMenuBorder),
                     static_cast<float>(panel.w - kMenuBorder * 2),
                     static_cast<float>(panel.h - kMenuBorder * 2), 0.0f);
    canvas.fill();
}

/// The gallery's own scrollbar: a bare thumb in the card's own frame colour,
/// with NO groove behind it. The reference draws only the thumb, so an
/// unscrolled panel shows one short bar in the top corner rather than a track
/// running the height of the card.
void galleryScrollbar(Canvas& canvas, Rect track, double contentHeight, double scroll,
                      double maxScroll) {
    const double thumbHeight = std::max(20.0, track.h * (track.h / contentHeight));
    const double thumbY = track.y + (scroll / maxScroll) * (track.h - thumbHeight);
    setFill(canvas, kGallerySkin.accent);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(track.x), static_cast<float>(thumbY),
                     static_cast<float>(track.w), static_cast<float>(thumbHeight), 4.0f);
    canvas.fill();
}

void drawEmptyDropCell(Canvas& canvas, double cellX, double rowY) {
    const double cardX = cellX + (kDropCellW - kCardSize) * 0.5;
    setFill(canvas, kPaper, 0.05);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(cardX), static_cast<float>(rowY),
                     static_cast<float>(kCardSize), static_cast<float>(kCardSize), 4.0f);
    canvas.fill();

    setStroke(canvas, kPaper, 0.1);
    canvas.setLineWidth(1.0f);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(cardX + 0.5), static_cast<float>(rowY + 0.5),
                     static_cast<float>(kCardSize - 1.0), static_cast<float>(kCardSize - 1.0),
                     4.0f);
    canvas.stroke();
}

void drawDropCard(Canvas& canvas, const SpriteCache& sprites, double cellX, double rowY,
                  Rarity rarity, const DropCell& cell, double timeSeconds) {
    const double cardX = cellX + (kDropCellW - kCardSize) * 0.5;

    // Consumables draw as a bare coloured plate: only petals are compiled as
    // sprites, so a potion has no artwork to put on one.
    ItemTile tile;
    tile.petalIndex = cell.drop->petalIndex;
    tile.rarity = rarity;
    // The chance printed under the card is this card's caption; a name inside
    // it as well would give a 34px square two labels.
    tile.showName = false;
    if (cell.drop->maxQuantity > 1) tile.badge = "x" + std::to_string(cell.drop->maxQuantity);
    tile.timeSeconds = timeSeconds;
    drawItemTile(canvas, sprites, {cardX, rowY, kCardSize, kCardSize}, tile);

    // Aggregated branches can push the expected count past one drop per kill;
    // the display caps at 100% rather than printing a number no roll can mean.
    const std::string label = cell.probability < 0.01
                                  ? std::string("<0.01%")
                                  : formatFixed(std::min(100.0, cell.probability), 2) + "%";
    TextStyle probability = galleryStyle(10.0, kPaper, 2.0);
    probability.align = Align::Centre;
    probability.baseline = Baseline::Top;
    text(canvas, label, cellX + kDropCellW * 0.5, rowY + kCardSize + 4.0, probability);
}

/// Scrollbar drag state.
///
/// It belongs on GalleryPanel, but menus.h declares that class and is not this
/// change's to edit. There is exactly one gallery panel in the process, so a
/// file-local record behaves identically until the fields can move onto it.
struct ScrollDrag {
    bool active = false;
    double startY = 0;
    double startOffset = 0;
};

ScrollDrag& scrollDrag() {
    static ScrollDrag drag;
    return drag;
}

} // namespace

double GalleryPanel::preferredWidth() { return kCardWidth; }

double GalleryPanel::preferredHeight() { return kCardHeight; }

void GalleryPanel::reset() {
    // Deliberately empty. The browser panel's scroll offset is a member that
    // toggling never touches, so reopening the bestiary returns the player to
    // the row they were reading rather than to the bees.
}

bool GalleryPanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Profile& profile = ctx.net.profile();
    const Rect panel = ctx.bounds;
    const Vec2 mouse = ctx.mouse();
    // The reference's listeners live on the panel's own canvas, so hover,
    // wheel and press only exist while the pointer is over the card.
    const bool overPanel = panel.contains(mouse);

    galleryCard(canvas, panel);

    TextStyle title = galleryStyle(24.0, kPaper, 4.0);
    title.align = Align::Centre;
    text(canvas, "Mob Gallery", panel.x + panel.w * 0.5, panel.y + kPad + kTitleHeight * 0.5,
         title);

    const Rect closeRect = closeButtonRect(panel);
    panelClose(canvas, closeRect, closeRect.contains(mouse));

    // --- layout ------------------------------------------------------------
    const double contentTop = panel.y + kContentTop;
    const Rect view{panel.x + kPad, contentTop, panel.w - kPad * 2 - kScrollbarWidth - 4.0,
                    std::max(0.0, panel.h - kContentTop - kPad)};

    const double startX = view.x + std::max(0.0, (view.w - kGridWidth) * 0.5);

    std::vector<Cell> cells;
    cells.reserve(content().mobCount() * kTierColumns);
    double y = kContentTop;
    // Declaration order, not index order: the registry's indices are sorted.
    for (const std::uint16_t mobIndex : mobRows().order) {
        for (int tier = 0; tier < kTierColumns; ++tier) {
            const Rarity rarity = clampRarity(tier);
            Cell cell;
            cell.rect = {startX + tier * (kCell + kGridGap), y, kCell, kCell};
            cell.mobIndex = mobIndex;
            cell.rarity = rarity;
            cell.kills = profile.killCount(mobIndex, rarity);
            cells.push_back(cell);
        }
        y += kCell + kGridGap;
    }
    // The trailing row gap counts: the reference measures from the first row's
    // top to the cursor AFTER the last row's gap, which is one gap of slack at
    // the bottom of the scroll range.
    const double contentHeight = y - kContentTop;

    scroll_.contentHeight = contentHeight;
    scroll_.viewHeight = view.h;
    const double maxScroll = scroll_.maxOffset();

    // --- input -------------------------------------------------------------
    // The close button answers the PRESS, before anything else can claim it.
    const bool closing = ctx.pressed() && closeRect.contains(mouse);

    // Narrower than the reserve the grid was centred against, and hard against
    // the right padding: the bar the reference draws is 8 wide, not 12.
    const Rect trackRect{panel.right() - kPad - kThumbWidth, view.y, kThumbWidth, view.h};
    ScrollDrag& drag = scrollDrag();
    if (!ctx.window.mouseDown(MouseButton::Left)) drag.active = false;
    if (!closing && ctx.pressed() && maxScroll > 0 && trackRect.contains(mouse)) {
        drag.active = true;
        drag.startY = mouse.y;
        drag.startOffset = scroll_.offset;
    }
    if (drag.active) {
        // Track travel maps to content travel, so grabbing anywhere in the
        // groove drags at the same rate the thumb moves.
        scroll_.offset = drag.startOffset +
                         (mouse.y - drag.startY) * (maxScroll / std::max(1.0, view.h));
    } else if (overPanel) {
        // The wheel delta the browser reports is about 100px a notch, and its
        // sign is inverted against SDL's.
        scroll_.offset -= ctx.wheel() * 100.0;
    }
    scroll_.offset = clamp(scroll_.offset, 0.0, maxScroll);

    // Hit-tested over EVERY cell, not just the drawn ones, and gated on the
    // content band rather than the grid: that is what the reference does, and
    // it is why a cell in the never-drawn bottom strip can still be hovered.
    int hovered = -1;
    if (overPanel && !drag.active && mouse.y >= view.y && mouse.y <= view.bottom()) {
        const double yInGrid = mouse.y - panel.y + scroll_.offset;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const Rect& r = cells[i].rect;
            if (mouse.x >= r.x && mouse.x <= r.right() && yInGrid >= r.y &&
                yInGrid <= r.bottom()) {
                hovered = static_cast<int>(i);
                break;
            }
        }
    }

    // --- cells -------------------------------------------------------------
    canvas.save();
    canvas.beginPath();
    canvas.rect(static_cast<float>(view.x), static_cast<float>(view.y), static_cast<float>(view.w),
                static_cast<float>(view.h));
    canvas.clip();

    for (std::size_t i = 0; i < cells.size(); ++i) {
        const Cell& cell = cells[i];
        // Culled in the CARD's space, which is the space cell.rect.y is
        // measured in: a cell is on screen when it lands inside the band the
        // content area occupies, kContentTop down from the card's top. The
        // browser build compared the same y against a band starting at zero,
        // which threw away the bottom row of every card it drew -- visible as
        // a strip of bare panel under the grid, and hoverable, since the hit
        // test never had the bug.
        const double bandTop = scroll_.offset + kContentTop;
        if (cell.rect.bottom() <= bandTop || cell.rect.y >= bandTop + view.h) continue;
        const Rect rect{cell.rect.x, panel.y + cell.rect.y - scroll_.offset, cell.rect.w,
                        cell.rect.h};

        const bool known = cell.kills > 0;

        setFill(canvas, known ? rarityColor(cell.rarity) : kGallerySkin.accent);
        canvas.beginPath();
        canvas.roundRect(static_cast<float>(rect.x), static_cast<float>(rect.y),
                         static_cast<float>(rect.w), static_cast<float>(rect.h),
                         static_cast<float>(kCellRadius));
        canvas.fill();
        // An unkilled cell is a bare plate in the card's own frame colour: a
        // hole punched in the sheet rather than a tile with something in it.
        // Only a killed one gets the rim -- the rarity at the 0.8 value every
        // item plate in the game is outlined with -- so the grid reads as a
        // checklist from across the panel, before a single tally is legible.
        if (known) {
            setStroke(canvas, shade(rarityColor(cell.rarity), kItemTilePlateShade));
            canvas.setLineWidth(static_cast<float>(kCellRim));
            canvas.stroke();
        }

        if (known) {
            // Common's radius, whatever tier this cell is -- see kIconZoom.
            const double diameter =
                std::min(kIconCap, content().mobStats(cell.mobIndex, Rarity::Common).radius * 2.0 *
                                       kIconZoom);
            ctx.sprites.drawMob(canvas, cell.mobIndex, rect.x + rect.w * 0.5,
                                rect.y + rect.h * 0.5, diameter, 0.0, ctx.timeSeconds);

            // Abbreviated, unlike the drop table's own counts: five figures of
            // ant kills laid across a 60-unit plate is a smear, and a bestiary
            // tally is read for its order of magnitude.
            const std::string tally = "x" + abbreviateNumber(cell.kills);
            TextStyle count = galleryStyle(kTallySize, kPaper, kTallyStroke);
            count.align = Align::Centre;
            count.baseline = Baseline::Middle;
            canvas.save();
            canvas.translate(static_cast<float>(rect.right() - kTallyInset),
                             static_cast<float>(rect.y + kTallyInset));
            canvas.rotate(static_cast<float>(kTallyTilt));
            text(canvas, tally, 0.0, 0.0, count);
            canvas.restore();
        }

        if (hovered == static_cast<int>(i)) {
            setStroke(canvas, kPaper);
            canvas.setLineWidth(2.0f);
            canvas.beginPath();
            canvas.roundRect(static_cast<float>(rect.x), static_cast<float>(rect.y),
                             static_cast<float>(rect.w), static_cast<float>(rect.h),
                             static_cast<float>(kCellRadius));
            canvas.stroke();
        }
    }
    canvas.restore();

    if (maxScroll > 0) {
        galleryScrollbar(canvas, trackRect, contentHeight, scroll_.offset, maxScroll);
    }

    // --- tooltip -----------------------------------------------------------
    // Only over a mob the account has actually killed. A locked cell says
    // nothing, which is what keeps the grid a checklist.
    if (hovered >= 0 && cells[static_cast<std::size_t>(hovered)].kills > 0) {
        const Cell& cell = cells[static_cast<std::size_t>(hovered)];
        const MobConfig& config = content().mob(cell.mobIndex);
        const MobStats stats = content().mobStats(cell.mobIndex, cell.rarity);

        std::vector<TooltipLine> lines;
        lines.emplace_back(config.name, 20.0, kPaper, 0.0);
        lines.emplace_back(rarityLabel(cell.rarity), 14.0, rarityColor(cell.rarity), 0.0);
        if (!config.description.empty()) {
            TooltipLine body{config.description, 12.0, kPaper, 10.0};
            body.maxWidth = 280.0;
            lines.push_back(std::move(body));
        }
        const double statGap = config.description.empty() ? 10.0 : 4.0;
        const auto stat = [&](std::string body, double gapBefore) {
            TooltipLine line{std::move(body), 12.0, kPaper, gapBefore};
            line.alpha = 0.56;
            lines.push_back(std::move(line));
        };
        stat("HP: " + abbreviateNumber(stats.health), statGap);
        stat("Damage: " + abbreviateNumber(stats.damage), 0.0);
        // The raw config figure, not the units-per-second the simulation runs
        // on, and the AUTHORED sign with it: the browser tooltip reads straight
        // off mobs.json, where the moth's -2.4 is what a player sees.
        stat("Speed: " + formatFixed(mobRows().speed[cell.mobIndex], 1), 0.0);
        stat("XP: " + abbreviateNumber(stats.xp), 0.0);

        std::array<bool, kDropTiers> usedTiers{};
        const std::vector<DropRow> rows = computeMobDrops(cell.mobIndex, cell.rarity, usedTiers);
        std::vector<int> columns;
        for (int i = 0; i < kDropTiers; ++i) {
            if (usedTiers[static_cast<std::size_t>(i)]) columns.push_back(i);
        }
        const bool hasDrops = !rows.empty() && !columns.empty();

        const double tableWidth = static_cast<double>(columns.size()) * kDropCellW;
        const double dropsHeight =
            hasDrops ? kDropsGapY + kDropsHeaderH + kColHeaderH +
                           static_cast<double>(rows.size()) * kDropRowH +
                           std::max<double>(0.0, static_cast<double>(rows.size()) - 1.0) *
                               kDropRowGapY
                     : 0.0;
        const double minWidth = hasDrops ? tableWidth : 0.0;

        // Anchored to the CELL and clamped to the SCREEN, never to the cursor:
        // a box that follows the pointer slides over the thing being read.
        //
        // The screen, not the card, because the card is a corner overlay now:
        // a drop table is wider than half of it, so clamping to the card would
        // fold the box back over the very cell it describes. Overhanging the
        // right edge onto the world is what the reference does.
        const Vec2 size = measureTooltip(lines, minWidth, dropsHeight);
        double tx = cell.rect.right() + 8.0;
        double ty = panel.y + cell.rect.y - scroll_.offset;
        if (tx + size.x > canvas.width() - 4.0) tx = cell.rect.x - size.x - 8.0;
        if (ty + size.y > canvas.height() - 4.0) ty = canvas.height() - size.y - 4.0;
        if (tx < 4.0) tx = 4.0;
        if (ty < contentTop) ty = contentTop;

        const Rect box = paintTooltip(canvas, tx, ty, lines, minWidth, dropsHeight);

        if (hasDrops) {
            double cy = box.y + kDropsGapY;
            TextStyle heading = galleryStyle(12.0, 0xFFD700u, -1.0);
            heading.baseline = Baseline::Top;
            text(canvas, "Drops:", box.x, cy, heading);
            cy += kDropsHeaderH;

            const double tableX = tx + (size.x - tableWidth) * 0.5;
            TextStyle header = galleryStyle(10.0, kPaper, 2.0);
            header.align = Align::Centre;
            for (std::size_t i = 0; i < columns.size(); ++i) {
                const Rarity rarity = clampRarity(columns[i]);
                header.fill = rarityColor(rarity);
                text(canvas, rarityLabel(rarity),
                     tableX + static_cast<double>(i) * kDropCellW + kDropCellW * 0.5,
                     cy + kColHeaderH * 0.5, header);
            }
            cy += kColHeaderH;

            for (std::size_t r = 0; r < rows.size(); ++r) {
                const double rowY = cy + static_cast<double>(r) * (kDropRowH + kDropRowGapY);
                for (std::size_t i = 0; i < columns.size(); ++i) {
                    const double cellX = tableX + static_cast<double>(i) * kDropCellW;
                    const DropCell& square = rows[r].cells[static_cast<std::size_t>(columns[i])];
                    if (square.drop != nullptr) {
                        drawDropCard(canvas, ctx.sprites, cellX, rowY, clampRarity(columns[i]),
                                     square, ctx.timeSeconds);
                    } else {
                        drawEmptyDropCell(canvas, cellX, rowY);
                    }
                }
            }
        }
    }

    return !closing;
}

} // namespace flix
