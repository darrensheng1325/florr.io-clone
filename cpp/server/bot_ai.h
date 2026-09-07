#pragma once
// What a bot decides, and what it has to remember to decide it.
//
// The controller itself is server/bot_ai.cpp -- GameServer methods, in the
// same way server/chat_commands.cpp holds the console, because the decision
// path reads the terrain, the map's annotation layer, the broadphase, the drop
// list, the squad roster and the chat channel, and threading six services
// through a free-standing class would only move the coupling.
//
// This header holds the three things that have to be VISIBLE from
// game_server.h: the per-bot memory, the pathfinder's reusable scratch, and
// the tuning. The tuning is here rather than in the .cpp because it is the
// part that has to be checkable against the reference
// (src/server/botManager.ts) line by line -- every constant below is that
// file's, under the same name, and a number that drifts is a bot that plays
// differently for no stated reason.

#include <cstdint>
#include <vector>

#include "shared/core/entity.h"
#include "shared/core/types.h"
#include "shared/game/components.h"
#include "shared/game/constants.h"
#include "shared/game/rarity.h"

namespace flix {

// ---------------------------------------------------------------------------
// Population
// ---------------------------------------------------------------------------

/// Total flowers the world aims to hold, bots plus humans.
inline constexpr int kBotTargetTotalPlayers = 23;
/// How often the population is reconsidered.
inline constexpr double kBotMaintainMillis = 1500.0;
/// Bots created per maintenance pass. A deficit is filled over several passes
/// rather than in one burst, which is what makes a restart look like players
/// arriving instead of a crowd appearing.
inline constexpr int kBotSpawnBurstCap = 4;
/// Bots outlive an empty server by this long, so a quick reconnect does not
/// land in a world that was emptied the moment the last player left.
inline constexpr double kBotIdleTimeoutMillis = 45000.0;

/// The target wanders by +-1 on a slow clock so the population drifts instead
/// of sitting on an exact number.
inline constexpr int kBotJitterMin = -3;
inline constexpr int kBotJitterMax = 2;
inline constexpr double kBotJitterStepChance = 0.35;
inline constexpr double kBotJitterIntervalMillis = 25000.0;

/// A dead bot's body is replaced after this long. Instant replacement reads as
/// a flower that never died.
inline constexpr double kBotRespawnDelayMillis = 3000.0;

// ---------------------------------------------------------------------------
// Combat
// ---------------------------------------------------------------------------

inline constexpr double kBotAggroRange = 500.0;          ///< common/uncommon/rare
inline constexpr double kBotHighTierAggroRange = 900.0;  ///< epic..ultra
inline constexpr double kBotBossRaidRange = 4000.0;      ///< super/unique/apex

/// Padding on the standoff ring, so position jitter still lands hits. Folded
/// into the reach estimate and subtracted again by the standoff maths, exactly
/// as the reference folds it.
inline constexpr double kBotStandoffBuffer = 18.0;
inline constexpr double kBotFleeHealthRatio = 0.22;
inline constexpr double kBotItemSeekRange = 600.0;

/// How sharply the orbit controller corrects toward the standoff ring. Larger
/// is gentler; the correction passes smoothly through zero at the ring, which
/// is what stops a bot flipping between closing and backing off every tick.
inline constexpr double kBotOrbitRadialGain = 90.0;
/// Recover to this multiple of the flee threshold before re-engaging, and stay
/// in flight at least this long once committed.
inline constexpr double kBotFleeRecoverRatio = 2.0;
inline constexpr double kBotFleeMinMillis = 1200.0;

/// Distance advantage a rival must beat before a bot abandons what it is
/// already committed to. Without it two mobs at near-equal range swap the
/// "best" slot every tick and the bot walks back and forth between them.
inline constexpr double kBotTargetStickiness = 320.0;
/// The same for ground loot, smaller: drops do not move, so the only thing
/// being damped is the bot's own position wobble.
inline constexpr double kBotPickupStickiness = 140.0;

// ---------------------------------------------------------------------------
// Territory
// ---------------------------------------------------------------------------

inline constexpr double kBotTetherRadius = 1400.0;
inline constexpr double kBotTetherReturnRadius = 2200.0;
/// Ultra+ bots patrol mythic ground looking for boss spawns, so their leash is
/// longer than everyone else's.
inline constexpr double kBotUltraRoamRadius = 2400.0;
inline constexpr double kBotUltraRoamReturn = 3200.0;

/// Tight clump around a raid target -- sized so every raider shares most of
/// its viewport with every other raider.
inline constexpr double kBotRaidClusterRadius = 90.0;
inline constexpr double kBotRaidClusterReturn = 180.0;

/// A legendary-or-better mob this close puts the bot into group mode.
inline constexpr double kBotHighRarityScanRange = 1200.0;
inline constexpr double kBotGroupClusterRadius = 500.0;
inline constexpr double kBotGroupClusterReturn = 900.0;
inline constexpr int kBotGroupTargetSize = 7;
inline constexpr int kBotGroupMinForMode = 4;

/// How long group mode survives past the last positive scan, and how long a
/// bot farms one zone before rotating onward.
inline constexpr double kBotHighRarityLingerMillis = 4000.0;
inline constexpr double kBotFarmZoneMinMillis = 90000.0;
inline constexpr double kBotFarmZoneMaxMillis = 240000.0;
/// Past this the farm timer stops mattering: a bot still walking across the
/// map must not rotate to a new zone mid-trip.
inline constexpr double kBotFarmZoneArrivedDist = 1200.0;

// ---------------------------------------------------------------------------
// Raids
// ---------------------------------------------------------------------------

/// A chat-triggered raid rallies every bot for this long.
inline constexpr double kBotForcedRaidMillis = 45000.0;
/// Cooldown between boss callouts, rolled per announcement so bots do not
/// chain-call raids the instant a wave of bosses pops.
inline constexpr double kBotBossAnnounceMinMillis = 60000.0;
inline constexpr double kBotBossAnnounceMaxMillis = 90000.0;

/// Far enough from the raid anchor that traversal speed matters: slot 0 is
/// swapped for powder. Two thresholds rather than one, or a bot hovering at
/// the boundary re-rolls its loadout every tick.
inline constexpr double kBotRaidPowderEquipDist = 700.0;
inline constexpr double kBotRaidPowderUnequipDist = 460.0;
/// Powder has no common tier, so the swap's floor is uncommon.
inline constexpr int kBotPowderMinRarityIndex = 1;

/// Long-haul routing kicks in past this; under it a bot simply walks.
inline constexpr double kBotRaidShortcutMinDist = 4000.0;
/// A teleporter is only worth taking when its destination lands this much
/// closer to the boss than the bot already is.
inline constexpr double kBotRaidTelePayoffRatio = 0.55;

// ---------------------------------------------------------------------------
// Yggdrasil
// ---------------------------------------------------------------------------

/// Another bot inside this range is somebody worth carrying a revive for.
inline constexpr double kBotYggBuddyRange = 600.0;
/// Once equipped the buddy has to get this far away before the petal is
/// dropped again, or a bot pacing the boundary re-rolls its loadout each tick.
inline constexpr double kBotYggBuddyDropRange = 820.0;
/// How far a bot will go out of its way to revive a downed one. Wider than the
/// petal's own revive reach so it has time to close the last of the distance.
inline constexpr double kBotYggReviveSeekRange = 1500.0;

// ---------------------------------------------------------------------------
// Steering
// ---------------------------------------------------------------------------

/// Gap kept between the bot's body and a mob's edge, the band outside it where
/// steering starts, the weight of the repulsion, and its hard cap. The cap is
/// below 1 deliberately: this may bend a heading around a body, never invert
/// the bot's intent and leave it unable to reach a guarded goal.
inline constexpr double kBotMobAvoidMargin = 24.0;
inline constexpr double kBotMobAvoidLookahead = 85.0;
inline constexpr double kBotMobAvoidStrength = 1.1;
inline constexpr double kBotMobAvoidMax = 0.85;
inline constexpr double kBotMobAvoidQueryRadius = 200.0;

/// Just larger than two flower bodies: bots do not overlap, and do not
/// scatter.
inline constexpr double kBotSeparationRadius = kPlayerBaseRadius * 2.0 * 2.2;
inline constexpr double kBotSeparationStrength = 0.9;

/// Speed below which heading changes are instant. A near-stationary flower can
/// pivot freely; only one already moving has to arc into its new direction.
inline constexpr double kBotFreeTurnSpeed = 55.0;

/// The strafe direction is held for seconds at a time so a bot commits to a
/// circling direction, then flipped on a slow randomised timer so orbits do
/// not read as a fixed animation loop.
inline constexpr double kBotStrafeFlipMinMillis = 3500.0;
inline constexpr double kBotStrafeFlipMaxMillis = 9000.0;

// ---------------------------------------------------------------------------
// Pathfinding
// ---------------------------------------------------------------------------

inline constexpr int kBotPathMaxNodes = 4000;
/// How many bots may recompute in one tick, so a whole raid repathing together
/// cannot dominate a frame.
inline constexpr int kBotPathMaxPerTick = 2;
inline constexpr double kBotPathWaypointReachedDist = kTileSize * 0.55;
inline constexpr double kBotPathStaleMillis = 5000.0;
inline constexpr int kBotPathGoalInvalidateTiles = 2;
/// A bot may not re-run A* more often than this even when its goal keeps
/// moving: it follows the slightly stale path and lets local steering close
/// the gap.
inline constexpr double kBotPathMinRepathMillis = 1500.0;
/// Full greedy line-of-sight smoothing runs at most this often per bot;
/// between passes a single ray re-validates the current waypoint.
inline constexpr double kBotPathSmoothIntervalMillis = 200.0;

// ---------------------------------------------------------------------------
// Trajectory watchdog
// ---------------------------------------------------------------------------
//
// Every individual decision can be locally reasonable and still add up to a
// bot pacing between two spots forever. Rather than enumerating causes, this
// watches the trajectory the decisions actually produce.

inline constexpr double kBotOscSampleMillis = 450.0;
inline constexpr double kBotOscMinStep = 10.0;
inline constexpr double kBotOscReversalDot = -0.30;
inline constexpr int kBotOscTripReversals = 3;
inline constexpr int kBotOscStillTrips = 3;
/// Net-displacement test, the one that catches a bot circling forever: it
/// never reverses and is never still, so the two tests above cannot see it.
inline constexpr int kBotOscNetWindow = 8;
inline constexpr double kBotOscNetMinPath = 320.0;
inline constexpr double kBotOscNetMaxDrift = 120.0;

inline constexpr double kBotUnstickMinMillis = 900.0;
inline constexpr double kBotUnstickMaxMillis = 1700.0;
/// While escaping, targeting is suppressed so the bot does not walk straight
/// back into whatever it was oscillating in.
inline constexpr double kBotUnstickTargetSuppressMillis = 1200.0;
inline constexpr double kBotUnstickProbeDist = 260.0;
/// Jamming this often inside this window means sidestepping is not enough --
/// the destination itself keeps walking the bot back into the same corner --
/// so it is sent somewhere else entirely.
inline constexpr int kBotUnstickEscalateTrips = 3;
inline constexpr double kBotUnstickEscalateWindowMillis = 15000.0;

// ---------------------------------------------------------------------------
// Squads
// ---------------------------------------------------------------------------

inline constexpr double kBotSquadTickMillis = 8000.0;
inline constexpr double kBotSquadCreateChance = 0.03;
inline constexpr double kBotSquadJoinChance = 0.5;

// ---------------------------------------------------------------------------
// Tiers
// ---------------------------------------------------------------------------

/// Raid rally points. Ultra is deliberately NOT one: bots treat an ultra as a
/// high-tier mob to fight, not as something to cross the map for.
inline constexpr bool isBotBossTier(Rarity r) {
    return r == Rarity::Super || r == Rarity::Unique || r == Rarity::Apex;
}
inline constexpr bool isBotHighTier(Rarity r) {
    return r == Rarity::Epic || r == Rarity::Legendary || r == Rarity::Mythic ||
           r == Rarity::Ultra;
}

/// Unique ranks above super so a raid always commits to a unique when both
/// exist.
inline constexpr int botTierPriority(Rarity r) {
    if (r == Rarity::Unique) return 4;
    if (r == Rarity::Super) return 3;
    if (isBotHighTier(r)) return 2;
    return 1;
}

inline constexpr double botAggroRangeForTier(Rarity r) {
    if (isBotBossTier(r)) return kBotBossRaidRange;
    if (isBotHighTier(r)) return kBotHighTierAggroRange;
    return kBotAggroRange;
}

// ---------------------------------------------------------------------------
// Per-bot memory
// ---------------------------------------------------------------------------

/// A bot's "personality": a persistent set of small behavioural offsets so no
/// two bots play identically. Rolled once per bot and held for its lifetime,
/// seeded off its id so it is stable and reproducible when debugging one.
struct BotPersona {
    /// Unit bias vector; keeps two bots chasing the same spot off identical
    /// coordinates.
    Vec2 bias{1, 0};
    /// How far INSIDE max petal reach this bot orbits. Always negative: bots
    /// vary in how tightly they crowd a mob, never in whether their petals can
    /// connect at all.
    double standoffBias = 0;
    /// Delay between spotting a target and committing to it.
    double reactionMillis = 0;
    /// Maximum heading change per tick. Low reads as lumbering.
    double turnRate = 0.3;
    /// Cruise speed while wandering.
    double wanderSpeed = 0.5;
    /// Chance of pausing on arrival at a wander point.
    double idleChance = 0;
    /// How long the bot stays in a fight before running.
    double aggression = 1.0;
};

/// Everything one bot remembers between ticks.
///
/// Field for field with the reference's `BotAIState` plus its persona map,
/// with one difference of representation: TypeScript spells "no value" as
/// `undefined` and this spells it as a companion flag or a sentinel, because a
/// zero deadline is a real time here.
struct BotAiState {
    // -- wander ------------------------------------------------------------
    Vec2 wanderTarget;
    double nextWanderMillis = 0;
    /// Idle pause during a wander. `atWanderTarget` latches arrival so the "do
    /// I pause here?" roll happens once per trip, not once per tick spent
    /// standing on the target.
    double idleUntilMillis = 0;
    bool atWanderTarget = false;

    // -- farming ground ----------------------------------------------------
    //
    // Sticky. Re-picking the anchor from a distance-sorted list every tick made
    // the "k-th nearest zone" flip as the bot moved, which is the classic
    // two-point shuffle: walk toward A, A becomes nearest, anchor becomes B,
    // walk back. The zone is chosen once and held.
    bool hasFarmZone = false;
    Vec2 farmZone;
    int farmZoneRarityIndex = -1;
    double farmZoneUntilMillis = 0;
    int farmZoneRotation = 0;

    /// Group mode lingers this long past the last positive scan so a mob
    /// drifting in and out of range cannot flap the anchor every tick.
    double highRarityUntilMillis = 0;

    // -- commitment --------------------------------------------------------
    Entity target = NULL_ENTITY;
    double targetAcquiredMillis = 0;
    Entity pickup = NULL_ENTITY;

    /// Flee hysteresis: bots keep running until meaningfully healed rather
    /// than flipping at the threshold.
    bool fleeing = false;
    double fleeUntilMillis = 0;

    // -- movement smoothing ------------------------------------------------
    /// Last committed heading, turn-rate limited toward what the AI asked for,
    /// so bots arc instead of snapping.
    bool hasHeading = false;
    Vec2 heading{1, 0};
    /// Strafe direction, +1 or -1, flipped on a slow timer. Zero means unset.
    int strafeDir = 0;
    double strafeFlipMillis = 0;
    /// Smoothed raid slot angle -- the assigned slot jumps whenever the raider
    /// set changes, so the bot eases toward it instead of teleporting around.
    bool hasSlotAngle = false;
    double slotAngle = 0;

    // -- watchdog ----------------------------------------------------------
    bool oscStarted = false;
    double oscSampleMillis = 0;
    Vec2 oscPosition;
    Vec2 oscPrevStep;
    bool hasOscPrevStep = false;
    int oscReversals = 0;
    int oscStill = 0;
    /// Ring of recent sample positions, for the net-displacement test.
    std::vector<Vec2> oscTrack;

    double unstickUntilMillis = 0;
    Vec2 unstickDir{1, 0};
    int unstickTrips = 0;
    bool hasUnstickTrips = false;
    double unstickTripsAtMillis = 0;
    double suppressTargetUntilMillis = 0;

    // -- path --------------------------------------------------------------
    std::vector<Vec2> pathNodes;
    std::size_t pathIndex = 0;
    bool hasPath = false;
    bool hasPathGoal = false;
    int pathGoalTileX = 0;
    int pathGoalTileY = 0;
    double pathCreatedMillis = 0;
    bool hasRepathed = false;
    double lastRepathMillis = 0;
    bool hasSmoothed = false;
    double lastSmoothMillis = 0;

    // -- loadout swaps -----------------------------------------------------
    //
    // The slot's original contents, so a traversal petal is handed back when
    // the trip is over rather than becoming the bot's build.
    bool powderSwapped = false;
    LoadoutSlot powderOriginal;
    bool yggSwapped = false;
    LoadoutSlot yggOriginal;

    /// Next time this bot considers a squad action. Staggered per bot.
    double nextSquadMillis = 0;

    BotPersona persona;
    bool personaReady = false;
};

/// Reusable A* scratch.
///
/// Flat arrays indexed by tile, which cannot be cleared per call (40,000
/// entries), so `stamp` marks which entries belong to the CURRENT search: a
/// tile counts as unvisited unless its stamp matches. That is the exact
/// equivalent of a map lookup returning nothing, without the clear and without
/// the hashing -- and it is why the reference's own profile put A* at a
/// quarter of its server before the same change was made there.
struct BotPathScratch {
    std::vector<double> gScore;
    std::vector<std::int32_t> cameFrom;
    std::vector<std::uint32_t> stamp;
    std::uint32_t stampValue = 0;

    /// Min-heap keyed by `f`, in parallel arrays: A* pushes up to eight times
    /// per expansion, and an array of nodes would allocate tens of thousands
    /// of short-lived objects per search.
    std::vector<double> heapF;
    std::vector<std::int32_t> heapX;
    std::vector<std::int32_t> heapY;
    std::size_t heapSize = 0;

    /// Reconstruction buffer, so a finished path is built without allocating.
    std::vector<Vec2> path;
};

} // namespace flix
