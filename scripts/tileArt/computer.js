'use strict';
/**
 * Skin 2, computer: a circuit-board chip with gold pins, liquid coolant, and
 * traces / solder pads / LEDs on the board. Palette from the tile skin
 * contract.
 *
 * Seamless tiling: every trace that leaves the tile does so at one of five
 * fixed offsets (the pin pitch) and the layout is the same on all four sides,
 * so a trace exiting at x=300 meets the neighbour's trace entering at x=0
 * at the same y. Everything else stays inside a margin.
 */

const TILE = 300;
const GOLD = '#d4a017';
const GOLD_DIM = '#a37a12';
const GOLD_LIT = '#f2c94c';
const PIN_OFFSETS = [105, 127.5, 150, 172.5, 195];   // pin centres along each side
const CHIP = 96;     // chip package half-size box edge is at 150 +/- CHIP/2 + ...

module.exports = {
    name: 'computer',

    wall: {
        fill: '#1f2a35',        // the board substrate
        band: '#0f151b',        // the chip's shaded edge
        line: '#06090c',
        lineWidth: 3,
        // A row of pin sockets: square notches at the pin pitch.
        profile: {
            kind: 'custom',
            build(scale, { withCornerRuns, CORNER_RUN, TILE: T }) {
                const shallow = 20 * scale;
                const deep = 29 * scale;
                const pins = 8;
                const span = T - 2 * CORNER_RUN;
                const pitch = span / pins;
                const inner = [];
                for (let i = 0; i < pins; i++) {
                    const x0 = CORNER_RUN + pitch * i;
                    inner.push({ to: [x0 + pitch * 0.28, shallow] });
                    inner.push({ to: [x0 + pitch * 0.28, deep] });
                    inner.push({ to: [x0 + pitch * 0.72, deep] });
                    inner.push({ to: [x0 + pitch * 0.72, shallow] });
                }
                inner.push({ to: [T - CORNER_RUN, shallow] });
                return withCornerRuns(shallow, inner);
            },
        },
        highlight: { color: '#3d5266', width: 2, scale: 0.5 },
        motif(g) {
            const half = CHIP / 2;
            const cx = TILE / 2;
            const cy = TILE / 2;
            // Traces from the edges in to the pins, on all four sides.
            const trace = { stroke: '#6f5a1c', 'stroke-width': 4, 'stroke-linecap': 'butt' };
            for (const o of PIN_OFFSETS) {
                g.line(0, o, cx - half - 10, o, trace);
                g.line(cx + half + 10, o, TILE, o, trace);
                g.line(o, 0, o, cy - half - 10, trace);
                g.line(o, cy + half + 10, o, TILE, trace);
            }
            // Vias where the traces cross a ring around the chip.
            for (const o of PIN_OFFSETS) {
                for (const [x, y] of [[40, o], [TILE - 40, o], [o, 40], [o, TILE - 40]]) {
                    g.circle(x, y, 5.5, { fill: GOLD_DIM });
                    g.circle(x, y, 2.2, { fill: '#1f2a35' });
                }
            }
            // Small surface-mount parts in the four corners.
            const parts = [[45, 45, 0], [255, 45, 1], [45, 255, 1], [255, 255, 0]];
            for (const [x, y, vertical] of parts) {
                const w = vertical ? 12 : 30;
                const h = vertical ? 30 : 12;
                g.rect(x - w / 2, y - h / 2, w, h, { fill: '#2c3a48', rx: 2 });
                if (vertical) {
                    g.rect(x - w / 2, y - h / 2, w, 6, { fill: '#b8c0c8' });
                    g.rect(x - w / 2, y + h / 2 - 6, w, 6, { fill: '#b8c0c8' });
                } else {
                    g.rect(x - w / 2, y - h / 2, 6, h, { fill: '#b8c0c8' });
                    g.rect(x + w / 2 - 6, y - h / 2, 6, h, { fill: '#b8c0c8' });
                }
            }
            // The chip: pins first, then the package on top of them.
            for (const o of PIN_OFFSETS) {
                g.rect(cx - half - 14, o - 5, 16, 10, { fill: GOLD, rx: 2 });
                g.rect(cx + half - 2, o - 5, 16, 10, { fill: GOLD, rx: 2 });
                g.rect(o - 5, cy - half - 14, 10, 16, { fill: GOLD, rx: 2 });
                g.rect(o - 5, cy + half - 2, 10, 16, { fill: GOLD, rx: 2 });
                g.rect(cx - half - 14, o - 3, 16, 3, { fill: GOLD_LIT, 'fill-opacity': 0.8 });
                g.rect(cx + half - 2, o - 3, 16, 3, { fill: GOLD_LIT, 'fill-opacity': 0.8 });
            }
            g.rect(cx - half, cy - half, CHIP, CHIP, { fill: '#0c1116', rx: 6 });
            g.rect(cx - half + 6, cy - half + 6, CHIP - 12, CHIP - 12, { fill: '#141c24', rx: 4 });
            // The die window and the pin-one dot.
            g.rect(cx - 22, cy - 22, 44, 44, { fill: '#1b2b3a' });
            for (let i = 0; i < 4; i++) {
                g.line(cx - 16, cy - 14 + i * 9, cx + 16, cy - 14 + i * 9, { stroke: '#2e4a5e', 'stroke-width': 2 });
            }
            g.circle(cx - half + 14, cy - half + 14, 4, { fill: '#8a949c' });
        },
    },

    water: {
        fill: '#00d0ff',
        band: '#0088aa',        // the deeper channel wall
        foam: '#ccf6ff',
        foamWidth: 2.5,
        // A fast, shallow ripple.
        profile: { kind: 'wave', depth: 15, periods: 4, amplitude: 2 },
        layers: [{ scale: 1, fill: '#0088aa' }, { scale: 0.5, fill: '#00a8d4' }],
        motif(g) {
            // Flow streaks along the channel.
            for (let i = 0; i < 5; i++) {
                const y = 32 + i * 52 + g.range(-8, 8);
                const x0 = g.range(20, 60);
                const x1 = g.range(230, 280);
                g.line(x0, y, x1, y, { stroke: '#7de8ff', 'stroke-width': 4, 'stroke-linecap': 'round' });
                g.line(x0 + 20, y + 9, x1 - 30, y + 9, { stroke: '#33dcff', 'stroke-width': 2.5, 'stroke-linecap': 'round' });
            }
            // Bubbles with a bright rim.
            for (const [x, y] of g.scatter(8, { margin: 24, minDist: 34 })) {
                const r = g.range(4, 9);
                g.circle(x, y, r, { fill: '#33dcff' });
                g.circle(x, y, r, { fill: 'none', stroke: '#ccf6ff', 'stroke-width': 2 });
                g.circle(x - r * 0.35, y - r * 0.35, r * 0.28, { fill: '#ffffff' });
            }
        },
    },

    floors: [
        // 0: traces -- gold runs with right-angle bends and a via at each end
        g => {
            const width = 5;
            const via = (x, y) => {
                g.circle(x, y, 8, { fill: GOLD });
                g.circle(x, y, 3.2, { fill: '#2a2a2a' });
            };
            for (const [x, y] of g.scatter(5, { margin: 40, minDist: 70 })) {
                // Walk 2-3 Manhattan legs from (x, y).
                let px = x;
                let py = y;
                const pts = [[px, py]];
                let horizontal = g.rng() < 0.5;
                for (let leg = 0; leg < 3; leg++) {
                    const len = g.range(35, 80) * (g.rng() < 0.5 ? -1 : 1);
                    if (horizontal) px = Math.max(24, Math.min(276, px + len));
                    else py = Math.max(24, Math.min(276, py + len));
                    pts.push([px, py]);
                    horizontal = !horizontal;
                }
                g.polyline(pts, { stroke: GOLD_DIM, 'stroke-width': width + 3, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' });
                g.polyline(pts, { stroke: GOLD, 'stroke-width': width, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' });
                via(pts[0][0], pts[0][1]);
                via(px, py);
            }
        },
        // 1: solder pads -- rows of gold pads with shiny solder blobs
        g => {
            const rows = 2 + Math.floor(g.rng() * 2);
            for (let r = 0; r < rows; r++) {
                const cols = 5 + Math.floor(g.rng() * 3);
                const y = 70 + (r + 0.5) * (160 / rows) + g.range(-6, 6);
                const x0 = 150 - (cols - 1) * 14;
                for (let c = 0; c < cols; c++) {
                    const x = x0 + c * 28;
                    g.rect(x - 9, y - 9, 18, 18, { fill: GOLD_DIM, rx: 3 });
                    g.rect(x - 7, y - 7, 14, 14, { fill: GOLD, rx: 2 });
                    if (g.rng() < 0.55) {
                        g.circle(x, y, 5.5, { fill: '#c9ced3' });
                        g.circle(x - 1.8, y - 1.8, 2, { fill: '#ffffff' });
                    }
                }
                // Silkscreen outline around the row.
                g.rect(x0 - 16, y - 16, (cols - 1) * 28 + 32, 32, { fill: 'none', stroke: '#e8eef2', 'stroke-width': 1.5, 'stroke-dasharray': '6 4', rx: 4 });
            }
        },
        // 2: LEDs -- coloured lamps with a glow, on dark bases
        g => {
            const colours = [['#ff3b3b', '#ff9a9a'], ['#3bff6e', '#a6ffbf'], ['#3b8bff', '#a6cbff'], ['#ffd23b', '#fff0a6']];
            for (const [x, y] of g.scatter(6, { margin: 40, minDist: 64 })) {
                const [core, rim] = g.pick(colours);
                const r = g.range(9, 12);
                g.circle(x, y, r * 2.6, { fill: core, 'fill-opacity': 0.14 });
                g.circle(x, y, r * 1.7, { fill: core, 'fill-opacity': 0.22 });
                g.rect(x - r - 4, y - r - 4, 2 * r + 8, 2 * r + 8, { fill: '#2a2f36', rx: 4 });
                g.circle(x, y, r, { fill: core });
                g.circle(x, y, r, { fill: 'none', stroke: rim, 'stroke-width': 2 });
                g.circle(x - r * 0.35, y - r * 0.35, r * 0.32, { fill: '#ffffff', 'fill-opacity': 0.9 });
            }
        },
    ],
};
