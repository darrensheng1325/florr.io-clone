#!/usr/bin/env node
/**
 * One-way conversion of the legacy map literal into the Tiled map that
 * replaces it.
 *
 *   node scripts/mapToTiled.js <map.json> [-o maps]
 *
 * The input is a `MapData` literal — a raw `{elements, wallGrid,
 * customTileTypes}` JSON, which is what `MapEditor.html` exports, or the
 * retired `src/map_source.ts` from before the migration.
 *
 * Writes `maps/world.tmj`, `maps/terrain.tsj` and the tile art under
 * `maps/tiles/`, then reads all of it back and checks that the result is the
 * map it started from. That check is the point of the script: a map conversion
 * that silently drops a spawn table or shifts a rectangle by a tile is worse
 * than no conversion, and the only way to know is to compare.
 *
 * The world map has already been converted, so this is here for an old branch,
 * a second map, or anything still coming out of MapEditor.html. Editing the
 * live map means editing `maps/world.tmj` in Tiled.
 */

const fs = require('fs');
const path = require('path');
const tiled = require('./lib/tiled');

const ROOT = path.resolve(__dirname, '..');

const args = process.argv.slice(2);
let input = null;
let outDir = path.join(ROOT, 'maps');
for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === '-o' || a === '--output') outDir = path.resolve(args[++i]);
    else if (!a.startsWith('-')) input = path.resolve(a);
}
// No default input: the map this once converted no longer exists, and
// defaulting to maps/world.tmj would mean overwriting the map with itself.
if (!input) {
    console.error('usage: node scripts/mapToTiled.js <map.json> [-o maps]');
    process.exit(2);
}

/**
 * The three built-in tile types, read out of constants.ts rather than copied.
 *
 * They are not in the map data — constants.ts registers them itself and
 * setCustomTileTypes() refuses to redefine them — but the tileset has to show
 * all six tiles or the map cannot be edited. Both literals are plain JS inside
 * the TypeScript file, so they are evaluated as written instead of transcribed
 * here, which would be one more copy to keep in step.
 */
function readBuiltinTileTypes() {
    const source = fs.readFileSync(path.join(ROOT, 'src', 'constants.ts'), 'utf8');
    const texture = source.match(/const BUILTIN_WALL_TEXTURE_SVG = (`[\s\S]*?`);/);
    const table = source.match(/export const BUILTIN_TILE_TYPES: TileTypeConfig\[\] = (\[[\s\S]*?\n\]);/);
    if (!texture || !table) throw new Error('could not locate BUILTIN_TILE_TYPES in src/constants.ts');
    const build = new Function(`const BUILTIN_WALL_TEXTURE_SVG = ${texture[1]};\nreturn ${table[1]};`);
    return build().map(config => ({ ...config, builtin: true }));
}

function loadLegacyMap(file) {
    // A Tiled map is accepted too, so the art and the tilesets can be
    // regenerated in place without a legacy map to convert from.
    if (tiled.isTiledMapFile(file)) return tiled.fromTiled(file);
    const raw = fs.readFileSync(file, 'utf8');
    if (path.extname(file).toLowerCase() === '.json') return JSON.parse(raw);
    const m = raw.match(/const WORLD_MAP_DATA\s*:\s*MapData\s*=\s*(\{[\s\S]*?\});\s*\n/);
    if (!m) throw new Error(`could not locate WORLD_MAP_DATA literal in ${file}`);
    return JSON.parse(m[1]);
}

/**
 * The nine ground types, from SECTION_CONFIGS in constants.ts.
 *
 * These used to be nailed to the map's 3x3 section grid -- which third of the
 * world you stood in decided what the ground was painted with. They become a
 * tileset here, so the background layer can say it per cell instead. The nine
 * are still the nine; what changes is that a map can now put any of them
 * anywhere.
 *
 * Art is fitted to one tile for the same reason the terrain art is, and it is
 * inert for the same reason: SvgDocument::renderFitted maps the viewBox into
 * the box it is given, so the root width/height is the editor's business only.
 */
function readGroundTypes() {
    const source = fs.readFileSync(path.join(ROOT, 'src', 'constants.ts'), 'utf8');
    const table = source.match(/export const SECTION_CONFIGS: SectionConfig\[\] = (\[[\s\S]*?\n\]);/);
    if (!table) throw new Error('could not locate SECTION_CONFIGS in src/constants.ts');
    const sections = new Function(`return ${table[1]};`)();
    return sections.map((section, id) => {
        const name = section.name.toLowerCase().replace(/\s+/g, '_');
        const background = section.background || '#000000';
        const isFile = background.endsWith('.svg');
        // A section that declares a colour rather than a file gets a file, so
        // every ground type is a tile the editor can show and the renderer can
        // load. The result is the same flat fill either way.
        const art = isFile ? background : `${name}.svg`;
        const svg = isFile
            ? tiled.fitSvgToTile(fs.readFileSync(path.join(ROOT, 'src', background), 'utf8'))
            : tiled.groundColorSvg(background);
        return { id, name, art, source: background, svg };
    });
}

/**
 * JSON with the tile layer wrapped one map row per line.
 *
 * Forty thousand numbers on a single line is a file no diff can say anything
 * useful about; one row per line makes a terrain edit show up as the rows it
 * actually touched. Tiled reads either and will reformat the file its own way
 * the first time it saves.
 */
function stringifyMap(map) {
    const marker = '@@TILE_DATA@@';
    const rows = [];
    const layer = map.layers.find(l => l.type === 'tilelayer');
    for (let y = 0; y < layer.height; y++) {
        rows.push('   ' + layer.data.slice(y * layer.width, (y + 1) * layer.width).join(','));
    }
    const text = JSON.stringify({ ...map, layers: map.layers.map(l => (l === layer ? { ...l, data: marker } : l)) }, null, 1);
    return text.replace(`"${marker}"`, `[\n${rows.join(',\n')}\n  ]`) + '\n';
}

function writeFile(file, contents) {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, contents);
    return `${path.relative(ROOT, file)} (${contents.length} bytes)`;
}

/** Reports the first difference between what went in and what came back. */
function diff(expected, actual, at = '') {
    if (expected === actual) return null;
    if (typeof expected !== typeof actual || expected === null || actual === null) {
        return `${at}: ${JSON.stringify(expected)} became ${JSON.stringify(actual)}`;
    }
    if (Array.isArray(expected)) {
        if (!Array.isArray(actual)) return `${at}: array became ${typeof actual}`;
        if (expected.length !== actual.length) {
            return `${at}: ${expected.length} entries became ${actual.length}`;
        }
        for (let i = 0; i < expected.length; i++) {
            const d = diff(expected[i], actual[i], `${at}[${i}]`);
            if (d) return d;
        }
        return null;
    }
    if (typeof expected === 'object') {
        const keys = new Set([...Object.keys(expected), ...Object.keys(actual)]);
        for (const key of keys) {
            const d = diff(expected[key], actual[key], `${at}.${key}`);
            if (d) return d;
        }
        return null;
    }
    return `${at}: ${JSON.stringify(expected)} became ${JSON.stringify(actual)}`;
}

(function main() {
    console.log(`[mapToTiled] reading: ${path.relative(ROOT, input)}`);
    const legacy = loadLegacyMap(input);
    const builtins = readBuiltinTileTypes();
    const grounds = readGroundTypes();
    const { map, tilesets, art } = tiled.toTiled(legacy, builtins, grounds);

    const written = [];
    for (const entry of art) written.push(writeFile(path.join(outDir, entry.file), entry.svg));
    for (const entry of tilesets) {
        written.push(writeFile(path.join(outDir, entry.file), JSON.stringify(entry.tileset, null, 1) + '\n'));
    }
    written.push(writeFile(path.join(outDir, tiled.MAP_FILE), stringifyMap(map)));
    for (const line of written) console.log(`[mapToTiled] wrote: ${line}`);

    // -- the check ----------------------------------------------------------
    // Element order is grouped by kind on the way out (one object layer each),
    // which every consumer is blind to but a naive comparison is not, so the
    // source is grouped the same way before the two are compared.
    const roundTrip = tiled.fromTiled(path.join(outDir, tiled.MAP_FILE));
    const grouped = tiled.OBJECT_LAYERS.flatMap(spec => legacy.elements.filter(e => e.type === spec.kind));
    const expected = {
        elements: grouped.map(e => ({ ...e, properties: e.properties || {} })),
        wallGrid: legacy.wallGrid,
        customTileTypes: (legacy.customTileTypes || []).map(t => (
            t.textureSvg ? { ...t, textureSvg: tiled.fitSvgToTile(t.textureSvg) } : t)),
    };
    const problem = diff(expected, {
        elements: roundTrip.elements,
        wallGrid: roundTrip.wallGrid,
        customTileTypes: roundTrip.customTileTypes,
    }, 'map');
    if (problem) {
        console.error(`[mapToTiled] ROUND TRIP FAILED — ${problem}`);
        process.exit(1);
    }
    // A background the source did not have was derived from the 3x3 section
    // grid, so it has to come back reading exactly as that grid did -- that is
    // the whole claim that the layer replaces sectionAt() rather than
    // redecorating the map.
    const wide = roundTrip.wallGrid[0].length;
    const expectedBackground = legacy.background && legacy.background.length === roundTrip.background.length
        ? legacy.background
        : tiled.backgroundFromSections(wide, roundTrip.wallGrid.length);
    const drift = roundTrip.background.findIndex((id, i) => id !== expectedBackground[i]);
    if (drift >= 0) {
        console.error(`[mapToTiled] ROUND TRIP FAILED — background cell ` +
                      `(${drift % wide},${Math.floor(drift / wide)}) is ground ` +
                      `${roundTrip.background[drift]}, expected ${expectedBackground[drift]}`);
        process.exit(1);
    }
    console.log(`[mapToTiled] round trip verified: ${roundTrip.elements.length} elements, ` +
                `${roundTrip.wallGrid.length}x${roundTrip.wallGrid[0].length} tiles, ` +
                `${roundTrip.customTileTypes.length} custom tile types, ` +
                `${roundTrip.groundTypes.length} ground types`);
})();
