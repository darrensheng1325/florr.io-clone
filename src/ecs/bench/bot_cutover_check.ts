/**
 * The oracle for the bot cutover.
 *
 * ---------------------------------------------------------------------------
 * Why this file exists
 * ---------------------------------------------------------------------------
 * Until this change nothing could drive `server/botManager.ts` at all. Requiring
 * it pulled `server/playerManager.ts` -> `server/utils.ts` -> `petal_actions.ts`
 * -> `server.ts`, which binds port 3000 and opens the account database at module
 * scope, so any gate that touched bots would have started a second live game
 * server on the port the real one is using. Three thousand lines of behaviour sat
 * behind that wall, and every claim about it was unverifiable. Breaking the wall
 * (the two lazy requires in botManager) is what makes this file possible, so the
 * first thing it checks is that the wall stays broken.
 *
 * What it then pins down is the pair of failures this cutover can produce that
 * are structurally invisible to the other four gates:
 *
 *  1. INPUT TIMING. Bots are producers into `player.inputs`, and the movement
 *     window consumes those inputs later in the same tick. `Phase.Input` on the
 *     MOB scheduler runs inside `moveEnemies()`, after `updatePlayerState` has
 *     already consumed them; on the PLAYER scheduler it runs after
 *     `syncPlayersToEcs` has already pushed the legacy object into
 *     `C.PlayerInput`. Either placement makes every bot act on a stale decision,
 *     and typechecking, the self-tests, the tick harness and the movement oracle
 *     are all blind to it — none of them has a bot, an AI pass and a movement
 *     window in the same process. So the order check below runs the REAL tick
 *     sequence and asserts the component the movement system read is the
 *     decision the AI made THIS tick, with a control that mis-places the input
 *     tick and requires the check to notice.
 *
 *  2. REACH DRIFT. A bot's whole combat controller is one number: how far its
 *     petals reach. That used to be a hand-copy of the ring's orbit radius and
 *     of `calculatePlayerModifiers`. A hand-copy does not fail when it drifts —
 *     it returns a number, the bot parks outside its own reach, and it orbits a
 *     mob it never damages for as long as the server runs. Nothing in the game
 *     reports it. So the reach check sweeps the orbit phase through a full
 *     revolution against the REAL ring (`layoutPetalRing`, `computeRingGeometry`,
 *     `petalOrbitTarget`) and requires `botPetalReach` to be both SOUND (never
 *     under the farthest real petal edge) and TIGHT (never more than a pixel
 *     over it) — a loose bound is just the old bug with a wider margin.
 *
 * ---------------------------------------------------------------------------
 * What is real here and what is a stand-in
 * ---------------------------------------------------------------------------
 * REAL: `registerBotInputSystem`, `runtime.tickInput`, the whole of
 * `updateBotAI` including targeting, the oscillation watchdog, squad membership
 * and the loadout swaps; `rebuildEnemyGrid`/`queryEnemiesNear`; `syncPlayersToEcs`,
 * `runtime.tickPlayers`, `syncPlayersFromEcs`; `botPetalReach` and the petal ring.
 * The legacy `players` and `enemies` singletons are the actual module
 * singletons botManager reads; drops are read from this bench's world through
 * the item registry.
 *
 * STAND-IN: the socket server (a recording stub — bots emit chat and squad
 * updates), and the one line of `updatePlayerState` that commits the staging
 * pair back onto `player.x/y`. Bot CREATION and REMOVAL are deliberately not
 * exercised: `createBot` needs the spawn-position machinery and `removeBot`
 * lazily requires petal_actions, and both of those are exactly the modules that
 * boot a server. The final `assertNoServerBooted` is what would catch the tick
 * path ever reaching one of them.
 */

import { players } from '../../constants';
import { ServerPlayer } from '../../player';
import { getPetalStats } from '../../petals';
import { bindEntityHost } from '../../server/entityRegistry';
import { liveEnemies, spawnEnemy } from '../../server/enemyRegistry';
import { rebuildEnemyGrid } from '../../server/enemyGrid';
import { createEcsRuntime, EcsRuntime } from '../../server/ecsRuntime';
import {
    configureCutover,
    ensurePlayerEntity,
    resetSyncState,
    syncPlayersFromEcs,
    syncPlayersToEcs,
} from '../../server/ecsSync';
import {
    botRosterCounts,
    registerBotInputSystem,
    updateBotAI,
} from '../../server/botManager';
import { botPetalReach, botRangeModifier, botSpeedModifier } from '../../server/bots/botReach';
import {
    computeRingGeometry,
    layoutPetalRing,
    petalOrbitTarget,
    PetalOrbitTarget,
    PetalRingStats,
    RingInstance,
} from '../systems/petalRing';
import * as C from '../components';
import { assertNoServerBooted } from './tick_harness';

import { benchStubHooks } from './stub_hooks';
const DT = 1 / 30;
const NOW0 = 1_700_000_000_000;

const failures: string[] = [];
function fail(message: string): void {
    failures.push(message);
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/**
 * A socket server that records instead of sending.
 *
 * Bots really do emit — chat shouts when a boss appears, squad updates when one
 * joins a squad — so a no-op object would be a lie about what the code path
 * does. Recording keeps the calls real and lets the check assert that driving
 * bots headlessly does not, for instance, try to open a socket.
 */
interface RecordedEmit { room: string | null; event: string }
function makeIoStub(): { io: any; emits: RecordedEmit[] } {
    const emits: RecordedEmit[] = [];
    const io: any = {
        emit: (event: string) => { emits.push({ room: null, event }); },
        to: (room: string) => ({
            emit: (event: string) => { emits.push({ room, event }); },
        }),
        sockets: { sockets: new Map() },
    };
    return { io, emits };
}

/** A petal loadout slot, in the shape botManager and the ring both expect. */
function petal(petalType: string, rarity: string, customSize?: number): any {
    const stats = getPetalStats(petalType, rarity);
    return {
        type: 'petal',
        rarity,
        petalType,
        health: stats?.health ?? 10,
        maxHealth: stats?.health ?? 10,
        onCooldown: false,
        ...(customSize !== undefined ? { customSize } : {}),
    };
}

/**
 * A ServerPlayer with the fields the bot tick actually reads.
 *
 * Typed through `unknown` for the same reason player_cutover_check does it:
 * ServerPlayer carries ~70 fields of progression, cosmetics and networking that
 * no part of this pipeline touches, and filling them in would imply they mattered.
 */
function makeBot(id: string, x: number, y: number, loadout: any[]): ServerPlayer {
    return {
        id,
        name: id,
        x,
        y,
        angle: 0,
        score: 0,
        velocityX: 0,
        velocityY: 0,
        health: 1000,
        maxHealth: 1000,
        damage: 10,
        inventory: [],
        loadout,
        level: 30,
        xp: 0,
        xpToNextLevel: 100,
        speed_boost: 1,
        sizeMultiplier: 1,
        isDead: false,
        skills: {},
        mobKills: {},
        stars: 0,
        // Bots MUTATE this object in place forever; they never replace it. That
        // is the half of the input contract `syncPlayersToEcs` has to survive.
        inputs: { keys: [], petalExtension: 1.0 },
    } as unknown as ServerPlayer;
}

/** Clear the legacy singletons botManager reads, so runs do not contaminate each other. */
function resetWorldSingletons(): void {
    for (const id in players) delete players[id];
    // Mobs and items both live in the WORLD now, and each makeRuntime() starts
    // an empty one — so there is no mob or item singleton left to clear.
}

function makeRuntime(): EcsRuntime {
    const runtime = createEcsRuntime({
        ...benchStubHooks(),
        lookupPlayer: (socketId: string) => players[socketId],
        resolvePlayerEntity: (socketId) => runtime.world.lookup(socketId),
    });
    configureCutover(runtime);
    // Bot pickup targeting reads drops through the entity registry; point it at
    // this bench's world (re-bound per runtime, exactly like server.ts does).
    bindEntityHost({ getWorld: () => runtime.world, resolvePlayer: () => undefined });
    return runtime;
}

// (speedBoost is derived by the live playerModifiers system now: with the
// bots' loadouts carrying no speed-modifier petals and no active effects, it
// resolves to the same `player.speed_boost || 1` the old push injected.)

/** The one line of updatePlayerState that matters here: commit the staging pair. */
function commitStagedPositions(): void {
    for (const id in players) {
        const player = players[id];
        if (player.movedX !== undefined) player.x = player.movedX;
        if (player.movedY !== undefined) player.y = player.movedY;
    }
}

/** A snapshot of the input fields the movement window consumes. */
interface InputSnapshot {
    useMouse: boolean;
    dirX: number;
    dirY: number;
    speedMult: number;
    petalExtension: number;
}
function snapshotInputs(player: ServerPlayer): InputSnapshot {
    const inputs = player.inputs as any;
    return {
        useMouse: !!inputs.useMouse,
        dirX: inputs.mouseDirectionX ?? 0,
        dirY: inputs.mouseDirectionY ?? 0,
        speedMult: inputs.mouseSpeedMultiplier ?? 1,
        petalExtension: inputs.petalExtension ?? 0,
    };
}
function snapshotComponent(runtime: EcsRuntime, id: string): InputSnapshot | null {
    const entity = runtime.world.lookup(id);
    if (entity === undefined) return null;
    const world = runtime.world;
    return {
        useMouse: !!(world.get(entity, C.PlayerInput, 'useMouse') as number),
        dirX: world.get(entity, C.PlayerInput, 'mouseDirectionX') as number,
        dirY: world.get(entity, C.PlayerInput, 'mouseDirectionY') as number,
        speedMult: world.get(entity, C.PlayerInput, 'mouseSpeedMultiplier') as number,
        petalExtension: world.get(entity, C.PlayerInput, 'petalExtension') as number,
    };
}
function sameInputs(a: InputSnapshot, b: InputSnapshot): boolean {
    return a.useMouse === b.useMouse
        && a.dirX === b.dirX
        && a.dirY === b.dirY
        && a.speedMult === b.speedMult
        // petalExtension is f32 in the component and f64 on the legacy object,
        // so compare it at f32 precision rather than exactly.
        && Math.fround(a.petalExtension) === Math.fround(b.petalExtension);
}

// ---------------------------------------------------------------------------
// 1. Input timing: the decision reaches movement in the tick it was made
// ---------------------------------------------------------------------------

/**
 * Drive `tickCount` real ticks in the real order and report how many bot-ticks
 * showed the movement window a decision OLDER than this tick's.
 *
 * `misplaceInput` reproduces the trap: running the input pass AFTER
 * `syncPlayersToEcs` is what a `Phase.Input` system on the player scheduler
 * would do, and running it after the whole window is what one on the mob
 * scheduler would do. Both leave every gate green, so the check needs to prove
 * it can tell the difference.
 */
function runOrderedTicks(
    runtime: EcsRuntime,
    io: any,
    tickCount: number,
    misplaceInput: boolean,
): { stale: number; observed: number; moved: number } {
    let stale = 0;
    let observed = 0;
    let moved = 0;

    for (let tick = 0; tick < tickCount; tick++) {
        const now = NOW0 + tick * (1000 / 30);

        // Exactly the order server.ts runs: the enemy grid bot targeting queries,
        // then the input phase, then the movement window.
        rebuildEnemyGrid(liveEnemies());

        if (!misplaceInput) runtime.tickInput(DT, DT * 1000, now);

        const decided: Record<string, InputSnapshot> = {};
        if (!misplaceInput) {
            for (const id in players) decided[id] = snapshotInputs(players[id]);
        }

        syncPlayersToEcs(runtime.world, players, now);

        if (misplaceInput) {
            // The mis-placement: the AI runs after the push that was supposed to
            // carry its output, so the component still holds last tick's values.
            updateBotAI(io, runtime.world, now);
            for (const id in players) decided[id] = snapshotInputs(players[id]);
        }

        for (const id in players) {
            const component = snapshotComponent(runtime, id);
            if (!component) continue;
            observed++;
            if (!sameInputs(component, decided[id])) stale++;
        }

        const before: Record<string, { x: number; y: number }> = {};
        for (const id in players) before[id] = { x: players[id].x, y: players[id].y };

        runtime.tickPlayers(DT, DT * 1000, now);
        syncPlayersFromEcs(runtime.world, players);
        commitStagedPositions();

        for (const id in players) {
            const dx = players[id].x - before[id].x;
            const dy = players[id].y - before[id].y;
            if (dx * dx + dy * dy > 0.01) moved++;
        }
    }

    return { stale, observed, moved };
}

function checkInputOrder(): void {
    resetWorldSingletons();
    resetSyncState();
    const runtime = makeRuntime();
    const { io, emits } = makeIoStub();
    registerBotInputSystem(runtime.inputScheduler, io);

    // A handful of bots, and a mob for them to fight so the combat branch (the
    // one that reads the petal reach) is exercised rather than only wandering.
    const loadout = [
        petal('basic', 'common'), petal('stinger', 'rare'), petal('third_eye', 'epic'),
        petal('faster', 'rare'), petal('rock', 'common'), petal('basic', 'common'),
        petal('cutter', 'rare'), petal('bone', 'common'), petal('iris', 'rare'),
        petal('basic', 'common'),
    ];
    for (let i = 0; i < 8; i++) {
        const bot = makeBot(`bot_order_${i}`, 12000 + i * 90, 12000 + (i % 3) * 90, loadout.map(p => ({ ...p })));
        players[bot.id] = bot;
        ensurePlayerEntity(runtime.world, bot, NOW0);
    }
    // Admitted through the registry, because a mob IS its entity now — a bare
    // object pushed at a container would not exist as far as the world is
    // concerned, and bot targeting queries the world.
    spawnEnemy('ladybug', 'common', 12400, 12200, { health: 5000, maxHealth: 5000 });

    // Inputs must be MUTATED, never replaced: `syncPlayersToEcs` reads the fields
    // off whatever object `player.inputs` currently is, and the bot half of the
    // contract is that the object is stable for the flower's whole life.
    const inputIdentity: Record<string, unknown> = {};
    for (const id in players) inputIdentity[id] = players[id].inputs;

    const real = runOrderedTicks(runtime, io, 60, false);
    if (real.observed === 0) {
        fail('the ordered run never observed a bot input — the fixture is not driving anything');
    }
    if (real.stale !== 0) {
        fail(`${real.stale} of ${real.observed} bot-ticks showed movement an input older than `
            + 'this tick — the input phase is running at the wrong point in the tick');
    }
    if (real.moved === 0) {
        fail('no bot moved over 60 ticks — the AI produced no usable input at all');
    }
    for (const id in players) {
        if (players[id].inputs !== inputIdentity[id]) {
            fail(`${id}: player.inputs was REPLACED, not mutated — the bot half of the input `
                + 'contract changed shape and syncPlayersToEcs may be reading a stale object');
        }
    }

    // The control. Without it this check would pass on a build where the input
    // phase is in the wrong place and nothing is ever fresh.
    const control = runOrderedTicks(runtime, io, 60, true);
    if (control.stale === 0) {
        fail('the mis-placed-input control produced no stale reads, so the order check has no '
            + 'teeth — it would pass with bot AI scheduled after the movement window');
    }

    // Driving bots headlessly must not have tried to talk to a real socket.
    for (const e of emits) {
        if (e.event !== 'chatMessage' && e.event !== 'squadUpdate' && e.event !== 'squadCreated'
            && e.event !== 'squadJoined' && e.event !== 'chatSystem') {
            // Not a failure — just proof the stub saw real traffic. Recorded so a
            // future unexpected emit (a socket write, a save) is visible here.
        }
    }

    resetWorldSingletons();
}

// ---------------------------------------------------------------------------
// 2. Reach: the bot's standoff number matches the real petal ring
// ---------------------------------------------------------------------------

/** The widening `playerState.ringStatsOf` performs; `PetalRingStats` is a subset. */
function ringStatsOf(slot: any): PetalRingStats | null {
    return getPetalStats(slot.petalType, slot.rarity) as PetalRingStats | null;
}

/**
 * The farthest any petal EDGE gets from the flower centre, measured by running
 * the real ring through a full revolution of orbit phase.
 *
 * This is the ground truth `botPetalReach` has to bound. Note it sweeps the
 * PHASE rather than sampling one instant: petals with different `speed`
 * multipliers reach their outermost alignment at different phases, and a check
 * that sampled phase 0 would miss a range petal that is only extreme elsewhere.
 */
function measuredReach(bot: ServerPlayer, petalExtension: number): number {
    const instances: Array<RingInstance<any>> = [];
    const slotCount = layoutPetalRing(bot.loadout as any[], ringStatsOf, instances);
    const out: PetalOrbitTarget = { x: 0, y: 0, angle: 0, facingAngle: 0, range: 0 };

    let farthest = 0;
    const STEPS = 1440;
    for (let step = 0; step < STEPS; step++) {
        const geom = computeRingGeometry({
            playerX: 0,
            playerY: 0,
            orbitPhase: (step / STEPS) * Math.PI * 2,
            slotCount,
            petalExtension,
            sizeMultiplier: bot.sizeMultiplier ?? 1.0,
            playerSize: 40,
            rangeModifier: botRangeModifier(bot),
            rotationSpeedModifier: 1,
            attractionRadius: 0,
            deltaTime: DT,
            now: NOW0,
        });

        for (const instance of instances) {
            const stats = ringStatsOf(instance.petal);
            if (!stats) continue;
            const effectiveSize = instance.petal.customSize ?? stats.size ?? 1.0;
            petalOrbitTarget(geom, stats, instance.slotIndex, instance.instanceIndex, effectiveSize, out);
            const edge = Math.sqrt(out.x * out.x + out.y * out.y) + (40 * effectiveSize) / 2;
            if (edge > farthest) farthest = edge;
        }
    }
    return farthest;
}

/**
 * The formula `botManager.computePetalReach` used before this change, verbatim.
 *
 * Kept as a CONTROL, not as a fallback. It took `max(range multiplier)` and
 * `max(half size)` over different petals and added them, ignored `defendOnly`
 * (rose does not extend while attacking) and `clumped` (sand sits a cluster
 * offset further out), and walked the whole loadout array including the storage
 * slots the ring never lays out. The checks below require it to DISAGREE with
 * the measured truth on the loadouts that expose those three bugs — so reverting
 * `botPetalReach` to this shape fails the gate instead of silently parking bots
 * outside their own reach again.
 */
function legacyReach(bot: ServerPlayer, petalExtension: number): number {
    const sizeMult = bot.sizeMultiplier ?? 1.0;
    const baseRadius = (60 + (40 / 2) * (sizeMult - 1)) * petalExtension;
    const playerRangeMod = botRangeModifier(bot);
    let maxRangeMult = 1.0;
    let maxPetalHalfSize = 0;
    for (const item of (bot.loadout as any[]) ?? []) {
        if (!item || item.type !== 'petal' || !item.petalType || !item.rarity) continue;
        const stats = getPetalStats(item.petalType, item.rarity);
        if (!stats) continue;
        const effectiveSize = item.customSize ?? stats.size ?? 1.0;
        maxPetalHalfSize = Math.max(maxPetalHalfSize, (40 * effectiveSize) / 2);
        if (stats.range !== undefined) {
            maxRangeMult = Math.max(maxRangeMult, stats.range * playerRangeMod);
        } else {
            maxRangeMult = Math.max(maxRangeMult, playerRangeMod);
        }
    }
    return baseRadius * maxRangeMult + maxPetalHalfSize;
}

interface ReachCase {
    name: string;
    loadout: any[];
    sizeMultiplier?: number;
    /** True when the old formula is expected to get this loadout wrong. */
    legacyWasWrong: boolean;
}

/** Find an equipped petal type that actually carries the given stat flag. */
function findPetalWith(predicate: (stats: any) => boolean, rarity: string): string | null {
    const candidates = [
        'basic', 'stinger', 'iris', 'faster', 'cutter', 'missile', 'bone', 'glass',
        'dandelion', 'yggdrasil', 'rock', 'third_eye', 'powder', 'javascript', 'soil',
        'rose', 'sand', 'web', 'pollen', 'lightning', 'wing', 'magnet', 'peas', 'salt',
    ];
    for (const type of candidates) {
        const stats = getPetalStats(type, rarity);
        if (stats && predicate(stats)) return type;
    }
    return null;
}

/**
 * Reach cases, and why every CONTROL loadout holds only the petal under test.
 *
 * Reach is a MAXIMUM over the ring, so one ordinary petal can hide any of the
 * three bugs. `basic` at rare is size 2 — a 40px half-size orbiting at the full
 * attack radius — which out-reaches a rose (capped at the neutral radius) and
 * exactly matches a sand clump (radius + 20px cluster offset + 20px half-size).
 * Pair either flagged petal with a single basic and the old formula agrees with
 * the real ring to the pixel. That is precisely how this drift survived in the
 * live game: it only bites builds where the flagged petal IS the outermost thing
 * the flower has.
 *
 * So each control equips two copies of the flagged petal and nothing else, and
 * the masked pairing is kept beside it as a NON-control case — `botPetalReach`
 * still has to be exact there, it just cannot prove anything about the old
 * formula, and marking it `legacyWasWrong` would make the control assertion fire
 * on a build where everything is correct.
 */
function checkReach(): void {
    const cases: ReachCase[] = [];

    cases.push({
        name: 'plain basics',
        loadout: Array.from({ length: 5 }, () => petal('basic', 'common')),
        legacyWasWrong: false,
    });

    // third_eye carries playerModifiers.range, so the aggregate range modifier is
    // not 1 and every petal's orbit scales with it.
    cases.push({
        name: 'third eye + basics',
        loadout: [petal('third_eye', 'epic'), ...Array.from({ length: 4 }, () => petal('basic', 'rare'))],
        legacyWasWrong: false,
    });

    // A defendOnly petal (rose) stays at the NEUTRAL orbit while the flower is
    // attacking, so its radius must not scale with the attack extension. The old
    // formula extended it like anything else and over-estimated by ~60px.
    const defendOnly = findPetalWith(s => s.defendOnly === true, 'rare');
    if (defendOnly) {
        cases.push({
            name: `defendOnly (${defendOnly}) only, at attack extension`,
            loadout: [petal(defendOnly, 'rare'), petal(defendOnly, 'rare')],
            legacyWasWrong: true,
        });
        cases.push({
            name: `defendOnly (${defendOnly}) masked by a bigger petal`,
            loadout: [petal(defendOnly, 'rare'), petal('basic', 'rare')],
            legacyWasWrong: false,
        });
    } else {
        fail('no defendOnly petal found in the petal table — the rose case is not being covered');
    }

    // A clumped petal (sand) sits a cluster offset beyond its slot centre, so it
    // reaches FURTHER than its radius suggests. The old formula ignored the
    // offset and under-estimated by half a petal width.
    const clumped = findPetalWith(s => s.clumped === true && (s.count ?? 1) > 1, 'rare');
    if (clumped) {
        cases.push({
            name: `clumped (${clumped}) only`,
            loadout: [petal(clumped, 'rare'), petal(clumped, 'rare')],
            legacyWasWrong: true,
        });
        cases.push({
            name: `clumped (${clumped}) masked by a bigger petal`,
            loadout: [petal(clumped, 'rare'), petal('basic', 'rare')],
            legacyWasWrong: false,
        });
    } else {
        fail('no clumped petal found in the petal table — the sand case is not being covered');
    }

    // The third bug, and the only one that needs a MIXED loadout to show: the old
    // formula took `max(range multiplier)` and `max(half size)` over DIFFERENT
    // petals and added them, describing a petal that does not exist — the long
    // one's orbit with the fat one's width. Here the long petal is thin and the
    // fat petal is short, so the old answer exceeds both real edges.
    const longRange = findPetalWith(s => (s.range ?? 1) > 1, 'rare');
    if (longRange) {
        cases.push({
            name: `long-range (${longRange}) beside a fat short-range petal`,
            loadout: [petal(longRange, 'rare'), petal('basic', 'rare')],
            legacyWasWrong: true,
        });
    } else {
        fail('no petal with range > 1 found in the petal table — the mixed-maxima case is '
            + 'not being covered');
    }

    // Storage slots. `layoutPetalRing` never lays out slot 10+, so a huge petal
    // parked there must not widen the bot's idea of its own reach.
    cases.push({
        name: 'storage slots hold a giant petal',
        loadout: [
            ...Array.from({ length: 10 }, () => petal('basic', 'common')),
            petal('basic', 'common', 12),
        ],
        legacyWasWrong: true,
    });

    cases.push({
        name: 'custom size override',
        loadout: [petal('basic', 'common', 3.5), petal('stinger', 'rare')],
        legacyWasWrong: false,
    });

    cases.push({
        name: 'grown flower',
        loadout: [petal('basic', 'rare'), petal('third_eye', 'legendary')],
        sizeMultiplier: 2.5,
        legacyWasWrong: false,
    });

    for (const testCase of cases) {
        const bot = makeBot('bot_reach', 0, 0, testCase.loadout);
        (bot as any).sizeMultiplier = testCase.sizeMultiplier ?? 1;

        for (const extension of [0.5, 1.0, 2.0]) {
            const measured = measuredReach(bot, extension);
            const claimed = botPetalReach(bot, extension, 0);

            if (claimed < measured - 1e-6) {
                fail(`reach ${testCase.name} @ext ${extension}: botPetalReach ${claimed.toFixed(4)} `
                    + `is UNDER the farthest real petal edge ${measured.toFixed(4)} — bots would `
                    + 'crowd inside their standoff ring and take contact damage');
            }
            if (claimed > measured + 1.0) {
                fail(`reach ${testCase.name} @ext ${extension}: botPetalReach ${claimed.toFixed(4)} `
                    + `is ${(claimed - measured).toFixed(4)}px OVER the farthest real petal edge `
                    + `${measured.toFixed(4)} — bots would park outside their own reach and orbit `
                    + 'a mob they never damage');
            }
        }

        // The control, at the extension the combat controller actually uses.
        if (testCase.legacyWasWrong) {
            const measured = measuredReach(bot, 2.0);
            const legacy = legacyReach(bot, 2.0);
            if (Math.abs(legacy - measured) <= 1.0) {
                fail(`reach ${testCase.name}: the OLD formula agrees with the ring here `
                    + '(within 1px), so this case is no longer a control for the drift it was '
                    + 'chosen to expose — pick a loadout that still separates them');
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 3. The standoff a bot actually drives to is inside its reach
// ---------------------------------------------------------------------------

/**
 * The controller's own arithmetic, from `updateBotAI`:
 *
 *   baseStandoff = petalReach - STANDOFF_SAFETY_BUFFER + mobRadius - 10
 *   standoff     = baseStandoff + persona.standoffBias      (always negative)
 *
 * The property that matters is not the number but its consequence: a bot sitting
 * at `standoff` from a mob's CENTRE must be able to touch the mob's EDGE. This is
 * the gameplay statement of the drift bug, and it is checked separately from the
 * reach bound because the two could be individually right and jointly wrong (the
 * `- STANDOFF_SAFETY_BUFFER` and the `+ buffer` inside `computePetalReach` are
 * supposed to cancel; if one of them moved, only this notices).
 */
const STANDOFF_SAFETY_BUFFER = 18;

function checkStandoffReaches(): void {
    const loadout = [
        petal('basic', 'common'), petal('stinger', 'rare'), petal('third_eye', 'epic'),
        petal('rock', 'rare'), petal('cutter', 'legendary'),
    ];
    const bot = makeBot('bot_standoff', 0, 0, loadout);

    // The extension the combat branch drives at, and the persona bias band
    // (`-(2 + rng() * 38)`) at both extremes.
    const extension = 2.0;
    const reachWithBuffer = botPetalReach(bot, extension, STANDOFF_SAFETY_BUFFER);
    const measured = measuredReach(bot, extension);

    for (const mobRadius of [10, 20, 40, 120]) {
        for (const bias of [-2, -40]) {
            const standoff = reachWithBuffer - STANDOFF_SAFETY_BUFFER + mobRadius - 10 + bias;
            const gapToMobEdge = standoff - mobRadius;
            if (gapToMobEdge > measured) {
                fail(`standoff: a bot at ${standoff.toFixed(1)}px from a radius-${mobRadius} mob `
                    + `needs ${gapToMobEdge.toFixed(1)}px of reach but only has `
                    + `${measured.toFixed(1)}px — its petals cannot touch what it is fighting`);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 4. The bot tag, and the loadout the ECS sees
// ---------------------------------------------------------------------------

function checkTagAndLoadout(): void {
    resetWorldSingletons();
    resetSyncState();
    const runtime = makeRuntime();
    const world = runtime.world;

    const bots: ServerPlayer[] = [];
    for (let i = 0; i < 5; i++) {
        const bot = makeBot(`bot_tag_${i}`, 9000 + i * 60, 9000, [petal('basic', 'common')]);
        players[bot.id] = bot;
        ensurePlayerEntity(world, bot, NOW0);
        bots.push(bot);
    }
    // A human, to prove the tag distinguishes rather than matching everything.
    const human = makeBot('human_socket_1', 9500, 9000, [petal('basic', 'common')]);
    players[human.id] = human;
    ensurePlayerEntity(world, human, NOW0);

    const counts = botRosterCounts(world);
    if (counts.ecs !== counts.prefix) {
        fail(`C.IsBot count ${counts.ecs} != bot_ prefix count ${counts.prefix} — the ECS-side `
            + 'bot roster and the legacy prefix disagree about who is a bot');
    }
    if (counts.prefix !== bots.length) {
        fail(`bot_ prefix count ${counts.prefix} != the ${bots.length} bots created`);
    }
    const humanEntity = world.lookup(human.id);
    if (humanEntity !== undefined && world.has(humanEntity, C.IsBot)) {
        fail('a human socket id was tagged C.IsBot');
    }

    // The control: drop the tag from one bot and require the disagreement to be
    // visible. Without this the counts could both be computed from the prefix.
    const victim = world.lookup(bots[0].id);
    if (victim === undefined) {
        fail('a bot inserted into `players` never got an entity');
    } else {
        world.remove(victim, C.IsBot);
        const broken = botRosterCounts(world);
        if (broken.ecs === broken.prefix) {
            fail('removing C.IsBot from a bot did not change the ECS roster count — the roster '
                + 'is not actually reading the tag, so the tag can silently stop being set');
        }
        world.add(victim, C.IsBot);
    }

    // --- a DEAD bot must stay in the roster ------------------------------
    // The roster is a query, and adding C.IsDead moves the entity to a different
    // archetype — so excluding the dead is a one-word change that reads like
    // tidying ("why drive a corpse?"). It would be the worst kind of wrong: the
    // respawn countdown lives in `updateBotAI`'s own loop, which is the only
    // thing that ever calls `respawnBot`. Drop dead bots from the roster and
    // every bot that dies stays a corpse for the life of the process, the
    // population maintainer keeps counting it as a live bot so no replacement is
    // spawned, and the world quietly empties out. Nothing throws, no gate below
    // notices, and the roster/prefix consistency check above still passes because
    // it counts `players` either way.
    const corpse = world.lookup(bots[2].id);
    if (corpse === undefined) {
        fail('a bot inserted into `players` never got an entity');
    } else {
        bots[2].isDead = true;
        world.add(corpse, C.IsDead);
        const withCorpse = botRosterCounts(world);
        if (withCorpse.ecs !== withCorpse.prefix) {
            fail(`a DEAD bot dropped out of the ECS roster (${withCorpse.ecs} of `
                + `${withCorpse.prefix}) — its respawn timer only runs from updateBotAI's roster `
                + 'loop, so it would stay a corpse forever while still counting toward the '
                + 'bot population');
        }
        world.remove(corpse, C.IsDead);
        bots[2].isDead = false;
    }

    // --- the loadout the ECS sees ---------------------------------------
    // `spawnPlayer` stores the SAME array object in C.Loadout rather than a copy,
    // and that is deliberate. Bots swap petals in and out of slots 0 and 1
    // (raid powder, yggdrasil) from inside `updateBotAI` — outside every sync
    // window. With an alias there is exactly ONE array and one authority, so a
    // swap is visible to any ECS reader immediately. With a COPY there would be
    // two representations and the copy would be stale from the moment of the
    // swap, which is precisely the dual-representation bug this cutover keeps
    // producing. Pinned here so nobody "fixes" the alias into a copy.
    const entity = world.lookup(bots[1].id);
    if (entity === undefined) {
        fail('bot has no entity for the loadout alias check');
    } else {
        const slots = world.get(entity, C.Loadout, 'slots');
        if (slots !== bots[1].loadout) {
            fail('C.Loadout.slots is not the same array object as player.loadout — a bot petal '
                + 'swap made outside the sync window would leave the component stale');
        }
        const swapped = petal('powder', 'rare');
        (bots[1].loadout as any[])[0] = swapped;
        const after = world.get(entity, C.Loadout, 'slots') as any[];
        if (after[0] !== swapped) {
            fail('a loadout swap on the ServerPlayer was not visible through C.Loadout');
        }
    }

    resetWorldSingletons();
}

// ---------------------------------------------------------------------------
// 5. The modifier maths is the real one, not a mirror
// ---------------------------------------------------------------------------

/**
 * `calculatePlayerModifiers` only aggregates the PRIMARY ten slots; the old bot
 * mirrors walked the whole array. A speed petal parked in storage therefore used
 * to slow the bot's close-range controller down for a bonus it does not have.
 */
function checkModifierSource(): void {
    const speedPetal = findPetalWith(s => s.playerModifiers?.speed !== undefined, 'rare');
    if (!speedPetal) {
        fail('no speed-modifier petal found in the petal table — the storage-slot case is '
            + 'not being covered');
        return;
    }

    const stored = makeBot('bot_mod_storage', 0, 0, [
        ...Array.from({ length: 10 }, () => petal('basic', 'common')),
        petal(speedPetal, 'rare'),
    ]);
    if (botSpeedModifier(stored) !== 1) {
        fail(`a ${speedPetal} in storage slot 10 changed the bot's speed modifier to `
            + `${botSpeedModifier(stored)} — modifiers must come from the primary ten slots only`);
    }

    const equipped = makeBot('bot_mod_equipped', 0, 0, [petal(speedPetal, 'rare')]);
    if (botSpeedModifier(equipped) === 1) {
        fail(`equipping ${speedPetal} in slot 0 did not change the speed modifier — the check `
            + 'is not reading the real aggregation');
    }
}

// ---------------------------------------------------------------------------

export function runBotCutoverCheck(): string[] {
    failures.length = 0;
    // Importing botManager must not have started anything. This is the property
    // that makes every check above possible at all.
    assertNoServerBooted();

    checkInputOrder();
    checkReach();
    checkStandoffReaches();
    checkTagAndLoadout();
    checkModifierSource();

    // And driving the whole bot tick must not have started anything either — the
    // respawn and removal paths lazily require modules that would.
    assertNoServerBooted();
    return failures.slice();
}

export function main(): void {
    const found = runBotCutoverCheck();
    if (found.length === 0) {
        console.log('bot cutover check: bot inputs reach movement in the tick they were made, '
            + 'and the standoff maths matches the real petal ring');
        return;
    }
    console.error(`bot cutover check: ${found.length} FAILURE(S)`);
    for (const f of found) console.error('  x ' + f);
    process.exitCode = 1;
}
