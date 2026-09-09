#include "shared/game/tiled_map.h"

#include <cstdio>
#include <fstream>
#include <iterator>

#include "shared/game/constants.h"

namespace flix {

namespace {

/// The object layers, in the order their objects are concatenated.
///
/// Grouping the annotations by kind is what gives the editor three layers it
/// can show and hide separately, and it is invisible to the game: every reader
/// of elements() filters by kind before it looks at order, so only the order
/// WITHIN a kind is observable, and that is the order they appear in here.
constexpr struct { const char* layer; const char* kind; } kObjectLayers[] = {
    {"spawns", "spawn"},
    {"biomes", "biome"},
    {"teleporters", "teleporter"},
};

/// The file name half of a path, for a tileset tile's `image`.
std::string fileNameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string directoryOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

/// Resolves a tileset reference the way Tiled does: relative to the file that
/// names it, and absolute paths left alone.
std::string resolveRelative(const std::string& base, const std::string& reference) {
    if (!reference.empty() && (reference[0] == '/' || reference[0] == '\\')) return reference;
    return directoryOf(base) + "/" + reference;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    out.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return true;
}

bool parseJsonFile(const std::string& path, Json& out, std::string& errorOut) {
    std::string text;
    if (!readFile(path, text)) {
        errorOut = "could not open " + path;
        return false;
    }
    std::string parseError;
    if (!Json::parse(text, out, parseError)) {
        errorOut = path + " did not parse: " + parseError;
        return false;
    }
    return true;
}

bool decodeBase64(const std::string& encoded, std::vector<std::uint8_t>& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const int digit = value(c);
        if (digit < 0) return false;
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xffu));
        }
    }
    return true;
}

/// Tiled's typed property list, flattened to the name -> value map the rest of
/// this file wants. Absent properties are simply absent, so a caller reads them
/// through Json's null-returning lookup and gets its own default.
Json propertiesOf(const Json& node) {
    Json out = Json::object();
    const Json& list = node["properties"];
    if (!list.isArray()) return out;
    for (const Json& entry : list.items()) {
        if (!entry.isObject()) continue;
        const std::string name = entry["name"].asString();
        if (!name.empty()) out.set(name, entry["value"]);
    }
    return out;
}

/// One tile layer's cells, as raw gids.
///
/// Tiled writes a layer as a plain array of numbers or as base64. Compressed
/// base64 is deliberately refused rather than supported: adding zlib to the
/// shared library for a map file would put a decompressor in the wasm build
/// too, and Tiled's layer format is a per-map setting the author can change.
bool decodeLayer(const Json& layer, const std::string& path, std::vector<std::uint32_t>& out,
                 std::string& errorOut) {
    const std::string name = layer["name"].asString();
    const Json& data = layer["data"];
    if (data.isArray()) {
        out.reserve(data.size());
        for (const Json& cell : data.items()) out.push_back(static_cast<std::uint32_t>(cell.asInt64()));
        return true;
    }
    if (!data.isString()) {
        errorOut = path + ": layer \"" + name + "\" has no data";
        return false;
    }
    if (layer["encoding"].asString() != "base64") {
        errorOut = path + ": layer \"" + name + "\" uses an unsupported encoding";
        return false;
    }
    const std::string compression = layer["compression"].asString();
    if (!compression.empty()) {
        errorOut = path + ": layer \"" + name + "\" is " + compression +
                   "-compressed; save the map with CSV or uncompressed layer data";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!decodeBase64(data.stringRef(), bytes) || bytes.size() % 4 != 0) {
        errorOut = path + ": layer \"" + name + "\" is not valid base64";
        return false;
    }
    out.reserve(bytes.size() / 4);
    for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
        out.push_back(static_cast<std::uint32_t>(bytes[i]) |
                      (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                      (static_cast<std::uint32_t>(bytes[i + 2]) << 16) |
                      (static_cast<std::uint32_t>(bytes[i + 3]) << 24));
    }
    return true;
}

/// Resolves one gid through a tileset's gid map. Tiled packs its flip flags
/// into the top three bits; nothing here flips a tile, but a stray flag from a
/// drag in the editor would otherwise read as a wildly out-of-range id.
bool resolveGid(const std::vector<std::pair<int, int>>& map, std::uint32_t raw, int& out) {
    const int gid = static_cast<int>(raw & 0x1FFFFFFFu);
    for (const auto& entry : map) {
        if (entry.first == gid) { out = entry.second; return true; }
    }
    out = gid;   // for the caller's error message
    return false;
}

/// Tiled 1.9 renamed an object's `type` to `class` and still reads both.
std::string classOf(const Json& node) {
    const std::string name = node["class"].asString();
    return name.empty() ? node["type"].asString() : name;
}

} // namespace

bool TiledMap::load(const std::string& path, std::string& errorOut) {
    tiles_.clear();
    background_.clear();
    palette_.clear();
    groundPalette_.clear();
    elements_ = Json::array();
    width_ = height_ = 0;

    Json map;
    if (!parseJsonFile(path, map, errorOut)) return false;
    if (map["type"].asString() != "map") {
        errorOut = path + " is not a Tiled map";
        return false;
    }
    if (map["infinite"].asBool()) {
        errorOut = path + " is an infinite map; the game's grid is fixed";
        return false;
    }
    const std::string orientation = map["orientation"].asString();
    if (orientation != "orthogonal") {
        errorOut = path + " is " + orientation + "; the game's grid is orthogonal";
        return false;
    }
    // One Tiled pixel is one world unit. See the header.
    const double tileWidth = map["tilewidth"].asDouble();
    const double tileHeight = map["tileheight"].asDouble();
    if (tileWidth != kTileSize || tileHeight != kTileSize) {
        errorOut = path + " has " + std::to_string(static_cast<int>(tileWidth)) + "x" +
                   std::to_string(static_cast<int>(tileHeight)) + " tiles; the game's are " +
                   std::to_string(static_cast<int>(kTileSize)) + " square";
        return false;
    }

    // -- palette ------------------------------------------------------------
    // Tiled's global tile ids (gids) are per-map and depend on tileset order,
    // so the game's tile id is read off each tile as a property rather than
    // inferred. gid 0 is Tiled's empty cell, which is walkable ground here.
    std::vector<std::pair<int, int>> tileIdOfGid;
    tileIdOfGid.emplace_back(0, 0);
    std::vector<std::pair<int, int>> groundIdOfGid;
    groundIdOfGid.emplace_back(0, -1);   // an empty background cell is bare void
    for (const Json& reference : map["tilesets"].items()) {
        if (!reference.isObject()) continue;
        const int firstGid = reference["firstgid"].asInt();
        const std::string source = reference["source"].asString();
        Json tileset;
        if (source.empty()) {
            tileset = reference;                     // embedded in the map
        } else if (!parseJsonFile(resolveRelative(path, source), tileset, errorOut)) {
            return false;
        }
        for (const Json& tile : tileset["tiles"].items()) {
            if (!tile.isObject()) continue;
            const int localId = tile["id"].asInt();
            const Json properties = propertiesOf(tile);
            if (properties.contains("groundId")) {
                const int groundId = properties["groundId"].asInt();
                if (groundId < 0 || groundId > 127) {
                    errorOut = path + ": ground tile \"" + classOf(tile) + "\" declares ground id " +
                               std::to_string(groundId) + ", which is out of range";
                    return false;
                }
                groundIdOfGid.emplace_back(firstGid + localId, groundId);
                TiledGroundType ground;
                ground.id = groundId;
                ground.name = classOf(tile);
                ground.art = fileNameOf(tile["image"].asString());
                groundPalette_.push_back(std::move(ground));
                continue;
            }
            const int tileId = properties.contains("tileId") ? properties["tileId"].asInt() : localId;
            if (tileId < 0 || tileId > 255) {
                errorOut = path + ": tile \"" + classOf(tile) + "\" declares tile id " +
                           std::to_string(tileId) + ", which is out of range";
                return false;
            }
            tileIdOfGid.emplace_back(firstGid + localId, tileId);
            TiledTileType entry;
            entry.id = tileId;
            entry.name = classOf(tile);
            entry.solid = properties["solid"].asBool();
            entry.water = properties["water"].asBool();
            palette_.push_back(std::move(entry));
        }
    }

    // -- tile grid ----------------------------------------------------------
    // Named, not positional: the map has two tile layers now, and taking the
    // first one would make the ground the collision grid the day someone
    // reorders them in Tiled. The unnamed fallback is for a map written before
    // the background layer existed.
    const Json* terrain = nullptr;
    const Json* backgroundLayer = nullptr;
    const Json* firstTileLayer = nullptr;
    for (const Json& layer : map["layers"].items()) {
        if (!layer.isObject() || layer["type"].asString() != "tilelayer") continue;
        if (firstTileLayer == nullptr) firstTileLayer = &layer;
        const std::string name = layer["name"].asString();
        if (name == "terrain") terrain = &layer;
        else if (name == "background") backgroundLayer = &layer;
    }
    if (terrain == nullptr) terrain = firstTileLayer;
    if (terrain == nullptr) {
        errorOut = path + " has no tile layer";
        return false;
    }
    width_ = (*terrain)["width"].asInt();
    height_ = (*terrain)["height"].asInt();
    if (width_ <= 0 || height_ <= 0) {
        errorOut = path + " has an empty tile layer";
        return false;
    }
    const std::size_t cellCount =
        static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);

    std::vector<std::uint32_t> gids;
    if (!decodeLayer(*terrain, path, gids, errorOut)) return false;
    if (gids.size() != cellCount) {
        errorOut = path + ": the tile layer holds " + std::to_string(gids.size()) +
                   " cells, expected " + std::to_string(cellCount);
        return false;
    }
    tiles_.reserve(gids.size());
    for (const std::uint32_t raw : gids) {
        int tileId = 0;
        if (!resolveGid(tileIdOfGid, raw, tileId)) {
            errorOut = path + ": tile " + std::to_string(tiles_.size()) + " uses gid " +
                       std::to_string(tileId) + ", which no tileset defines";
            return false;
        }
        tiles_.push_back(static_cast<std::uint8_t>(tileId));
    }

    // -- background -----------------------------------------------------------
    // Optional. A map without the layer leaves background() empty, and the
    // renderer falls back to the 3x3 section grid the layer replaced.
    if (backgroundLayer != nullptr) {
        std::vector<std::uint32_t> groundGids;
        if (!decodeLayer(*backgroundLayer, path, groundGids, errorOut)) return false;
        if (groundGids.size() != cellCount) {
            errorOut = path + ": the background layer holds " + std::to_string(groundGids.size()) +
                       " cells, expected " + std::to_string(cellCount);
            return false;
        }
        background_.reserve(groundGids.size());
        for (const std::uint32_t raw : groundGids) {
            int groundId = 0;
            if (!resolveGid(groundIdOfGid, raw, groundId)) {
                errorOut = path + ": background cell " + std::to_string(background_.size()) +
                           " uses gid " + std::to_string(groundId) +
                           ", which no ground tileset defines";
                return false;
            }
            background_.push_back(static_cast<std::int8_t>(groundId));
        }
    }

    // -- annotations --------------------------------------------------------
    for (const auto& spec : kObjectLayers) {
        for (const Json& layer : map["layers"].items()) {
            if (!layer.isObject()) continue;
            if (layer["type"].asString() != "objectgroup") continue;
            if (layer["name"].asString() != spec.layer) continue;
            for (const Json& object : layer["objects"].items()) {
                if (!object.isObject()) continue;
                const std::string kind = classOf(object);
                // An object of the wrong class on a kind's layer is an editing
                // mistake, and one that would otherwise turn a biome into a
                // spawn zone silently. Skipped, not guessed at.
                if (!kind.empty() && kind != spec.kind) continue;

                const Json custom = propertiesOf(object);
                Json properties = Json::object();
                for (const char* name : {"spawnType", "biomeName", "backgroundTexture"}) {
                    if (custom.contains(name)) properties.set(name, custom[name]);
                }
                if (custom.contains("isNoCombat")) properties.set("isNoCombat", custom["isNoCombat"]);
                if (custom.contains("teleportToX") || custom.contains("teleportToY")) {
                    Json destination = Json::object();
                    destination.set("x", custom["teleportToX"].asDouble());
                    destination.set("y", custom["teleportToY"].asDouble());
                    if (custom.contains("serverPort")) destination.set("serverPort", custom["serverPort"]);
                    properties.set("teleportTo", std::move(destination));
                }
                if (custom.contains("spawnTable")) {
                    // Tiled has no property type for a list of records, so a
                    // biome's spawn table travels as a JSON string. A table
                    // that will not parse is dropped with the biome left
                    // table-less, which safeForSpawn() already treats as
                    // "never spawn anyone here" -- the safe reading.
                    Json table;
                    std::string tableError;
                    if (Json::parse(custom["spawnTable"].asString(), table, tableError) && table.isArray()) {
                        properties.set("spawnTable", std::move(table));
                    } else {
                        std::fprintf(stderr, "[map] %s: object on layer \"%s\" has an unreadable spawnTable\n",
                                     path.c_str(), spec.layer);
                    }
                }

                Json element = Json::object();
                element.set("type", spec.kind);
                element.set("x", object["x"].asDouble());
                element.set("y", object["y"].asDouble());
                element.set("width", object["width"].asDouble());
                element.set("height", object["height"].asDouble());
                element.set("properties", std::move(properties));
                elements_.push(std::move(element));
            }
        }
    }

    return true;
}

std::vector<std::string> TiledMap::mismatchedFlags() const {
    std::vector<std::string> out;
    for (const TiledTileType& entry : palette_) {
        if (entry.id > static_cast<int>(Tile::Block)) continue;
        const Tile tile = static_cast<Tile>(entry.id);
        const bool water = tileIsWater(tile);
        const bool solid = tileBlocks(tile) && !water;
        if (entry.solid != solid || entry.water != water) out.push_back(entry.name);
    }
    return out;
}

std::string worldMapPath(const std::string& dataDir) {
    const std::string tiled = dataDir + "/world.tmj";
    std::ifstream probe(tiled, std::ios::binary);
    if (probe) return tiled;
    return dataDir + "/map_bundle.ts";
}

bool isTiledMapPath(const std::string& path) {
    return path.size() > 4 && path.compare(path.size() - 4, 4, ".tmj") == 0;
}

} // namespace flix
