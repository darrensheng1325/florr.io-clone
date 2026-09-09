// Runtime shim. The canonical map is the Tiled map `maps/world.tmj`, which
// scripts/encodeMap.js compiles into the compact RLE-compressed `./map_bundle`
// (auto-generated). At runtime we use the bundle, which decodes once into the
// shared wall grid. The C++ client and server read maps/world.tmj directly.
//
// Loading the full 435 KB source on the server cost ~5–15 MB of resident heap
// (parsed JSON literal + a duplicated 200×200 wallGrid kept alive by both
// WORLD_MAP_DATA and SHARED_WALL_GRID). Routing through the bundle removes
// that duplication.

import {
    MapElement,
    WallGrid,
    setCustomTileTypes,
    decodeTileGridRLE,
    WALL_GRID as SHARED_WALL_GRID,
} from './constants';
import {
    MAP_ELEMENTS,
    MAP_CUSTOM_TILE_TYPES,
    MAP_TILE_RLE,
    MAP_GRID_WIDTH,
    MAP_GRID_HEIGHT,
} from './map_bundle';

setCustomTileTypes(MAP_CUSTOM_TILE_TYPES);

(function populateSharedWallGrid() {
    const flat = decodeTileGridRLE(MAP_TILE_RLE, MAP_GRID_WIDTH * MAP_GRID_HEIGHT);
    const h = Math.min(MAP_GRID_HEIGHT, SHARED_WALL_GRID.length);
    for (let y = 0; y < h; y++) {
        const dst = SHARED_WALL_GRID[y];
        const w = Math.min(MAP_GRID_WIDTH, dst.length);
        const base = y * MAP_GRID_WIDTH;
        for (let x = 0; x < w; x++) dst[x] = flat[base + x] | 0;
    }
})();

export const WORLD_MAP: MapElement[] = MAP_ELEMENTS;
export const WALL_GRID: WallGrid = SHARED_WALL_GRID;
