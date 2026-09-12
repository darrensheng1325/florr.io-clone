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
// the tuning.
//
// THE SHAPE OF THE CONTROLLER, and why it is this shape
// -----------------------------------------------------
// A bot is not a decision tree evaluated from scratch every tick. That is what
// it used to be, and the trouble with it is that every individual branch can
// be right while the behaviour they add up to is wrong: a bot re-derives its
// destination thirty times a second, the answer wobbles between two equally
// good options, and what a player sees is a flower pacing.
//
// So the controller is an ACTIVITY MACHINE. A bot is doing exactly one thing
// at a time (BotActivity), it stays in it for at least a dwell period, and it
// leaves only on a reason strong enough to be worth interrupting what it was
// doing. Under that sits one sensing pass per tick (BotSenses) that every
// activity reads from, so the decision and the steering always agree about
// what is in the world.
//
// Two rules everything below serves:
//
//   * A BOT FIGHTS WHAT IS IN FRONT OF IT. There is no state a bot can be in
//     where it walks through a mob without engaging it. Travelling, roaming,
//     regrouping -- all of them yield to something close enough to hit, which
//     is what stops bots shouldering through a field of mobs untouched.
//   * A BOT STAYS WHERE THE GAME IS. Ambient mobs are stocked around HUMANS
//     (game_server.h: humanPlayers_), so ground with nobody on it has nothing
//     living on it. A bot that walks to the far side of the map to farm is a
//     bot that stands in an empty field forever, which is precisely what it
//     used to do. Hunting grounds are therefore chosen around the people who
//     are online, spread out around them rather than stacked on them.

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
// Senses
// ---------------------------------------------------------------------------

/// The one broadphase radius a bot's whole tick is answered out of.
///
/// Everything a bot can react to locally -- what to fight, what is about to
/// ram it, what is hunting it, what to pick up -- comes from ONE query at this
/// radius. The old controller ran three or four queries per bot per tick and
/// each one applied its own filters, which is how it ended up with a target
/// test and a steering test that disagreed about which mobs existed.
inline constexpr double kBotSenseRadius = 1600.0;

/// How far a bot notices a mob worth fighting, by the mob's tier. A rarer mob
/// is worth crossing more ground for; a boss is worth crossing a lot.
///
/// The common-tier figure is deliberately about A SCREEN. A player fights what
/// they can see, and a 1920x1080 viewport reaches 960 units sideways and 540
/// up; anything much shorter than that produces a bot standing in an open
/// field ignoring a mob the player watching it can see perfectly well. The old
/// 620 was under half a screen, which on ground stocked at the reference's own
/// density (one mob per ~700 units) meant a bot usually had no target at all
/// and simply wandered.
inline constexpr double kBotNoticeRange = 1150.0;
inline constexpr double kBotHighTierNoticeRange = 1500.0;
inline constexpr double kBotBossNoticeRange = 3200.0;

/// Ground a bot leaves alone because somebody real is standing on it.
///
/// Twenty-odd bots farming a band out-kill the spawner's trickle, and what
/// that looks like from inside the game is a player whose screen empties the
/// moment the bots arrive -- measured, the live mob count fell by four fifths
/// and the mobs on a player's own screen went from six to almost none. So a
/// mob within this of a HUMAN is not a bot's to take. The exceptions are the
/// two that are not choices: a mob already hunting the bot, and one it is
/// about to walk into.
inline constexpr double kBotPlayerClaimRadius = 1500.0;

/// How far below its own gear a bot stops bothering. A legendary flower does
/// not stop to kill baby ants, and one that does is both unbelievable and --
/// with twenty of them on a map -- the thing that strips the world bare
/// between one wave of the spawner and the next.
///
/// Blockers and anything already biting are exempt, as always: those are not
/// choices about what is worth farming.
inline constexpr int kBotTierFloorBelowGear = 3;

/// A mob this close to the bot's skin is IN THE WAY: it gets fought whatever
/// the bot thought it was doing. The margin is about a body's width, so a bot
/// commits before the collision rather than after it.
inline constexpr double kBotBlockerMargin = 55.0;

/// How long a mob stays "the thing that hurt me" after it lands a hit. Longer
/// than the mob hit interval, so a mob chewing on a bot reads as one
/// continuous attacker rather than a hit every half second.
inline constexpr double kBotAggressorMemoryMillis = 2500.0;

// ---------------------------------------------------------------------------
// Combat
// ---------------------------------------------------------------------------

/// Padding on the standoff ring, so position jitter still lands hits. Folded
/// into the reach estimate and subtracted again by the standoff maths.
inline constexpr double kBotStandoffBuffer = 18.0;

/// How far outside the standoff ring a bot still counts itself as IN the
/// fight. Without a band here, a target drifting a few units out flips the
/// bot from Fight to Hunt and back, which is the in-combat form of pacing.
inline constexpr double kBotEngageSlack = 120.0;

/// How far a bot will chase what it has committed to before giving up. Scaled
/// by the persona's aggression, and much longer for a boss.
inline constexpr double kBotPursueRange = 1500.0;
inline constexpr double kBotBossPursueRange = 3600.0;

/// How sharply the orbit controller corrects toward the standoff ring. Larger
/// is gentler; the correction passes smoothly through zero at the ring, which
/// is what stops a bot flipping between closing and backing off every tick.
inline constexpr double kBotOrbitRadialGain = 95.0;

/// Health at which a bot disengages, before the persona scales it. Recover to
/// this multiple of it before re-engaging, and stay out at least this long.
inline constexpr double kBotFleeHealthRatio = 0.26;
inline constexpr double kBotFleeRecoverRatio = 1.9;
inline constexpr double kBotFleeMinMillis = 1400.0;

/// Distance advantage a rival must beat before a bot abandons what it is
/// already committed to. Without it two mobs at near-equal range swap the
/// "best" slot every tick and the bot walks back and forth between them.
inline constexpr double kBotTargetStickiness = 300.0;
/// The same for ground loot, smaller: drops do not move, so the only thing
/// being damped is the bot's own position wobble.
inline constexpr double kBotPickupStickiness = 140.0;
inline constexpr double kBotItemSeekRange = 700.0;

/// The shortest time a bot may spend in an activity before something else may
/// claim it. Cheap hysteresis over the whole machine: a reason that is still
/// true in a third of a second will still be true then.
inline constexpr double kBotActivityDwellMillis = 320.0;

/// Petals are thrown out (attack) while a bot is in its standoff band and
/// pulled in (defend) while it is running. Between those it carries them
/// neutral, which is what a player walking somewhere looks like.
///
/// The attack toggle is held for at least this long once flipped: petals cost
/// nothing to extend, but a ring snapping in and out every other tick is the
/// most visible bot tell there is.
inline constexpr double kBotPetalHoldMillis = 260.0;

// ---------------------------------------------------------------------------
// Hunting grounds
// ---------------------------------------------------------------------------
//
// Where a bot sets up shop. See the header note: mobs live where people are,
// so this is fundamentally "pick a spot near somebody, far enough out not to
// crowd them".

/// The ring around a human a bot will settle in. The inner edge is far enough
/// that a bot is not standing on the player; the outer edge is inside the
/// neighbourhood the spawner actually stocks, which is what makes the ground
/// the bot picks have mobs on it.
inline constexpr double kBotHumanOrbitMin = 520.0;
inline constexpr double kBotHumanOrbitMax = 2600.0;

/// The working radius of a hunting ground, and how far outside it a bot may
/// get before it walks back. The gap between them is deliberately wide: a
/// leash a bot brushes against is a leash it oscillates on.
inline constexpr double kBotHomeRadius = 1500.0;
inline constexpr double kBotHomeReturnRadius = 2400.0;

/// The cell the world's mobs are counted into when a bot is choosing where to
/// work. About a screen across: fine enough to tell a busy corner of a band
/// from an empty one, coarse enough that two dozen bots do not all pick the
/// same cell and then find one mob in it.
inline constexpr double kBotHeatCellSize = 1200.0;
/// What one mob standing in a cell is worth, as a distance a bot would walk to
/// be there.
///
/// It has to be weighed against the WALK, or the whole population commutes:
/// with heat alone deciding, every bot picks whichever corner of the world is
/// busiest this second, spends a minute walking to it, and arrives after the
/// mobs that made it busy are dead. Measured, that put the bots ten thousand
/// units from the only player and nineteen of twenty-four of them permanently
/// in transit. The score below therefore charges the trip against the prize.
inline constexpr double kBotHeatWorth = 400.0;

/// How long a bot works one patch before moving on, and the shorter clock it
/// uses when it has not managed to reach the patch at all. A bot that cannot
/// get somewhere must not spend two minutes proving it.
inline constexpr double kBotHomeMinMillis = 35000.0;
inline constexpr double kBotHomeMaxMillis = 110000.0;
inline constexpr double kBotHomeUnreachedMillis = 12000.0;

/// A hunting ground with nothing alive on it is abandoned early: this is how
/// long a bot tolerates an empty patch before re-rolling.
inline constexpr double kBotBarrenPatienceMillis = 9000.0;

/// Past this distance from its ground a bot is TRAVELLING: it paths, it may
/// carry powder, and it walks rather than working. It stays in the trip until
/// it is back inside kBotHomeRadius, and the gap between the two is what keeps
/// a bot loitering on the boundary from flipping between walking and working
/// every few ticks.
inline constexpr double kBotTravelDistance = 2400.0;

// ---------------------------------------------------------------------------
// Raids
// ---------------------------------------------------------------------------

/// A chat-triggered raid rallies every bot for this long.
inline constexpr double kBotForcedRaidMillis = 45000.0;
/// Cooldown between boss callouts, rolled per announcement so bots do not
/// chain-call raids the instant a wave of bosses pops.
inline constexpr double kBotBossAnnounceMinMillis = 60000.0;
inline constexpr double kBotBossAnnounceMaxMillis = 90000.0;

/// How far a raid rally reaches, and how wide the crowd around the boss is.
///
/// The ring is sized off the RAIDERS, not fixed: twenty flowers packed into a
/// ninety-unit circle -- which is what the old constant asked for -- spend the
/// whole fight shoving each other out of the way, and the shoving is what
/// reads as a crowd of bots vibrating.
inline constexpr double kBotBossRallyRange = 4200.0;
inline constexpr double kBotRaidRingMin = 260.0;
inline constexpr double kBotRaidRingPerRaider = 26.0;

/// Far enough from the rally that traversal speed matters: slot 0 is swapped
/// for powder. Two thresholds rather than one, or a bot hovering at the
/// boundary re-rolls its loadout every tick.
inline constexpr double kBotPowderEquipDist = 2600.0;
inline constexpr double kBotPowderUnequipDist = 1700.0;
/// Powder has no common tier, so the swap's floor is uncommon.
inline constexpr int kBotPowderMinRarityIndex = 1;

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
inline constexpr double kBotYggReviveSeekRange = 1400.0;

// ---------------------------------------------------------------------------
// Steering
// ---------------------------------------------------------------------------

/// Gap kept between the bot's body and a mob's edge, the band outside it where
/// steering starts, and the hard cap on the repulsion. The cap is below 1
/// deliberately: this may bend a heading around a body, never invert the bot's
/// intent and leave it unable to reach a guarded goal.
inline constexpr double kBotMobAvoidMargin = 26.0;
inline constexpr double kBotMobAvoidLookahead = 110.0;
inline constexpr double kBotMobAvoidMax = 0.9;
inline constexpr double kBotMobAvoidQueryRadius = 260.0;
/// How hard the repulsion pushes while walking somewhere, and while fighting.
/// They are different numbers because they are different problems: a
/// traveller wants to go AROUND everything, and a fighter that dodged every
/// mob near it could never close on the one it picked.
inline constexpr double kBotAvoidStrengthTravel = 1.35;
inline constexpr double kBotAvoidStrengthFight = 0.35;

/// Just larger than two flower bodies: bots do not overlap, and do not
/// scatter.
inline constexpr double kBotSeparationRadius = kPlayerBaseRadius * 2.0 * 2.2;
inline constexpr double kBotSeparationStrength = 0.85;

/// Speed below which heading changes are instant. A near-stationary flower can
/// pivot freely; only one already moving has to arc into its new direction.
inline constexpr double kBotFreeTurnSpeed = 55.0;

/// The strafe direction is held for seconds at a time so a bot commits to a
/// circling direction, then flipped on a slow randomised timer so orbits do
/// not read as a fixed animation loop.
inline constexpr double kBotStrafeFlipMinMillis = 2600.0;
inline constexpr double kBotStrafeFlipMaxMillis = 7000.0;

// ---------------------------------------------------------------------------
// Roaming
// ---------------------------------------------------------------------------
//
// A correlated random walk, not a sequence of random destinations.
//
// The old wander picked a point, walked to it, stopped dead, and picked
// another -- which produces a stop-start trajectory with hard corners in it
// that nothing with hands on a mouse would ever draw. This holds a HEADING and
// drifts it, so the path curves; the destination is implied rather than
// chosen, and there is no arrival to stop at.

/// How fast the roam heading drifts, in radians per second of wandering, and
/// how hard it is pulled back toward the middle of the hunting ground once the
/// bot is near the edge.
inline constexpr double kBotRoamDriftRate = 0.9;
inline constexpr double kBotRoamHomePull = 1.9;
/// Where the pull starts, as a fraction of the home radius.
inline constexpr double kBotRoamEdgeFraction = 0.62;
/// A roaming bot stops now and then to look around. How often it considers it,
/// and how long a pause runs.
inline constexpr double kBotRoamPauseCheckMillis = 2600.0;
inline constexpr double kBotRoamPauseMinMillis = 450.0;
inline constexpr double kBotRoamPauseMaxMillis = 2400.0;

// ---------------------------------------------------------------------------
// Getting unstuck
// ---------------------------------------------------------------------------
//
// Not a trajectory watchdog. The old one watched for reversals and for
// circling and answered them with a timed sprint in a random direction, which
// is both a very visible tell and a good way to throw a bot out of a fight it
// was winning. What is left is the one case that is unambiguously a fault: the
// bot is ASKING to move and is not moving.

inline constexpr double kBotStuckSpeed = 30.0;        ///< units/second
inline constexpr double kBotStuckTripMillis = 700.0;  ///< how long before it counts
inline constexpr double kBotStuckProbeDist = 300.0;
/// Once freed, the bot holds its escape heading this long so it actually
/// clears whatever it was caught on rather than turning straight back.
inline constexpr double kBotUnstickHoldMillis = 550.0;

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

inline constexpr double botNoticeRangeForTier(Rarity r) {
    if (isBotBossTier(r)) return kBotBossNoticeRange;
    if (isBotHighTier(r)) return kBotHighTierNoticeRange;
    return kBotNoticeRange;
}

// ---------------------------------------------------------------------------
// Per-bot memory
// ---------------------------------------------------------------------------

/// What one bot is doing right now.
///
/// Exactly one at a time, held for at least kBotActivityDwellMillis. The order
/// here is the order the machine considers them in, which is also roughly the
/// order of how interruptible each one is.
enum class BotActivity : std::uint8_t {
    Roam,     ///< working its patch with nothing in particular to do
    Travel,   ///< walking to somewhere far enough to need a path
    Hunt,     ///< closing on a target it has committed to
    Fight,    ///< at the standoff ring, petals out
    Loot,     ///< walking onto a drop
    Retreat,  ///< hurt; breaking contact until it heals
    Revive,   ///< standing over a downed bot with a yggdrasil out
};

const char* botActivityName(BotActivity);

/// How a bot plays, as distinct from how well.
///
/// Four tempers rather than one set of jittered numbers, because a scalar
/// spread makes twenty bots that all do the same thing at slightly different
/// speeds. These make different CHOICES: a hunter leaves its patch to chase
/// something rare, a forager would rather pick up the drop, a skirmisher
/// fights at arm's length and bails early, a drifter mostly wanders and joins
/// in when a fight comes to it.
enum class BotTemper : std::uint8_t { Hunter, Forager, Skirmisher, Drifter };

/// A bot's persistent behavioural offsets. Rolled once per bot from its id, so
/// it is stable across deaths and reproducible when debugging one.
struct BotPersona {
    BotTemper temper = BotTemper::Hunter;
    /// Unit bias vector; keeps two bots chasing the same spot off identical
    /// coordinates, and seeds the phase of the slow steering noise.
    Vec2 bias{1, 0};
    double noisePhase = 0;
    /// How far INSIDE max petal reach this bot orbits. Always negative: bots
    /// vary in how tightly they crowd a mob, never in whether their petals can
    /// connect at all.
    double standoffBias = 0;
    /// Delay between spotting a target and committing to it.
    double reactionMillis = 0;
    /// Maximum heading change per tick. Low reads as lumbering.
    double turnRate = 0.3;
    /// Cruise speed while roaming.
    double cruise = 0.5;
    /// Scales the flee threshold and the pursue range: high is a bot that
    /// overstays and chases, low is one that plays it safe.
    double aggression = 1.0;
    /// How readily it abandons a patch for a new one.
    double restlessness = 1.0;
    /// How often it stops to look around.
    double stillness = 0.3;
    /// How much a drop is worth relative to a mob.
    double greed = 1.0;
};

/// Everything one bot remembers between ticks.
struct BotAiState {
    BotPersona persona;
    bool personaReady = false;

    // -- the machine -------------------------------------------------------
    BotActivity activity = BotActivity::Roam;
    double activitySinceMillis = 0;

    // -- hunting ground ----------------------------------------------------
    bool hasHome = false;
    Vec2 home;
    double homeRadius = kBotHomeRadius;
    double homeUntilMillis = 0;
    /// Whether the bot has actually GOT to this patch yet. A patch it cannot
    /// reach is abandoned on a much shorter clock than one it is working: a
    /// bot must not spend two minutes proving it cannot walk somewhere.
    bool homeReached = false;
    /// When the bot last saw anything alive on its patch. A patch that stays
    /// barren is abandoned early -- which is the difference between a bot that
    /// looks like it is farming and one standing in an empty field.
    double lastMobSeenMillis = 0;

    // -- commitment --------------------------------------------------------
    Entity target = NULL_ENTITY;
    /// When the candidate was first noticed, for the reaction delay, and
    /// whether the bot has committed to it yet.
    double targetNoticedMillis = 0;
    bool targetCommitted = false;
    Entity pickup = NULL_ENTITY;

    // -- damage ------------------------------------------------------------
    /// Last tick's health, so a drop can be seen without the combat system
    /// having to tell anyone, and how long the bot goes on counting itself as
    /// under attack afterwards. The second one is what keeps a bot being shot
    /// from off-screen -- by something its senses never saw -- from calmly
    /// carrying on with whatever it was doing.
    double lastHealth = -1.0;
    double hurtUntilMillis = 0;
    double fleeUntilMillis = 0;

    // -- movement ----------------------------------------------------------
    /// Last committed heading, turn-rate limited toward what the AI asked for,
    /// so bots arc instead of snapping.
    bool hasHeading = false;
    Vec2 heading{1, 0};
    /// The roam walk's heading, drifted rather than re-picked.
    double roamAngle = 0;
    bool roamReady = false;
    double roamPauseCheckMillis = 0;
    double idleUntilMillis = 0;
    /// Strafe direction, +1 or -1, flipped on a slow timer. Zero means unset.
    int strafeDir = 0;
    double strafeFlipMillis = 0;
    /// Smoothed raid slot angle -- the assigned slot jumps whenever the raider
    /// set changes, so the bot eases toward it instead of teleporting around.
    bool hasSlotAngle = false;
    double slotAngle = 0;
    /// Petal state, held briefly once flipped. See kBotPetalHoldMillis.
    bool petalsOut = false;
    double petalsFlippedMillis = 0;

    // -- stuck -------------------------------------------------------------
    double stuckMillis = 0;
    double unstickUntilMillis = 0;
    Vec2 unstickDir{1, 0};

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
};

/// Reusable A* scratch.
///
/// Flat arrays indexed by tile, which cannot be cleared per call, so `stamp`
/// marks which entries belong to the CURRENT search: a tile counts as
/// unvisited unless its stamp matches. That is the exact equivalent of a map
/// lookup returning nothing, without the clear and without the hashing.
struct BotPathScratch {
    std::vector<double> gScore;
    std::vector<std::int32_t> cameFrom;
    std::vector<std::uint32_t> stamp;
    std::uint32_t stampValue = 0;
    /// The grid the tables above are sized for. A map decides its own
    /// dimensions, so this is not a constant: sizing the search to
    /// kTilesPerAxis on a 128-tile map walks the search off the end of the
    /// world the map actually has.
    int cols = 0;
    int rows = 0;

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
