'use strict';
/**
 * Skin 3, unknown: faceted void crystal with glowing cracks, a starless void,
 * and static / runes / shards on the floor. Palette from the tile skin
 * contract.
 *
 * Seamless tiling: the crystal facets are a 4x4 lattice of jittered quads
 * split into triangles; the lattice wraps (a vertex on the west edge carries
 * the same jitter as its twin on the east edge, and likewise north/south),
 * so the facet outlines meet across every seam.
 */

const TILE = 300;
const CELLS = 4;
const CELL = TILE / CELLS;

/** Keeps a wandering polyline inside the tile (with room for its stroke). */
function clampPoints(pts, inset = 6) {
    return pts.map(([x, y]) => [Math.max(inset, Math.min(TILE - inset, x)), Math.max(inset, Math.min(TILE - inset, y))]);
}

module.exports = {
    name: 'unknown',

    wall: {
        fill: '#3a2a5c',
        band: '#221735',        // the shaded lip
        line: '#120a1f',
        lineWidth: 3,
        // Sharp, irregular crystal points.
        profile: { kind: 'teeth', depth: 22, teeth: [22, 38, 12, 30, 16, 40, 9, 27, 22] },
        highlight: { color: '#6a52a0', width: 2.5, scale: 0.5 },
        motif(g) {
            // The wrapping lattice.
            const jitter = 14;
            const verts = [];
            for (let j = 0; j <= CELLS; j++) {
                verts.push([]);
                for (let i = 0; i <= CELLS; i++) {
                    let dx = g.range(-jitter, jitter);
                    let dy = g.range(-jitter, jitter);
                    const onX = i === 0 || i === CELLS;
                    const onY = j === 0 || j === CELLS;
                    if (onX) dx = 0;
                    if (onY) dy = 0;
                    if (i === CELLS) dy = verts[j][0][1] - j * CELL;      // twin of the west edge vertex
                    if (j === CELLS) dx = verts[0][i][0] - i * CELL;      // twin of the north edge vertex
                    verts[j].push([i * CELL + dx, j * CELL + dy]);
                }
            }
            const facets = ['#432f6b', '#31224f', '#4b3778', '#2c1e47', '#3f2c66'];
            for (let j = 0; j < CELLS; j++) {
                for (let i = 0; i < CELLS; i++) {
                    const a = verts[j][i];
                    const b = verts[j][i + 1];
                    const c = verts[j + 1][i + 1];
                    const d = verts[j + 1][i];
                    if ((i + j) % 2 === 0) {
                        g.polygon([a, b, c], { fill: g.pick(facets) });
                        g.polygon([a, c, d], { fill: g.pick(facets) });
                    } else {
                        g.polygon([a, b, d], { fill: g.pick(facets) });
                        g.polygon([b, c, d], { fill: g.pick(facets) });
                    }
                }
            }
            // Facet edges: a dark seam line over the lattice.
            for (let j = 0; j < CELLS; j++) {
                for (let i = 0; i < CELLS; i++) {
                    const a = verts[j][i];
                    const b = verts[j][i + 1];
                    const c = verts[j + 1][i + 1];
                    const d = verts[j + 1][i];
                    const diag = (i + j) % 2 === 0 ? [a, c] : [b, d];
                    g.polyline([a, b], { stroke: '#261a3d', 'stroke-width': 2 });
                    g.polyline([a, d], { stroke: '#261a3d', 'stroke-width': 2 });
                    g.polyline(diag, { stroke: '#261a3d', 'stroke-width': 1.5 });
                }
            }
            // Glowing cracks: a dim halo under a bright core.
            const cracks = [];
            for (let i = 0; i < 3; i++) {
                cracks.push(clampPoints(g.crackPoints(g.range(40, 260), g.range(40, 260), g.range(0, 6.28), 5, 20, 0.85)));
            }
            for (const pts of cracks) g.polyline(pts, { stroke: '#5a3f8f', 'stroke-width': 7, 'stroke-linecap': 'round', 'stroke-linejoin': 'round', 'stroke-opacity': 0.8 });
            for (const pts of cracks) g.polyline(pts, { stroke: '#8f6fd9', 'stroke-width': 2.5, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' });
            // Glints on the facets.
            for (const [x, y] of g.scatter(6, { margin: 28, minDist: 44 })) {
                g.path(g.starPath(x, y, g.range(5, 8), 1.6, 4, g.range(0, 1.5)), { fill: '#c8b8f0', 'fill-opacity': 0.85 });
            }
        },
    },

    water: {
        fill: '#0b0714',
        band: '#3a2e5c',        // the void's glowing rim
        foam: '#7a5cc7',
        foamWidth: 3,
        // A slow, wide undulation.
        profile: { kind: 'wave', depth: 16, periods: 2, amplitude: 4 },
        layers: [{ scale: 1, fill: '#3a2e5c' }, { scale: 0.55, fill: '#241a3d' }],
        motif(g) {
            // Faint swirls pulling in toward a centre off to one side.
            const cx = g.range(110, 190);
            const cy = g.range(110, 190);
            for (let i = 0; i < 4; i++) {
                const r = 30 + i * 26;
                const a0 = g.range(0, 6.28);
                const a1 = a0 + g.range(2.2, 3.4);
                const x0 = cx + Math.cos(a0) * r;
                const y0 = cy + Math.sin(a0) * r;
                const x1 = cx + Math.cos(a1) * r * 0.85;
                const y1 = cy + Math.sin(a1) * r * 0.85;
                const large = a1 - a0 > Math.PI ? 1 : 0;
                g.path(`M${x0.toFixed(1)},${y0.toFixed(1)}A${r},${r} 0 ${large} 1 ${x1.toFixed(1)},${y1.toFixed(1)}`,
                    { fill: 'none', stroke: '#241a3d', 'stroke-width': 6, 'stroke-linecap': 'round' });
            }
            // Motes drifting in the dark, a few bright enough to twinkle.
            for (const [x, y] of g.scatter(14, { margin: 22, minDist: 26 })) {
                const r = g.range(1.8, 3.6);
                g.circle(x, y, r * 2.4, { fill: '#7a5cc7', 'fill-opacity': 0.18 });
                g.circle(x, y, r, { fill: g.pick(['#7a5cc7', '#9d84e0', '#c8b8f0']) });
            }
            for (const [x, y] of g.scatter(3, { margin: 40, minDist: 80 })) {
                g.path(g.starPath(x, y, 9, 2.2, 4, 0), { fill: '#e6dcff', 'fill-opacity': 0.9 });
            }
        },
    },

    floors: [
        // 0: static -- scanline bursts of grey and violet noise
        g => {
            const tones = ['#d8d8d8', '#9a9a9a', '#5c5c5c', '#b9a6ec', '#8f6fd9', '#ffffff'];
            for (let row = 0; row < 12; row++) {
                const y = 26 + row * 22 + g.range(-3, 3);
                let x = g.range(20, 70);
                while (x < 270) {
                    const w = g.range(4, 26);
                    if (g.rng() < 0.7) g.rect(x, y, Math.min(w, 280 - x), g.range(4, 7), { fill: g.pick(tones), 'fill-opacity': g.range(0.45, 0.95) });
                    x += w + g.range(3, 14);
                }
            }
        },
        // 1: a glowing rune circle
        g => {
            const cx = 150 + g.range(-10, 10);
            const cy = 150 + g.range(-10, 10);
            const R = 92;
            g.circle(cx, cy, R + 18, { fill: '#7a5cc7', 'fill-opacity': 0.12 });
            g.circle(cx, cy, R, { fill: '#3a2a5c', 'fill-opacity': 0.35 });
            g.circle(cx, cy, R, { fill: 'none', stroke: '#8f6fd9', 'stroke-width': 4 });
            g.circle(cx, cy, R - 12, { fill: 'none', stroke: '#8f6fd9', 'stroke-width': 1.5 });
            // Two overlaid squares make an eight-pointed star; a triangle inside.
            const ring = (n, r, rot) => {
                const pts = [];
                for (let i = 0; i < n; i++) {
                    const a = rot + (i / n) * Math.PI * 2;
                    pts.push([cx + Math.cos(a) * r, cy + Math.sin(a) * r]);
                }
                return pts;
            };
            g.polygon(ring(4, R - 12, 0), { fill: 'none', stroke: '#b9a6ec', 'stroke-width': 2.5, 'stroke-linejoin': 'round' });
            g.polygon(ring(4, R - 12, Math.PI / 4), { fill: 'none', stroke: '#b9a6ec', 'stroke-width': 2.5, 'stroke-linejoin': 'round' });
            g.polygon(ring(3, R - 40, -Math.PI / 2), { fill: 'none', stroke: '#8f6fd9', 'stroke-width': 3, 'stroke-linejoin': 'round' });
            g.circle(cx, cy, 9, { fill: '#e6dcff' });
            // Glyph ticks around the outer ring.
            for (let i = 0; i < 16; i++) {
                const a = (i / 16) * Math.PI * 2;
                const r0 = R - 6;
                const r1 = i % 4 === 0 ? R + 10 : R + 4;
                g.line(cx + Math.cos(a) * r0, cy + Math.sin(a) * r0, cx + Math.cos(a) * r1, cy + Math.sin(a) * r1,
                    { stroke: '#c8b8f0', 'stroke-width': 2, 'stroke-linecap': 'round' });
            }
        },
        // 2: crystal shards
        g => {
            for (const [x, y] of g.scatter(7, { margin: 34, minDist: 52 })) {
                const a = g.range(0, 6.28);
                const len = g.range(30, 50);
                const wid = g.range(9, 14);
                const cos = Math.cos(a);
                const sin = Math.sin(a);
                const p = (dx, dy) => [x + dx * cos - dy * sin, y + dx * sin + dy * cos];
                const tip = p(len / 2, 0);
                const base = p(-len / 2, 0);
                const left = p(-len * 0.15, -wid);
                const right = p(-len * 0.15, wid);
                g.polygon([tip, left, base, right], { fill: '#7a5cc7', 'fill-opacity': 0.25 }); // ground glow
                g.polygon([tip, left, base], { fill: '#8f6fd9' });
                g.polygon([tip, base, right], { fill: '#5a3f8f' });
                g.polyline([base, tip], { stroke: '#c8b8f0', 'stroke-width': 1.5, 'stroke-linecap': 'round' });
            }
            for (const [x, y] of g.scatter(10, { margin: 22, minDist: 22 })) {
                g.path(g.starPath(x, y, g.range(3, 5), 1.2, 4, g.range(0, 1.5)), { fill: '#c8b8f0', 'fill-opacity': 0.8 });
            }
        },
    ],
};
