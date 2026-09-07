#pragma once
// The client's mirror of the server's world.
//
// Deliberately NOT an ECS. The client does not simulate these entities -- it
// stores what the last snapshot said and interpolates between snapshots for
// rendering. A flat record per entity is the honest shape for that, and it
// keeps the draw loop free of component lookups.

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "client/interpolation.h"
#include "shared/core/types.h"
#include "shared/game/constants.h"
#include "shared/game/rarity.h"
#include "shared/net/bytebuffer.h"
#include "shared/net/protocol.h"

namespace flix {

/// One entity as the client knows it.
struct RemoteEntity {
    std::uint32_t netId = 0;
    net::EntityKind kind = net::EntityKind::Mob;
    std::uint16_t typeIndex = 0;
    Rarity rarity = Rarity::Common;
    std::uint8_t spawnFlags = 0;
    std::string name;

    /// The last authoritative position and facing off the wire. Never drawn
    /// directly -- `position` chases this one, and writing the wire value
    /// straight into `position` is what makes an entity stutter at the tick
    /// rate.
    Vec2 targetPosition;
    double targetAngle = 0;

    /// What the renderer reads: the DRAWN position and facing.
    Vec2 position;
    double angle = 0;

    /// Mob-only position history, stamped on the shared server timeline (see
    /// WorldView::clockOffsetMillis_). Flowers deliberately have none: they
    /// all ease at one rate so the viewer's flower and everyone else's move
    /// alike, and a buffer on one side of that breaks it.
    struct Sample {
        double timeMillis = 0;
        Vec2 position;
    };
    std::vector<Sample> samples;

    /// Cut to the target on the next interpolate() instead of easing to it.
    /// Set for a fresh spawn, where there is no previous position to come
    /// from, and after a respawn or teleport.
    bool needsSnap = true;

    double healthFraction = 1;
    double radius = 10;
    std::uint8_t state = 0;

    /// The three flag families used only by the player sprite. They arrive for
    /// player records in a snapshot and remain zero for all other entity kinds.
    std::uint8_t faceFlags = FaceNone;
    std::uint8_t equipFlags = EquipNone;
    std::uint32_t renderFlags = PlayerRenderNone;

    /// Player-only: what the plate under the flower reads. `bestRarity` is the
    /// highest rarity anywhere in that flower's loadout, which is what tints
    /// the level label.
    std::uint16_t level = 1;
    Rarity bestRarity = Rarity::Common;

    /// The guild whose tag hangs under this flower's health bar. Nothing sets
    /// it yet: there is no guild protocol, exactly as menu_guild.cpp records,
    /// so the tag draws for nobody -- which is what the reference draws for a
    /// player in no guild. The plate reads the field rather than the transport
    /// so the wire lands in one place when it exists.
    std::string guildName;

    /// Petal-only: the flower this petal orbits, and the petal's DRAWN offset
    /// from it.
    ///
    /// The server places petals on a ring around the owner's tick position
    /// while the owner is drawn at its eased one, so a petal's absolute
    /// position is not directly drawable. What is smoothed is this OFFSET --
    /// the petal's place in the flower's own frame -- and the petal is then
    /// drawn at the owner's drawn position plus it. The ring is therefore
    /// rigidly centred on the flower by construction, whatever the flower is
    /// doing, and only the ring's own rotation is being interpolated.
    ///
    /// Easing the absolute position instead leaves the ring centred only to
    /// the extent that the petal's ease lag happens to equal the flower's, and
    /// they differ by the orbital velocity. Subtracting the owner's raw
    /// position from it -- the obvious repair -- is worse still: that value
    /// stair-steps at the snapshot rate, so the correction is a sawtooth and
    /// every petal visibly shakes at the beat between snapshot and frame rate.
    std::uint32_t ownerNetId = 0;
    Vec2 ownerOffset;

    /// Smoothed eye-pupil offset in the flower's radius=25 local space.
    double eyeX = 0;
    double eyeY = 0;

    bool isSelf() const { return (spawnFlags & net::SpawnIsSelf) != 0; }
    bool dead() const { return (state & net::StateDead) != 0; }
};

/// A transient effect the server reported.
struct ViewEvent {
    net::EventKind kind = net::EventKind::Damage;
    std::uint32_t netId = 0;
    std::uint32_t otherNetId = 0;
    double amount = 0;
    Vec2 position;
    double radius = 0;
    std::uint8_t flag = 0;
};

/// Every slot untouched, which is what a bar with no snapshot behind it yet
/// has to draw as: a zeroed array would open the game on ten dead-looking
/// tiles.
inline std::array<double, kLoadoutActiveSlots> fullSlotHealth() {
    std::array<double, kLoadoutActiveSlots> full{};
    full.fill(1.0);
    return full;
}

/// The authoritative state of the player's own flower, straight from the
/// snapshot and never interpolated -- prediction owns its position.
struct SelfState {
    std::uint32_t netId = 0;
    Vec2 position;
    Vec2 velocity;
    double health = 0;
    double maxHealth = 0;
    double totalXp = 0;
    int level = 1;
    int stars = 0;
    std::uint32_t acknowledgedInput = 0;

    /// How much of each orbiting slot's reload is still to run, in
    /// milliseconds; 0 for a slot that is ready or empty. The loadout bar
    /// sweeps its wedge with it.
    ///
    /// The snapshot rewrites this outright, and `interpolate` counts it down
    /// between snapshots -- twenty of those a second is a visibly stepped
    /// sweep on its own, and the wedge turns through five whole turns.
    std::array<double, kLoadoutActiveSlots> slotReloadRemainingMillis{};

    /// How much of each orbiting slot's petal is still standing, 1.0 for
    /// untouched. The bar drains its tile by it. Unlike the reload it does not
    /// run on the frame clock: a petal loses health in the discrete steps
    /// something hits it in, and there is nothing between them to animate.
    std::array<double, kLoadoutActiveSlots> slotHealthFraction = fullSlotHealth();
};

class WorldView {
public:
    /// Consumes a Snapshot payload. The message id must already be read.
    /// Returns false when the frame was truncated, in which case nothing is
    /// applied -- a half-read snapshot is worse than a skipped one.
    bool applySnapshot(ByteReader& reader);

    /// Advances every drawn position and facing by one frame. Call once per
    /// rendered frame, not once per snapshot: this is what decouples a 144 Hz
    /// display from a 20 Hz snapshot stream.
    ///
    /// `nowMillis` is the client render clock, which is also the clock mob
    /// playback runs behind. `dtSeconds` is the frame delta the eases use.
    void interpolate(double nowMillis, double dtSeconds);

    /// Cuts every entity onto its authoritative position on the next
    /// interpolate(). For joining and respawning, where there is no continuity
    /// to preserve. Ordinary teleports need no call: an ease over
    /// kTeleportSnapDistance cuts on its own.
    void snapAll();

    /// Where the viewer's own flower is being DRAWN this frame.
    ///
    /// The camera pins to this, the cursor control law measures from it, and
    /// the petal ring is anchored to it. Before the first spawn record it is
    /// the raw authoritative position, which is the only thing there is.
    Vec2 selfDrawnPosition() const;

    void clear();

    /// Inserts an entity outright, for tests and for tools that assemble a
    /// scene with no socket behind it. Nothing on the wire path uses it.
    void seedForTest(const RemoteEntity& entity) { entities_[entity.netId] = entity; }

    /// Moves an existing entity's authoritative position, leaving the
    /// interpolation state alone -- what a snapshot does, without needing one.
    /// seedForTest() cannot serve: it replaces the record, ease state included.
    void setTargetForTest(std::uint32_t netId, Vec2 position) {
        const auto it = entities_.find(netId);
        if (it != entities_.end()) it->second.targetPosition = position;
    }

    const std::unordered_map<std::uint32_t, RemoteEntity>& entities() const { return entities_; }
    const SelfState& self() const { return self_; }
    std::uint32_t tick() const { return tick_; }
    double serverTimeMillis() const { return serverTimeMillis_; }

    /// Events from the most recent snapshot. Drained by the effects layer.
    std::vector<ViewEvent>& events() { return events_; }

    /// Client-only overrides for the local flower's flag families.
    ///
    /// `/forcelocalplayerflags` is a rendering probe: it makes THIS client draw
    /// its own flower with a chosen face, equipment or skin so the sprite can
    /// be checked without a server that would grant any of it. Reapplied after
    /// every snapshot, because the server keeps sending the real values and
    /// would otherwise take the override back a fifth of a second later.
    struct LocalFlagOverride {
        bool overridesFace = false;
        bool overridesEquip = false;
        bool overridesRender = false;
        std::uint8_t faceFlags = FaceNone;
        std::uint8_t equipFlags = EquipNone;
        std::uint32_t renderFlags = PlayerRenderNone;
    };
    LocalFlagOverride localFlags;

    /// The ease rate every flower, petal, drop and projectile closes its gap
    /// at, and the rate mob facing turns at. Driven by the settings panel's
    /// Interpolation slider; see easeRateFromAmount().
    double easeRatePerSecond = easeRateFromAmount(kDefaultInterpolationAmount);

    /// How far behind the render clock buffered mobs are played back.
    double interpolationDelayMillis = kMobRenderDelayMillis;

private:
    /// Maps a server timestamp onto the local render clock.
    ///
    /// Mob samples MUST be stamped with the server's own tick time and not
    /// with their arrival time. Arrival stamping compresses the timeline under
    /// bursty TCP -- four snapshots 33 ms apart on the server can land 0, 0, 0
    /// and 100 ms apart here -- and playback then stutters through them. The
    /// offset is a slow EWMA because the two clocks drift; a large divergence
    /// (a reconnect, a suspended laptop) re-anchors outright rather than
    /// crawling back over a minute.
    double toRenderClock(double serverMillis, double localMillis);

    std::unordered_map<std::uint32_t, RemoteEntity> entities_;
    std::vector<ViewEvent> events_;
    SelfState self_;
    std::uint32_t tick_ = 0;
    double serverTimeMillis_ = 0;
    /// Drawn position of the viewer's flower. Held here rather than looked up
    /// on the self entity because the self entity does not exist until its
    /// spawn record arrives, and the camera needs an answer before then.
    Vec2 selfDrawnPosition_;
    bool selfSnapPending_ = true;
    double clockOffsetMillis_ = 0;
    bool clockAnchored_ = false;
};

} // namespace flix
