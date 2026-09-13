#pragma once
// Mob and petal content: the JSON tables, loaded once, addressed by index.
//
// Two things shape this file.
//
//  * The index is what crosses the wire. It is assigned in sorted-key order so
//    that a server and a client reading the same files necessarily agree on
//    what entry 17 is; contentHash() catches the case where they are not
//    reading the same files at all.
//
//  * Everything the simulation reads per tick is a plain field on a struct in
//    a contiguous vector. No map lookups and no string comparisons survive
//    past load: ids are resolved to indices in a link pass, `ai_type` becomes
//    an AiKind, and `#rrggbb` becomes a packed integer.
//
// The shipped data is dirty -- nulls, a negative damage, an angle in the wrong
// unit, an offset of -1e100. Load sanitises every one of those, records a
// human-readable line in warnings(), and guarantees that nothing non-finite
// reaches the simulation.

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "shared/core/types.h"
#include "shared/game/components.h"
#include "shared/game/rarity.h"

namespace flix {

/// What mobIndex()/petalIndex() answer for an id the content does not define.
/// Deliberately the same sentinel as an empty loadout slot: "no petal" and
/// "a petal nobody has heard of" mean the same thing to everything downstream.
inline constexpr std::uint16_t kInvalidIndex = kNoPetal;

// ---------------------------------------------------------------------------
// Shared sub-structures
// ---------------------------------------------------------------------------

/// A packed 0xRRGGBBAA colour. Alpha is carried because the `bubble` petal
/// ships a fully transparent `rgba(...)` fill and means it.
using Rgba = std::uint32_t;

inline constexpr Rgba kOpaqueWhite = 0xFFFFFFFFu;

/// A volley. `present` is what distinguishes "fires nothing" from "fires a
/// projectile whose fields all happen to be zero".
struct ProjectileSpec {
    bool present = false;
    int count = 1;
    double distance = 0;      ///< units travelled before it expires
    double speed = 0;         ///< units per second
    /// Angular STEP between adjacent projectiles, RADIANS -- not the total fan
    /// width, and not degrees however large it looks. `flower` ships 72, which
    /// is 72 radians: the five shots wrap to a lopsided pattern rather than the
    /// clean 72-degree star the number suggests, and the reference fires it
    /// that way. Reinterpreting it as degrees is a different weapon.
    double spreadAngle = 0.2;
    double seekRange = 0;     ///< 0 for a projectile that does not home
    /// Half-angle around the FIRING bearing that a seeking shot will re-aim
    /// within, radians. Applied once, at launch: the shot then flies straight,
    /// which is what keeps the client's dead reckoning exact.
    double seekCone = kPi * 0.25;

    /// A mob fires a named petal as ammunition; a petal fires itself and
    /// leaves this empty.
    std::string ammoPetalId;
    std::uint16_t ammoPetalIndex = kInvalidIndex;
    Rarity ammoRarity = Rarity::Common;
};

/// A ring of petals a mob carries, as a flower does.
struct PetalRingSpec {
    bool present = false;
    std::string petalId;
    std::uint16_t petalIndex = kInvalidIndex;
    int count = 0;
};

/// A nest that keeps producing escorts.
struct PeriodicSpawnSpec {
    bool present = false;
    std::string mobId;
    std::uint16_t mobIndex = kInvalidIndex;
    double intervalMillis = 0;
    double lifetimeMillis = 0;   ///< 0 = the escort never expires on its own
    int maxAlive = 0;
    /// Tiers relative to the parent, so a rare queen fields uncommon soldiers.
    int rarityOffset = 0;
};

/// A lingering damage field (uranium).
struct RadiationSpec {
    bool present = false;
    double radius = 0;
    double intervalMillis = 0;
};

/// Passive bonuses a petal grants its holder, exactly as the JSON writes them.
///
/// Some of these are multipliers around a neutral 1.0 and some are additive
/// amounts in world units. They are kept apart here rather than normalised
/// because rarity scales the two kinds differently -- see petalStats().
struct PetalModifiers {
    // Multiplicative, neutral at 1.0.
    double maxHealth = 1.0;
    double speed = 1.0;
    double range = 1.0;          ///< petal reach
    double rotationSpeed = 1.0;  ///< ring spin; negative reverses it
    double playerRadius = 1.0;
    double damage = 1.0;

    // Additive, neutral at 0.
    double luck = 0.0;
    double magnetism = 0.0;              ///< extra pickup radius, units
    double aggroRadius = 0.0;            ///< extra mob notice range, units
    double petalAttractionRadius = 0.0;  ///< pulls loose petals in, units
    double poisonArmor = 0.0;            ///< poison damage absorbed per second

    bool any = false;   ///< set when the JSON carried a playerModifiers block
};

// ---------------------------------------------------------------------------
// MobConfig
// ---------------------------------------------------------------------------

/// One mob's place in one group: which group, and how heavily it is weighted
/// inside it. Read from either side -- a MobConfig lists the groups it is in,
/// and a MobGroup lists the mobs in it -- because a spawn rolls over a group
/// and a tool asks about a mob.
struct MobGroupMember {
    std::uint16_t group = 0;   ///< index into ContentRegistry::mobGroups()
    std::uint16_t mob = 0;     ///< index into ContentRegistry::mob()
    double weight = 1.0;
};

/// One entry of mobs.json, at its base (common) tier. Rarity is applied by
/// mobStats(); nothing here is pre-scaled.
struct MobConfig {
    std::string id;             ///< the JSON key, e.g. "soldier_ant"
    std::string name;
    std::string description;
    std::string color;          ///< as written, for tooling and tooltips
    Rgba colorRgba = kOpaqueWhite;
    std::string image;          ///< inline SVG source

    double damage = 0;
    double health = 1;
    double size = 1;            ///< body diameter in "size units"; see mobStats()
    double speed = 0;           ///< config units; mobStats() converts to units/s
    double cooldownMillis = 0;  ///< gap between attacks
    double range = 0;           ///< aggro range, world units
    double visualScale = 1.0;   ///< art only; never touches the hitbox

    AiKind ai = AiKind::Neutral;

    /// The groups this mob belongs to, and how heavily it is weighted inside
    /// each one.
    ///
    /// A group is whatever mobs.json says it is -- `garden`, `desert`,
    /// `sewers`, `boss_rush`, anything an author types. There is no fixed list
    /// and no fixed count: the groups ARE the union of the names the mobs use,
    /// which is what lets a map's spawn band name one without a second file
    /// having to agree that it exists.
    ///
    /// The weight is per group on purpose. A hornet that is common in the
    /// garden and rare in the jungle is one mob with two weights, and a single
    /// `spawn_weight` could only say one of those.
    std::vector<MobGroupMember> groups;

    /// The weight a group membership takes when it does not state one of its
    /// own, from `spawn_weight`. Also what the maze pool weights by.
    double spawnWeight = 1.0;

    /// The mob does not exist below this tier: mobStats() reports it as
    /// non-ambient for anything lower, which is the single place every spawner
    /// already looks.
    Rarity minRarity = Rarity::Common;

    bool hideRotation = false;   ///< draw upright regardless of heading
    bool noEggDrop = false;
    bool reversed = false;       ///< art is mirrored horizontally
    bool noMobCollision = false;

    /// The mob never appears in a GROUP roll -- a band or a region naming a
    /// group never produces it. `target_dummy` declares no
    /// spawn_weight and would otherwise inherit the default 1.0 and take its
    /// share of every group it belongs to; it only reaches the world through
    /// a band that names it outright, which is how the dummy plots work.
    bool neverAmbient = false;

    /// Touching this mob -- body, ring or shot -- leaves the flower glitched
    /// until it next logs in. A property of the family rather than of the
    /// contact path, so every path infects at once.
    bool glitchInfecting = false;

    /// A chain head -- a centipede or a leech: `segmentCount` body mobs of type
    /// `segmentBodyIndex` trail it. Both are derived from the id at load,
    /// because the reference expresses the head->body link as a naming rule
    /// rather than a JSON field and every spawn path would otherwise have to
    /// repeat it.
    std::uint16_t segmentBodyIndex = kInvalidIndex;
    int segmentCount = 0;

    /// Applied when this mob is SUMMONED rather than spawned wild. Only the
    /// digger is nerfed, and only as a pet: a wild digger keeps its full stats.
    double petHealthScale = 1.0;
    double petDamageScale = 1.0;

    /// Per-spawn size jitter, multiplying `size`. Equal bounds means none.
    double randomSizeMin = 1.0;
    double randomSizeMax = 1.0;

    /// Escorts placed the moment the nest spawns, and the waves it sends
    /// afterwards. Both are already resolved to mob indices.
    std::vector<std::uint16_t> initialSpawns;
    std::vector<std::vector<std::uint16_t>> spawnWaves;

    ProjectileSpec projectile;
    PetalRingSpec petalRing;
    PeriodicSpawnSpec periodicSpawn;

    /// Poison the mob's touch applies. Stored per second, converted from the
    /// per-millisecond figure the JSON uses.
    double poisonPerSecond = 0;
    double poisonDurationMillis = 0;

    bool emissive = false;
    std::string lightColor;
    Rgba lightColorRgba = kOpaqueWhite;
    double lightRadius = 0;

    /// XP awarded per tier, filled from mob_xp.json with the derivations
    /// described in loadFiles().
    std::array<double, kRarityCount> xp{};
};

// ---------------------------------------------------------------------------
// PetalConfig
// ---------------------------------------------------------------------------

/// One entry of petals.json, at its base (common) tier.
struct PetalConfig {
    std::string id;
    std::string name;
    std::string description;
    std::string color;
    Rgba colorRgba = kOpaqueWhite;
    std::string image;

    double damage = 0;
    double health = 0;
    double size = 1;            ///< diameter in "size units"; see petalStats()
    /// Art only, exactly as MobConfig::visualScale is: it multiplies the drawn
    /// petal everywhere the world paints one, and never the `size` the
    /// simulation reaches, hits and orbits with.
    double visualScale = 1.0;
    double cooldownMillis = kDefaultPetalReloadMillis;   ///< reload after breaking
    int count = 1;              ///< petals spawned per equipped slot

    bool isAdminPetal = false;

    PetalModifiers modifiers;

    /// Mirrors the web game's default: petals without an explicit knockback
    /// field still push for 5.  Zero is reserved for petals that opt out.
    double knockback = 5;
    ProjectileSpec projectile;
    double range = 0;           ///< reach for the petals that have one
    /// What a worn cutter adds to the flower's BODY damage, before rarity.
    /// Scaled on the damage ladder, so it is stated at the common tier like
    /// every other damage figure. Not `damage` itself: nothing about this
    /// petal touches a mob, and putting it there would make every tooltip and
    /// drop call it a weapon.
    double bodyDamage = 0;
    std::uint8_t equipFlags = EquipNone;

    double poisonPerSecond = 0;
    double poisonDurationMillis = 0;

    double speed = 0;           ///< orbit speed for the petals that override it
    bool noPhysics = false;     ///< no body, no collision: a pure modifier
    bool defendOnly = false;    ///< only acts while the ring is pulled in
    bool clumped = false;       ///< a count > 1 spawns as one cluster
    bool independentHealth = false;  ///< each petal of a cluster breaks alone
    bool wallCollide = false;
    bool emissive = false;

    double burstHeal = 0;
    double burstHealChargeMillis = 0;
    double passiveHeal = 0;     ///< per second, before rarity
    double burstShield = 0;

    /// Held at a fixed angle instead of orbiting. `has` distinguishes the
    /// petals pinned to 0 radians from the ones that simply orbit.
    bool hasFixedDirection = false;
    double fixedDirection = 0;

    /// Draw offset in the petal's own rotated frame. Both axes, because the
    /// reference translates by both after the rotation and a petal that
    /// declares only an X offset would otherwise be drawn on the ring centre.
    double visualOffsetX = 0;
    double visualOffsetY = 0;
    /// The petal is not drawn at all. Three entries express this in the JSON
    /// with a visualOffsetY of -1e100, which is a way of shoving the sprite
    /// off the world rather than a number anything should compute with.
    bool hidden = false;

    /// The `damageCooldown` key, or kPetalHitIntervalMillis (zero) when the
    /// petal declares none. Non-zero therefore means exactly what truthiness
    /// means in the reference: this petal is throttled, and per INSTANCE
    /// rather than per victim. Three of the seventy-four are.
    double damageIntervalMillis = kPetalHitIntervalMillis;

    double cameraZoom = 1.0;    ///< < 1 zooms out
    double lightRadius = 0;
    std::string lightColor;
    Rgba lightColorRgba = kOpaqueWhite;

    /// A summoned mob. `petCount` is how many one petal keeps alive.
    std::string petMobId;
    std::uint16_t petMobIndex = kInvalidIndex;
    Rarity petMobRarity = Rarity::Common;
    int petCount = 1;

    double slowFactor = 1.0;    ///< multiplies the victim's speed; 1 = none
    double slowDurationMillis = 0;

    double spongeDamageDurationMillis = 0;
    double attractionForce = 0;
    double webRadius = 0;
    RadiationSpec radiation;

    /// False when the JSON gave no health pool at all. Such a petal is a pure
    /// modifier or an emitter and can never be broken; it is NOT a petal with
    /// zero health, which would break on the first tick.
    bool breakable = true;
};

// ---------------------------------------------------------------------------
// Derived per-rarity stats
// ---------------------------------------------------------------------------

/// A mob at one tier. Computed on demand: the whole table is a few multiplies,
/// and a precomputed 51x10 grid would only be a cache the loader has to keep
/// coherent.
struct MobStats {
    double health = 1;
    double damage = 0;
    double radius = 0;          ///< world units
    double mass = 1;            ///< proportional to area
    double speed = 0;           ///< world units per second
    /// Speed used ONLY while pursuing a target. Ten mob types chase at exactly
    /// the flower's top speed so a fleeing player can never outrun them, but
    /// they still WANDER at their authored speed -- folding the override into
    /// `speed` makes an idle bee cross the screen. A slow scales `speed`, so a
    /// chase that bypasses it has to re-derive the same ratio.
    double chaseSpeed = 0;
    bool playerSpeedChaser = false;
    double xp = 1;
    double aggroRange = 0;
    double attackCooldownMillis = 0;
    double poisonPerSecond = 0;
    double poisonDurationMillis = 0;
    double visualScale = 1.0;
    double spawnWeight = 1.0;
    /// Some mob behaviours change by rarity (for example rare bees become
    /// neutral/retaliatory). This is derived alongside the numeric stats.
    AiKind ai = AiKind::Neutral;

    /// True when an ambient group roll may produce this mob AT THIS TIER.
    ///
    /// False below `min_rarity`, which is how that rule is enforced everywhere
    /// at once: a spawner asks this and never has to know the rule exists.
    bool ambient = false;

    bool spawnable() const { return ambient; }
};

/// A petal at one tier.
struct PetalStats {
    double damage = 0;
    double health = 0;
    /// Body damage granted to the wearer, scaled for this tier.
    double bodyDamage = 0;
    double reloadMillis = kDefaultPetalReloadMillis;
    double poisonPerSecond = 0;
    double poisonDurationMillis = 0;
    double heal = 0;                    ///< burst heal per charge
    double healChargeMillis = 0;
    double passiveHealPerSecond = 0;
    double knockback = 0;
    double shield = 0;
    double slowFactor = 1.0;
    double slowDurationMillis = 0;
    double webRadius = 0;               ///< rarity-scaled field radius
    double spongeDamageDurationMillis = 0;
    double attractionForce = 0;
    double radius = 0;                  ///< world units
    /// The petal's `size` stat as authored. Several derived reaches are stated
    /// against it rather than against `radius` -- a projectile is half the
    /// firing petal's radius, a pollen puff is 6 x size, a wall resolve uses
    /// 20 x size -- and deriving them by dividing `radius` back out is how the
    /// two halved by the wrong scale went unnoticed.
    double size = 1;
    double visualScale = 1.0;           ///< art only; flat across rarities
    double damageIntervalMillis = kPetalHitIntervalMillis;
    int count = 1;
    bool breakable = true;
    double cameraZoom = 1.0;
    PetalModifiers modifiers;           ///< already scaled for this tier
};

// ---------------------------------------------------------------------------
// ContentRegistry
// ---------------------------------------------------------------------------

/// One mob group, as the mobs themselves define it.
///
/// Built by the loader out of every `groups` entry in mobs.json: the groups
/// are the union of the names used, in first-mention order, and a group's
/// members are the mobs that named it with the weight each gave.
struct MobGroup {
    std::string id;
    std::vector<MobGroupMember> members;
};

class ContentRegistry {
public:
    /// Loads `mobs.json`, `petals.json` and (optionally) `mob_xp.json` from
    /// one directory, and folds the directory's maps -- `maps.json` and every
    /// map it names -- into contentHash(), so a client whose staged maps
    /// differ from the server's (a moved pad, a renamed door) is refused at
    /// the handshake rather than drawing annotations the server does not
    /// have. A directory with no manifest folds nothing.
    ///
    /// On failure `errorOut` says what went wrong and the registry keeps
    /// whatever it already held -- a bad hot reload must not leave a running
    /// server with no content.
    bool load(const std::string& dataDir, std::string& errorOut);

    /// The same, with the three paths given explicitly. `xpPath` may be empty
    /// or absent: XP then falls back to 1 and a warning is recorded.
    bool loadFiles(const std::string& mobsPath, const std::string& petalsPath,
                   const std::string& xpPath, std::string& errorOut);

    /// An out-of-range index yields a shared placeholder rather than undefined
    /// behaviour: these indices arrive from the wire, and a corrupt one must
    /// cost a wrong-looking mob, not the process.
    const MobConfig& mob(std::uint16_t index) const;
    const PetalConfig& petal(std::uint16_t index) const;

    std::uint16_t mobIndex(const std::string& id) const;
    std::uint16_t petalIndex(const std::string& id) const;

    std::size_t mobCount() const { return mobs_.size(); }
    std::size_t petalCount() const { return petals_.size(); }

    /// Every petal index in CATALOGUE order: petals.json's own key order, then
    /// the generated eggs in mobs.json's order. This is what the browser's
    /// `Object.keys(PETAL_CONFIG)` yields, and what any surface that lists
    /// petals must walk -- the indices themselves are sorted so that the wire
    /// stays stable, which is a different order and the wrong one to paint.
    const std::vector<std::uint16_t>& petalDisplayOrder() const { return petalOrder_; }

    /// FNV-1a folded over the raw bytes of every file loaded, in a fixed
    /// order. Compared in the connect handshake.
    std::uint32_t contentHash() const { return hash_; }

    /// Everything the loader had to repair, one line each. Empty on clean
    /// data; the shipped data is not clean.
    const std::vector<std::string>& warnings() const { return warnings_; }

    bool loaded() const { return !mobs_.empty(); }

    MobStats mobStats(std::uint16_t index, Rarity r) const;
    PetalStats petalStats(std::uint16_t index, Rarity r) const;

    /// The mob groups, in first-mention order. A map's spawn band names one of
    /// these, or a mob id; the group is looked up first, so a group and a mob
    /// sharing a name resolves to the group.
    const std::vector<MobGroup>& mobGroups() const { return mobGroups_; }
    std::size_t mobGroupCount() const { return mobGroups_.size(); }

    /// The group `id` names, or kInvalidIndex.
    std::uint16_t mobGroupIndex(const std::string& id) const;

    /// An out-of-range index yields a shared empty group, for the same reason
    /// mob() yields a placeholder: these indices come out of map files.
    const MobGroup& mobGroup(std::uint16_t index) const;

private:
    std::vector<MobConfig> mobs_;
    std::vector<MobGroup> mobGroups_;
    std::unordered_map<std::string, std::uint16_t> mobGroupIds_;
    std::vector<PetalConfig> petals_;
    std::unordered_map<std::string, std::uint16_t> mobIds_;
    std::unordered_map<std::string, std::uint16_t> petalIds_;
    std::vector<std::uint16_t> petalOrder_;
    std::vector<std::string> warnings_;
    std::uint32_t hash_ = 0;

    /// The maps half of load(): see there.
    void foldMapsIntoHash(const std::string& dataDir);
};

// ---------------------------------------------------------------------------
// Process-wide content
// ---------------------------------------------------------------------------

/// The loaded content. Reachable from anywhere so that a system does not have
/// to thread a registry reference through every call it makes; content is
/// immutable after load, so there is nothing to synchronise.
const ContentRegistry& content();

/// Loads (or reloads) the process-wide registry. False leaves the previous
/// content in place and fills `errorOut`.
bool loadContent(const std::string& dataDir, std::string& errorOut);

} // namespace flix
