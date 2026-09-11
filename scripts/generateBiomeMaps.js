#!/usr/bin/env node
'use strict';
/**
 * Generates the 45 biome maps -- `maps/<biome>_<n>.tmj`, five per biome -- and
 * rewrites `maps/maps.json` so the server and client load them. It also owns
 * the two things the overworld needs to reach them: a mob REGION per section
 * and an entrance pad next to each biome's door (`--world-regions`,
 * `--world-entrances`).
 *
 * Every map is a full Tiled map in exactly the shape of `maps/sewers.tmj`:
 * a `background` ground layer, a `terrain` tile layer, and the three object
 * layers (`spawns`, `player_spawns`, `teleporters`) that map_elements.cpp
 * reads. Terrain is written with the biome's BASE wall and water tiles and,
 * for a biome that has a tile skin (`skin` in BIOMES: sewers, computer,
 * unknown), its floor decorations: `wall_<skin>`, `water_<skin>`,
 * `floor_<skin>_<v>`. A biome without a skin (garden, desert, hel, ocean,
 * ant_hell, jungle) is painted with the default `wall` and `water` tiles and
 * gets no floor decorations. The edge variants are applied afterwards by
 * `scripts/edgeTiles.js --apply`, which `npm run build:map` does for every
 * map the manifest names.
 *
 * Nothing here knows a gid. Every tile is looked up BY CLASS NAME in
 * maps/terrain.tsj at run time, the ground tileset's firstgid is the terrain
 * tileset's tilecount + 1, and a class the generator needs but the tileset
 * lacks is a hard error. `--tileset <file>` points the lookup at another
 * tileset (a stub with the contract's layout, say) while the real one is in
 * flux.
 *
 * The output is deterministic. Every random decision comes from a mulberry32
 * stream seeded from the map's id, so running the script twice writes the
 * same bytes twice, and a change to one biome's generator leaves the other
 * eight biomes' files untouched.
 *
 * THE DENSITY CONTRACT, which check() holds every file on disk to:
 *   - open fraction (tileId 0 incl. floor decorations, plus sand) 40%..62%;
 *     ocean: water <= 55% and land >= 40%; wall+water >= 30% everywhere
 *   - at least 6 rooms, no fully open axis-aligned square wider than 9,
 *     every open cell reachable from the door
 *   - the door on a 3x3 open block; pads on open cells two tiles from
 *     anything solid, the "next" pad far from the door
 *   - every wall is the biome's wall_<skin> (plain `wall` for a skinless
 *     biome), every water its water_<skin> (plain `water`); a skinned biome
 *     has floor decorations on 6..14% of the open cells, a skinless one none
 *   - one whole-map mob region; tier bands over >= 85% of the open cells,
 *     nothing >= rare over the door; every door pickable = false
 *   - every teleporter target resolvable (world doors included); unique
 *     object ids; nextobjectid = max id + 1
 *
 * Usage:
 *   node scripts/generateBiomeMaps.js                   write the maps and the manifest, then verify
 *   node scripts/generateBiomeMaps.js --check           verify what is on disk without writing
 *   node scripts/generateBiomeMaps.js --show            also print an ASCII thumbnail of every map
 *   node scripts/generateBiomeMaps.js --thumb garden_1  print one map's thumbnail (repeatable; --scale 1 for full size)
 *   node scripts/generateBiomeMaps.js --world-regions   add the missing per-section mob regions to maps/world.tmj
 *   node scripts/generateBiomeMaps.js --world-entrances add/replace the nine entrance_<biome> pads in maps/world.tmj
 *   node scripts/generateBiomeMaps.js --tileset FILE    read tile classes from FILE instead of maps/terrain.tsj
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const MAPS_DIR = path.join(ROOT, 'maps');

const TILE_SIZE = 300;
const TERRAIN_FIRST_GID = 1;
const MAPS_PER_BIOME = 5;
/** World units per overworld section, the 3x3 grid the mob regions follow. */
const SECTION_SIZE = 20000;

/** Cell kinds inside the generator; the tileset decides what gid each becomes. */
const T = { OPEN: 0, WALL: 1, WATER: 2, SAND: 3, STONE: 4 };
const walkable = id => id === T.OPEN || id === T.SAND;

/** The density contract's numbers, in one place. */
const RULES = {
    minOpen: 0.40, maxOpen: 0.62,
    minSolidOrWater: 0.30,
    oceanMaxWater: 0.55, oceanMinLand: 0.40,
    minRooms: 6,
    maxOpenSquare: 9,
    minFloor: 0.06, maxFloor: 0.14,
    minBandCoverage: 0.85,
    nextPadMinDistance: 0.5,      // of the map's farthest walk from the door
    entranceMaxTiles: 8,
    entranceClearance: 2,         // wanted; a door in a narrow lane gets the best on offer
    entranceMinPadGap: 4,
};
/** What the generator aims for, comfortably inside the rules; a biome's `open` window narrows it. */
/** Biomes with a tile skin of their own in maps/terrain.tsj (scripts/lib/tileArt.js SKIN_NAMES). */
const SKINNED = new Set(['sewers', 'computer', 'unknown']);
const TARGET = { minOpen: 0.44, maxOpen: 0.58, oceanMaxWater: 0.50, floor: 0.10 };

/**
 * The nine biomes, in the order maps.json lists them and the order the
 * spawn picker shows their tabs. `mobs` is the distribution every band on
 * the map falls back to, `section` the overworld third the biome owns (its
 * mob region), and `size[n]` the map's tile dimensions -- deliberately not
 * all square and not all the same, so a map's shape is one more thing that
 * tells you where you are. The ground id comes from maps/ground.tsj by
 * class name and the tile gids from maps/terrain.tsj, never from here.
 *
 * `skin` names the biome's tile family (`wall_<skin>`, `water_<skin>`,
 * `floor_<skin>_<v>` in maps/terrain.tsj) and `floors` its three stamps'
 * weights and placement preferences. A biome without a `skin` is painted
 * with the default `wall` / `water` tiles and has no floor decorations.
 *
 * `unknown` has no mob group of its own in src/mobs.json: its maps spawn a
 * half-and-half mix of the computer and hel groups, and its defaultMobGroup
 * is `computer` so a band that names nothing still spawns something.
 *
 * `fill` and `carve` are the biome's answers to "this field is too open"
 * and "this map is too closed": what gets planted in a meadow is a boulder
 * or a pond, in a sewer a brick pillar or a sludge pool. `open` is the
 * window of open fraction the density pass steers the biome into.
 */
const BIOMES = [
    { id: 'garden',   name: 'Garden',   section: 0, color: '#FF00BE4F', mobs: 'garden',   defaultMobGroup: 'garden',
      style: 'meadow',      sizes: [[60, 60], [72, 56], [80, 80], [96, 72], [110, 110]], open: [0.46, 0.56],
      fill: fillGarden, carve: carveBlob },
    { id: 'desert',   name: 'Desert',   section: 1, color: '#FFFFFF9C', mobs: 'desert',   defaultMobGroup: 'desert',
      style: 'mesa',        sizes: [[70, 50], [80, 80], [90, 64], [100, 100], [120, 90]], open: [0.45, 0.55],
      fill: fillDesert, carve: carveBlob },
    { id: 'hel',      name: 'Hel',      section: 2, color: '#FFFF0000', mobs: 'hel',      defaultMobGroup: 'hel',
      style: 'caves',       sizes: [[56, 56], [64, 80], [80, 64], [96, 96], [112, 84]], open: [0.46, 0.56],
      fill: fillHel, carve: carveBlob },
    { id: 'ocean',    name: 'Ocean',    section: 3, color: '#FFC8FFFA', mobs: 'ocean',    defaultMobGroup: 'ocean',
      style: 'archipelago', sizes: [[64, 64], [84, 60], [90, 90], [100, 80], [120, 120]], ocean: true, open: [0.42, 0.50],
      fill: fillOcean, carve: carveIsland },
    { id: 'ant_hell', name: 'Ant Hell', section: 4, color: '#FFC9904F', mobs: 'ant_hell', defaultMobGroup: 'ant_hell',
      style: 'tunnels',     sizes: [[48, 48], [60, 44], [64, 64], [80, 60], [90, 90]], open: [0.42, 0.50],
      fill: fillAntHell, carve: carveBlob },
    { id: 'jungle',   name: 'Jungle',   section: 5, color: '#FF00FF00', mobs: 'jungle',   defaultMobGroup: 'jungle',
      style: 'thicket',     sizes: [[52, 52], [64, 64], [72, 90], [96, 80], [108, 108]], open: [0.45, 0.55],
      fill: fillJungle, carve: carveBlob },
    { id: 'sewers',   name: 'Sewers',   section: 6, color: '#FF803F02', mobs: 'sewers',   defaultMobGroup: 'sewers',
      style: 'lanes',       sizes: [[50, 40], [60, 60], [80, 56], [90, 90], [110, 70]], open: [0.44, 0.56],
      fill: fillSewers, carve: carveRect, skin: 'sewers', floors: [{ w: 2, near: 'open' }, { w: 3, near: 'water' }, { w: 3, near: 'wall' }] },
    { id: 'computer', name: 'Computer', section: 7, color: '#FF60FF95', mobs: 'computer', defaultMobGroup: 'computer',
      style: 'circuit',     sizes: [[44, 44], [60, 48], [72, 72], [84, 60], [100, 100]], open: [0.43, 0.53],
      fill: fillComputer, carve: carveRect, skin: 'computer', floors: [{ w: 4, near: 'open' }, { w: 2, near: 'wall' }, { w: 2, near: 'open' }] },
    { id: 'unknown',  name: 'Unknown',  section: 8, color: '#FF9A8FD0', mobs: 'computer 50% hel 50%', defaultMobGroup: 'computer',
      style: 'rings',       sizes: [[60, 60], [70, 70], [84, 84], [100, 100], [120, 120]], open: [0.45, 0.55],
      fill: fillUnknown, carve: carveBlob, skin: 'unknown', floors: [{ w: 3, near: 'open' }, { w: 2, near: 'open' }, { w: 3, near: 'wall' }] },
];

const TIERS = ['common', 'uncommon', 'rare', 'epic', 'legendary', 'mythic'];
/** Cumulative share of walkable tiles (by distance from the door) each tier ends at. */
const TIER_QUANTILES = [0.22, 0.42, 0.62, 0.82, 1.0];
/** The mythic band on maps 4 and 5: every block this far along the longest walk. */
const MYTHIC_FRACTION = 0.92;
/** Tier bands are laid out on blocks this many tiles square. */
const BAND_BLOCK = 4;

// ---------------------------------------------------------------------------
// Randomness
// ---------------------------------------------------------------------------

function hashString(text) {
    let h = 2166136261 >>> 0;
    for (let i = 0; i < text.length; i++) {
        h ^= text.charCodeAt(i);
        h = Math.imul(h, 16777619) >>> 0;
    }
    return h >>> 0;
}

function mulberry32(seed) {
    let a = seed >>> 0;
    const next = () => {
        a = (a + 0x6D2B79F5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
    return {
        next,
        int: (lo, hi) => lo + Math.floor(next() * (hi - lo + 1)),   // inclusive
        chance: p => next() < p,
        pick: list => list[Math.floor(next() * list.length)],
    };
}

// ---------------------------------------------------------------------------
// The tileset: every gid the generator writes is looked up by class name
// ---------------------------------------------------------------------------

function readProps(node) {
    const out = {};
    for (const p of node && Array.isArray(node.properties) ? node.properties : []) out[p.name] = p.value;
    return out;
}

/**
 * maps/terrain.tsj as a class -> tile table. A tile record carries what the
 * generator and the checker need to know about a gid: its game tileId, its
 * skin, its edge mask (edge variants) and its floor variant (decorations).
 */
function loadTileset(file) {
    const ts = JSON.parse(fs.readFileSync(file, 'utf8'));
    if (ts.type !== 'tileset' || !Array.isArray(ts.tiles)) throw new Error(`${file} is not a Tiled tileset`);
    const byClass = new Map();
    const byGid = new Map([[0, { gid: 0, cls: '', tileId: 0, skin: '', edges: '', variant: -1, solid: false, water: false }]]);
    let maxId = -1;
    for (const tile of ts.tiles) {
        const p = readProps(tile);
        const cls = tile.class || tile.type || '';
        const rec = {
            gid: TERRAIN_FIRST_GID + tile.id, cls,
            tileId: p.tileId !== undefined ? p.tileId | 0 : tile.id | 0,
            skin: typeof p.skin === 'string' ? p.skin : '',
            edges: typeof p.edges === 'string' ? p.edges : '',
            variant: p.variant !== undefined ? p.variant | 0 : -1,
            solid: !!p.solid, water: !!p.water,
        };
        if (cls) {
            if (byClass.has(cls)) throw new Error(`${file}: two tiles are class "${cls}"`);
            byClass.set(cls, rec);
        }
        byGid.set(rec.gid, rec);
        maxId = Math.max(maxId, tile.id);
    }
    if (ts.tilecount !== ts.tiles.length || maxId + 1 !== ts.tilecount) {
        throw new Error(`${file}: tilecount ${ts.tilecount} does not match its ${ts.tiles.length} tiles (max id ${maxId})`);
    }
    const need = cls => {
        const rec = byClass.get(cls);
        if (!rec) throw new Error(`${path.relative(ROOT, file)} has no tile of class "${cls}", which the generator needs`);
        return rec;
    };
    // The plain crossings every biome shares.
    const sand = need('bridge'), stone = need('sewage');
    if (sand.tileId !== 3 || sand.solid) throw new Error(`${file}: "bridge" must be the walkable tileId 3`);
    if (stone.tileId !== 4 || !stone.solid) throw new Error(`${file}: "sewage" must be the solid tileId 4`);
    return { file, tilecount: ts.tilecount, groundFirstGid: ts.tilecount + 1, byGid, byClass, need, sand, stone };
}

/**
 * The gids one biome paints with, each checked to be the tile it claims. A
 * skinned biome paints wall_<skin> / water_<skin> and its three floor stamps;
 * a skinless one the default `wall` / `water` and no floors at all.
 */
function biomeGids(ts, biome) {
    const skin = biome.skin || '';
    if (!!skin !== SKINNED.has(biome.id) || (skin && skin !== biome.id)) throw new Error(`${biome.id}: skin "${skin}" disagrees with SKINNED`);
    const wallClass = skin ? `wall_${skin}` : 'wall', waterClass = skin ? `water_${skin}` : 'water';
    const wall = ts.need(wallClass), water = ts.need(waterClass);
    if (wall.tileId !== 1 || !wall.solid || wall.skin !== skin || wall.edges) throw new Error(`${wallClass} is not a solid tileId-1 base tile of skin "${skin}"`);
    if (water.tileId !== 2 || !water.water || water.skin !== skin || water.edges) throw new Error(`${waterClass} is not a tileId-2 water base tile of skin "${skin}"`);
    if (!skin) {
        if (biome.floors) throw new Error(`${biome.id} has no skin and so cannot have floor stamps`);
        return { wall: wall.gid, water: water.gid, floors: [], sand: ts.sand.gid, stone: ts.stone.gid };
    }
    if (!Array.isArray(biome.floors) || biome.floors.length !== 3) throw new Error(`${biome.id} must list three floor stamps`);
    const floors = [0, 1, 2].map(v => {
        const rec = ts.need(`floor_${skin}_${v}`);
        if (rec.tileId !== 0 || rec.solid || rec.water || rec.skin !== skin || rec.variant !== v) throw new Error(`floor_${skin}_${v} is not an open tileId-0 tile of skin ${skin}, variant ${v}`);
        return rec.gid;
    });
    return { wall: wall.gid, water: water.gid, floors, sand: ts.sand.gid, stone: ts.stone.gid };
}

/** maps/ground.tsj: biome id -> ground local id, by class name. */
function loadGround(file) {
    const ts = JSON.parse(fs.readFileSync(file, 'utf8'));
    const ids = new Map();
    for (const tile of ts.tiles || []) {
        const p = readProps(tile);
        const cls = tile.class || tile.type || '';
        if (cls && p.groundId !== undefined) ids.set(cls, p.groundId | 0);
    }
    return {
        file, tilecount: ts.tilecount,
        idOf: biome => {
            if (!ids.has(biome)) throw new Error(`${path.relative(ROOT, file)} has no ground tile of class "${biome}"`);
            return ids.get(biome);
        },
    };
}

// ---------------------------------------------------------------------------
// Grid helpers
// ---------------------------------------------------------------------------

class Grid {
    constructor(w, h, fill) {
        this.w = w;
        this.h = h;
        this.cells = new Uint8Array(w * h).fill(fill);
        /** Cells no generator step may touch again: the door and the pads. */
        this.locked = new Uint8Array(w * h);
        /** Bumps on every write that changed a cell; the repair loops watch it. */
        this.changes = 0;
    }
    inside(x, y) { return x >= 0 && y >= 0 && x < this.w && y < this.h; }
    get(x, y) { return this.inside(x, y) ? this.cells[y * this.w + x] : T.WALL; }
    set(x, y, v) {
        if (!this.inside(x, y)) return;
        const i = y * this.w + x;
        if (this.locked[i] || this.cells[i] === v) return;
        this.cells[i] = v;
        this.changes++;
    }
    fillRect(x0, y0, w, h, v) {
        for (let y = y0; y < y0 + h; y++) for (let x = x0; x < x0 + w; x++) this.set(x, y, v);
    }
    lockRect(x0, y0, w, h) {
        for (let y = y0; y < y0 + h; y++) for (let x = x0; x < x0 + w; x++) if (this.inside(x, y)) this.locked[y * this.w + x] = 1;
    }
    /** Walls the outer ring so nothing is drawn against the void. */
    border(v, thickness = 1) {
        for (let t = 0; t < thickness; t++) {
            for (let x = 0; x < this.w; x++) { this.set(x, t, v); this.set(x, this.h - 1 - t, v); }
            for (let y = 0; y < this.h; y++) { this.set(t, y, v); this.set(this.w - 1 - t, y, v); }
        }
    }
    count(pred) {
        let n = 0;
        for (const c of this.cells) if (pred(c)) n++;
        return n;
    }
    /** Every cell satisfying `pred(value, x, y)`, in row order. */
    where(pred) {
        const out = [];
        for (let i = 0; i < this.cells.length; i++) {
            const x = i % this.w, y = (i / this.w) | 0;
            if (pred(this.cells[i], x, y)) out.push({ x, y });
        }
        return out;
    }
    /** True when a 4-neighbour of (x,y) satisfies pred. */
    touches(x, y, pred) {
        return pred(this.get(x + 1, y)) || pred(this.get(x - 1, y)) || pred(this.get(x, y + 1)) || pred(this.get(x, y - 1));
    }
}

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

/** `only` as a predicate: a cell kind, a list of kinds, or nothing. */
function onlyPred(only) {
    if (only === undefined) return null;
    if (Array.isArray(only)) return v => only.includes(v);
    return v => v === only;
}

/** A roughly round blob: every cell within a noisy radius of the centre. */
function blob(grid, rng, cx, cy, radius, value, options = {}) {
    const wobble = options.wobble === undefined ? 0.35 : options.wobble;
    const only = onlyPred(options.only);
    const phase = rng.next() * Math.PI * 2;
    const lobes = rng.int(2, 4);
    const r2 = radius * (1 + wobble) + 1;
    for (let y = Math.floor(cy - r2); y <= Math.ceil(cy + r2); y++) {
        for (let x = Math.floor(cx - r2); x <= Math.ceil(cx + r2); x++) {
            if (!grid.inside(x, y)) continue;
            const dx = x - cx, dy = y - cy;
            const angle = Math.atan2(dy, dx);
            const edge = radius * (1 + wobble * Math.sin(lobes * angle + phase));
            if (dx * dx + dy * dy <= edge * edge) {
                if (only && !only(grid.get(x, y))) continue;
                grid.set(x, y, value);
            }
        }
    }
}

/** A wandering line from (x,y) heading `angle`, `length` steps long, `width` cells thick. */
function meander(grid, rng, x, y, angle, length, width, value, options = {}) {
    const turn = options.turn === undefined ? 0.5 : options.turn;
    const only = onlyPred(options.only);
    for (let i = 0; i < length; i++) {
        const ix = Math.round(x), iy = Math.round(y);
        for (let dy = 0; dy < width; dy++) {
            for (let dx = 0; dx < width; dx++) {
                const px = ix + dx - (width >> 1), py = iy + dy - (width >> 1);
                if (!grid.inside(px, py)) continue;
                if (only && !only(grid.get(px, py))) continue;
                grid.set(px, py, value);
            }
        }
        angle += (rng.next() - 0.5) * turn;
        x += Math.cos(angle);
        y += Math.sin(angle);
        if (!grid.inside(Math.round(x), Math.round(y))) break;
    }
}

/**
 * Carves one cell of a corridor, `width` thick, never on the border. A wall
 * becomes `value` (open ground); water becomes the crossing (a bridge, so a
 * corridor over a pond or a strait reads as one); sand and open stay as
 * they are.
 */
function carveCell(grid, x, y, width, value, crossing) {
    for (let dy = 0; dy < width; dy++) {
        for (let dx = 0; dx < width; dx++) {
            const px = x + dx - (width >> 1), py = y + dy - (width >> 1);
            if (px <= 0 || py <= 0 || px >= grid.w - 1 || py >= grid.h - 1) continue;
            const was = grid.get(px, py);
            if (was === T.WATER) { if (crossing !== undefined) grid.set(px, py, crossing); }
            else if (!walkable(was)) grid.set(px, py, value);
        }
    }
}

/** An L-shaped corridor between two cells. */
function carveL(grid, rng, x0, y0, x1, y1, width, crossing = T.SAND) {
    const horizontalFirst = rng.chance(0.5);
    let x = x0, y = y0;
    const walkX = () => { while (x !== x1) { x += Math.sign(x1 - x); carveCell(grid, x, y, width, T.OPEN, crossing); } };
    const walkY = () => { while (y !== y1) { y += Math.sign(y1 - y); carveCell(grid, x, y, width, T.OPEN, crossing); } };
    carveCell(grid, x, y, width, T.OPEN, crossing);
    if (horizontalFirst) { walkX(); walkY(); } else { walkY(); walkX(); }
}

/** A winding corridor between two cells: heads for the target with a drifting heading. */
function windingPath(grid, rng, x0, y0, x1, y1, width, options = {}) {
    const wobble = options.wobble === undefined ? 0.6 : options.wobble;
    const crossing = options.crossing === undefined ? T.SAND : options.crossing;
    let x = x0, y = y0, drift = 0;
    const budget = Math.hypot(x1 - x0, y1 - y0) * 3 + 10;
    for (let step = 0; step < budget; step++) {
        carveCell(grid, Math.round(x), Math.round(y), width, T.OPEN, crossing);
        const dx = x1 - x, dy = y1 - y;
        if (Math.hypot(dx, dy) < 0.8) break;
        drift = drift * 0.8 + (rng.next() - 0.5) * wobble;
        const angle = Math.atan2(dy, dx) + drift;
        x += Math.cos(angle);
        y += Math.sin(angle);
    }
    carveCell(grid, x1, y1, width, T.OPEN, crossing);
}

/** A pond: water with a rim of sand where it meets open ground. */
function pond(grid, rng, cx, cy, r, options = {}) {
    blob(grid, rng, cx, cy, r + 1.2, T.SAND, { wobble: 0.3, only: T.OPEN });
    blob(grid, rng, cx, cy, r, T.WATER, { wobble: 0.3, only: options.only || [T.OPEN, T.SAND] });
}

/**
 * A jittered lattice of points with margins, and its 4-neighbour edges as a
 * random spanning tree plus a share of the rest: the skeleton every
 * room-and-corridor biome hangs off.
 */
function lattice(grid, rng, pitch, jitter, margin) {
    const cols = Math.max(2, Math.round((grid.w - 2 * margin) / pitch) + 1);
    const rows = Math.max(2, Math.round((grid.h - 2 * margin) / pitch) + 1);
    const sx = (grid.w - 1 - 2 * margin) / (cols - 1), sy = (grid.h - 1 - 2 * margin) / (rows - 1);
    const nodes = [];
    for (let j = 0; j < rows; j++) {
        for (let i = 0; i < cols; i++) {
            nodes.push({
                i, j, skip: false,
                x: clamp(Math.round(margin + i * sx + (rng.next() - 0.5) * 2 * jitter), margin, grid.w - 1 - margin),
                y: clamp(Math.round(margin + j * sy + (rng.next() - 0.5) * 2 * jitter), margin, grid.h - 1 - margin),
            });
        }
    }
    return { nodes, cols, rows, pitch: Math.min(sx, sy) };
}

function latticeEdges(lat, rng, extra) {
    const alive = lat.nodes.filter(n => !n.skip);
    const at = new Map(alive.map(n => [`${n.i},${n.j}`, n]));
    alive.forEach((n, k) => { n.k = k; });
    const candidates = [];
    for (const a of alive) {
        for (const [di, dj] of [[1, 0], [0, 1]]) {
            const b = at.get(`${a.i + di},${a.j + dj}`);
            if (b) candidates.push({ a, b, w: rng.next() });
        }
    }
    candidates.sort((p, q) => p.w - q.w);
    const parent = alive.map((_, k) => k);
    const find = k => { while (parent[k] !== k) { parent[k] = parent[parent[k]]; k = parent[k]; } return k; };
    const edges = [];
    for (const { a, b } of candidates) {
        const ra = find(a.k), rb = find(b.k);
        if (ra !== rb) { parent[ra] = rb; edges.push([a, b]); }
        else if (rng.chance(extra)) edges.push([a, b]);
    }
    return edges;
}

/** 4-connected flood fill over walkable cells; returns a component id per cell (-1 = not walkable). */
function components(grid) {
    const ids = new Int32Array(grid.w * grid.h).fill(-1);
    const sizes = [];
    const stack = [];
    for (let start = 0; start < ids.length; start++) {
        if (ids[start] !== -1 || !walkable(grid.cells[start])) continue;
        const id = sizes.length;
        let size = 0;
        stack.push(start);
        ids[start] = id;
        while (stack.length) {
            const i = stack.pop();
            size++;
            const x = i % grid.w, y = (i / grid.w) | 0;
            for (const [nx, ny] of [[x + 1, y], [x - 1, y], [x, y + 1], [x, y - 1]]) {
                if (!grid.inside(nx, ny)) continue;
                const j = ny * grid.w + nx;
                if (ids[j] === -1 && walkable(grid.cells[j])) { ids[j] = id; stack.push(j); }
            }
        }
        sizes.push(size);
    }
    return { ids, sizes };
}

/** BFS distance in tiles over walkable cells from one cell; -1 where unreachable. */
function distances(grid, sx, sy) {
    const dist = new Int32Array(grid.w * grid.h).fill(-1);
    const queue = [sy * grid.w + sx];
    dist[queue[0]] = 0;
    for (let head = 0; head < queue.length; head++) {
        const i = queue[head];
        const x = i % grid.w, y = (i / grid.w) | 0;
        for (const [nx, ny] of [[x + 1, y], [x - 1, y], [x, y + 1], [x, y - 1]]) {
            if (!grid.inside(nx, ny)) continue;
            const j = ny * grid.w + nx;
            if (dist[j] === -1 && walkable(grid.cells[j])) { dist[j] = dist[i] + 1; queue.push(j); }
        }
    }
    return dist;
}

/**
 * Makes every walkable cell reachable from the door.
 *
 * Pockets too small to be worth a corridor are filled in; every other
 * disconnected region gets an L-shaped corridor to the nearest cell of the
 * door's region. Repeats until one region is left, which it always is: each
 * pass merges at least one region and never splits one.
 */
function connect(grid, rng, doorX, doorY, options = {}) {
    const width = options.width || 2;
    const minRegion = options.minRegion === undefined ? 8 : options.minRegion;
    const fillWith = options.fillWith === undefined ? T.WALL : options.fillWith;
    const crossing = options.crossing === undefined ? T.SAND : options.crossing;
    for (let pass = 0; pass < 600; pass++) {
        const { ids, sizes } = components(grid);
        const main = ids[doorY * grid.w + doorX];
        if (main < 0) throw new Error('the door is not on a walkable cell');
        if (sizes.length === 1) return;

        // Fill the crumbs first so the corridors only go somewhere worth going.
        let filled = false;
        for (let id = 0; id < sizes.length; id++) {
            if (id === main || sizes[id] >= minRegion) continue;
            for (let i = 0; i < ids.length; i++) {
                if (ids[i] === id && !grid.locked[i]) { grid.set(i % grid.w, (i / grid.w) | 0, fillWith); filled = true; }
            }
        }
        if (filled) continue;

        // The region whose cell is nearest (Manhattan) to the main region.
        const mainCells = [];
        for (let i = 0; i < ids.length; i++) if (ids[i] === main) mainCells.push(i);
        let best = null;
        for (let i = 0; i < ids.length; i++) {
            if (ids[i] < 0 || ids[i] === main) continue;
            const x = i % grid.w, y = (i / grid.w) | 0;
            for (const j of mainCells) {
                const d = Math.abs(x - j % grid.w) + Math.abs(y - ((j / grid.w) | 0));
                if (best === null || d < best.d) best = { d, from: [x, y], to: [j % grid.w, (j / grid.w) | 0] };
            }
        }
        carveL(grid, rng, best.from[0], best.from[1], best.to[0], best.to[1], width, crossing);
    }
    throw new Error('could not connect the map');
}

/** Cellular-automata caves: random fill, then smoothing. */
function automata(grid, rng, fill, iterations, birth = 5, survive = 4) {
    for (let i = 0; i < grid.cells.length; i++) grid.cells[i] = rng.chance(fill) ? T.WALL : T.OPEN;
    grid.border(T.WALL);
    for (let it = 0; it < iterations; it++) {
        const next = new Uint8Array(grid.cells);
        for (let y = 1; y < grid.h - 1; y++) {
            for (let x = 1; x < grid.w - 1; x++) {
                let walls = 0;
                for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
                    if ((dx || dy) && grid.get(x + dx, y + dy) === T.WALL) walls++;
                }
                const isWall = grid.get(x, y) === T.WALL;
                next[y * grid.w + x] = isWall ? (walls >= survive ? T.WALL : T.OPEN) : (walls >= birth ? T.WALL : T.OPEN);
            }
        }
        grid.cells = next;
    }
}

/** Chips single cells off every wall face: mesa rims, cave walls. */
function crumble(grid, rng, chance) {
    const snapshot = Uint8Array.from(grid.cells);
    for (let y = 1; y < grid.h - 1; y++) {
        for (let x = 1; x < grid.w - 1; x++) {
            if (snapshot[y * grid.w + x] !== T.WALL) continue;
            const open = [[1, 0], [-1, 0], [0, 1], [0, -1]].some(([dx, dy]) => snapshot[(y + dy) * grid.w + x + dx] === T.OPEN);
            if (open && rng.chance(chance)) grid.set(x, y, T.OPEN);
        }
    }
}

// ---------------------------------------------------------------------------
// Layout styles
// ---------------------------------------------------------------------------

/** Garden: meadows joined by winding paths, broken by boulder clusters, hedges and ponds. */
function meadow(grid, rng, n) {
    grid.cells.fill(T.WALL);
    const lat = lattice(grid, rng, 11 + (n >= 4 ? 1 : 0), 3, 5);
    for (const node of lat.nodes) {
        node.r = 3 + rng.next() * 2.5;
        blob(grid, rng, node.x, node.y, node.r, T.OPEN, { wobble: 0.35 });
    }
    for (const [a, b] of latticeEdges(lat, rng, 0.35)) {
        windingPath(grid, rng, a.x, a.y, b.x, b.y, rng.int(2, 3), { wobble: 0.5 });
    }
    // Ponds in a third of the meadows, boulder clusters in most, and a hedge
    // (a straight run of wall) cutting across the bigger ones.
    for (const node of lat.nodes) {
        if (rng.chance(0.3)) pond(grid, rng, node.x + rng.int(-2, 2), node.y + rng.int(-2, 2), 1.5 + rng.next() * 1.5);
        const boulders = rng.int(1, 3);
        for (let k = 0; k < boulders; k++) {
            blob(grid, rng, node.x + (rng.next() - 0.5) * node.r * 1.8, node.y + (rng.next() - 0.5) * node.r * 1.8,
                 0.7 + rng.next(), T.WALL, { wobble: 0.5, only: T.OPEN });
        }
        if (node.r > 4 && rng.chance(0.5)) {
            const horizontal = rng.chance(0.5), len = rng.int(3, 6);
            const x0 = node.x - (horizontal ? len >> 1 : 0) + rng.int(-1, 1), y0 = node.y - (horizontal ? 0 : len >> 1) + rng.int(-1, 1);
            for (let k = 0; k < len; k++) {
                const x = x0 + (horizontal ? k : 0), y = y0 + (horizontal ? 0 : k);
                if (grid.get(x, y) === T.OPEN) grid.set(x, y, T.WALL);
            }
        }
    }
    grid.border(T.WALL);
}

/** Desert: sandstone mesas with canyon corridors between the flats, oases and lone rocks. */
function mesa(grid, rng, n) {
    grid.cells.fill(T.WALL);
    const lat = lattice(grid, rng, 10, 2, 5);
    for (const node of lat.nodes) {
        if (rng.chance(0.12)) { node.skip = true; continue; }
        blob(grid, rng, node.x, node.y, 2.5 + rng.next() * 1.8, T.OPEN, { wobble: 0.18 });
    }
    for (const [a, b] of latticeEdges(lat, rng, 0.3)) carveL(grid, rng, a.x, a.y, b.x, b.y, rng.int(2, 3));
    crumble(grid, rng, 0.12);
    for (const node of lat.nodes) {
        if (node.skip) continue;
        if (rng.chance(0.28)) pond(grid, rng, node.x, node.y, 1.4 + rng.next() * 1.2);
        else if (rng.chance(0.5)) blob(grid, rng, node.x + rng.int(-2, 2), node.y + rng.int(-2, 2), 0.6 + rng.next() * 0.8, T.WALL, { wobble: 0.4, only: T.OPEN });
    }
    const rocks = Math.round(grid.w * grid.h / 350);
    for (let i = 0; i < rocks; i++) {
        const x = rng.int(2, grid.w - 3), y = rng.int(2, grid.h - 3);
        if (grid.get(x, y) === T.OPEN) grid.set(x, y, T.WALL);
    }
    grid.border(T.WALL);
}

/** Hel: basalt caves cut by rivers of lava, with lava pools where the floor has sagged. */
function caves(grid, rng, n) {
    automata(grid, rng, 0.46, 5);
    const rivers = 2 + Math.floor(n / 2);
    for (let i = 0; i < rivers; i++) {
        const fromTop = rng.chance(0.5);
        const x = fromTop ? rng.int(4, grid.w - 5) : 1;
        const y = fromTop ? 1 : rng.int(4, grid.h - 5);
        meander(grid, rng, x, y, fromTop ? Math.PI / 2 : 0, Math.max(grid.w, grid.h) * 1.6, 2, T.WATER, { turn: 0.35 });
    }
    const pools = Math.round(grid.w * grid.h / 900);
    for (let i = 0; i < pools; i++) {
        blob(grid, rng, rng.int(3, grid.w - 4), rng.int(3, grid.h - 4), 1 + rng.next() * 1.5, T.WATER, { only: T.OPEN });
    }
    grid.border(T.WALL);
}

/** Ocean: an archipelago -- big islands with coral-rock cores, sand beaches, reefs and causeways. */
function archipelago(grid, rng, n) {
    grid.cells.fill(T.WATER);
    const lat = lattice(grid, rng, 15 + n, 3, 7);
    let biggest = null;
    for (const node of lat.nodes) {
        if (rng.chance(0.12)) { node.skip = true; continue; }
        // Islands are the pitch's third or so across, so open sea stays
        // between them; the beach is a one-tile rim, the coral core sits
        // off-centre, and the big ones hold a lagoon.
        node.r = lat.pitch * (0.3 + rng.next() * 0.12);
        blob(grid, rng, node.x, node.y, node.r + 1, T.SAND, { wobble: 0.4 });
        blob(grid, rng, node.x, node.y, node.r, T.OPEN, { wobble: 0.4 });
        if (rng.chance(0.85)) {
            blob(grid, rng, node.x + rng.int(-2, 2), node.y + rng.int(-2, 2), node.r * (0.3 + rng.next() * 0.2), T.WALL, { wobble: 0.5, only: T.OPEN });
        }
        if (node.r > 5.5 && rng.chance(0.35)) pond(grid, rng, node.x + rng.int(-3, 3), node.y + rng.int(-3, 3), node.r * 0.2);
        if (!biggest || node.r > biggest.r) biggest = node;
    }
    for (const [a, b] of latticeEdges(lat, rng, 0.2)) carveL(grid, rng, a.x, a.y, b.x, b.y, rng.int(1, 2), T.SAND);
    const reefs = Math.round(grid.w * grid.h / 800);
    for (let i = 0; i < reefs; i++) {
        blob(grid, rng, rng.int(2, grid.w - 3), rng.int(2, grid.h - 3), 0.7 + rng.next() * 0.9, T.WALL, { only: T.WATER });
    }
    const bars = Math.round(grid.w * grid.h / 2500);
    for (let i = 0; i < bars; i++) {
        blob(grid, rng, rng.int(2, grid.w - 3), rng.int(2, grid.h - 3), 1 + rng.next(), T.SAND, { only: T.WATER });
    }
    grid.border(T.WATER);
    return [biggest.x, biggest.y];
}

/** Ant Hell: branching tunnels dug out from the nest entrance, with brood chambers, some flooded. */
function tunnels(grid, rng, n, door) {
    grid.cells.fill(T.WALL);
    const target = grid.w * grid.h * 0.36;
    const walkers = [{ x: door[0], y: door[1], dir: rng.int(0, 3) }];
    let dug = 0;
    for (let step = 0; step < 200000 && dug < target; step++) {
        const w = walkers[step % walkers.length];
        const width = rng.chance(0.3) ? 2 : 1;
        for (let oy = 0; oy < width; oy++) for (let ox = 0; ox < width; ox++) {
            if (w.x + ox > 0 && w.y + oy > 0 && w.x + ox < grid.w - 1 && w.y + oy < grid.h - 1 && grid.get(w.x + ox, w.y + oy) === T.WALL) {
                grid.set(w.x + ox, w.y + oy, T.OPEN);
                dug++;
            }
        }
        if (rng.chance(0.18)) w.dir = (w.dir + (rng.chance(0.5) ? 1 : 3)) % 4;
        if (rng.chance(0.015)) { blob(grid, rng, w.x, w.y, rng.int(2, 3) + rng.next(), T.OPEN, { wobble: 0.4 }); dug += 12; }
        if (walkers.length < 6 + n && rng.chance(0.01)) walkers.push({ x: w.x, y: w.y, dir: (w.dir + (rng.chance(0.5) ? 1 : 3)) % 4 });
        const [mx, my] = [[1, 0], [0, 1], [-1, 0], [0, -1]][w.dir];
        const nx = w.x + mx, ny = w.y + my;
        if (nx < 2 || ny < 2 || nx >= grid.w - 2 || ny >= grid.h - 2) { w.dir = (w.dir + 2) % 4; continue; }
        w.x = nx; w.y = ny;
    }
    // Brood chambers, and a few flooded ones.
    const chambers = Math.round(grid.w * grid.h / 600);
    for (let i = 0; i < chambers; i++) {
        const cx = rng.int(4, grid.w - 5), cy = rng.int(4, grid.h - 5);
        blob(grid, rng, cx, cy, rng.int(2, 3) + rng.next(), T.OPEN, { wobble: 0.4 });
        if (rng.chance(0.3)) blob(grid, rng, cx, cy, 1 + rng.next(), T.WATER, { only: T.OPEN });
    }
    grid.border(T.WALL);
}

/** Jungle: dense canopy with clearings, winding trails between them and streams across. */
function thicket(grid, rng, n) {
    automata(grid, rng, 0.53, 4, 5, 4);
    const lat = lattice(grid, rng, 12, 3, 5);
    for (const node of lat.nodes) {
        if (rng.chance(0.35)) { node.skip = true; continue; }
        blob(grid, rng, node.x, node.y, 2.5 + rng.next() * 1.5, T.OPEN, { wobble: 0.4 });
    }
    for (const [a, b] of latticeEdges(lat, rng, 0.3)) windingPath(grid, rng, a.x, a.y, b.x, b.y, rng.chance(0.3) ? 2 : 1, { wobble: 0.9 });
    const streams = 2 + Math.floor(n / 2);
    for (let i = 0; i < streams; i++) {
        const fromLeft = rng.chance(0.5);
        meander(grid, rng, fromLeft ? 1 : rng.int(3, grid.w - 4), fromLeft ? rng.int(3, grid.h - 4) : 1,
                fromLeft ? 0 : Math.PI / 2, Math.max(grid.w, grid.h) * 1.5, 1, T.WATER, { turn: 0.6 });
    }
    grid.border(T.WALL);
}

/** Sewers: a lattice of brick lanes, some carrying a channel, with junction halls and side rooms. */
function lanes(grid, rng, n) {
    grid.cells.fill(T.WALL);
    const xs = [], ys = [];
    for (let x = rng.int(3, 5); x < grid.w - 4; x += rng.int(8, 11)) xs.push(x);
    for (let y = rng.int(3, 5); y < grid.h - 4; y += rng.int(7, 10)) ys.push(y);
    const lat = { nodes: [] };
    ys.forEach((y, j) => xs.forEach((x, i) => lat.nodes.push({ i, j, x, y, skip: false })));
    const channelRow = ys.map((_, j) => j % 2 === 1);
    for (const [a, b] of latticeEdges(lat, rng, 0.55)) {
        if (a.j === b.j) {
            // A horizontal lane: 3 wide with a channel down the middle on
            // channel rows, 2 wide and dry otherwise.
            const y = a.y;
            if (channelRow[a.j]) {
                grid.fillRect(a.x, y, b.x - a.x + 3, 3, T.OPEN);
                grid.fillRect(a.x + 3, y + 1, b.x - a.x - 3, 1, T.WATER);
            } else {
                grid.fillRect(a.x, y, b.x - a.x + 3, 2, T.OPEN);
            }
        } else {
            grid.fillRect(a.x, a.y, 3, b.y - a.y + 3, T.OPEN);
        }
    }
    // Junctions: a hall at nearly half of them, some with a sludge pool.
    for (const node of lat.nodes) {
        if (!rng.chance(0.45)) continue;
        const w = rng.int(5, 9), h = rng.int(5, 9);
        const x0 = clamp(node.x + 1 - (w >> 1), 1, grid.w - 1 - w), y0 = clamp(node.y + 1 - (h >> 1), 1, grid.h - 1 - h);
        grid.fillRect(x0, y0, w, h, T.OPEN);
        if (rng.chance(0.35)) grid.fillRect(x0 + 2, y0 + 2, w - 4, h - 4, rng.chance(0.5) ? T.STONE : T.WATER);
        else if (w >= 7 && h >= 7) { grid.set(x0 + 1, y0 + 1, T.WALL); grid.set(x0 + w - 2, y0 + h - 2, T.WALL); }
    }
    // Side rooms hung off the lanes.
    const rooms = Math.round(lat.nodes.length * 0.6);
    for (let k = 0; k < rooms; k++) {
        const node = rng.pick(lat.nodes);
        const w = rng.int(3, 5), h = rng.int(3, 5);
        const dx = rng.pick([-w - 1, 3]), dy = rng.int(-2, 2);
        grid.fillRect(clamp(node.x + dx, 1, grid.w - 1 - w), clamp(node.y + dy, 1, grid.h - 1 - h), w, h, T.OPEN);
    }
    grid.border(T.WALL);
}

/** Computer: a circuit board; chip pads joined by traces, wide buses, coolant pools on the big chips. */
function circuit(grid, rng, n) {
    grid.cells.fill(T.WALL);
    const lat = lattice(grid, rng, 8, 1, 4);
    for (const node of lat.nodes) {
        if (rng.chance(0.12)) { node.skip = true; continue; }
        const size = rng.chance(0.3) ? rng.int(5, 7) : rng.int(3, 4);
        const x = clamp(node.x - (size >> 1), 1, grid.w - 1 - size), y = clamp(node.y - (size >> 1), 1, grid.h - 1 - size);
        grid.fillRect(x, y, size, size, T.OPEN);
        if (size >= 5 && rng.chance(0.5)) grid.fillRect(x + 1, y + 1, size - 2, size - 2, T.WATER);
        else if (size >= 6) grid.set(x + (size >> 1), y + (size >> 1), T.WALL);
    }
    for (const [a, b] of latticeEdges(lat, rng, 0.4)) carveL(grid, rng, a.x, a.y, b.x, b.y, rng.chance(0.2) ? 2 : 1, T.SAND);
    // Buses: long straight runs across the board, with vias along them.
    const buses = 1 + Math.floor(n / 2);
    for (let i = 0; i < buses; i++) {
        const width = rng.int(2, 3);
        if (rng.chance(0.5)) {
            const y = rng.int(3, grid.h - 4 - width);
            grid.fillRect(2, y, grid.w - 4, width, T.OPEN);
            for (let x = 4; x < grid.w - 4; x += rng.int(5, 8)) grid.set(x, y + (width >> 1), T.WALL);
        } else {
            const x = rng.int(3, grid.w - 4 - width);
            grid.fillRect(x, 2, width, grid.h - 4, T.OPEN);
            for (let y = 4; y < grid.h - 4; y += rng.int(5, 8)) grid.set(x + (width >> 1), y, T.WALL);
        }
    }
    grid.border(T.WALL);
}

/** Unknown: rings of corridor around a hub, joined by spokes, chambers hung off the rings, shards adrift. */
function rings(grid, rng, n) {
    grid.cells.fill(T.WALL);
    const cx = grid.w / 2, cy = grid.h / 2;
    const maxR = Math.min(grid.w, grid.h) / 2 - 2;
    const step = rng.int(6, 8);
    const ringRadii = [];
    for (let r = 4; r < maxR; r += step) ringRadii.push(r);
    for (const r of ringRadii) {
        const width = rng.int(2, 3);
        for (let y = 1; y < grid.h - 1; y++) for (let x = 1; x < grid.w - 1; x++) {
            const d = Math.hypot(x + 0.5 - cx, y + 0.5 - cy);
            if (d >= r && d < r + width) grid.set(x, y, T.OPEN);
        }
    }
    for (let i = 0; i + 1 < ringRadii.length; i++) {
        const count = rng.int(3, 5);
        for (let k = 0; k < count; k++) {
            const angle = rng.next() * Math.PI * 2;
            meander(grid, rng, cx + Math.cos(angle) * ringRadii[i], cy + Math.sin(angle) * ringRadii[i], angle, step + 2, 2, T.OPEN, { turn: 0 });
        }
    }
    blob(grid, rng, cx, cy, 4.5, T.OPEN, { wobble: 0.2 });
    for (const r of ringRadii) {
        const chambers = rng.int(3, 5);
        for (let k = 0; k < chambers; k++) {
            const angle = rng.next() * Math.PI * 2;
            const px = cx + Math.cos(angle) * (r + 1), py = cy + Math.sin(angle) * (r + 1);
            blob(grid, rng, px, py, rng.int(3, 4) + rng.next(), T.OPEN, { wobble: 0.3 });
            if (rng.chance(0.4)) blob(grid, rng, px, py, 1 + rng.next() * 1.5, T.WATER, { only: T.OPEN });
        }
    }
    // Corner halls, so the rings do not leave the corners dead.
    for (const [fx, fy] of [[0.15, 0.15], [0.85, 0.15], [0.15, 0.85], [0.85, 0.85]]) {
        blob(grid, rng, grid.w * fx, grid.h * fy, 3 + rng.next() * 2, T.OPEN, { wobble: 0.3 });
    }
    const shards = Math.round(grid.w * grid.h / 350 * (0.5 + n * 0.1));
    for (let i = 0; i < shards; i++) {
        const x = rng.int(2, grid.w - 3), y = rng.int(2, grid.h - 3);
        if (grid.get(x, y) === T.OPEN) grid.set(x, y, T.WALL);
    }
    grid.border(T.WALL);
}

// ---------------------------------------------------------------------------
// Density: what each biome plants in a field that is too open, and digs
// where a map is too closed
// ---------------------------------------------------------------------------

function fillGarden(grid, rng, x, y, r) {
    if (rng.chance(0.6)) blob(grid, rng, x, y, r, T.WALL, { wobble: 0.5, only: T.OPEN });
    else pond(grid, rng, x, y, r);
}
function fillDesert(grid, rng, x, y, r) {
    if (rng.chance(0.75)) blob(grid, rng, x, y, r, T.WALL, { wobble: 0.3, only: T.OPEN });
    else pond(grid, rng, x, y, Math.max(1, r - 0.5));
}
function fillHel(grid, rng, x, y, r) {
    blob(grid, rng, x, y, r, rng.chance(0.5) ? T.WALL : T.WATER, { wobble: 0.4, only: T.OPEN });
}
function fillOcean(grid, rng, x, y, r) {
    if (rng.chance(0.5)) blob(grid, rng, x, y, r, T.WALL, { wobble: 0.5, only: T.OPEN });
    else pond(grid, rng, x, y, r);
}
function fillAntHell(grid, rng, x, y, r) {
    blob(grid, rng, x, y, r, rng.chance(0.7) ? T.WALL : T.WATER, { wobble: 0.4, only: T.OPEN });
}
function fillJungle(grid, rng, x, y, r) {
    blob(grid, rng, x, y, r, rng.chance(0.7) ? T.WALL : T.WATER, { wobble: 0.5, only: T.OPEN });
}
function fillSewers(grid, rng, x, y, r) {
    const w = Math.max(1, Math.round(r * (0.8 + rng.next() * 0.8))), h = Math.max(1, Math.round(r * (0.8 + rng.next() * 0.8)));
    const kind = rng.chance(0.5) ? T.WALL : rng.chance(0.6) ? T.WATER : T.STONE;
    for (let py = y - (h >> 1); py < y - (h >> 1) + h; py++) for (let px = x - (w >> 1); px < x - (w >> 1) + w; px++) {
        if (grid.get(px, py) === T.OPEN) grid.set(px, py, kind);
    }
}
function fillComputer(grid, rng, x, y, r) {
    const w = Math.max(1, Math.round(r * (0.8 + rng.next() * 0.8))), h = Math.max(1, Math.round(r * (0.8 + rng.next() * 0.8)));
    const kind = rng.chance(0.5) ? T.WALL : T.WATER;
    for (let py = y - (h >> 1); py < y - (h >> 1) + h; py++) for (let px = x - (w >> 1); px < x - (w >> 1) + w; px++) {
        if (grid.get(px, py) === T.OPEN) grid.set(px, py, kind);
    }
}
function fillUnknown(grid, rng, x, y, r) {
    blob(grid, rng, x, y, r, rng.chance(0.6) ? T.WALL : T.WATER, { wobble: 0.5, only: T.OPEN });
}
function carveBlob(grid, rng, x, y, r) {
    blob(grid, rng, x, y, r, T.OPEN, { wobble: 0.4, only: [T.WALL, T.STONE] });
}
function carveRect(grid, rng, x, y, r) {
    const w = Math.round(r * 1.5), h = Math.round(r * 1.5);
    for (let py = y - (h >> 1); py < y - (h >> 1) + h; py++) for (let px = x - (w >> 1); px < x - (w >> 1) + w; px++) {
        if (px > 0 && py > 0 && px < grid.w - 1 && py < grid.h - 1 && (grid.get(px, py) === T.WALL || grid.get(px, py) === T.STONE)) grid.set(px, py, T.OPEN);
    }
}
function carveIsland(grid, rng, x, y, r) {
    blob(grid, rng, x, y, r + 1, T.SAND, { wobble: 0.35, only: T.WATER });
    blob(grid, rng, x, y, r, T.OPEN, { wobble: 0.35, only: [T.WATER, T.SAND, T.WALL] });
}

/** The largest fully-open (walkable) axis-aligned square, as its side and top-left corner. */
function largestOpenSquare(grid) {
    const w = grid.w, h = grid.h;
    const dp = new Uint16Array(w * h);
    let best = { side: 0, x0: 0, y0: 0 };
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const i = y * w + x;
            if (!walkable(grid.cells[i])) continue;
            const up = y > 0 ? dp[i - w] : 0, left = x > 0 ? dp[i - 1] : 0, diag = x > 0 && y > 0 ? dp[i - w - 1] : 0;
            dp[i] = 1 + Math.min(up, left, diag);
            if (dp[i] > best.side) best = { side: dp[i], x0: x - dp[i] + 1, y0: y - dp[i] + 1 };
        }
    }
    return best;
}

/**
 * Rooms: components of cells whose whole 5x5 neighbourhood is walkable.
 * A corridor up to four wide has no such cell, a room at least 5x6 has two.
 */
function roomCount(grid) {
    const interior = new Uint8Array(grid.w * grid.h);
    for (let y = 2; y < grid.h - 2; y++) {
        for (let x = 2; x < grid.w - 2; x++) {
            let ok = true;
            for (let dy = -2; dy <= 2 && ok; dy++) for (let dx = -2; dx <= 2; dx++) if (!walkable(grid.get(x + dx, y + dy))) { ok = false; break; }
            if (ok) interior[y * grid.w + x] = 1;
        }
    }
    const seen = new Uint8Array(grid.w * grid.h);
    let rooms = 0;
    for (let start = 0; start < seen.length; start++) {
        if (!interior[start] || seen[start]) continue;
        let size = 0;
        const stack = [start];
        seen[start] = 1;
        while (stack.length) {
            const i = stack.pop();
            size++;
            const x = i % grid.w, y = (i / grid.w) | 0;
            for (const [nx, ny] of [[x + 1, y], [x - 1, y], [x, y + 1], [x, y - 1]]) {
                if (!grid.inside(nx, ny)) continue;
                const j = ny * grid.w + nx;
                if (interior[j] && !seen[j]) { seen[j] = 1; stack.push(j); }
            }
        }
        if (size >= 2) rooms++;
    }
    return rooms;
}

function fractions(grid) {
    const total = grid.w * grid.h;
    let open = 0, water = 0, solid = 0;
    for (const c of grid.cells) {
        if (walkable(c)) open++;
        else if (c === T.WATER) water++;
        else solid++;
    }
    return { open: open / total, water: water / total, solid: solid / total, land: 1 - water / total };
}

/**
 * Plants and digs until the map sits inside the density targets: no open
 * square wider than the rule allows, the open fraction inside the target
 * window, and (ocean) no more water than allowed. Returns true when it
 * changed anything, so the caller can re-run connect() and come back.
 */
function densify(grid, rng, biome) {
    const before = grid.changes;
    const [minOpen, maxOpen] = biome.open || [TARGET.minOpen, TARGET.maxOpen];
    const edge = biome.ocean ? T.WATER : T.WALL;
    for (let iter = 0; iter < 800; iter++) {
        const f = fractions(grid);
        const sq = largestOpenSquare(grid);
        if (sq.side > RULES.maxOpenSquare) {
            // Somewhere inside the square, away from its edge; a locked spot
            // (the door, a pad) swallows the fill, so try a few.
            const was = grid.changes;
            for (let attempt = 0; attempt < 8 && grid.changes === was; attempt++) {
                const x = rng.int(sq.x0 + 2, sq.x0 + sq.side - 3), y = rng.int(sq.y0 + 2, sq.y0 + sq.side - 3);
                biome.fill(grid, rng, x, y, 1.5 + rng.next() * 1.5);
            }
            if (grid.changes === was) break;
            continue;
        }
        if (biome.ocean && f.water > TARGET.oceanMaxWater) {
            const shore = grid.where((v, x, y) => v === T.WATER && x > 1 && y > 1 && x < grid.w - 2 && y < grid.h - 2 && grid.touches(x, y, c => c !== T.WATER));
            if (!shore.length) break;
            const at = rng.pick(shore);
            carveIsland(grid, rng, at.x, at.y, 1.5 + rng.next() * 2);
            grid.border(edge);
            continue;
        }
        if (f.open > maxOpen) {
            const open = grid.where((v, x, y) => v === T.OPEN && !grid.locked[y * grid.w + x]);
            if (!open.length) break;
            const at = rng.pick(open);
            biome.fill(grid, rng, at.x, at.y, 1 + rng.next() * 2);
            continue;
        }
        if (f.open < minOpen) {
            const faces = grid.where((v, x, y) => (v === T.WALL || v === T.STONE || (biome.ocean && v === T.WATER)) && x > 1 && y > 1 && x < grid.w - 2 && y < grid.h - 2 && grid.touches(x, y, walkable));
            if (!faces.length) break;
            const at = rng.pick(faces);
            biome.carve(grid, rng, at.x, at.y, 2 + rng.next() * 2);
            grid.border(edge);   // a blob near the edge may have opened the rim
            continue;
        }
        break;
    }
    return grid.changes !== before;
}

// ---------------------------------------------------------------------------
// Doors, pads, bands and floors
// ---------------------------------------------------------------------------

/**
 * Where the door goes: the walkable cell nearest an anchor point that varies
 * per map (the hub for the rings, the biggest island for the ocean, a spot in
 * from one edge otherwise), with a clear 5x5 carved around it so the 3x3
 * spawn rectangle is open and has open neighbours on every side. The 5x5 is
 * then locked: nothing later may plant anything in it.
 */
function placeDoor(grid, rng, anchor) {
    let best = null;
    for (let y = 3; y < grid.h - 3; y++) {
        for (let x = 3; x < grid.w - 3; x++) {
            if (!walkable(grid.get(x, y))) continue;
            const d = Math.hypot(x - anchor[0], y - anchor[1]);
            if (best === null || d < best.d) best = { d, x, y };
        }
    }
    const [x, y] = best ? [best.x, best.y] : [clamp(Math.round(anchor[0]), 3, grid.w - 4), clamp(Math.round(anchor[1]), 3, grid.h - 4)];
    grid.fillRect(x - 2, y - 2, 5, 5, T.OPEN);
    grid.lockRect(x - 2, y - 2, 5, 5);
    return [x, y];
}

function edgeAnchor(grid, rng) {
    const side = rng.int(0, 3);
    const inset = 0.12 + rng.next() * 0.1;
    const along = 0.25 + rng.next() * 0.5;
    switch (side) {
        case 0: return [grid.w * along, grid.h * inset];
        case 1: return [grid.w * (1 - inset), grid.h * along];
        case 2: return [grid.w * along, grid.h * (1 - inset)];
        default: return [grid.w * inset, grid.h * along];
    }
}

/** True when the 5x5 around a cell is all walkable: a pad two tiles from anything solid. */
function padClear(grid, x, y) {
    for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) {
        if (!walkable(grid.get(x + dx, y + dy))) return false;
    }
    return grid.get(x, y) === T.OPEN;
}

/**
 * A teleporter pad: the reachable open cell scoring best under `score`, that
 * has clear ground around it and is not in the door. When no cell qualifies
 * the best-scoring reachable cell is cleared to fit one -- clearing only ever
 * opens ground, so the map stays connected. The pad's 5x5 is locked.
 */
function placePad(grid, dist, door, score, taken) {
    const inDoor = (x, y) => Math.abs(x - door[0]) <= 2 && Math.abs(y - door[1]) <= 2;
    const candidates = [];
    for (let y = 3; y < grid.h - 3; y++) {
        for (let x = 3; x < grid.w - 3; x++) {
            const d = dist[y * grid.w + x];
            if (d < 0 || inDoor(x, y) || grid.get(x, y) !== T.OPEN) continue;
            if (taken.some(([tx, ty]) => Math.abs(tx - x) <= 3 && Math.abs(ty - y) <= 3)) continue;
            candidates.push({ x, y, s: score(d, x, y), clear: padClear(grid, x, y) });
        }
    }
    candidates.sort((a, b) => b.s - a.s || a.y - b.y || a.x - b.x);
    let chosen = candidates.find(c => c.clear);
    if (!chosen) {
        chosen = candidates[0];
        if (!chosen) throw new Error('no cell can hold a teleporter pad');
        grid.fillRect(chosen.x - 2, chosen.y - 2, 5, 5, T.OPEN);
    }
    grid.lockRect(chosen.x - 2, chosen.y - 2, 5, 5);
    return [chosen.x, chosen.y];
}

/** The rarity threshold list for one map: distance at which each tier ends. */
function tierThresholds(dist) {
    const sorted = Array.from(dist).filter(d => d >= 0).sort((a, b) => a - b);
    return TIER_QUANTILES.map(q => sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))]);
}

/**
 * Tier bands as rectangles.
 *
 * The map is cut into BAND_BLOCK-square blocks; a block's tier comes from the
 * shortest walk from the door to any walkable cell in it, and a block with no
 * walkable cell borrows its nearest neighbour's. Runs of equal blocks are
 * then merged into rectangles row by row. The door's block is at distance 0
 * and the common band ends a good way out, so the door always sits in common
 * ground; the checker holds the generator to that. Every block gets a band,
 * so the bands cover every open cell.
 */
function bands(grid, dist, withMythic) {
    const bw = Math.ceil(grid.w / BAND_BLOCK), bh = Math.ceil(grid.h / BAND_BLOCK);
    let blockDist = new Int32Array(bw * bh).fill(-1);
    let maxDist = 0;
    for (let y = 0; y < grid.h; y++) for (let x = 0; x < grid.w; x++) {
        const d = dist[y * grid.w + x];
        if (d < 0) continue;
        maxDist = Math.max(maxDist, d);
        const b = ((y / BAND_BLOCK) | 0) * bw + ((x / BAND_BLOCK) | 0);
        if (blockDist[b] < 0 || d < blockDist[b]) blockDist[b] = d;
    }
    for (let changed = true; changed;) {
        changed = false;
        const next = Int32Array.from(blockDist);
        for (let by = 0; by < bh; by++) for (let bx = 0; bx < bw; bx++) {
            const b = by * bw + bx;
            if (blockDist[b] >= 0) continue;
            let best = -1;
            for (const [nx, ny] of [[bx + 1, by], [bx - 1, by], [bx, by + 1], [bx, by - 1]]) {
                if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
                const v = blockDist[ny * bw + nx];
                if (v >= 0 && (best < 0 || v < best)) best = v;
            }
            if (best >= 0) { next[b] = best; changed = true; }
        }
        blockDist = next;
    }

    const thresholds = tierThresholds(dist);
    const tierOf = new Int8Array(bw * bh);
    for (let b = 0; b < tierOf.length; b++) {
        let tier = 0;
        while (tier < thresholds.length - 1 && blockDist[b] > thresholds[tier]) tier++;
        tierOf[b] = tier;
    }
    if (withMythic) {
        let far = 0;
        for (let b = 0; b < blockDist.length; b++) {
            if (blockDist[b] > blockDist[far]) far = b;
            if (blockDist[b] >= maxDist * MYTHIC_FRACTION) tierOf[b] = 5;
        }
        tierOf[far] = 5;
    }

    const used = new Uint8Array(bw * bh);
    const rects = [];
    for (let by = 0; by < bh; by++) for (let bx = 0; bx < bw; bx++) {
        const b = by * bw + bx;
        if (used[b]) continue;
        const tier = tierOf[b];
        let w = 1;
        while (bx + w < bw && !used[b + w] && tierOf[b + w] === tier) w++;
        let h = 1;
        outer: while (by + h < bh) {
            for (let i = 0; i < w; i++) {
                const c = (by + h) * bw + bx + i;
                if (used[c] || tierOf[c] !== tier) break outer;
            }
            h++;
        }
        for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) used[(by + j) * bw + bx + i] = 1;
        const x0 = bx * BAND_BLOCK, y0 = by * BAND_BLOCK;
        rects.push({
            tier: TIERS[tier],
            x: x0, y: y0,
            w: Math.min(grid.w, x0 + w * BAND_BLOCK) - x0,
            h: Math.min(grid.h, y0 + h * BAND_BLOCK) - y0,
        });
    }
    return rects;
}

/**
 * Floor decorations: a variant per open cell (-1 = none), laid down as
 * clusters -- a seed, then a short random walk over open ground -- rather
 * than as noise, on about a tenth of the open cells. Each of a biome's three
 * stamps has a weight and a preference (`near` open ground, a wall, or
 * water) that decides where its seeds fall. Locked cells (the door, the
 * pads) are left bare.
 */
function decorate(grid, rng, biome) {
    const floors = new Int8Array(grid.w * grid.h).fill(-1);
    const open = grid.where((v, x, y) => v === T.OPEN && !grid.locked[y * grid.w + x]);
    const openCount = grid.count(walkable);
    const target = Math.round(openCount * TARGET.floor);
    const nearWater = open.filter(c => [[1, 0], [-1, 0], [0, 1], [0, -1], [1, 1], [-1, -1], [1, -1], [-1, 1]].some(([dx, dy]) => grid.get(c.x + dx, c.y + dy) === T.WATER));
    const nearWall = open.filter(c => grid.touches(c.x, c.y, v => v === T.WALL));
    const seeds = { open, water: nearWater.length ? nearWater : open, wall: nearWall.length ? nearWall : open };
    const weights = biome.floors.map(f => f.w);
    const totalWeight = weights.reduce((a, b) => a + b, 0);
    let placed = 0;
    for (let guard = 0; guard < 4000 && placed < target; guard++) {
        let roll = rng.next() * totalWeight, variant = 0;
        while (variant < weights.length - 1 && roll >= weights[variant]) { roll -= weights[variant]; variant++; }
        const pool = seeds[biome.floors[variant].near];
        let { x, y } = rng.pick(pool);
        const length = rng.int(3, 12);
        for (let step = 0; step < length; step++) {
            const i = y * grid.w + x;
            if (floors[i] < 0) { floors[i] = variant; placed++; }
            const moves = [[1, 0], [-1, 0], [0, 1], [0, -1]].filter(([dx, dy]) => grid.get(x + dx, y + dy) === T.OPEN && !grid.locked[(y + dy) * grid.w + x + dx]);
            if (!moves.length) break;
            const [dx, dy] = rng.pick(moves);
            x += dx; y += dy;
        }
    }
    return floors;
}

// ---------------------------------------------------------------------------
// One map
// ---------------------------------------------------------------------------

function generate(biome, biomeIndex, n, ts, ground) {
    const id = `${biome.id}_${n}`;
    const rng = mulberry32(hashString(id));
    const [w, h] = biome.sizes[n - 1];
    const grid = new Grid(w, h, T.WALL);
    const gids = biomeGids(ts, biome);

    let anchor;
    let door;
    switch (biome.style) {
        case 'meadow': meadow(grid, rng, n); anchor = edgeAnchor(grid, rng); break;
        case 'mesa': mesa(grid, rng, n); anchor = edgeAnchor(grid, rng); break;
        case 'caves': caves(grid, rng, n); anchor = edgeAnchor(grid, rng); break;
        case 'thicket': thicket(grid, rng, n); anchor = edgeAnchor(grid, rng); break;
        case 'archipelago': anchor = archipelago(grid, rng, n); break;
        case 'tunnels': {
            const a = edgeAnchor(grid, rng);
            door = [clamp(Math.round(a[0]), 3, w - 4), clamp(Math.round(a[1]), 3, h - 4)];
            tunnels(grid, rng, n, door);
            anchor = door;
            break;
        }
        case 'lanes': lanes(grid, rng, n); anchor = edgeAnchor(grid, rng); break;
        case 'circuit': circuit(grid, rng, n); anchor = edgeAnchor(grid, rng); break;
        case 'rings': rings(grid, rng, n); anchor = [w / 2, h / 2]; break;
        default: throw new Error(`unknown style ${biome.style}`);
    }
    door = placeDoor(grid, rng, anchor);

    const connectOptions = {
        width: biome.style === 'tunnels' || biome.style === 'circuit' || biome.ocean ? 1 : 2,
        minRegion: biome.ocean ? 12 : 8,
        fillWith: biome.ocean ? T.WATER : T.WALL,
        crossing: T.SAND,
    };
    const settle = () => {
        // Plant, reconnect, and go round again until neither changes anything:
        // a corridor can open a field, a boulder can cut a corridor.
        for (let round = 0; round < 12; round++) {
            const changed = densify(grid, rng, biome);
            const before = grid.changes;
            connect(grid, rng, door[0], door[1], connectOptions);
            if (!changed && grid.changes === before) return;
        }
    };
    connect(grid, rng, door[0], door[1], connectOptions);
    settle();
    // A map short of rooms gets chambers dug at random wall faces until it
    // has a margin over the rule; each one is settled in like the rest.
    for (let attempt = 0; attempt < 40 && roomCount(grid) < RULES.minRooms + 1; attempt++) {
        const faces = grid.where((v, x, y) => (v === T.WALL || v === T.STONE || (biome.ocean && v === T.WATER)) && x > 3 && y > 3 && x < grid.w - 4 && y < grid.h - 4 && grid.touches(x, y, walkable));
        if (!faces.length) break;
        const at = rng.pick(faces);
        biome.carve(grid, rng, at.x, at.y, 3.5 + rng.next());
        grid.border(biome.ocean ? T.WATER : T.WALL);
        connect(grid, rng, door[0], door[1], connectOptions);
        settle();
    }

    // Pads: "next" as far from the door as the map allows, "home" a short
    // walk from it. Both may clear ground for themselves, and both get
    // locked, so the settle pass afterwards leaves them alone.
    let dist = distances(grid, door[0], door[1]);
    const nextPad = placePad(grid, dist, door, d => d, []);
    dist = distances(grid, door[0], door[1]);
    const homePad = placePad(grid, dist, door, d => (d < 5 ? -100 : -d), [nextPad]);
    settle();
    dist = distances(grid, door[0], door[1]);

    const rects = bands(grid, dist, n >= 4);
    // A skinless biome has nothing to stamp; skipping decorate() also leaves
    // the rng untouched, though nothing after this point draws from it.
    const floors = gids.floors.length ? decorate(grid, rng, biome) : new Int8Array(grid.w * grid.h).fill(-1);

    // -- Tiled objects ------------------------------------------------------
    const prop = (name, type, value) => ({ name, type, value });
    let nextId = 1;
    const spawnObjects = [];
    const region = {
        height: h * TILE_SIZE, id: nextId++, name: '', rotation: 0, type: 'spawn', visible: true,
        width: w * TILE_SIZE, x: 0, y: 0,
        properties: [prop('mobs', 'string', biome.mobs)],
    };
    if (biome.id === 'unknown') {
        // Neither reader acts on a `note`; it is there for whoever opens the
        // map in Tiled and wonders why the region names two other biomes.
        region.properties.push(prop('note', 'string', 'src/mobs.json has no "unknown" group, so this map spawns a computer/hel mix'));
    }
    spawnObjects.push(region);
    for (const rect of rects) {
        spawnObjects.push({
            height: rect.h * TILE_SIZE, id: nextId++, name: '', rotation: 0, type: 'spawn', visible: true,
            width: rect.w * TILE_SIZE, x: rect.x * TILE_SIZE, y: rect.y * TILE_SIZE,
            properties: [prop('spawnType', 'string', rect.tier)],
        });
    }
    const label = `${biome.name} ${n}`;
    const doorObject = {
        height: 3 * TILE_SIZE, id: nextId++, name: id, rotation: 0, type: 'player_spawn', visible: true,
        width: 3 * TILE_SIZE, x: (door[0] - 1) * TILE_SIZE, y: (door[1] - 1) * TILE_SIZE,
        properties: [
            prop('label', 'string', label),
            prop('color', 'color', biome.color),
            prop('order', 'float', 100 + biomeIndex * 10 + n),
            prop('backdrop', 'string', biome.id),
            prop('biome', 'string', biome.id),
            // A sublevel is entered through the pads from the biome's main
            // area; the picker never offers it (an admin session may still
            // name it, for tooling).
            prop('pickable', 'bool', false),
        ],
    };
    const pad = ([x, y], targetMap, targetSpawn) => ({
        height: 0, id: nextId++, name: '', rotation: 0, type: 'teleporter', visible: true, width: 0,
        x: (x + 0.5) * TILE_SIZE, y: (y + 0.5) * TILE_SIZE, point: true,
        properties: [prop('targetMap', 'string', targetMap), prop('targetSpawn', 'string', targetSpawn)],
    });
    const teleporters = [
        pad(nextPad, `${biome.id}_${n % MAPS_PER_BIOME + 1}`, `${biome.id}_${n % MAPS_PER_BIOME + 1}`),
        pad(homePad, 'world', biome.id),
    ];

    const layer = (id, name, extra) => ({ ...extra, id, name, opacity: 1, visible: true, x: 0, y: 0 });
    const objectLayer = (id, name, objects) => layer(id, name, { draworder: 'topdown', objects, type: 'objectgroup' });
    const tileLayer = (id, name, data) => layer(id, name, { data, height: h, type: 'tilelayer', width: w });

    const background = new Array(w * h).fill(ts.groundFirstGid + ground.idOf(biome.id));
    const terrain = new Array(w * h);
    for (let i = 0; i < terrain.length; i++) {
        switch (grid.cells[i]) {
            case T.OPEN: terrain[i] = floors[i] >= 0 ? gids.floors[floors[i]] : 0; break;
            case T.WALL: terrain[i] = gids.wall; break;
            case T.WATER: terrain[i] = gids.water; break;
            case T.SAND: terrain[i] = gids.sand; break;
            case T.STONE: terrain[i] = gids.stone; break;
            default: throw new Error(`${id}: cell ${i} holds kind ${grid.cells[i]}`);
        }
    }

    const map = {
        compressionlevel: -1,
        height: h,
        infinite: false,
        layers: [
            tileLayer(1, 'background', background),
            tileLayer(2, 'terrain', terrain),
            objectLayer(3, 'spawns', spawnObjects),
            objectLayer(4, 'player_spawns', [doorObject]),
            objectLayer(5, 'teleporters', teleporters),
        ],
        nextlayerid: 6,
        nextobjectid: nextId,
        orientation: 'orthogonal',
        properties: [
            prop('displayName', 'string', label),
            prop('defaultMobGroup', 'string', biome.defaultMobGroup),
            prop('biome', 'string', biome.id),
        ],
        renderorder: 'right-down',
        tiledversion: '1.10.2',
        tileheight: TILE_SIZE,
        tilesets: [
            { firstgid: TERRAIN_FIRST_GID, source: 'terrain.tsj' },
            { firstgid: ts.groundFirstGid, source: 'ground.tsj' },
        ],
        tilewidth: TILE_SIZE,
        type: 'map',
        version: '1.10',
        width: w,
    };
    return { id, map: sortKeys(map) };
}

/** Tiled writes every object's keys in alphabetical order; matching it keeps a re-save in Tiled a no-op. */
function sortKeys(value) {
    if (Array.isArray(value)) return value.map(sortKeys);
    if (value && typeof value === 'object') {
        const out = {};
        for (const key of Object.keys(value).sort()) out[key] = sortKeys(value[key]);
        return out;
    }
    return value;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

function manifest(ids) {
    const existing = JSON.parse(fs.readFileSync(path.join(MAPS_DIR, 'maps.json'), 'utf8'));
    const lines = [
        '{',
        '  "_comment": [',
        ...existing._comment.map((line, i, all) => `    ${JSON.stringify(line)}${i + 1 < all.length ? ',' : ''}`),
        '  ],',
        '  "maps": [',
        '    { "file": "world.tmj" },',
        '    { "file": "sewers.tmj" },',
        ...ids.map((id, i) => `    { "file": "${id}.tmj" }${i + 1 < ids.length ? ',' : ''}`),
        '  ]',
        '}',
        '',
    ];
    return lines.join('\n');
}

// ---------------------------------------------------------------------------
// Reading maps back: shared by the checker and the world tools
// ---------------------------------------------------------------------------

function readMap(file) {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function layersOf(map) {
    return Object.fromEntries(map.layers.map(l => [l.name, l]));
}

/** A map's terrain layer as tile records, resolved through the tileset the map references. */
function terrainOf(map, ts, context) {
    const ref = (map.tilesets || []).find(t => t.source === 'terrain.tsj');
    if (!ref) throw new Error(`${context}: no terrain.tsj tileset`);
    if (ref.firstgid !== TERRAIN_FIRST_GID) throw new Error(`${context}: terrain.tsj firstgid is ${ref.firstgid}, expected ${TERRAIN_FIRST_GID}`);
    const layer = layersOf(map).terrain;
    if (!layer || !Array.isArray(layer.data)) throw new Error(`${context}: no plain-array terrain layer`);
    return layer.data.map((raw, i) => {
        const gid = raw & 0x1fffffff;
        const rec = ts.byGid.get(gid);
        if (!rec) throw new Error(`${context}: tile (${i % map.width},${(i / map.width) | 0}) uses gid ${gid}, which ${path.relative(ROOT, ts.file)} does not define`);
        return rec;
    });
}

/** The spawn ids a map file defines, for teleporter targets. */
function spawnIdsOf(file) {
    const ids = new Set();
    for (const layer of readMap(file).layers) {
        if (layer.type !== 'objectgroup' || layer.name !== 'player_spawns') continue;
        for (const o of layer.objects) ids.add(readProps(o).spawnId || o.name);
    }
    return ids;
}

function objectKind(o) {
    return o.class || o.type || '';
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

function check(id, map, ctx) {
    const { ts, ground, manifestIds, spawnIdCache } = ctx;
    const fail = message => { throw new Error(`${id}: ${message}`); };
    const w = map.width, h = map.height;
    if (!(w >= 40 && w <= 120 && h >= 40 && h <= 120)) fail(`size ${w}x${h} is outside 40..120`);
    if (map.tilewidth !== TILE_SIZE || map.tileheight !== TILE_SIZE) fail('tile size is not 300');
    if (map.orientation !== 'orthogonal' || map.infinite) fail('not a finite orthogonal map');
    const props = readProps(map);
    for (const name of ['displayName', 'defaultMobGroup', 'biome']) if (!props[name]) fail(`map property ${name} missing`);
    const biome = BIOMES.find(b => b.id === props.biome);
    if (!biome) fail(`biome ${props.biome} is not one of the nine`);

    const layers = layersOf(map);
    for (const name of ['background', 'terrain', 'spawns', 'player_spawns', 'teleporters']) if (!layers[name]) fail(`layer ${name} missing`);

    // Tilesets: terrain at 1, ground right after it, no overlap possible.
    const groundRef = (map.tilesets || []).find(t => t.source === 'ground.tsj');
    if (!groundRef) fail('no ground.tsj tileset');
    if (groundRef.firstgid !== ts.groundFirstGid) fail(`ground.tsj firstgid is ${groundRef.firstgid}, expected ${ts.groundFirstGid} (terrain tilecount + 1)`);
    const terrain = terrainOf(map, ts, id);
    if (terrain.length !== w * h) fail(`terrain holds ${terrain.length} cells, expected ${w * h}`);
    if (layers.background.data.length !== w * h) fail('background length mismatch');
    const groundGid = ts.groundFirstGid + ground.idOf(biome.id);
    for (const gid of layers.background.data) if (gid !== groundGid) fail(`background uses gid ${gid}, expected ${groundGid}`);

    // Every cell is one of the biome's own tiles or a shared crossing. A
    // skinned biome's walls/water carry its skin; a skinless biome's are the
    // default family (skin "") and it has no floor decorations at all.
    const skin = biome.skin || '';
    if (!!skin !== SKINNED.has(biome.id)) fail(`skin "${skin}" disagrees with SKINNED`);
    const wallName = skin ? `wall_${skin}` : 'wall', waterName = skin ? `water_${skin}` : 'water';
    let open = 0, water = 0, solid = 0, floor = 0, decorated = 0;
    const kind = new Uint8Array(w * h);   // T.* per cell
    terrain.forEach((rec, i) => {
        switch (rec.tileId) {
            case 0:
                if (rec.gid !== 0) {
                    if (!skin) fail(`cell ${i} is ${rec.cls}, but a skinless biome has no floor decorations`);
                    if (rec.skin !== skin || rec.variant < 0 || rec.variant > 2 || rec.solid || rec.water) fail(`cell ${i} is ${rec.cls}, not one of this biome's floor decorations`);
                    decorated++;
                }
                kind[i] = T.OPEN; open++; floor++;
                break;
            case 1:
                if (rec.skin !== skin || !rec.solid) fail(`cell ${i} is ${rec.cls}, not a ${wallName} tile`);
                kind[i] = T.WALL; solid++;
                break;
            case 2:
                if (rec.skin !== skin || !rec.water) fail(`cell ${i} is ${rec.cls}, not a ${waterName} tile`);
                kind[i] = T.WATER; water++;
                break;
            case 3:
                if (rec.cls !== 'bridge') fail(`cell ${i} is ${rec.cls}, not bridge`);
                kind[i] = T.SAND; open++;
                break;
            case 4:
                if (rec.cls !== 'sewage') fail(`cell ${i} is ${rec.cls}, not sewage`);
                kind[i] = T.STONE;
                break;
            default: fail(`cell ${i} is ${rec.cls} (tileId ${rec.tileId}), which the generator never writes`);
        }
    });
    const total = w * h;
    const openFraction = open / total, waterFraction = water / total;
    if (openFraction < RULES.minOpen || openFraction > RULES.maxOpen) fail(`open fraction ${(openFraction * 100).toFixed(1)}% is outside ${RULES.minOpen * 100}..${RULES.maxOpen * 100}%`);
    if ((solid + water) / total < RULES.minSolidOrWater) fail(`wall+water is only ${(((solid + water) / total) * 100).toFixed(1)}% of the map`);
    if (biome.ocean) {
        if (waterFraction > RULES.oceanMaxWater) fail(`water is ${(waterFraction * 100).toFixed(1)}% of the map, more than ${RULES.oceanMaxWater * 100}%`);
        if (1 - waterFraction < RULES.oceanMinLand) fail(`land is ${((1 - waterFraction) * 100).toFixed(1)}% of the map, less than ${RULES.oceanMinLand * 100}%`);
    }
    const floorFraction = decorated / open;
    if (skin) {
        if (floorFraction < RULES.minFloor || floorFraction > RULES.maxFloor) fail(`floor decorations cover ${(floorFraction * 100).toFixed(1)}% of the open cells, outside ${RULES.minFloor * 100}..${RULES.maxFloor * 100}%`);
    } else if (decorated) {
        fail(`${decorated} floor decorations on a skinless biome`);
    }

    const grid = new Grid(w, h, T.WALL);
    grid.cells = kind;
    const square = largestOpenSquare(grid);
    if (square.side > RULES.maxOpenSquare) fail(`an open ${square.side}x${square.side} square at (${square.x0},${square.y0}); nothing wider than ${RULES.maxOpenSquare} is allowed`);
    const rooms = roomCount(grid);
    if (rooms < RULES.minRooms) fail(`only ${rooms} rooms, fewer than ${RULES.minRooms}`);

    // Object ids.
    const ids = new Set();
    let maxId = 0;
    for (const layer of map.layers) {
        for (const o of layer.objects || []) {
            if (ids.has(o.id)) fail(`object id ${o.id} repeats`);
            ids.add(o.id);
            maxId = Math.max(maxId, o.id);
            if (o.polyline) fail(`object ${o.id} is a polyline`);
        }
    }
    if (map.nextobjectid !== maxId + 1) fail(`nextobjectid ${map.nextobjectid} != ${maxId + 1}`);

    // The door.
    const doors = layers.player_spawns.objects;
    if (doors.length !== 1) fail(`${doors.length} doors`);
    const door = doors[0];
    if (objectKind(door) !== 'player_spawn') fail('door is not a player_spawn');
    const doorProps = readProps(door);
    if ((doorProps.spawnId || door.name) !== id) fail('door id is not the map id');
    for (const name of ['label', 'color', 'order', 'backdrop', 'biome']) if (doorProps[name] === undefined) fail(`door lacks ${name}`);
    if (doorProps.pickable !== false) fail('door is not pickable = false');
    if (door.width !== 3 * TILE_SIZE || door.height !== 3 * TILE_SIZE) fail('door is not 3x3 tiles');
    if (door.x % TILE_SIZE || door.y % TILE_SIZE) fail('door is not tile-aligned');
    const dx0 = door.x / TILE_SIZE, dy0 = door.y / TILE_SIZE;
    for (let y = dy0; y < dy0 + 3; y++) for (let x = dx0; x < dx0 + 3; x++) {
        if (x < 0 || y < 0 || x >= w || y >= h || kind[y * w + x] !== T.OPEN) fail(`door tile (${x},${y}) is not open`);
    }
    const doorCentre = [dx0 + 1, dy0 + 1];
    let openNeighbours = 0;
    for (const [nx, ny] of [[dx0 - 1, dy0 + 1], [dx0 + 3, dy0 + 1], [dx0 + 1, dy0 - 1], [dx0 + 1, dy0 + 3]]) {
        if (nx >= 0 && ny >= 0 && nx < w && ny < h && walkable(kind[ny * w + nx])) openNeighbours++;
    }
    if (openNeighbours < 2) fail('door has fewer than two open neighbours');

    // Reachability, and how far the map goes.
    const dist = distances(grid, doorCentre[0], doorCentre[1]);
    let maxDist = 0;
    for (let i = 0; i < dist.length; i++) {
        if (walkable(kind[i]) && dist[i] < 0) fail(`tile (${i % w},${(i / w) | 0}) is unreachable from the door`);
        maxDist = Math.max(maxDist, dist[i]);
    }

    // Teleporters.
    const pads = layers.teleporters.objects;
    if (pads.length !== 2) fail(`${pads.length} teleporters`);
    for (const pad of pads) {
        const p = readProps(pad);
        if (objectKind(pad) !== 'teleporter') fail(`teleporter ${pad.id} is not a teleporter`);
        if (!pad.point || pad.width || pad.height) fail(`teleporter ${pad.id} is not a point`);
        if (!manifestIds.has(p.targetMap)) fail(`teleporter ${pad.id} targets map ${p.targetMap}, which maps.json does not list`);
        if (!spawnIdCache.has(p.targetMap)) spawnIdCache.set(p.targetMap, spawnIdsOf(path.join(MAPS_DIR, `${p.targetMap}.tmj`)));
        if (!spawnIdCache.get(p.targetMap).has(p.targetSpawn)) fail(`teleporter ${pad.id} targets spawn ${p.targetMap}:${p.targetSpawn}, which does not exist`);
        const tx = Math.floor(pad.x / TILE_SIZE), ty = Math.floor(pad.y / TILE_SIZE);
        if (kind[ty * w + tx] !== T.OPEN) fail(`teleporter ${pad.id} is not on open ground`);
        for (let y = ty - 2; y <= ty + 2; y++) for (let x = tx - 2; x <= tx + 2; x++) {
            if (x < 0 || y < 0 || x >= w || y >= h || !walkable(kind[y * w + x])) fail(`teleporter ${pad.id} is within two tiles of something solid`);
        }
        if (tx >= dx0 && tx < dx0 + 3 && ty >= dy0 && ty < dy0 + 3) fail(`teleporter ${pad.id} sits in the door`);
        if (dist[ty * w + tx] < 0) fail(`teleporter ${pad.id} is unreachable`);
        pad.tileDistance = dist[ty * w + tx];
    }
    const n = Number(id.split('_').pop());
    const nextTarget = readProps(pads[0]).targetMap;
    if (nextTarget !== `${biome.id}_${n % MAPS_PER_BIOME + 1}`) fail(`next pad leads to ${nextTarget}`);
    if (readProps(pads[1]).targetMap !== 'world' || readProps(pads[1]).targetSpawn !== biome.id) fail('home pad does not lead to the world door');
    if (pads[0].tileDistance < maxDist * RULES.nextPadMinDistance) fail(`the next pad is ${pads[0].tileDistance} tiles from the door, less than half the map's ${maxDist}`);

    // Bands.
    const spawns = layers.spawns.objects;
    for (const o of spawns) if (objectKind(o) !== 'spawn') fail(`spawn object ${o.id} is not a spawn`);
    const regions = spawns.filter(o => readProps(o).spawnType === undefined);
    if (regions.length !== 1) fail(`${regions.length} mob regions`);
    if (regions[0].x !== 0 || regions[0].y !== 0 || regions[0].width !== w * TILE_SIZE || regions[0].height !== h * TILE_SIZE) fail('mob region does not cover the map');
    if (readProps(regions[0]).mobs !== biome.mobs) fail(`mob region names "${readProps(regions[0]).mobs}", not "${biome.mobs}"`);
    const tiers = {};
    const covered = new Uint8Array(w * h);
    for (const o of spawns) {
        const tier = readProps(o).spawnType;
        if (tier === undefined) continue;
        if (!TIERS.includes(tier)) fail(`band ${o.id} has tier ${tier}`);
        tiers[tier] = (tiers[tier] || 0) + 1;
        if (o.x % TILE_SIZE || o.y % TILE_SIZE || o.width % TILE_SIZE || o.height % TILE_SIZE) fail(`band ${o.id} is not tile-aligned`);
        const x0 = o.x / TILE_SIZE, y0 = o.y / TILE_SIZE, x1 = x0 + o.width / TILE_SIZE, y1 = y0 + o.height / TILE_SIZE;
        if (x0 < 0 || y0 < 0 || x1 > w || y1 > h) fail(`band ${o.id} leaves the map`);
        const holds = (x, y) => x >= x0 && x < x1 && y >= y0 && y < y1;
        if (TIERS.indexOf(tier) >= TIERS.indexOf('rare')) {
            for (let j = 0; j < 3; j++) for (let i = 0; i < 3; i++) if (holds(dx0 + i, dy0 + j)) fail(`${tier} band ${o.id} covers the door`);
        }
        for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) covered[y * w + x] = 1;
    }
    for (const tier of ['common', 'uncommon', 'rare', 'epic', 'legendary']) if (!tiers[tier]) fail(`no ${tier} band`);
    if ((tiers.mythic || 0) > 0 !== n >= 4) fail(`${tiers.mythic || 0} mythic bands on map ${n}`);
    let coveredOpen = 0;
    for (let i = 0; i < covered.length; i++) if (walkable(kind[i]) && covered[i]) coveredOpen++;
    if (coveredOpen / open < RULES.minBandCoverage) fail(`bands cover ${((coveredOpen / open) * 100).toFixed(1)}% of the open cells, less than ${RULES.minBandCoverage * 100}%`);

    return {
        bands: spawns.length - 1, open: openFraction, water: waterFraction, solid: solid / total,
        floor: floorFraction, rooms, square: square.side, total, farthest: maxDist, nextPad: pads[0].tileDistance,
    };
}

// ---------------------------------------------------------------------------
// The overworld: mob regions per section, and an entrance pad per biome
// ---------------------------------------------------------------------------

const WORLD_FILE = path.join(MAPS_DIR, 'world.tmj');

function readWorld() {
    const text = fs.readFileSync(WORLD_FILE, 'utf8');
    const map = JSON.parse(text);
    // world.tmj is written by Tiled and by scripts with one-space indent;
    // only rewrite it when the round trip is byte-exact, so the edit touches
    // nothing but the objects it means to.
    if (JSON.stringify(map, null, 1) + '\n' !== text) throw new Error('maps/world.tmj does not round-trip through JSON.stringify(map, null, 1); refusing to rewrite it');
    return map;
}

function writeWorld(map) {
    fs.writeFileSync(WORLD_FILE, JSON.stringify(map, null, 1) + '\n');
}

function isRegion(o) {
    return objectKind(o) === 'spawn' && readProps(o).spawnType === undefined && readProps(o).mobs !== undefined;
}

/** Adds a whole-section mob region for every biome that has none. */
function worldRegions() {
    const map = readWorld();
    const layer = layersOf(map).spawns;
    if (!layer) throw new Error('world.tmj has no spawns layer');
    const added = [];
    for (const biome of BIOMES) {
        if (layer.objects.some(o => isRegion(o) && o.name === biome.id)) continue;
        const sx = biome.section % 3, sy = Math.floor(biome.section / 3);
        const region = sortKeys({
            height: SECTION_SIZE, id: map.nextobjectid++, name: biome.id, rotation: 0, type: 'spawn', visible: true,
            width: SECTION_SIZE, x: sx * SECTION_SIZE, y: sy * SECTION_SIZE,
            properties: [{ name: 'mobs', type: 'string', value: biome.mobs }],
        });
        // Next to the other regions: right after the last one.
        let at = -1;
        layer.objects.forEach((o, i) => { if (isRegion(o)) at = i; });
        layer.objects.splice(at + 1, 0, region);
        added.push(biome.id);
    }
    if (added.length) writeWorld(map);
    return added;
}

/** A world door as a tile rectangle. */
function worldDoor(map, name) {
    const layer = layersOf(map).player_spawns;
    const door = (layer ? layer.objects : []).find(o => objectKind(o) === 'player_spawn' && (readProps(o).spawnId || o.name) === name);
    if (!door) throw new Error(`world.tmj has no door "${name}"`);
    return {
        object: door,
        x0: Math.floor(door.x / TILE_SIZE), y0: Math.floor(door.y / TILE_SIZE),
        x1: Math.ceil((door.x + door.width) / TILE_SIZE), y1: Math.ceil((door.y + door.height) / TILE_SIZE),
    };
}

/** Tiles between a cell and a door rectangle (0 when inside it). */
function tileGap(x, y, door) {
    const gx = x < door.x0 ? door.x0 - x : x >= door.x1 ? x - door.x1 + 1 : 0;
    const gy = y < door.y0 ? door.y0 - y : y >= door.y1 ? y - door.y1 + 1 : 0;
    return Math.max(gx, gy);
}

/**
 * Tiles of walkable ground on every side of a world cell, up to `limit`:
 * 2 means the 5x5 around it is clear, 0 that only the cell itself is open.
 */
function clearanceAt(kindAt, x, y, limit) {
    if (kindAt(x, y) !== 0) return -1;
    for (let r = 1; r <= limit; r++) {
        for (let dy = -r; dy <= r; dy++) for (let dx = -r; dx <= r; dx++) {
            const k = kindAt(x + dx, y + dy);
            if (k !== 0 && k !== 3) return r - 1;
        }
    }
    return limit;
}

/**
 * Every cell that could hold a biome's entrance pad, with its clearance:
 * open ground within reach of the door, outside it, and clear of every
 * other pad. The pad wants two tiles of clearance; a door standing in a
 * narrow lane (the sewers') has no such cell, so the best clearance any
 * candidate offers is the bar, and the checker recomputes that bar.
 */
function entranceCandidates(map, terrain, biome, door, otherPads) {
    const w = map.width, h = map.height;
    const kindAt = (x, y) => (x < 0 || y < 0 || x >= w || y >= h ? 1 : terrain[y * w + x].tileId);
    const candidates = [];
    let bestClearance = -1;
    for (let y = door.y0 - RULES.entranceMaxTiles; y <= door.y1 + RULES.entranceMaxTiles; y++) {
        for (let x = door.x0 - RULES.entranceMaxTiles; x <= door.x1 + RULES.entranceMaxTiles; x++) {
            const gap = tileGap(x, y, door);
            if (gap === 0 || gap > RULES.entranceMaxTiles) continue;
            const clearance = clearanceAt(kindAt, x, y, RULES.entranceClearance);
            if (clearance < 0) continue;
            if (otherPads.some(([px, py]) => Math.max(Math.abs(px - x), Math.abs(py - y)) < RULES.entranceMinPadGap)) continue;
            candidates.push({ x, y, gap, clearance });
            bestClearance = Math.max(bestClearance, clearance);
        }
    }
    return { candidates: candidates.filter(c => c.clearance === bestClearance), clearance: bestClearance };
}

function worldPads(layer, except) {
    return layer.objects.filter(o => objectKind(o) === 'teleporter' && o.name !== except)
        .map(o => [Math.floor(o.x / TILE_SIZE), Math.floor(o.y / TILE_SIZE)]);
}

/**
 * Adds (or replaces, by object name) the nine `entrance_<biome>` pads: each
 * within reach of the biome's door, on open ground with the best clearance
 * from anything solid the ground there offers, outside the door, clear of
 * every other pad, leading to `<biome>_1`'s door. The chosen cell is the
 * qualifying one whose gap to the door is nearest four tiles -- far enough
 * that walking out of the door does not step straight onto it -- ties
 * broken by row, then column.
 */
function worldEntrances(ts) {
    const map = readWorld();
    const layers = layersOf(map);
    const layer = layers.teleporters;
    if (!layer) throw new Error('world.tmj has no teleporters layer');
    const terrain = terrainOf(map, ts, 'world');
    const placed = [];
    for (const biome of BIOMES) {
        const name = `entrance_${biome.id}`;
        const door = worldDoor(map, biome.id);
        const { candidates, clearance } = entranceCandidates(map, terrain, biome, door, worldPads(layer, name));
        let best = null;
        for (const c of candidates) {
            const score = Math.abs(c.gap - 4);
            if (!best || score < best.score || (score === best.score && (c.y < best.y || (c.y === best.y && c.x < best.x)))) best = { ...c, score };
        }
        if (!best) throw new Error(`no open cell within ${RULES.entranceMaxTiles} tiles of the world door "${biome.id}" can hold an entrance pad`);
        best.clearance = clearance;
        const existing = layer.objects.findIndex(o => o.name === name);
        const pad = sortKeys({
            height: 0, id: existing >= 0 ? layer.objects[existing].id : map.nextobjectid++, name, rotation: 0, type: 'teleporter', visible: true, width: 0,
            x: (best.x + 0.5) * TILE_SIZE, y: (best.y + 0.5) * TILE_SIZE, point: true,
            properties: [
                { name: 'targetMap', type: 'string', value: `${biome.id}_1` },
                { name: 'targetSpawn', type: 'string', value: `${biome.id}_1` },
            ],
        });
        if (existing >= 0) layer.objects[existing] = pad; else layer.objects.push(pad);
        placed.push({ biome: biome.id, x: best.x, y: best.y, gap: best.gap, clearance: best.clearance });
    }
    writeWorld(map);
    return placed;
}

/** Holds world.tmj to what the two world tools promise: nine regions, nine valid entrances. */
function checkWorld(ts, manifestIds, spawnIdCache) {
    const map = readMap(WORLD_FILE);
    const layers = layersOf(map);
    const fail = message => { throw new Error(`world: ${message}`); };
    const terrain = terrainOf(map, ts, 'world');
    const w = map.width, h = map.height;
    const ids = new Set();
    let maxId = 0;
    for (const layer of map.layers) for (const o of layer.objects || []) {
        if (ids.has(o.id)) fail(`object id ${o.id} repeats`);
        ids.add(o.id);
        maxId = Math.max(maxId, o.id);
    }
    if (map.nextobjectid <= maxId) fail(`nextobjectid ${map.nextobjectid} is not past the highest object id ${maxId}`);
    for (const biome of BIOMES) {
        const region = layers.spawns.objects.find(o => isRegion(o) && o.name === biome.id);
        if (!region) fail(`no mob region "${biome.id}" (run --world-regions)`);
        const sx = biome.section % 3, sy = Math.floor(biome.section / 3);
        if (region.x !== sx * SECTION_SIZE || region.y !== sy * SECTION_SIZE || region.width !== SECTION_SIZE || region.height !== SECTION_SIZE) fail(`region "${biome.id}" does not cover section ${biome.section}`);
        if (readProps(region).mobs !== biome.mobs) fail(`region "${biome.id}" names "${readProps(region).mobs}", not "${biome.mobs}"`);
    }
    const pads = layers.teleporters.objects.filter(o => objectKind(o) === 'teleporter');
    for (const biome of BIOMES) {
        const name = `entrance_${biome.id}`;
        const pad = pads.find(o => o.name === name);
        if (!pad) fail(`no teleporter "${name}" (run --world-entrances)`);
        const p = readProps(pad);
        if (p.targetMap !== `${biome.id}_1` || p.targetSpawn !== `${biome.id}_1`) fail(`${name} leads to ${p.targetMap}:${p.targetSpawn}`);
        if (!manifestIds.has(p.targetMap)) fail(`${name} targets map ${p.targetMap}, which maps.json does not list`);
        if (!spawnIdCache.has(p.targetMap)) spawnIdCache.set(p.targetMap, spawnIdsOf(path.join(MAPS_DIR, `${p.targetMap}.tmj`)));
        if (!spawnIdCache.get(p.targetMap).has(p.targetSpawn)) fail(`${name} targets spawn ${p.targetMap}:${p.targetSpawn}, which does not exist`);
        const door = worldDoor(map, biome.id);
        const x = Math.floor(pad.x / TILE_SIZE), y = Math.floor(pad.y / TILE_SIZE);
        const gap = tileGap(x, y, door);
        if (gap === 0) fail(`${name} sits in the door`);
        if (gap > RULES.entranceMaxTiles) fail(`${name} is ${gap} tiles from its door, more than ${RULES.entranceMaxTiles}`);
        if (terrain[y * w + x].tileId !== 0) fail(`${name} is not on open ground`);
        const kindAt = (px, py) => (px < 0 || py < 0 || px >= w || py >= h ? 1 : terrain[py * w + px].tileId);
        const { clearance } = entranceCandidates(map, terrain, biome, door, worldPads(layers.teleporters, name));
        const have = clearanceAt(kindAt, x, y, RULES.entranceClearance);
        if (have < clearance) fail(`${name} has ${have} tiles of clear ground around it; a cell near the door offers ${clearance}`);
        if (have < RULES.entranceClearance) console.warn(`world.tmj: entrance_${biome.id} stands in a lane with only ${have} tile(s) of clearance -- the door has no roomier ground within ${RULES.entranceMaxTiles} tiles`);
        for (const other of pads) {
            if (other === pad) continue;
            const ox = Math.floor(other.x / TILE_SIZE), oy = Math.floor(other.y / TILE_SIZE);
            if (Math.max(Math.abs(ox - x), Math.abs(oy - y)) < RULES.entranceMinPadGap) fail(`${name} is within ${RULES.entranceMinPadGap} tiles of teleporter ${other.id}`);
        }
    }
    // Every world pad that names a map must resolve.
    for (const pad of pads) {
        const p = readProps(pad);
        if (p.targetMap === undefined || p.targetSpawn === undefined) continue;
        if (!manifestIds.has(p.targetMap)) fail(`teleporter ${pad.id} targets map ${p.targetMap}, which maps.json does not list`);
        if (!spawnIdCache.has(p.targetMap)) spawnIdCache.set(p.targetMap, spawnIdsOf(path.join(MAPS_DIR, `${p.targetMap}.tmj`)));
        if (!spawnIdCache.get(p.targetMap).has(p.targetSpawn)) fail(`teleporter ${pad.id} targets spawn ${p.targetMap}:${p.targetSpawn}, which does not exist`);
    }
}

// ---------------------------------------------------------------------------
// Thumbnails
// ---------------------------------------------------------------------------

/**
 * An ASCII picture of a map, one character per `scale` tiles on each axis.
 * A block of tiles shows its commonest kind, ties going to the more solid.
 *   .  open        ,  floor decoration   #  wall    ~  water
 *   =  sand/bridge :  stone              D  door    N  next pad   H  home pad
 */
function thumbnail(map, ts, scale = 2) {
    const w = map.width, h = map.height;
    const terrain = terrainOf(map, ts, 'thumbnail');
    const glyphOf = rec => (rec.tileId === 0 ? (rec.gid ? ',' : '.') : rec.tileId === 1 ? '#' : rec.tileId === 2 ? '~' : rec.tileId === 3 ? '=' : ':');
    const priority = { '#': 6, '~': 5, ':': 4, '=': 3, ',': 2, '.': 1 };
    const marks = new Map();
    const door = layersOf(map).player_spawns.objects[0];
    marks.set(`${Math.floor((door.x / TILE_SIZE + 1) / scale)},${Math.floor((door.y / TILE_SIZE + 1) / scale)}`, 'D');
    layersOf(map).teleporters.objects.forEach((pad, i) => {
        marks.set(`${Math.floor(pad.x / TILE_SIZE / scale)},${Math.floor(pad.y / TILE_SIZE / scale)}`, i === 0 ? 'N' : 'H');
    });
    const rows = [];
    for (let y = 0; y < h; y += scale) {
        let line = '';
        for (let x = 0; x < w; x += scale) {
            const mark = marks.get(`${x / scale},${y / scale}`);
            if (mark) { line += mark; continue; }
            const counts = {};
            for (let dy = 0; dy < scale && y + dy < h; dy++) for (let dx = 0; dx < scale && x + dx < w; dx++) {
                const g = glyphOf(terrain[(y + dy) * w + x + dx]);
                counts[g] = (counts[g] || 0) + 1;
            }
            let best = '.';
            for (const g of Object.keys(counts)) {
                if (counts[g] > (counts[best] || 0) || (counts[g] === counts[best] && priority[g] > priority[best])) best = g;
            }
            line += best;
        }
        rows.push(line);
    }
    return rows.join('\n');
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

function parseArgs(argv) {
    const args = { thumbs: [], scale: 2, tileset: path.join(MAPS_DIR, 'terrain.tsj') };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a === '--check') args.check = true;
        else if (a === '--show') args.show = true;
        else if (a === '--thumb') args.thumbs.push(argv[++i]);
        else if (a === '--scale') args.scale = Number(argv[++i]);
        else if (a === '--tileset') args.tileset = path.resolve(argv[++i]);
        else if (a === '--world-regions') args.worldRegions = true;
        else if (a === '--world-entrances') args.worldEntrances = true;
        else throw new Error(`unknown argument ${a}`);
    }
    return args;
}

function main() {
    const args = parseArgs(process.argv.slice(2));
    const ts = loadTileset(args.tileset);
    const ground = loadGround(path.join(MAPS_DIR, 'ground.tsj'));
    if (ground.tilecount + ts.groundFirstGid - 1 <= ts.tilecount) throw new Error('the ground tileset would overlap the terrain tileset');

    const ids = [];
    for (const biome of BIOMES) for (let n = 1; n <= MAPS_PER_BIOME; n++) ids.push(`${biome.id}_${n}`);

    if (args.thumbs.length) {
        for (const id of args.thumbs) {
            const map = readMap(path.join(MAPS_DIR, `${id}.tmj`));
            console.log(`${id} ${map.width}x${map.height}`);
            console.log(thumbnail(map, ts, args.scale) + '\n');
        }
        return;
    }
    if (args.worldRegions) {
        const added = worldRegions();
        console.log(added.length ? `world.tmj: added mob regions ${added.join(', ')}` : 'world.tmj: every biome already has a mob region');
    }
    if (args.worldEntrances) {
        for (const p of worldEntrances(ts)) console.log(`world.tmj: entrance_${p.biome} at tile (${p.x},${p.y}), ${p.gap} tiles from the door, clearance ${p.clearance}`);
    }
    if (args.worldRegions || args.worldEntrances) {
        const listed = new Set(readMap(path.join(MAPS_DIR, 'maps.json')).maps.map(m => m.file.replace(/\.tmj$/, '')));
        checkWorld(ts, listed, new Map());
        console.log('world.tmj ok');
        return;
    }

    if (!args.check) {
        BIOMES.forEach((biome, biomeIndex) => {
            for (let n = 1; n <= MAPS_PER_BIOME; n++) {
                const { id, map } = generate(biome, biomeIndex, n, ts, ground);
                fs.writeFileSync(path.join(MAPS_DIR, `${id}.tmj`), JSON.stringify(map, null, 1) + '\n');
            }
        });
        fs.writeFileSync(path.join(MAPS_DIR, 'maps.json'), manifest(ids));
    }

    // Verify what is on disk, whichever mode: the checker only trusts files.
    const listed = readMap(path.join(MAPS_DIR, 'maps.json')).maps.map(m => m.file.replace(/\.tmj$/, ''));
    const manifestIds = new Set(listed);
    for (const id of ids) if (!manifestIds.has(id)) throw new Error(`maps.json does not list ${id}`);
    const spawnIdCache = new Map();
    let bandTotal = 0;
    const pct = v => `${String(Math.round(100 * v)).padStart(3)}%`;
    console.log(`${'map'.padEnd(12)} size     open water solid floor rooms sq  far next bands`);
    for (const id of ids) {
        const map = readMap(path.join(MAPS_DIR, `${id}.tmj`));
        const s = check(id, map, { ts, ground, manifestIds, spawnIdCache });
        bandTotal += s.bands;
        console.log(`${id.padEnd(12)} ${String(map.width).padStart(3)}x${String(map.height).padEnd(3)} ${pct(s.open)} ${pct(s.water)} ${pct(s.solid)} ${pct(s.floor)}  ${String(s.rooms).padStart(3)}  ${String(s.square).padStart(2)} ${String(s.farthest).padStart(4)} ${String(s.nextPad).padStart(4)} ${String(s.bands).padStart(4)}`);
        if (args.show) console.log(thumbnail(map, ts, args.scale) + '\n');
    }
    checkWorld(ts, manifestIds, spawnIdCache);
    console.log(`${ids.length} maps ok, ${bandTotal} tier bands, world.tmj ok${args.check ? '' : ', written to maps/ and maps/maps.json'}`);
}

if (require.main === module) {
    try {
        main();
    } catch (err) {
        console.error(err.message);
        process.exit(1);
    }
}

module.exports = { BIOMES, RULES, generate, check, thumbnail, loadTileset, loadGround };
