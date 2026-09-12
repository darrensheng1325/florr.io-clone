#include "shared/game/map_elements.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "shared/core/json.h"
#include "shared/game/constants.h"
#include "shared/game/terrain.h"

namespace flix {

namespace {

/// Keeps a spawn clear of the rectangle's own edge, so a player never appears
/// half inside the wall that bounds their zone.
constexpr double kSpawnPadding = 50.0;

/// How many points to try inside a zone before giving up on it. The zones are
/// large and mostly open; a zone that fails fifty times is one the map has
/// since walled over.
constexpr int kSpawnAttempts = 50;

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
bool bodyInsideWall(const Terrain& terrain, Vec2 centre, double halfSize, Realm realm) {
    const int minTx = Terrain::toTileCoord(centre.x - halfSize);
    const int maxTx = Terrain::toTileCoord(centre.x + halfSize);
    const int minTy = Terrain::toTileCoord(centre.y - halfSize);
    const int maxTy = Terrain::toTileCoord(centre.y + halfSize);
    const int cols = terrain.tileCols(realm);
    const int rows = terrain.tileRows(realm);
    for (int ty = minTy; ty <= maxTy; ++ty) {
        for (int tx = minTx; tx <= maxTx; ++tx) {
            if (tx < 0 || ty < 0 || tx >= cols || ty >= rows) continue;
            if (tileBlocks(terrain.atTile(tx, ty, realm))) return true;
        }
    }
    return false;
}

/// Somewhere a body can actually stand, as near `hint` as the map allows.
///
/// The last resort of every spawn picker below. Handing back a rectangle's
/// raw centre used to be that last resort, on the reasoning that the movement
/// step pushes a body out of a wall -- which is true of a wall FACE and not of
/// a body that started in the middle of a block. The shipped map is more than
/// half solid, so a rectangle whose fifty samples all missed is no longer an
/// exotic case and its centre is as likely to be inside a block as not.
///
/// Terrain::findOpenSpawn samples a disc around the hint and then, if that
/// fails, walks rings of tiles outward from it, so it always answers with open
/// ground when the realm has any within reach. `reach` is sized from the
/// rectangle, which keeps the answer inside the area the author drew whenever
/// that area has one open cell in it at all.
Vec2 openGroundNear(const Terrain& terrain, Realm realm, Rng& rng, const Rect& area) {
    const Vec2 centre{area.x + area.w * 0.5, area.y + area.h * 0.5};
    const double reach = std::max(area.w, area.h) * 0.5;
    return terrain.findOpenSpawn(rng, centre, reach, realm);
}

/// A colour property, as Tiled writes one. Its colour picker emits `#AARRGGBB`
/// (or `#RRGGBB`); a map may also just type a number. The alpha is dropped --
/// the buttons are opaque plates and a half-transparent one reads as broken.
bool parseColor(const Json& value, std::uint32_t& out) {
    if (value.isNumber()) {
        out = static_cast<std::uint32_t>(value.asDouble()) & 0xFFFFFFu;
        return true;
    }
    std::string text = value.asString();
    if (text.empty()) return false;
    if (text[0] == '#') text.erase(0, 1);
    if (text.size() != 6 && text.size() != 8) return false;
    std::uint32_t packed = 0;
    for (const char c : text) {
        int digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        packed = (packed << 4) | static_cast<std::uint32_t>(digit);
    }
    out = packed & 0xFFFFFFu;
    return true;
}

/// `garden_east` -> `Garden East`. What a spawn button says when the map did
/// not bother to give it a label, which is most of them.
std::string titleCase(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    bool boundary = true;
    for (const char c : id) {
        if (c == '_' || c == '-') {
            out.push_back(' ');
            boundary = true;
            continue;
        }
        out.push_back(boundary ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                               : c);
        boundary = false;
    }
    return out;
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

/// `maps/garden.tmj` -> `garden`. A map's id is its file stem, so the manifest
/// need not repeat it and a teleporter's `targetMap` reads like a file name.
std::string stemOf(const std::string& fileName) {
    const std::size_t slash = fileName.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? fileName : fileName.substr(slash + 1);
    const std::size_t dot = base.find_last_of('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

/// `Garden East!` -> `garden_east`. The inverse of titleCase(), and what a
/// door with no id of its own is called.
///
/// Tiled gives an object a NAME field and a property bag, and an author
/// filling in the label their button should say has already typed the only
/// human name the door has. Deriving an id from it beats refusing the door:
/// lower-cased, with every run of punctuation or space collapsed to one
/// underscore, so the id stays something a teleporter or a settings file can
/// hold. Empty when the label had no letters or digits at all.
std::string slugOf(const std::string& label) {
    std::string out;
    bool pending = false;
    for (const char c : label) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            if (pending && !out.empty()) out.push_back('_');
            pending = false;
            out.push_back(static_cast<char>(std::tolower(u)));
        } else {
            pending = true;
        }
    }
    return out;
}

} // namespace

std::vector<ZoneMobEntry> parseMobDistribution(const std::string& text, std::string* warningOut) {
    std::vector<ZoneMobEntry> rows;
    const auto warn = [&](const std::string& message) {
        if (warningOut == nullptr) return;
        if (!warningOut->empty()) *warningOut += "; ";
        *warningOut += message;
    };

    // Tokenised on anything that is not part of a name or a number, so commas,
    // percent signs and newlines all just separate. `garden 50% hornet 50%`,
    // `garden 50, hornet 50` and `garden 1 hornet 1` are the same distribution.
    std::size_t at = 0;
    const auto skipSeparators = [&] {
        while (at < text.size()) {
            const char c = text[at];
            const bool part = std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
                              c == '.' || c == '-';
            if (part) break;
            ++at;
        }
    };

    while (true) {
        skipSeparators();
        if (at >= text.size()) break;

        // A name: letters, digits and underscores, starting with a letter.
        if (!std::isalpha(static_cast<unsigned char>(text[at]))) {
            const std::size_t start = at;
            while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at]))) ++at;
            warn("expected a mob or group name, found \"" + text.substr(start, at - start) + "\"");
            continue;
        }
        const std::size_t nameStart = at;
        while (at < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[at])) || text[at] == '_')) {
            ++at;
        }
        const std::string name = text.substr(nameStart, at - nameStart);

        // An optional weight. Its absence means 1, so a bare name is a band of
        // nothing but that.
        double weight = 1.0;
        std::size_t lookahead = at;
        while (lookahead < text.size() &&
               (text[lookahead] == ' ' || text[lookahead] == '\t' || text[lookahead] == '=' ||
                text[lookahead] == ':')) {
            ++lookahead;
        }
        if (lookahead < text.size() &&
            (std::isdigit(static_cast<unsigned char>(text[lookahead])) || text[lookahead] == '.')) {
            std::size_t consumed = 0;
            try {
                weight = std::stod(text.substr(lookahead), &consumed);
            } catch (const std::exception&) {
                consumed = 0;
            }
            if (consumed > 0) at = lookahead + consumed;
        }
        if (!(weight > 0.0) || !std::isfinite(weight)) {
            warn("\"" + name + "\" has a weight of " + std::to_string(weight) + "; skipped");
            continue;
        }

        ZoneMobEntry row;
        row.weight = weight;
        // Whether this is a GROUP or a mob id is the spawner's question: only
        // the content registry knows what groups exist, and the map layer must
        // not depend on mobs.json to be readable.
        row.name = name;
        rows.push_back(std::move(row));
    }
    return rows;
}

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

void MapData::reset(Realm realm) {
    elements_.clear();
    playerSpawns_.clear();
    displayName_.clear();
    biome_.clear();
    defaultMobGroup_.clear();
    artFiles_.clear();
    layers_.clear();
    sourcePath_.clear();
    width_ = height_ = 0;
    realm_ = realm;
}

bool MapData::loadTiled(const std::string& path, std::string& errorOut, Realm realm) {
    reset(realm);

    TiledMap map;
    if (!map.load(path, errorOut)) return false;
    sourcePath_ = path;

    // The id is the file stem unless a caller already named this map. It is
    // needed BEFORE the defaults below, because two of them fall back to it.
    if (id_.empty()) id_ = stemOf(path);

    const Json& properties = map.properties();
    displayName_ = properties["displayName"].asString();
    // A map that never says which biome it is IS its own biome. The picker's
    // first row is biomes, so a silent map would otherwise share a nameless
    // tab with every other silent map -- and `garden.tmj` should not have to
    // write "garden" into a property bag to be the garden.
    biome_ = properties["biome"].asString();
    if (biome_.empty()) biome_ = id_;
    // And a map's default mob group is its biome unless it says otherwise:
    // mobs.json's groups are named after biomes, so the two agree by default
    // and only a map that deliberately disagrees has to say so.
    defaultMobGroup_ = properties["defaultMobGroup"].asString();
    if (defaultMobGroup_.empty()) defaultMobGroup_ = biome_;
    // There is deliberately NO map-wide difficulty property. Difficulty is a
    // band's, and a band is the only thing that spawns anything: a number for
    // "the ground no band covers" would configure nothing, because that ground
    // never grows a mob to roll it against.

    adopt(map.elements());

    artFiles_ = map.artFiles();
    layers_ = map.layers();
    width_ = map.width();
    height_ = map.height();

    // What the file resolved to, once, at load. An author who leaves every
    // property blank still gets told which biome and which mob group their map
    // ended up being, and which door ids the picker will offer -- otherwise the
    // defaults above are invisible until something spawns wrong.
    std::string doors;
    for (const MapElement* point : playerSpawns_) {
        if (!doors.empty()) doors += ", ";
        doors += point->spawnId;
        if (!point->pickable) doors += " (not pickable)";
    }
    // The bands, said out loud -- see bandSummary() for why in those words.
    const std::string zones = bandSummary();
    int regions = 0;
    for (const MapElement& element : elements_) {
        if (element.isMobRegion()) ++regions;
    }

    // stdout, not stderr: this is what the map RESOLVED to, not something
    // wrong with it. stderr stays the channel that means a map needs fixing,
    // so "no [map] lines on stderr" is still a meaningful thing to check.
    std::fprintf(stdout,
                 "[map] %s: %dx%d tiles, biome \"%s\", mobs \"%s\", %s, %d region%s, "
                 "%d art files, %d layers, doors: %s\n",
                 id_.c_str(), width_, height_, biome_.c_str(), defaultMobGroup_.c_str(),
                 zones.c_str(), regions, regions == 1 ? "" : "s",
                 static_cast<int>(artFiles_.size()), static_cast<int>(layers_.size()),
                 doors.empty() ? "none" : doors.c_str());
    return true;
}

std::string MapData::bandSummary() const {
    // A difficulty band is invisible in the art -- it is a shape on an object
    // layer with one number on it -- so an author who brushes danger onto a map
    // otherwise has nothing but the mobs that turn up to tell them whether the
    // engine read it. The range is printed with the tier each end resolves to,
    // because "difficulty 15" only means something once you know it is uncommon
    // ground.
    //
    // And a map with NO band is worth saying outright rather than as an
    // absence: bands are the only source of ambient mobs there are, so such a
    // map grows nothing at all, and an author who sees an empty world needs to
    // be told it is their data rather than the spawner.
    int bands = 0;
    double softest = 0.0;
    double hardest = 0.0;
    for (const MapElement& element : elements_) {
        if (!element.isSpawnBand()) continue;
        if (bands == 0) softest = hardest = element.difficulty;
        softest = std::min(softest, element.difficulty);
        hardest = std::max(hardest, element.difficulty);
        ++bands;
    }
    if (bands == 0) return "NO SPAWN BANDS -- no mobs will spawn on this map";

    char text[160];
    std::snprintf(text, sizeof(text), "%d band%s difficulty %g (%s)..%g (%s)", bands,
                  bands == 1 ? "" : "s", softest,
                  rarityName(dominantTierForDifficulty(softest)), hardest,
                  rarityName(dominantTierForDifficulty(hardest)));
    return text;
}

TiledCell MapData::cellAt(std::size_t layer, int tx, int ty) const {
    if (layer >= layers_.size()) return {};
    if (tx < 0 || ty < 0 || tx >= width_ || ty >= height_) return {};
    return layers_[layer].cells[static_cast<std::size_t>(ty) * static_cast<std::size_t>(width_) +
                                static_cast<std::size_t>(tx)];
}

/// The one element parser, shared by both map formats.
///
/// `array` is the bundle's MAP_ELEMENTS shape either way: the Tiled reader
/// rebuilds its objects into it rather than growing a second parser here, so
/// there is exactly one place that decides what an annotation means.
void MapData::adopt(const Json& array) {
    for (const Json& value : array.items()) {
        if (!value.isObject()) continue;
        MapElement element;
        const std::string kind = value["type"].asString();
        if (kind == "spawn") element.kind = MapElementKind::Spawn;
        else if (kind == "player_spawn") element.kind = MapElementKind::PlayerSpawn;
        else if (kind == "teleporter") element.kind = MapElementKind::Teleporter;
        else continue;   // an annotation this build has no meaning for
        element.bounds = {value["x"].asDouble(), value["y"].asDouble(), value["width"].asDouble(),
                          value["height"].asDouble()};
        // An outline, when the element has one. The bounding box is recomputed
        // from it rather than trusted: the two are written by the same tool,
        // but a hand-edited map whose box and outline disagree would put the
        // broadphase and the containment test on different shapes, and the
        // failure mode is a band that quietly spawns nothing.
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
        // A zero-sized rectangle is a mistake in a mob band or a spawn point,
        // but it is how EVERY teleporter is authored: the pad is a point, so
        // its width and height are both 0. Discarding those left the
        // annotation layer with no teleporters at all -- no dots on the
        // minimap, and no glow in the world.
        if (element.kind != MapElementKind::Teleporter &&
            (element.bounds.w <= 0 || element.bounds.h <= 0)) {
            continue;
        }

        const Json& properties = value["properties"];
        if (properties.isObject()) {
            // A `spawn` object with a `difficulty` is a BAND; one without is a
            // mob region. Tested by presence, not by value, because zero is a
            // real difficulty -- a band of commons.
            //
            // Tiled writes an int and a float property as the same JSON number,
            // so both spellings land here. Anything else -- a difficulty typed
            // as a string, most likely -- is reported rather than read as zero,
            // which would silently turn an author's hard band into a common
            // one.
            if (properties.contains("difficulty")) {
                const Json& authored = properties["difficulty"];
                if (authored.isNumber()) {
                    element.difficulty = authored.asDouble();
                    element.hasDifficulty = true;
                } else {
                    std::fprintf(stderr,
                                 "[map] a spawn object's `difficulty` is not a number; the band "
                                 "was read as a mob region\n");
                }
            }
            const std::string mobs = properties["mobs"].asString();
            if (!mobs.empty()) {
                std::string warning;
                element.mobDistribution = parseMobDistribution(mobs, &warning);
                if (!warning.empty()) {
                    std::fprintf(stderr, "[map] spawn band \"%s\": %s\n", mobs.c_str(),
                                 warning.c_str());
                }
            }

            element.spawnId = properties["spawnId"].asString();
            element.label = properties["label"].asString();
            element.backdrop = properties["backdrop"].asString();
            element.biome = properties["biome"].asString();
            if (properties.contains("pickable")) element.pickable = properties["pickable"].asBool();
            element.hasColor = parseColor(properties["color"], element.color);
            element.order = properties["order"].asDouble(0.0);

            element.targetMap = properties["targetMap"].asString();
            element.targetSpawn = properties["targetSpawn"].asString();
            const Json& destination = properties["teleportTo"];
            if (destination.isObject()) {
                element.teleportTo = {destination["x"].asDouble(), destination["y"].asDouble()};
                element.hasTeleportTo = true;
            }
        }
        elements_.push_back(std::move(element));
    }

    // Every door needs an id: it is how a teleporter aims at one, how a saved
    // preference names one, and how a join request asks for one. Three places
    // an author might have put it, in order, so a map drawn entirely with
    // Tiled's default fields still has working doors:
    //
    //   1. the `spawnId` property, or the object's NAME (tiled_map folds the
    //      name into the property before it gets here);
    //   2. a slug of the door's `label` -- if they typed "Garden" on the
    //      button, the door is `garden`;
    //   3. the MAP's id, which every map has.
    //
    // Only a map with several unnamed doors can now collide, and that shows up
    // as a duplicate id rather than as a door that silently vanished.
    for (MapElement& element : elements_) {
        if (element.kind != MapElementKind::PlayerSpawn) continue;
        if (element.spawnId.empty()) element.spawnId = slugOf(element.label);
        if (element.spawnId.empty()) element.spawnId = id_;
        if (element.spawnId.empty()) {
            std::fprintf(stderr, "[map] a player spawn rectangle has no spawnId; ignored\n");
            continue;
        }
    }
    for (const MapElement& element : elements_) {
        if (element.kind != MapElementKind::PlayerSpawn || element.spawnId.empty()) continue;
        playerSpawns_.push_back(&element);
    }
    // Stable, so `order` breaks ties by map order rather than arbitrarily: two
    // buttons an author left at the default 0 keep the order they were drawn.
    std::stable_sort(playerSpawns_.begin(), playerSpawns_.end(),
                     [](const MapElement* a, const MapElement* b) { return a->order < b->order; });
}

const MapElement* MapData::playerSpawn(const std::string& spawnId) const {
    for (const MapElement* element : playerSpawns_) {
        if (element->spawnId == spawnId) return element;
    }
    return nullptr;
}

MapData::TeleportStep MapData::stepTeleporters(Vec2 centre, double deltaSeconds, double nowMillis,
                                               TeleporterState& state) const {
    TeleportStep step;
    step.position = centre;

    const bool onCooldown = nowMillis < state.cooldownUntilMillis;
    int standingOn = -1;

    for (std::size_t i = 0; i < elements_.size(); ++i) {
        const MapElement& element = elements_[i];
        // A pad with no destination map is scenery: it draws, but it does not
        // charge and it does not pull. WorldMaps reports those at load.
        if (element.kind != MapElementKind::Teleporter || element.targetMap.empty()) continue;

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
            // The destination is in ANOTHER map, so the jump is not something
            // this class can carry out: it reports which pad fired and the
            // caller -- which holds the other maps and the terrain to place a
            // body in -- does the move. `position` is left where the suction
            // put it, and the caller overwrites it.
            step.fired = standingOn;
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
        if (bodyInsideWall(terrain, candidate, kPlayerBaseRadius, realm_)) continue;
        if (mobs && overlapsMob(*mobs, candidate, kPlayerBaseRadius)) continue;
        if (mobs && tooManyMobsNearby(*mobs, candidate)) continue;
        out = candidate;
        return true;
    }
    return false;
}

Vec2 MapData::defaultSpawn(Rng& rng, const Terrain& terrain,
                           const std::vector<MobDisc>* mobs) const {
    // The map's own front door first: the first player spawn rectangle in
    // button order is what a player who picked nothing gets, and it is
    // authored rather than inferred -- which is the whole point of the layer.
    Vec2 spawn;
    for (const MapElement* point : playerSpawns_) {
        if (findOpenPoint(*point, rng, terrain, spawn, mobs)) return spawn;
    }

    // No spawn rectangle would take anyone. A BEGINNER band is the next best
    // guess: ground the map calls safe -- a difficulty below the first one that
    // can roll a mob a fresh flower cannot fight (kDangerousGroundDifficulty)
    // -- even if nobody drew a door onto it.
    std::vector<const MapElement*> common;
    for (const MapElement& element : elements_) {
        if (!element.isSpawnBand()) continue;
        if (element.difficulty >= kDangerousGroundDifficulty) continue;
        common.push_back(&element);
    }
    for (std::size_t i = common.size(); i > 1; --i) {
        std::swap(common[i - 1], common[rng.below(static_cast<std::uint32_t>(i))]);
    }
    for (const MapElement* zone : common) {
        if (findOpenPoint(*zone, rng, terrain, spawn, mobs)) return spawn;
    }

    // Every candidate inside every door and every beginner band was solid or
    // crowded. The first door is still where the author meant people to
    // arrive, so the search starts from it -- but it starts, rather than
    // stopping there: see openGroundNear().
    if (!playerSpawns_.empty()) {
        return openGroundNear(terrain, realm_, rng, playerSpawns_.front()->bounds);
    }
    if (!common.empty()) return openGroundNear(terrain, realm_, rng, common.front()->bounds);
    const Vec2 extent = terrain.realmExtent(realm_);
    return terrain.findOpenSpawn(rng, {extent.x * 0.5, extent.y * 0.5}, 600.0, realm_);
}

bool MapData::spawnInElement(const MapElement& element, Rng& rng, const Terrain& terrain,
                             Vec2& out, const std::vector<MobDisc>* mobs) const {
    return findOpenPoint(element, rng, terrain, out, mobs);
}

bool MapData::spawnAt(const std::string& spawnId, Rng& rng, const Terrain& terrain, Vec2& out,
                      const std::vector<MobDisc>* mobs) const {
    const MapElement* point = playerSpawn(spawnId);
    if (point == nullptr) return false;
    if (findOpenPoint(*point, rng, terrain, out, mobs)) return true;
    // The rectangle exists but nothing inside it was clear. Its centre is
    // still where the author meant people to arrive, so that is where the
    // search for standable ground starts -- on a map that is mostly wall the
    // centre itself is often solid, and a body started inside a block is one
    // the movement step cannot reliably push out.
    out = openGroundNear(terrain, realm_, rng, point->bounds);
    return true;
}

// ---------------------------------------------------------------------------
// WorldMaps
// ---------------------------------------------------------------------------

void WorldMaps::adoptSingle(MapData map) {
    maps_.clear();
    if (map.id().empty()) map.setId("world");
    maps_.push_back(std::move(map));
    index();
}

void WorldMaps::adoptMaps(std::vector<MapData> maps) {
    maps_ = std::move(maps);
    for (std::size_t i = 0; i < maps_.size(); ++i) {
        if (maps_[i].id().empty()) maps_[i].setId(i == 0 ? "world" : "map_" + std::to_string(i));
    }
    index();
}

bool WorldMaps::load(const std::string& dataDir, Terrain* terrain, std::string& errorOut) {
    maps_.clear();
    spawnChoices_.clear();
    warnings_.clear();

    // The manifest, when there is one. Its ORDER is the realm order, and that
    // is why the set is not discovered by scanning the directory: a client and
    // a server that sorted the same files differently would disagree about
    // which realm is which map, and a player would arrive in the wrong world.
    std::vector<std::string> files;
    Json manifest;
    std::string manifestError;
    std::ifstream probe(dataDir + "/maps.json", std::ios::binary);
    if (probe) {
        const std::string text((std::istreambuf_iterator<char>(probe)),
                               std::istreambuf_iterator<char>());
        if (!Json::parse(text, manifest, manifestError)) {
            errorOut = dataDir + "/maps.json did not parse: " + manifestError;
            return false;
        }
        for (const Json& entry : manifest["maps"].items()) {
            files.push_back(entry.isObject() ? entry["file"].asString() : entry.asString());
        }
        if (files.empty()) {
            errorOut = dataDir + "/maps.json names no maps";
            return false;
        }
    } else {
        // The manifest is not optional. It is the only thing that says which
        // realm is which map, and a directory without one is a staging bug
        // rather than a shape to guess at -- guessing is what let a server
        // boot with a realm missing.
        errorOut = dataDir + "/maps.json is missing; it is what names the maps";
        return false;
    }

    if (static_cast<int>(files.size()) > kMaxWorldMaps) {
        errorOut = "the manifest names " + std::to_string(files.size()) + " maps; at most " +
                   std::to_string(kMaxWorldMaps) + " can be loaded at once";
        return false;
    }

    maps_.resize(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        const Realm realm = worldRealm(static_cast<int>(i));
        // A manifest entry is a file name beside the manifest; the no-manifest
        // path already handed over a full path.
        const std::string path = files[i].find('/') == std::string::npos
                                     ? dataDir + "/" + files[i]
                                     : files[i];
        if (terrain != nullptr && !terrain->loadTiledMap(path, errorOut, realm)) return false;
        maps_[i].setId(stemOf(files[i]));
        std::string annotationError;
        if (!maps_[i].loadTiled(path, annotationError, realm)) {
            // The annotation layer is optional: a map without one still has
            // walls and can be walked around. Losing it silently is what is
            // not acceptable.
            warnings_.push_back(path + ": " + annotationError +
                                "; this map has no spawns or teleporters");
        }
    }

    index();
    return true;
}

const MapData* WorldMaps::forRealm(Realm realm) const {
    const int slot = worldMapSlot(realm);
    if (slot < 0 || slot >= static_cast<int>(maps_.size())) return nullptr;
    return &maps_[static_cast<std::size_t>(slot)];
}

MapData* WorldMaps::forRealm(Realm realm) {
    const int slot = worldMapSlot(realm);
    if (slot < 0 || slot >= static_cast<int>(maps_.size())) return nullptr;
    return &maps_[static_cast<std::size_t>(slot)];
}

Realm WorldMaps::realmOfId(const std::string& mapId, bool& found) const {
    for (std::size_t i = 0; i < maps_.size(); ++i) {
        if (maps_[i].id() != mapId) continue;
        found = true;
        return worldRealm(static_cast<int>(i));
    }
    found = false;
    return Realm::Overworld;
}

void WorldMaps::index() {
    spawnChoices_.clear();
    doors_.clear();

    // A spawn id that only one map uses is offered unqualified, because that
    // is what an author types into a teleporter and what a settings file
    // written before a second map existed already holds. Ambiguous ids are
    // qualified, so `garden:entrance` and `warren:entrance` stay distinct.
    // Counted over EVERY door, pickable or not: an id is qualified by what
    // exists, not by what is offered, so a sublevel door an admin names
    // resolves to the same door a pad arrives at.
    std::vector<std::pair<std::string, int>> idCounts;
    for (const MapData& map : maps_) {
        for (const MapElement* point : map.playerSpawns()) {
            auto found = std::find_if(idCounts.begin(), idCounts.end(),
                                      [&](const std::pair<std::string, int>& entry) {
                                          return entry.first == point->spawnId;
                                      });
            if (found == idCounts.end()) idCounts.emplace_back(point->spawnId, 1);
            else ++found->second;
        }
    }

    for (std::size_t slot = 0; slot < maps_.size(); ++slot) {
        const MapData& map = maps_[slot];
        for (const MapElement* point : map.playerSpawns()) {
            const auto found = std::find_if(idCounts.begin(), idCounts.end(),
                                            [&](const std::pair<std::string, int>& entry) {
                                                return entry.first == point->spawnId;
                                            });
            const bool unique = found != idCounts.end() && found->second == 1;

            SpawnChoice choice;
            choice.id = unique ? point->spawnId : map.id() + ":" + point->spawnId;
            choice.label = point->label.empty() ? titleCase(point->spawnId) : point->label;
            choice.color = point->color;
            choice.realm = worldRealm(static_cast<int>(slot));
            choice.backdrop = point->backdrop.empty() ? point->spawnId : point->backdrop;
            // The map's own biome already defaults to its id, so a door that
            // says nothing lands in its map's tab.
            choice.biome = point->biome.empty() ? map.biome() : point->biome;
            for (std::size_t i = 0; i < map.elements().size(); ++i) {
                if (&map.elements()[i] == point) {
                    choice.element = static_cast<int>(i);
                    break;
                }
            }
            choice.pickable = point->pickable;
            // Sublevel doors are arrival points, not buttons: every door is a
            // door, only the pickable ones are choices.
            if (choice.pickable) spawnChoices_.push_back(choice);
            doors_.push_back(std::move(choice));
        }
    }

    // Teleporters are checked HERE, once, rather than when a player stands on
    // one: a pad aimed at a map nobody staged is a map bug, and the moment to
    // find out is when the server starts, not when somebody falls through it.
    for (const MapData& map : maps_) {
        for (const MapElement& element : map.elements()) {
            if (element.kind != MapElementKind::Teleporter) continue;
            const std::string where = "[" + map.id() + "] teleporter at (" +
                                      std::to_string(static_cast<long>(element.centre().x)) + ", " +
                                      std::to_string(static_cast<long>(element.centre().y)) + ")";
            if (element.targetMap.empty()) {
                warnings_.push_back(where + " names no targetMap; it leads nowhere");
                continue;
            }
            bool found = false;
            const Realm target = realmOfId(element.targetMap, found);
            if (!found) {
                warnings_.push_back(where + " leads to map \"" + element.targetMap +
                                    "\", which is not staged");
                continue;
            }
            const MapData* destination = forRealm(target);
            if (destination != nullptr && !element.targetSpawn.empty() &&
                destination->playerSpawn(element.targetSpawn) == nullptr) {
                warnings_.push_back(where + " arrives at spawn point \"" + element.targetSpawn +
                                    "\", which map \"" + element.targetMap + "\" does not define");
            }
        }
    }
}

namespace {

const SpawnChoice* findDoor(const std::vector<SpawnChoice>& list, const std::string& id) {
    for (const SpawnChoice& entry : list) {
        if (entry.id == id) return &entry;
    }
    // A bare spawn id when the choice was qualified, so a client that stored
    // `entrance` before a second map claimed the name still reaches one.
    for (const SpawnChoice& entry : list) {
        const std::size_t colon = entry.id.find(':');
        if (colon != std::string::npos && entry.id.compare(colon + 1, std::string::npos, id) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

const SpawnChoice* WorldMaps::choice(const std::string& id) const {
    return findDoor(spawnChoices_, id);
}

const SpawnChoice* WorldMaps::door(const std::string& id) const {
    return findDoor(doors_, id);
}

bool WorldMaps::resolveTeleporter(const MapElement& pad, Rng& rng, const Terrain& terrain,
                                  Destination& out, const std::vector<MobDisc>* mobs) const {
    bool found = false;
    const Realm target = realmOfId(pad.targetMap, found);
    if (!found) return false;
    const MapData* destination = forRealm(target);
    if (destination == nullptr) return false;

    out.realm = target;
    if (!pad.targetSpawn.empty() &&
        destination->spawnAt(pad.targetSpawn, rng, terrain, out.position, mobs)) {
        return true;
    }
    if (pad.hasTeleportTo) {
        // An explicit point, resolved out of whatever it landed in: the pad
        // author picked a coordinate, not a tile, and the map may have grown a
        // wall there since.
        out.position = terrain.resolveCircle(pad.teleportTo, kPlayerBaseRadius, target);
        return true;
    }
    out.position = destination->defaultSpawn(rng, terrain, mobs);
    return true;
}

} // namespace flix
