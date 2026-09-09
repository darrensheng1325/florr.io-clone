'use strict';
/**
 * The world map, in Tiled's format.
 *
 * Tiled (mapeditor.org) is the map editor now, and `maps/world.tmj` is the
 * canonical map. This module is the only place that knows how the game's map
 * model — a tile grid plus a list of annotation rectangles — is spelled in
 * Tiled's JSON, and it goes both ways so a round trip is checkable.
 *
 * Two conventions carry the whole translation:
 *
 *   - ONE TILED PIXEL IS ONE WORLD UNIT. The map's tile size is WALL_TILE_SIZE
 *     (300), so every object's x/y/width/height in the .tmj is already a world
 *     coordinate and nothing is scaled on the way in or out. Getting this wrong
 *     is not a rounding error, it is a map that no longer lines up with itself.
 *
 *   - THE TILESET IS THE TILE PALETTE. `maps/terrain.tsj` carries one tile per
 *     game tile id with the id, name, solid/water flags and style on it as
 *     custom properties, and its tile images are the SVGs the game paints those
 *     tiles with. Adding a tile type to the map therefore means adding a tile to
 *     the tileset, which is what an editor can actually do.
 *
 *   - THE BACKGROUND LAYER IS THE GROUND. It used to be `sectionAt()`: which
 *     third of the map you stood in decided what the ground was painted with,
 *     and the nine artworks were nailed to a 3x3 grid nobody could move. The
 *     map says it now, cell by cell, in the `background` layer.
 */

const fs = require('fs');
const path = require('path');

/** What Tiled writes into files it saves; matched so re-saving is a no-op. */
const TILED_VERSION = '1.10.2';
const FORMAT_VERSION = '1.10';

/** World units per tile — constants.WALL_TILE_SIZE. See the 1:1 rule above. */
const TILE_SIZE = 300;

const TILESET_NAME = 'terrain';
const TILESET_FILE = `${TILESET_NAME}.tsj`;
const GROUND_TILESET_NAME = 'ground';
const GROUND_TILESET_FILE = `${GROUND_TILESET_NAME}.tsj`;
const MAP_FILE = 'world.tmj';
const TILE_ART_DIR = 'tiles';
const GROUND_ART_DIR = 'ground';

/// World units per map section, back when the ground came from a 3x3 grid.
/// Only the converter uses it, to paint a background layer that starts out
/// looking exactly like what it replaces.
const SECTION_SIZE = 20000;
const SECTIONS_PER_AXIS = 3;

/** Object layers, in the order their elements are concatenated on import. */
const OBJECT_LAYERS = [
    { name: 'spawns', kind: 'spawn' },
    { name: 'biomes', kind: 'biome' },
    { name: 'teleporters', kind: 'teleporter' },
];

// ---------------------------------------------------------------------------
// Custom properties
// ---------------------------------------------------------------------------

/** Tiled stores custom properties as a typed list; the game thinks in objects. */
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

function readProperties(node) {
    const out = {};
    for (const entry of node && Array.isArray(node.properties) ? node.properties : []) {
        if (entry && typeof entry.name === 'string') out[entry.name] = entry.value;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Tile art
// ---------------------------------------------------------------------------

/**
 * Rewrites an SVG's root width/height to exactly one tile, leaving every child
 * untouched and giving the document a viewBox if it had none.
 *
 * Tiled draws an image-collection tile at the image's own size, so a 400-unit
 * texture would hang 100 units out of its 300-unit cell and the map would not
 * line up with itself in the editor. The game never reads these attributes —
 * map-drawing.ts rasterizes the SVG with `drawImage(img, 0, 0, WALL_TILE_SIZE,
 * WALL_TILE_SIZE)`, stretching whatever it is handed to one tile — so pinning
 * them is inert everywhere except in Tiled, where it is the difference between
 * a readable map and a smeared one.
 *
 * Inert, that is, only while a viewBox says what the coordinates mean. Without
 * one, width/height ARE the coordinate space, and rewriting them silently
 * redefines it: land.svg draws its grass out to 400 with no viewBox, so pinning
 * it to 300 cropped the whole bottom-right quarter away and cut every shape the
 * crop crossed in half — a hard seam down each tile edge, in the editor and in
 * the game both. So the old width/height is written back as the viewBox first,
 * which is the same document said explicitly.
 */
function fitSvgToTile(svg) {
    const open = svg.match(/<svg\b[^>]*>/);
    if (!open) return svg;
    let tag = open[0];
    const width = tag.match(/\swidth\s*=\s*"([\d.]+)"/);
    const height = tag.match(/\sheight\s*=\s*"([\d.]+)"/);
    const box = /\sviewBox\s*=/.test(tag) ? '' : width && height
        ? ` viewBox="0 0 ${width[1]} ${height[1]}"` : '';
    tag = tag.replace(/\s(width|height)\s*=\s*"[^"]*"/g, '');
    tag = tag.replace(/<svg\b/, `<svg width="${TILE_SIZE}" height="${TILE_SIZE}"${box}`);
    return svg.slice(0, open.index) + tag + svg.slice(open.index + open[0].length);
}

/**
 * Tile art for a type the game paints from a colour rather than from an SVG.
 *
 * Air, water and the block tile carry no `textureSvg`: the renderer fills them
 * with `color` and nothing else -- world_renderer.cpp's kTileColor() is that
 * one fill. So this is that one fill, and deliberately nothing more. Palette
 * art that invents a border or a texture the game does not draw is worse than
 * no art: it tells whoever is editing the map that the tile looks like
 * something it does not.
 *
 * Air's colour is fully transparent, and it comes out as an empty image on
 * purpose -- air is where the background layer shows through, in Tiled exactly
 * as in the game.
 *
 * Water's shoreline is not here either. The renderer draws it as a separate
 * pass over every EXPOSED edge, with an outline generated from the tile's own
 * coordinates (terrain.h's jaggedEdge), so it belongs to a pair of tiles
 * rather than to one, and no single palette image can be honest about it.
 */
function paletteSvg(config) {
    const size = TILE_SIZE;
    const open = `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">`;
    const alpha = alphaOf(config.color);
    if (alpha === 0) return `${open}\n</svg>\n`;
    const opacity = alpha === 1 ? '' : ` fill-opacity="${alpha}"`;
    return `${open}\n  <rect x="0" y="0" width="${size}" height="${size}" ` +
           `fill="${normalizeHex(config.color)}"${opacity}/>\n</svg>\n`;
}

/** Ground art, for a section that declares a colour instead of a file. */
function groundColorSvg(color) {
    const size = TILE_SIZE;
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">\n` +
           `  <rect x="0" y="0" width="${size}" height="${size}" fill="${normalizeHex(color)}"/>\n</svg>\n`;
}

/** The alpha of a `#RRGGBBAA`, or 1 for the shorter forms. */
function alphaOf(color) {
    if (typeof color !== 'string' || color.length !== 9) return 1;
    return parseInt(color.slice(7), 16) / 255;
}

function normalizeHex(color) {
    if (typeof color !== 'string' || !color.startsWith('#')) return '#000000';
    // `#RRGGBBAA` down to `#RRGGBB`: the palette art is opaque by construction.
    return color.length === 9 ? color.slice(0, 7) : color;
}

// ---------------------------------------------------------------------------
// Elements <-> objects
// ---------------------------------------------------------------------------

/**
 * A biome's spawn table is a list of records, and Tiled has no property type
 * for that — its custom types cannot hold an array of structs. It travels as a
 * JSON string, which Tiled edits in its multi-line string editor and which
 * round-trips exactly.
 */
function encodeSpawnTable(table) {
    return JSON.stringify(table, null, 1).replace(/\n\s*/g, ' ');
}

/**
 * A spawn zone's outline, as a Tiled polygon.
 *
 * Tiled writes polygon points RELATIVE to the object's own x/y, so the object
 * anchors at the outline's first point and the points are offsets from it. A
 * zone that is still a plain rectangle is written as its four corners rather
 * than as a rectangle object: the whole point of the change is that a tier band
 * can follow a coastline, and a shape you cannot add a vertex to without first
 * converting it is a shape nobody converts.
 */
function polygonOf(element) {
    if (Array.isArray(element.polygon) && element.polygon.length >= 3) return element.polygon;
    return [
        { x: element.x, y: element.y },
        { x: element.x + element.width, y: element.y },
        { x: element.x + element.width, y: element.y + element.height },
        { x: element.x, y: element.y + element.height },
    ];
}

function objectFromElement(element, id) {
    const p = element.properties || {};
    const object = {
        id,
        name: '',
        class: element.type,
        x: element.x,
        y: element.y,
        width: element.width,
        height: element.height,
        rotation: 0,
        visible: true,
    };
    if (element.type === 'spawn') {
        const points = polygonOf(element);
        // The anchor is the first point, so a dragged object moves its whole
        // outline and the offsets stay put.
        object.x = points[0].x;
        object.y = points[0].y;
        object.width = 0;
        object.height = 0;
        object.polygon = points.map(point => ({ x: point.x - points[0].x, y: point.y - points[0].y }));
    }
    const custom = {};
    if (element.type === 'teleporter') {
        // Every teleporter in the map is authored as a point: the pad has no
        // extent, and map_elements.cpp keeps zero-sized teleporters for exactly
        // that reason. Tiled's point object is the same statement.
        object.point = true;
        object.width = 0;
        object.height = 0;
        if (p.teleportTo) {
            custom.teleportToX = p.teleportTo.x;
            custom.teleportToY = p.teleportTo.y;
            if (p.teleportTo.serverPort !== undefined) custom.serverPort = p.teleportTo.serverPort;
        }
    }
    if (p.spawnType !== undefined) custom.spawnType = p.spawnType;
    // The zone's mob distribution, verbatim. It is parsed on the C++ side --
    // only the server has a view of what mobs exist — so this layer's whole job
    // is not to mangle the string.
    if (p.mobs !== undefined) custom.mobs = p.mobs;
    if (p.biomeName !== undefined) custom.biomeName = p.biomeName;
    if (p.backgroundTexture !== undefined) custom.backgroundTexture = p.backgroundTexture;
    if (p.isNoCombat !== undefined) custom.isNoCombat = p.isNoCombat;
    if (Array.isArray(p.spawnTable)) custom.spawnTable = encodeSpawnTable(p.spawnTable);

    const properties = propertyList(custom);
    if (properties.length) object.properties = properties;
    return object;
}

/** The axis-aligned box around a set of points. */
function boundsOf(points) {
    let minX = points[0].x, maxX = points[0].x, minY = points[0].y, maxY = points[0].y;
    for (const point of points) {
        if (point.x < minX) minX = point.x;
        if (point.x > maxX) maxX = point.x;
        if (point.y < minY) minY = point.y;
        if (point.y > maxY) maxY = point.y;
    }
    return { x: minX, y: minY, width: maxX - minX, height: maxY - minY };
}

function elementFromObject(object, kind, context) {
    const custom = readProperties(object);
    const properties = {};
    if (custom.spawnType !== undefined) properties.spawnType = custom.spawnType;
    if (custom.mobs !== undefined) properties.mobs = custom.mobs;
    if (custom.biomeName !== undefined) properties.biomeName = custom.biomeName;
    if (custom.backgroundTexture !== undefined) properties.backgroundTexture = custom.backgroundTexture;
    if (custom.isNoCombat !== undefined) properties.isNoCombat = custom.isNoCombat;
    if (custom.spawnTable !== undefined) {
        let table;
        try {
            table = JSON.parse(custom.spawnTable);
        } catch (err) {
            throw new Error(`${context}: spawnTable is not valid JSON (${err.message})`);
        }
        if (!Array.isArray(table)) throw new Error(`${context}: spawnTable must be a JSON array`);
        properties.spawnTable = table;
    }
    if (custom.teleportToX !== undefined || custom.teleportToY !== undefined) {
        properties.teleportTo = { x: custom.teleportToX || 0, y: custom.teleportToY || 0 };
        if (custom.serverPort !== undefined) properties.teleportTo.serverPort = custom.serverPort;
    }
    // A polygon object carries its points relative to its own x/y; the game
    // wants world coordinates, and the rectangle it reports is the AABB. A
    // rectangle object still works and stays a rectangle -- there is nothing to
    // gain from writing four points for a shape that is four points.
    if (Array.isArray(object.polygon) && object.polygon.length >= 3) {
        const points = object.polygon.map(point => ({
            x: (object.x || 0) + point.x,
            y: (object.y || 0) + point.y,
        }));
        return { type: kind, ...boundsOf(points), polygon: points, properties };
    }
    if (Array.isArray(object.polyline)) {
        throw new Error(`${context}: a zone is an area, not a polyline; close it into a polygon`);
    }
    return {
        type: kind,
        x: object.x || 0,
        y: object.y || 0,
        width: object.width || 0,
        height: object.height || 0,
        properties,
    };
}

// ---------------------------------------------------------------------------
// Export: game map -> Tiled
// ---------------------------------------------------------------------------

/**
 * The background layer a map that has never had one should start with.
 *
 * The ground used to come from `sectionAt()` -- the 3x3 grid of 20000-unit
 * sections -- so painting each cell with the section it sits in is what makes
 * the layer a faithful replacement rather than a redecoration.
 *
 * The seams land exactly where they did, and that is arithmetic rather than
 * luck: the renderer tiles ground art every 400 units, so the ground tile
 * nearest a section boundary has its centre 200 units away from it, and the
 * 300-unit cell containing that centre has its own centre within 150 units of
 * it. 150 < 200, so the cell and the ground tile are always on the same side.
 */
function backgroundFromSections(width, height) {
    const data = new Array(width * height);
    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const cx = Math.floor((x + 0.5) * TILE_SIZE / SECTION_SIZE);
            const cy = Math.floor((y + 0.5) * TILE_SIZE / SECTION_SIZE);
            const inside = cx >= 0 && cy >= 0 && cx < SECTIONS_PER_AXIS && cy < SECTIONS_PER_AXIS;
            data[y * width + x] = inside ? cy * SECTIONS_PER_AXIS + cx : -1;
        }
    }
    return data;
}

/**
 * Builds every file that makes up the Tiled map.
 *
 * @param {{elements, wallGrid, customTileTypes, background?}} mapData
 * @param {Array} builtinTileTypes  ids 0-2, which the map data does not carry.
 * @param {Array} groundTypes  one per ground id: {id, name, art, svg}.
 * @returns {{map, tilesets: Array, art: Array<{file, svg}>}}
 */
function toTiled(mapData, builtinTileTypes, groundTypes) {
    const height = mapData.wallGrid.length;
    const width = mapData.wallGrid[0].length;

    const palette = [...builtinTileTypes, ...(mapData.customTileTypes || [])]
        .slice()
        .sort((a, b) => a.id - b.id);

    // Local tileset index -> game tile id. Kept as an explicit map rather than
    // assumed to be the identity, so a map whose palette skips an id (or starts
    // above 0) still loads: the game tile id lives on the tile as a property.
    const art = [];
    const tiles = palette.map((config, index) => {
        const file = `${TILE_ART_DIR}/${config.name}.svg`;
        art.push({ file, svg: config.textureSvg ? fitSvgToTile(config.textureSvg) : paletteSvg(config) });
        const custom = {
            tileId: config.id,
            solid: !!config.solid,
            water: !!config.water,
            style: config.style || 'flat',
            color: config.color,
        };
        if (config.borderColor) custom.borderColor = config.borderColor;
        // Present only where the game really paints the tile with this SVG. Its
        // absence is what tells the importer that `tiles/air.svg` and friends
        // are the editor's palette art and must not become map data.
        if (config.textureSvg) custom.textureSvg = file;
        if (config.textureTileSize) custom.textureTileSize = config.textureTileSize;
        // ids 0-2 are owned by constants.ts and are re-added there on load, so
        // they must not be written back into the bundle's custom palette.
        if (config.builtin) custom.builtin = true;
        return {
            id: index,
            class: config.name,
            image: file,
            imagewidth: TILE_SIZE,
            imageheight: TILE_SIZE,
            properties: propertyList(custom),
        };
    });

    const tileset = {
        columns: 0,           // an image collection, not a grid-sliced sheet
        grid: { height: TILE_SIZE, orientation: 'orthogonal', width: TILE_SIZE },
        margin: 0,
        name: TILESET_NAME,
        spacing: 0,
        tilecount: tiles.length,
        tiledversion: TILED_VERSION,
        tileheight: TILE_SIZE,
        tilewidth: TILE_SIZE,
        tiles,
        type: 'tileset',
        version: FORMAT_VERSION,
    };

    // The ground palette. A separate tileset from the terrain one, and not just
    // for tidiness: its tiles carry `groundId` where the terrain's carry
    // `tileId`, so a ground tile dropped into the terrain layer (or the
    // reverse) is a gid neither reader can resolve, and says so, instead of
    // silently becoming a wall.
    const groundFirstGid = 1 + tiles.length;
    const groundTiles = groundTypes.map((type, index) => {
        const file = `${GROUND_ART_DIR}/${type.art}`;
        art.push({ file, svg: type.svg });
        return {
            id: index,
            class: type.name,
            image: file,
            imagewidth: TILE_SIZE,
            imageheight: TILE_SIZE,
            properties: propertyList({ groundId: type.id, source: type.source }),
        };
    });
    const groundTileset = {
        columns: 0,
        grid: { height: TILE_SIZE, orientation: 'orthogonal', width: TILE_SIZE },
        margin: 0,
        name: GROUND_TILESET_NAME,
        spacing: 0,
        tilecount: groundTiles.length,
        tiledversion: TILED_VERSION,
        tileheight: TILE_SIZE,
        tilewidth: TILE_SIZE,
        tiles: groundTiles,
        type: 'tileset',
        version: FORMAT_VERSION,
    };

    const localOf = new Map(palette.map((config, index) => [config.id, index]));
    const data = new Array(width * height);
    for (let y = 0; y < height; y++) {
        const row = mapData.wallGrid[y] || [];
        for (let x = 0; x < width; x++) {
            const id = row[x] | 0;
            const local = localOf.get(id);
            if (local === undefined) {
                throw new Error(`tile (${x},${y}) uses id ${id}, which the palette does not define`);
            }
            // Air is written as Tiled's EMPTY cell rather than as the air tile,
            // so the background shows through it in the editor exactly as the
            // ground shows through it in the game. Both decode back to tile 0.
            data[y * width + x] = id === 0 ? 0 : local + 1;   // firstgid is 1
        }
    }

    const groundLocalOf = new Map(groundTypes.map((type, index) => [type.id, index]));
    const backgroundSource = mapData.background && mapData.background.length === width * height
        ? mapData.background
        : backgroundFromSections(width, height);
    const backgroundData = backgroundSource.map((id, i) => {
        if (id < 0) return 0;                  // outside the map: no ground at all
        const local = groundLocalOf.get(id);
        if (local === undefined) {
            throw new Error(`background cell ${i} uses ground id ${id}, which no ground type defines`);
        }
        return local + groundFirstGid;
    });

    let nextobjectid = 1;
    // Background first: it is drawn under the terrain, and Tiled draws layers
    // in file order.
    const layers = [{
        data: backgroundData,
        height,
        id: 1,
        name: 'background',
        opacity: 1,
        type: 'tilelayer',
        visible: true,
        width,
        x: 0,
        y: 0,
    }, {
        data,
        height,
        id: 2,
        name: 'terrain',
        opacity: 1,
        type: 'tilelayer',
        visible: true,
        width,
        x: 0,
        y: 0,
    }];
    for (const [index, spec] of OBJECT_LAYERS.entries()) {
        const objects = (mapData.elements || [])
            .filter(element => element.type === spec.kind)
            .map(element => objectFromElement(element, nextobjectid++));
        layers.push({
            draworder: 'index',
            id: index + 3,
            name: spec.name,
            objects,
            opacity: 1,
            type: 'objectgroup',
            visible: true,
            x: 0,
            y: 0,
        });
    }

    const map = {
        compressionlevel: -1,
        height,
        infinite: false,
        layers,
        nextlayerid: layers.length + 1,
        nextobjectid,
        orientation: 'orthogonal',
        renderorder: 'right-down',
        tiledversion: TILED_VERSION,
        tileheight: TILE_SIZE,
        tilesets: [
            { firstgid: 1, source: TILESET_FILE },
            { firstgid: groundFirstGid, source: GROUND_TILESET_FILE },
        ],
        tilewidth: TILE_SIZE,
        type: 'map',
        version: FORMAT_VERSION,
        width,
    };

    return { map, tilesets: [{ file: TILESET_FILE, tileset }, { file: GROUND_TILESET_FILE, tileset: groundTileset }], art };
}

// ---------------------------------------------------------------------------
// Import: Tiled -> game map
// ---------------------------------------------------------------------------

function readJson(file) {
    try {
        return JSON.parse(fs.readFileSync(file, 'utf8'));
    } catch (err) {
        throw new Error(`could not read ${file}: ${err.message}`);
    }
}

/** True when a file is a Tiled JSON map, so callers can accept either format. */
function isTiledMapFile(file) {
    if (!/\.(tmj|json)$/i.test(file)) return false;
    try {
        const data = JSON.parse(fs.readFileSync(file, 'utf8'));
        return data && data.type === 'map' && Array.isArray(data.layers);
    } catch (err) {
        return false;
    }
}

/**
 * Reads a Tiled map back into the game's map model.
 *
 * @param {string} mapPath  path to the .tmj
 * @returns {{elements: Array, wallGrid: number[][], customTileTypes: Array}}
 */
function fromTiled(mapPath) {
    const map = readJson(mapPath);
    if (map.type !== 'map') throw new Error(`${mapPath} is not a Tiled map`);
    if (map.infinite) throw new Error(`${mapPath} is an infinite map; the game's grid is fixed`);
    if (map.orientation !== 'orthogonal') {
        throw new Error(`${mapPath} is ${map.orientation}; the game's grid is orthogonal`);
    }
    if (map.tilewidth !== TILE_SIZE || map.tileheight !== TILE_SIZE) {
        throw new Error(
            `${mapPath} has ${map.tilewidth}x${map.tileheight} tiles; the game's are ${TILE_SIZE}x${TILE_SIZE}. ` +
            'One Tiled pixel must be one world unit or every object coordinate shifts.');
    }

    const baseDir = path.dirname(mapPath);

    // -- palette ------------------------------------------------------------
    // gid -> game tile id, and the custom half of the palette on the way past.
    // The ground tileset gets its own map: a tile carries either `tileId` or
    // `groundId`, never both, so a tile painted into the wrong layer is a gid
    // that layer's map cannot resolve, and is reported.
    const tileIdOfGid = new Map([[0, 0]]);   // an empty cell is air
    const groundIdOfGid = new Map([[0, -1]]);   // an empty cell is bare void
    const customTileTypes = [];
    const groundTypes = [];
    for (const reference of map.tilesets || []) {
        const firstgid = reference.firstgid | 0;
        const tileset = reference.source
            ? readJson(path.resolve(baseDir, reference.source))
            : reference;
        const tilesetDir = reference.source
            ? path.dirname(path.resolve(baseDir, reference.source))
            : baseDir;
        for (const tile of tileset.tiles || []) {
            const custom = readProperties(tile);
            if (custom.groundId !== undefined) {
                groundIdOfGid.set(firstgid + (tile.id | 0), custom.groundId | 0);
                groundTypes.push({
                    id: custom.groundId | 0,
                    name: tile.class || tile.type || `ground_${custom.groundId | 0}`,
                    art: path.basename(tile.image || ''),
                    source: custom.source,
                });
                continue;
            }
            const tileId = custom.tileId !== undefined ? custom.tileId | 0 : tile.id | 0;
            tileIdOfGid.set(firstgid + (tile.id | 0), tileId);
            if (custom.builtin) continue;   // constants.ts owns ids 0-2
            const config = {
                id: tileId,
                name: tile.class || tile.type || `tile_${tileId}`,
                solid: !!custom.solid,
                water: !!custom.water,
                color: custom.color !== undefined ? custom.color : '#000000',
                style: custom.style || 'flat',
            };
            if (custom.borderColor !== undefined) config.borderColor = custom.borderColor;
            if (custom.textureSvg) {
                const artPath = path.resolve(tilesetDir, custom.textureSvg);
                try {
                    config.textureSvg = fs.readFileSync(artPath, 'utf8');
                } catch (err) {
                    throw new Error(`tile "${config.name}" points at missing art ${custom.textureSvg}`);
                }
            }
            if (custom.textureTileSize !== undefined) config.textureTileSize = custom.textureTileSize;
            customTileTypes.push(config);
        }
    }
    customTileTypes.sort((a, b) => a.id - b.id);
    groundTypes.sort((a, b) => a.id - b.id);

    // -- tile grid ----------------------------------------------------------
    const tileLayers = (map.layers || []).filter(layer => layer.type === 'tilelayer');
    const terrain = tileLayers.find(layer => layer.name === 'terrain') || tileLayers[0];
    if (!terrain) throw new Error(`${mapPath} has no tile layer`);
    const data = decodeLayerData(terrain, mapPath);
    const width = terrain.width | 0;
    const height = terrain.height | 0;
    if (data.length !== width * height) {
        throw new Error(`tile layer "${terrain.name}" holds ${data.length} cells, expected ${width * height}`);
    }
    const wallGrid = [];
    for (let y = 0; y < height; y++) {
        const row = new Array(width);
        for (let x = 0; x < width; x++) {
            // Tiled packs flip flags into the top three bits of a gid. Nothing
            // here flips a tile, but a stray flag from an editor drag would
            // otherwise read as a wildly out-of-range tile id.
            const gid = data[y * width + x] & 0x1fffffff;
            const id = tileIdOfGid.get(gid);
            if (id === undefined) throw new Error(`tile (${x},${y}) uses gid ${gid}, which no tileset defines`);
            row[x] = id;
        }
        wallGrid.push(row);
    }

    // -- background ---------------------------------------------------------
    // Optional: a map that predates the layer simply has no ground of its own,
    // and the renderer falls back to the 3x3 section grid it used to use.
    let background = null;
    const backgroundLayer = tileLayers.find(layer => layer.name === 'background');
    if (backgroundLayer) {
        const cells = decodeLayerData(backgroundLayer, mapPath);
        if (cells.length !== width * height) {
            throw new Error(`background layer holds ${cells.length} cells, expected ${width * height}`);
        }
        background = cells.map((raw, i) => {
            const gid = raw & 0x1fffffff;
            const id = groundIdOfGid.get(gid);
            if (id === undefined) {
                throw new Error(`background cell ${i} uses gid ${gid}, which no ground tileset defines`);
            }
            return id;
        });
    }

    // -- annotations --------------------------------------------------------
    // Layer order, then object order within a layer. Every consumer filters by
    // element type before it looks at order, so grouping is invisible to the
    // game and gives the editor three layers it can show and hide separately.
    const elements = [];
    for (const spec of OBJECT_LAYERS) {
        for (const layer of map.layers || []) {
            if (layer.type !== 'objectgroup' || layer.name !== spec.name) continue;
            for (const object of layer.objects || []) {
                const kind = object.class || object.type || spec.kind;
                if (kind !== spec.kind) {
                    throw new Error(`object ${object.id} on layer "${layer.name}" has class "${kind}"`);
                }
                elements.push(elementFromObject(object, spec.kind, `object ${object.id}`));
            }
        }
    }

    return { elements, wallGrid, customTileTypes, background, groundTypes };
}

/** Tiled writes a tile layer as a plain array, or base64 with optional zlib. */
function decodeLayerData(layer, mapPath) {
    if (Array.isArray(layer.data)) return layer.data;
    if (layer.encoding !== 'base64') {
        throw new Error(`tile layer "${layer.name}" uses an unsupported encoding`);
    }
    let bytes = Buffer.from(layer.data, 'base64');
    if (layer.compression === 'zlib' || layer.compression === 'gzip') {
        const zlib = require('zlib');
        bytes = layer.compression === 'zlib' ? zlib.inflateSync(bytes) : zlib.gunzipSync(bytes);
    } else if (layer.compression) {
        throw new Error(
            `tile layer "${layer.name}" in ${mapPath} is ${layer.compression}-compressed; ` +
            'save the map with CSV or uncompressed layer data');
    }
    const out = new Array(bytes.length / 4);
    for (let i = 0; i < out.length; i++) out[i] = bytes.readUInt32LE(i * 4);
    return out;
}

module.exports = {
    TILE_SIZE,
    GROUND_TILESET_FILE,
    GROUND_ART_DIR,
    TILED_VERSION,
    FORMAT_VERSION,
    TILESET_FILE,
    TILE_ART_DIR,
    MAP_FILE,
    OBJECT_LAYERS,
    toTiled,
    groundColorSvg,
    backgroundFromSections,
    fromTiled,
    isTiledMapFile,
    fitSvgToTile,
};
