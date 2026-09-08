/**
 * Self-test for the petal ring.
 *
 * The INTEGRATOR is not tested here — `ecs/bench/petal_cutover_check.ts` proves
 * it bit-identical against a verbatim copy of the code it replaced, which is a
 * far stronger statement than any hand-written expectation could be, and it runs
 * in the harness gate because it needs the server tree for `openPetalRing`.
 *
 * What IS tested here is everything the ring DECIDES rather than computes, and
 * every one of these is a trap the cutover check cannot see because the oracle
 * would make the same mistake:
 *
 *   - the orbit-slot assignment, which sets the ring divisor every angle
 *     depends on;
 *   - which loadout slots are in orbit at all;
 *   - the instance-key packing, where two (slot, instance) pairs colliding on
 *     one key makes one petal quietly drive another's momentum;
 *   - the drop-a-slot sweep, which in its old string-prefix form was one
 *     character away from `"..._1_"` also matching slot 10;
 *   - which position modes acquire per-instance state and which must not.
 */

import {
    advanceOrbitPhase,
    computeRingGeometry,
    layoutPetalRing,
    petalInstanceKey,
    petalOrbitTarget,
    PetalOrbitTarget,
    PetalRingDeps,
    PetalRingState,
    PetalRingStats,
    RingInstance,
    stepPetalKinematics,
    PetalKinematicsResult,
} from './petalRing';

interface Slot {
    type: string;
    petalType: string;
    rarity: string;
}

function slot(petalType: string): Slot {
    return { type: 'petal', petalType, rarity: 'common' };
}

const TABLE: Record<string, PetalRingStats> = {
    one: { count: 1, size: 1 },
    three: { count: 3, size: 1 },
    clump: { count: 4, size: 1, clumped: true },
    snap: { count: 1, size: 1, noPhysics: true },
    pinned: { count: 1, size: 1, fixedDirection: 0 },
    zeroRange: { count: 1, size: 1, range: 0 },
    // Refused: past the key stride, so two slots' instances would collide.
    huge: { count: 99999, size: 1 },
    // Refused: not finite.
    endless: { count: Infinity, size: 1 },
    // Refused: below one.
    negative: { count: -5, size: 1 },
    // NOT refused: `count || 1` turns both of these into a single instance, and
    // that fallback is legacy behaviour rather than an accident.
    nan: { count: NaN, size: 1 },
    zero: { count: 0, size: 1 },
};

function statsOf(s: Slot): PetalRingStats | null {
    return TABLE[s.petalType] ?? null;
}

/** Deps that see an empty world: no mobs, no walls, never homing. */
const INERT_DEPS: PetalRingDeps = {
    findAttractionTarget: () => null,
    isEnemyPresent: () => false,
    resolveWall: (x, y) => ({ collided: false, x, y }),
    isHoming: () => false,
};

export function runPetalRingSelfTest(): string[] {
    const failures: string[] = [];

    const check = (name: string, condition: boolean, detail?: string) => {
        if (!condition) failures.push(detail ? `${name}: ${detail}` : name);
    };
    const checkEqual = (name: string, actual: unknown, expected: unknown) => {
        if (actual !== expected) failures.push(`${name}: expected ${String(expected)}, got ${String(actual)}`);
    };

    const out: Array<RingInstance<Slot>> = [];

    // -- slot assignment: spread vs clumped -----------------------------------
    {
        // one(1) + three(3) + clump(4) + one(1): the clump takes a SINGLE slot
        // for all four of its instances, so the divisor is 1 + 3 + 1 + 1 = 6
        // while the instance count is 1 + 3 + 4 + 1 = 9.
        const loadout = [slot('one'), slot('three'), slot('clump'), slot('one')];
        const slotCount = layoutPetalRing(loadout, statsOf, out);

        checkEqual('spread+clumped divisor', slotCount, 6);
        checkEqual('spread+clumped instance count', out.length, 9);

        const slots = out.map(i => i.slotIndex).join(',');
        checkEqual('slot indices', slots, '0,1,2,3,4,4,4,4,5');

        const inst = out.map(i => `${i.loadoutIndex}:${i.instanceIndex}`).join(' ');
        checkEqual('instance order is loadout-then-count',
            inst, '0:0 1:0 1:1 1:2 2:0 2:1 2:2 2:3 3:0');
    }

    // -- only the primary loadout orbits ---------------------------------------
    {
        // 12 slots; everything from index 10 on is storage and must contribute
        // neither instances nor ring slots. Getting this wrong shifts every
        // angle in the ring the moment a player's storage is non-empty.
        const loadout: Array<Slot | null> = [];
        for (let i = 0; i < 12; i++) loadout.push(slot('one'));
        const slotCount = layoutPetalRing(loadout, statsOf, out);

        checkEqual('secondary loadout excluded from divisor', slotCount, 10);
        checkEqual('secondary loadout spawns no instances', out.length, 10);
    }

    // -- empty and unknown slots ------------------------------------------------
    {
        const loadout: Array<Slot | null> = [
            null,
            slot('one'),
            { type: 'egg', petalType: 'one', rarity: 'common' },
            { type: 'petal', petalType: 'not_in_table', rarity: 'common' },
            slot('one'),
        ];
        const slotCount = layoutPetalRing(loadout, statsOf, out);
        checkEqual('holes and non-petals consume no ring slot', slotCount, 2);
        checkEqual('holes and non-petals spawn no instances', out.length, 2);
    }

    // -- refused counts ---------------------------------------------------------
    {
        // An infinite count would spin the inner loop forever; a count past the
        // key stride would make two slots' instances collide on one key; a
        // negative one is nonsense. All three must be dropped WITHOUT consuming a
        // ring slot — consuming one would shift every later petal's angle.
        const loadout = [slot('one'), slot('endless'), slot('huge'), slot('negative'), slot('one')];
        const slotCount = layoutPetalRing(loadout, statsOf, out);
        checkEqual('invalid counts consume no ring slot', slotCount, 2);
        checkEqual('invalid counts spawn no instances', out.length, 2);
    }

    // -- falsy counts fall back to one -----------------------------------------
    {
        // `count || 1` predates the port and NaN/0 reach it as 1 rather than as
        // an error. Pinned down because the guard above looks like it should
        // reject them, and "fixing" that would silently delete a petal.
        const loadout = [slot('nan'), slot('zero')];
        const slotCount = layoutPetalRing(loadout, statsOf, out);
        checkEqual('falsy count still takes a ring slot', slotCount, 2);
        checkEqual('falsy count yields one instance each', out.length, 2);
    }

    // -- instance keys are injective -------------------------------------------
    {
        // The whole point of packing (slot, instance) into one integer is that it
        // stays a bijection over the reachable range. A collision here is
        // invisible in position output — the two instances simply share momentum.
        const seen = new Set<number>();
        let collisions = 0;
        for (let s = 0; s < 10; s++) {
            for (let i = 0; i < 64; i++) {
                const key = petalInstanceKey(s, i);
                if (seen.has(key)) collisions++;
                seen.add(key);
            }
        }
        checkEqual('instance keys never collide across slots', collisions, 0);
    }

    // -- dropSlot hits exactly one slot ----------------------------------------
    {
        // The string-keyed original swept by the prefix `"<id>_1_"`, which is one
        // formatting change away from also matching slot 10. Assert the integer
        // form draws the boundary in the right place, from both sides.
        const ring = new PetalRingState();
        ring.acquire(0, 0, 0, 0, 0);
        ring.acquire(1, 0, 0, 0, 0);
        ring.acquire(1, 3, 0, 0, 0);
        ring.acquire(10, 0, 0, 0, 0);
        ring.acquire(2, 0, 0, 0, 0);
        checkEqual('ring size before drop', ring.size, 5);

        ring.dropSlot(1);
        checkEqual('dropSlot removed both instances of slot 1', ring.size, 3);
        check('slot 1 instance 0 gone', ring.peek(1, 0) === undefined);
        check('slot 1 instance 3 gone', ring.peek(1, 3) === undefined);
        check('slot 10 survives dropSlot(1)', ring.peek(10, 0) !== undefined);
        check('slot 0 survives dropSlot(1)', ring.peek(0, 0) !== undefined);
        check('slot 2 survives dropSlot(1)', ring.peek(2, 0) !== undefined);

        ring.dropInstance(10, 0);
        checkEqual('dropInstance removes exactly one', ring.size, 2);
    }

    // -- a fresh instance starts on the flower ---------------------------------
    {
        // Reload has to fly the petal back out from the centre; resuming from
        // wherever it broke is the bug this replaces.
        const ring = new PetalRingState();
        const state = ring.acquire(0, 0, 123, -456, 1000);
        checkEqual('fresh instance starts at the flower (x)', state.x, 123);
        checkEqual('fresh instance starts at the flower (y)', state.y, -456);
        checkEqual('fresh instance has no momentum', state.vx, 0);
        check('fresh instance opens a spawn glide', state.glideUntil > 1000);

        state.x = 999;
        const again = ring.acquire(0, 0, 0, 0, 5000);
        checkEqual('acquire is idempotent', again.x, 999);
    }

    // -- the orbit phase is an integral ----------------------------------------
    {
        // Changing the modifier must BEND the rate, never remap the angle: after
        // three ticks at rate 1 and one at rate 2, the phase is 3dt + 2dt, not
        // 4dt * 2.
        const dt = 1 / 30;
        let phase = 0;
        for (let i = 0; i < 3; i++) phase = advanceOrbitPhase(phase, 1, dt);
        phase = advanceOrbitPhase(phase, 2, dt);
        checkEqual('orbit phase integrates rather than remaps', phase, dt * 3 + dt * 2);
    }

    // -- position modes and state acquisition ----------------------------------
    {
        // Only the spring branch may own per-instance state. If a snap or pinned
        // petal acquired one, the wall write-back would start persisting into
        // state nothing reads, and a reload would resume from it.
        const geom = computeRingGeometry({
            playerX: 500,
            playerY: -300,
            orbitPhase: 1.234,
            slotCount: 4,
            petalExtension: 1,
            sizeMultiplier: 1,
            playerSize: 40,
            rangeModifier: 1,
            rotationSpeedModifier: 1,
            attractionRadius: 0,
            deltaTime: 1 / 30,
            now: 10_000,
        });
        const result: PetalKinematicsResult = { x: 0, y: 0, angle: 0, facingAngle: 0, homing: false };
        const target: PetalOrbitTarget = { x: 0, y: 0, angle: 0, facingAngle: 0, range: 0 };

        const modes: Array<{ name: string; stats: PetalRingStats; wantsState: boolean }> = [
            { name: 'spring', stats: TABLE.one, wantsState: true },
            { name: 'noPhysics', stats: TABLE.snap, wantsState: false },
            { name: 'fixedDirection', stats: TABLE.pinned, wantsState: false },
            { name: 'zeroRange', stats: TABLE.zeroRange, wantsState: false },
        ];

        for (let m = 0; m < modes.length; m++) {
            const ring = new PetalRingState();
            const mode = modes[m];
            stepPetalKinematics(ring, geom, mode.stats, m, 0, m, 1, INERT_DEPS, result);
            checkEqual(`${mode.name} state acquisition`, ring.size, mode.wantsState ? 1 : 0);

            if (mode.name === 'fixedDirection') {
                // Pinned petals sit ON the flower, not on the ring.
                checkEqual('fixedDirection sits on the flower (x)', result.x, geom.playerX);
                checkEqual('fixedDirection sits on the flower (y)', result.y, geom.playerY);
            }
            if (mode.name === 'noPhysics') {
                // Snapped petals land exactly on the orbit point, with no lag.
                petalOrbitTarget(geom, mode.stats, m, 0, 1, target);
                checkEqual('noPhysics snaps to the orbit point (x)', result.x, target.x);
                checkEqual('noPhysics snaps to the orbit point (y)', result.y, target.y);
            }
            if (mode.name === 'zeroRange') {
                // range 0 collapses the orbit onto the flower through the
                // no-physics door rather than the fixedDirection one.
                checkEqual('zeroRange collapses onto the flower (x)', result.x, geom.playerX);
                checkEqual('zeroRange collapses onto the flower (y)', result.y, geom.playerY);
            }
        }
    }

    // -- clumped instances cluster, spread instances do not ---------------------
    {
        const geom = computeRingGeometry({
            playerX: 0,
            playerY: 0,
            orbitPhase: 0,
            slotCount: 1,
            petalExtension: 1,
            sizeMultiplier: 1,
            playerSize: 40,
            rangeModifier: 1,
            rotationSpeedModifier: 1,
            attractionRadius: 0,
            deltaTime: 1 / 30,
            now: 0,
        });
        const a: PetalOrbitTarget = { x: 0, y: 0, angle: 0, facingAngle: 0, range: 0 };
        const b: PetalOrbitTarget = { x: 0, y: 0, angle: 0, facingAngle: 0, range: 0 };

        // Two instances of a CLUMPED petal share slot 0 but must not coincide.
        petalOrbitTarget(geom, TABLE.clump, 0, 0, 1, a);
        petalOrbitTarget(geom, TABLE.clump, 0, 1, 1, b);
        check('clumped instances are offset from each other', a.x !== b.x || a.y !== b.y);
        check('clumped instances share a bearing', a.angle === b.angle);
        // ...but each FACES its own way out of the clump, a quarter turn apart
        // for a clump of four. A projectile petal fires down this, which is how
        // four peas leave in four directions instead of stacking on one.
        let delta = b.facingAngle - a.facingAngle;
        while (delta > Math.PI) delta -= Math.PI * 2;
        while (delta < -Math.PI) delta += Math.PI * 2;
        check('clumped instances face a quarter turn apart',
            Math.abs(Math.abs(delta) - Math.PI / 2) < 1e-9);

        // A non-clumped petal ignores instanceIndex entirely at a given slot.
        petalOrbitTarget(geom, TABLE.three, 0, 0, 1, a);
        petalOrbitTarget(geom, TABLE.three, 0, 1, 1, b);
        check('non-clumped instances at one slot coincide', a.x === b.x && a.y === b.y);
        check('an unclumped instance faces its own bearing', a.facingAngle === a.angle);
    }

    // -- defendOnly never extends on attack ------------------------------------
    {
        // Attacking (extension > 1) must leave a defend-only petal at its neutral
        // radius while an ordinary petal flies out; defending (< 1) must pull
        // BOTH in. That asymmetry is the whole point of the second base radius.
        const make = (petalExtension: number) => computeRingGeometry({
            playerX: 0,
            playerY: 0,
            orbitPhase: 0,
            slotCount: 1,
            petalExtension,
            sizeMultiplier: 1,
            playerSize: 40,
            rangeModifier: 1,
            rotationSpeedModifier: 1,
            attractionRadius: 0,
            deltaTime: 1 / 30,
            now: 0,
        });
        const neutral = make(1);
        const attack = make(1.8);
        const defend = make(0.5);

        checkEqual('attack extends the ordinary radius', attack.baseRadius > neutral.baseRadius, true);
        checkEqual('attack does not extend defendOnly',
            attack.defendOnlyBaseRadius, neutral.defendOnlyBaseRadius);
        checkEqual('defend pulls the ordinary radius in', defend.baseRadius < neutral.baseRadius, true);
        checkEqual('defend also pulls defendOnly in',
            defend.defendOnlyBaseRadius, defend.baseRadius);
    }

    return failures;
}
