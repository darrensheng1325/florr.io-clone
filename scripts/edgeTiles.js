#!/usr/bin/env node
/**
 * Edge tiles and tile skins: the wall / water lip is tile art, not a runtime
 * outline, and every biome has its own family of tiles.
 *
 * A wall or water tile shows a band along every side that faces open ground
 * (or, for a wall, water). That band is baked into the tileset as 15 variants
 * per base tile, one per combination of exposed sides, and the MAP says which
 * variant each cell uses. Nothing downstream recomputes exposure -- this script
 * is the only place the rule lives.
 *
 * Edge mask bits: n=1, e=2, s=4, w=8. A variant's name lists its exposed sides
 * in that fixed order ("wall_edge_ne", "water_sewers_edge_nsw"); mask 0 is the
 * plain base tile.
 *
 * Skins: skin 0 is the default family (wall.svg / water.svg and their edge
 * variants, ids 0..35 of maps/terrain.tsj, untouched). Skins 1..3 are the
 * biome families sewers, computer and unknown; each owns a fixed block of 35
 * ids after the default block (36..70, 71..105, 106..140; tilecount 141) --
 * see scripts/lib/tileArt.js for the layout. Every other biome (garden,
 * desert, hel, ocean, ant_hell, jungle) is drawn with the default family; the
 * engine's skinForGround() picks a skin from the ground under a default wall
 * or water cell. A skin's look lives in scripts/tileArt/<skin>.js; the
 * geometry every skin shares lives in scripts/lib/tileArt.js.
 *
 * Exposure rule (what jaggedEdgeExposed() in cpp/shared/game/terrain.cpp did):
 *   - a side is exposed when the neighbour on that side is off the map or is
 *     tileId 0 (air / open ground, including floor decorations);
 *   - a WALL side is also exposed when the neighbour is water (tileId 2);
 *   - water against wall, water against water, and anything against sand (3),
 *     stone (4) or block (5) shows nothing.
 *
 * Usage:
 *   node scripts/edgeTiles.js --art
 *       Regenerates maps/tiles/*.svg for every skin whose module exists and
 *       rewrites maps/terrain.tsj: ids 0..5 verbatim, 6..35 the default edge
 *       variants, then one 35-id block per skin in SKIN_NAMES (a listed skin
 *       whose module is not there yet gets placeholder entries so its ids and
 *       the ground tileset's firstgid are stable before its art lands; a skin
 *       that is not listed gets nothing).
 *
 *   node scripts/edgeTiles.js --art --skin <name> --out <dir>
 *       Preview: one skin's 35 SVGs into <dir>; the tileset and maps/tiles are
 *       left alone.
 *
 *   node scripts/edgeTiles.js --apply [--check] [--manifest maps/maps.json] [map.tmj ...]
 *   node scripts/edgeTiles.js --check [--manifest maps/maps.json] [map.tmj ...]
 *       For each map: moves the ground tileset's firstgid to terrain
 *       tilecount + 1 (remapping the background layer's gids), then normalises
 *       every wall/water cell of the "terrain" layer back to its skin's base
 *       tile, recomputes exposure, and writes the matching variant gids back.
 *       Floor decorations are left alone. Idempotent. A map is written only
 *       when something actually changes, so an unchanged map keeps its mtime
 *       (the C++ build stages the maps and relinks when one moves).
 *       --manifest appends every "file" the manifest lists, relative to the
 *       manifest's directory. --check reports instead of writing and exits 1
 *       if any map is stale.
 *
 * package.json's "build:map" runs the apply over the manifest before
 * scripts/encodeMap.js, so the bundle always sees an up-to-date map.
 */

const fs = require('fs');
const path = require('path');
const art = require('./lib/tileArt');

const ROOT = path.resolve(__dirname, '..');
const MAPS_DIR = path.join(ROOT, 'maps');
const TILESET_FILE = path.join(MAPS_DIR, 'terrain.tsj');
const TILES_DIR = path.join(MAPS_DIR, 'tiles');
const SKINS_DIR = path.join(__dirname, 'tileArt');

const { TILE, TILE_AIR, TILE_WALL, TILE_WATER, SIDES, SKIN_NAMES, SKIN_COUNT } = art;

const KINDS = [
    { tileId: TILE_WALL, family: 'wall' },
    { tileId: TILE_WATER, family: 'water' },
];

function readProperties(node) {
    const out = {};
    for (const entry of node && Array.isArray(node.properties) ? node.properties : []) {
        if (entry && typeof entry.name === 'string') out[entry.name] = entry.value;
    }
    return out;
}

function readJson(file) {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function serialize(doc) {
    // The same shape every map tool here writes: 1-space indent, trailing newline.
    return JSON.stringify(doc, null, 1) + '\n';
}

function writeIfChanged(file, text) {
    const unchanged = fs.existsSync(file) && fs.readFileSync(file, 'utf8') === text;
    if (!unchanged) fs.writeFileSync(file, text);
    return !unchanged;
}

function relative(file) {
    const inTree = path.relative(ROOT, file);
    return inTree.startsWith('..') ? path.relative(process.cwd(), file) || file : inTree;
}

// ---------------------------------------------------------------------------
// Skin modules
// ---------------------------------------------------------------------------

function skinModuleFile(skin) {
    return path.join(SKINS_DIR, `${SKIN_NAMES[skin] || 'default'}.js`);
}

/** The module of one skin, or null (with a message) when it is not there yet. */
function loadSkin(skin, { quiet } = {}) {
    const file = skinModuleFile(skin);
    if (!fs.existsSync(file)) {
        if (!quiet) console.log(`[edgeTiles] skin ${skin} "${SKIN_NAMES[skin]}": ${relative(file)} not found, skipped`);
        return null;
    }
    const mod = require(file);
    const expected = SKIN_NAMES[skin];
    if ((mod.name || '') !== expected) {
        throw new Error(`${relative(file)} exports name "${mod.name}", expected "${expected}"`);
    }
    for (const family of ['wall', 'water']) {
        const spec = mod[family];
        if (!spec || !spec.fill || !spec.band || !spec.profile) {
            throw new Error(`${relative(file)}: ${family} needs at least fill, band and profile`);
        }
        if (family === 'wall' && !spec.line) throw new Error(`${relative(file)}: wall needs a line colour`);
        if (family === 'water' && !spec.foam) throw new Error(`${relative(file)}: water needs a foam colour`);
    }
    if (skin !== 0 && (!Array.isArray(mod.floors) || mod.floors.length !== art.FLOOR_VARIANTS)) {
        throw new Error(`${relative(file)} must export exactly ${art.FLOOR_VARIANTS} floor stamps`);
    }
    return mod;
}

/** Everything --art renders for one skin: { family, key, name, svg }. */
function renderSkin(skin, mod) {
    const out = [];
    if (skin === 0) {
        for (const kind of KINDS) {
            for (let mask = 1; mask <= 15; mask++) {
                out.push({ family: kind.family, key: mask, name: art.className(0, kind.family, mask), svg: art.renderTile(mod, kind.family, mask) });
            }
        }
        return out;
    }
    for (const { family, key } of art.skinTiles()) {
        out.push({ family, key, name: art.className(skin, family, key), svg: art.renderTile(mod, family, key) });
    }
    return out;
}

// ---------------------------------------------------------------------------
// Tileset
// ---------------------------------------------------------------------------

/** Colours a skin's tileset entries carry before its module exists. */
function placeholderSkin(skin) {
    const p = art.SKIN_PALETTE[skin];
    return {
        name: SKIN_NAMES[skin],
        wall: { fill: p.wall.fill, band: p.wall.band },
        water: { fill: p.water.fill, band: p.water.band },
    };
}

/**
 * The tileset with every block in place: ids 0..5 verbatim from the file, the
 * default edge variants rebuilt from ids 1 and 2, then each skin's block from
 * its module (or a placeholder when the module is missing).
 */
function tilesetWithSkins(doc, skins) {
    const builtin = (doc.tiles || []).filter(tile => (tile.id | 0) < art.BUILTIN_CLASSES.length);
    builtin.sort((a, b) => (a.id | 0) - (b.id | 0));
    for (let id = 0; id < art.BUILTIN_CLASSES.length; id++) {
        const tile = builtin[id];
        if (!tile || (tile.id | 0) !== id || (tile.class || tile.type) !== art.BUILTIN_CLASSES[id]) {
            throw new Error(`${relative(TILESET_FILE)}: id ${id} must be "${art.BUILTIN_CLASSES[id]}"`);
        }
    }
    const tiles = [...builtin];

    // Default edge variants: the base tile's properties verbatim (tileId,
    // solid, water, style, colour, borderColor, builtin...), then the two that
    // make it an edge variant. `builtin` must travel too: scripts/lib/tiled.js
    // skips builtin tiles when it collects the custom palette, and without it
    // every variant would land in src/map_bundle.ts as a custom tile type
    // shadowing id 1 or 2.
    for (const kind of KINDS) {
        const base = builtin[kind.tileId];
        for (let mask = 1; mask <= 15; mask++) {
            const name = art.className(0, kind.family, mask);
            const file = `tiles/${name}.svg`;
            const properties = (base.properties || [])
                .filter(p => p.name !== 'edges' && p.name !== 'textureSvg')
                .map(p => ({ ...p }));
            properties.push({ name: 'edges', type: 'string', value: art.maskName(mask) });
            properties.push({ name: 'textureSvg', type: 'string', value: file });
            properties.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
            tiles.push({ id: art.localId(0, kind.family, mask), class: name, image: file, imagewidth: TILE, imageheight: TILE, properties });
        }
    }

    const placeholders = [];
    for (let skin = 1; skin < SKIN_COUNT; skin++) {
        const mod = skins[skin] || placeholderSkin(skin);
        if (!skins[skin]) placeholders.push(SKIN_NAMES[skin]);
        for (const { family, key } of art.skinTiles()) tiles.push(art.tilesetEntry(skin, mod, family, key));
    }
    tiles.sort((a, b) => a.id - b.id);
    for (let i = 0; i < tiles.length; i++) if (tiles[i].id !== i) throw new Error(`tileset id ${i} is missing`);
    const out = { ...doc, tiles };
    out.tilecount = tiles.length;
    if (out.tilecount !== art.TILE_COUNT) throw new Error(`tileset has ${out.tilecount} tiles, expected ${art.TILE_COUNT}`);
    return { tileset: out, placeholders };
}

function runArt({ skin: only, out }) {
    if (only !== undefined) {
        // Preview mode: one skin's art into a directory, nothing else touched.
        const skin = art.skinIndex(only);
        const mod = loadSkin(skin);
        if (!mod) throw new Error(`skin "${only}" has no module at ${relative(skinModuleFile(skin))}`);
        if (!out) throw new Error('--skin needs --out <dir>');
        fs.mkdirSync(out, { recursive: true });
        let written = 0;
        for (const tile of renderSkin(skin, mod)) {
            if (writeIfChanged(path.join(out, `${tile.name}.svg`), tile.svg)) written++;
        }
        console.log(`[edgeTiles] ${relative(out)}: skin "${SKIN_NAMES[skin] || 'default'}" previewed, ${written} file(s) written`);
        return;
    }

    const skins = [];
    let files = 0;
    let written = 0;
    for (let skin = 0; skin < SKIN_COUNT; skin++) {
        const mod = loadSkin(skin);
        skins[skin] = mod;
        if (!mod) continue;
        let count = 0;
        for (const tile of renderSkin(skin, mod)) {
            if (writeIfChanged(path.join(TILES_DIR, `${tile.name}.svg`), tile.svg)) written++;
            count++;
        }
        files += count;
        console.log(`[edgeTiles] skin ${skin} "${SKIN_NAMES[skin] || 'default'}": ${count} tiles, ids ${art.skinFirstId(skin)}..${art.skinFirstId(skin) + (skin === 0 ? art.DEFAULT_TILE_COUNT : art.SKIN_BLOCK) - 1}`);
    }
    const { tileset, placeholders } = tilesetWithSkins(readJson(TILESET_FILE), skins);
    const changed = writeIfChanged(TILESET_FILE, serialize(tileset));
    console.log(`[edgeTiles] ${relative(TILES_DIR)}: ${files} SVGs, ${written} written`);
    console.log(`[edgeTiles] ${relative(TILESET_FILE)}: ${tileset.tilecount} tiles` +
        (placeholders.length ? ` (placeholder entries, no art yet: ${placeholders.join(', ')})` : '') +
        `${changed ? '' : ', unchanged'}`);
}

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------

function tileLayers(layers, out = []) {
    for (const layer of layers || []) {
        if (layer.type === 'tilelayer') out.push(layer);
        else if (layer.type === 'group') tileLayers(layer.layers, out);
    }
    return out;
}

/** A map's tileset references, each with its parsed tileset and its role. */
function loadTilesets(mapPath, map) {
    const baseDir = path.dirname(mapPath);
    const out = [];
    for (const reference of map.tilesets || []) {
        const tileset = reference.source ? readJson(path.resolve(baseDir, reference.source)) : reference;
        const tiles = tileset.tiles || [];
        const ground = tiles.some(tile => readProperties(tile).groundId !== undefined);
        const terrain = tiles.some(tile => readProperties(tile).tileId !== undefined);
        if (ground && terrain) {
            throw new Error(`${reference.source || 'embedded tileset'} mixes ground and terrain tiles`);
        }
        const maxId = tiles.reduce((max, tile) => Math.max(max, tile.id | 0), -1);
        out.push({ reference, tileset, role: ground ? 'ground' : 'terrain', count: Math.max(tileset.tilecount | 0, maxId + 1) });
    }
    return out;
}

/**
 * Moves the ground tileset to firstgid = (terrain firstgid + terrain
 * tilecount) and shifts every gid of the non-terrain tile layers along with
 * it. Returns the old -> new firstgid pair, or null when nothing moved.
 */
function fixGroundFirstGid(rel, map, sets, terrainLayer) {
    const terrain = sets.find(set => set.role === 'terrain');
    const ground = sets.find(set => set.role === 'ground');
    if (!terrain || !ground) return null;
    const expected = (terrain.reference.firstgid | 0) + terrain.count;
    const old = ground.reference.firstgid | 0;
    if (old === expected) return null;
    const delta = expected - old;
    for (const layer of tileLayers(map.layers)) {
        if (layer === terrainLayer) continue;
        if (!Array.isArray(layer.data)) throw new Error(`${rel}: tile layer "${layer.name}" is not a plain array`);
        for (let i = 0; i < layer.data.length; i++) {
            const raw = layer.data[i] >>> 0;
            const gid = raw & 0x1fffffff;
            if (gid === 0) continue;
            if (gid < old || gid >= old + ground.count) {
                throw new Error(`${rel}: layer "${layer.name}" cell ${i} uses gid ${gid}, outside the ground tileset (${old}..${old + ground.count - 1})`);
            }
            layer.data[i] = ((raw & ~0x1fffffff) | (gid + delta)) >>> 0;
        }
    }
    ground.reference.firstgid = expected;
    return { old, expected };
}

function refuseOverlap(rel, sets) {
    const sorted = sets.slice().sort((a, b) => (a.reference.firstgid | 0) - (b.reference.firstgid | 0));
    for (let i = 1; i < sorted.length; i++) {
        const prev = sorted[i - 1];
        const next = sorted[i];
        const prevEnd = (prev.reference.firstgid | 0) + prev.count - 1;
        if ((next.reference.firstgid | 0) <= prevEnd) {
            throw new Error(`${rel}: tilesets ${prev.reference.source || 'embedded'} (gids ${prev.reference.firstgid}..${prevEnd}) and ` +
                `${next.reference.source || 'embedded'} (firstgid ${next.reference.firstgid}) overlap`);
        }
    }
}

/**
 * gid -> { tileId, skin } for every terrain tile a map can see, and
 * (tileId, skin, mask) -> gid for the wall / water variants.
 */
function loadPalette(rel, sets) {
    const tileOfGid = new Map([[0, { tileId: TILE_AIR, skin: 0 }]]);   // an empty cell is air
    const gidOfVariant = new Map();
    for (const set of sets) {
        if (set.role !== 'terrain') continue;
        const firstgid = set.reference.firstgid | 0;
        for (const tile of set.tileset.tiles || []) {
            const props = readProperties(tile);
            const gid = firstgid + (tile.id | 0);
            const tileId = props.tileId !== undefined ? props.tileId | 0 : tile.id | 0;
            const skin = art.skinIndex(props.skin);
            tileOfGid.set(gid, { tileId, skin });
            if (tileId !== TILE_WALL && tileId !== TILE_WATER) continue;
            const mask = art.maskOf(props.edges);
            const key = `${tileId}:${skin}:${mask}`;
            if (gidOfVariant.has(key)) {
                throw new Error(`${set.reference.source || 'embedded tileset'} defines tileId ${tileId} skin "${props.skin || ''}" ` +
                    `edges "${art.maskName(mask)}" twice (gids ${gidOfVariant.get(key)} and ${gid})`);
            }
            gidOfVariant.set(key, gid);
        }
    }
    const variantGid = (tileId, skin, mask) => {
        const gid = gidOfVariant.get(`${tileId}:${skin}:${mask}`);
        if (gid === undefined) {
            const family = tileId === TILE_WALL ? 'wall' : 'water';
            throw new Error(`no tileset of ${rel} defines ${art.className(skin, family, mask)}; run \`node scripts/edgeTiles.js --art\` first`);
        }
        return gid;
    };
    return { tileOfGid, variantGid };
}

function exposedMask(ids, width, height, x, y) {
    const self = ids[y * width + x];
    let mask = 0;
    for (const side of SIDES) {
        const nx = x + side.dx;
        const ny = y + side.dy;
        let exposed;
        if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
            exposed = true;
        } else {
            const other = ids[ny * width + nx];
            exposed = other === TILE_AIR || (self === TILE_WALL && other === TILE_WATER);
        }
        if (exposed) mask |= side.bit;
    }
    return mask;
}

function applyToMap(mapPath, { check }) {
    const rel = relative(mapPath);
    const original = fs.readFileSync(mapPath, 'utf8');
    const map = JSON.parse(original);
    if (map.type !== 'map' || !Array.isArray(map.layers)) throw new Error(`${rel} is not a Tiled map`);
    if (map.infinite) throw new Error(`${rel} is an infinite map; the game's grid is fixed`);

    const layers = tileLayers(map.layers);
    const terrain = layers.find(layer => layer.name === 'terrain') || layers[0];
    if (!terrain) throw new Error(`${rel} has no tile layer`);
    if (!Array.isArray(terrain.data)) {
        throw new Error(`${rel}: tile layer "${terrain.name}" is not a plain array ` +
            `(encoding ${terrain.encoding || 'chunks'}${terrain.compression ? ', ' + terrain.compression : ''}); ` +
            'save it as CSV (Map > Map Properties > Tile Layer Format) first');
    }
    const width = terrain.width | 0;
    const height = terrain.height | 0;
    const data = terrain.data;
    if (data.length !== width * height) {
        throw new Error(`${rel}: tile layer "${terrain.name}" holds ${data.length} cells, expected ${width * height}`);
    }

    const sets = loadTilesets(mapPath, map);
    const moved = fixGroundFirstGid(rel, map, sets, terrain);
    refuseOverlap(rel, sets);
    const { tileOfGid, variantGid } = loadPalette(rel, sets);

    // Normalise: every cell to its game tile id and skin. A variant's id is
    // its base tile's, so this is where "wall_sewers_edge_ne" becomes plain
    // sewers wall again. Floor decorations are air and stay as they are.
    const ids = new Uint8Array(width * height);
    const skins = new Uint8Array(width * height);
    for (let i = 0; i < data.length; i++) {
        const gid = (data[i] >>> 0) & 0x1fffffff;   // Tiled packs flip flags in the top bits
        const tile = tileOfGid.get(gid);
        if (tile === undefined) {
            throw new Error(`${rel}: tile (${i % width},${Math.floor(i / width)}) uses gid ${gid}, which no terrain tileset defines`);
        }
        ids[i] = tile.tileId;
        skins[i] = tile.skin;
    }

    const counts = {};
    for (const kind of KINDS) counts[kind.tileId] = { cells: 0, edged: 0, skinned: 0 };
    let changed = 0;
    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const i = y * width + x;
            const id = ids[i];
            if (id !== TILE_WALL && id !== TILE_WATER) continue;
            const mask = exposedMask(ids, width, height, x, y);
            // A flipped edge tile would wear its lip on the wrong side, so the
            // flags do not survive; nothing here was ever meant to be flipped.
            const gid = variantGid(id, skins[i], mask);
            counts[id].cells++;
            if (mask !== 0) counts[id].edged++;
            if (skins[i] !== 0) counts[id].skinned++;
            if (data[i] !== gid) {
                data[i] = gid;
                changed++;
            }
        }
    }

    const text = serialize(map);
    const stale = text !== original;
    const details = [`${changed} cells`];
    if (moved) details.push(`ground firstgid ${moved.old} -> ${moved.expected}`);
    let status;
    if (!stale) status = 'unchanged';
    else if (check) status = `STALE (${details.join(', ')} would change)`;
    else {
        fs.writeFileSync(mapPath, text);
        status = `written (${details.join(', ')} changed)`;
    }
    const wall = counts[TILE_WALL];
    const water = counts[TILE_WATER];
    console.log(`[edgeTiles] ${rel}: ${width}x${height}, ` +
        `${wall.cells} wall cells (${wall.edged} edge variants, ${wall.skinned} skinned), ` +
        `${water.cells} water cells (${water.edged} edge variants, ${water.skinned} skinned) -- ${status}`);
    return stale;
}

function runApply(files, options) {
    if (files.length === 0) throw new Error('--apply needs at least one map (a .tmj path or --manifest)');
    let stale = 0;
    for (const file of files) if (applyToMap(file, options)) stale++;
    if (options.check && stale > 0) {
        console.error(`[edgeTiles] ${stale} map(s) need \`node scripts/edgeTiles.js --apply\``);
        process.exit(1);
    }
}

// ---------------------------------------------------------------------------

function usage() {
    console.log([
        'Usage:',
        '  node scripts/edgeTiles.js --art',
        '      regenerate maps/tiles/*.svg for every skin in scripts/tileArt/ and rewrite maps/terrain.tsj',
        '  node scripts/edgeTiles.js --art --skin <name> --out <dir>',
        '      preview one skin\'s 35 SVGs into <dir>; the tileset is left alone',
        '  node scripts/edgeTiles.js --apply [--check] [--manifest maps/maps.json] [map.tmj ...]',
        '      rewrite each map\'s "terrain" layer so wall/water cells use the edge variant of their',
        '      skin matching their exposure, and move the ground tileset to firstgid terrain tilecount+1',
        '      (--check: report stale maps and exit 1 instead of writing)',
        '',
        `  skins: ${SKIN_NAMES.map((name, i) => `${i}=${name || 'default'}`).join(' ')}`,
    ].join('\n'));
}

(function main() {
    const args = process.argv.slice(2);
    let doArt = false;
    let apply = false;
    let check = false;
    let skin;
    let out;
    const files = [];
    for (let i = 0; i < args.length; i++) {
        const a = args[i];
        if (a === '--art') doArt = true;
        else if (a === '--apply') apply = true;
        else if (a === '--check') check = true;
        else if (a === '--skin') skin = args[++i];
        else if (a === '--out') out = path.resolve(args[++i] || '');
        else if (a === '--manifest') {
            const manifestPath = path.resolve(args[++i] || '');
            const manifest = readJson(manifestPath);
            if (!Array.isArray(manifest.maps)) throw new Error(`${manifestPath} has no "maps" array`);
            for (const entry of manifest.maps) {
                if (!entry || typeof entry.file !== 'string') throw new Error(`${manifestPath}: every map needs a "file"`);
                files.push(path.resolve(path.dirname(manifestPath), entry.file));
            }
        } else if (a === '-h' || a === '--help') {
            usage();
            process.exit(0);
        } else if (a.startsWith('-')) {
            throw new Error(`unknown flag ${a}`);
        } else {
            files.push(path.resolve(a));
        }
    }
    if (check && !doArt) apply = true;   // --check alone is the dry run of --apply
    if (!doArt && !apply) {
        usage();
        process.exit(1);
    }
    if ((skin !== undefined || out !== undefined) && !doArt) throw new Error('--skin / --out only make sense with --art');
    if (doArt) runArt({ skin, out });
    if (apply) runApply(files, { check });
})();
