#include "shared/game/tiled_map.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include "shared/game/constants.h"

namespace flix {

namespace {

/// The object layers, in the order their objects are concatenated.
///
/// Grouping the annotations by kind is what gives the editor layers it can
/// show and hide separately, and it is invisible to the game: every reader of
/// elements() filters by kind before it looks at order, so only the order
/// WITHIN a kind is observable, and that is the order they appear in here.
constexpr struct { const char* layer; const char* kind; } kObjectLayers[] = {
    {"spawns", "spawn"},
    {"player_spawns", "player_spawn"},
    {"teleporters", "teleporter"},
};

/// The custom properties each kind of object carries through verbatim.
///
/// Listed per kind rather than merged, so a property written onto the wrong
/// object -- a `targetMap` on a spawn band, say -- is dropped here instead of
/// reaching MapData and being acted on somewhere it means nothing.
constexpr struct { const char* kind; const char* properties[9]; } kObjectProperties[] = {
    {"spawn",        {"spawnType", "mobs", nullptr}},
    {"player_spawn", {"spawnId", "label", "color", "order", "backdrop", "biome", "pickable", nullptr}},
    {"teleporter",   {"targetMap", "targetSpawn", nullptr}},
};

/// Tiled's flip flags, in the top three bits of a gid.
constexpr std::uint32_t kGidFlipHorizontal = 0x80000000u;
constexpr std::uint32_t kGidFlipVertical = 0x40000000u;
constexpr std::uint32_t kGidFlipDiagonal = 0x20000000u;
constexpr std::uint32_t kGidMask = 0x1FFFFFFFu;

/// A cell's `art` is a signed 16-bit index, so a map may name this many
/// distinct artworks. Two orders of magnitude past any real tileset; the check
/// exists so the cast cannot silently wrap.
constexpr std::size_t kMaxArtFiles = 32767;

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
        // A chunked (infinite-map) layer lands here too: its `data` is an array
        // of chunk objects, not of numbers, and the map-level check above has
        // already refused it.
        errorOut = path + ": layer \"" + name + "\" has no plain cell data";
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

/// The gid range one tileset owns, for the overlap check.
struct TilesetRange {
    std::string name;
    int firstGid = 0;
    int tileCount = 0;
};

/// How many gids a tileset spans: its declared `tilecount`, or, for a tileset
/// that does not say, one past the highest tile id it defines. Never less than
/// one past the highest id either way, so a stale `tilecount` cannot leave a
/// painted tile unresolvable.
int tileCountOf(const Json& tileset) {
    int highest = -1;
    for (const Json& tile : tileset["tiles"].items()) {
        if (tile.isObject()) highest = std::max(highest, tile["id"].asInt());
    }
    return std::max(tileset["tilecount"].asInt(), highest + 1);
}

/// Tiled 1.9 renamed an object's `type` to `class` and still reads both.
std::string classOf(const Json& node) {
    const std::string name = node["class"].asString();
    return name.empty() ? node["type"].asString() : name;
}

/// Flattens Tiled's layer tree into one list in draw order.
///
/// A `group` layer is a folder in the editor and nothing at all in the file
/// format: its children draw in its place, bottom to top, exactly as if they
/// had been written where it stands. Recursing here is what lets an author
/// tidy the layer panel without changing what the game reads.
void collectLayers(const Json& list, std::vector<const Json*>& out) {
    for (const Json& layer : list.items()) {
        if (!layer.isObject()) continue;
        if (layer["type"].asString() == "group") {
            collectLayers(layer["layers"], out);
            continue;
        }
        out.push_back(&layer);
    }
}

} // namespace

bool TiledMap::load(const std::string& path, std::string& errorOut) {
    artFiles_.clear();
    layers_.clear();
    tiles_.clear();
    palette_.clear();
    elements_ = Json::array();
    properties_ = Json::object();
    paintedOnBlocker_.clear();
    paintedOnScenery_.clear();
    width_ = height_ = 0;
    wallCells_ = waterCells_ = groundCells_ = 0;

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
    width_ = map["width"].asInt();
    height_ = map["height"].asInt();
    if (width_ <= 0 || height_ <= 0) {
        errorOut = path + " is " + std::to_string(width_) + "x" + std::to_string(height_) +
                   " cells, which is not a size a map can be";
        return false;
    }
    const std::size_t cellCount =
        static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);

    // The map's OWN custom properties, as Tiled's Map Properties dialog
    // writes them. A map says things about itself that no object on it can --
    // what to call it in the spawn picker, which biome it is, and which mob
    // group its untagged spawn bands fall back to.
    properties_ = propertiesOf(map);

    // -- palette ------------------------------------------------------------
    // Tiled's global tile ids (gids) are per-map and depend on tileset order,
    // so a cell is resolved through the tilesets the map names rather than
    // assumed to index anything directly. gid 0 is Tiled's empty cell.
    std::vector<TilesetRange> ranges;
    struct Tileset {
        Json json;
        int firstGid = 0;
        int tileCount = 0;
    };
    std::vector<Tileset> tilesets;
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
        const std::string name = source.empty() ? tileset["name"].asString() : fileNameOf(source);
        if (firstGid < 1) {
            errorOut = path + ": tileset \"" + name + "\" starts at gid " +
                       std::to_string(firstGid) + "; gids start at 1";
            return false;
        }
        const int count = tileCountOf(tileset);
        if (count <= 0) {
            errorOut = path + ": tileset \"" + name + "\" defines no tiles";
            return false;
        }
        ranges.push_back({name, firstGid, count});
        tilesets.push_back({std::move(tileset), firstGid, count});
    }
    if (tilesets.empty()) {
        errorOut = path + " names no tilesets";
        return false;
    }

    // No two tilesets may own the same gid. Tiled itself does not check this
    // -- it resolves an ambiguous gid to whichever tileset it finds first --
    // and a map that draws one thing in the editor and another here is worse
    // than one that does not load. Checked over every pair by sorting on
    // firstgid: each tileset must end before the next begins.
    std::sort(ranges.begin(), ranges.end(),
              [](const TilesetRange& a, const TilesetRange& b) { return a.firstGid < b.firstGid; });
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        const TilesetRange& before = ranges[i - 1];
        const TilesetRange& after = ranges[i];
        if (before.firstGid + before.tileCount > after.firstGid) {
            errorOut = path + ": tilesets \"" + before.name + "\" (gids " +
                       std::to_string(before.firstGid) + ".." +
                       std::to_string(before.firstGid + before.tileCount - 1) + ") and \"" +
                       after.name + "\" (first gid " + std::to_string(after.firstGid) +
                       ") overlap; set \"" + after.name + "\"'s firstgid to at least " +
                       std::to_string(before.firstGid + before.tileCount);
            return false;
        }
    }

    // One palette entry per gid every tileset spans, in tileset order, and a
    // gid -> entry table beside it. A tileset that names no properties for a
    // tile -- a plain spritesheet cell -- still gets an entry: it is walkable,
    // draws nothing of its own, and, crucially, RESOLVES, so a map painted
    // with it is a map with no art rather than a map that will not load.
    std::unordered_map<std::string, int> artIndexByName;
    int highestGid = 0;
    for (const Tileset& tileset : tilesets) {
        highestGid = std::max(highestGid, tileset.firstGid + tileset.tileCount - 1);
    }
    std::vector<int> paletteOfGid(static_cast<std::size_t>(highestGid) + 1, -1);
    for (const Tileset& tileset : tilesets) {
        std::unordered_map<int, const Json*> byLocalId;
        for (const Json& tile : tileset.json["tiles"].items()) {
            if (tile.isObject()) byLocalId[tile["id"].asInt()] = &tile;
        }
        for (int localId = 0; localId < tileset.tileCount; ++localId) {
            TiledTileType entry;
            entry.gid = tileset.firstGid + localId;
            const auto found = byLocalId.find(localId);
            if (found != byLocalId.end()) {
                const Json& tile = *found->second;
                const Json properties = propertiesOf(tile);
                entry.water = properties["water"].asBool();
                entry.coversEverything = properties["covers_everything"].asBool();
                entry.art = fileNameOf(tile["image"].asString());
                entry.name = classOf(tile);
                if (!entry.art.empty()) {
                    const auto at = artIndexByName.find(entry.art);
                    if (at != artIndexByName.end()) {
                        entry.artIndex = at->second;
                    } else {
                        if (artFiles_.size() >= kMaxArtFiles) {
                            errorOut = path + " names more than " +
                                       std::to_string(kMaxArtFiles) + " distinct artworks";
                            return false;
                        }
                        entry.artIndex = static_cast<int>(artFiles_.size());
                        artIndexByName.emplace(entry.art, entry.artIndex);
                        artFiles_.push_back(entry.art);
                    }
                }
            }
            // Something to print in a message about this tile, whatever the
            // tileset chose to say about it.
            if (!entry.art.empty()) entry.name = entry.art;
            else if (entry.name.empty()) entry.name = "gid " + std::to_string(entry.gid);
            paletteOfGid[static_cast<std::size_t>(entry.gid)] = static_cast<int>(palette_.size());
            palette_.push_back(std::move(entry));
        }
    }

    // -- layers and the collision grid ---------------------------------------
    std::vector<const Json*> allLayers;
    collectLayers(map["layers"], allLayers);

    // COLLISION IS THE LAYER'S, not the tile's. `blockedCell` accumulates over
    // the layers that carry `has_collision`; `waterCell` is overwritten by
    // each blocking cell, so the last such layer to paint one -- the topmost
    // blocker -- decides what KIND of blocker the cell is. Layers without the
    // property are art and touch neither array. See the header.
    std::vector<std::uint8_t> blockedCell(cellCount, 0);
    std::vector<std::uint8_t> waterCell(cellCount, 0);
    paintedOnBlocker_.assign(palette_.size(), 0);
    paintedOnScenery_.assign(palette_.size(), 0);
    for (const Json* layer : allLayers) {
        if ((*layer)["type"].asString() != "tilelayer") continue;
        const std::string name = (*layer)["name"].asString();
        std::vector<std::uint32_t> gids;
        if (!decodeLayer(*layer, path, gids, errorOut)) return false;
        if (gids.size() != cellCount) {
            errorOut = path + ": layer \"" + name + "\" holds " + std::to_string(gids.size()) +
                       " cells, expected " + std::to_string(cellCount);
            return false;
        }
        TiledLayer out;
        out.name = name;
        // The whole of the collision rule, read once per layer. Tiled writes
        // the property only when the author has touched it, and an absent
        // property is a layer that does not block.
        out.collides = propertiesOf(*layer)[kLayerCollisionProperty].asBool();
        out.cells.resize(cellCount);
        for (std::size_t i = 0; i < cellCount; ++i) {
            const std::uint32_t raw = gids[i];
            const std::uint32_t gid = raw & kGidMask;
            if (gid == 0) continue;   // an empty cell: nothing drawn, nothing collided with
            if (gid >= paletteOfGid.size() || paletteOfGid[gid] < 0) {
                errorOut = path + ": cell " + std::to_string(i) + " of layer \"" + name +
                           "\" uses gid " + std::to_string(gid) + ", which no tileset defines";
                return false;
            }
            const std::size_t typeIndex = static_cast<std::size_t>(paletteOfGid[gid]);
            const TiledTileType& type = palette_[typeIndex];
            TiledCell& cell = out.cells[i];
            cell.art = static_cast<std::int16_t>(type.artIndex);
            // The flips are ART only; collision masks them off, because a
            // rotated tile still fills the same cell.
            if (raw & kGidFlipHorizontal) cell.flags |= kTileFlipHorizontal;
            if (raw & kGidFlipVertical) cell.flags |= kTileFlipVertical;
            if (raw & kGidFlipDiagonal) cell.flags |= kTileFlipDiagonal;
            if (type.coversEverything) cell.flags |= kTileCoversEverything;
            (out.collides ? paintedOnBlocker_ : paintedOnScenery_)[typeIndex] = 1;
            if (!out.collides) continue;
            blockedCell[i] = 1;
            waterCell[i] = type.water ? 1 : 0;
        }
        layers_.push_back(std::move(out));
    }
    if (layers_.empty()) {
        errorOut = path + " has no tile layer";
        return false;
    }
    tiles_.resize(cellCount);
    for (std::size_t i = 0; i < cellCount; ++i) {
        const Tile tile = blockedCell[i] == 0 ? Tile::Ground
                        : waterCell[i] != 0   ? Tile::Water
                                              : Tile::Wall;
        tiles_[i] = static_cast<std::uint8_t>(tile);
        if (tile == Tile::Wall) ++wallCells_;
        else if (tile == Tile::Water) ++waterCells_;
        else ++groundCells_;
    }

    // -- annotations --------------------------------------------------------
    for (const auto& spec : kObjectLayers) {
        for (const Json* layerPtr : allLayers) {
            const Json& layer = *layerPtr;
            if (layer["type"].asString() != "objectgroup") continue;
            if (layer["name"].asString() != spec.layer) continue;
            for (const Json& object : layer["objects"].items()) {
                if (!object.isObject()) continue;
                const std::string kind = classOf(object);
                // An object of the wrong class on a kind's layer is an editing
                // mistake, and one that would otherwise turn a door into a
                // spawn band silently. Skipped, not guessed at.
                if (!kind.empty() && kind != spec.kind) continue;

                const Json custom = propertiesOf(object);
                Json properties = Json::object();
                for (const auto& allowed : kObjectProperties) {
                    if (std::string(allowed.kind) != spec.kind) continue;
                    for (const char* name : allowed.properties) {
                        if (name == nullptr) break;
                        if (custom.contains(name)) properties.set(name, custom[name]);
                    }
                }
                // A teleporter may name a point in the target map instead of
                // one of its spawn rectangles. Both halves are optional and
                // default to zero, which is what `targetSpawn` exists to avoid
                // having to write.
                if (custom.contains("teleportToX") || custom.contains("teleportToY")) {
                    Json destination = Json::object();
                    destination.set("x", custom["teleportToX"].asDouble());
                    destination.set("y", custom["teleportToY"].asDouble());
                    properties.set("teleportTo", std::move(destination));
                }
                // Tiled writes an object's name outside its property bag, and
                // it is the obvious place to type a spawn point's id. The
                // explicit `spawnId` property still wins, so a map that wants
                // a human name and a stable id can have both. An object with
                // neither is named by MapData, off its label.
                if (!object["name"].asString().empty() && !properties.contains("spawnId")) {
                    properties.set("spawnId", object["name"].asString());
                }

                Json element = Json::object();
                element.set("type", spec.kind);
                element.set("x", object["x"].asDouble());
                element.set("y", object["y"].asDouble());
                element.set("width", object["width"].asDouble());
                element.set("height", object["height"].asDouble());

                // A polygon object. Tiled writes its points RELATIVE to the
                // object's own x/y, so a dragged zone moves as one; the game
                // wants world coordinates, and MapData recomputes the bounding
                // box from them. Without this the zone arrives as a Tiled
                // polygon's zero-sized rectangle and is dropped as degenerate.
                const Json& outline = object["polygon"];
                if (outline.isArray() && outline.size() >= 3) {
                    Json points = Json::array();
                    for (const Json& point : outline.items()) {
                        Json at = Json::object();
                        at.set("x", object["x"].asDouble() + point["x"].asDouble());
                        at.set("y", object["y"].asDouble() + point["y"].asDouble());
                        points.push(std::move(at));
                    }
                    element.set("polygon", std::move(points));
                } else if (object["polyline"].isArray()) {
                    // An area, not a path. Reported rather than closed for the
                    // author: a polyline that happens to enclose something is
                    // not the same shape as the polygon they meant to draw.
                    std::fprintf(stderr, "[map] %s: object on layer \"%s\" is a polyline; "
                                         "a zone must be a closed polygon\n",
                                 path.c_str(), spec.layer);
                    continue;
                }
                element.set("properties", std::move(properties));
                elements_.push(std::move(element));
            }
        }
    }

    return true;
}

std::vector<std::string> TiledMap::strandedWaterTiles() const {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < palette_.size(); ++i) {
        // Painted, but never anywhere it can block: every cell holding it will
        // be Ground, whatever the tag says and whatever the editor draws.
        if (!palette_[i].water) continue;
        if (i >= paintedOnScenery_.size() || paintedOnScenery_[i] == 0) continue;
        if (i < paintedOnBlocker_.size() && paintedOnBlocker_[i] != 0) continue;
        static_assert(tileIsWater(Tile::Water), "a blocking water tile becomes Tile::Water");
        static_assert(!tileIsWater(Tile::Ground) && !tileBlocks(Tile::Ground),
                      "a tile on a layer that does not collide leaves the cell walkable");
        out.push_back(palette_[i].name);
    }
    return out;
}

} // namespace flix
