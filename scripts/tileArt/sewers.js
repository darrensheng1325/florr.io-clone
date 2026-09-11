'use strict';
/**
 * Skin 1, sewers: mortared brickwork, sluggish sewage with a scum line, and
 * grates / puddles / rubble on the tunnel floor. Palette from the tile skin
 * contract.
 *
 * The brick courses are laid on a grid that divides the tile evenly (six
 * 50-unit courses of 100-unit bricks, odd courses offset by half a brick), so
 * a brick cut by the tile edge continues as the same brick in the neighbour
 * and the motif tiles seamlessly.
 */

const TILE = 300;
const COURSE = 50;       // brick course height
const BRICK = 100;       // brick length
const MORTAR = 7;        // mortar joint width

/** Keeps a wandering polyline inside the tile (with room for its stroke). */
function clampPoints(pts, inset = 6) {
    return pts.map(([x, y]) => [Math.max(inset, Math.min(TILE - inset, x)), Math.max(inset, Math.min(TILE - inset, y))]);
}

module.exports = {
    name: 'sewers',

    wall: {
        fill: '#5a3b2a',        // the damp masonry behind the bricks
        band: '#2c1c12',        // the shaded lip
        line: '#1a0f08',
        lineWidth: 3,
        // A brick-course lip: alternate half-bricks stand proud, so the profile
        // is square castellations -- five bays across the tile, shallow at both
        // ends so the run meets the corner at the same depth on every side.
        profile: {
            kind: 'custom',
            build(scale, { withCornerRuns, CORNER_RUN, TILE: T }) {
                const shallow = 20 * scale;
                const deep = 30 * scale;
                const bays = 5;
                const span = T - 2 * CORNER_RUN;
                const w = span / bays;
                const inner = [];
                for (let i = 0; i < bays; i++) {
                    const x0 = CORNER_RUN + w * i;
                    const d = i % 2 ? deep : shallow;
                    inner.push({ to: [x0, d] });
                    inner.push({ to: [x0 + w, d] });
                }
                return withCornerRuns(shallow, inner);
            },
        },
        highlight: { color: '#8a6446', width: 2.5, scale: 0.5 },
        motif(g) {
            // Mortar bed, then the bricks inset by half a joint on every side.
            g.rect(0, 0, TILE, TILE, { fill: '#3e2a1e' });
            const tones = ['#7a5238', '#734c33', '#82593c', '#6d4730', '#7f5539'];
            const brick = (x, y, tone) => {
                // One brick (or the part of it inside the tile): face, lit top
                // edge, shaded bottom edge. Nothing is drawn outside 0..300.
                const x0 = Math.max(0, x + MORTAR / 2);
                const x1 = Math.min(TILE, x + BRICK - MORTAR / 2);
                if (x1 <= x0) return;
                g.rect(x0, y + MORTAR / 2, x1 - x0, COURSE - MORTAR, { fill: tone, rx: 3 });
                const hx0 = Math.max(0, x + MORTAR / 2 + 3);
                const hx1 = Math.min(TILE, x + BRICK - MORTAR / 2 - 3);
                if (hx1 <= hx0) return;
                g.rect(hx0, y + MORTAR / 2 + 2, hx1 - hx0, 3, { fill: '#8e6446', 'fill-opacity': 0.7 });
                g.rect(hx0, y + COURSE - MORTAR / 2 - 5, hx1 - hx0, 3, { fill: '#4b3020', 'fill-opacity': 0.7 });
            };
            for (let r = 0; r < TILE / COURSE; r++) {
                const y = r * COURSE;
                const offset = r % 2 ? BRICK / 2 : 0;
                for (let c = 0; c < TILE / BRICK; c++) {
                    const x = c * BRICK + offset;
                    const tone = g.pick(tones);
                    brick(x, y, tone);
                    // An offset course's last brick straddles the seam: its
                    // other half wraps to the west edge with the same tone, so
                    // the brick the neighbour continues is one brick.
                    if (x + BRICK > TILE) brick(x - TILE, y, tone);
                }
            }
            // Damp stains and moss creeping over the joints (kept off the seam).
            for (const [x, y] of g.scatter(4, { margin: 36, minDist: 70 })) {
                g.blob(x, y, g.range(18, 28), { fill: '#3b2a1c', 'fill-opacity': 0.55 }, { wobble: 0.35, n: 8, squash: 0.8 });
            }
            for (const [x, y] of g.scatter(6, { margin: 30, minDist: 44 })) {
                g.blob(x, y, g.range(7, 12), { fill: '#5c6b2e' }, { wobble: 0.3, n: 7 });
                g.circle(x - 3, y - 3, 3, { fill: '#7d8f3f' });
            }
            // A hairline crack through a couple of bricks.
            g.polyline(clampPoints(g.crackPoints(g.range(70, 230), g.range(70, 230), g.range(0, 6.28), 4, 14, 0.7)),
                { stroke: '#3e2a1e', 'stroke-width': 3, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' });
        },
    },

    water: {
        fill: '#5b6b2a',
        band: '#3a4518',        // the dark shallows under the scum line
        foam: '#a3b35a',        // the scum line along the shore
        foamWidth: 3,
        // A slow, thick swell: two long waves.
        profile: { kind: 'wave', depth: 16, periods: 2, amplitude: 5 },
        layers: [{ scale: 1, fill: '#3a4518' }, { scale: 0.55, fill: '#4b5a20' }],
        motif(g) {
            // Oily streaks, then rafts of scum and a few rising bubbles.
            for (let i = 0; i < 4; i++) {
                const y = 38 + i * 62 + g.range(-10, 10);
                g.path(g.wavePath(g.range(20, 55), g.range(225, 280), y, 2, g.range(6, 10)),
                    { fill: 'none', stroke: '#4e5c22', 'stroke-width': 5, 'stroke-linecap': 'round' });
            }
            for (const [x, y] of g.scatter(4, { margin: 40, minDist: 70 })) {
                g.blob(x, y, g.range(16, 26), { fill: '#8f9e4c', 'fill-opacity': 0.8 }, { wobble: 0.4, n: 8, squash: 0.7 });
                g.blob(x + g.range(-6, 6), y + g.range(-4, 4), g.range(7, 11), { fill: '#a3b35a' }, { wobble: 0.3, n: 6, squash: 0.7 });
            }
            for (const [x, y] of g.scatter(7, { margin: 24, minDist: 34 })) {
                const r = g.range(4, 8);
                g.circle(x, y, r, { fill: 'none', stroke: '#8a9a48', 'stroke-width': 2.5 });
                g.circle(x - r * 0.35, y - r * 0.35, r * 0.28, { fill: '#c2cf7a' });
            }
        },
    },

    floors: [
        // 0: a drain grate
        g => {
            const cx = 150 + g.range(-12, 12);
            const cy = 150 + g.range(-12, 12);
            const w = 168;
            const h = 118;
            g.rect(cx - w / 2 - 6, cy - h / 2 - 6, w + 12, h + 12, { fill: '#1c1c1c', rx: 10 });
            g.rect(cx - w / 2, cy - h / 2, w, h, { fill: '#3a3a3a', rx: 7 });
            g.rect(cx - w / 2 + 10, cy - h / 2 + 10, w - 20, h - 20, { fill: '#0d0d0d', rx: 4 });
            // Bars across the opening, each with a lit top edge.
            const bars = 6;
            const pitch = (w - 20) / bars;
            for (let i = 0; i < bars; i++) {
                const x = cx - w / 2 + 10 + pitch * (i + 0.5);
                g.rect(x - 6, cy - h / 2 + 8, 12, h - 16, { fill: '#4a4a4a', rx: 3 });
                g.rect(x - 4, cy - h / 2 + 10, 3, h - 20, { fill: '#6a6a6a' });
            }
            // Bolts in the corners and a couple of rust blooms.
            for (const [sx, sy] of [[-1, -1], [1, -1], [-1, 1], [1, 1]]) {
                g.circle(cx + sx * (w / 2 - 2), cy + sy * (h / 2 - 2), 5, { fill: '#5c5c5c' });
                g.circle(cx + sx * (w / 2 - 2) - 1.5, cy + sy * (h / 2 - 2) - 1.5, 2, { fill: '#8a8a8a' });
            }
            for (const [x, y] of g.scatter(3, { margin: 40, minDist: 60 })) {
                g.blob(x, y, g.range(9, 14), { fill: '#8a4a1e', 'fill-opacity': 0.6 }, { wobble: 0.4, n: 7 });
            }
        },
        // 1: a puddle
        g => {
            const cx = 150 + g.range(-20, 20);
            const cy = 150 + g.range(-15, 15);
            g.blob(cx, cy, 78, { fill: '#2f3a18', 'fill-opacity': 0.9 }, { wobble: 0.3, n: 9, squash: 0.7 });
            g.blob(cx + 4, cy + 3, 62, { fill: '#4a5a22' }, { wobble: 0.3, n: 8, squash: 0.68 });
            // A sheen and two ripple rings.
            g.ellipseRotated(cx - 22, cy - 16, 26, 8, -0.35, { fill: '#8a9a4a', 'fill-opacity': 0.55 });
            for (let i = 0; i < 2; i++) {
                const rx = 18 + i * 14;
                g.ellipse(cx + 18, cy + 12, rx, rx * 0.55, { fill: 'none', stroke: '#6d7d33', 'stroke-width': 2.5 });
            }
            // Drips that splashed around the edge.
            for (const [x, y] of g.scatter(6, { margin: 30, minDist: 40 })) {
                g.ellipse(x, y, g.range(5, 9), g.range(3, 5), { fill: '#3f4a1e', 'fill-opacity': 0.8 });
            }
        },
        // 2: rubble
        g => {
            const bricks = ['#7a5238', '#6d4730', '#82593c'];
            for (const [x, y] of g.scatter(6, { margin: 36, minDist: 56 })) {
                const a = g.range(0, 3.14);
                const w = g.range(32, 48);
                const h = g.range(16, 24);
                const cos = Math.cos(a);
                const sin = Math.sin(a);
                const corner = (dx, dy) => [x + dx * cos - dy * sin, y + dx * sin + dy * cos];
                g.polygon([corner(-w / 2, -h / 2), corner(w / 2, -h / 2), corner(w / 2, h / 2), corner(-w / 2, h / 2)], { fill: '#2c1c12' });
                g.polygon([corner(-w / 2 + 2, -h / 2 + 2), corner(w / 2 - 2, -h / 2 + 2), corner(w / 2 - 2, h / 2 - 3), corner(-w / 2 + 2, h / 2 - 3)], { fill: g.pick(bricks) });
            }
            for (const [x, y] of g.scatter(7, { margin: 26, minDist: 36 })) {
                const r = g.range(9, 15);
                g.blob(x, y, r, { fill: '#5e5a54' }, { wobble: 0.25, n: 7, squash: 0.8 });
                g.blob(x - r * 0.25, y - r * 0.25, r * 0.4, { fill: '#84807a' }, { wobble: 0.2, n: 6, squash: 0.8 });
            }
            for (const [x, y] of g.scatter(12, { margin: 20, minDist: 20 })) g.circle(x, y, g.range(2, 3.5), { fill: '#4a3a2a' });
        },
    ],
};
