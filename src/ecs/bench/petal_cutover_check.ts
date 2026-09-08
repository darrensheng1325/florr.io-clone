/**
 * The oracle for the petal-ring cutover.
 *
 * ---------------------------------------------------------------------------
 * Why this file exists
 * ---------------------------------------------------------------------------
 * On the projectile cutover all four gates passed while the feature was
 * completely broken. On the client cutover they passed again while health was
 * stored f32 and damage numbers on high-HP mobs were arithmetic garbage. Both
 * times the reason was the same: typechecking, the self-tests and the tick
 * harness each exercise only ONE side of a boundary, so a value that is written
 * correctly and then silently clobbered — or written correctly into a column
 * that is too narrow to hold it — fails nothing.
 *
 * The petal ring has both of those traps and a third:
 *
 *   1. NARROW COLUMN. `PlayerModifiers.petalOrbitPhase` was declared `f32`.
 *      It is an unbounded accumulator that feeds cos/sin, and the position it
 *      produces is hashed for broadcast change detection. In f32 a long-lived
 *      flower's ring quantises to a coarse ladder — and every gate stays green.
 *      This check round-trips the phase through the REAL component column via
 *      the real `openPetalRing`, and asserts exact equality against a plain
 *      JS-double accumulator. Narrow the column again and this fails on tick 1.
 *
 *   2. STATE IDENTITY. The kinematic store is keyed by (loadoutIndex,
 *      instanceIndex). Key it by ring position instead — the "obvious" dense
 *      layout — and equipping into an earlier slot hands one petal another's
 *      momentum. The fixture edits the loadout mid-run to catch that.
 *
 *   3. FLOAT DRIFT. `petalPositions` feeds a change-detection signature, so an
 *      algebraically-equal-but-differently-associated rewrite of the orbit maths
 *      costs per-tick bandwidth forever and nothing reports it. Every assertion
 *      below is EXACT equality, never a tolerance.
 *
 * So this drives the real exported pipeline — `layoutPetalRing`,
 * `openPetalRing` (which is the real ecsSync bridge, writing the real component
 * on a real World), `computeRingGeometry`, `stepPetalKinematics` — against a
 * verbatim transcription of the legacy petal kinematics, and compares every
 * emitted position bit for bit.
 *
 * ---------------------------------------------------------------------------
 * Reading and maintaining the oracle
 * ---------------------------------------------------------------------------
 * `legacyStepPetal` below is a byte-for-byte copy of the block that used to sit
 * in the middle of `updatePlayerState`, including its local constants. Do not
 * tidy it — every simplification makes it a weaker oracle. It is deliberately
 * duplicated rather than imported because importing playerState.ts binds port
 * 3000 at module scope, and the code it came from has been deleted from that
 * file anyway.
 *
 * If a TUNABLE is deliberately changed (spring force, glide rate, the mob-orbit
 * spin boost), it must be changed in BOTH places — here and in
 * ecs/systems/petalRing.ts — and the resulting failure in between is the point:
 * it forces the change to be acknowledged rather than absorbed.
 *
 * The petal stat table is SYNTHETIC rather than the real one from petals.ts.
 * That is deliberate: the real table is tuning data that changes for gameplay
 * reasons, and an oracle keyed to it would go red for reasons that have nothing
 * to do with the port. The synthetic table instead spans the BRANCH space —
 * every position mode, both clump modes, custom physics constants, wall
 * collision, burst homing — which is what the check is actually for. The real
 * configs are exercised by the live tick harness.
 */

import { ServerPlayer } from '../../player';
import { World } from '../world';
import { openPetalRing, resetSyncState } from '../../server/ecsSync';
import * as C from '../components';
import {
    computeRingGeometry,
    layoutPetalRing,
    stepPetalKinematics,
    PetalAttractionTarget,
    PetalKinematicsResult,
    PetalRingDeps,
    PetalRingStats,
    RingInstance,
} from '../systems/petalRing';
import { assertNoServerBooted } from './tick_harness';
import { mulberry32 } from './rng';

// ---------------------------------------------------------------------------
// Shared fixture types
// ---------------------------------------------------------------------------

/** A petal config. Structurally a `PetalStats` as far as either side cares. */
interface FixtureStats extends PetalRingStats {
    burstHeal?: number;
    burstShield?: number;
    burstHealChargeMs?: number;
}

/** A loadout slot, in the shape both the layout and the legacy loop expect. */
interface FixtureSlot {
    type: 'petal';
    petalType: string;
    rarity: string;
    customSize?: number;
}

/**
 * A mob, carrying BOTH radii the legacy code used.
 *
 * `gridRadius` is `enemy._radius` (the grid's cached value, which folds in pet
 * and rarity size scaling) and decides ELIGIBILITY. `configRadius` is
 * `getMobStats(...).size * 40 / 2` and decides where the attracted petal
 * PROJECTS to. The two differ for every rarity-scaled mob, and the legacy code
 * used one for each — a fact that is easy to "clean up" into a single radius, so
 * the fixture gives them different values to make sure nobody does.
 */
interface FixtureMob {
    id: string;
    x: number;
    y: number;
    isDead: boolean;
    gridRadius: number;
    configRadius: number;
}

/** The flower's state the ring reads. */
interface FixtureFlower {
    x: number;
    y: number;
    health: number;
    maxHealth: number;
    shield: number;
    sizeMultiplier: number;
    petalExtension: number;
}

const PLAYER_SIZE = 40;
const ENEMY_SIZE = 40;

/** The synthetic petal table. One entry per kinematic branch. */
const STATS: Record<string, FixtureStats> = {
    // The ordinary case: spring physics, one instance, default constants.
    basic: { count: 1, size: 1.0 },
    // Multi-instance, spread round the ring (each gets its own slot).
    spread: { count: 3, size: 0.8, speed: 1.4 },
    // Multi-instance sharing ONE slot, arranged in a cluster.
    clumped: { count: 4, size: 0.6, clumped: true },
    // Snaps to the orbit point; must acquire NO physics state.
    noPhysics: { count: 1, size: 1.2, noPhysics: true },
    // Pinned to the flower's centre; must acquire NO physics state.
    fixed: { count: 2, size: 0.9, fixedDirection: 0.5 },
    // Never extends while attacking, still contracts on defend.
    defender: { count: 1, size: 1.1, defendOnly: true, range: 1.3 },
    // range 0 collapses to the no-physics branch through a different door.
    zeroRange: { count: 1, size: 1.0, range: 0 },
    // Non-default integrator constants, exercised together.
    stiff: { count: 1, size: 1.0, springForce: 1400, damping: 0.55, spawnSmoothTime: 90 },
    // Pushed out of solid tiles; the resolved position must persist into state.
    walled: { count: 1, size: 1.5, wallCollide: true, range: 1.6 },
    // Wall-colliding AND no-physics. This one has no kinematic state at all, so
    // the wall write-back must NOT create one — if it did, the instance would
    // silently start carrying state that nothing reads and a reload would resume
    // from it. Only a petal in this combination can catch that.
    walledSnap: { count: 2, size: 1.4, wallCollide: true, noPhysics: true, range: 1.5 },
    // Flies home to deliver a burst heal once charged.
    rose: { count: 1, size: 1.0, defendOnly: true, burstHeal: 25, burstHealChargeMs: 400 },
    // Flies home to deliver a shield once charged.
    shell: { count: 1, size: 1.0, burstShield: 30, burstHealChargeMs: 600 },
};

function statsOf(slot: FixtureSlot): FixtureStats | null {
    return STATS[slot.petalType] ?? null;
}

function slot(petalType: string, customSize?: number): FixtureSlot {
    return { type: 'petal', petalType, rarity: 'common', customSize };
}

function effectiveSizeOf(s: FixtureSlot, stats: FixtureStats): number {
    return s.customSize !== undefined ? s.customSize : stats.size;
}

// ---------------------------------------------------------------------------
// A deterministic wall, shared by both sides
// ---------------------------------------------------------------------------

/**
 * A solid box, re-anchored per region.
 *
 * It has to MOVE with the region: petals orbit within a few hundred pixels of
 * their flower, so a wall fixed near the origin would never be touched by the
 * mid-map or maze flowers and the whole `wallCollide` branch would sit untested
 * while the check reported success. It is placed inside the orbit's sweep on
 * purpose, so contacts happen on most ticks.
 */
const WALL = { minX: 0, minY: 0, maxX: 0, maxY: 0 };

function anchorWall(originX: number, originY: number): void {
    WALL.minX = originX + 60;
    WALL.minY = originY - 70;
    WALL.maxX = originX + 340;
    WALL.maxY = originY + 70;
}

/**
 * Push a body out of `WALL` along whichever axis it is least deep into.
 *
 * A stand-in for `checkPlayerWallCollisions`: the real one reads the tile grid,
 * so which petals hit geometry would depend on the map. Both sides call THIS,
 * so the check is about whether the resolved position is persisted into the
 * physics state, not about wall geometry.
 */
function resolveWall(x: number, y: number, size: number): { collided: boolean; x: number; y: number } {
    const half = size / 2;
    if (x + half <= WALL.minX || x - half >= WALL.maxX
        || y + half <= WALL.minY || y - half >= WALL.maxY) {
        return { collided: false, x, y };
    }
    const pushLeft = (x + half) - WALL.minX;
    const pushRight = WALL.maxX - (x - half);
    const pushUp = (y + half) - WALL.minY;
    const pushDown = WALL.maxY - (y - half);
    wallHits++;
    const min = Math.min(pushLeft, pushRight, pushUp, pushDown);
    if (min === pushLeft) return { collided: true, x: x - pushLeft, y };
    if (min === pushRight) return { collided: true, x: x + pushRight, y };
    if (min === pushUp) return { collided: true, x, y: y - pushUp };
    return { collided: true, x, y: y + pushDown };
}

/** Ticks on which the wall actually deflected something, for the coverage report. */
let wallHits = 0;

// ---------------------------------------------------------------------------
// The oracle: the petal kinematics exactly as they were
// ---------------------------------------------------------------------------

/** `PetalPhysicsState`, copied from server/playerState.ts before the cutover. */
interface LegacyPhysicsState {
    vx: number;
    vy: number;
    x: number;
    y: number;
    spawnTime?: number;
    attractedEnemyId?: string;
    glideUntil?: number;
}

const PETAL_SPAWN_GLIDE_MS = 300;
const PETAL_RELEASE_GLIDE_MS = 250;
const PETAL_GLIDE_RATE = 14;
const SPRING_FORCE = 600;
const DAMPING = 0.72;
const SPAWN_SMOOTH_TIME = 300;

/** The legacy per-tick, per-player constants, as locals in updatePlayerState. */
interface LegacyRingConstants {
    baseRadius: number;
    defendOnlyBaseRadius: number;
    angleStep: number;
    playerRangeModifier: number;
    playerRotationSpeedModifier: number;
    playerOrbitPhase: number;
    playerPetalAttractionRadius: number;
    deltaTime: number;
    currentTime: number;
}

/**
 * `buildPetalInstances`' slot assignment, copied verbatim.
 *
 * Kept separate from the step so the ring DIVISOR — which every angle depends on
 * — is verified independently of the physics.
 */
function legacyBuildInstances(
    loadout: ReadonlyArray<FixtureSlot | null>,
): { instances: Array<{ petal: FixtureSlot; instanceIndex: number; loadoutIndex: number; slotIndex: number }>; nextSlotIndex: number } {
    const instances: Array<{ petal: FixtureSlot; instanceIndex: number; loadoutIndex: number; slotIndex: number }> = [];
    let nextSlotIndex = 0;
    for (let i = 0; i < loadout.length; i++) {
        if (i >= 10) continue;
        const petal = loadout[i];
        if (petal && petal.type === 'petal' && petal.petalType && petal.rarity) {
            const petalStats = statsOf(petal);
            if (!petalStats) continue;
            const count = petalStats.count || 1;
            if (typeof count !== 'number' || count < 1 || !isFinite(count)) continue;
            const clumped = !!petalStats.clumped;
            const sharedSlot = nextSlotIndex;
            for (let j = 0; j < count; j++) {
                const slotIndex = clumped ? sharedSlot : nextSlotIndex;
                if (!clumped) nextSlotIndex++;
                instances.push({ petal, instanceIndex: j, loadoutIndex: i, slotIndex });
            }
            if (clumped) nextSlotIndex++;
        }
    }
    return { instances, nextSlotIndex };
}

/**
 * The orbit + physics + wall-collide block of `updatePlayerState`, verbatim.
 *
 * Returns the emitted position and the two homing flags, which is exactly what
 * the surrounding legacy code consumed from it.
 */
function legacyStepPetal(
    states: Map<string, LegacyPhysicsState>,
    petalId: string,
    flower: FixtureFlower,
    petalStats: FixtureStats,
    slotIndex: number,
    instanceIndex: number,
    effectiveSize: number,
    mobs: FixtureMob[],
    k: LegacyRingConstants,
): { x: number; y: number; burstHealHoming: boolean; burstShieldHoming: boolean } {
    const {
        baseRadius, defendOnlyBaseRadius, angleStep, playerRangeModifier,
        playerRotationSpeedModifier, playerOrbitPhase, playerPetalAttractionRadius,
        deltaTime, currentTime,
    } = k;

    const rotationSpeed = (petalStats.speed ?? 1.0) * playerRotationSpeedModifier * 0.002;
    const baseAngle = slotIndex * angleStep;
    const rotationAngle = ((petalStats.speed ?? 1.0) * playerOrbitPhase * 2) % (Math.PI * 2);
    const totalAngle = petalStats.fixedDirection !== undefined ? baseAngle : baseAngle + rotationAngle;

    const petalRange = (petalStats.range ?? 1.0) * playerRangeModifier;
    const petalRadius = (petalStats.defendOnly ? defendOnlyBaseRadius : baseRadius) * petalRange;

    let targetX = flower.x + Math.cos(totalAngle) * petalRadius;
    let targetY = flower.y + Math.sin(totalAngle) * petalRadius;

    const clumpCount = petalStats.count || 1;
    if (petalStats.clumped && clumpCount > 1) {
        const clumpSpacing = effectiveSize * 20 * 0.5;
        const subAngle = (instanceIndex / clumpCount) * Math.PI * 2 + totalAngle;
        targetX += Math.cos(subAngle) * clumpSpacing;
        targetY += Math.sin(subAngle) * clumpSpacing;
    }

    let petalX: number;
    let petalY: number;
    let burstHealHoming = false;
    let burstShieldHoming = false;

    if (petalStats.fixedDirection !== undefined) {
        petalX = flower.x;
        petalY = flower.y;
    } else if (petalRange === 0 || petalStats.noPhysics) {
        petalX = targetX;
        petalY = targetY;
    } else {
        const petalSpringForce = petalStats.springForce ?? SPRING_FORCE;
        const petalDamping = petalStats.damping ?? DAMPING;
        const petalSpawnSmoothTime = petalStats.spawnSmoothTime ?? SPAWN_SMOOTH_TIME;

        let physicsState = states.get(petalId);
        if (!physicsState) {
            physicsState = {
                x: flower.x,
                y: flower.y,
                vx: 0,
                vy: 0,
                spawnTime: currentTime,
                glideUntil: currentTime + PETAL_SPAWN_GLIDE_MS,
            };
            states.set(petalId, physicsState);
        }

        const timeSinceSpawn = physicsState.spawnTime ? currentTime - physicsState.spawnTime : petalSpawnSmoothTime;
        const smoothFactor = Math.min(1.0, timeSinceSpawn / petalSpawnSmoothTime);

        burstHealHoming = !!petalStats.burstHeal &&
            flower.health < flower.maxHealth &&
            timeSinceSpawn >= (petalStats.burstHealChargeMs ?? 1000);
        burstShieldHoming = !!petalStats.burstShield &&
            flower.shield <= 0 &&
            timeSinceSpawn >= (petalStats.burstHealChargeMs ?? 1000);

        let closestEnemy: FixtureMob | null = null;
        let closestDistanceSq = Infinity;
        if (playerPetalAttractionRadius > 0 && !burstHealHoming && !burstShieldHoming) {
            for (let ai = 0; ai < mobs.length; ai++) {
                const enemy = mobs[ai];
                if (enemy.isDead) continue;
                const candidateEnemyRadius = enemy.gridRadius ?? (ENEMY_SIZE / 2);
                const dx = enemy.x - targetX;
                const dy = enemy.y - targetY;
                const distSq = dx * dx + dy * dy;
                const maxDist = playerPetalAttractionRadius + candidateEnemyRadius;
                if (distSq <= maxDist * maxDist && distSq < closestDistanceSq) {
                    closestDistanceSq = distSq;
                    closestEnemy = enemy;
                }
            }
        }

        let effectiveTargetX = targetX;
        let effectiveTargetY = targetY;

        if (closestEnemy) {
            physicsState.attractedEnemyId = closestEnemy.id;
        } else if (physicsState.attractedEnemyId !== undefined) {
            const releasedFrom = physicsState.attractedEnemyId;
            physicsState.attractedEnemyId = undefined;
            if (!mobs.some(e => e.id === releasedFrom)) {
                physicsState.glideUntil = currentTime + PETAL_RELEASE_GLIDE_MS;
            }
        }

        if (closestEnemy) {
            const closestEnemyRadius = closestEnemy.configRadius;
            const dx = targetX - closestEnemy.x;
            const dy = targetY - closestEnemy.y;
            const len = Math.sqrt(dx * dx + dy * dy);
            const mobOrbitRadius = closestEnemyRadius * 0.85;
            const MOB_ORBIT_SPIN_BOOST = 2;
            const baseProjectionAngle = len > 0 ? Math.atan2(dy, dx) : totalAngle;
            const projectionAngle = baseProjectionAngle + rotationSpeed * MOB_ORBIT_SPIN_BOOST * (deltaTime * 1000);
            effectiveTargetX = closestEnemy.x + Math.cos(projectionAngle) * mobOrbitRadius;
            effectiveTargetY = closestEnemy.y + Math.sin(projectionAngle) * mobOrbitRadius;
        }

        if (burstHealHoming || burstShieldHoming) {
            effectiveTargetX = flower.x;
            effectiveTargetY = flower.y;
            physicsState.glideUntil = currentTime + PETAL_RELEASE_GLIDE_MS;
        }

        if (physicsState.glideUntil !== undefined && currentTime < physicsState.glideUntil) {
            const approach = 1 - Math.exp(-PETAL_GLIDE_RATE * deltaTime);
            const glideX = physicsState.x + (effectiveTargetX - physicsState.x) * approach;
            const glideY = physicsState.y + (effectiveTargetY - physicsState.y) * approach;
            physicsState.vx = (glideX - physicsState.x) / deltaTime;
            physicsState.vy = (glideY - physicsState.y) / deltaTime;
            physicsState.x = glideX;
            physicsState.y = glideY;
        } else {
            if (physicsState.glideUntil !== undefined) physicsState.glideUntil = undefined;

            const SPRING_SUBSTEP_DT = 0.05;
            const substeps = Math.min(4, Math.max(1, Math.ceil(deltaTime / SPRING_SUBSTEP_DT)));
            const subDt = deltaTime / substeps;

            for (let sub = 0; sub < substeps; sub++) {
                const springDx = effectiveTargetX - physicsState.x;
                const springDy = effectiveTargetY - physicsState.y;
                const springDistance = Math.sqrt(springDx * springDx + springDy * springDy);

                let springFx = 0;
                let springFy = 0;

                if (springDistance > 0) {
                    const normalizedSpringDx = springDx / springDistance;
                    const normalizedSpringDy = springDy / springDistance;
                    springFx = normalizedSpringDx * petalSpringForce * springDistance * subDt * smoothFactor;
                    springFy = normalizedSpringDy * petalSpringForce * springDistance * subDt * smoothFactor;
                }

                physicsState.vx += springFx;
                physicsState.vy += springFy;

                physicsState.vx *= petalDamping;
                physicsState.vy *= petalDamping;

                physicsState.x += physicsState.vx * subDt;
                physicsState.y += physicsState.vy * subDt;
            }

            if (!Number.isFinite(physicsState.x) || !Number.isFinite(physicsState.y)) {
                physicsState.x = effectiveTargetX;
                physicsState.y = effectiveTargetY;
                physicsState.vx = 0;
                physicsState.vy = 0;
            }
        }

        petalX = physicsState.x;
        petalY = physicsState.y;
    }

    if (petalStats.wallCollide) {
        const resolved = resolveWall(petalX, petalY, 20 * effectiveSize);
        if (resolved.collided) {
            petalX = resolved.x;
            petalY = resolved.y;
            const ps = states.get(petalId);
            if (ps) {
                ps.x = petalX;
                ps.y = petalY;
                ps.vx = 0;
                ps.vy = 0;
            }
        }
    }

    return { x: petalX, y: petalY, burstHealHoming, burstShieldHoming };
}

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------


/**
 * Where the test flowers stand.
 *
 * `maze` is not decoration. The maze sits at (200000, 200000), which is past f32
 * precision — the same reason Position is f64. A ring checked only near the
 * origin would pass with a narrowed physics column.
 */
const START_REGIONS = [
    { name: 'origin', x: 0, y: 0 },
    { name: 'mid-map', x: 3100, y: -2400 },
    { name: 'maze', x: 200000, y: 200000 },
];

/**
 * Timesteps to cycle through.
 *
 * 0.1 is the ceiling server.ts's delta smoothing allows and is PAST the spring's
 * stability threshold (~0.089s), so it is what forces the substep loop to run
 * more than once. Without it the substep count is always 1 and the whole
 * substepping branch is untested.
 */
const DELTAS = [1 / 30, 1 / 60, 0.05, 0.08, 0.1, 0.02];

/** A `ServerPlayer` with only the fields `openPetalRing` touches. */
function makePlayer(id: string, x: number, y: number): ServerPlayer {
    return {
        id,
        name: id,
        x,
        y,
        angle: 0,
        score: 0,
        velocityX: 0,
        velocityY: 0,
        health: 100,
        maxHealth: 100,
        damage: 10,
        inventory: [],
        loadout: [],
        level: 1,
        xp: 0,
        xpToNextLevel: 100,
        speed_boost: 1,
        sizeMultiplier: 1,
        inputs: { keys: [], useMouse: false },
    } as unknown as ServerPlayer;
}

interface Failure {
    where: string;
    detail: string;
}

const LOADOUT_A: Array<FixtureSlot | null> = [
    slot('basic'), slot('spread'), slot('clumped'), slot('zeroRange'),
    slot('noPhysics'), slot('fixed'), slot('defender'), slot('walledSnap'),
    slot('stiff'), slot('walled', 1.25),
];

/** Same ring, but with a burst-homing pair swapped in and a slot emptied. */
const LOADOUT_B: Array<FixtureSlot | null> = [
    slot('basic'), slot('rose'), slot('clumped'), slot('shell'),
    slot('noPhysics'), null, slot('defender'), slot('walledSnap'),
    slot('stiff'), slot('walled', 1.25),
];

/**
 * Drive one flower's ring through both implementations and compare.
 *
 * Both sides are stepped in lockstep, instance by instance, in the same order —
 * which is the order the real petal loop uses — so a divergence is reported on
 * the tick it first appears rather than after it has been amplified.
 */
function runFlower(
    world: World,
    regionIndex: number,
    ticks: number,
    seed: number,
    failures: Failure[],
): void {
    const region = START_REGIONS[regionIndex];
    anchorWall(region.x, region.y);
    const rng = mulberry32(seed);
    const id = `check_${region.name}`;
    const player = makePlayer(id, region.x, region.y);

    const flower: FixtureFlower = {
        x: region.x,
        y: region.y,
        health: 100,
        maxHealth: 100,
        shield: 0,
        sizeMultiplier: 1,
        petalExtension: 1,
    };

    const legacyStates = new Map<string, LegacyPhysicsState>();
    let legacyPhase = 0;

    const ecsInstances: Array<RingInstance<FixtureSlot>> = [];
    const stepResult: PetalKinematicsResult = { x: 0, y: 0, angle: 0, facingAngle: 0, homing: false };
    const attraction: PetalAttractionTarget = { id: '', x: 0, y: 0, radius: 0 };

    let mobs: FixtureMob[] = [];
    // Deliberately mismatched radii; see FixtureMob.
    for (let m = 0; m < 4; m++) {
        mobs.push({
            id: `mob${m}`,
            x: region.x + 90 + m * 55,
            y: region.y - 40 + m * 30,
            isDead: false,
            gridRadius: 18 + m * 6,
            configRadius: 26 + m * 9,
        });
    }

    // The ECS side's view of the same world. Mirrors the injected deps the real
    // playerState builds, over the SAME arrays the oracle scans.
    const deps: PetalRingDeps = {
        findAttractionTarget(x, y, radius) {
            let closest: FixtureMob | null = null;
            let closestDistanceSq = Infinity;
            for (let ai = 0; ai < mobs.length; ai++) {
                const enemy = mobs[ai];
                if (enemy.isDead) continue;
                const candidateEnemyRadius = enemy.gridRadius ?? (ENEMY_SIZE / 2);
                const dx = enemy.x - x;
                const dy = enemy.y - y;
                const distSq = dx * dx + dy * dy;
                const maxDist = radius + candidateEnemyRadius;
                if (distSq <= maxDist * maxDist && distSq < closestDistanceSq) {
                    closestDistanceSq = distSq;
                    closest = enemy;
                }
            }
            if (!closest) return null;
            attraction.id = closest.id;
            attraction.x = closest.x;
            attraction.y = closest.y;
            attraction.radius = closest.configRadius;
            return attraction;
        },
        isEnemyPresent(mobId) {
            for (let i = 0; i < mobs.length; i++) if (mobs[i].id === mobId) return true;
            return false;
        },
        resolveWall,
        isHoming(stats, timeSinceSpawn) {
            const full = stats as FixtureStats;
            const chargeMs = full.burstHealChargeMs ?? 1000;
            const heal = !!full.burstHeal && flower.health < flower.maxHealth && timeSinceSpawn >= chargeMs;
            const shield = !!full.burstShield && flower.shield <= 0 && timeSinceSpawn >= chargeMs;
            return heal || shield;
        },
    };

    let now = 1_700_000_000_000;
    let loadout = LOADOUT_A;

    for (let tick = 0; tick < ticks; tick++) {
        const deltaTime = DELTAS[tick % DELTAS.length];
        now += Math.round(deltaTime * 1000);

        // --- drive the world ------------------------------------------------
        // The flower orbits, so every petal's target moves and the spring is
        // never at rest.
        const t = tick * 0.07;
        flower.x = region.x + Math.cos(t) * 240;
        flower.y = region.y + Math.sin(t * 1.3) * 190;
        player.x = flower.x;
        player.y = flower.y;

        // Extension sweeps through attack (>1), neutral and defend (<1), which
        // is the only thing that separates baseRadius from defendOnlyBaseRadius.
        flower.petalExtension = [1.0, 1.6, 0.7, 1.0][tick % 4];
        flower.sizeMultiplier = 1 + (tick % 7) * 0.25;
        // Dip below max so rose's burst-heal homing arms, then top back up.
        flower.health = tick % 90 < 45 ? 60 : 100;
        flower.shield = tick % 70 < 35 ? 0 : 40;

        for (let m = 0; m < mobs.length; m++) {
            mobs[m].x += Math.cos(tick * 0.11 + m) * 6;
            mobs[m].y += Math.sin(tick * 0.09 + m) * 6;
        }

        // Kill a mob outright at a fixed tick: an attracted petal must take the
        // RELEASE-GLIDE branch (mob gone) rather than the plain spring, and the
        // two are told apart only by whether the id is still in the array.
        if (tick === 120) mobs = mobs.filter(m => m.id !== 'mob1');
        // ...and mark one dead-but-present, which is the other case: it drops
        // out of eligibility but does NOT trigger the release glide.
        if (tick === 200) mobs[0].isDead = true;

        // Swap the loadout mid-run. Instance state is keyed by (slot, instance),
        // so every petal that did NOT change must keep its exact momentum across
        // the edit; a ring-position-keyed store would shuffle them.
        if (tick === 260) loadout = LOADOUT_B;

        // Break a petal instance on both sides, so the reload path (state
        // dropped -> re-acquired at the flower's centre -> spawn glide) runs.
        const attractionRadius = tick % 50 < 25 ? 30 : 0;
        const rotationSpeedModifier = 1 + (tick % 11) * 0.05;
        const rangeModifier = 1 + (tick % 5) * 0.1;

        // --- layout ----------------------------------------------------------
        const legacyLayout = legacyBuildInstances(loadout);
        const ecsSlotCount = layoutPetalRing(loadout, statsOf, ecsInstances);

        if (ecsSlotCount !== legacyLayout.nextSlotIndex) {
            failures.push({
                where: `${region.name} tick ${tick}`,
                detail: `slot count ${ecsSlotCount} != legacy ${legacyLayout.nextSlotIndex}`,
            });
            return;
        }
        if (ecsInstances.length !== legacyLayout.instances.length) {
            failures.push({
                where: `${region.name} tick ${tick}`,
                detail: `instance count ${ecsInstances.length} != legacy ${legacyLayout.instances.length}`,
            });
            return;
        }

        // --- the phase, through the REAL component column --------------------
        const ring = openPetalRing(world, player, now, ecsSlotCount, rotationSpeedModifier, deltaTime);
        legacyPhase = legacyPhase + rotationSpeedModifier * deltaTime;

        if (ring.orbitPhase !== legacyPhase) {
            failures.push({
                where: `${region.name} tick ${tick}`,
                detail: `orbitPhase ${ring.orbitPhase} != legacy ${legacyPhase} `
                    + '(a narrowed PlayerModifiers.petalOrbitPhase column looks exactly like this)',
            });
            return;
        }

        const neutral = 60 + (PLAYER_SIZE / 2) * (flower.sizeMultiplier - 1);
        const legacyConstants: LegacyRingConstants = {
            baseRadius: neutral * flower.petalExtension,
            defendOnlyBaseRadius: neutral * Math.min(flower.petalExtension, 1.0),
            angleStep: legacyLayout.nextSlotIndex > 0 ? (Math.PI * 2) / legacyLayout.nextSlotIndex : 0,
            playerRangeModifier: rangeModifier,
            playerRotationSpeedModifier: rotationSpeedModifier,
            playerOrbitPhase: legacyPhase,
            playerPetalAttractionRadius: attractionRadius,
            deltaTime,
            currentTime: now,
        };

        const geom = computeRingGeometry({
            playerX: flower.x,
            playerY: flower.y,
            orbitPhase: ring.orbitPhase,
            slotCount: ecsSlotCount,
            petalExtension: flower.petalExtension,
            sizeMultiplier: flower.sizeMultiplier,
            playerSize: PLAYER_SIZE,
            rangeModifier,
            rotationSpeedModifier,
            attractionRadius,
            deltaTime,
            now,
        });

        // --- step every instance, in ring order, on both sides ---------------
        for (let idx = 0; idx < ecsInstances.length; idx++) {
            const ecsInst = ecsInstances[idx];
            const legacyInst = legacyLayout.instances[idx];

            if (ecsInst.loadoutIndex !== legacyInst.loadoutIndex
                || ecsInst.instanceIndex !== legacyInst.instanceIndex
                || ecsInst.slotIndex !== legacyInst.slotIndex) {
                failures.push({
                    where: `${region.name} tick ${tick} idx ${idx}`,
                    detail: `layout mismatch: ecs (${ecsInst.loadoutIndex},${ecsInst.instanceIndex},slot ${ecsInst.slotIndex})`
                        + ` vs legacy (${legacyInst.loadoutIndex},${legacyInst.instanceIndex},slot ${legacyInst.slotIndex})`,
                });
                return;
            }

            const stats = statsOf(ecsInst.petal)!;
            const size = effectiveSizeOf(ecsInst.petal, stats);
            const petalId = `${id}_${ecsInst.loadoutIndex}_${ecsInst.instanceIndex}`;

            const legacyOut = legacyStepPetal(
                legacyStates, petalId, flower, stats,
                legacyInst.slotIndex, legacyInst.instanceIndex, size, mobs, legacyConstants,
            );

            stepPetalKinematics(
                ring.state, geom, stats,
                ecsInst.loadoutIndex, ecsInst.instanceIndex, ecsInst.slotIndex, size,
                deps, stepResult,
            );

            if (stepResult.x !== legacyOut.x || stepResult.y !== legacyOut.y) {
                failures.push({
                    where: `${region.name} tick ${tick} ${ecsInst.petal.petalType}`
                        + `[${ecsInst.loadoutIndex}:${ecsInst.instanceIndex}]`,
                    detail: `position (${stepResult.x}, ${stepResult.y}) != legacy (${legacyOut.x}, ${legacyOut.y})`,
                });
                return;
            }

            const legacyHoming = legacyOut.burstHealHoming || legacyOut.burstShieldHoming;
            if (stepResult.homing !== legacyHoming) {
                failures.push({
                    where: `${region.name} tick ${tick} ${ecsInst.petal.petalType}`,
                    detail: `homing ${stepResult.homing} != legacy ${legacyHoming}`,
                });
                return;
            }

            // A petal that takes a non-physics branch must NOT have acquired
            // kinematic state. If it did, a later `wallCollide` write-back would
            // start persisting into a state nothing else reads, and a reload
            // would resume from it.
            const hasEcsState = ring.state.peek(ecsInst.loadoutIndex, ecsInst.instanceIndex) !== undefined;
            const hasLegacyState = legacyStates.has(petalId);
            if (hasEcsState !== hasLegacyState) {
                failures.push({
                    where: `${region.name} tick ${tick} ${ecsInst.petal.petalType}`,
                    detail: `physics-state presence ${hasEcsState} != legacy ${hasLegacyState}`,
                });
                return;
            }

            // Break this instance on both sides on a fixed schedule, so the
            // drop -> re-acquire -> spawn-glide path is exercised in lockstep.
            if ((tick + idx) % 137 === 0) {
                ring.state.dropInstance(ecsInst.loadoutIndex, ecsInst.instanceIndex);
                legacyStates.delete(petalId);
            }
            // ...and a whole-slot break, which is the other reset path.
            if ((tick + idx) % 211 === 0) {
                ring.state.dropSlot(ecsInst.loadoutIndex);
                const prefix = `${id}_${ecsInst.loadoutIndex}_`;
                for (const key of Array.from(legacyStates.keys())) {
                    if (key.startsWith(prefix)) legacyStates.delete(key);
                }
            }
        }

        // The stores must stay the same SIZE too. A key-packing collision — two
        // (slot, instance) pairs landing on one key — shows up here and nowhere
        // else, because the positions would still match while one petal quietly
        // drove another's state.
        if (ring.state.size !== legacyStates.size) {
            failures.push({
                where: `${region.name} tick ${tick}`,
                detail: `ring holds ${ring.state.size} instances, legacy holds ${legacyStates.size}`,
            });
            return;
        }

        // Unused locals kept meaningful: the RNG drives nothing yet but is
        // seeded per region so adding stochastic drive later stays reproducible.
        void rng;
    }
}

/**
 * A flower's ring must die with the flower.
 *
 * The whole point of moving the store into a component is that cleanup is
 * structural rather than a prefix scan someone can forget to call. Destroying
 * the entity must clear the column slot, or the port has bought a leak instead
 * of fixing one.
 */
function checkRingReleasedOnDestroy(failures: Failure[]): void {
    resetSyncState();
    const world = new World();
    const player = makePlayer('leak_check', 0, 0);
    const opened = openPetalRing(world, player, 1_700_000_000_000, 4, 1, 1 / 30);
    opened.state.acquire(0, 0, 0, 0, 1_700_000_000_000);

    const entity = world.lookup('leak_check');
    if (entity === undefined) {
        failures.push({ where: 'destroy', detail: 'openPetalRing did not create an entity' });
        return;
    }

    const archetype = (world as unknown as {
        archetypes: Array<{ columns: Array<{ type: { id: number }; arrays: Record<string, unknown[]> } | undefined> }>;
    }).archetypes;

    world.destroy(entity);

    // The state object must no longer be reachable from ANY column slot.
    for (const a of archetype) {
        const col = a.columns[C.PetalRing.id];
        if (!col) continue;
        const arr = col.arrays.state as unknown[];
        for (let i = 0; i < arr.length; i++) {
            if (arr[i] === opened.state) {
                failures.push({
                    where: 'destroy',
                    detail: 'PetalRing.state survived world.destroy() — the ring leaks with the flower',
                });
                return;
            }
        }
    }
}

export function main(): void {
    assertNoServerBooted();

    const failures: Failure[] = [];
    const ticks = 400;

    for (let r = 0; r < START_REGIONS.length; r++) {
        resetSyncState();
        const world = new World();
        runFlower(world, r, ticks, 90210 + r * 17, failures);
    }

    checkRingReleasedOnDestroy(failures);

    // Leave the module-level sync state clean for anything that runs after.
    resetSyncState();

    if (failures.length > 0) {
        console.log(`petal cutover check: ${failures.length} FAILURE(S)`);
        for (const f of failures) console.log(`  x ${f.where}: ${f.detail}`);
        process.exitCode = 1;
        throw new Error('petal cutover check failed');
    }

    // Coverage, not decoration: `wallCollide` is the one branch whose reach
    // depends on where the fixture happens to put its geometry, and a wall the
    // orbit never touches would leave it untested while the check still passed.
    if (wallHits === 0) {
        console.log('petal cutover check: FAILURE — the wallCollide branch was never reached');
        process.exitCode = 1;
        throw new Error('petal cutover check failed');
    }

    const regions = START_REGIONS.map(r => r.name).join(', ');
    console.log(
        `petal cutover check: ECS petal ring matches the legacy kinematics exactly `
        + `(${ticks} ticks x ${START_REGIONS.length} regions: ${regions}; `
        + `${wallHits} wall deflections)`,
    );
}
