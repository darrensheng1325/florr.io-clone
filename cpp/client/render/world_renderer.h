#pragma once
// Draws the world: terrain, entities, and the effects layered over them.
//
// Reads only from the WorldView -- it never simulates. Anything that needs to
// persist between frames (a floating damage number, an explosion, a mob still
// playing its death animation) lives in a pool here, seeded from the events
// the server sent.

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "canvas.h"

#include "client/camera.h"
#include "client/render/sprites.h"
#include "client/world_view.h"
#include "shared/core/types.h"
#include "shared/game/constants.h"

namespace flix {

class ContentRegistry;
class MapData;
class Terrain;
class WorldMaps;
struct MobConfig;

/// One map cell's artwork orientation: the rotation to apply about the cell's
/// centre, and whether the result is then mirrored across its own vertical
/// axis. A canvas composes translate * rotate * scale, so this pair IS the
/// matrix R(radians) * diag(mirror ? -1 : 1, 1).
struct TileOrientation {
    double radians = 0;
    bool mirror = false;
};

/// The orientation Tiled's three flip bits name, for a cell's `flags` (the
/// kTileFlip* bits; anything above them is ignored).
///
/// Tiled defines the ORDER these compose in: the ANTI-DIAGONAL flip first -- a
/// transpose, (u,v) -> (v,u) -- then horizontal, then vertical. So the matrix
/// a cell wants is V^v * H^h * D^d, and the eight of them are the eight
/// symmetries of the square. Applying them in any other order draws three of
/// the four rotations mirrored, and an edge tile that serves all four
/// rotations of one corner is exactly where that shows.
///
/// Declared out here, rather than buried in the draw loop, so the mapping can
/// be checked against Tiled's spec on its own.
TileOrientation tileOrientation(std::uint8_t flags);

/// One piece of an explosion's debris. Velocity and life are per second here;
/// the browser build counts both per frame at 60 Hz.
struct EffectParticle {
    Vec2 position;
    Vec2 velocity;
    double lifeSeconds = 0;
    double maxLifeSeconds = 1;
    double size = 1;
    /// Radians the grain is turned by. Only the square grains show it; a disc
    /// looks the same at every angle, so theirs stays zero.
    double rotation = 0;
    std::uint32_t color = 0xFFFFFFu;
};

/// A short-lived visual with no gameplay meaning.
struct Effect {
    /// `Sparkle` is the particle-only kind: the high-rarity shimmer around a
    /// petal or a drop, and the burst a drop throws when it lands. It has no
    /// body of its own -- only its particles are drawn.
    enum class Kind : std::uint8_t { DamageNumber, Explosion, Sparkle };
    Kind kind = Kind::DamageNumber;
    /// Sparkle only: a drop's grains are hard-cornered squares, a petal's are
    /// discs. Kept per effect rather than per particle because one burst is
    /// all of one shape.
    bool squareParticles = false;
    Vec2 position;
    Vec2 drift;
    double value = 0;
    double radius = 0;
    /// World-unit type size for the number kinds: a flower reports its damage
    /// larger than a mob does.
    double textSize = 16.0;
    std::uint32_t color = 0xFFFFFFu;
    double ageSeconds = 0;
    double lifeSeconds = 1.0;
    std::vector<EffectParticle> particles;
};

/// One arm of a lightning strike: the jagged run from where the strike landed
/// to one of the mobs it hit.
///
/// The jitter is rolled ONCE, when the strike arrives, and the polyline is
/// kept. Re-rolling it per frame makes the arc crawl and shimmer instead of
/// flashing; the style reference seeds its own jitter off entity ids to get the
/// same fixed shape, which a stored path gives for free.
///
/// A bolt rather than a strike is the unit here because each arm is jittered on
/// its own. The arms of one strike are born in the same frame and so fade
/// together without having to be grouped.
struct LightningBolt {
    std::vector<Vec2> points;
    double ageSeconds = 0;
};

/// The flourish a drop plays as it lands: it slides in from a random offset
/// and unwinds a random spin over 400 ms.
struct DropSpawn {
    double angle = 0;
    double distance = 0;
    double rotation = 0;
    double ageSeconds = 0;
};

/// A drop the snapshot has already removed, kept alive locally for the 150 ms
/// its pickup flight or the 300 ms its despawn spin runs. Everything the plate
/// needs is copied: the entity record is gone by the time the removal is seen.
struct DyingDrop {
    std::uint32_t netId = 0;
    Vec2 position;
    std::uint16_t typeIndex = 0;
    Rarity rarity = Rarity::Common;
    /// Who is collecting it. Zero means nobody did: the drop timed out, and
    /// the spin-and-fade despawn plays instead of the flight.
    std::uint32_t takerNetId = 0;
    double ageSeconds = 0;
    /// Only meaningful in the live table: cleared every frame so a drop that
    /// stopped being sent can be told apart from one that is still there.
    bool seenThisFrame = false;
    /// Only meaningful in the live table: fractional grains owed by the
    /// shimmer's steady emission rate, carried between frames so the rate does
    /// not depend on how long a frame happened to be.
    double sparkleCredit = 0;
};

/// A mob the server has already destroyed, kept alive locally for the 200 ms
/// its death animation runs. Only the handful of fields the body needs are
/// copied: the snapshot record is gone by the time the event arrives.
struct DyingMob {
    std::uint32_t netId = 0;
    Vec2 position;
    double angle = 0;
    double radius = 10;
    std::uint16_t typeIndex = 0;
    Rarity rarity = Rarity::Common;
    double ageSeconds = 0;
};

class WorldRenderer {
public:
    void setContent(const ContentRegistry* content) { content_ = content; }
    void setSprites(const SpriteCache* sprites) { sprites_ = sprites; }

    /// Turns this frame's server events into effects. Drains `view.events()`.
    void ingestEvents(WorldView& view);

    /// Ages effects and retires expired ones.
    void update(double dt);

    /// What the renderer actually needs: a set of entities to draw.
    using EntityMap = std::unordered_map<std::uint32_t, RemoteEntity>;

    /// Draws the whole world. `selfDrawn` is where the viewer's own flower is
    /// being drawn this frame -- the same eased position the camera is pinned
    /// to, passed in rather than looked up so the body and the camera cannot
    /// disagree.
    void draw(Canvas&, const EntityMap&, const Camera&, Vec2 selfDrawn,
              double timeSeconds) const;

    /// The same, reading the entities out of a live view. Taking the map
    /// separately as well is what lets a tool or a replay render a scene it
    /// assembled itself, with no socket behind it.
    void draw(Canvas&, const WorldView&, const Camera&, Vec2 selfDrawn,
              double timeSeconds) const;

    /// Draws just the flower body, in the artwork's own local space (radius
    /// 25, centred on the origin, no rotation). Public because the skins menu
    /// previews a cosmetic by drawing the very same body the world does --
    /// a second, panel-only copy of each skin is exactly how the two drift.
    void drawFlowerBody(Canvas&, const RemoteEntity&, double timeSeconds) const;

    /// Terrain is optional: a client that has not yet been sent the map draws a
    /// plain biome ground rather than nothing.
    void setTerrain(const Terrain* terrain) { terrain_ = terrain; }

    /// Every world map's art and annotations: the tile layers the world is
    /// PAINTED from, the teleporters, and the spawn zones the rarity glow
    /// tints. The renderer picks the map of whichever realm the view is in,
    /// frame by frame. Optional in the same way the terrain is: without it
    /// there is nothing to paint and the realm draws as void.
    void setWorldMaps(const WorldMaps* maps) { worldMaps_ = maps; }

    /// A single map, taken as the OVERWORLD's. What a tool or a test that
    /// assembled one map in memory hands over; setWorldMaps() wins when both
    /// are set.
    void setMapData(const MapData* map) { map_ = map; }

    /// What the settings menu switches off. Presentation only: nothing here
    /// changes what the client sends, predicts, or is told.
    struct Options {
        bool names = true;
        bool healthBars = true;
        bool damageNumbers = true;
        /// Draws every body's collision circle. A debugging view, and the one
        /// honest way to see how generous a petal's reach really is.
        bool hitboxes = false;
        /// Tints every spawn zone with the tier that spawns in it. Held down
        /// rather than toggled: the browser build shows it while ALT is down.
        bool rarityGlow = false;
    };
    Options options;

    /// Cap on live effects. A crowded fight can generate hundreds of damage
    /// numbers a second, and past a point they are noise that costs frame time.
    std::size_t maxEffects = 256;

    /// What the last draw() spent in each layer, in milliseconds, for the
    /// stats overlay's "Render avg/peak" line. The browser times the same
    /// three sections inside drawGameObjects, and these are the same cuts:
    /// `mobs` runs from the top of the entity passes through the flowers and
    /// their petals (so it carries the ground-effect layer too, as the
    /// browser's does), `items` is the loot lying on the ground, and
    /// `projectiles` is the last pass. Terrain is outside all three, in both
    /// builds.
    ///
    /// Mutable because draw() is const -- it reports what it cost, it does not
    /// change what it draws. `itemCount` is how many drops survived culling,
    /// which is the number beside the items figure in the readout.
    struct SectionTiming {
        double mobsMillis = 0;
        double itemsMillis = 0;
        double projectilesMillis = 0;
        int itemCount = 0;
    };
    const SectionTiming& sectionTiming() const { return timing_; }

private:
    /// A world realm, painted from its map's tile LAYERS: every visible cell,
    /// bottom layer to top, each cell's artwork fitted to its 300-unit square
    /// and turned by the flip bits Tiled gave it. Nothing here reads the
    /// collision grid -- what a cell looks like and what it blocks are two
    /// answers to two questions, and the picture is the map file's. Outside
    /// the map, and in a cell every layer left empty, there is the black the
    /// frame was cleared to.
    void drawTerrain(Canvas&, const Camera&, Realm realm) const;
    /// The annotations of a realm: its entry in the catalogue, or the single
    /// map for the overworld when only that was handed over. Null for the
    /// arena, the maze and any realm nothing was staged for.
    const MapData* mapFor(Realm realm) const;
    /// Resolves `map`'s art file names to compiled documents, once per map.
    /// Returns an empty span when there is nothing to paint with.
    const std::vector<const SvgDocument*>& artFor(const MapData& map) const;
    /// The maze realm: its biome's flat ground colour under rrolf-style walls,
    /// every corridor junction rounded by a quarter-circle fillet, and the
    /// void beyond its square left black. The maze has no map file and so no
    /// artwork of its own -- terrain.h's kBiomes is its whole palette.
    void drawMaze(Canvas&, const Camera&) const;
    /// The arena realm: a grey gridded floor inside the ring, dark void
    /// outside it, and the red boundary the server clamps bodies to.
    void drawArena(Canvas&, const Camera&) const;
    /// Teleporters, and the spawn-zone tints while the rarity glow is held,
    /// off the realm's own map.
    void drawMapElements(Canvas&, const Camera&, Realm realm, double timeSeconds) const;
    void drawEntity(Canvas&, const RemoteEntity&, const Camera&, Vec2 at, double timeSeconds) const;
    void drawFlower(Canvas&, const RemoteEntity&, const Camera&, Vec2 at, double timeSeconds) const;
    void drawDefaultFlower(Canvas&, const RemoteEntity&, double timeSeconds) const;
    void drawPumpkin(Canvas&, const RemoteEntity&) const;
    void drawRobot(Canvas&, const RemoteEntity&) const;
    void drawHitbox(Canvas&, const RemoteEntity&, const Camera&, Vec2 at) const;
    void drawEffects(Canvas&, const Camera&) const;
    void drawLightning(Canvas&, const Camera&) const;

    /// One loot drop: the shadow backdrop, the rarity plate, the petal and the
    /// item's name, all in world units. `rotation`, `scale` and `alpha` are
    /// the spawn/pickup/despawn animation's; a drop that is just lying there
    /// still carries its resting tilt, seeded on its net id, and the shared
    /// idle pulse `timeSeconds` drives. The petal artwork itself stays frozen
    /// at frame zero, as the reference bakes it: only the plate breathes.
    void drawDrop(Canvas&, const Camera&, Vec2 at, std::uint16_t typeIndex, Rarity,
                  double rotation, double scale, double alpha, double timeSeconds) const;

    /// Everything one mob body needs that does not come from its config. The
    /// death animation replays a mob the snapshot has already dropped, so this
    /// is deliberately a value rather than a reference into the view.
    struct MobDraw {
        std::uint32_t netId = 0;
        Vec2 position;
        double angle = 0;
        /// Collision radius in world units, i.e. half the undecorated body.
        double radius = 10;
        std::uint16_t typeIndex = 0;
        Rarity rarity = Rarity::Common;
        double healthFraction = 1.0;
        /// Locked on to a player. A neutral or hostile mob animates at double
        /// speed while it is.
        bool chasing = false;
        /// Negative for a live mob; 0..1 while the death animation runs.
        double deathProgress = -1.0;
    };

    /// `clockSeconds` is the frame clock; a chasing mob's artwork is advanced
    /// at twice that rate, which is where the two names differ.
    void drawMobBody(Canvas&, const Camera&, const MobDraw&, double clockSeconds) const;
    /// Name, tier, health bar and (for a dummy) DPS, all below the body.
    void drawMobLabel(Canvas&, const Camera&, const MobDraw&) const;
    /// The digger: a grey flower carrying a spinning cutter, never its SVG.
    void drawDiggerMob(Canvas&, const MobDraw&, double radius, double timeSeconds) const;
    /// A mob that carries an orbiting ring of petals (the glitch flower).
    void drawPetalRingMob(Canvas&, const MobConfig&, const MobDraw&, double radius,
                          double timeSeconds) const;
    /// The garbage mob, whose artwork in mobs.json is an empty document: a
    /// deterministic pile of petals seeded on where it stands.
    void drawGarbagePile(Canvas&, Vec2 at, double baseSize, double timeSeconds) const;
    /// The eased eye offset a flower-shaped mob looks along. Keyed by netId so
    /// each mob eases on its own.
    Vec2 mobEye(std::uint32_t netId, double angle) const;
    /// The petal types a garbage pile may be built from: the same rule the
    /// server's drop roll uses, so both clients pick the same artwork.
    const std::vector<std::uint16_t>& droppablePetals() const;

    /// The spinning saw a worn cutter puts on the flower, in the caller's
    /// radius-25 art space. Drawn before the body so it reads as carried
    /// behind, and never as an orbiting petal -- a cutter takes no ring place.
    void drawCutterBlade(Canvas&, std::uint8_t equipFlags, double timeSeconds) const;

    /// The face, at an explicit mouth curve. A corpse is the one caller that
    /// needs a mouth the live attack/defend bits would never produce, and the
    /// digger the one that needs a body that is not flower yellow.
    void drawFace(Canvas&, std::uint8_t faceFlags, std::uint8_t equipFlags, double eyeX,
                  double eyeY, double mouth, double timeSeconds,
                  std::uint32_t bodyColor = 0xFFE763u) const;

    /// Draws `body` (centred on the origin of the canvas it is handed) through
    /// the scanline tear. Shared by the Glitch cosmetic and the glitch flower.
    void drawGlitched(Canvas&, Vec2 screen, double radius, std::uint32_t seed, double timeSeconds,
                      const std::function<void(Canvas&)>& body) const;

    /// Name, health bar and level, all below the flower. Drawn BEFORE the body
    /// so a grown flower paints over its own plate rather than under it.
    void drawPlayerPlate(Canvas&, const RemoteEntity&, const Camera&, Vec2 at,
                         double timeSeconds) const;
    void drawCorpse(Canvas&, const RemoteEntity&, const Camera&, Vec2 at, double timeSeconds) const;
    void drawPetalSprite(Canvas&, const RemoteEntity&, const Camera&, Vec2 at,
                         double timeSeconds) const;
    /// The soft disc under an emissive petal. cpp_canvas has no radial
    /// gradient, so this is the same three-stop ramp painted as nested discs.
    /// `bands` buys smoothness with one filled disc each, which is why a glow
    /// the size of the screen asks for fewer of them.
    /// `centreAlpha` and `kneeAlpha` are the ramp's first two stops. They are
    /// parameters because the teleporter's own gradient is a third the petal
    /// glow's strength and reusing the petal ramp scaled by a blanket
    /// globalAlpha lands between the two.
    void drawPetalGlow(Canvas&, double radius, std::uint32_t rgb, int bands = 16,
                       double centreAlpha = 0.6, double kneeAlpha = 0.25) const;

    /// Green, or the invulnerability yellow fading back to green over the
    /// 500 ms after a spawn shield drops.
    std::uint32_t healthBarColor(const RemoteEntity&, double timeSeconds) const;

    /// How much bigger than the 25-unit artwork this flower draws. Petal size
    /// modifiers are the only thing that moves it, and the server folds them
    /// into the replicated body radius, so it is recovered by dividing that
    /// back out by what the flower's level alone would have asked for.
    double playerSizeMultiplier(const RemoteEntity&) const;

    const ContentRegistry* content_ = nullptr;
    const SpriteCache* sprites_ = nullptr;
    const Terrain* terrain_ = nullptr;
    const WorldMaps* worldMaps_ = nullptr;
    const MapData* map_ = nullptr;
    /// The last map artFor() resolved, and its art in artFiles() order. One
    /// entry is enough: a frame draws one realm, and a realm change is rare.
    /// Null entries are files the data directory does not hold, which draw
    /// nothing.
    mutable const MapData* artMap_ = nullptr;
    mutable std::vector<const SvgDocument*> art_;
    std::vector<Effect> effects_;

    /// Lightning arms still flashing, kept apart from the effect pool for the
    /// same reason the drop shimmer is: one strike lands two dozen of them at
    /// once and would evict every damage number on screen.
    std::vector<LightningBolt> bolts_;

    /// Every grain a drop throws -- the steady shimmer and the burst it lands
    /// with -- kept apart from the effect pool. The grains have no shared
    /// birth, no shared death, and nothing to group them by: one flat pool of
    /// independent particles is what that is. Putting them in `effects_` also
    /// starved them twice over -- a single drop keeps roughly a hundred grains
    /// alive against a 256-effect pool for the whole screen, and the landing
    /// burst arrives in the same tick as a dying mob's damage numbers, which
    /// is exactly when that pool is fullest.
    ///
    /// Drawn at the head of the drop layer, not with the effects: they belong
    /// under the loot they come off.
    std::vector<EffectParticle> dropSparkles_;

    /// The drops on screen, by net id, and what each was last seen holding. A
    /// removal arrives as an absence rather than an event, so the outgoing
    /// animation has nowhere else to read the drop's plate from.
    std::unordered_map<std::uint32_t, DyingDrop> knownDrops_;
    /// Drops still sliding in, and drops the snapshot has already dropped but
    /// that are still flying to their taker or spinning out.
    std::unordered_map<std::uint32_t, DropSpawn> dropSpawns_;
    std::vector<DyingDrop> dyingDrops_;

    /// Mobs the server has destroyed, still playing their 200 ms death
    /// animation. Bounded: past the cap the oldest simply stops animating,
    /// which is invisible in the crowd that produced it.
    std::vector<DyingMob> dying_;

    /// The last state each visible mob was drawn in. A Killed event arrives
    /// AFTER the snapshot has already erased the entity, so the death
    /// animation has nowhere else to read the mob's size and artwork from.
    mutable std::unordered_map<std::uint32_t, MobDraw> mobShadows_;

    /// Eased eye offsets for the two flower-shaped mobs, keyed by netId.
    mutable std::unordered_map<std::uint32_t, Vec2> mobEyes_;

    /// Damage landed on each target dummy over the last ten seconds, which is
    /// the window the browser build's server reports DPS over.
    mutable std::unordered_map<std::uint32_t, std::deque<std::pair<double, double>>> dummyDamage_;

    /// Built on first use from the content registry; the registry is immutable
    /// after load, so one pass is enough.
    mutable std::vector<std::uint16_t> droppablePetals_;

    /// The mobs drawn this frame, drained into the label pass afterwards. In
    /// the browser build every bar is drawn after every body, so a mob drawn
    /// later can never cover an earlier one's name.
    mutable std::vector<MobDraw> mobLabels_;

    /// Advanced by update(), so the parts of the draw and ingest paths that
    /// need a clock (the invulnerability fade, the damage-number throttle) do
    /// not each invent one.
    double nowSeconds_ = 0;
    /// The clock at the previous ingest, which is the only place a frame's
    /// length is available to the shimmer's per-frame emission roll.
    double lastIngestSeconds_ = 0;

    /// netId -> the moment invulnerability ended, or -1 while it still holds.
    mutable std::unordered_map<std::uint32_t, double> invulnFade_;

    /// Which flower is the viewer's. Captured by the WorldView draw overload
    /// and used to decide whose petal ring follows the PREDICTED body rather
    /// than the interpolated one.
    mutable std::uint32_t selfNetId_ = 0;
    /// Which realm the view being drawn is in; decides what goes under the
    /// entities. Taken from the WorldView on each draw, like selfNetId_.
    mutable Realm realm_ = Realm::Overworld;

    mutable SectionTiming timing_;

    /// Mob damage numbers accumulate for 100 ms before one number is emitted,
    /// so a ring of petals landing in the same tick reads as one hit and not
    /// as a column of tiny ones. Keyed by net id with a poison bit above it:
    /// one shared bucket would let a poison tick land inside a petal hit's
    /// window and repaint the whole total purple.
    std::unordered_map<std::uint64_t, double> damageTextAt_;
    std::unordered_map<std::uint64_t, double> damagePending_;

    // A single transparent buffer is reused one flower at a time for the
    // Glitch flag. It grows with the largest on-screen flower and never needs
    // to survive a frame, so sharing avoids a canvas allocation per player.
    // The second buffer holds one chromatic-fringe copy of the first, built
    // and blitted twice per burst frame.
    mutable std::unique_ptr<Canvas> glitchBody_;
    mutable std::unique_ptr<Canvas> glitchTint_;
#ifndef __EMSCRIPTEN__
    // Only the desktop software backend needs a CPU-side tint buffer. The web
    // build makes both tints with native Canvas2D composite operations.
    mutable std::vector<std::uint8_t> glitchPixels_;
#endif
    mutable int glitchSide_ = 0;
};

} // namespace flix
