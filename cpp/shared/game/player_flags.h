#pragma once
// Player-only visual flags shared by simulation, snapshots and rendering.
//
// Keep these values in lockstep with src/player.ts.  They are deliberately
// separate from the generic EntityState bits: EntityState is transient
// simulation state, while these describe the flower body and its cosmetics.

#include <cstdint>

namespace flix {

enum FaceFlag : std::uint8_t {
    FaceNone          = 0,
    FacePoisoned      = 1 << 0,
    FaceDandelioned   = 1 << 1,
    FaceDeadEyes      = 1 << 2,
    FaceSquareEyes    = 1 << 3,
    FaceAttacking     = 1 << 4,
    FaceDefending     = 1 << 5,
    FaceHasCorruption = 1 << 6,
};

enum EquipFlag : std::uint8_t {
    EquipNone     = 0,
    EquipCutter   = 1 << 0,
    EquipThirdEye = 1 << 1,
    EquipObserver = 1 << 2,
    EquipAntennae = 1 << 3,
    EquipTest1    = 1 << 4,
    /// Set alongside EquipCutter for the lightning cutter, so the client can
    /// paint its cyan blade. Derived in the content loader from the petal id --
    /// see parsePetal() for why it is not written in petals.json.
    EquipLightningCutter = 1 << 5,
};

enum PlayerRenderFlag : std::uint32_t {
    PlayerRenderNone    = 0,
    PlayerRenderPumpkin = 1 << 0,
    PlayerRenderRobot   = 1 << 1,
    PlayerRenderGlitch  = 1 << 2,
};

} // namespace flix
