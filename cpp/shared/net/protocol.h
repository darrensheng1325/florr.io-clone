#pragma once
// The client/server wire protocol.
//
// Framing:   every message is [u32 byteLength][u8 type][payload], little-endian.
// Direction: ClientMessage ids and ServerMessage ids are separate spaces.
//
// There are no type tags inside a payload -- both sides read the fields a
// message declares, in order. Compatibility is therefore established once, at
// connect time: the client sends kProtocolVersion together with a hash of the
// content it was built against, and a server that disagrees says so plainly
// instead of letting the two sides misread each other's bytes for a session.

#include <cstdint>

#include "shared/game/player_flags.h"
#include "shared/net/bytebuffer.h"

namespace flix::net {

/// Identifies one connected client for the lifetime of its socket.
using ConnectionId = std::uint32_t;

/// Bumped whenever any message layout in this file changes.
inline constexpr std::uint16_t kProtocolVersion = 14;

/// "Not one of the rotating store's cards": a purchase at the full ladder
/// price. Any other value is a slot index the server checks against the offers
/// it generates for itself.
inline constexpr std::uint8_t kNoShopOffer = 0xFF;

/// Frames larger than this are refused before allocation, so a bad length
/// prefix costs a dropped connection rather than a 4GB allocation.
inline constexpr std::uint32_t kMaxFrameBytes = 1u << 20;   // 1 MiB

/// Simulation rate. Fixed, because the physics integrates a constant step:
/// input, movement and cooldowns are all expressed per tick.
// The TypeScript authority advances gameplay at 30 Hz.  This is simulation
// behaviour, not a transport implementation detail: contact opportunities,
// fixed-step mob AI and every one-tick transition depend on it.
inline constexpr int kTicksPerSecond = 30;
inline constexpr double kTickSeconds = 1.0 / kTicksPerSecond;
inline constexpr double kTickMillis = 1000.0 / kTicksPerSecond;

/// How often each client is sent a snapshot.
///
/// Deliberately BELOW the tick rate. Physics and combat want 30 Hz resolution;
/// the wire does not, and the reference server says so in as many words -- the
/// snapshot broadcast runs on its own 20 Hz timer so the per-recipient
/// encode/cull/delta pass and its socket writes are not charged to the tick's
/// 33 ms budget. A client interpolating between snapshots needs its delay
/// window to cover this interval, not the tick.
inline constexpr int kSnapshotsPerSecond = 20;
inline constexpr double kSnapshotMillis = 1000.0 / kSnapshotsPerSecond;

// ---------------------------------------------------------------------------
// Message ids
// ---------------------------------------------------------------------------

enum class ClientMessage : std::uint8_t {
    Hello = 1,          ///< u16 protocolVersion, u32 contentHash
    Register,           ///< str username, str password
    Login,              ///< str username, str password
    ResumeSession,      ///< str token
    Logout,             ///< (empty)
    JoinGame,           ///< u16 viewportWidth, u16 viewportHeight, str spawnBiome,
                        ///< str playerName. An empty biome picks the beginner
                        ///< ground; an empty name spawns as "Unnamed".
    LeaveGame,          ///< (empty) -- back to the title screen, keeps the session
    Input,              ///< see InputFrame
    SetLoadout,         ///< u8 slot, u16 itemType, u8 rarity  (0xFFFF = clear)
    SwapLoadout,        ///< u8 slotA, u8 slotB
    Craft,              ///< u16 itemType, u8 rarity, u16 count -- the whole
                        ///< staging area at once, which the server crafts as
                        ///< one pool. u16 because a shift-craft stages every
                        ///< petal of a stack and a stack outgrows a byte.
    Chat,               ///< str text
    Respawn,            ///< (empty)
    Ping,               ///< u64 clientTimeMillis
    UpgradeSkill,       ///< u8 skillId, u8 tier   -- buys ONE tier, the next one
    ResetSkills,        ///< (empty) -- refunds the whole tree
    BuyPetal,           ///< u16 itemType, u8 rarity, u8 offerSlot -- price is server-side
                        ///< only. `offerSlot` names WHICH card of the current
                        ///< rotation was clicked, or kNoShopOffer for a
                        ///< full-price catalogue purchase; it is a claim about
                        ///< the card, never about the price.
    SetSkin,            ///< u32 renderFlags
    RequestLeaderboard, ///< (empty)
    RedeemCode,         ///< str code -- a star code, checked server-side
    PublishSkin,        ///< str name, u8 shapeCount, { SkinShape }*  (skin_format.h)
    EquipSkin,          ///< str skinId -- empty takes the current skin off
    DeleteSkin,         ///< str skinId -- own skin, or anyone's for an admin
    RequestNotifications, ///< u16 limit, f64 beforeMillis (0 asks for the newest
                        ///< page). The browser fetches GET /api/notifications;
                        ///< this client has no HTTP, so the same query is an
                        ///< opcode with the same two parameters.
    GuildCreate,        ///< str name -- 5 alphanumerics, upper-cased server-side
    GuildInvite,        ///< str username
    GuildAccept,        ///< (empty) -- answers the one pending invite
    GuildDecline,       ///< (empty)
    GuildKick,          ///< str username
    GuildLeave,         ///< (empty)
    GuildSquadAll,      ///< (empty)
    GuildInviteToSquad, ///< str username
};

enum class ServerMessage : std::uint8_t {
    Welcome = 1,        ///< u16 protocolVersion, u8 accepted, str reason
    AuthResult,         ///< u8 status(AuthStatus), str token, str username, str reason
    Profile,            ///< full account state: xp, level, stars, inventory, loadout,
                        ///< skins, the talent tree and the mob-kill ledger
    JoinAccepted,       ///< u32 selfNetId, f32 x, f32 y, u32 tick, u16 tileCount, u8 tiles[]
    Snapshot,           ///< see below
    Chat,               ///< u8 channel, str author, str text
    Notice,             ///< u8 severity, str text
    Died,               ///< str killerName, u32 xpLost, u32 survivedTicks
    CraftResult,        ///< u8 success, u16 itemType, u8 rarity, u16 crafted,
                        ///< u8 petalsReturned, str reason. `crafted` is how
                        ///< many upgrades the pool produced and
                        ///< `petalsReturned` the sub-batch tail (0-4) handed
                        ///< back, which is what the ring's surviving slots
                        ///< are drawn from.
    Leaderboard,        ///< u8 count, u32 totalAccounts, u32 dailyActiveUsers,
                        ///< { str name, u16 level, f64 totalXp }*.
                        ///< `dailyActiveUsers` is 0 for a non-admin, which is
                        ///< how the browser's payload omits the field.
    Pong,               ///< u64 clientTimeMillis, u64 serverTimeMillis
    Kick,               ///< str reason
    DailyStreak,        ///< u16 streak, u8 newDay, u16 starsAwarded,
                        ///< i64 nextClaimAtMillis, i64 streakExpiresAtMillis.
                        ///< Sent once per authentication, after Profile, so the
                        ///< stars it awarded are already in the profile beside it.
    ShopResult,         ///< u8 kind(ShopResultKind), u8 ok, u32 stars, str reason.
                        ///< The shop panel's own reply channel: a refusal is a
                        ///< modal on the card, not a line in the chat, so it
                        ///< cannot travel as a Notice.
    SkinCatalog,        ///< u8 isAdmin, str equippedSkinId, u16 count,
                        ///< { CustomSkin }*. Sent once per authentication: the
                        ///< studio's Browse tab lists it, and it is also what
                        ///< lets a client draw a skin somebody else is wearing.
    SkinPublished,      ///< CustomSkin -- broadcast, for the same reason
    SkinDeleted,        ///< str skinId -- broadcast
    Notifications,      ///< u8 more, u16 count,
                        ///< { str id, u8 kind(NotificationKind), str message,
                        ///<   f64 timestampMillis }*, newest first.
                        ///< `more` is set when the page filled the requested
                        ///< limit, which is the browser's `hasMore`.
    GuildUpdate,        ///< u8 joined, str name, str leader, u16 memberCount,
                        ///< { str username, u8 online }*. `joined` 0 is the
                        ///< browser's `guildUpdate null` and carries no rest.
    GuildInviteReceived, ///< str guildName, str fromUsername
    SquadUpdate,        ///< u8 inSquad, str squadId, u8 isPublic, u8 memberCount,
                        ///< { str account, str name, u32 netId, u8 flags }*.
                        ///< `inSquad` 0 is the browser's `squadUpdate null` and
                        ///< carries nothing after its flag. `flags` bit 0 marks
                        ///< the leader and bit 1 a bot; `netId` is 0 for a
                        ///< member with no body in the world just now, which is
                        ///< every member sitting on the title screen.
    DebugStats,         ///< f64 residentBytes, f64 heapBytes, f32 tickAvgMs,
                        ///< f32 tickMaxMs -- once a second, and only while
                        ///< somebody is authenticated. The browser's payload
                        ///< also carries heapTotal; nothing draws it, so it is
                        ///< not on this wire.
};

/// What a notification announces. The stripe down a card's left edge is the
/// only thing that distinguishes them, and the browser sends the same five as
/// a string tag.
enum class NotificationKind : std::uint8_t {
    Generic = 0,
    SuperCraft,
    UniqueCraft,
    ApexCraft,
    StarCode,
};

/// Which shop action a ShopResult answers.
enum class ShopResultKind : std::uint8_t { Purchase = 0, Redeem = 1 };

enum class AuthStatus : std::uint8_t {
    Ok = 0,
    BadCredentials,
    UsernameTaken,
    UsernameInvalid,
    PasswordInvalid,
    SessionExpired,
    AlreadyOnline,
    RateLimited,
    ServerError,
};

enum class ChatChannel : std::uint8_t { Global = 0, System = 1, Squad = 2 };
enum class NoticeSeverity : std::uint8_t { Info = 0, Good = 1, Warning = 2, Bad = 3 };

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

enum InputFlags : std::uint8_t {
    InputAttack  = 1 << 0,
    InputDefend  = 1 << 1,
};

/// One tick of intent from a client.
///
/// `sequence` is echoed back in the next snapshot as `lastInputSequence`,
/// which is what lets the client discard the predicted inputs the server has
/// already applied and replay only the rest.
struct InputFrame {
    std::uint32_t sequence = 0;
    double moveAngle = 0;      ///< radians; meaningless when moveStrength is 0
    double moveStrength = 0;   ///< 0..1, so an analogue stick can walk
    double aimAngle = 0;       ///< radians, where the petals point
    std::uint8_t flags = 0;

    /// The window the client is actually drawing, in world units.
    ///
    /// It rides every input frame rather than being fixed at join because the
    /// reference refreshes it from every input packet: a window resize or a
    /// zoom change widens the drawn world, and a server still culling to the
    /// join-time box would leave the new margins empty of entities that exist.
    /// Zero on either axis means "unchanged", which is what an older client
    /// that never learned to report it sends.
    std::uint16_t viewportWidth = 0;
    std::uint16_t viewportHeight = 0;

    void write(ByteWriter& w) const {
        w.u32(sequence);
        w.angle(moveAngle);
        w.unitByte(moveStrength);
        w.angle(aimAngle);
        w.u8(flags);
        w.u16(viewportWidth);
        w.u16(viewportHeight);
    }

    static InputFrame read(ByteReader& r) {
        InputFrame f;
        f.sequence = r.u32();
        f.moveAngle = r.angle();
        f.moveStrength = r.unitByte();
        f.aimAngle = r.angle();
        f.flags = r.u8();
        f.viewportWidth = r.u16();
        f.viewportHeight = r.u16();
        return f;
    }

    bool attacking() const { return (flags & InputAttack) != 0; }
    bool defending() const { return (flags & InputDefend) != 0; }
};

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

/// What a networked entity is, which decides how the client draws it.
enum class EntityKind : std::uint8_t {
    Player = 0,
    Mob,
    Petal,
    Projectile,
    Drop,        ///< a dropped petal waiting to be picked up
    Effect,      ///< a ground effect: poison cloud, web, lightning scar
};

/// Immutable per-entity facts, sent once when an entity first enters view.
///
/// A Player record additionally carries u8 face flags, u8 equipment flags,
/// u32 render flags, u16 level and u8 best-loadout rarity; a Petal record
/// carries the u32 net id of the flower it orbits, which is what lets the
/// client anchor a ring to the DRAWN owner rather than to a snapshot-old one.
enum SpawnFlags : std::uint8_t {
    SpawnHasName    = 1 << 0,   ///< a player name follows
    SpawnIsSelf     = 1 << 1,   ///< this is the viewer's own body
    SpawnIsPet      = 1 << 2,   ///< a summoned ally, drawn with an owner tint
};

/// Which mutable fields a snapshot carries for one entity this tick.
///
/// The mask exists because most entities change one or two of these per tick:
/// a drifting mob moves, a full-health petal does not, and a corpse changes
/// nothing at all. Sending the mask costs a byte and saves the rest.
enum UpdateFields : std::uint8_t {
    FieldPosition = 1 << 0,   ///< f32 x, f32 y
    FieldAngle    = 1 << 1,   ///< u16 quantised
    FieldHealth   = 1 << 2,   ///< u16 fraction of max
    FieldState    = 1 << 3,   ///< u8 EntityState bits
    FieldSize     = 1 << 4,   ///< f32 radius; changes only on level-up or growth
    /// u8 face flags, u8 equipment flags, u32 render/skin flags, u16 level,
    /// u8 best loadout rarity. Set only for players; the payload remains
    /// self-contained for decoding.
    FieldPlayerVisuals = 1 << 5,
};

/// Transient visual state, refreshed whenever FieldState is set.
enum EntityState : std::uint8_t {
    StateHurt      = 1 << 0,   ///< took damage recently; draw the white flash
    StatePoisoned  = 1 << 1,
    StateSlowed    = 1 << 2,
    StateInvulnerable = 1 << 3,
    StateDefending = 1 << 4,   ///< petals pulled in
    StateAttacking = 1 << 5,   ///< petals pushed out
    StateDead      = 1 << 6,   ///< playing its death animation
    /// A mob that has locked on. Costs no bytes -- it rides in the state byte
    /// that was already being sent -- and is what makes a chasing bug beat its
    /// wings at twice the rate an idle one does.
    StateChasing   = 1 << 7,
};

/// One-off things that happened this tick, for effects the client can play
/// without the server streaming per-frame animation state.
/// `flag` bits on a Damage event. Poison is called out because the browser
/// build shows a poison tick in its own purple, nudged sideways so it cannot
/// stack on the petal hit that landed in the same tick.
enum DamageEventFlags : std::uint8_t {
    DamageCritical = 1 << 0,
    DamagePoison   = 1 << 1,
};

enum class EventKind : std::uint8_t {
    Damage = 0,     ///< u32 netId, f32 amount, u8 DamageEventFlags -- floating number
    Heal,           ///< u32 netId, u16 amount
    PetalBroke,     ///< u32 ownerNetId, u8 slot
    Killed,         ///< u32 netId  -- pop/particles at its last position
    PickedUp,       ///< u32 dropNetId, u32 byNetId -- fly-to-player animation
    LevelUp,        ///< u32 netId, u16 newLevel
    Explosion,      ///< f32 x, f32 y, f32 radius, u8 colorIndex
};

// ---------------------------------------------------------------------------
// Content hash
// ---------------------------------------------------------------------------

/// FNV-1a over the config files both sides load.
///
/// Mob and petal stats are read from JSON at startup by BOTH the server and
/// the client (the client needs names, colours and artwork). If the two read
/// different files, every stat the client shows is quietly wrong. Hashing the
/// bytes at load and comparing them in the handshake turns that into an
/// explicit "your content is out of date" at connect time.
inline std::uint32_t contentHash(const std::string& text, std::uint32_t seed = 2166136261u) {
    std::uint32_t h = seed;
    for (const unsigned char c : text) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

} // namespace flix::net
