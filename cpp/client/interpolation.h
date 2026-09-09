#pragma once
// How the client turns a 20 Hz snapshot stream into display-rate motion.
//
// This is a port of the browser build's model (src/ecs/client/interpolation.ts),
// which is itself gardn's. The two rules that matter, and why:
//
//  * FLOWERS ARE NEVER PREDICTED. Every flower -- the viewer's included --
//    eases toward the authoritative position at one fixed, frame-rate
//    independent rate. The client runs no movement simulation of its own, so
//    there is nothing to reconcile and nothing to snap back.
//
//    This client used to predict locally and reconcile on every snapshot, and
//    it is exactly the model the browser build removed. Two things make it
//    jitter badly here. The prediction integrated velocity with no terrain
//    collision at all, so walking into a wall meant predicting straight
//    through it and being yanked back on every snapshot. And the camera is
//    pinned to the flower, so a correction of any size does not nudge the
//    flower -- it jolts the whole world.
//
//    The cost is input latency of about half a round trip plus the ease time
//    constant. That is the trade the browser build makes on purpose.
//
//  * MOBS ARE PLAYED BACK ON A DELAY, flowers are not. A mob carries a short
//    sample history and is rendered `kMobRenderDelayMillis` behind the render
//    clock, which absorbs jitter and packet loss. Flowers get no buffer,
//    because a buffered remote flower visibly lags the local one and the whole
//    point of the shared ease is that every flower moves alike.
//
// Facing follows the same split. A flower's angle comes straight off the wire
// -- it drives the eyes, and easing it makes the pupils swim. A mob's angle is
// eased rather than interpolated from the history: passive AI turns up to 180
// degrees in one server step, and interpolating that whips the mob through the
// whole turn inside a single sample interval.

#include <chrono>
#include <cmath>

#include "shared/core/types.h"

namespace flix {

/// The client's render clock, in milliseconds since an arbitrary fixed epoch.
///
/// There is exactly ONE of these on purpose. Snapshot arrivals are stamped
/// against it and mob playback is measured against it, and the two used to
/// come from different clocks -- arrivals from a steady_clock zeroed on the
/// first snapshot, playback from SDL's counter zeroed at window creation.
/// Nothing about that is visible in either expression; the difference is a
/// constant of however many seconds passed between the two, which pushed the
/// playback point permanently outside the sample window. Every mob then
/// rendered at a clamped endpoint, which is to say it held still and jumped.
inline double renderClockMillis() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

/// The fraction of the remaining gap an ease closes per frame at 60 fps. The
/// browser build's default (`localStorage.interpolationAmount`), and the
/// settings panel's slider is the same number.
inline constexpr double kDefaultInterpolationAmount = 0.15;

/// Beyond this gap an ease is a glide across the map rather than a smoothing.
/// Respawns, portals and a maze rotation all produce it.
inline constexpr double kTeleportSnapDistance = 600.0;

/// Below this the ease is invisible; settle exactly instead of asymptoting.
inline constexpr double kSettleEpsilon = 0.01;

/// How far behind the render clock buffered mobs are played back.
inline constexpr double kMobRenderDelayMillis = 80.0;

/// Samples kept per mob. At 20 Hz this is half a second of history -- more
/// than the playback delay needs, enough to ride out a burst of late packets.
inline constexpr int kMobSampleCapacity = 10;

/// `amount` (the per-frame fraction at 60 fps) as a rate per second. Same
/// shape as gardn's `Ui::lerp_amount = 1 - (1 - k)^(dt*60)`.
inline double easeRateFromAmount(double amount) {
    const double k = clamp(amount, 0.001, 0.999);
    return -std::log(1.0 - k) * 60.0;
}

/// The fraction of the gap to close this frame. Frame-rate independent, so a
/// 30 fps client and a 144 fps one reach the target on the same wall clock.
///
/// `dt` is clamped because a stalled frame -- a resize, a breakpoint, a window
/// the compositor stopped scheduling -- otherwise resolves to a full-strength
/// snap on the frame it resumes.
inline double easeAmount(double ratePerSecond, double dtSeconds) {
    const double dt = clamp(dtSeconds, 0.0, 0.1);
    return 1.0 - std::exp(-ratePerSecond * dt);
}

} // namespace flix
