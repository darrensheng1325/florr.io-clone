#include "client/world_view.h"

#include <algorithm>
#include <cmath>

namespace flix {

void WorldView::clear() {
    entities_.clear();
    events_.clear();
    self_ = SelfState{};
    tick_ = 0;
    selfDrawnPosition_ = {};
    selfSnapPending_ = true;
    clockAnchored_ = false;
    clockOffsetMillis_ = 0;
}

void WorldView::snapAll() {
    for (auto& entry : entities_) {
        entry.second.needsSnap = true;
        entry.second.samples.clear();
    }
    selfSnapPending_ = true;
}

Vec2 WorldView::selfDrawnPosition() const {
    // Until the first snapshot has placed this flower there is nothing to ease
    // from, and the authoritative position is the honest answer.
    return selfSnapPending_ ? self_.position : selfDrawnPosition_;
}

double WorldView::toRenderClock(double serverMillis, double localMillis) {
    const double observed = serverMillis - localMillis;
    if (!clockAnchored_ || std::fabs(observed - clockOffsetMillis_) > 2000.0) {
        clockOffsetMillis_ = observed;
        clockAnchored_ = true;
    } else {
        // Slow, because this is correcting drift between two crystals, not
        // tracking the network. A fast gain would let one late packet drag the
        // whole playback timeline and undo the de-jitter it exists for.
        clockOffsetMillis_ += (observed - clockOffsetMillis_) * 0.02;
    }
    return serverMillis - clockOffsetMillis_;
}

bool WorldView::applySnapshot(ByteReader& reader) {
    const std::uint32_t tick = reader.u32();
    const double serverTime = reader.f64();
    const std::uint32_t acknowledged = reader.u32();

    const Vec2 selfPosition = reader.position();
    const Vec2 selfVelocity = reader.position();
    const double health = reader.f32();
    const double maxHealth = reader.f32();
    const double totalXp = reader.f64();
    const int level = reader.u16();
    const int stars = static_cast<int>(reader.u32());

    // Snapshots are ordered by TCP, but a reconnect can replay an older tick.
    // Applying it would rewind every entity by a frame.
    if (tick_ != 0 && tick <= tick_) return true;

    const std::uint16_t spawnCount = reader.u16();
    const std::uint16_t updateCount = reader.u16();

    // Decode into scratch first so a truncated frame leaves the view untouched
    // rather than half-applied.
    struct Spawn {
        std::uint32_t netId;
        net::EntityKind kind;
        std::uint16_t typeIndex;
        Rarity rarity;
        std::uint8_t flags;
        Vec2 position;
        double angle;
        double radius;
        double healthFraction;
        std::uint8_t state;
        std::uint8_t faceFlags;
        std::uint8_t equipFlags;
        std::uint32_t renderFlags;
        std::uint16_t level;
        Rarity bestRarity;
        std::uint32_t ownerNetId;
        std::string name;
    };
    std::vector<Spawn> spawns;
    spawns.reserve(spawnCount);
    for (std::uint16_t i = 0; i < spawnCount; ++i) {
        Spawn s{};
        s.netId = reader.u32();
        s.kind = static_cast<net::EntityKind>(reader.u8());
        s.typeIndex = reader.u16();
        s.rarity = clampRarity(reader.u8());
        s.flags = reader.u8();
        s.position = reader.position();
        s.angle = reader.angle();
        s.radius = reader.f32();
        s.healthFraction = reader.unitShort();
        s.state = reader.u8();
        s.level = 1;
        s.bestRarity = Rarity::Common;
        if (s.kind == net::EntityKind::Player) {
            s.faceFlags = reader.u8();
            s.equipFlags = reader.u8();
            s.renderFlags = reader.u32();
            s.level = reader.u16();
            s.bestRarity = clampRarity(reader.u8());
        }
        if (s.kind == net::EntityKind::Petal) s.ownerNetId = reader.u32();
        if (s.flags & net::SpawnHasName) s.name = reader.str();
        spawns.push_back(std::move(s));
        if (!reader.ok()) return false;
    }

    struct Update {
        std::uint32_t netId;
        std::uint8_t mask;
        Vec2 position;
        double angle;
        double healthFraction;
        std::uint8_t state;
        double radius;
        std::uint8_t faceFlags;
        std::uint8_t equipFlags;
        std::uint32_t renderFlags;
        std::uint16_t level;
        Rarity bestRarity;
    };
    std::vector<Update> updates;
    updates.reserve(updateCount);
    for (std::uint16_t i = 0; i < updateCount; ++i) {
        Update u{};
        u.netId = reader.u32();
        u.mask = reader.u8();
        if (u.mask & net::FieldPosition) u.position = reader.position();
        if (u.mask & net::FieldAngle) u.angle = reader.angle();
        if (u.mask & net::FieldHealth) u.healthFraction = reader.unitShort();
        if (u.mask & net::FieldState) u.state = reader.u8();
        if (u.mask & net::FieldSize) u.radius = reader.f32();
        if (u.mask & net::FieldPlayerVisuals) {
            u.faceFlags = reader.u8();
            u.equipFlags = reader.u8();
            u.renderFlags = reader.u32();
            u.level = reader.u16();
            u.bestRarity = clampRarity(reader.u8());
        }
        updates.push_back(u);
        if (!reader.ok()) return false;
    }

    const std::uint16_t removalCount = reader.u16();
    std::vector<std::uint32_t> removals;
    removals.reserve(removalCount);
    for (std::uint16_t i = 0; i < removalCount; ++i) removals.push_back(reader.u32());
    if (!reader.ok()) return false;

    const std::uint16_t eventCount = reader.u16();
    std::vector<ViewEvent> events;
    events.reserve(eventCount);
    for (std::uint16_t i = 0; i < eventCount; ++i) {
        ViewEvent e;
        e.kind = static_cast<net::EventKind>(reader.u8());
        e.netId = reader.u32();
        e.otherNetId = reader.u32();
        e.amount = reader.f32();
        e.position = reader.position();
        e.radius = reader.f32();
        e.flag = reader.u8();
        events.push_back(e);
    }
    if (!reader.ok()) return false;

    // --- commit ------------------------------------------------------------
    tick_ = tick;
    serverTimeMillis_ = serverTime;
    // Mob samples are stamped on the server's own timeline, mapped onto this
    // client's render clock. See toRenderClock().
    const double sampleMillis = toRenderClock(serverTime, renderClockMillis());

    self_.position = selfPosition;
    self_.velocity = selfVelocity;
    self_.health = health;
    self_.maxHealth = maxHealth;
    self_.totalXp = totalXp;
    self_.level = level;
    self_.stars = stars;
    self_.acknowledgedInput = acknowledged;

    for (Spawn& s : spawns) {
        RemoteEntity e;
        e.netId = s.netId;
        e.kind = s.kind;
        e.typeIndex = s.typeIndex;
        e.rarity = s.rarity;
        e.spawnFlags = s.flags;
        e.name = std::move(s.name);
        // Drawn AT the spawn position, not eased toward it: a fresh entity
        // must not slide in from wherever a recycled record left the slot.
        e.targetPosition = e.position = s.position;
        e.targetAngle = e.angle = s.angle;
        e.needsSnap = true;
        if (s.kind == net::EntityKind::Mob) {
            e.samples.reserve(kMobSampleCapacity);
            e.samples.push_back({sampleMillis, s.position});
        }
        e.radius = s.radius;
        e.healthFraction = s.healthFraction;
        e.state = s.state;
        e.faceFlags = s.faceFlags;
        e.equipFlags = s.equipFlags;
        e.renderFlags = s.renderFlags;
        e.level = s.level;
        e.bestRarity = s.bestRarity;
        e.ownerNetId = s.ownerNetId;
        if (s.flags & net::SpawnIsSelf) {
            self_.netId = s.netId;
            // A spawn record for your own flower is a join or a respawn.
            // Either way there is no continuity worth easing across.
            selfSnapPending_ = true;
        }
        entities_[s.netId] = std::move(e);
    }

    for (const Update& u : updates) {
        auto it = entities_.find(u.netId);
        // An update for an entity we never saw spawn is not an error: the
        // spawn may have been dropped by a viewport edge case. Ignoring it is
        // correct -- the next tick's diff will spawn it properly.
        if (it == entities_.end()) continue;
        RemoteEntity& e = it->second;

        // An omitted field falls back to the last AUTHORITATIVE value and
        // never to the rendered one: `position` and `angle` are mutated every
        // frame by interpolate(), so feeding them back in would have the
        // client chase its own lagging output and wobble.
        if (u.mask & net::FieldPosition) e.targetPosition = u.position;
        if (u.mask & net::FieldAngle) e.targetAngle = u.angle;
        if (e.kind == net::EntityKind::Mob) {
            // One sample per snapshot whether or not the position changed: a
            // standing mob still has to advance its timeline, or playback
            // replays the last move it made.
            if (e.samples.size() >= static_cast<std::size_t>(kMobSampleCapacity)) {
                e.samples.erase(e.samples.begin());
            }
            e.samples.push_back({sampleMillis, e.targetPosition});
        }
        if (u.mask & net::FieldHealth) e.healthFraction = u.healthFraction;
        if (u.mask & net::FieldState) e.state = u.state;
        if (u.mask & net::FieldSize) e.radius = u.radius;
        if (u.mask & net::FieldPlayerVisuals) {
            e.faceFlags = u.faceFlags;
            e.equipFlags = u.equipFlags;
            e.renderFlags = u.renderFlags;
            e.level = u.level;
            e.bestRarity = u.bestRarity;
        }
    }

    for (const std::uint32_t netId : removals) entities_.erase(netId);

    events_.insert(events_.end(), events.begin(), events.end());
    return true;
}

namespace {

/// Plays a mob back from its sample history at `renderMillis`.
///
/// Returns false when there is not enough history to bracket that instant, in
/// which case the caller falls back to the plain ease -- freezing on the
/// newest sample instead would stall every mob for the first playback delay
/// after it comes into view.
bool playBack(const std::vector<RemoteEntity::Sample>& samples, double renderMillis,
              Vec2& out) {
    if (samples.size() < 2) return false;

    // Walk back from the newest pair to the one bracketing renderMillis.
    std::size_t b = samples.size() - 1;
    while (b > 1 && samples[b - 1].timeMillis > renderMillis) --b;
    const RemoteEntity::Sample& before = samples[b - 1];
    const RemoteEntity::Sample& after = samples[b];
    const double span = after.timeMillis - before.timeMillis;
    if (!(span > 0.0)) {
        out = after.position;
        return true;
    }

    if (after.timeMillis <= renderMillis) {
        // The stream has not caught up with the render clock. Extrapolate at
        // most one sample forward, which rides out a single late packet;
        // beyond that the mob holds, because guessing further sends it
        // through walls and then snaps it back.
        const double overshoot = std::min((renderMillis - after.timeMillis) / span, 1.0);
        out = after.position + (after.position - before.position) * overshoot;
        return true;
    }

    // Clamped: renderMillis can predate the oldest pair just after the buffer
    // seeds, and a negative alpha extrapolates backwards.
    const double f = std::max(0.0, (renderMillis - before.timeMillis) / span);
    out = before.position + (after.position - before.position) * f;
    return true;
}

/// Eases `position` a fraction `t` of the way to `target`.
///
/// `cut` asks for the flower rules: a gap wider than a teleport is a portal, a
/// respawn or the maze at (200000, 200000) and must not be glided across, and
/// a gap under the settle epsilon lands exactly instead of asymptoting.
void easeToward(Vec2& position, Vec2 target, double t, bool cut) {
    const Vec2 gap = target - position;
    if (cut && (gap.lengthSq() > kTeleportSnapDistance * kTeleportSnapDistance ||
                (std::fabs(gap.x) < kSettleEpsilon && std::fabs(gap.y) < kSettleEpsilon))) {
        position = target;
        return;
    }
    position += gap * t;
}

} // namespace

void WorldView::interpolate(double nowMillis, double dtSeconds) {
    const double t = easeAmount(easeRatePerSecond, dtSeconds);
    const double renderMillis = nowMillis - interpolationDelayMillis;

    // Reapplied per frame rather than once when the command runs: the server
    // resends the real flags with every snapshot, and an override written once
    // would last until the next one and no longer.
    if (localFlags.overridesFace || localFlags.overridesEquip || localFlags.overridesRender) {
        const auto mine = entities_.find(self_.netId);
        if (mine != entities_.end()) {
            if (localFlags.overridesFace) mine->second.faceFlags = localFlags.faceFlags;
            if (localFlags.overridesEquip) mine->second.equipFlags = localFlags.equipFlags;
            if (localFlags.overridesRender) mine->second.renderFlags = localFlags.renderFlags;
        }
    }

    // --- the viewer's own flower ------------------------------------------
    //
    // First, because the camera, the cursor control law and this flower's own
    // petal ring are all anchored to it. Eased at exactly the same rate as
    // every other flower. Tracked separately from its entity record because
    // that record does not exist until its spawn arrives, and the camera needs
    // an answer before then.
    if (selfSnapPending_) {
        selfDrawnPosition_ = self_.position;
        selfSnapPending_ = false;
    } else {
        const Vec2 gap = self_.position - selfDrawnPosition_;
        if (gap.lengthSq() > kTeleportSnapDistance * kTeleportSnapDistance ||
            (std::fabs(gap.x) < kSettleEpsilon && std::fabs(gap.y) < kSettleEpsilon)) {
            selfDrawnPosition_ = self_.position;
        } else {
            selfDrawnPosition_ += gap * t;
        }
    }

    // --- everything that is not a petal -----------------------------------
    for (auto& entry : entities_) {
        RemoteEntity& e = entry.second;
        if (e.kind == net::EntityKind::Petal) continue;   // second pass, below
        const bool isFlower = e.kind == net::EntityKind::Player;

        if (e.needsSnap) {
            e.position = e.targetPosition;
            e.angle = e.targetAngle;
            e.needsSnap = false;
        } else if (e.kind == net::EntityKind::Mob && playBack(e.samples, renderMillis,
                                                              e.position)) {
            // Position came from the sample history; facing is handled below.
        } else {
            easeToward(e.position, e.targetPosition, t, isFlower);
        }

        // Facing. A flower's comes straight off the wire because it drives the
        // eyes, and easing it makes the pupils swim behind the cursor. A mob's
        // is eased instead of played back: passive AI turns up to 180 degrees
        // in one server step, and replaying that inside a single sample
        // interval reads as a snap.
        if (e.kind == net::EntityKind::Mob) {
            e.angle = lerpAngle(e.angle, e.targetAngle, t);
        } else {
            e.angle = e.targetAngle;
        }

        e.eyeX += (std::cos(e.angle) * 2.0 - e.eyeX) * 0.15;
        e.eyeY += (std::sin(e.angle) * 4.4 - e.eyeY) * 0.15;
    }

    // The viewer's flower reads the same position the camera and its ring use,
    // so nothing can disagree with it by even a fraction of a pixel.
    const auto self = entities_.find(self_.netId);
    if (self != entities_.end() && self->second.isSelf()) {
        self->second.position = selfDrawnPosition_;
    }

    // --- petals, after the flowers they hang off --------------------------
    for (auto& entry : entities_) {
        RemoteEntity& e = entry.second;
        if (e.kind != net::EntityKind::Petal) continue;

        const auto owner = entities_.find(e.ownerNetId);
        if (owner == entities_.end()) {
            // No owner on screen: nothing to anchor to, so smooth the absolute
            // position and let the ring fend for itself. Happens for the frame
            // or two before an owner's spawn record arrives.
            if (e.needsSnap) {
                e.position = e.targetPosition;
                e.needsSnap = false;
            } else {
                easeToward(e.position, e.targetPosition, t, false);
            }
            e.angle = e.targetAngle;
            continue;
        }

        // Smooth where the petal sits IN THE FLOWER'S FRAME, then put it back
        // on the flower's drawn position. See RemoteEntity::ownerOffset.
        const Vec2 targetOffset = e.targetPosition - owner->second.targetPosition;
        if (e.needsSnap) {
            e.ownerOffset = targetOffset;
            e.needsSnap = false;
        } else {
            e.ownerOffset += (targetOffset - e.ownerOffset) * t;
        }
        e.position = owner->second.position + e.ownerOffset;
        e.angle = e.targetAngle;
    }
}

} // namespace flix
