'use strict';
/**
 * Tile art framework: the geometry every tile family shares.
 *
 * A terrain tile is `family x skin x mask/variant`:
 *
 *   - family: wall (tileId 1), water (tileId 2) or floor (tileId 0, a
 *     decoration painted over the ground);
 *   - skin: 0 is the default look (today's wall.svg / water.svg), 1..3 are the
 *     three biome families in `SKIN_NAMES` order: sewers, computer, unknown.
 *     A skin index is NOT a ground id: the engine's skinForGround() maps
 *     groundId 6/7/8 (sewers/computer/unknown) to skin 1/2/3 and every other
 *     ground to skin 0, so a wall on garden ground is the default brown wall;
 *   - mask: for wall/water, the bit set of exposed sides (n=1, e=2, s=4, w=8)
 *     the tile shows a lip / shoreline on; for floors, the variant 0..2.
 *
 * This module owns everything that is the SAME for every skin: the lip and
 * shoreline profiles and how they join around corners, the band fills, the
 * outline strokes, the SVG document, the tileset entry, the id layout and the
 * drawing context handed to a skin's motif and stamp drawers. A skin module
 * (scripts/tileArt/<skin>.js) owns only the LOOK: palette, profile parameters,
 * a base motif drawer per family and three floor stamp drawers. See
 * scripts/tileArt/sewers.js for the shape of one, and `renderTile()` below
 * for exactly what the framework does with each field.
 *
 * Id layout of maps/terrain.tsj (fixed, independent of which skins exist):
 *
 *   0..5     air, wall, water, bridge, sewage, block      (untouched)
 *   6..20    wall_edge_<sides>, masks 1..15               default skin
 *   21..35   water_edge_<sides>, masks 1..15              default skin
 *   36 + 35*(skin-1) + k, skin 1..3 (sewers 36..70, computer 71..105,
 *   unknown 106..140):
 *     k = 0        wall_<skin>                 base
 *     k = 1..15    wall_<skin>_edge_<sides>    mask k
 *     k = 16       water_<skin>                base
 *     k = 17..31   water_<skin>_edge_<sides>   mask k-16
 *     k = 32..34   floor_<skin>_<v>            variant k-32
 *   tilecount 141; the ground tileset's firstgid is therefore 142.
 *
 * Adding a family: append its name to `SKIN_NAMES` and its colours to
 * `SKIN_PALETTE`, write scripts/tileArt/<name>.js, teach the engine's
 * kTileSkinNames / skinForGround about it, then run `--art` and
 * `npm run build:map` (the ground tileset's firstgid moves with tilecount).
 *
 * Geometry. Every band is drawn in the "north frame": x runs along the side
 * from 0 to 300, d is the depth into the tile. The other three sides are the
 * same shape rotated about the tile centre, so a band that ends at depth D on
 * the north side meets the east band's start at exactly (300-D, D): corners
 * join without a computed intersection, and the profile's end depth is what
 * makes two neighbouring tiles' bands continue across the seam.
 *
 * A profile is `{ start: [x, d], segments: [...] }` where a segment is either
 * `{ to }` (a line) or `{ c1, c2, to }` (a cubic). The first and last segment
 * are the flat "corner runs" -- the part of the band that lies inside the
 * perpendicular band when that side is exposed too -- and get dropped from the
 * outline when the neighbouring side is exposed.
 */

const TILE = 300;
const TILE_AIR = 0;
const TILE_WALL = 1;
const TILE_WATER = 2;

/** Bit per side, in the order a variant's name lists them. */
const SIDES = [
    { name: 'n', bit: 1, dx: 0, dy: -1 },
    { name: 'e', bit: 2, dx: 1, dy: 0 },
    { name: 's', bit: 4, dx: 0, dy: 1 },
    { name: 'w', bit: 8, dx: -1, dy: 0 },
];

/**
 * Skin index -> name; index 0 is the default family. Must match
 * kTileSkinNames in cpp/shared/game/constants.h, in this order.
 */
const SKIN_NAMES = ['', 'sewers', 'computer', 'unknown'];
const SKIN_COUNT = SKIN_NAMES.length;

/**
 * The contract's palette per skin: the base and lip / shore colours a skin's
 * tileset entries carry (`color`, `borderColor`). A skin module is expected to
 * use these; until its module exists they are what its placeholder entries
 * are written with.
 */
const SKIN_PALETTE = [
    { wall: { fill: '#99550c', band: '#783f01' }, water: { fill: '#4169E1', band: '#2A4FA0' } },   // default
    { wall: { fill: '#5a3b2a', band: '#2c1c12' }, water: { fill: '#5b6b2a', band: '#3a4518' } },   // sewers
    { wall: { fill: '#1f2a35', band: '#0f151b' }, water: { fill: '#00d0ff', band: '#0088aa' } },   // computer
    { wall: { fill: '#3a2a5c', band: '#221735' }, water: { fill: '#0b0714', band: '#3a2e5c' } },   // unknown
];

const FAMILIES = ['wall', 'water', 'floor'];
const FLOOR_VARIANTS = 3;
const DEFAULT_TILE_COUNT = 36;                 // ids 0..35
const SKIN_BLOCK = 2 * 16 + FLOOR_VARIANTS;   // 35 ids per skin >= 1
const TILE_COUNT = DEFAULT_TILE_COUNT + (SKIN_COUNT - 1) * SKIN_BLOCK;   // 141
if (SKIN_PALETTE.length !== SKIN_COUNT) throw new Error(`SKIN_PALETTE has ${SKIN_PALETTE.length} entries for ${SKIN_COUNT} skins`);

/** The six tiles that predate skins, by local id. */
const BUILTIN_CLASSES = ['air', 'wall', 'water', 'bridge', 'sewage', 'block'];

// ---------------------------------------------------------------------------
// Names, masks, ids
// ---------------------------------------------------------------------------

function maskName(mask) {
    return SIDES.filter(side => mask & side.bit).map(side => side.name).join('');
}

function maskOf(edges) {
    if (edges === undefined || edges === null || edges === '') return 0;
    if (typeof edges !== 'string') throw new Error(`"edges" must be a string, got ${JSON.stringify(edges)}`);
    let mask = 0;
    for (const ch of edges) {
        const side = SIDES.find(s => s.name === ch);
        if (!side) throw new Error(`"edges" value "${edges}" names an unknown side "${ch}"`);
        if (mask & side.bit) throw new Error(`"edges" value "${edges}" repeats "${ch}"`);
        mask |= side.bit;
    }
    return mask;
}

function skinIndex(name) {
    const index = SKIN_NAMES.indexOf(name || '');
    if (index < 0) throw new Error(`unknown skin "${name}" (known: ${SKIN_NAMES.slice(1).join(', ')})`);
    return index;
}

function familyOfTileId(tileId) {
    return tileId === TILE_WALL ? 'wall' : tileId === TILE_WATER ? 'water' : tileId === TILE_AIR ? 'floor' : null;
}

function tileIdOfFamily(family) {
    return family === 'wall' ? TILE_WALL : family === 'water' ? TILE_WATER : TILE_AIR;
}

/** "wall", "wall_edge_ne", "wall_sewers", "wall_sewers_edge_ne", "water_computer", "floor_unknown_2". */
function className(skin, family, key) {
    const name = SKIN_NAMES[skin];
    if (family === 'floor') {
        if (skin === 0) throw new Error('the default skin has no floor decorations');
        return `floor_${name}_${key}`;
    }
    const base = skin === 0 ? family : `${family}_${name}`;
    return key === 0 ? base : `${base}_edge_${maskName(key)}`;
}

/** Local tileset id of (skin, family, mask-or-variant). */
function localId(skin, family, key) {
    if (skin === 0) {
        if (family === 'floor') throw new Error('the default skin has no floor decorations');
        if (key === 0) return family === 'wall' ? 1 : 2;
        return (family === 'wall' ? 6 : 21) + key - 1;
    }
    const first = DEFAULT_TILE_COUNT + (skin - 1) * SKIN_BLOCK;
    if (family === 'wall') return first + key;
    if (family === 'water') return first + 16 + key;
    return first + 32 + key;
}

/** First local id of a skin's block (0 for the default skin). */
function skinFirstId(skin) {
    return skin === 0 ? 0 : DEFAULT_TILE_COUNT + (skin - 1) * SKIN_BLOCK;
}

/**
 * Every id of the full tileset with what it must be: `{ id, class, family,
 * skin, mask, variant }`; the six builtin ids carry `family: null`.
 */
function expectedLayout() {
    const out = [];
    for (let id = 0; id < BUILTIN_CLASSES.length; id++) {
        out.push({ id, class: BUILTIN_CLASSES[id], family: id === 1 ? 'wall' : id === 2 ? 'water' : null, skin: 0, mask: 0, builtin: true });
    }
    for (let mask = 1; mask <= 15; mask++) out.push({ id: localId(0, 'wall', mask), class: className(0, 'wall', mask), family: 'wall', skin: 0, mask });
    for (let mask = 1; mask <= 15; mask++) out.push({ id: localId(0, 'water', mask), class: className(0, 'water', mask), family: 'water', skin: 0, mask });
    for (let skin = 1; skin < SKIN_COUNT; skin++) {
        for (let mask = 0; mask <= 15; mask++) out.push({ id: localId(skin, 'wall', mask), class: className(skin, 'wall', mask), family: 'wall', skin, mask });
        for (let mask = 0; mask <= 15; mask++) out.push({ id: localId(skin, 'water', mask), class: className(skin, 'water', mask), family: 'water', skin, mask });
        for (let v = 0; v < FLOOR_VARIANTS; v++) out.push({ id: localId(skin, 'floor', v), class: className(skin, 'floor', v), family: 'floor', skin, variant: v });
    }
    out.sort((a, b) => a.id - b.id);
    for (let i = 0; i < out.length; i++) if (out[i].id !== i) throw new Error(`id layout has a hole at ${i}`);
    if (out.length !== TILE_COUNT) throw new Error(`id layout has ${out.length} ids, expected ${TILE_COUNT}`);
    return out;
}

// ---------------------------------------------------------------------------
// Numbers and SVG text
// ---------------------------------------------------------------------------

function fmt(v) {
    const s = (Math.round(v * 10) / 10).toFixed(1);
    return s.endsWith('.0') ? s.slice(0, -2) : s;
}

function pt([x, y]) {
    return `${fmt(x)},${fmt(y)}`;
}

function attrText(attrs) {
    if (!attrs) return '';
    return Object.entries(attrs)
        .filter(([, v]) => v !== undefined && v !== null && v !== false)
        .map(([k, v]) => ` ${k}="${typeof v === 'number' ? fmt(v) : v}"`)
        .join('');
}

function svgDocument(parts) {
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${TILE}" height="${TILE}" viewBox="0 0 ${TILE} ${TILE}">\n` +
        parts.map(p => `  ${p}\n`).join('') + '</svg>\n';
}

// ---------------------------------------------------------------------------
// Deterministic randomness
// ---------------------------------------------------------------------------

function hashString(text) {
    let h = 2166136261 >>> 0;
    for (let i = 0; i < text.length; i++) {
        h ^= text.charCodeAt(i);
        h = Math.imul(h, 16777619) >>> 0;
    }
    return h;
}

/** mulberry32: the same stream for the same seed on every machine. */
function makeRng(seed) {
    let a = seed >>> 0;
    return function rng() {
        a = (a + 0x6D2B79F5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

const CORNER_RUN = 20;   // flat run at each end of every profile

/**
 * Wraps the interesting part of a profile (segments from (CORNER_RUN, depth)
 * to (300-CORNER_RUN, depth)) in its corner runs. The first and last segment
 * end exactly at the corner point (depth, depth): that is where the outline of
 * a perpendicular exposed band arrives too, so trimmed outlines meet there
 * instead of cutting the corner with a diagonal.
 */
function withCornerRuns(depth, inner) {
    const segments = [{ to: [depth, depth] }];
    if (CORNER_RUN > depth) segments.push({ to: [CORNER_RUN, depth] });
    segments.push(...inner);
    if (CORNER_RUN > depth) segments.push({ to: [TILE - depth, depth] });
    segments.push({ to: [TILE, depth] });
    return { start: [0, depth], segments };
}

/**
 * Polyline of evenly spaced vertex depths between the corner runs. The first
 * and last depth must equal `depth` (the corner depth). Repeating a depth
 * twice makes a flat step (a mesa ledge); alternating makes teeth (rock).
 */
function teethProfile({ depth, teeth }, scale = 1) {
    if (teeth[0] !== depth || teeth[teeth.length - 1] !== depth) {
        throw new Error(`a teeth profile must start and end at its depth ${depth} (got ${teeth[0]}, ${teeth[teeth.length - 1]})`);
    }
    const span = TILE - 2 * CORNER_RUN;
    const inner = [];
    for (let i = 1; i < teeth.length; i++) {
        inner.push({ to: [CORNER_RUN + span * i / (teeth.length - 1), teeth[i] * scale] });
    }
    return withCornerRuns(depth * scale, inner);
}

/**
 * 1-cos wave: depth at every corner run is exactly `depth` with a flat
 * tangent, so the shoreline leaves and enters the corners smoothly. The wave
 * swings from `depth` to `depth + 2*amplitude`.
 */
function waveProfile({ depth, periods, amplitude }, scale = 1) {
    const span = TILE - 2 * CORNER_RUN;
    const half = span / (periods * 2);
    const inner = [];
    for (let i = 0; i < periods * 2; i++) {
        const x0 = CORNER_RUN + half * i;
        const x1 = x0 + half;
        const d0 = (i % 2 === 0 ? depth : depth + 2 * amplitude) * scale;
        const d1 = (i % 2 === 0 ? depth + 2 * amplitude : depth) * scale;
        inner.push({ c1: [x0 + half / 2, d0], c2: [x0 + half / 2, d1], to: [x1, d1] });
    }
    return withCornerRuns(depth * scale, inner);
}

/**
 * Rounded bumps: one cubic per bulge, each bulging `bulges[i]` deeper than
 * `depth` at its middle and returning to `depth` at its ends -- boulders,
 * moss, coral heads. A negative bulge is a notch.
 */
function scallopProfile({ depth, bulges }, scale = 1) {
    const span = TILE - 2 * CORNER_RUN;
    const width = span / bulges.length;
    const inner = [];
    for (let i = 0; i < bulges.length; i++) {
        const x0 = CORNER_RUN + width * i;
        const x1 = x0 + width;
        const d = (depth + bulges[i] * 1.5) * scale;   // a cubic reaches 3/4 of its control depth
        inner.push({ c1: [x0 + width * 0.25, d], c2: [x1 - width * 0.25, d], to: [x1, depth * scale] });
    }
    return withCornerRuns(depth * scale, inner);
}

/** A profile spec -> profile, scaled in depth (the corner join survives scaling). */
function buildProfile(spec, scale = 1) {
    switch (spec.kind) {
        case 'teeth': return teethProfile(spec, scale);
        case 'wave': return waveProfile(spec, scale);
        case 'scallop': return scallopProfile(spec, scale);
        case 'custom': return spec.build(scale, { withCornerRuns, CORNER_RUN, TILE });
        default: throw new Error(`unknown profile kind "${spec.kind}"`);
    }
}

/** The same profile walked from x=300 back to x=0. */
function reverseProfile(profile) {
    const points = [profile.start, ...profile.segments.map(s => s.to)];
    const segments = [];
    for (let i = profile.segments.length - 1; i >= 0; i--) {
        const seg = profile.segments[i];
        const to = points[i];
        segments.push(seg.c1 ? { c1: seg.c2, c2: seg.c1, to } : { to });
    }
    return { start: points[points.length - 1], segments };
}

/** North-frame point -> tile point for the given side (index into SIDES). */
function orient(sideIndex, [x, d]) {
    switch (sideIndex) {
        case 0: return [x, d];
        case 1: return [TILE - d, x];
        case 2: return [TILE - x, TILE - d];
        default: return [d, TILE - x];
    }
}

function segmentPath(seg, sideIndex) {
    const to = orient(sideIndex, seg.to);
    if (!seg.c1) return `L${pt(to)}`;
    return `C${pt(orient(sideIndex, seg.c1))} ${pt(orient(sideIndex, seg.c2))} ${pt(to)}`;
}

/** The filled band of one side: the outer edge plus the profile walked back. */
function bandFill(profile, sideIndex) {
    const back = reverseProfile(profile);
    let d = `M${pt(orient(sideIndex, [0, 0]))}L${pt(orient(sideIndex, [TILE, 0]))}L${pt(orient(sideIndex, back.start))}`;
    for (const seg of back.segments) d += segmentPath(seg, sideIndex);
    return d + 'Z';
}

/** The band fills of every exposed side, as one path `d`. */
function bandFills(profile, mask) {
    return SIDES.map((side, i) => (mask & side.bit ? bandFill(profile, i) : '')).join('');
}

/**
 * The inner outline of the union of every exposed band, as open runs that
 * chain around corners. `profileOf(sideIndex)` returns the north-frame profile
 * for that side. A corner run is dropped when the neighbouring side is
 * exposed, because it sits inside the other band.
 */
function chainedOutline(mask, profileOf) {
    const exposed = SIDES.map(side => !!(mask & side.bit));
    const runs = [];
    if (mask === 0) return runs;
    const all = exposed.every(Boolean);
    // Start each run at a side whose predecessor is not exposed; the all-four
    // case has no such side and becomes one closed loop.
    const starts = all ? [0] : SIDES.map((_, i) => i).filter(i => exposed[i] && !exposed[(i + 3) % 4]);
    for (const start of starts) {
        let d = '';
        let i = start;
        let first = true;
        while (exposed[i]) {
            const prevExposed = exposed[(i + 3) % 4];
            const nextExposed = exposed[(i + 1) % 4];
            const profile = profileOf(i);
            const segs = profile.segments.slice(prevExposed ? 1 : 0, nextExposed ? -1 : undefined);
            const startPoint = prevExposed ? profile.segments[0].to : profile.start;
            if (first) {
                d += `M${pt(orient(i, startPoint))}`;
                first = false;
            }
            for (const seg of segs) d += segmentPath(seg, i);
            i = (i + 1) % 4;
            if (all && i === start) break;
        }
        if (all) d += 'Z';
        runs.push(d);
    }
    return runs;
}

/** The rim along the tile's own edge, inset so the whole stroke stays in the box. */
function rimOutline(mask, lineWidth) {
    const inset = lineWidth / 2;
    return chainedOutline(mask, () => ({
        start: [0, inset],
        segments: [{ to: [inset, inset] }, { to: [TILE - inset, inset] }, { to: [TILE, inset] }],
    }));
}

function strokePath(d, color, width) {
    return `<path d="${d}" fill="none" stroke="${color}" stroke-width="${fmt(width)}" ` +
        'stroke-linejoin="round" stroke-linecap="round"/>';
}

// ---------------------------------------------------------------------------
// Drawing context
// ---------------------------------------------------------------------------

/**
 * What a skin's motif / stamp drawer is handed. Every method appends one SVG
 * element to the tile; `rng()` is seeded from the tile's identity so the same
 * tile draws the same bytes on every run, and the base motif of a wall is
 * identical across all sixteen of its edge variants. `TILE` is 300.
 */
class Draw {
    constructor(seed) {
        this.parts = [];
        this.rng = makeRng(seed);
        this.TILE = TILE;
    }

    /** Uniform in [lo, hi). */
    range(lo, hi) { return lo + (hi - lo) * this.rng(); }

    /** One of the given values. */
    pick(values) { return values[Math.floor(this.rng() * values.length)]; }

    raw(text) { this.parts.push(text); return this; }

    rect(x, y, w, h, attrs) {
        this.parts.push(`<rect x="${fmt(x)}" y="${fmt(y)}" width="${fmt(w)}" height="${fmt(h)}"${attrText(attrs)}/>`);
        return this;
    }

    circle(cx, cy, r, attrs) {
        this.parts.push(`<circle cx="${fmt(cx)}" cy="${fmt(cy)}" r="${fmt(r)}"${attrText(attrs)}/>`);
        return this;
    }

    ellipse(cx, cy, rx, ry, attrs) {
        this.parts.push(`<ellipse cx="${fmt(cx)}" cy="${fmt(cy)}" rx="${fmt(rx)}" ry="${fmt(ry)}"${attrText(attrs)}/>`);
        return this;
    }

    line(x1, y1, x2, y2, attrs) {
        this.parts.push(`<line x1="${fmt(x1)}" y1="${fmt(y1)}" x2="${fmt(x2)}" y2="${fmt(y2)}"${attrText(attrs)}/>`);
        return this;
    }

    path(d, attrs) {
        this.parts.push(`<path d="${d}"${attrText(attrs)}/>`);
        return this;
    }

    polygon(points, attrs) {
        this.parts.push(`<polygon points="${points.map(pt).join(' ')}"${attrText(attrs)}/>`);
        return this;
    }

    polyline(points, attrs) {
        this.parts.push(`<polyline points="${points.map(pt).join(' ')}" fill="none"${attrText(attrs)}/>`);
        return this;
    }

    /** `<g attrs>` around everything `fn` draws, on one line. */
    group(attrs, fn) {
        const outer = this.parts;
        this.parts = [];
        fn(this);
        const inner = this.parts.join('');
        this.parts = outer;
        this.parts.push(`<g${attrText(attrs)}>${inner}</g>`);
        return this;
    }

    // -- placement helpers ---------------------------------------------------

    /**
     * `count` points at least `minDist` apart, `margin` in from the tile edge,
     * by rejection sampling; gives up on a point after many tries so a too
     * tight request degrades to fewer points instead of hanging.
     */
    scatter(count, { margin = 20, minDist = 30, region } = {}) {
        const [x0, y0, x1, y1] = region || [margin, margin, TILE - margin, TILE - margin];
        const points = [];
        for (let n = 0; n < count; n++) {
            for (let tries = 0; tries < 60; tries++) {
                const p = [this.range(x0, x1), this.range(y0, y1)];
                if (points.every(q => Math.hypot(q[0] - p[0], q[1] - p[1]) >= minDist)) {
                    points.push(p);
                    break;
                }
            }
        }
        return points;
    }

    /** `count` points on a jittered grid -- even coverage for strata, pins, bricks. */
    grid(cols, rows, { margin = 0, jitter = 0 } = {}) {
        const points = [];
        const w = (TILE - 2 * margin) / cols;
        const h = (TILE - 2 * margin) / rows;
        for (let r = 0; r < rows; r++) {
            for (let c = 0; c < cols; c++) {
                points.push([
                    margin + w * (c + 0.5) + (jitter ? this.range(-jitter, jitter) : 0),
                    margin + h * (r + 0.5) + (jitter ? this.range(-jitter, jitter) : 0),
                ]);
            }
        }
        return points;
    }

    // -- shape helpers ---------------------------------------------------------

    /**
     * Irregular rounded shape (a pebble, boulder, leaf pile, cloud): `n`
     * vertices around (cx, cy) at radius r +/- wobble, joined by cubics so the
     * outline stays smooth.
     */
    blobPath(cx, cy, r, { wobble = 0.25, n = 7, squash = 1 } = {}) {
        const pts = [];
        for (let i = 0; i < n; i++) {
            const a = (i / n) * Math.PI * 2 + this.range(-0.3, 0.3) / n;
            const rr = r * (1 + this.range(-wobble, wobble));
            pts.push([cx + Math.cos(a) * rr, cy + Math.sin(a) * rr * squash]);
        }
        return smoothClosedPath(pts);
    }

    blob(cx, cy, r, attrs, options) {
        return this.path(this.blobPath(cx, cy, r, options), attrs);
    }

    /** A leaf: two arcs meeting at tip and stem, rotated by `angle` radians. */
    leafPath(cx, cy, length, width, angle) {
        const cos = Math.cos(angle);
        const sin = Math.sin(angle);
        const local = (x, y) => [cx + x * cos - y * sin, cy + x * sin + y * cos];
        const h = length / 2;
        const a = local(-h, 0);
        const b = local(h, 0);
        const c1 = local(-h * 0.3, -width);
        const c2 = local(h * 0.3, -width);
        const c3 = local(h * 0.3, width);
        const c4 = local(-h * 0.3, width);
        return `M${pt(a)}C${pt(c1)} ${pt(c2)} ${pt(b)}C${pt(c3)} ${pt(c4)} ${pt(a)}Z`;
    }

    leaf(cx, cy, length, width, angle, attrs) {
        return this.path(this.leafPath(cx, cy, length, width, angle), attrs);
    }

    /** A jagged polyline from (x, y) heading `angle`, wandering `steps` times. */
    crackPoints(x, y, angle, steps, stepLength, wander = 0.7) {
        const pts = [[x, y]];
        let a = angle;
        for (let i = 0; i < steps; i++) {
            a += this.range(-wander, wander);
            x += Math.cos(a) * stepLength * this.range(0.6, 1.4);
            y += Math.sin(a) * stepLength * this.range(0.6, 1.4);
            pts.push([x, y]);
        }
        return pts;
    }

    /** A regular star / flower outline of `points` spikes. */
    starPath(cx, cy, outer, inner, points, rotation = 0) {
        const pts = [];
        for (let i = 0; i < points * 2; i++) {
            const a = rotation + (i / (points * 2)) * Math.PI * 2;
            const r = i % 2 === 0 ? outer : inner;
            pts.push([cx + Math.cos(a) * r, cy + Math.sin(a) * r]);
        }
        return `M${pts.map(pt).join('L')}Z`;
    }

    /** A flower of round petals around a disc. */
    flower(cx, cy, r, { petals = 5, petal, centre, rotation = 0 } = {}) {
        for (let i = 0; i < petals; i++) {
            const a = rotation + (i / petals) * Math.PI * 2;
            this.ellipseRotated(cx + Math.cos(a) * r * 0.62, cy + Math.sin(a) * r * 0.62, r * 0.42, r * 0.3, a, { fill: petal });
        }
        this.circle(cx, cy, r * 0.32, { fill: centre });
        return this;
    }

    /** An ellipse rotated by `angle` radians, as a path (no transform attribute needed). */
    ellipseRotated(cx, cy, rx, ry, angle, attrs) {
        const k = 0.5523;
        const cos = Math.cos(angle);
        const sin = Math.sin(angle);
        const local = (x, y) => [cx + x * cos - y * sin, cy + x * sin + y * cos];
        const d = `M${pt(local(rx, 0))}` +
            `C${pt(local(rx, ry * k))} ${pt(local(rx * k, ry))} ${pt(local(0, ry))}` +
            `C${pt(local(-rx * k, ry))} ${pt(local(-rx, ry * k))} ${pt(local(-rx, 0))}` +
            `C${pt(local(-rx, -ry * k))} ${pt(local(-rx * k, -ry))} ${pt(local(0, -ry))}` +
            `C${pt(local(rx * k, -ry))} ${pt(local(rx, -ry * k))} ${pt(local(rx, 0))}Z`;
        return this.path(d, attrs);
    }

    /** A wavy horizontal band from x0 to x1 at y: `waves` bumps of `amp`. */
    wavePath(x0, x1, y, waves, amp) {
        const half = (x1 - x0) / (waves * 2);
        let d = `M${pt([x0, y])}`;
        for (let i = 0; i < waves * 2; i++) {
            const xa = x0 + half * i;
            const xb = xa + half;
            const ya = i % 2 === 0 ? y : y + amp;
            const yb = i % 2 === 0 ? y + amp : y;
            d += `C${pt([xa + half / 2, ya])} ${pt([xa + half / 2, yb])} ${pt([xb, yb])}`;
        }
        return d;
    }

    /** A capsule (rounded bar) from (x1,y1) to (x2,y2), as a stroked line. */
    bar(x1, y1, x2, y2, width, color) {
        return this.line(x1, y1, x2, y2, { stroke: color, 'stroke-width': width, 'stroke-linecap': 'round' });
    }
}

/** Closed Catmull-Rom-ish cubic through the points. */
function smoothClosedPath(pts) {
    const n = pts.length;
    let d = `M${pt(pts[0])}`;
    for (let i = 0; i < n; i++) {
        const p0 = pts[(i - 1 + n) % n];
        const p1 = pts[i];
        const p2 = pts[(i + 1) % n];
        const p3 = pts[(i + 2) % n];
        const c1 = [p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6];
        const c2 = [p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6];
        d += `C${pt(c1)} ${pt(c2)} ${pt(p2)}`;
    }
    return d + 'Z';
}

// ---------------------------------------------------------------------------
// Tile rendering
// ---------------------------------------------------------------------------

/**
 * Skin module contract (scripts/tileArt/<skin>.js):
 *
 *   module.exports = {
 *     name: 'sewers',                      // must equal SKIN_NAMES[index]
 *     wall: {
 *       fill: '#5a3b2a',                   // base rock colour (also the tile's `color`)
 *       band: '#2c1c12',                   // the lip on every exposed side
 *       line: '#1a0f08',                   // rim + lip outline stroke
 *       lineWidth: 3,                      // optional, default 3
 *       profile: { kind: 'teeth'|'wave'|'scallop', ... },   // see buildProfile()
 *       layers: [{ scale: 1, fill }],      // optional: extra bands under the lip
 *                                          //   (scale < 1 = shallower); default one band
 *       highlight: { color, width, scale },// optional: a stroke along the profile at
 *                                          //   `scale` of its depth, drawn before the line
 *       rim: true,                         // optional: false drops the tile-edge rim
 *       motif(g) {...},                    // draws the base detail (dots, strata, cracks)
 *                                          //   over the fill, under the bands
 *     },
 *     water: { fill, band, foam, foamWidth: 2, profile, layers?, motif(g) },
 *     floors: [g => {...}, g => {...}, g => {...}],   // three stamps over transparent ground
 *   };
 *
 * `g` is a Draw (above). `renderTile()` is the whole pipeline; a skin never
 * touches the bands or the outlines, only what is drawn under them.
 */

function seedFor(skin, family, key) {
    return hashString(`${skin.name || 'default'}/${family}/${key}`);
}

function baseRect(fill) {
    return `<rect width="${TILE}" height="${TILE}" fill="${fill}"/>`;
}

function renderWall(skin, mask) {
    const art = skin.wall;
    if (!art) throw new Error(`skin "${skin.name}" has no wall`);
    const lineWidth = art.lineWidth === undefined ? 3 : art.lineWidth;
    const g = new Draw(seedFor(skin, 'wall', 'motif'));
    g.raw(baseRect(art.fill));
    if (art.motif) art.motif(g, { mask });
    const layers = art.layers || [{ scale: 1, fill: art.band }];
    for (const layer of layers) {
        const profile = buildProfile(art.profile, layer.scale === undefined ? 1 : layer.scale);
        const fills = bandFills(profile, mask);
        g.parts.push(`<path d="${fills}" fill="${layer.fill}"/>`);
    }
    if (art.highlight && mask !== 0) {
        const profile = buildProfile(art.profile, art.highlight.scale === undefined ? 0.5 : art.highlight.scale);
        g.parts.push(strokePath(chainedOutline(mask, () => profile).join(''), art.highlight.color, art.highlight.width || 2));
    }
    const lip = chainedOutline(mask, () => buildProfile(art.profile, 1));
    const rim = art.rim === false ? [] : rimOutline(mask, lineWidth);
    g.parts.push(strokePath([...rim, ...lip].join(''), art.line, lineWidth));
    return svgDocument(g.parts);
}

function renderWater(skin, mask) {
    const art = skin.water;
    if (!art) throw new Error(`skin "${skin.name}" has no water`);
    const g = new Draw(seedFor(skin, 'water', 'motif'));
    g.raw(baseRect(art.fill));
    if (art.motif) art.motif(g, { mask });
    const layers = art.layers || [{ scale: 1, fill: art.band }];
    for (const layer of layers) {
        const profile = buildProfile(art.profile, layer.scale === undefined ? 1 : layer.scale);
        g.parts.push(`<path d="${bandFills(profile, mask)}" fill="${layer.fill}"/>`);
    }
    const foam = chainedOutline(mask, () => buildProfile(art.profile, 1));
    g.parts.push(strokePath(foam.join(''), art.foam, art.foamWidth === undefined ? 2 : art.foamWidth));
    return svgDocument(g.parts);
}

function renderFloor(skin, variant) {
    if (!Array.isArray(skin.floors) || skin.floors.length !== FLOOR_VARIANTS) {
        throw new Error(`skin "${skin.name}" must export exactly ${FLOOR_VARIANTS} floor stamps`);
    }
    const g = new Draw(seedFor(skin, 'floor', variant));
    skin.floors[variant](g);
    return svgDocument(g.parts);
}

/** SVG text of (skin module, family, mask-or-variant). */
function renderTile(skin, family, key) {
    switch (family) {
        case 'wall': return renderWall(skin, key);
        case 'water': return renderWater(skin, key);
        case 'floor': return renderFloor(skin, key);
        default: throw new Error(`unknown family "${family}"`);
    }
}

// ---------------------------------------------------------------------------
// Tileset entries
// ---------------------------------------------------------------------------

function propertyList(values) {
    const out = [];
    for (const [name, value] of Object.entries(values)) {
        if (value === undefined || value === null) continue;
        const type = typeof value === 'boolean' ? 'bool'
            : typeof value === 'number' ? (Number.isInteger(value) ? 'int' : 'float')
            : 'string';
        out.push({ name, type, value });
    }
    out.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
    return out;
}

/**
 * The maps/terrain.tsj entry for one skinned tile. Wall / water tiles carry
 * their base tile's game properties plus `skin` (and `edges` on a variant);
 * floor tiles are air (tileId 0) plus `skin` and `variant`.
 */
function tilesetEntry(skinIndexValue, skin, family, key) {
    const name = className(skinIndexValue, family, key);
    const art = `tiles/${name}.svg`;
    let custom;
    if (family === 'wall') {
        custom = {
            tileId: TILE_WALL, solid: true, water: false, style: 'wall',
            color: skin.wall.fill, builtin: false, skin: skin.name,
            edges: key === 0 ? undefined : maskName(key), textureSvg: art,
        };
    } else if (family === 'water') {
        custom = {
            tileId: TILE_WATER, solid: false, water: true, style: 'water',
            color: skin.water.fill, borderColor: skin.water.band, builtin: false, skin: skin.name,
            edges: key === 0 ? undefined : maskName(key), textureSvg: art,
        };
    } else {
        custom = {
            tileId: TILE_AIR, solid: false, water: false, style: 'flat',
            color: '#00000000', builtin: false, skin: skin.name, variant: key, textureSvg: art,
        };
    }
    return {
        id: localId(skinIndexValue, family, key),
        class: name,
        image: art,
        imagewidth: TILE,
        imageheight: TILE,
        properties: propertyList(custom),
    };
}

/** Every (family, key) a skin >= 1 renders, in id order. */
function skinTiles() {
    const out = [];
    for (let mask = 0; mask <= 15; mask++) out.push({ family: 'wall', key: mask });
    for (let mask = 0; mask <= 15; mask++) out.push({ family: 'water', key: mask });
    for (let v = 0; v < FLOOR_VARIANTS; v++) out.push({ family: 'floor', key: v });
    return out;
}

module.exports = {
    TILE, TILE_AIR, TILE_WALL, TILE_WATER, SIDES,
    SKIN_NAMES, SKIN_COUNT, SKIN_PALETTE, FAMILIES, FLOOR_VARIANTS,
    DEFAULT_TILE_COUNT, SKIN_BLOCK, TILE_COUNT, BUILTIN_CLASSES,
    maskName, maskOf, skinIndex, familyOfTileId, tileIdOfFamily,
    className, localId, skinFirstId, expectedLayout,
    fmt, pt, attrText, svgDocument, hashString, makeRng,
    CORNER_RUN, withCornerRuns, teethProfile, waveProfile, scallopProfile, buildProfile,
    reverseProfile, orient, bandFill, bandFills, chainedOutline, rimOutline, strokePath,
    Draw, smoothClosedPath,
    renderWall, renderWater, renderFloor, renderTile,
    propertyList, tilesetEntry, skinTiles,
};
