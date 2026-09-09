#include "shared/game/map_elements.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include "shared/core/json.h"
#include "shared/game/constants.h"
#include "shared/game/terrain.h"

namespace flix {

namespace {

/// Tiers a starting area may contain. Anything above uncommon is a place a
/// level-1 flower is killed in seconds, which is the whole test.
bool safeTier(Rarity tier) { return tier == Rarity::Common || tier == Rarity::Uncommon; }

/// Keeps a spawn clear of the rectangle's own edge, so a player never appears
/// half inside the wall that bounds their zone.
constexpr double kSpawnPadding = 50.0;

/// How many points to try inside a zone before giving up on it. The zones are
/// large and mostly open; a zone that fails fifty times is one the map has
/// since walled over.
constexpr int kSpawnAttempts = 50;

/// Section 0 is the map's top-left, which is where the beginner ground is.
constexpr int kBeginnerSection = 0;

/// A spot with more mobs than this inside kSpawnCrowdRadius is somewhere a
/// level-1 flower is surrounded the instant its invulnerability ends, so the
/// reference throws the candidate away rather than the player.
constexpr double kSpawnCrowdRadius = 200.0;
constexpr int kSpawnCrowdMaxMobs = 5;

/// True when any tile the flower's BODY would overlap is solid.
///
/// A centre-only test passes a candidate twenty units from a wall face and
/// then hands the first movement substep a body already inside it. Off-grid
/// tiles read as air here rather than as wall, because that is what the
/// reference's grid answers and a zone drawn over the map edge should not be
/// rejected for tiles that do not exist.
bool bodyInsideWall(const Terrain& terrain, Vec2 centre, double halfSize) {
    const int minTx = Terrain::toTileCoord(centre.x - halfSize);
    const int maxTx = Terrain::toTileCoord(centre.x + halfSize);
    const int minTy = Terrain::toTileCoord(centre.y - halfSize);
    const int maxTy = Terrain::toTileCoord(centre.y + halfSize);
    for (int ty = minTy; ty <= maxTy; ++ty) {
        for (int tx = minTx; tx <= maxTx; ++tx) {
            if (tx < 0 || ty < 0 || tx >= Terrain::tilesPerAxis() || ty >= Terrain::tilesPerAxis()) {
                continue;
            }
            if (tileBlocks(terrain.atTile(tx, ty))) return true;
        }
    }
    return false;
}

bool overlapsMob(const std::vector<MobDisc>& mobs, Vec2 centre, double halfSize) {
    for (const MobDisc& mob : mobs) {
        const double minDistance = halfSize + mob.radius;
        if (distanceSq(mob.position, centre) < minDistance * minDistance) return true;
    }
    return false;
}

/// True once a SIXTH mob is inside the crowd radius: the reference counts up
/// and refuses on `count > maxMobs`, so exactly five is still a legal spot,
/// and a mob sitting exactly on the radius counts.
bool tooManyMobsNearby(const std::vector<MobDisc>& mobs, Vec2 centre) {
    int count = 0;
    for (const MobDisc& mob : mobs) {
        if (distanceSq(mob.position, centre) > kSpawnCrowdRadius * kSpawnCrowdRadius) continue;
        if (++count > kSpawnCrowdMaxMobs) return true;
    }
    return false;
}

/// How far off an edge still counts as on it, in world units.
constexpr double kEdgeEpsilon = 1e-6;

/// True when `at` lies on the segment a-b.
bool onSegment(Vec2 a, Vec2 b, Vec2 at) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double cross = (at.x - a.x) * dy - (at.y - a.y) * dx;
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.0) {
        return std::fabs(at.x - a.x) <= kEdgeEpsilon && std::fabs(at.y - a.y) <= kEdgeEpsilon;
    }
    if (std::fabs(cross) > kEdgeEpsilon * length) return false;
    const double dot = (at.x - a.x) * dx + (at.y - a.y) * dy;
    return dot >= -kEdgeEpsilon && dot <= length * length + kEdgeEpsilon;
}

} // namespace

bool zoneContains(const Rect& bounds, const std::vector<Vec2>& polygon, Vec2 at) {
    // Inclusive on every edge, as the reference's own rectangle test is.
    const bool inBox = at.x >= bounds.left() && at.x <= bounds.right() &&
                       at.y >= bounds.top() && at.y <= bounds.bottom();
    if (polygon.size() < 3) return inBox;
    // The box first: this is asked of every mob on the map against every zone
    // on it, and eight ninths of those pairs are nowhere near each other.
    if (!inBox) return false;

    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec2 a = polygon[j];
        const Vec2 b = polygon[i];
        if (onSegment(a, b, at)) return true;
        if ((b.y > at.y) != (a.y > at.y) &&
            at.x < b.x + ((at.y - b.y) / (a.y - b.y)) * (a.x - b.x)) {
            inside = !inside;
        }
    }
    return inside;
}

double zoneArea(const Rect& bounds, const std::vector<Vec2>& polygon) {
    if (polygon.size() < 3) return bounds.w * bounds.h;
    double twice = 0.0;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        twice += (polygon[j].x + polygon[i].x) * (polygon[j].y - polygon[i].y);
    }
    return std::fabs(twice) * 0.5;
}

BiomeDisplay biomeDisplay(const std::string& biomeName) {
    struct Entry { const char* id; BiomeDisplay display; };
    static constexpr Entry kTable[] = {
        {"default",  {"Garden", 0x00BE4Fu}},
        {"garden",   {"Garden", 0x00BE4Fu}},
        {"desert",   {"Desert", 0xFFFF9Cu}},
        {"ocean",    {"Ocean", 0xC8FFFAu}},
        {"hel",      {"Hel", 0xFF0000u}},
        {"ant_hell", {"Ant Hell", 0xC9904Fu}},
        {"jungle",   {"Jungle", 0x00FF00u}},
        {"sewers",   {"Sewers", 0x803F02u}},
        {"computer", {"Computer Lab", 0x60FF95u}},
    };
    for (const Entry& entry : kTable) {
        if (biomeName == entry.id) return entry.display;
    }
    return {nullptr, 0xCCCCCCu};
}

bool MapData::safeForSpawn(const MapElement& element) {
    // A biome that declares no table inherits the world's tiers, which run all
    // the way to mythic. "No table" is therefore not "no dangerous mobs" -- it
    // is the opposite, and reading it the other way is how a spawn picker ends
    // up dropping people into Hel.
    if (!element.hasSpawnTable || element.spawnTable.empty()) return false;
    return std::all_of(element.spawnTable.begin(), element.spawnTable.end(),
                       [](const BiomeSpawnEntry& entry) { return safeTier(entry.tier); });
}

const MapElement* MapData::biomeAt(Vec2 at) const {
    for (const MapElement& element : elements_) {
        if (element.kind != MapElementKind::Biome) continue;
        if (element.contains(at)) return &element;
    }
    return nullptr;
}

bool MapData::load(const std::string& bundlePath, std::string& errorOut) {
    elements_.clear();
    spawnableBiomes_.clear();
    pickableBiomes_.clear();
    // The bundle has no background layer, and saying so is what makes the
    // renderer fall back to the section grid rather than paint nothing.
    background_.clear();
    groundPalette_.clear();
    backgroundWidth_ = backgroundHeight_ = 0;

    std::ifstream input(bundlePath, std::ios::binary);
    if (!input) {
        errorOut = "could not open TypeScript map bundle: " + bundlePath;
        return false;
    }
    const std::string source((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());

    // The array is plain JSON inside a TypeScript literal, so it is sliced out
    // and handed to the JSON parser rather than lexed again here.
    constexpr const char* kMarker = "export const MAP_ELEMENTS";
    const std::size_t marker = source.find(kMarker);
    if (marker == std::string::npos) {
        errorOut = "MAP_ELEMENTS is missing from " + bundlePath;
        return false;
    }
    // Past the '=' first. The declaration reads
    //   export const MAP_ELEMENTS: MapElement[] = [
    // so the first '[' after the name belongs to the TYPE, not the value, and
    // slicing from it yields an empty array that parses perfectly.
    const std::size_t assign = source.find('=', marker);
    const std::size_t begin = assign == std::string::npos ? std::string::npos
                                                          : source.find('[', assign);
    if (begin == std::string::npos) {
        errorOut = "MAP_ELEMENTS is not an array in " + bundlePath;
        return false;
    }
    // Scan for the matching bracket rather than the next "];": the elements
    // nest arrays of their own, and every one of them would end the slice early.
    int depth = 0;
    std::size_t end = std::string::npos;
    bool inString = false;
    for (std::size_t i = begin; i < source.size(); ++i) {
        const char c = source[i];
        if (inString) {
            if (c == '\\') ++i;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '[') ++depth;
        else if (c == ']' && --depth == 0) { end = i; break; }
    }
    if (end == std::string::npos) {
        errorOut = "MAP_ELEMENTS is unterminated in " + bundlePath;
        return false;
    }

    Json root;
    std::string parseError;
    if (!Json::parse(source.substr(begin, end - begin + 1), root, parseError) || !root.isArray()) {
        errorOut = "MAP_ELEMENTS did not parse: " + parseError;
        return false;
    }

    adopt(root);
    return true;
}

bool MapData::loadWorldMap(const std::string& path, std::string& errorOut) {
    return isTiledMapPath(path) ? loadTiled(path, errorOut) : load(path, errorOut);
}

bool MapData::loadTiled(const std::string& path, std::string& errorOut) {
    elements_.clear();
    spawnableBiomes_.clear();
    pickableBiomes_.clear();
    background_.clear();
    groundPalette_.clear();
    backgroundWidth_ = backgroundHeight_ = 0;

    TiledMap map;
    if (!map.load(path, errorOut)) return false;
    adopt(map.elements());
    background_ = map.background();
    groundPalette_ = map.groundPalette();
    backgroundWidth_ = map.width();
    backgroundHeight_ = map.height();
    return true;
}

int MapData::groundAt(Vec2 at) const {
    if (background_.empty()) return -1;
    const int tx = static_cast<int>(std::floor(at.x / kTileSize));
    const int ty = static_cast<int>(std::floor(at.y / kTileSize));
    if (tx < 0 || ty < 0 || tx >= backgroundWidth_ || ty >= backgroundHeight_) return -1;
    return background_[static_cast<std::size_t>(ty) * static_cast<std::size_t>(backgroundWidth_) +
                       static_cast<std::size_t>(tx)];
}

/// The one element parser, shared by both map formats.
///
/// `array` is the bundle's MAP_ELEMENTS shape either way: the Tiled reader
/// rebuilds its objects into it rather than growing a second parser here, so
/// there is exactly one place that decides what a spawn table row means.
void MapData::adopt(const Json& array) {
    for (const Json& value : array.items()) {
        if (!value.isObject()) continue;
        MapElement element;
        const std::string kind = value["type"].asString();
        if (kind == "spawn") element.kind = MapElementKind::Spawn;
        else if (kind == "biome") element.kind = MapElementKind::Biome;
        else if (kind == "teleporter") element.kind = MapElementKind::Teleporter;
        element.bounds = {value["x"].asDouble(), value["y"].asDouble(), value["width"].asDouble(),
                          value["height"].asDouble()};
        // An outline, when the element has one. The bounding box is recomputed
        // from it rather than trusted: the two are written by the same tool,
        // but a hand-edited map whose box and outline disagree would put the
        // broadphase and the containment test on different shapes, and the
        // failure mode is a zone that quietly spawns nothing.
        const Json& outline = value["polygon"];
        if (outline.isArray() && outline.size() >= 3) {
            element.polygon.reserve(outline.size());
            for (const Json& point : outline.items()) {
                element.polygon.push_back({point["x"].asDouble(), point["y"].asDouble()});
            }
            double minX = element.polygon[0].x, maxX = minX;
            double minY = element.polygon[0].y, maxY = minY;
            for (const Vec2 point : element.polygon) {
                minX = std::min(minX, point.x); maxX = std::max(maxX, point.x);
                minY = std::min(minY, point.y); maxY = std::max(maxY, point.y);
            }
            element.bounds = {minX, minY, maxX - minX, maxY - minY};
        }
        // A zero-sized rectangle is a mistake in a spawn zone or a biome, but
        // it is how EVERY teleporter in the bundle is authored: the pad is a
        // point, so its width and height are both 0. Discarding those left the
        // annotation layer with no teleporters at all -- no dots on the
        // minimap, and no glow in the world.
        if (element.kind != MapElementKind::Teleporter &&
            (element.bounds.w <= 0 || element.bounds.h <= 0)) {
            continue;
        }

        const Json& properties = value["properties"];
        if (properties.isObject()) {
            const std::string tier = properties["spawnType"].asString();
            if (!tier.empty()) {
                element.spawnTier = parseRarity(tier);
                element.hasSpawnTier = true;
            }
            element.biomeName = properties["biomeName"].asString();
            const Json& destination = properties["teleportTo"];
            if (destination.isObject()) {
                element.teleportTo = {destination["x"].asDouble(), destination["y"].asDouble()};
                element.hasTeleportTo = true;
            }
            const Json& table = properties["spawnTable"];
            if (table.isArray()) {
                element.hasSpawnTable = true;
                for (const Json& entry : table.items()) {
                    if (!entry.isObject()) continue;
                    BiomeSpawnEntry row;
                    row.tier = parseRarity(entry["tier"].asString());
                    // A row with no weight, or a nonsensical one, still takes a
                    // share: dropping it to zero would make it unreachable and
                    // silently delete whatever the map author put there.
                    const double weight = entry["weight"].asDouble(1.0);
                    row.weight = (std::isfinite(weight) && weight > 0.0) ? weight : 1.0;
                    row.mobType = entry["mobType"].asString();
                    element.spawnTable.push_back(std::move(row));
                }
            }
        }
        elements_.push_back(std::move(element));
    }

    // Two lists off one walk, in map order and without duplicates. `garden`
    // and `unnamed_biome` are the editor's names for the default ground and
    // for rectangles nobody named; neither is a destination in either list.
    //
    // The picker takes every named biome, because that is what the browser's
    // title screen offers -- it applies no tier test at all. The safety filter
    // belongs to the server's spawn logic, not to the menu: applying it here
    // too would silently delete a button the reference still draws the moment
    // a biome's first spawn table is raised above uncommon.
    for (const MapElement& element : elements_) {
        if (element.kind != MapElementKind::Biome) continue;
        if (element.biomeName.empty() || element.biomeName == "garden" ||
            element.biomeName == "unnamed_biome") {
            continue;
        }
        if (std::find(pickableBiomes_.begin(), pickableBiomes_.end(), element.biomeName) ==
            pickableBiomes_.end()) {
            pickableBiomes_.push_back(element.biomeName);
        }
        if (!safeForSpawn(element)) continue;
        if (std::find(spawnableBiomes_.begin(), spawnableBiomes_.end(), element.biomeName) ==
            spawnableBiomes_.end()) {
            spawnableBiomes_.push_back(element.biomeName);
        }
    }
}

MapData::TeleportStep MapData::stepTeleporters(Vec2 centre, double deltaSeconds, double nowMillis,
                                               TeleporterState& state) const {
    TeleportStep step;
    step.position = centre;

    const bool onCooldown = nowMillis < state.cooldownUntilMillis;
    int standingOn = -1;

    for (std::size_t i = 0; i < elements_.size(); ++i) {
        const MapElement& element = elements_[i];
        if (element.kind != MapElementKind::Teleporter || !element.hasTeleportTo) continue;

        const Vec2 offset = step.position - element.centre();
        const double distSq = offset.lengthSq();

        // Suction reads the distance from BEFORE its own pull, and the pull of
        // one pad is carried into the next pad's measurement. Both fall out of
        // the reference walking the list with a running position; between two
        // pads close enough to overlap it is the difference between being
        // dragged onto one and being held between them.
        if (distSq <= kTeleporterSuctionRadius * kTeleporterSuctionRadius && !onCooldown) {
            const double dist = std::sqrt(distSq);
            // A flower exactly on the centre has no direction to be pulled in;
            // the reference's `|| 1` keeps the division finite and the offset
            // is zero anyway.
            const double safe = dist > 0.0 ? dist : 1.0;
            const double pull =
                kTeleporterSuctionForce * (1.0 - safe / kTeleporterSuctionRadius) * deltaSeconds;
            step.position -= offset / safe * pull;
        }

        if (distSq > kTeleporterRadius * kTeleporterRadius) continue;
        standingOn = static_cast<int>(i);

        if (state.pad != standingOn) {
            state.pad = standingOn;
            state.enteredAtMillis = nowMillis;
            step.entered = standingOn;
        }
        // The cooldown blocks the jump but NOT the charge-up: a flower that
        // walks back onto the pad it arrived on still spins, it just does not
        // go anywhere until the five seconds are up.
        if (nowMillis - state.enteredAtMillis >= kTeleporterDwellMillis && !onCooldown) {
            state.cooldownUntilMillis = nowMillis + kTeleporterCooldownMillis;
            state.pad = -1;
            state.enteredAtMillis = 0;
            step.position = element.teleportTo;
            step.teleported = true;
        }
        // One pad acts per tick, even when the jump was refused: pads come in
        // pairs close enough that the far one would otherwise grab the arrival.
        break;
    }

    if (standingOn < 0 && state.pad >= 0) {
        state.pad = -1;
        state.enteredAtMillis = 0;
        step.exited = true;
    }
    return step;
}

bool MapData::findOpenPoint(const MapElement& area, Rng& rng, const Terrain& terrain, Vec2& out,
                            const std::vector<MobDisc>* mobs) const {
    const double width = area.bounds.w - kSpawnPadding * 2;
    const double height = area.bounds.h - kSpawnPadding * 2;
    if (width <= 0 || height <= 0) return false;

    for (int attempt = 0; attempt < kSpawnAttempts; ++attempt) {
        const Vec2 candidate{area.bounds.x + kSpawnPadding + rng.unit() * width,
                             area.bounds.y + kSpawnPadding + rng.unit() * height};
        // Rejection sampling over the bounding box, so the distribution stays
        // uniform over the outline rather than being biased by however a
        // triangulation happened to cut it up. The padded box is sampled and
        // the OUTLINE is tested, so a point in the corner the polygon does not
        // cover is thrown away like any other unusable candidate.
        if (!area.contains(candidate)) continue;
        // The reference's three-part safety test, in its order: the geometry
        // the body sits in, then the mob it would be sitting inside, then the
        // crowd around it.
        if (bodyInsideWall(terrain, candidate, kPlayerBaseRadius)) continue;
        if (mobs && overlapsMob(*mobs, candidate, kPlayerBaseRadius)) continue;
        if (mobs && tooManyMobsNearby(*mobs, candidate)) continue;
        out = candidate;
        return true;
    }
    return false;
}

Vec2 MapData::defaultSpawn(Rng& rng, const Terrain& terrain,
                           const std::vector<MobDisc>* mobs) const {
    // Beginner ground first. Only if the map declares none does this widen to
    // every common zone, and only if it declares none of those does it fall
    // back to the middle -- which is the behaviour this class exists to stop
    // being the normal case.
    std::vector<const MapElement*> preferred;
    std::vector<const MapElement*> anyCommon;
    for (const MapElement& element : elements_) {
        if (element.kind != MapElementKind::Spawn || !element.hasSpawnTier) continue;
        if (element.spawnTier != Rarity::Common) continue;
        anyCommon.push_back(&element);
        if (sectionAt(element.centre()) == kBeginnerSection) preferred.push_back(&element);
    }
    std::vector<const MapElement*>& zones = preferred.empty() ? anyCommon : preferred;

    // Shuffled, so a busy server does not stack every arrival in one corner of
    // one zone.
    for (std::size_t i = zones.size(); i > 1; --i) {
        std::swap(zones[i - 1], zones[rng.below(static_cast<std::uint32_t>(i))]);
    }
    Vec2 spawn;
    for (const MapElement* zone : zones) {
        if (findOpenPoint(*zone, rng, terrain, spawn, mobs)) return spawn;
    }
    if (!zones.empty()) {
        // Every candidate was solid. The zone's centre is still a better guess
        // than the middle of the map, and movement pushes a body out of a wall.
        return zones.front()->centre();
    }
    return terrain.findOpenSpawn(rng, {kWorldHalf, kWorldHalf}, 600.0, Realm::Overworld);
}

bool MapData::spawnInElement(const MapElement& element, Rng& rng, const Terrain& terrain,
                             Vec2& out, const std::vector<MobDisc>* mobs) const {
    return findOpenPoint(element, rng, terrain, out, mobs);
}

bool MapData::spawnInBiome(const std::string& biomeName, Rng& rng, const Terrain& terrain,
                           Vec2& out, const std::vector<MobDisc>* mobs) const {
    std::vector<const MapElement*> areas;
    for (const MapElement& element : elements_) {
        if (element.kind != MapElementKind::Biome || element.biomeName != biomeName) continue;
        if (!safeForSpawn(element)) continue;
        areas.push_back(&element);
    }
    if (areas.empty()) return false;

    for (std::size_t i = areas.size(); i > 1; --i) {
        std::swap(areas[i - 1], areas[rng.below(static_cast<std::uint32_t>(i))]);
    }
    for (const MapElement* area : areas) {
        if (findOpenPoint(*area, rng, terrain, out, mobs)) return true;
    }
    out = areas.front()->centre();
    return true;
}

} // namespace flix
