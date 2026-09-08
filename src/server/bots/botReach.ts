/**
 * How far a bot can actually hit from, and how fast it actually moves.
 *
 * ---------------------------------------------------------------------------
 * Why this file exists
 * ---------------------------------------------------------------------------
 * A bot's whole combat controller is built on one number: the distance at which
 * its orbiting petals reach the mob it is fighting. `botManager` computed that
 * number three times over from hand-copied formulae —
 *
 *   getBotSpeedMod      a re-implementation of calculatePlayerModifiers().speed
 *   getBotRangeMod      a re-implementation of calculatePlayerModifiers().range
 *   computePetalReach   a re-implementation of the ring's orbit radius
 *
 * — and hand copies of a formula do not fail when they drift. They go on
 * returning a number. A bot whose reach estimate is 30px optimistic parks just
 * outside where its petals can connect and orbits a mob it never damages, for as
 * long as the server runs. No typecheck, self-test, harness or cutover oracle
 * can see that, because each of them exercises one side of the copy and the two
 * sides agree on nothing but their type.
 *
 * Since the petal cutover the orbit radius has a single owner: the ECS ring
 * (`ecs/systems/petalRing.ts`). `computeRingGeometry` derives `baseRadius` and
 * `defendOnlyBaseRadius`, and `petalOrbitTarget` turns those into an instance's
 * orbit point. So this module calls THAT, rather than reproducing it, and the
 * modifier aggregation comes from the real `calculatePlayerModifiers` (moved to
 * server/shared/playerModifiers.ts precisely so it could be called from here
 * without booting a server).
 *
 * The reach expression below is the only thing that is still spelled out rather
 * than called, because `petalOrbitTarget` answers "where is instance k right
 * now" and the controller needs "how far out can ANY instance ever get". They
 * are pinned to each other by `ecs/bench/bot_cutover_check.ts`, which sweeps the
 * orbit phase through a full revolution against the real ring and fails if the
 * farthest real orbit point ever escapes this bound — or if the bound drifts
 * loose enough to be useless.
 *
 * ---------------------------------------------------------------------------
 * Loadable without a server
 * ---------------------------------------------------------------------------
 * Every import here is side-effect free. That is a requirement, not a
 * coincidence: the gate that pins this to the ring has to be able to require it,
 * and `server/playerManager.ts` — the obvious home for this code — transitively
 * imports petal_actions.ts and playerState.ts, both of which bind port 3000 and
 * open the account database at module scope.
 */

import { PLAYER_SIZE } from '../../constants';
import { ServerPlayer } from '../../player';
import { getPetalStats } from '../../petals';
import { computeRingGeometry, PRIMARY_LOADOUT_SLOTS } from '../../ecs/systems/petalRing';
import { calculatePlayerModifiers } from '../shared/playerModifiers';

/**
 * The aggregate multiplicative speed modifier from a flower's equipped petals.
 *
 * The real one, not a mirror. Bots divide their close-range movement by this so
 * a powder build runs the standoff bands at the pace the controller was tuned
 * for instead of overshooting the ring every tick.
 */
export function botSpeedModifier(bot: ServerPlayer): number {
    return calculatePlayerModifiers(bot).speed ?? 1.0;
}

/**
 * The aggregate orbit-range modifier (third eye and friends).
 *
 * Exported mostly so the gate can assert the bot side and the ring side are
 * reading the same value; `botPetalReach` folds it in itself.
 */
export function botRangeModifier(bot: ServerPlayer): number {
    return calculatePlayerModifiers(bot).range ?? 1.0;
}

/**
 * The farthest a petal edge can be from the flower's centre this tick.
 *
 * Mirrors `petalOrbitTarget`'s radius term for the WORST case over the ring:
 *
 *   radius = (defendOnly ? defendOnlyBaseRadius : baseRadius)
 *            * ((stats.range ?? 1) * rangeModifier)
 *
 * plus, for a clumped petal, the `effectiveSize * 20 * 0.5` cluster offset that
 * `petalOrbitTarget` adds — worst case, since the offset points outward for one
 * instance of the clump on every revolution. The petal's own half-size is added
 * last, because reach is measured to the petal's far EDGE.
 *
 * Three things here are not in the formula it replaces, and each is a case where
 * the old copy was wrong rather than merely different:
 *
 *   - `defendOnly` petals (rose) do not extend while attacking, so their radius
 *     is capped at the neutral orbit. The old copy applied the attack extension
 *     to them and over-estimated a rose-heavy build's reach by ~60px.
 *   - clumped petals (sand) sit up to a cluster-offset further out than their
 *     slot centre; the old copy ignored it and UNDER-estimated.
 *   - the old copy took `max(radius multiplier)` and `max(half size)` over
 *     DIFFERENT petals and added them, which cannot describe any single petal.
 *     The max is now taken over each petal's own total.
 *
 * Only the primary ten slots are considered, matching `layoutPetalRing`: slots
 * 10+ are storage and never enter the ring. The old copy walked the whole array.
 */
export function botPetalReach(
    bot: ServerPlayer,
    petalExtension: number,
    safetyBuffer: number,
): number {
    const modifiers = calculatePlayerModifiers(bot);

    // The ring's own geometry, from the ring's own code. Position and phase are
    // irrelevant to a radius, so they are zero; `slotCount` only sets the
    // angular step, which is likewise irrelevant here.
    const geom = computeRingGeometry({
        playerX: 0,
        playerY: 0,
        orbitPhase: 0,
        slotCount: 0,
        petalExtension,
        sizeMultiplier: bot.sizeMultiplier ?? 1.0,
        playerSize: PLAYER_SIZE,
        rangeModifier: modifiers.range ?? 1.0,
        rotationSpeedModifier: modifiers.rotationSpeed ?? 1.0,
        attractionRadius: modifiers.petalAttractionRadius ?? 0,
        deltaTime: 0,
        now: 0,
    });

    let farthest = 0;
    const loadout = bot.loadout;
    if (loadout) {
        const slots = Math.min(loadout.length, PRIMARY_LOADOUT_SLOTS);
        for (let i = 0; i < slots; i++) {
            const item = loadout[i];
            if (!item || item.type !== 'petal' || !item.petalType || !item.rarity) continue;
            const stats = getPetalStats(item.petalType, item.rarity);
            if (!stats) continue;

            const effectiveSize = (item as any).customSize ?? stats.size ?? 1.0;
            const base = stats.defendOnly ? geom.defendOnlyBaseRadius : geom.baseRadius;
            let radius = base * ((stats.range ?? 1.0) * geom.rangeModifier);

            const count = stats.count || 1;
            if (stats.clumped && count > 1) {
                radius += effectiveSize * 20 * 0.5;
            }

            const edge = radius + (20 * effectiveSize) / 2;
            if (edge > farthest) farthest = edge;
        }
    }

    return farthest + safetyBuffer;
}
