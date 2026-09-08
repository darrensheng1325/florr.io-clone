/**
 * The petal ring: orbit layout and kinematics.
 *
 * ===========================================================================
 * THE MODELLING DECISION: petal instances are NOT entities
 * ===========================================================================
 * The idiomatic ECS answer is one entity per petal instance, with a reference
 * back to the owning flower. That answer is wrong for THIS code, and the reason
 * is not taste — four concrete access patterns rule it out.
 *
 * 1. LIFETIME IS DERIVED, NOT EVENTED.
 *    Nothing ever "spawns a petal". The authoritative record of which instances
 *    exist is `player.loadout`, a legacy-owned array that a dozen sites REPLACE
 *    wholesale: equipping, the cooldown restore (which builds a brand-new Item
 *    object), PVP arena enter/exit, the maze rarity shift, splitter cloning,
 *    cross-server transfer, respawn. The instance list is recomputed from that
 *    array every tick by `layoutPetalRing` below. Entities would need a
 *    reconcile pass — the `reconcileEnemyEntities` shape — running for every
 *    player every tick over up to ~70 instances, with an archetype move (a full
 *    row copy plus a swap-remove) every time anyone touches their loadout.
 *    That is real per-tick churn bought to reproduce a keyed lookup.
 *
 * 2. HEALTH IS PER-SLOT, NOT PER-INSTANCE, FOR ALMOST EVERY PETAL.
 *    `getInstanceHealth`/`setInstanceHealth` fall through to `petal.health` —
 *    one shared number — unless the petal is `clumped` or `independentHealth`.
 *    A 5-count rose is five instances sharing one health pool and one cooldown;
 *    damage to any of them breaks all five. An entity-per-instance would either
 *    have no Health of its own (so the component that supposedly justifies the
 *    entity lives elsewhere) or hold a COPY of the slot's health — which is
 *    exactly the dual-representation shape that made mobs unkillable on the
 *    projectile cutover. The per-instance arrays that DO exist
 *    (`instanceHealth`, `instanceOnCooldown`, `instanceCooldownEndTime`) live
 *    on the loadout Item because the Item is persisted to the database and
 *    serialised across the cross-server portal; an entity cannot own state that
 *    has to survive leaving the process.
 *
 * 3. NOTHING QUERIES PETALS EXCEPT BY OWNER, IN SLOT ORDER.
 *    No spatial index contains petals — every petal-vs-X test is petal-driven
 *    (the petal queries the mob grid, the projectile bridge, the player map).
 *    No system iterates petals globally. Every single access is "for this
 *    flower, walk its instances in loadout order". Archetype iteration buys
 *    dense chunked iteration that has no consumer here, and its order is
 *    swap-remove order — neither stable nor owner-grouped.
 *
 * 4. THE BROADCAST NEEDS A DENSE PER-PLAYER ARRAY IN RING ORDER.
 *    `player.petalPositions` feeds a change-detection signature in
 *    tickBroadcast, so its ORDER and LENGTH are load-bearing. Rebuilding it
 *    bit-identically means walking the loadout in slot order — i.e. the
 *    derivation — not gathering scattered entities and sorting them back.
 *
 * So the flower is the entity and the ring is component data on it: `PetalRing`
 * (see components/petal.ts) carries a `PetalRingState`, and the orbit phase
 * lives in the `PlayerModifiers` column it always belonged in. The ECS owns the
 * KINEMATIC state (where each instance physically is, and how it got there);
 * the loadout Item keeps the state that has to outlive the process.
 *
 * Why the store is a Map of small structs rather than struct-of-arrays: the set
 * is at most ~70 entries per flower, it is never iterated densely (only point
 * looked-up by (loadoutIndex, instanceIndex)), and its indexing must stay
 * STABLE across loadout edits — equipping into slot 3 must not shift slot 5's
 * physics onto a different petal, which is precisely what a dense ring-ordered
 * layout would do. SoA's win is dense iteration; there is none to win here.
 *
 * ---------------------------------------------------------------------------
 * Two invariants this file exists to protect
 * ---------------------------------------------------------------------------
 * PREVIOUS-TICK PHASING. Every orbit position is measured from the flower's
 * PREVIOUS committed position, not its live one. That is why the geometry takes
 * `playerX`/`playerY` as explicit numbers instead of reading a Position column:
 * inside `updatePlayerState` the ECS-integrated position is parked in the
 * `movedX`/`movedY` staging pair and `player.x`/`player.y` still hold last
 * tick's commit, which is what makes petals trail the flower instead of
 * orbiting its live centre. Handing this module the entity would let it read
 * the wrong one.
 *
 * BIT-IDENTICAL POSITIONS. `petalPositions` is hashed for broadcast change
 * detection, so float drift here costs per-tick bandwidth forever. Every
 * expression below is a transcription of the code it replaced, in the same
 * association order, including the parts that look redundant (the `% (PI*2)` on
 * an angle that is about to go through cos/sin, the `* 2` that undoes a
 * historical 0.002 rad/ms rate, the separate `defendOnly` base radius). Do not
 * simplify them. `ecs/bench/petal_cutover_check.ts` asserts exact equality
 * against a verbatim copy of the legacy code and will fail on any algebraic
 * "cleanup".
 *
 * ---------------------------------------------------------------------------
 * What is NOT here, and why
 * ---------------------------------------------------------------------------
 * Petal COMBAT (mob damage, poison, slow, knockback, projectile blocking, PVP,
 * the flower/bubble/yggdrasil specials), the break/cooldown/reload state
 * machine, the fields and auras, and the petal action VM all stay in
 * playerState.ts / petal_actions.ts. The reason is ordering, not effort: the
 * legacy loop INTERLEAVES kinematics and effects per instance, and instance k's
 * effects change instance k+1's kinematics — a kill removes an attraction
 * target, and (for a shared-health slot) damage to instance 0 makes instance 1
 * take the break path and emit no position at all. Batching all the kinematics
 * first would therefore change both the VALUES and the LENGTH of
 * `petalPositions`. So this module is stepped per instance, in the legacy
 * order, by the legacy loop — the same shape as `stepPlayerMovement`, which is
 * shared verbatim rather than reimplemented.
 */

// ---------------------------------------------------------------------------
// Tunables, moved verbatim from server/playerState.ts
// ---------------------------------------------------------------------------

/** Reload/spawn: fly-out from the flower into orbit. */
export const PETAL_SPAWN_GLIDE_MS = 300;
/** The mob a petal was attracted to died: glide back into orbit. */
export const PETAL_RELEASE_GLIDE_MS = 250;
/** 1/s first-order approach rate (~95% converged in 220ms). */
export const PETAL_GLIDE_RATE = 14;
/** Spring force back to the orbit position, px/s^2. */
export const PETAL_SPRING_FORCE = 600;
/** Velocity damping per substep (0-1, lower = more damping). */
export const PETAL_DAMPING = 0.72;
/** Time in ms over which spring force ramps up after a (re)spawn. */
export const PETAL_SPAWN_SMOOTH_TIME = 300;

/**
 * Substep cap for the orbit spring.
 *
 * The semi-implicit Euler spring below is unconditionally unstable once dt
 * exceeds sqrt(2*(1+damping)/(damping*springForce)) — ~0.089s at the defaults —
 * because the tracked error's per-tick growth factor passes -(1+damping) and
 * diverges with no restoring force able to recover it. That is how a petal
 * "flies off and never returns". server.ts's delta smoothing allows dt up to
 * 0.1s under load, which is past the threshold, so the integration is substepped
 * to keep each slice safely below it regardless of real tick time.
 */
const SPRING_SUBSTEP_DT = 0.05;
const SPRING_MAX_SUBSTEPS = 4;

/**
 * Extra angular kick applied on top of the projection point's own rotation when
 * a petal is whipping around a mob. Most of the spin falls out for free from the
 * player orbit dragging the projection point around the mob's edge each frame;
 * this makes it feel snappier. Bigger = faster whip.
 */
const MOB_ORBIT_SPIN_BOOST = 2;

/** Fraction of a mob's radius the attracted petal orbits at (slightly inside). */
const MOB_ORBIT_RADIUS_SCALE = 0.85;

/**
 * Loadout slots that put petals in orbit. Slots 10+ are storage only.
 * Mirrors the `i >= 10` guard the legacy layout used.
 */
export const PRIMARY_LOADOUT_SLOTS = 10;

/**
 * Stride used to pack (loadoutIndex, instanceIndex) into one integer key.
 *
 * Comfortably above any configured `count` (the largest in petals.ts is 10) and
 * a power of two, so the pack is a shift. A count that somehow exceeded this
 * would COLLIDE keys between adjacent slots rather than error, so
 * `layoutPetalRing` refuses such a count outright.
 */
const PETAL_SLOT_STRIDE = 1024;

/** Pack an instance's stable identity into one integer. */
export function petalInstanceKey(loadoutIndex: number, instanceIndex: number): number {
    return loadoutIndex * PETAL_SLOT_STRIDE + instanceIndex;
}

// ---------------------------------------------------------------------------
// The config slice the ring reads
// ---------------------------------------------------------------------------

/**
 * The subset of `PetalStats` the kinematics depend on.
 *
 * Declared structurally rather than imported so this module stays inside
 * `src/ecs` (petals.ts is config, but the ECS layer deliberately owns no import
 * edge to the game's data modules beyond interning). A real `PetalStats` is
 * assignable to this.
 */
export interface PetalRingStats {
    /** Instances this equipped petal puts in orbit. */
    count: number;
    /** Base visual/hit size, before a `customSize` override. */
    size: number;
    /** Rotation speed multiplier (default 1.0). */
    speed?: number;
    /** Orbit radius multiplier (default 1.0). */
    range?: number;
    /** All instances share one orbit slot instead of spreading round the ring. */
    clumped?: boolean;
    /** Never extends while attacking; still contracts on defend. */
    defendOnly?: boolean;
    /** Snaps to the orbit point with no spring/damping. */
    noPhysics?: boolean;
    /** Sits on the flower at a fixed facing instead of orbiting. */
    fixedDirection?: number;
    /** Pushed out of solid tiles instead of orbiting through them. */
    wallCollide?: boolean;
    springForce?: number;
    damping?: number;
    spawnSmoothTime?: number;
}

/** One entry of the expanded ring: an instance and the orbit slot it occupies. */
export interface RingInstance<TSlot = unknown> {
    /** The loadout Item this instance belongs to. */
    petal: TSlot;
    /** Index within the slot's `count`. */
    instanceIndex: number;
    /** Which loadout slot it came from. */
    loadoutIndex: number;
    /** Which of the `slotCount` ring positions it sits at. */
    slotIndex: number;
}

/** What a loadout slot must look like for the layout to expand it. */
interface LayoutSlot {
    type?: string;
    petalType?: string;
    rarity?: string;
}

/**
 * Expand a loadout into one ring entry per petal instance, assigning orbit
 * slots.
 *
 * Ported from `buildPetalInstances`. Petals with `count` occupy `count` slots;
 * `clumped` petals share ONE slot so their instances cluster around a single
 * ring position instead of spreading evenly. The returned slot count is the ring
 * divisor — `angleStep = 2*PI / slotCount`.
 *
 * `out` is cleared and refilled so the caller can keep one array for the whole
 * server lifetime; the ring is rebuilt every tick for every player and
 * allocating a fresh array each time showed up in profiles.
 *
 * The per-instance SIDE EFFECTS the legacy function also performed (sizing the
 * instanceHealth arrays, seeding petal action states) stay with the caller:
 * they touch the loadout Item and the action VM, neither of which is ECS state.
 */
export function layoutPetalRing<TSlot extends LayoutSlot>(
    slots: ReadonlyArray<TSlot | null | undefined>,
    statsOf: (slot: TSlot) => PetalRingStats | null | undefined,
    out: Array<RingInstance<TSlot>>,
): number {
    out.length = 0;
    let nextSlotIndex = 0;

    for (let i = 0; i < slots.length; i++) {
        // Secondary loadout (slots 10+) is storage only — no orbit, no ring slot.
        if (i >= PRIMARY_LOADOUT_SLOTS) continue;
        const petal = slots[i];
        if (!petal || petal.type !== 'petal' || !petal.petalType || !petal.rarity) continue;

        const stats = statsOf(petal);
        if (!stats) continue;

        const count = stats.count || 1;
        // A bad count would either spin the inner loop forever or collide packed
        // instance keys between adjacent slots. Refuse it, as the original did.
        if (typeof count !== 'number' || count < 1 || !isFinite(count)
            || count > PETAL_SLOT_STRIDE) {
            console.warn('Invalid petal count:', count, 'for', petal.petalType, petal.rarity);
            continue;
        }

        const clumped = !!stats.clumped;
        const sharedSlot = nextSlotIndex;
        for (let j = 0; j < count; j++) {
            const slotIndex = clumped ? sharedSlot : nextSlotIndex;
            if (!clumped) nextSlotIndex++;
            out.push({ petal, instanceIndex: j, loadoutIndex: i, slotIndex });
        }
        if (clumped) nextSlotIndex++;
    }

    return nextSlotIndex;
}

// ---------------------------------------------------------------------------
// Per-instance kinematic state
// ---------------------------------------------------------------------------

/**
 * Where one petal instance physically is, and how it is getting where it should
 * be.
 *
 * Mutated in place; never reallocated except on (re)spawn, so a steady-state
 * ring allocates nothing per tick.
 */
export interface PetalPhysics {
    x: number;
    y: number;
    vx: number;
    vy: number;
    /** When this instance entered orbit; drives the force ramp-up. */
    spawnTime: number;
    /**
     * Deadline of a transit glide, or 0 for none.
     *
     * 0 rather than `undefined` because every assignment is `now + positive` and
     * `now` is a wallclock in ms, so 0 is unreachable as a real deadline and the
     * field stays in a monomorphic numeric shape.
     */
    glideUntil: number;
    /** Mob id this instance is attraction-locked to, for smooth release. */
    attractedEnemyId: string | undefined;
}

/**
 * One flower's petal kinematics, stored in the `PetalRing` component.
 *
 * Keyed by the instance's STABLE identity — (loadoutIndex, instanceIndex) — not
 * by its position in the ring, because ring position shifts whenever any earlier
 * slot's count changes and a shifted key would hand slot 5's petal slot 3's
 * momentum.
 *
 * Owned by the entity: when the player entity is destroyed the component column
 * slot is cleared and the whole map goes with it. That replaces the global
 * `petalPhysicsStates` map, which needed an explicit prefix-scan cleanup on
 * every disconnect and leaked a flower's entire ring if that call was ever
 * missed.
 */
export class PetalRingState {
    private readonly instances = new Map<number, PetalPhysics>();

    /** Live instance count. Diagnostics and tests. */
    get size(): number {
        return this.instances.size;
    }

    /** The state for an instance, or undefined if it has none yet. */
    peek(loadoutIndex: number, instanceIndex: number): PetalPhysics | undefined {
        return this.instances.get(petalInstanceKey(loadoutIndex, instanceIndex));
    }

    /**
     * The state for an instance, creating it at the flower's centre if absent.
     *
     * A new or reloaded petal starts ON the flower and glides out into orbit
     * (overshoot-free — see the glide branch in `stepPetalKinematics`), which is
     * what makes a reload read as the petal flying back out rather than
     * teleporting into place.
     */
    acquire(
        loadoutIndex: number,
        instanceIndex: number,
        playerX: number,
        playerY: number,
        now: number,
    ): PetalPhysics {
        const key = petalInstanceKey(loadoutIndex, instanceIndex);
        let state = this.instances.get(key);
        if (state === undefined) {
            state = {
                x: playerX,
                y: playerY,
                vx: 0,
                vy: 0,
                spawnTime: now,
                glideUntil: now + PETAL_SPAWN_GLIDE_MS,
                attractedEnemyId: undefined,
            };
            this.instances.set(key, state);
        }
        return state;
    }

    /**
     * Forget one instance's spring state.
     *
     * Called when an instance breaks, so its reload re-initialises at the
     * flower's centre and flies back out instead of resuming from wherever it
     * was floating when it died.
     */
    dropInstance(loadoutIndex: number, instanceIndex: number): void {
        this.instances.delete(petalInstanceKey(loadoutIndex, instanceIndex));
    }

    /** Forget every instance of one loadout slot (a whole-slot break). */
    dropSlot(loadoutIndex: number): void {
        const first = petalInstanceKey(loadoutIndex, 0);
        const end = first + PETAL_SLOT_STRIDE;
        for (const key of this.instances.keys()) {
            if (key >= first && key < end) this.instances.delete(key);
        }
    }

    /** Drop everything. For tests and for a clean world rebuild. */
    clear(): void {
        this.instances.clear();
    }
}

// ---------------------------------------------------------------------------
// Per-tick, per-player geometry
// ---------------------------------------------------------------------------

/**
 * The ring constants for one flower for one tick.
 *
 * Computed once per player by `computeRingGeometry` and then read by every
 * instance step, so the trig inputs are shared and identical across the ring.
 */
export interface PetalRingGeometry {
    /**
     * The flower's PREVIOUS-tick committed position. See the file header: this
     * is deliberately a pair of numbers rather than an entity, because inside
     * `updatePlayerState` the live integrated position lives in the movedX/movedY
     * staging pair and reading it here would stop petals trailing the flower.
     */
    playerX: number;
    playerY: number;
    /** Accumulated integral of the rotation-speed modifier (see below). */
    orbitPhase: number;
    /** 2*PI / slotCount, or 0 when the ring is empty. */
    angleStep: number;
    /** Orbit radius for ordinary petals, already scaled by petal extension. */
    baseRadius: number;
    /** Orbit radius for `defendOnly` petals, whose extension is clamped at 1. */
    defendOnlyBaseRadius: number;
    /** Player-wide orbit range multiplier. */
    rangeModifier: number;
    /** Player-wide rotation speed multiplier. */
    rotationSpeedModifier: number;
    /** Pixels within which a petal is pulled toward the closest mob (0 = off). */
    attractionRadius: number;
    /** Smoothed timestep in SECONDS. */
    deltaTime: number;
    /** Server clock in ms, sampled once for the tick. */
    now: number;
}

/** Inputs to `computeRingGeometry` that come from outside the ECS. */
export interface RingGeometryInput {
    playerX: number;
    playerY: number;
    orbitPhase: number;
    /** Number of ring slots `layoutPetalRing` consumed. */
    slotCount: number;
    /** `inputs.petalExtension || 1.0`. */
    petalExtension: number;
    /** The flower's size multiplier. */
    sizeMultiplier: number;
    /** PLAYER_SIZE. Injected: the ECS layer does not import constants.ts. */
    playerSize: number;
    rangeModifier: number;
    rotationSpeedModifier: number;
    attractionRadius: number;
    deltaTime: number;
    now: number;
}

/**
 * The neutral orbit radius before extension.
 *
 * Only the BODY-RADIUS portion is scaled by `sizeMultiplier`, so petals stay a
 * constant distance from the flower's edge as it grows rather than being flung
 * outward proportionally.
 */
function neutralOrbitRadius(playerSize: number, sizeMultiplier: number): number {
    return 60 + (playerSize / 2) * (sizeMultiplier - 1);
}

/** Derive this tick's ring constants. */
export function computeRingGeometry(input: RingGeometryInput): PetalRingGeometry {
    const neutral = neutralOrbitRadius(input.playerSize, input.sizeMultiplier);
    return {
        playerX: input.playerX,
        playerY: input.playerY,
        orbitPhase: input.orbitPhase,
        angleStep: input.slotCount > 0 ? (Math.PI * 2) / input.slotCount : 0,
        baseRadius: neutral * input.petalExtension,
        // Defend-only petals (rose) never fly out while attacking — their
        // extension is clamped at the neutral orbit, though they still pull in
        // on defend (<1).
        defendOnlyBaseRadius: neutral * Math.min(input.petalExtension, 1.0),
        rangeModifier: input.rangeModifier,
        rotationSpeedModifier: input.rotationSpeedModifier,
        attractionRadius: input.attractionRadius,
        deltaTime: input.deltaTime,
        now: input.now,
    };
}

/**
 * Advance the orbit phase.
 *
 * The rotation-speed modifier is INTEGRATED rather than multiplied into the
 * clock: swapping a petal that changes the modifier (Faster, Yin Yang) must bend
 * the rate from that point forward, where remapping `now * newSpeed` would yank
 * every petal to a different angle on the tick the swap lands.
 */
export function advanceOrbitPhase(
    orbitPhase: number,
    rotationSpeedModifier: number,
    deltaTime: number,
): number {
    return orbitPhase + rotationSpeedModifier * deltaTime;
}

/** Where one instance's orbit point sits this tick. */
export interface PetalOrbitTarget {
    x: number;
    y: number;
    /** The instance's bearing from the flower, before any clump offset. */
    angle: number;
    /**
     * The direction this INSTANCE faces.
     *
     * Equal to `angle` for an unclumped petal, but for a clumped one it is the
     * sub-bearing that places the instance inside its clump — i.e. the outward
     * direction from the clump centre. A clump of four peas therefore faces
     * four ways, and fires four ways, instead of all firing down the shared
     * slot bearing.
     */
    facingAngle: number;
    /** `(stats.range ?? 1) * rangeModifier`; 0 means "no physics, sit on target". */
    range: number;
}

/**
 * The orbit point an instance is springing toward.
 *
 * Shared by the main petal loop AND by the field-drop pre-pass (pollen puffs and
 * thrown webs land at their instance's own orbit position), so the two cannot
 * drift apart.
 *
 * `effectiveSize` is the petal's `customSize` override when it has one, else
 * `stats.size` — the caller resolves it because the override lives on the
 * loadout Item.
 */
export function petalOrbitTarget(
    geom: PetalRingGeometry,
    stats: PetalRingStats,
    slotIndex: number,
    instanceIndex: number,
    effectiveSize: number,
    out: PetalOrbitTarget,
): void {
    const baseAngle = slotIndex * geom.angleStep;
    // The per-petal speed times the integrated phase. The `* 2` preserves the
    // original 0.002 rad/ms x 1000 ms/s rate; the modulo is kept because the
    // legacy expression had it and removing it perturbs the low bits.
    const rotationAngle = ((stats.speed ?? 1.0) * geom.orbitPhase * 2) % (Math.PI * 2);
    // Fixed-direction petals don't orbit — they hold a fixed relative bearing.
    const totalAngle = stats.fixedDirection !== undefined ? baseAngle : baseAngle + rotationAngle;

    const range = (stats.range ?? 1.0) * geom.rangeModifier;
    const radius = (stats.defendOnly ? geom.defendOnlyBaseRadius : geom.baseRadius) * range;

    let x = geom.playerX + Math.cos(totalAngle) * radius;
    let y = geom.playerY + Math.sin(totalAngle) * radius;

    // Clumped petals arrange their instances in a small cluster around the
    // single slot centre they share.
    const clumpCount = stats.count || 1;
    let facingAngle = totalAngle;
    if (stats.clumped && clumpCount > 1) {
        const clumpSpacing = effectiveSize * 20 * 0.5;
        const subAngle = (instanceIndex / clumpCount) * Math.PI * 2 + totalAngle;
        x += Math.cos(subAngle) * clumpSpacing;
        y += Math.sin(subAngle) * clumpSpacing;
        // The instance sits out along `subAngle` from the clump centre, so that
        // is the way it faces — and the way anything it launches goes.
        facingAngle = subAngle;
    }

    out.x = x;
    out.y = y;
    out.angle = totalAngle;
    out.facingAngle = facingAngle;
    out.range = range;
}

// ---------------------------------------------------------------------------
// The instance step
// ---------------------------------------------------------------------------

/** The mob a petal has locked onto, as the ring needs to see it. */
export interface PetalAttractionTarget {
    /** Mob id, so a release can ask whether it died or merely drifted away. */
    id: string;
    x: number;
    y: number;
    /**
     * The mob's radius from its CONFIG (`size * 40 / 2`), not the grid's cached
     * `_radius`. The legacy code used the config value for the projection and
     * the cached one only for the eligibility test, and the two differ for pets
     * and rarity-scaled mobs — so the split is preserved rather than unified.
     */
    radius: number;
}

/** What the ring needs from the world outside the ECS. */
export interface PetalRingDeps {
    /**
     * The closest attractable mob to (x, y) within `radius`, or null.
     *
     * INJECTED rather than queried from the ECS spatial grid on purpose. The
     * legacy attraction reads server/enemyGrid, which is rebuilt once per tick
     * at the top of the loop; the ECS grid is rebuilt inside the mob tick and
     * again inside the projectile tick, so it holds mob positions from a
     * different point in the tick. Swapping grids would silently change which
     * mob a petal locks onto — a behavioural change dressed up as a refactor.
     */
    findAttractionTarget(x: number, y: number, radius: number): PetalAttractionTarget | null;
    /**
     * Whether a mob id is still in the world.
     *
     * Distinguishes the two ways an attraction ends: the mob DIED (glide back
     * into orbit, because the raw spring would cross the whole gap in two ticks
     * and read as the entire ring jumping) versus the orbit simply swept out of
     * range (resume normally).
     */
    isEnemyPresent(id: string): boolean;
    /**
     * Push a `wallCollide` petal out of solid geometry.
     * `size` is the petal's full diameter, matching the legacy call.
     */
    resolveWall(x: number, y: number, size: number): { collided: boolean; x: number; y: number };
    /**
     * Whether this instance is homing to the flower to deliver a burst effect
     * (rose's heal, shell's shield).
     *
     * A predicate rather than a flag because it depends on `timeSinceSpawn`,
     * which only exists once the physics state has been acquired inside the
     * step. The caller uses the call to record WHICH burst fired; the ring only
     * needs to know that the target is the flower and the glide stays open.
     */
    isHoming(stats: PetalRingStats, timeSinceSpawn: number): boolean;
}

/** Where an instance ended up, and what happened on the way. */
export interface PetalKinematicsResult {
    x: number;
    y: number;
    /**
     * The instance's orbit BEARING this tick, before any clump offset and
     * regardless of which position mode it took.
     *
     * Reported because a thrown web is flung along it rather than from the
     * petal's resolved position. Recomputing it at that site is how a
     * duplicated formula starts drifting. A projectile petal fires along
     * `facingAngle` instead, which differs only inside a clump.
     */
    angle: number;
    /**
     * The instance's own facing, `angle` plus its clump sub-bearing. A
     * projectile petal fires down THIS bearing so the members of a clump shoot
     * outward in different directions rather than all down the slot bearing.
     */
    facingAngle: number;
    /** True when `isHoming` fired this tick (the caller delivers the burst). */
    homing: boolean;
}

/** Scratch, reused across every instance of every player. */
const orbitScratch: PetalOrbitTarget = { x: 0, y: 0, angle: 0, facingAngle: 0, range: 0 };

/**
 * Advance one petal instance and report where it is.
 *
 * Three mutually exclusive position modes, exactly as before:
 *
 *   fixedDirection   pinned to the flower's centre; no state, no physics
 *   range 0/noPhysics snapped to the orbit point; no state, no physics
 *   otherwise        spring/glide physics against the (possibly redirected)
 *                    orbit point, with per-instance state
 *
 * `wallCollide` applies to all three, and writes the resolved position back into
 * the physics state (killing the velocity into the wall) so the spring does not
 * keep driving the petal back inside on the next tick.
 */
export function stepPetalKinematics(
    ring: PetalRingState,
    geom: PetalRingGeometry,
    stats: PetalRingStats,
    loadoutIndex: number,
    instanceIndex: number,
    slotIndex: number,
    effectiveSize: number,
    deps: PetalRingDeps,
    out: PetalKinematicsResult,
): void {
    petalOrbitTarget(geom, stats, slotIndex, instanceIndex, effectiveSize, orbitScratch);
    const targetX = orbitScratch.x;
    const targetY = orbitScratch.y;
    const totalAngle = orbitScratch.angle;
    const petalRange = orbitScratch.range;

    out.angle = totalAngle;
    out.facingAngle = orbitScratch.facingAngle;
    out.homing = false;

    let petalX: number;
    let petalY: number;

    if (stats.fixedDirection !== undefined) {
        // Fixed-direction petals sit directly on the flower.
        petalX = geom.playerX;
        petalY = geom.playerY;
    } else if (petalRange === 0 || stats.noPhysics) {
        // No lag behind the player: snap straight to the orbit point.
        petalX = targetX;
        petalY = targetY;
    } else {
        const springForce = stats.springForce ?? PETAL_SPRING_FORCE;
        const damping = stats.damping ?? PETAL_DAMPING;
        const spawnSmoothTime = stats.spawnSmoothTime ?? PETAL_SPAWN_SMOOTH_TIME;

        const state = ring.acquire(loadoutIndex, instanceIndex, geom.playerX, geom.playerY, geom.now);

        // Ramp forces up over the smooth window so a fresh petal does not get
        // slingshotted out of the flower.
        const timeSinceSpawn = state.spawnTime ? geom.now - state.spawnTime : spawnSmoothTime;
        const smoothFactor = Math.min(1.0, timeSinceSpawn / spawnSmoothTime);

        const homing = deps.isHoming(stats, timeSinceSpawn);
        out.homing = homing;

        // Eligibility is measured from the ORBIT point, not from the petal's
        // current physics-displaced position and not from the flower: "30px
        // attraction" then reliably lights up when a mob is 30px from where the
        // petal will naturally swing past.
        let target: PetalAttractionTarget | null = null;
        if (geom.attractionRadius > 0 && !homing) {
            target = deps.findAttractionTarget(targetX, targetY, geom.attractionRadius);
        }

        let effectiveTargetX = targetX;
        let effectiveTargetY = targetY;

        if (target) {
            state.attractedEnemyId = target.id;
        } else if (state.attractedEnemyId !== undefined) {
            const releasedFrom = state.attractedEnemyId;
            state.attractedEnemyId = undefined;
            if (!deps.isEnemyPresent(releasedFrom)) {
                state.glideUntil = geom.now + PETAL_RELEASE_GLIDE_MS;
            }
        }

        if (target) {
            // Redirect the spring to a point just inside the mob's hitbox edge,
            // along the bearing of the natural orbit point FROM the mob. As the
            // player's orbit rotates, that projection rotates around the mob —
            // so the petal spinning around its victim falls out of the existing
            // rotation with no dedicated angular-motion code.
            const dx = targetX - target.x;
            const dy = targetY - target.y;
            const len = Math.sqrt(dx * dx + dy * dy);
            const mobOrbitRadius = target.radius * MOB_ORBIT_RADIUS_SCALE;
            // Per-frame angular velocity in rad/ms, integrated against this
            // frame's deltaTime below.
            const rotationSpeed = (stats.speed ?? 1.0) * geom.rotationSpeedModifier * 0.002;
            const baseProjectionAngle = len > 0 ? Math.atan2(dy, dx) : totalAngle;
            const projectionAngle =
                baseProjectionAngle + rotationSpeed * MOB_ORBIT_SPIN_BOOST * (geom.deltaTime * 1000);
            effectiveTargetX = target.x + Math.cos(projectionAngle) * mobOrbitRadius;
            effectiveTargetY = target.y + Math.sin(projectionAngle) * mobOrbitRadius;
        }

        if (homing) {
            // Fly straight home. Re-arming the glide every tick keeps the
            // overshoot-free first-order approach in play instead of the spring,
            // so the petal tracks a moving flower and lands cleanly.
            effectiveTargetX = geom.playerX;
            effectiveTargetY = geom.playerY;
            state.glideUntil = geom.now + PETAL_RELEASE_GLIDE_MS;
        }

        if (state.glideUntil !== 0 && geom.now < state.glideUntil) {
            // Transit glide (spawn fly-out / post-kill release): first-order
            // approach toward the live target. vx/vy track the glide motion so
            // the spring takes over seamlessly when the window closes.
            const approach = 1 - Math.exp(-PETAL_GLIDE_RATE * geom.deltaTime);
            const glideX = state.x + (effectiveTargetX - state.x) * approach;
            const glideY = state.y + (effectiveTargetY - state.y) * approach;
            state.vx = (glideX - state.x) / geom.deltaTime;
            state.vy = (glideY - state.y) / geom.deltaTime;
            state.x = glideX;
            state.y = glideY;
        } else {
            if (state.glideUntil !== 0) state.glideUntil = 0;

            const substeps = Math.min(
                SPRING_MAX_SUBSTEPS,
                Math.max(1, Math.ceil(geom.deltaTime / SPRING_SUBSTEP_DT)),
            );
            const subDt = geom.deltaTime / substeps;

            for (let sub = 0; sub < substeps; sub++) {
                const springDx = effectiveTargetX - state.x;
                const springDy = effectiveTargetY - state.y;
                const springDistance = Math.sqrt(springDx * springDx + springDy * springDy);

                let springFx = 0;
                let springFy = 0;

                if (springDistance > 0) {
                    const nx = springDx / springDistance;
                    const ny = springDy / springDistance;
                    // Force proportional to distance, ramped by the spawn smooth
                    // factor.
                    springFx = nx * springForce * springDistance * subDt * smoothFactor;
                    springFy = ny * springForce * springDistance * subDt * smoothFactor;
                }

                state.vx += springFx;
                state.vy += springFy;

                state.vx *= damping;
                state.vy *= damping;

                state.x += state.vx * subDt;
                state.y += state.vy * subDt;
            }

            // Defence in depth: if the integrator ever goes non-finite anyway,
            // self-heal to the target rather than leaving the petal stranded
            // forever at NaN.
            if (!Number.isFinite(state.x) || !Number.isFinite(state.y)) {
                state.x = effectiveTargetX;
                state.y = effectiveTargetY;
                state.vx = 0;
                state.vy = 0;
            }
        }

        petalX = state.x;
        petalY = state.y;
    }

    if (stats.wallCollide) {
        const resolved = deps.resolveWall(petalX, petalY, 20 * effectiveSize);
        if (resolved.collided) {
            petalX = resolved.x;
            petalY = resolved.y;
            // Persist the resolved position and kill the velocity into the wall,
            // or the spring drives the petal straight back inside next frame.
            // `peek`, not `acquire`: the fixedDirection and noPhysics modes have
            // no state and must not gain one here.
            const state = ring.peek(loadoutIndex, instanceIndex);
            if (state) {
                state.x = petalX;
                state.y = petalY;
                state.vx = 0;
                state.vy = 0;
            }
        }
    }

    out.x = petalX;
    out.y = petalY;
}
