import { ServerPlayer } from "./player";
import { Dot } from "./enemy";
import { Obstacle } from "./enemy";
import { Item } from "./item";
import { isInMazeRegion, resolveMazeCollision, mazeCircleWallOverlap } from "./maze";

// Mob animation framerate utility - cached to avoid localStorage reads per frame
let _cachedMobAnimFPS: number | null = null;
let _cachedMobAnimFrameTime: number | null = null;
let _cachedHighQualityMobs: boolean | null = null;
let _cachedDisableUltraParticles: boolean | null = null;
let _cachedGpuAcceleration: boolean | null = null;

export function getMobAnimationFramerate(): number {
    if (_cachedMobAnimFPS === null) {
        const saved = typeof localStorage !== 'undefined' ? localStorage.getItem('mobAnimationFramerate') : null;
        _cachedMobAnimFPS = saved ? parseInt(saved, 10) : 15;
    }
    return _cachedMobAnimFPS;
}

export function getMobAnimationFrameTime(): number {
    if (_cachedMobAnimFrameTime === null) {
        _cachedMobAnimFrameTime = 1000 / getMobAnimationFramerate();
    }
    return _cachedMobAnimFrameTime;
}

// High quality mobs setting utility - cached
export function getHighQualityMobs(): boolean {
    if (_cachedHighQualityMobs === null) {
        const saved = typeof localStorage !== 'undefined' ? localStorage.getItem('highQualityMobs') : null;
        _cachedHighQualityMobs = saved === 'true';
    }
    return _cachedHighQualityMobs;
}

// Disable ultra+ petal particles setting utility - cached
export function getDisableUltraParticles(): boolean {
    if (_cachedDisableUltraParticles === null) {
        const saved = typeof localStorage !== 'undefined' ? localStorage.getItem('disableUltraParticles') : null;
        _cachedDisableUltraParticles = saved === 'true';
    }
    return _cachedDisableUltraParticles;
}

// GPU acceleration for the main game canvas. Default ON. When OFF we pass
// `willReadFrequently: true` to getContext, which makes Chrome keep the
// canvas in system memory (software rasterization) instead of a GPU texture.
// Only honored when the 2D context is first created, so a change needs a
// canvas/page reload to take effect (see applyGpuAcceleration / core.ts).
export function getGpuAcceleration(): boolean {
    if (_cachedGpuAcceleration === null) {
        const saved = typeof localStorage !== 'undefined' ? localStorage.getItem('gpuAcceleration') : null;
        _cachedGpuAcceleration = saved !== 'false';
    }
    return _cachedGpuAcceleration;
}

// Call this when settings change to invalidate caches
export function invalidateSettingsCache(): void {
    _cachedMobAnimFPS = null;
    _cachedMobAnimFrameTime = null;
    _cachedHighQualityMobs = null;
    _cachedDisableUltraParticles = null;
    _cachedGpuAcceleration = null;
}

// Server protocol configuration
export const USE_HTTPS = typeof process !== 'undefined' && process.env ? process.env.USE_HTTPS !== 'false' : true;  // Default to HTTPS, set USE_HTTPS=false to use HTTP
export const SERVER_PROTOCOL = USE_HTTPS ? 'https' : 'http';

// Viewport optimization constants
export const VIEWPORT_BUFFER = 500;  // Extra distance beyond viewport to keep enemies active

// Fallback viewport dimensions. Real clients report their own canvas size
// (player.viewportWidth/Height); these are only used until that arrives.
export const VIEWPORT_WIDTH = 1920;
export const VIEWPORT_HEIGHT = 1080;

export const players: Record<string, ServerPlayer> = {};
export const dots: Dot[] = [];
// There is no `enemies` array here any more. Mobs live in the ECS world and
// nowhere else; `liveEnemies()` in server/enemyRegistry.ts projects their shells
// back out on demand. A module-level array was a SECOND place a mob could exist,
// which is what every mob/entity disagreement this codebase has had came from.
export const obstacles: Obstacle[] = [];
export const items: Item[] = [];

export const WORLD_WIDTH = 60000;
export const WORLD_HEIGHT = 60000;
export const ACTUAL_WORLD_WIDTH = 60000;
export const ACTUAL_WORLD_HEIGHT = 60000;

// Wall grid system constants
export const WALL_TILE_SIZE = 300; // Size of each wall tile in pixels (3x larger for better performance)
export const WALL_GRID_WIDTH = Math.ceil(ACTUAL_WORLD_WIDTH / WALL_TILE_SIZE);
export const WALL_GRID_HEIGHT = Math.ceil(ACTUAL_WORLD_HEIGHT / WALL_TILE_SIZE);

// Tile state is now an 8-bit value (0-255). Built-in IDs: 0 = air, 1 = wall, 2 = water.
// User-configurable tile types (3-255) can be added via the map's tile palette config.
export type WallTileState = number;

// Wall grid type: 2D array where [y][x] = tile state (0-255)
export type WallGrid = WallTileState[][];

// --- Tile type registry (8-bit per tile, 256 types max) ---

/** Configuration for a single tile type. */
export interface TileTypeConfig {
    /** 0-255 numeric ID stored in the tile grid. */
    id: number;
    /** Human-readable name for editors / debugging. */
    name: string;
    /** Whether the tile blocks movement (collision). */
    solid: boolean;
    /** Whether the tile counts as water for swimming logic. */
    water: boolean;
    /** Primary fill color (hex). Used when no texture is set, or as a fallback while a textureSvg loads. */
    color: string;
    /** Optional secondary border / stroke color (used by water-style edges). */
    borderColor?: string;
    /**
     * Visual style for the renderer:
     *   - "wall"  → textured fill (uses textureSvg if set, else the built-in wall texture) + jagged edges
     *   - "water" → flat fill + smoothed edges with borderColor
     *   - "flat"  → solid colored fill, no edges
     * Default (undefined) is treated as "flat".
     */
    style?: 'wall' | 'water' | 'flat';
    /**
     * Optional inline SVG markup (e.g. "<svg xmlns=…><rect…/></svg>") used as the
     * tile's repeat-pattern texture. The renderer rasterizes this once at the
     * size given by textureTileSize (default WALL_TILE_SIZE) and tiles it across
     * each cell of this type. Bundled with the map so any client can render it.
     */
    textureSvg?: string;
    /** Pixel size at which textureSvg is rasterized. Defaults to WALL_TILE_SIZE. */
    textureTileSize?: number;
}

const BUILTIN_WALL_TEXTURE_SVG = `<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400" viewBox="0 0 124 124" fill="none">
<rect x="0" y="0" width="400" height="400" fill="#99550c"/>
<circle cx="25.2109" cy="51.5391" r="5.1641" fill="#783f01"/>
<circle cx="105.5341" cy="25.5207" r="5.1641" fill="#783f01"/>
<circle cx="51.5308" cy="85.3607" r="5.1641" fill="#783f01"/>
<circle cx="64.5341" cy="15.5207" r="5.1641" fill="#783f01"/>
<circle cx="103.5341" cy="102.5207" r="5.1641" fill="#783f01"/>
</svg>`;

/** Built-in tile types — IDs 0-2 are reserved and always present. */
export const BUILTIN_TILE_TYPES: TileTypeConfig[] = [
    { id: 0, name: 'air',   solid: false, water: false, color: '#00000000', style: 'flat' },
    { id: 1, name: 'wall',  solid: true,  water: false, color: '#99550c',   style: 'wall', textureSvg: BUILTIN_WALL_TEXTURE_SVG },
    { id: 2, name: 'water', solid: false, water: true,  color: '#4169E1',   borderColor: '#2a4fa0', style: 'water' },
];

/** Mutable registry: built-ins + custom types loaded from the map bundle. */
const TILE_TYPE_REGISTRY: Map<number, TileTypeConfig> = (() => {
    const m = new Map<number, TileTypeConfig>();
    for (const t of BUILTIN_TILE_TYPES) m.set(t.id, t);
    return m;
})();

// Flat blocking table mirroring the registry. isTileIdBlocking runs on every
// raycast sample, A* neighbor expansion, and wall-collision tile scan — the
// Map.get chain behind getTileTypeConfig showed up as ~3% of total server CPU,
// so those reads go through this LUT instead. Rebuilt on every registry change.
const TILE_BLOCKING_LUT = new Uint8Array(256);
function rebuildTileBlockingLut(): void {
    TILE_BLOCKING_LUT.fill(0);
    for (const cfg of TILE_TYPE_REGISTRY.values()) {
        TILE_BLOCKING_LUT[cfg.id] = (cfg.solid || cfg.water) ? 1 : 0;
    }
}
rebuildTileBlockingLut();

/** Register or override a tile type (typically called once at startup with custom map types). */
export function registerTileType(config: TileTypeConfig): void {
    if (config.id < 0 || config.id > 255 || !Number.isInteger(config.id)) {
        throw new Error(`TileTypeConfig.id must be an integer in [0, 255], got ${config.id}`);
    }
    if (config.id < BUILTIN_TILE_TYPES.length) {
        // Allow overriding built-in colors/styles but keep semantic flags (solid/water) sane.
        const builtin = BUILTIN_TILE_TYPES[config.id];
        if (config.solid !== builtin.solid || config.water !== builtin.water) {
            console.warn(`[TileRegistry] Refusing to change solid/water flags of built-in tile ${builtin.name} (id=${builtin.id})`);
            config = { ...config, solid: builtin.solid, water: builtin.water };
        }
    }
    TILE_TYPE_REGISTRY.set(config.id, config);
    rebuildTileBlockingLut();
}

/** Replace the entire custom-tile portion of the registry. Built-ins are restored first. */
export function setCustomTileTypes(configs: TileTypeConfig[]): void {
    TILE_TYPE_REGISTRY.clear();
    for (const t of BUILTIN_TILE_TYPES) TILE_TYPE_REGISTRY.set(t.id, t);
    for (const c of configs) {
        if (c.id < BUILTIN_TILE_TYPES.length) continue; // skip attempts to redefine built-ins via this path
        registerTileType(c);
    }
    rebuildTileBlockingLut();
}

/** Look up a tile type by ID. Falls back to air for unknown IDs (so forward-compat reads don't crash). */
export function getTileTypeConfig(id: number): TileTypeConfig {
    return TILE_TYPE_REGISTRY.get(id) || BUILTIN_TILE_TYPES[0];
}

/** Iterate all currently registered tile types (built-in + custom). */
export function getAllTileTypes(): TileTypeConfig[] {
    return Array.from(TILE_TYPE_REGISTRY.values()).sort((a, b) => a.id - b.id);
}

/** True if the given tile ID is solid (blocks movement). */
export function isTileIdSolid(id: number): boolean {
    return getTileTypeConfig(id).solid;
}

/** True if the given tile ID is water. */
export function isTileIdWater(id: number): boolean {
    return getTileTypeConfig(id).water;
}

/**
 * True if a tile blocks player/enemy positioning — i.e. it is either solid
 * (collides) or water (not walkable). Used for movement collision, spawn
 * validation, line-of-sight, and pathfinding obstacle checks. Equivalent to
 * `isTileIdSolid(id) || isTileIdWater(id)`, exposed as a single helper so
 * custom tile types automatically participate everywhere the legacy 0/1/2
 * system used `state === 1 || state === 2`.
 */
export function isTileIdBlocking(id: number): boolean {
    // Out-of-range ids match getTileTypeConfig's air fallback (not blocking).
    return id >= 0 && id <= 255 && TILE_BLOCKING_LUT[id] === 1;
}

// Density calculation constants (defined after world dimensions)
export const TOTAL_WORLD_AREA = ACTUAL_WORLD_WIDTH * ACTUAL_WORLD_HEIGHT;  // 3,600,000,000 pixels²
export const ORIGINAL_ENEMY_COUNT = 9000;
export const ORIGINAL_ENEMY_DENSITY = ORIGINAL_ENEMY_COUNT / TOTAL_WORLD_AREA;  // 0.0000025 enemies per pixel²
export const VIEWPORT_WITH_BUFFER_AREA = (VIEWPORT_WIDTH + VIEWPORT_BUFFER * 2) * (VIEWPORT_HEIGHT + VIEWPORT_BUFFER * 2);  // 6,073,600 pixels²
export const ENEMIES_PER_VIEWPORT = Math.ceil(ORIGINAL_ENEMY_DENSITY * VIEWPORT_WITH_BUFFER_AREA);  // ~16 enemies per viewport
export const SCALE_FACTOR = 1;

// Base values for the level curves in server/playerManager.ts:
//   maxHealth = PLAYER_MAX_HEALTH + ceil(level^1.5 * HEALTH_PER_LEVEL)
//   damage    = PLAYER_DAMAGE     + ceil(level^1.5 * DAMAGE_PER_LEVEL)
export const PLAYER_MAX_HEALTH = 100;
export const PLAYER_DAMAGE = 5;

// Per-tier natural spawn weights (must sum to 1) and the rarity color used by
// server-side chat announcements. Colors MUST match ITEM_RARITY_COLORS in
// petals.ts — the canonical table the client renders mobs and UI with.
// Tiers above mythic never spawn naturally (probability 0); they are produced
// by tier upgrades, spawn zones, and boss logic in server/enemySpawner.ts.
// Health/speed/damage are NOT here: per-mob, per-tier stats come from
// getMobStats() in mobs.ts.
export const ENEMY_TIERS = {
  common:    { probability: 0.4,  color: '#7eef6d' },
  uncommon:  { probability: 0.3,  color: '#ffe65d' },
  rare:      { probability: 0.15, color: '#4d52e3' },
  epic:      { probability: 0.1,  color: '#861fde' },
  legendary: { probability: 0.04, color: '#de1f1f' },
  mythic:    { probability: 0.01, color: '#1fdbde' },
  ultra:     { probability: 0.0,  color: '#de1f65' },
  super:     { probability: 0.0,  color: '#2bffa4' },
  unique:    { probability: 0.0,  color: '#ffffff' },
  apex:      { probability: 0.0,  color: '#ff00ff' }
};

export const RESPAWN_INVULNERABILITY_TIME = 3000; // 3 seconds of invulnerability after respawn

// Player top speed (units/sec), matched to gardn. gardn's terminal velocity is
// PLAYER_ACCELERATION / DEFAULT_FRICTION per tick = (5 / (1/3)) = 15 units/tick,
// × SIM_RATE (20) = 300 units/sec. This is the terminal velocity the friction
// model in playerState.ts converges to (before speed_boost / multipliers).
export const MAX_SPEED = 300;
// Mouse-control speed law, matched to gardn (Server/Client.cc): the flower's
// speed scales LINEARLY with the world-space distance from the flower to the
// cursor, reaching full speed at MOUSE_FULL_SPEED_DISTANCE units and capping
// there (gardn's `m > 200 ? full : m/200 * full`). Since MAX_SPEED is 1:1 with
// gardn's units, gardn's 200-unit reference ports directly. Full speed engages
// at a modest cursor throw (~200 units ≈ 5 flower-radii out), which is the
// snappy gardn feel; the previous screen-relative power curve only hit full
// speed at a half-screen throw (sluggish) and its exploding derivative near
// center turned tiny cursor jiggles into speed swings (jitter). No minimum
// floor — near the flower the speed eases to 0 for precise positioning, exactly
// like gardn (which has no floor).
export const MOUSE_FULL_SPEED_DISTANCE = 200;

// XP / level curve (server/playerManager.ts, leaderboard.ts).
// NOTE: database.ts's one-time old-format migration deliberately uses the
// pre-2025-11 multiplier (1.25) because that is the curve those records were
// written under — it is not a copy of this value.
export const BASE_XP_REQUIREMENT = 100;
export const XP_MULTIPLIER = 1.08;
export const HEALTH_PER_LEVEL = 10;
export const DAMAGE_PER_LEVEL = 1;
export const PLAYER_SIZE = 40;
export const ENEMY_SIZE = 40;
export const TELEPORTER_RADIUS = 60; // Radius for point-based teleporter interaction
export const TELEPORTER_SUCTION_RADIUS = 150; // Larger radius for suction pull effect
export const TELEPORTER_SUCTION_FORCE = 400; // Force magnitude (enough to overcome knockback of 25)
export const TELEPORTER_COOLDOWN = 5000; // 5 second cooldown after teleporting

// PVP arena: a circular zone where players can damage each other.
// Placed well outside the regular world bounds (60000x60000) so it shares no
// coordinate space with the map — there is no walkable path between them.
// Entry is exclusively via the "PVP" choice on the title screen; exit is via
// the teleporter at the arena center.
export const PVP_ARENA_CENTER_X = 150000;
export const PVP_ARENA_CENTER_Y = 150000;
export const PVP_ARENA_RADIUS = 2500;
// Spawn point inside the arena (offset from center so players don't sit on the exit teleporter)
export const PVP_ARENA_SPAWN_X = PVP_ARENA_CENTER_X + 1500;
export const PVP_ARENA_SPAWN_Y = PVP_ARENA_CENTER_Y;
// Fraction of petals gained inside PVP that survive the trip back to the regular inventory.
export const PVP_INVENTORY_KEEP_RATIO = 0.25;
// All players in the PVP arena share the same fixed max health regardless of level/petals.
export const PVP_MAX_HEALTH = 100;

export function isInPvpArena(x: number, y: number): boolean {
    const dx = x - PVP_ARENA_CENTER_X;
    const dy = y - PVP_ARENA_CENTER_Y;
    return dx * dx + dy * dy <= PVP_ARENA_RADIUS * PVP_ARENA_RADIUS;
}

// Section configuration for the 3x3 map grid (9 sections total)
// Each section is 20000x20000 pixels
// Section numbering: 1-9 (row-major order, top-left to bottom-right)
export interface SectionConfig {
    name: string;
    background?: string;  // Optional background color (hex like '#00d885') or SVG path (like 'land.svg')
}

// Default section configurations (indexed 0-8, displayed as sections 1-9)
export const SECTION_CONFIGS: SectionConfig[] = [
    { name: 'Garden', background: 'land.svg' },           // Section 1 (top-left)
    { name: 'Desert', background: 'desert.svg' },           // Section 2 (top-center)
    { name: 'Hel', background: 'hel.svg' },           // Section 3 (top-right)
    { name: 'Ocean', background: 'ocean.svg' },           // Section 4 (middle-left)
    { name: 'Ant Hell', background: 'ant_hell.svg' },       // Section 5 (center)
    { name: 'Jungle', background: 'jungle.svg' },           // Section 6 (middle-right)
    { name: 'Sewers', background: 'sewers.svg' },           // Section 7 (bottom-left)
    { name: 'Computer', background: '#000000' },        // Section 8 (bottom-center)
    { name: 'Unknown', background: '#000000' },            // Section 9 (bottom-right)
];

// Add map configuration

// Define spawn table entry for biomes
export interface BiomeSpawnEntry {
    mobType?: string;  // If not specified, uses any mob type
    tier: 'common' | 'uncommon' | 'rare' | 'epic' | 'legendary' | 'mythic' | 'ultra' | 'super' | 'unique' | 'apex';
    weight: number;  // Relative probability (higher = more common)
    reversed?: boolean;  // Whether the mob image should be flipped horizontally
}

export interface MapElement {
    /**
     * `biome` is kept in the union only so the frozen TypeScript server's own
     * type guards still compile; no map has produced one since the object
     * layers were reworked, and the C++ readers ignore it.
     */
    type: 'wall' | 'spawn' | 'teleporter' | 'biome';
    /**
     * The element's bounding box. When `polygon` is present these four are its
     * AABB rather than its shape: every broadphase question — is this zone near
     * a viewport, which sections does it touch — is asked of the box, and only
     * containment, area and point sampling go to the outline.
     */
    x: number;
    y: number;
    width: number;
    height: number;
    /**
     * The zone's outline in world coordinates, when it is not a rectangle.
     *
     * DECLARED HERE, NOT HONOURED HERE. This file's only stake in it is that
     * `map_bundle.ts` typechecks; nothing in `src/` reads the field. The C++
     * server is the one that reads outlines — see cpp/shared/game/map_elements.h.
     */
    polygon?: { x: number; y: number }[];
    /**
     * Everything an annotation says about itself. Like `polygon`, declared so
     * the generated bundle typechecks: the C++ side is what reads a spawn
     * band's `mobs`, a player spawn rectangle's button, or a teleporter's
     * target map. Biome rectangles and their spawn tables no longer exist —
     * what they said is said by mob regions and tier bands now.
     */
    properties?: {
        spawnType?: 'common' | 'uncommon' | 'rare' | 'epic' | 'legendary' | 'mythic' | 'ultra'
            | 'super' | 'unique' | 'apex';
        /** A spawn band's or mob region's distribution: `garden 50% hornet 50%`. */
        mobs?: string;
        /**
         * Player spawn rectangles: the picker's button. The bundle never
         * carries one -- encodeMap.js leaves `player_spawn` objects out,
         * because the frozen TypeScript server has no picker to feed and its
         * renderer indexes a colour table by element type. They are read off
         * the Tiled map directly by the C++ side.
         */
        spawnId?: string;
        label?: string;
        color?: string;
        order?: number;
        backdrop?: string;
        biome?: string;
        pickable?: boolean;
        /** Teleporters: the map the pad leads to, and where in it. */
        targetMap?: string;
        targetSpawn?: string;
        teleportTo?: { x: number; y: number; serverPort?: number };
        /** @deprecated Biome rectangles are gone; kept for the frozen TS server's typecheck. */
        biomeName?: string;
        /** @deprecated */
        spawnTable?: BiomeSpawnEntry[];
        /** @deprecated */
        backgroundTexture?: string;
        /** @deprecated */
        isNoCombat?: boolean;
    };
}

// Map data structure that includes wall grid
export interface MapData {
    elements: MapElement[];
    wallGrid?: WallGrid;
    /**
     * Optional palette of custom tile types layered on top of the built-ins.
     * Each entry must use an ID >= 3 (built-ins reserve 0-2: air/wall/water).
     */
    customTileTypes?: TileTypeConfig[];
}


// Add map element type guards
export function isWall(element: MapElement): boolean {
    return element.type === 'wall';
}

export function isSpawn(element: MapElement): boolean {
    return element.type === 'spawn';
}

export function isTeleporter(element: MapElement): boolean {
    return element.type === 'teleporter';
}

// Server configuration for cross-server teleportation
export interface ServerConfig {
    port: number;
    host: string;
    name: string;
    protocol?: string;  // Optional protocol, defaults to SERVER_PROTOCOL
}

// Default server configuration - can be overridden via environment variables or config file
export const DEFAULT_SERVER_CONFIGS: ServerConfig[] = [
    { port: 3000, host: 'localhost', name: 'Server1' },
    { port: 3001, host: 'localhost', name: 'Server2' },
    { port: 3002, host: 'localhost', name: 'Server3' }
];

// Get server configuration from environment or use defaults
export function getServerConfigs(): ServerConfig[] {
    const configStr = typeof process !== 'undefined' && process.env ? process.env.SERVER_CONFIGS : undefined;
    if (configStr) {
        try {
            const configs = JSON.parse(configStr);
            return configs.map((config: ServerConfig) => ({ 
                ...config, 
                protocol: config.protocol || SERVER_PROTOCOL 
            }));
        } catch (error) {
            console.error('Failed to parse SERVER_CONFIGS environment variable:', error);
        }
    }
    return DEFAULT_SERVER_CONFIGS.map(config => ({ 
        ...config, 
        protocol: config.protocol || SERVER_PROTOCOL 
    }));
}

// Find server config by port
export function getServerConfigByPort(port: number): ServerConfig | undefined {
    return getServerConfigs().find(config => config.port === port);
}

// Helper function to create empty wall grid
export function createEmptyWallGrid(): WallGrid {
    const grid: WallGrid = [];
    for (let y = 0; y < WALL_GRID_HEIGHT; y++) {
        grid[y] = new Array(WALL_GRID_WIDTH).fill(0) as number[];
    }
    return grid;
}

// Wall grid - populated from server data at runtime
export const WALL_GRID: WallGrid = createEmptyWallGrid();

// Helper functions for wall grid coordinate conversion
export function worldToTileX(worldX: number): number {
    return Math.floor(worldX / WALL_TILE_SIZE);
}

export function worldToTileY(worldY: number): number {
    return Math.floor(worldY / WALL_TILE_SIZE);
}

export function tileToWorldX(tileX: number): number {
    return tileX * WALL_TILE_SIZE;
}

export function tileToWorldY(tileY: number): number {
    return tileY * WALL_TILE_SIZE;
}

// Get tile state at world coordinates
export function getTileState(grid: WallGrid, worldX: number, worldY: number): WallTileState {
    const tileX = worldToTileX(worldX);
    const tileY = worldToTileY(worldY);
    if (tileY < 0 || tileY >= grid.length || tileX < 0 || tileX >= (grid[0]?.length || 0)) {
        return 0; // Out of bounds = air
    }
    return grid[tileY][tileX];
}

// Set tile state at world coordinates
export function setTileState(grid: WallGrid, worldX: number, worldY: number, state: WallTileState): void {
    const tileX = worldToTileX(worldX);
    const tileY = worldToTileY(worldY);
    if (tileY >= 0 && tileY < grid.length && tileX >= 0 && tileX < (grid[0]?.length || 0)) {
        grid[tileY][tileX] = state;
    }
}

// Check if a point collides with a wall tile (any solid tile type)
export function collidesWithWallTile(grid: WallGrid, worldX: number, worldY: number): boolean {
    return isTileIdSolid(getTileState(grid, worldX, worldY));
}

// Check if a point is in water (any water-flagged tile type)
export function isInWater(grid: WallGrid, worldX: number, worldY: number): boolean {
    return isTileIdWater(getTileState(grid, worldX, worldY));
}

// --- Jagged Edge System (shared between client and server) ---

/** Deterministic seeded random number generator */
export function seededRandom(seed: number): number {
    const x = Math.sin(seed) * 10000;
    return x - Math.floor(x);
}

/** A point on a jagged edge in tile-local coordinates.
 *  t = distance along the edge (0..WALL_TILE_SIZE)
 *  offset = perpendicular distance outward into air (0..JAGGED_MAX_OFFSET) */
export interface JaggedPoint {
    t: number;
    offset: number;
}

/** Cached jagged edge data for a single tile. null = edge not exposed. */
export interface TileJaggedEdges {
    top: JaggedPoint[] | null;
    bottom: JaggedPoint[] | null;
    left: JaggedPoint[] | null;
    right: JaggedPoint[] | null;
}

export const JAGGED_MAX_OFFSET = 20;
export const JAGGED_NUM_SEGMENTS = 7;

const JAGGED_EDGE_CACHE: Map<string, TileJaggedEdges> = new Map();

/** Check if a tile edge is exposed (adjacent tile is air, or solid adjacent to water) */
export function isTileEdgeExposed(
    grid: WallGrid, tileX: number, tileY: number,
    edge: 'top' | 'bottom' | 'left' | 'right'
): boolean {
    let adjX = tileX, adjY = tileY;
    if (edge === 'top') adjY--;
    else if (edge === 'bottom') adjY++;
    else if (edge === 'left') adjX--;
    else if (edge === 'right') adjX++;

    if (adjY < 0 || adjY >= grid.length || adjX < 0 || adjX >= (grid[0]?.length || 0)) {
        return true; // Out of bounds = exposed
    }
    const adjState = grid[adjY][adjX];
    if (adjState === 0) return true; // Adjacent to air = exposed
    // Solid (wall-style) tiles show edges against water; water doesn't draw edges against solid.
    const currentState = grid[tileY]?.[tileX] || 0;
    if (isTileIdSolid(currentState) && isTileIdWater(adjState)) return true;
    return false;
}

/** Generate jagged edge points for one edge of a tile */
export function generateJaggedEdgePoints(
    tileX: number, tileY: number,
    edge: 'top' | 'bottom' | 'left' | 'right'
): JaggedPoint[] {
    const edgeIndex = edge === 'top' ? 0 : edge === 'bottom' ? 1 : edge === 'left' ? 2 : 3;
    const baseSeed = ((tileX * 73856093) ^ (tileY * 19349669)) + edgeIndex * 1000;

    const points: JaggedPoint[] = [];
    points.push({ t: 0, offset: 0 });

    const segmentLength = WALL_TILE_SIZE / (JAGGED_NUM_SEGMENTS + 1);

    for (let i = 1; i <= JAGGED_NUM_SEGMENTS; i++) {
        const seed = baseSeed + i;
        const jitter = (seededRandom(seed) - 0.5) * segmentLength * 0.4;
        const t = Math.max(1, Math.min(WALL_TILE_SIZE - 1, i * segmentLength + jitter));
        const offset = seededRandom(seed + 100) * JAGGED_MAX_OFFSET;
        points.push({ t, offset });
    }

    points.push({ t: WALL_TILE_SIZE, offset: 0 });
    points.sort((a, b) => a.t - b.t);

    return points;
}

/** Get jagged edge data for a tile (cached) */
export function getTileJaggedEdges(grid: WallGrid, tileX: number, tileY: number): TileJaggedEdges {
    const key = `${tileX},${tileY}`;
    const cached = JAGGED_EDGE_CACHE.get(key);
    if (cached) return cached;

    const edges: TileJaggedEdges = {
        top: isTileEdgeExposed(grid, tileX, tileY, 'top')
            ? generateJaggedEdgePoints(tileX, tileY, 'top') : null,
        bottom: isTileEdgeExposed(grid, tileX, tileY, 'bottom')
            ? generateJaggedEdgePoints(tileX, tileY, 'bottom') : null,
        left: isTileEdgeExposed(grid, tileX, tileY, 'left')
            ? generateJaggedEdgePoints(tileX, tileY, 'left') : null,
        right: isTileEdgeExposed(grid, tileX, tileY, 'right')
            ? generateJaggedEdgePoints(tileX, tileY, 'right') : null,
    };

    JAGGED_EDGE_CACHE.set(key, edges);
    return edges;
}

// --- Tile grid RLE codec (used to bundle the wall grid as a compact base64 blob) ---

/**
 * Run-length encode a flat array of 8-bit tile IDs into a base64 string.
 *
 * Wire format: a sequence of variable-length records.
 *   byte 0: high 7 bits = run length (1-127), low 1 bit = continuation flag
 *   if continuation: next 2 bytes are an extra big-endian count to add
 *   final byte: tile id (0-255)
 *
 * For typical tile grids (large air/water runs), this hits ~1% of the raw size.
 */
export function encodeTileGridRLE(flat: ArrayLike<number>): string {
    const out: number[] = [];
    let i = 0;
    const len = flat.length;
    while (i < len) {
        const v = flat[i] & 0xff;
        let run = 1;
        while (i + run < len && (flat[i + run] & 0xff) === v && run < 0x7fffff) run++;
        i += run;
        if (run <= 127) {
            out.push((run << 1) & 0xff, v);
        } else {
            // 7-bit base length + 16-bit extension; total max = 127 + 65535 = 65662 (we cap at 0x7fffff above just in case)
            while (run > 0) {
                const base = Math.min(127, run);
                const ext = Math.min(0xffff, run - base);
                if (ext > 0) {
                    out.push(((base << 1) | 1) & 0xff, (ext >> 8) & 0xff, ext & 0xff, v);
                    run -= base + ext;
                } else {
                    out.push((base << 1) & 0xff, v);
                    run -= base;
                }
            }
        }
    }
    return bytesToBase64(new Uint8Array(out));
}

/** Decode a base64 RLE string back into a flat Uint8Array of tile IDs. */
export function decodeTileGridRLE(b64: string, expectedLength?: number): Uint8Array {
    const buf = base64ToBytes(b64);
    const out: number[] = [];
    let p = 0;
    while (p < buf.length) {
        const header = buf[p++];
        const cont = header & 1;
        let count = header >>> 1;
        if (cont) {
            count += (buf[p++] << 8) | buf[p++];
        }
        const v = buf[p++];
        for (let k = 0; k < count; k++) out.push(v);
    }
    const arr = Uint8Array.from(out);
    if (expectedLength !== undefined && arr.length !== expectedLength) {
        console.warn(`[decodeTileGridRLE] length mismatch: got ${arr.length}, expected ${expectedLength}`);
    }
    return arr;
}

/** Decode a flat tile array into a 2D WallGrid of the configured dimensions. */
export function tilesToWallGrid(flat: Uint8Array | number[], width = WALL_GRID_WIDTH, height = WALL_GRID_HEIGHT): WallGrid {
    const grid: WallGrid = new Array(height);
    for (let y = 0; y < height; y++) {
        const row = new Array<number>(width);
        const base = y * width;
        for (let x = 0; x < width; x++) row[x] = flat[base + x] | 0;
        grid[y] = row;
    }
    return grid;
}

/** Flatten a WallGrid into a Uint8Array (row-major). */
export function wallGridToFlat(grid: WallGrid, width = WALL_GRID_WIDTH, height = WALL_GRID_HEIGHT): Uint8Array {
    const flat = new Uint8Array(width * height);
    for (let y = 0; y < height; y++) {
        const row = grid[y] || [];
        for (let x = 0; x < width; x++) flat[y * width + x] = (row[x] | 0) & 0xff;
    }
    return flat;
}

// Tiny base64 helpers that work in both Node and the browser.
function bytesToBase64(bytes: Uint8Array): string {
    const B = (globalThis as any).Buffer;
    if (B) return B.from(bytes).toString('base64');
    let bin = '';
    for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
    return (typeof btoa === 'function' ? btoa(bin) : '');
}
function base64ToBytes(b64: string): Uint8Array {
    const B = (globalThis as any).Buffer;
    if (B) return new Uint8Array(B.from(b64, 'base64'));
    const bin = (typeof atob === 'function' ? atob(b64) : '');
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
}

/** Get the maximum jagged protrusion within a range [minT, maxT] on an edge */
export function getMaxJaggedOffset(points: JaggedPoint[], minT: number, maxT: number): number {
    let maxOffset = 0;

    for (const pt of points) {
        if (pt.t >= minT && pt.t <= maxT) {
            if (pt.offset > maxOffset) maxOffset = pt.offset;
        }
    }

    // Interpolate at range boundaries for segments that cross them
    for (let i = 0; i < points.length - 1; i++) {
        const p0 = points[i], p1 = points[i + 1];
        if (p1.t < minT || p0.t > maxT) continue;
        if (p0.t < minT && p1.t > minT) {
            const frac = (minT - p0.t) / (p1.t - p0.t);
            const interpOffset = p0.offset + frac * (p1.offset - p0.offset);
            if (interpOffset > maxOffset) maxOffset = interpOffset;
        }
        if (p0.t < maxT && p1.t > maxT) {
            const frac = (maxT - p0.t) / (p1.t - p0.t);
            const interpOffset = p0.offset + frac * (p1.offset - p0.offset);
            if (interpOffset > maxOffset) maxOffset = interpOffset;
        }
    }

    return maxOffset;
}

// ── Shared tile collision resolution ────────────────────────────────────────
// Used by BOTH the server (authoritative wall/water collision) and the client
// (movement prediction). Keeping a single implementation guarantees the predicted
// position resolves walls/water identically to the server, so prediction doesn't
// fight the authoritative position at walls (jitter) or at water edges (springing).

// Extra slack folded into the tile-scan reach and into the substep cap. It is NOT
// added to the collision shape itself: entities collide with walls as a disc of
// exactly their own radius (see checkTileCollision), same as mobs/players collide
// with each other and same as the maze resolver, so the hitbox matches the one the
// debug overlay draws. Keeping it in `reach` just makes the scan a superset.
export const COLLISION_BUFFER = 5;

// Push-out overshoot, in px. resolveTileCollision lands the entity at exactly its
// radius from the surface; float error at large world coordinates can leave that a
// hair *inside* the detection boundary, which resolveEntityWallCollisions reads as
// "unresolved" and stepPlayerMovement then refuses to move through — i.e. the
// entity wedges against the wall. Overshooting by a hundredth of a pixel is
// invisible and guarantees the resolved position tests clear.
const WALL_RESOLVE_EPS = 0.01;

export interface TileCollisionResult {
    collided: boolean;
    tileX: number;
    tileY: number;
    state: WallTileState;
    effectiveLeft: number;
    effectiveRight: number;
    effectiveTop: number;
    effectiveBottom: number;
}

// Sanity cap for collision reach. No real entity (player or mob) has a halfSize
// remotely this large; beyond it the tile-scan loops below would span the whole grid
// and spin forever. Used only to bound those loops against a degenerate size.
const MAX_COLLISION_REACH = 4096;
let _lastBadHalfSize = NaN;

// Sanity cap for world coordinates. The furthest legitimate region is the maze
// (origin 200000, span ~77k); nothing real lives past 1e6. Positions far beyond
// it break the grid-index math: past 2^53 a tile/cell index can no longer be
// incremented (`i++` is a float no-op), so even a "one tile" scan loop spins
// forever. Admin commands validate against this; grid code treats coordinates
// beyond it like non-finite ones.
export const MAX_SANE_WORLD_COORD = 1_000_000;

// Check if a position collides with a wall or water tile, accounting for jagged edges.
export function checkTileCollision(worldX: number, worldY: number, halfSize: number): TileCollisionResult | null {
    // Maze region: walls are the maze's corner-coded cell grid, not WALL_GRID.
    // Used by projectile wall checks; movement goes through resolveMazeCollision.
    if (isInMazeRegion(worldX, worldY)) {
        const rect = mazeCircleWallOverlap(worldX, worldY, halfSize);
        if (!rect) return null;
        return {
            collided: true,
            tileX: Math.floor(rect.left / WALL_TILE_SIZE),
            tileY: Math.floor(rect.top / WALL_TILE_SIZE),
            state: 1,
            effectiveLeft: rect.left,
            effectiveRight: rect.right,
            effectiveTop: rect.top,
            effectiveBottom: rect.bottom,
        };
    }
    // Reach includes COLLISION_BUFFER so an entity already resting at the buffer
    // distance still registers as in contact (see the inflated overlap test below).
    const reach = halfSize + JAGGED_MAX_OFFSET + COLLISION_BUFFER;
    // Guard a degenerate entity size/position. A non-finite position, or a huge/Infinity
    // halfSize (e.g. a player whose sizeMultiplier blew up), makes the tile range below
    // span (effectively) the whole grid and spins the nested loops forever — the 100% CPU
    // hang. No legitimate entity is anywhere near MAX_COLLISION_REACH; treat the degenerate
    // entity as not-colliding (it's the real bug) and log the value once so it's traceable.
    if (!Number.isFinite(worldX) || !Number.isFinite(worldY) || !(reach >= 0 && reach <= MAX_COLLISION_REACH)) {
        if (halfSize !== _lastBadHalfSize) {
            console.warn(`[tileCollision] degenerate entity halfSize=${halfSize} at (${worldX},${worldY}); skipping wall resolution`);
            _lastBadHalfSize = halfSize;
        }
        return null;
    }
    // Clamp the scan to the wall grid. Tiles outside it are air (getTileState
    // returns 0), so skipping them changes nothing — and it keeps the loop
    // counters small. Unclamped, a far-off position (e.g. a teleport to 1e20)
    // yields tile indices past 2^53 where `tileX++` no longer increments and
    // the loops below spin forever, even over a "single" tile.
    const minTileX = Math.max(0, worldToTileX(worldX - reach));
    const maxTileX = Math.min(WALL_GRID_WIDTH - 1, worldToTileX(worldX + reach));
    const minTileY = Math.max(0, worldToTileY(worldY - reach));
    const maxTileY = Math.min(WALL_GRID_HEIGHT - 1, worldToTileY(worldY + reach));

    // The disc's bounding box, used only to bound the span over which a jagged edge
    // is sampled (the jaggedness is the wall's shape, not the entity's).
    const entityLeft = worldX - halfSize;
    const entityRight = worldX + halfSize;
    const entityTop = worldY - halfSize;
    const entityBottom = worldY + halfSize;

    // First corner-region hit found, returned only if no flat-face hit turns up.
    let cornerHit: TileCollisionResult | null = null;

    for (let tileY = minTileY; tileY <= maxTileY; tileY++) {
        for (let tileX = minTileX; tileX <= maxTileX; tileX++) {
            const state = getTileState(WALL_GRID, tileToWorldX(tileX), tileToWorldY(tileY));
            if (!isTileIdBlocking(state)) continue;

            const tileWorldX = tileToWorldX(tileX);
            const tileWorldY = tileToWorldY(tileY);

            let effectiveLeft = tileWorldX;
            let effectiveRight = tileWorldX + WALL_TILE_SIZE;
            let effectiveTop = tileWorldY;
            let effectiveBottom = tileWorldY + WALL_TILE_SIZE;

            const cfg = getTileTypeConfig(state);
            const usesJaggedEdges = cfg.style === 'wall' || cfg.style === 'water';
            if (usesJaggedEdges) {
                const jaggedEdges = getTileJaggedEdges(WALL_GRID, tileX, tileY);

                if (jaggedEdges.top) {
                    const minT = Math.max(0, entityLeft - tileWorldX);
                    const maxT = Math.min(WALL_TILE_SIZE, entityRight - tileWorldX);
                    if (maxT > minT) effectiveTop = tileWorldY - getMaxJaggedOffset(jaggedEdges.top, minT, maxT);
                }
                if (jaggedEdges.bottom) {
                    const minT = Math.max(0, entityLeft - tileWorldX);
                    const maxT = Math.min(WALL_TILE_SIZE, entityRight - tileWorldX);
                    if (maxT > minT) effectiveBottom = tileWorldY + WALL_TILE_SIZE + getMaxJaggedOffset(jaggedEdges.bottom, minT, maxT);
                }
                if (jaggedEdges.left) {
                    const minT = Math.max(0, entityTop - tileWorldY);
                    const maxT = Math.min(WALL_TILE_SIZE, entityBottom - tileWorldY);
                    if (maxT > minT) effectiveLeft = tileWorldX - getMaxJaggedOffset(jaggedEdges.left, minT, maxT);
                }
                if (jaggedEdges.right) {
                    const minT = Math.max(0, entityTop - tileWorldY);
                    const maxT = Math.min(WALL_TILE_SIZE, entityBottom - tileWorldY);
                    if (maxT > minT) effectiveRight = tileWorldX + WALL_TILE_SIZE + getMaxJaggedOffset(jaggedEdges.right, minT, maxT);
                }
            }

            // Circular hitbox. The entity is a disc of radius `halfSize` centred on
            // (worldX, worldY) — NOT its bounding square, which reached 41% further at
            // the diagonals and stopped flowers short of every wall corner. Test the
            // nearest point on the (jagged-adjusted) tile rect against that radius.
            // resolveTileCollision pushes back out to exactly this same boundary, so
            // detection and resolution still agree — no dead zone, no sawtooth shake.
            const nearX = worldX < effectiveLeft ? effectiveLeft : (worldX > effectiveRight ? effectiveRight : worldX);
            const nearY = worldY < effectiveTop ? effectiveTop : (worldY > effectiveBottom ? effectiveBottom : worldY);
            const nearDX = worldX - nearX;
            const nearDY = worldY - nearY;
            // nearDX/nearDY both 0 ⇒ the centre itself is inside the rect: always a
            // collision, including for a zero-radius probe (point projectiles).
            const centerInside = nearDX === 0 && nearDY === 0;
            if (!centerInside && nearDX * nearDX + nearDY * nearDY >= halfSize * halfSize) continue;

            const hit: TileCollisionResult = { collided: true, tileX, tileY, state, effectiveLeft, effectiveRight, effectiveTop, effectiveBottom };
            // A hit whose centre lies inside one of the tile's slabs resolves against a
            // flat face; one outside both resolves radially against a corner point.
            // Prefer face hits. On a wall built from adjacent tiles an entity pressed
            // against the face sits within its radius of a tile seam, and that seam
            // "corner" is interior geometry — resolving against it would nudge the
            // entity sideways along the wall. The neighbouring tile that actually owns
            // the centre yields the clean face push, so keep scanning for it and fall
            // back to the corner hit only if no face hit exists.
            if (nearDX === 0 || nearDY === 0) return hit;
            if (!cornerHit) cornerHit = hit;
        }
    }

    return cornerHit;
}

// Push an entity's disc out of a tile: flat faces clamp the offending axis (so the
// entity slides along the wall), convex corners push radially off the corner point
// (so the disc rolls around it instead of catching on a square's edge), and a centre
// embedded in the tile ejects along the axis of least penetration.
export function resolveTileCollision(
    entityX: number,
    entityY: number,
    entityHalfSize: number,
    collision: TileCollisionResult
): { x: number; y: number } {
    const left = collision.effectiveLeft;
    const right = collision.effectiveRight;
    const top = collision.effectiveTop;
    const bottom = collision.effectiveBottom;
    // Rest the entity exactly its own radius from the surface — the same boundary
    // checkTileCollision detects on — plus the float-error overshoot.
    const r = entityHalfSize + WALL_RESOLVE_EPS;

    const insideX = entityX > left && entityX < right;
    const insideY = entityY > top && entityY < bottom;

    if (insideX && insideY) {
        // Centre embedded in the tile: there is no nearest surface point to push
        // along, so fall back to least-penetration ejection (unchanged behaviour —
        // stepPlayerMovement's MAX_STEP cap exists to keep this off the far face).
        const overlapLeft = entityX - left;
        const overlapRight = right - entityX;
        const overlapTop = entityY - top;
        const overlapBottom = bottom - entityY;
        const minOverlap = Math.min(overlapLeft, overlapRight, overlapTop, overlapBottom);
        if (minOverlap === overlapLeft) return { x: left - r, y: entityY };
        if (minOverlap === overlapRight) return { x: right + r, y: entityY };
        if (minOverlap === overlapTop) return { x: entityX, y: top - r };
        return { x: entityX, y: bottom + r };
    }

    if (insideY) {
        // Nearest surface is the left or right face.
        return { x: entityX < left ? left - r : right + r, y: entityY };
    }
    if (insideX) {
        return { x: entityX, y: entityY < top ? top - r : bottom + r };
    }

    // Corner region: push radially so the disc's edge — not its bounding box —
    // touches the corner point.
    const cornerX = entityX < left ? left : right;
    const cornerY = entityY < top ? top : bottom;
    let dx = entityX - cornerX;
    let dy = entityY - cornerY;
    let d = Math.sqrt(dx * dx + dy * dy);
    if (!(d > 0)) { dx = 1; dy = 0; d = 1; } // sitting exactly on the corner point
    const s = r / d;
    return { x: cornerX + dx * s, y: cornerY + dy * s };
}

// Iteratively resolve wall/water collisions for an entity of the given size.
// Mirrors the server's checkPlayerWallCollisions (4 iterations to escape corners).
export function resolveEntityWallCollisions(
    x: number,
    y: number,
    halfSize: number
): { x: number; y: number; collided: boolean; unresolved?: boolean } {
    // Maze region: use the rrolf-style circle resolver (flat-wall axis clamps +
    // quarter-circle corner fillets). Shared by server physics and client
    // prediction, so both resolve maze walls identically.
    if (isInMazeRegion(x, y)) {
        return resolveMazeCollision(x, y, halfSize);
    }
    let newX = x;
    let newY = y;
    let collided = false;
    let cleared = true;
    for (let i = 0; i < 4; i++) {
        const collision = checkTileCollision(newX, newY, halfSize);
        if (collision && collision.collided) {
            const resolved = resolveTileCollision(newX, newY, halfSize, collision);
            newX = resolved.x;
            newY = resolved.y;
            collided = true;
            cleared = false;
        } else {
            cleared = true;
            break;
        }
    }
    // All 4 iterations hit, so the final push was never re-checked. One extra
    // check (deep multi-tile overlap only — never on ordinary wall contact)
    // decides whether the position actually came out clear. Callers use
    // `unresolved` to refuse entering a state the resolver can't untangle,
    // because accepting one lets min-overlap ejection flip to a tile's far
    // face and carry the entity through the wall.
    if (!cleared) {
        const residual = checkTileCollision(newX, newY, halfSize);
        cleared = !(residual && residual.collided);
    }
    return { x: newX, y: newY, collided, unresolved: !cleared };
}

// Liang-Barsky: does the segment (x0,y0)→(x1,y1) touch the axis-aligned rect?
function segmentTouchesRect(
    x0: number, y0: number, x1: number, y1: number,
    left: number, top: number, right: number, bottom: number
): boolean {
    const dx = x1 - x0;
    const dy = y1 - y0;
    let t0 = 0;
    let t1 = 1;
    const clip = (p: number, q: number): boolean => {
        if (p === 0) return q >= 0; // parallel to this edge: inside iff q >= 0
        const r = q / p;
        if (p < 0) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
        return true;
    };
    return clip(-dx, x0 - left) && clip(dx, right - x0)
        && clip(-dy, y0 - top) && clip(dy, bottom - y0)
        && t0 <= t1;
}

// True if the straight path between two entity-center positions touches any
// blocking tile. Exact segment-vs-rect tests (tiles expanded by a hair so a
// path grazing the corner point of a diagonal wall seam still counts — such
// grazes can be sub-pixel). Used by stepPlayerMovement to refuse wall
// resolutions that would carry the center across solid: per-tile min-overlap
// ejection is free to choose the far side, and accepting that teleports the
// entity through walls / across sealed diagonal seams.
function centerPathCrossesWall(x0: number, y0: number, x1: number, y1: number): boolean {
    const EPS = 0.5;
    // Clamped to the wall grid for the same reason as checkTileCollision's scan:
    // off-grid tiles are air, and unclamped indices past 2^53 stall the loops.
    const minTX = Math.max(0, worldToTileX(Math.min(x0, x1) - EPS));
    const maxTX = Math.min(WALL_GRID_WIDTH - 1, worldToTileX(Math.max(x0, x1) + EPS));
    const minTY = Math.max(0, worldToTileY(Math.min(y0, y1) - EPS));
    const maxTY = Math.min(WALL_GRID_HEIGHT - 1, worldToTileY(Math.max(y0, y1) + EPS));
    for (let tileY = minTY; tileY <= maxTY; tileY++) {
        for (let tileX = minTX; tileX <= maxTX; tileX++) {
            if (!isTileIdBlocking(getTileState(WALL_GRID, tileToWorldX(tileX), tileToWorldY(tileY)))) continue;
            if (segmentTouchesRect(
                x0, y0, x1, y1,
                tileX * WALL_TILE_SIZE - EPS, tileY * WALL_TILE_SIZE - EPS,
                (tileX + 1) * WALL_TILE_SIZE + EPS, (tileY + 1) * WALL_TILE_SIZE + EPS
            )) return true;
        }
    }
    return false;
}

// ── Shared player movement step ─────────────────────────────────────────────
// The single source of truth for player movement physics, run by BOTH the server
// (authoritative, in playerState.ts) and the client (movement prediction, in
// game.ts). Applies the gardn friction model and substepped wall/water collision.
// Keeping one implementation guarantees the client predicts exactly what the
// server will do, so there's nothing to reconcile in open movement.
//
// gardn (Server/Process/Motion.cc): velocity *= (1 - friction); velocity += accel.
// Frame-rate-independent via dt: friction decay = (1 - friction)^(dt × SIM_RATE),
// applied to both the carried velocity and the input "refill" (targetVelocity is
// the terminal velocity it converges to). DEFAULT_FRICTION = 1/3, SIM_RATE = 20.
export interface PlayerMoveState { x: number; y: number; vx: number; vy: number; }

export function stepPlayerMovement(
    state: PlayerMoveState,
    targetVX: number,
    targetVY: number,
    dt: number,
    effectiveSize: number
): PlayerMoveState {
    const GARDN_FRICTION = 1 / 3;
    const GARDN_SIM_RATE = 20;
    const frictionDecay = Math.pow(1 - GARDN_FRICTION, dt * GARDN_SIM_RATE);
    const vx = state.vx * frictionDecay + targetVX * (1 - frictionDecay);
    const vy = state.vy * frictionDecay + targetVY * (1 - frictionDecay);

    const deltaX = vx * dt;
    const deltaY = vy * dt;

    // Substep movement so a single fast step can't skip past a wall. Step size is
    // bounded by half the hitbox so collision checks always sample an overlapping
    // position against any tile in the path.
    //
    // Guard the degenerate cases that otherwise make `steps` blow up and spin the
    // loop below forever at 100% CPU — a frozen, unservable server (the long-session
    // "hang"). If effectiveSize is 0 (e.g. sizeMultiplier driven to 0 by an effect),
    // MAX_STEP would be 0 and moveDistance/0 = Infinity → steps = Infinity. A blown-up
    // velocity makes moveDistance huge for the same effect. So: floor the step at 1px,
    // and hard-cap the substep count (also catches NaN, which fails the >=1 test).
    // Additionally cap the substep below half a tile minus the edge inflation:
    // penetrating past a tile's midline flips resolveTileCollision's min-overlap
    // axis to the FAR face — a through-the-wall ejection. halfSize-bounded steps
    // only keep penetration shy of the midline for hitboxes smaller than a tile;
    // giant hitboxes (stacked size petals) need the absolute cap.
    const MAX_STEP_HARD = WALL_TILE_SIZE / 2 - JAGGED_MAX_OFFSET - COLLISION_BUFFER;
    const MAX_STEP = effectiveSize > 0 ? Math.min(effectiveSize / 2, MAX_STEP_HARD) : 1;
    const moveDistance = Math.sqrt(deltaX * deltaX + deltaY * deltaY);
    let steps = Math.ceil(moveDistance / MAX_STEP);
    if (!(steps >= 1)) steps = 1;          // NaN, 0, or negative → 1
    if (steps > 1024) steps = 1024;        // Infinity or absurd velocity → clamp
    const stepX = deltaX / steps;
    const stepY = deltaY / steps;

    let newX = state.x;
    let newY = state.y;
    for (let i = 0; i < steps; i++) {
        const prevX = newX;
        const prevY = newY;
        newX += stepX;
        newY += stepY;
        const wall = resolveEntityWallCollisions(newX, newY, effectiveSize / 2);
        // A center already inside a blocking tile (teleported into geometry) is
        // exempt from the containment guards below — resolver output is its
        // only way out, arbitrary as the direction may be.
        const startEmbedded = isTileIdBlocking(getTileState(WALL_GRID, prevX, prevY));
        if (wall.unresolved) {
            // The resolver couldn't untangle the trial position (deep multi-tile
            // overlap — e.g. the hitbox grew via a size petal while wedged in a
            // corner and a fast substep pressed it further in). Accepting its
            // output would ratchet the entity through the wall over a few ticks.
            // Fall back to the pre-step position: if that resolves cleanly to
            // somewhere its center can reach without crossing solid, take that
            // ejection (pops back into open space and stops this tick's
            // movement); otherwise pin at the pre-step position — wedged until
            // it moves toward open space, but never trading a shallow overlap
            // for a deeper one.
            if (!startEmbedded) {
                const prev = resolveEntityWallCollisions(prevX, prevY, effectiveSize / 2);
                if (!prev.unresolved && !centerPathCrossesWall(prevX, prevY, prev.x, prev.y)) {
                    newX = prev.x;
                    newY = prev.y;
                } else {
                    newX = prevX;
                    newY = prevY;
                }
                break;
            }
        } else if (wall.collided && !startEmbedded
            && centerPathCrossesWall(prevX, prevY, wall.x, wall.y)) {
            // The resolver's ejection would carry the center across a blocking
            // tile — a through-the-wall or diagonal-seam-hop teleport (per-tile
            // min-overlap resolution is free to pick the far side; a just-grown
            // hitbox wedged at a seam corner reliably triggers this). Refuse it
            // and stop this tick's movement at the pre-step position.
            newX = prevX;
            newY = prevY;
            break;
        }
        newX = wall.x;
        newY = wall.y;
    }

    return { x: newX, y: newY, vx, vy };
}

