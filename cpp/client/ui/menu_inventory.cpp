// The petal inventory.
//
// The screen players spend the most time in, so the muscle memory is real and
// the layout is held to the reference shot: a five-column grid of 60px cells
// grouped under a centred tier label with a rule running out to either edge of
// the grid, a Stack toggle and a search box above them, and a scroll thumb in
// the strip the grid leaves free on the right.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "client/ui/item_tile.h"
#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "client/ui/text.h"
#include "shared/game/config.h"

namespace flix {

using namespace flix::ui;

namespace {

/// The grid's margins. NOT equal: the right one also has to clear the scroll
/// thumb, so the five columns sit a touch left of the panel's own centre and
/// the thumb rides in the strip they leave. Centring the block on the panel
/// instead puts the last column under the thumb.
constexpr double kGridPadding = 26.0;
constexpr double kGridGutter = 42.0;
/// The air above the first heading and below the last row. Its own constant
/// rather than kGridPadding: the side margins are set by the scroll thumb and
/// this one is not.
constexpr double kGridTopPad = 12.0;
constexpr double kSectionGap = 14.0;
/// The band a tier heading occupies, and how far the tiles under it clear its
/// baseline. Both grew with the cells: a 60px tile needs more air above it
/// than a 56px one before the heading stops looking stuck to it.
constexpr double kLabelHeight = 26.0;
constexpr double kLabelToGrid = 12.0;
constexpr int kColumns = 5;

/// The controls row and everything above it. Fixed, because the grid below
/// scrolls and the header must not.
constexpr double kHeaderHeight = 137.0;
constexpr double kControlsY = 91.0;
constexpr double kControlsHeight = 33.0;
constexpr double kToggleBoxSize = 29.0;
constexpr double kToggleHitWidth = 95.0;

/// The controls row's own inset, wider than `kMenuPadding`: the toggle lines
/// up with the grid's left margin rather than with the title's, so the header
/// and the cells under it share one left edge.
constexpr double kControlsPad = kGridPadding;
constexpr double kControlsRightPad = 22.0;
/// The word beside the toggle, and the tier heading over each run of cells.
/// Both sized to the taller controls and larger cells around them.
constexpr double kToggleLabelSize = 16.0;
constexpr double kSectionLabelSize = 15.0;

/// The height the scroll range, the cull and the scrollbar are all measured
/// against. Deliberately the panel's own bottom padding rather than the
/// kMenuBorder the content is CLIPPED to: the wider strip is reserved, so the
/// last few units of the clip rect are never scrolled into.
constexpr double kViewBottomPad = 14.0;

/// One wheel notch is a raw ~100px deltaY in the browser; SDL reports ±1, so
/// the step is spelled out here rather than taken from Scroller's own 42.
constexpr double kWheelStep = 100.0;

/// The thumb, and how far in from the panel's right edge its RIGHT side sits.
/// There is no track: the panel paints the thumb alone, in the skin's accent
/// so it reads as part of the frame rather than as content.
constexpr double kThumbWidth = 8.0;
constexpr double kThumbInset = 14.0;
constexpr double kThumbMinHeight = 20.0;

/// Long enough that no player reaches it. The field scrolls its own head out
/// of view rather than refusing keys, exactly as the DOM input does.
constexpr std::size_t kSearchLimit = 128;

/// The tier heading's rules: their ink, and the air they leave either side of
/// the word. Round-capped, so the gap reads as a gap and not as a cut.
constexpr double kRuleWidth = 5.0;
constexpr double kRuleGap = 13.0;

/// Dwell before the hover tooltip appears, and how wide its body text wraps.
constexpr double kTooltipWrapWidth = 230.0;

/// One laid-out cell. In stacked mode a cell stands for a petal TYPE at the
/// best tier the account holds, rather than for one stack.
struct Cell {
    Rect rect;
    std::uint16_t petalIndex = kNoPetal;
    Rarity rarity = Rarity::Common;
    std::uint32_t count = 0;
};

struct Section {
    Rarity rarity = Rarity::Common;
    double labelY = 0;
};

struct Grid {
    std::vector<Cell> cells;
    std::vector<Section> sections;
    double contentHeight = 0;
};

/// The tooltip's dwell timer.
///
/// File-local rather than a panel field because InventoryPanel's members are
/// declared in menus.h, which this file does not own -- and there is exactly
/// one inventory panel in the client, so one timer is the right number.
TooltipDelay tooltipDelay;

std::string lowercased(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

/// Leading and trailing ASCII whitespace, dropped before matching. The browser
/// trims the field's value, so a stray space must not filter the grid empty.
std::string trimmed(const std::string& text) {
    const auto blank = [&text](std::size_t at) {
        return std::isspace(static_cast<unsigned char>(text[at])) != 0;
    };
    std::size_t begin = 0;
    while (begin < text.size() && blank(begin)) ++begin;
    std::size_t end = text.size();
    while (end > begin && blank(end - 1)) --end;
    return text.substr(begin, end - begin);
}

bool matchesSearch(std::uint16_t petalIndex, const std::string& needle) {
    if (needle.empty()) return true;
    const PetalConfig& config = content().petal(petalIndex);
    return lowercased(titleCase(config.name)).find(needle) != std::string::npos ||
           lowercased(config.id).find(needle) != std::string::npos;
}

/// Places `entries` as a centred five-column block starting at `y`, then
/// advances `y` past it. The last row is centred on its own, which is what
/// stops a trailing pair of petals from hugging the left edge.
void layoutRun(Grid& grid, const std::vector<Cell>& entries, double left, double innerWidth,
               double& y) {
    if (entries.empty()) return;
    const int rows = static_cast<int>((entries.size() + kColumns - 1) / kColumns);
    const int lastRowCount =
        static_cast<int>(entries.size()) - (rows - 1) * kColumns;
    const double fullWidth = kColumns * kCellSize + (kColumns - 1) * kCellGap;
    const double fullStart = left + (innerWidth - fullWidth) * 0.5;
    const double lastWidth = lastRowCount * kCellSize + (lastRowCount - 1) * kCellGap;
    const double lastStart = left + (innerWidth - lastWidth) * 0.5;

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const int row = static_cast<int>(i) / kColumns;
        const int column = static_cast<int>(i) % kColumns;
        const double startX = (row == rows - 1) ? lastStart : fullStart;
        Cell cell = entries[i];
        cell.rect = {startX + column * (kCellSize + kCellGap), y + row * (kCellSize + kCellGap),
                     kCellSize, kCellSize};
        grid.cells.push_back(cell);
    }
    y += rows * kCellSize + (rows - 1) * kCellGap;
}

/// Builds the grid in CONTENT space: y = 0 is the top of the scrolling area,
/// so the scroll offset is applied once, when drawing, and never leaks into
/// the layout or the hit test.
///
/// Petals only, and not because the panel is unfinished: the native server
/// drops consumable lines from mob_drops.json on load (server/systems/loot.cpp)
/// and Profile::Stack carries a petal index alone, so no health_potion or
/// speed_boost ever reaches a client to lay out. The browser's grid takes them
/// inline with the petals of the same tier -- its dict is keyed by item type,
/// not by petal -- so when the wire does carry them, they belong in these same
/// per-rarity runs rather than in a section of their own.
Grid buildGrid(const Profile& profile, double innerWidth, bool stacked,
               const std::string& needle) {
    Grid grid;
    double y = kGridTopPad;

    if (stacked) {
        // One cell per petal TYPE, shown at the best tier the account holds.
        // Deliberately no tier headings: the point of this mode is a compact
        // catalogue of what you own, not where it sits on the ladder. Petal
        // indices are assigned in sorted-id order, which is the canonical
        // ordering this mode is sorted by in the browser too.
        std::vector<Cell> entries;
        for (std::size_t i = 0; i < content().petalCount(); ++i) {
            const auto petalIndex = static_cast<std::uint16_t>(i);
            if (!matchesSearch(petalIndex, needle)) continue;
            for (int tier = kRarityCount - 1; tier >= 0; --tier) {
                const Rarity rarity = clampRarity(tier);
                const std::uint32_t count = profile.stackCount(petalIndex, rarity);
                if (count == 0) continue;
                entries.push_back({Rect{}, petalIndex, rarity, count});
                break;
            }
        }
        layoutRun(grid, entries, kGridPadding, innerWidth, y);
        y += kSectionGap;
        grid.contentHeight = y + kGridTopPad;
        return grid;
    }

    for (int tier = kRarityCount - 1; tier >= 0; --tier) {
        const Rarity rarity = clampRarity(tier);
        std::vector<Cell> entries;
        // Left in the profile's own order, which is the order the server sent
        // and therefore acquisition order. Sorting it here would arrange the
        // same account's grid differently from the browser's.
        for (const Profile::Stack& stack : profile.inventory) {
            if (stack.rarity != rarity || stack.count == 0) continue;
            if (!matchesSearch(stack.petalIndex, needle)) continue;
            entries.push_back({Rect{}, stack.petalIndex, stack.rarity, stack.count});
        }
        if (entries.empty()) continue;

        y += kLabelHeight;
        grid.sections.push_back({rarity, y - kLabelToGrid});
        layoutRun(grid, entries, kGridPadding, innerWidth, y);
        y += kSectionGap;
    }

    grid.contentHeight = y + kGridTopPad;
    return grid;
}

/// The scroll thumb, and only the thumb: a square-cornered quarter-black bar
/// inset from the panel's right edge.
///
/// Local rather than ui::scrollbar(), whose rounded, tracked, opaque bar is
/// what the other panels are drawn with. This one has no track at all.
void scrollThumb(Canvas& canvas, Rect panel, double contentTop, double visibleH,
                 double contentHeight, double scroll) {
    if (contentHeight <= visibleH || visibleH <= 0) return;
    const double travel = contentHeight - visibleH;
    const double thumbHeight = std::max(kThumbMinHeight, visibleH * (visibleH / contentHeight));
    const double thumbY = contentTop + clamp(scroll / travel, 0.0, 1.0) * (visibleH - thumbHeight);
    setFill(canvas, kInventorySkin.accent);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(panel.right() - kThumbInset - kThumbWidth),
                     static_cast<float>(thumbY), static_cast<float>(kThumbWidth),
                     static_cast<float>(thumbHeight), static_cast<float>(kThumbWidth * 0.5));
    canvas.fill();
}

/// A whole number with no separators, for the ALT variant of a stat row. The
/// browser prints the raw value there; commas would be a second abbreviation.
std::string exactNumber(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.0f", value);
    return buffer;
}

/// Name / tier / description / HP / damage, in the shared petal tooltip's
/// order. HP and damage carry the account's skill tiers, since that is the
/// number the player will actually see in the field.
std::vector<TooltipLine> petalTooltipLines(std::uint16_t petalIndex, Rarity rarity,
                                           const SkillSet& skills) {
    const PetalConfig& config = content().petal(petalIndex);
    const PetalStats stats = content().petalStats(petalIndex, rarity);
    const double health = std::round(stats.health * skills.effectScale(SkillId::PetalHealth));
    const double damage = std::round(stats.damage * skills.effectScale(SkillId::Damage));

    std::vector<TooltipLine> lines;
    lines.push_back({titleCase(config.name), 20.0});
    lines.push_back({rarityLabel(rarity), 14.0, rarityColor(rarity)});
    if (!config.description.empty()) {
        TooltipLine body{config.description, 12.0};
        body.gapBefore = 10.0;
        body.maxWidth = kTooltipWrapWidth;
        lines.push_back(body);
    }

    // Dimmed to gardn's 0xffffff90 rather than a pre-mixed grey, and spaced
    // off the description when there is one, off the tier row when there is not.
    TooltipLine hp{"HP: " + abbreviate(health), 12.0, kPaper,
                   config.description.empty() ? 10.0 : 4.0};
    hp.alpha = 0.56;
    hp.altText = "HP: " + exactNumber(health);
    lines.push_back(hp);

    TooltipLine hit{"Damage: " + abbreviate(damage), 12.0};
    hit.alpha = 0.56;
    hit.altText = "Damage: " + exactNumber(damage);
    lines.push_back(hit);
    return lines;
}

} // namespace

double InventoryPanel::preferredWidth() {
    // Five cells, their gaps, and the two unequal margins. The panel is sized
    // to the grid rather than the grid fitted to the panel, so a change to the
    // cell size moves the card's edge instead of crowding its contents.
    return kColumns * kCellSize + (kColumns - 1) * kCellGap + kGridPadding + kGridGutter;
}

void InventoryPanel::reset() {
    // Only the hover and the field's focus. Scroll position, search text and
    // Stack mode all survive a close and reopen in the browser -- the panel is
    // a window onto the account, not a wizard that starts over.
    searchFocused_ = false;
    tooltipDelay.reset();
}

bool InventoryPanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Profile& profile = ctx.net.profile();
    const Rect panel = ctx.bounds;
    const Vec2 mouse = ctx.mouse();
    // The grid's own span, and its centre. Not the panel's: the scroll thumb
    // takes a strip off the right, so the cells -- and the tier headings and
    // rules that introduce them -- sit slightly left of the card's middle.
    const double gridLeft = panel.x + kGridPadding;
    const double gridRight = panel.right() - kGridGutter;
    const double centre = (gridLeft + gridRight) * 0.5;

    panelCard(canvas, panel, kInventorySkin);
    panelTitle(canvas, panel, "Inventory", "Drag a petal to equip it");

    // --- header controls ---------------------------------------------------
    const Rect closeRect = closeButtonRect(panel);
    const Rect toggleBoxRect{panel.x + kControlsPad,
                             panel.y + kControlsY + (kControlsHeight - kToggleBoxSize) * 0.5,
                             kToggleBoxSize, kToggleBoxSize};
    const Rect toggleHit{panel.x + kControlsPad, panel.y + kControlsY, kToggleHitWidth,
                         kControlsHeight};
    const double searchX = panel.x + kControlsPad + kToggleHitWidth;
    const Rect searchRect{searchX, panel.y + kControlsY,
                          std::max(60.0, panel.right() - kControlsRightPad - searchX),
                          kControlsHeight};

    // Eased rather than snapped: the toggle is the only animated control on
    // the panel and a hard flip reads as a redraw glitch. The rate matches the
    // browser's per-frame quarter-of-the-gap at 60fps.
    const double target = stacked_ ? 1.0 : 0.0;
    stackLerp_ += (target - stackLerp_) * std::min(1.0, ctx.dt * 15.0);
    if (std::fabs(stackLerp_ - target) < 0.01) stackLerp_ = target;

    toggleBox(canvas, toggleBoxRect, stackLerp_);
    TextStyle toggleLabel;
    toggleLabel.size = kToggleLabelSize;
    toggleLabel.bold = true;
    toggleLabel.strokeWidth = 3.0;
    toggleLabel.roundJoin = true;
    text(canvas, "Stack", toggleBoxRect.right() + 5.0,
         toggleBoxRect.y + kToggleBoxSize * 0.5 + 1.0, toggleLabel);

    // No placeholder: the browser's field is a bare white box until it is typed
    // into, and a grey "Search" there reads as a value the filter is applying.
    inputField(canvas, searchRect, search_, "", searchFocused_, ctx.timeSeconds);
    panelClose(canvas, closeRect, closeRect.contains(mouse));

    // --- content -----------------------------------------------------------
    const Rect view{panel.x + kMenuBorder, panel.y + kHeaderHeight, panel.w - kMenuBorder * 2,
                    std::max(0.0, panel.bottom() - (panel.y + kHeaderHeight) - kMenuBorder)};
    const double visibleH =
        std::max(0.0, panel.bottom() - (panel.y + kHeaderHeight) - kViewBottomPad);
    const double innerWidth = panel.w - kGridPadding - kGridGutter;
    const Grid grid = buildGrid(profile, innerWidth, stacked_, trimmed(lowercased(search_)));

    scroll_.contentHeight = grid.contentHeight;
    scroll_.viewHeight = visibleH;
    // The browser's wheel listener is on the whole canvas, so the title and the
    // controls row scroll the grid too.
    if (panel.contains(mouse)) scroll_.offset -= ctx.wheel() * kWheelStep;
    scroll_.offset = clamp(scroll_.offset, 0.0, scroll_.maxOffset());

    const double originX = panel.x;
    const double originY = view.y - scroll_.offset;
    const double viewBottom = view.y + visibleH;

    // The browser rejects the header strip and then tests every laid-out cell,
    // including one the cull below will not paint, so the hit test is its own
    // pass rather than a side effect of drawing.
    int hovered = -1;
    Rect hoveredRect{};
    if (panel.contains(mouse) && mouse.y >= view.y) {
        for (std::size_t i = 0; i < grid.cells.size(); ++i) {
            const Cell& cell = grid.cells[i];
            const Rect rect{originX + cell.rect.x, originY + cell.rect.y, cell.rect.w,
                            cell.rect.h};
            if (!rect.contains(mouse)) continue;
            hovered = static_cast<int>(i);
            hoveredRect = rect;
            break;
        }
    }

    canvas.save();
    canvas.beginPath();
    canvas.rect(static_cast<float>(view.x), static_cast<float>(view.y), static_cast<float>(view.w),
                static_cast<float>(view.h));
    canvas.clip();

    for (const Section& section : grid.sections) {
        const double labelY = originY + section.labelY;
        if (labelY < view.y - kLabelHeight || labelY >= viewBottom) continue;

        TextStyle label;
        label.size = kSectionLabelSize;
        label.bold = true;
        label.align = Align::Centre;
        label.baseline = Baseline::Bottom;
        label.fill = rarityColor(section.rarity);
        label.strokeWidth = 3.0;
        const std::string caption = rarityLabel(section.rarity);

        // The heading's outline is 60% black, not solid -- solid reads a whole
        // weight heavier. TextStyle carries no stroke alpha, so the outlined
        // text goes down once under a global alpha and the fill is then laid
        // over it opaque, which leaves exactly the browser's two layers.
        canvas.setGlobalAlpha(0.6f);
        text(canvas, caption, centre, labelY, label);
        canvas.setGlobalAlpha(1.0f);
        label.strokeWidth = 0.0;
        text(canvas, caption, centre, labelY, label);

        // Rules out to the grid's own edges, stopping clear of the text. They
        // are what makes a tier read as a section rather than a stray heading,
        // and running them to the GRID rather than to the panel is what keeps
        // the heading centred over the cells it introduces.
        const double half = measure(caption, label.size, true) * 0.5;
        const double ruleY = labelY - 6.0;
        setStroke(canvas, kInventorySkin.accent);
        canvas.setLineWidth(static_cast<float>(kRuleWidth));
        canvas.setLineCap("round");
        if (centre - half - kRuleGap > gridLeft) {
            canvas.beginPath();
            canvas.moveTo(static_cast<float>(gridLeft), static_cast<float>(ruleY));
            canvas.lineTo(static_cast<float>(centre - half - kRuleGap), static_cast<float>(ruleY));
            canvas.moveTo(static_cast<float>(centre + half + kRuleGap), static_cast<float>(ruleY));
            canvas.lineTo(static_cast<float>(gridRight), static_cast<float>(ruleY));
            canvas.stroke();
        }
        canvas.setLineCap("butt");
    }

    for (std::size_t i = 0; i < grid.cells.size(); ++i) {
        const Cell& cell = grid.cells[i];
        const Rect rect{originX + cell.rect.x, originY + cell.rect.y, cell.rect.w, cell.rect.h};
        if (rect.bottom() <= view.y || rect.y >= viewBottom) continue;

        ItemTile tile;
        tile.petalIndex = cell.petalIndex;
        tile.rarity = cell.rarity;
        // The badge is how many the ACCOUNT owns; the cluster the tile draws is
        // how many one equipped slot spawns. Two different counts, deliberately.
        tile.badge = cell.count > 1 ? ("x" + std::to_string(cell.count)) : std::string();
        tile.hovered = hovered == static_cast<int>(i);
        tile.timeSeconds = ctx.timeSeconds;
        drawItemTile(canvas, ctx.sprites, rect, tile);
    }
    canvas.restore();

    scrollThumb(canvas, panel, view.y, visibleH, grid.contentHeight, scroll_.offset);

    // --- tooltip -----------------------------------------------------------
    // Anchored to the CELL rather than the cursor, and only after the dwell.
    // A press cancels it: what follows is a drag, and a box under the dragged
    // petal is the one thing in the way.
    const bool pointerDown = ctx.window.mouseDown(MouseButton::Left) || ctx.drag.active();
    if (tooltipDelay.update(hovered, ctx.timeSeconds, pointerDown) && hovered >= 0) {
        const Cell& cell = grid.cells[static_cast<std::size_t>(hovered)];
        const std::vector<TooltipLine> lines =
            petalTooltipLines(cell.petalIndex, cell.rarity, profile.skills);
        const bool alt = ctx.window.altHeld();
        const Vec2 size = measureTooltip(lines, 0.0, 0.0, alt);
        const Vec2 at = tooltipAnchor(hoveredRect, size, canvas.width(), canvas.height());
        paintTooltip(canvas, at.x, at.y, lines, 0.0, 0.0, alt);
    }

    // --- input -------------------------------------------------------------
    if (searchFocused_) {
        ctx.wantsText = true;
        for (const char c : ctx.window.typedText()) {
            if (static_cast<unsigned char>(c) >= 0x20 && search_.size() < kSearchLimit) {
                search_ += c;
            }
        }
        if (ctx.window.keyPressed(Key::Backspace) && !search_.empty()) {
            // Erase a whole UTF-8 sequence: dropping one byte of a multi-byte
            // character leaves a string that will not measure or draw.
            std::size_t at = search_.size() - 1;
            while (at > 0 && (static_cast<unsigned char>(search_[at]) & 0xC0) == 0x80) --at;
            search_.erase(at);
        }
        if (ctx.window.keyPressed(Key::Enter) || ctx.window.keyPressed(Key::Escape)) {
            searchFocused_ = false;
        }
    }

    if (ctx.pressed()) {
        // Close and Stack act on PRESS, as the browser's mousedown handler
        // does. Both also swallow the press rather than letting it reach the
        // field or a cell, and neither disturbs the search focus -- the
        // browser's preventDefault leaves the input focused where it was.
        if (closeRect.contains(mouse)) return false;
        if (toggleHit.contains(mouse)) {
            // Scroll survives the flip: it is re-clamped against the new
            // content height on the next frame, which is all the browser does.
            stacked_ = !stacked_;
            return true;
        }
        searchFocused_ = searchRect.contains(mouse);
        if (hovered >= 0 && !ctx.drag.active()) {
            const Cell& cell = grid.cells[static_cast<std::size_t>(hovered)];
            ctx.drag.source = DragState::Source::Inventory;
            ctx.drag.petalIndex = cell.petalIndex;
            ctx.drag.rarity = cell.rarity;
            ctx.drag.slot = -1;
        }
    }

    if (ctx.released() && hovered >= 0 && ctx.drag.source == DragState::Source::Inventory &&
        ctx.drag.petalIndex == grid.cells[static_cast<std::size_t>(hovered)].petalIndex &&
        ctx.drag.rarity == grid.cells[static_cast<std::size_t>(hovered)].rarity) {
        // Released on the cell it was picked up from: a click, not a drag.
        // Equip it into the first free slot, which is what a player wants nine
        // times out of ten and saves the trip to the bar.
        for (std::size_t slot = 0; slot < kLoadoutSlots; ++slot) {
            if (slot < profile.loadout.size() && !profile.loadout[slot].empty()) continue;
            ctx.net.setLoadoutSlot(static_cast<int>(slot), ctx.drag.petalIndex, ctx.drag.rarity);
            break;
        }
        ctx.drag.clear();
    }
    return true;
}

} // namespace flix
