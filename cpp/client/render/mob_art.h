#pragma once
// The mobs whose picture is CODE rather than a document.
//
// Almost every sprite in the game is an inline SVG in mobs.json, compiled once
// and fitted into whatever box a call site asks for (see sprites.h). That works
// because almost every mob is ONE size: the document is drawn at the radius the
// mob happens to have and nothing about the picture depends on how big that is.
//
// A handful of mobs are not one size. A cactus spawns anywhere from 30 to 60
// units across, a rock grows a tier at a time, a sandstorm jitters between two
// bounds -- and a fitted document answers that by magnifying, which is exactly
// what it must not do. A rock twice as wide is not a rock photographed closer:
// it has MORE facets, cut the same size. A bigger cactus grows more spines, not
// longer ones. gardn draws those mobs by generating their geometry from the
// radius every frame, and this is that code, ported: the vertex counts come out
// of `radius`, the spine and the outline widths do not.
//
// The scorpion is here for the other reason -- it has no document at all. Its
// claws and legs move with a walk phase, which a static document cannot do.
//
// Every painter draws about the ORIGIN, in WORLD units, with the body's radius
// equal to the `radius` it is handed: a caller that wants it on screen scales
// and translates first, exactly as it would around a fitted document. Nothing
// here reads the clock, the camera, or the entity -- a painter is a pure
// function of the attributes it is given, which is what lets the world, the
// bestiary and a contact sheet all call it and get the same picture.

#include <cstdint>
#include <string>

#include "canvas.h"

namespace flix {

/// Which painter an `image` marker names. `None` is every ordinary mob.
enum class MobArt : std::uint8_t { None, Rock, Cactus, Sandstorm, Scorpion };

/// How fast a walk cycle runs, in radians of phase per second.
///
/// gardn advances a mob's phase by `(1 + 0.75 * speed) * 0.075` per frame; at
/// its 60 fps and at rest that is this. The world renderer already runs the
/// clock at double speed for a mob that has locked on, which is the same idea
/// as gardn's speed term arriving by a different road.
inline constexpr double kMobWalkRadiansPerSecond = 4.5;

/// The painter a mobs.json `image` field selects, or `None`.
///
/// The marker is the bare painter name behind a '$' -- `$rock`, `$cactus` --
/// the same shape of escape the sponge palette marker uses, and for the same
/// reason: `image` is the one field that says what a mob LOOKS like, so a mob
/// drawn by code should say so there rather than in a table somewhere else
/// that a new mob has to be added to twice.
MobArt mobArtFor(const std::string& image);

struct MobArtAttributes {
    /// The body's radius in world units. Both the size the picture is drawn at
    /// and the number it derives its detail from -- those are the same number
    /// on purpose, so a rock that grows a tier gains facets rather than scale.
    double radius = 1.0;
    /// The walk phase, in radians. Drives the scorpion's claws and legs and the
    /// sandstorm's spin; ignored by the mobs that hold still.
    double animation = 0.0;
    /// The mob's own colour, from its config. gardn bakes these into the
    /// painters; here they stay in mobs.json so one file still answers what a
    /// mob is coloured, whichever way its picture is made.
    std::uint32_t baseColor = 0xFFFFFFu;
};

/// Paints `art` about the origin. `None` draws nothing.
void paintMobArt(Canvas&, MobArt art, const MobArtAttributes&);

} // namespace flix
