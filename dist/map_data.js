"use strict";
// Runtime shim. The canonical map is the Tiled map `maps/world.tmj`, which
// scripts/encodeMap.js compiles into the compact RLE-compressed `./map_bundle`
// (auto-generated). At runtime we use the bundle, which decodes once into the
// shared wall grid. The C++ client and server read maps/world.tmj directly.
//
// Loading the full 435 KB source on the server cost ~5–15 MB of resident heap
// (parsed JSON literal + a duplicated 200×200 wallGrid kept alive by both
// WORLD_MAP_DATA and SHARED_WALL_GRID). Routing through the bundle removes
// that duplication.
Object.defineProperty(exports, "__esModule", { value: true });
exports.WALL_GRID = exports.WORLD_MAP = void 0;
const constants_1 = require("./constants");
const map_bundle_1 = require("./map_bundle");
(0, constants_1.setCustomTileTypes)(map_bundle_1.MAP_CUSTOM_TILE_TYPES);
(function populateSharedWallGrid() {
    const flat = (0, constants_1.decodeTileGridRLE)(map_bundle_1.MAP_TILE_RLE, map_bundle_1.MAP_GRID_WIDTH * map_bundle_1.MAP_GRID_HEIGHT);
    const h = Math.min(map_bundle_1.MAP_GRID_HEIGHT, constants_1.WALL_GRID.length);
    for (let y = 0; y < h; y++) {
        const dst = constants_1.WALL_GRID[y];
        const w = Math.min(map_bundle_1.MAP_GRID_WIDTH, dst.length);
        const base = y * map_bundle_1.MAP_GRID_WIDTH;
        for (let x = 0; x < w; x++)
            dst[x] = flat[base + x] | 0;
    }
})();
exports.WORLD_MAP = map_bundle_1.MAP_ELEMENTS;
exports.WALL_GRID = constants_1.WALL_GRID;
