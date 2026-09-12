#include "shared/game/tiled_map.h"

#include <algorithm>
#include <cmath>
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
    {"spawn",        {"difficulty", "mobs", nullptr}},
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

/// How finely an ellipse is polygonised, as the greatest distance the ring may
/// fall inside the true curve, in TILESET units.
///
/// One tileset unit is a bit over one world unit here (300/256), so the error a
/// player could feel is about a millimetre of a 300-unit cell. The ring is
/// INSCRIBED, so a polygonised ellipse is very slightly smaller than the one
/// the author drew, never larger.
constexpr double kEllipseTolerance = 1.0;
constexpr int kMinEllipseSegments = 8;
constexpr int kMaxEllipseSegments = 64;

/// Signed area of a ring, doubled. Positive means the winding TiledShape wants.
double doubleSignedArea(const std::vector<Vec2>& points) {
    double total = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec2& a = points[i];
        const Vec2& b = points[(i + 1) % points.size()];
        total += a.x * b.y - b.x * a.y;
    }
    return total;
}

/// Drops a finished ring into `out`, wound positive and scaled from the tile
/// space it was drawn in onto one cell of world units.
///
/// Degenerate rings -- fewer than three points, or zero area, which is what a
/// zero-sized rectangle or a shape whose points are all in a line comes to --
/// are dropped: they cannot contain a point and cannot push a circle, so
/// carrying them would only cost every query a few edges.
void addRing(std::vector<Vec2> points, double scaleX, double scaleY,
             std::vector<TiledShape>& out) {
    if (points.size() < 3) return;
    for (Vec2& point : points) {
        point.x *= scaleX;
        point.y *= scaleY;
    }
    const double area = doubleSignedArea(points);
    if (std::fabs(area) < 1e-9) return;
    if (area < 0.0) std::reverse(points.begin(), points.end());
    TiledShape shape;
    shape.points = std::move(points);
    out.push_back(std::move(shape));
}

/// One tile's `objectgroup`, as Tiled's Tile Collision Editor writes it, turned
/// into rings in cell-local world units.
///
/// The space the author drew in is the TILE'S OWN IMAGE, and that is what is
/// read first. maps/tileset.tsj is an image-collection tileset ("columns": 0,
/// every tile carrying its own <image>), and for one of those Tiled's
/// tileset-level tilewidth/tileheight is only the DISPLAY GRID -- it is the
/// largest image in the collection, and Tiled rewrites it the moment a bigger
/// one is dropped in. The Tile Collision Editor draws in the tile's image
/// regardless, so reading the tileset's number first would silently halve every
/// authored shape in the game the first time somebody added a 512-square tile.
/// The tileset's size is the fallback, for a SPRITESHEET tileset whose tiles
/// carry no image of their own; there it is the only size there is.
///
/// Scaled per axis onto the map's cell, so a 256-square tile painted onto
/// 300-unit cells stretches by 300/256 on both and a map with non-square cells
/// would stretch differently on each. No number here is written down: they all
/// come out of the files.
void readTileShapes(const Json& tile, const Json& tileset, const std::string& tileName,
                    const std::string& path, std::vector<TiledShape>& out) {
    const Json& group = tile["objectgroup"];
    if (!group.isObject()) return;
    const Json& objects = group["objects"];
    if (!objects.isArray()) return;

    double spaceW = tile["imagewidth"].asDouble();
    double spaceH = tile["imageheight"].asDouble();
    if (!(spaceW > 0.0)) spaceW = tileset["tilewidth"].asDouble();
    if (!(spaceH > 0.0)) spaceH = tileset["tileheight"].asDouble();
    if (!(spaceW > 0.0) || !(spaceH > 0.0)) {
        std::fprintf(stderr, "[map] %s: tile \"%s\" has collision shapes but no tile size to "
                             "read them in; ignoring them\n", path.c_str(), tileName.c_str());
        return;
    }
    const double scaleX = kTileSize / spaceW;
    const double scaleY = kTileSize / spaceH;

    // Tiled anchors an object's transform to its (x, y) -- the top-left of a
    // rectangle's or an ellipse's bounding box, the origin a polygon's points
    // are relative to -- and writes the angle in degrees. ONE turn for every
    // shape kind, so a kind cannot be added that quietly ignores `rotation`:
    // an ellipse that did exactly that collided along the wrong axis.
    const auto turnAbout = [](std::vector<Vec2>& points, double x, double y, double degrees) {
        if (degrees == 0.0) return;
        const double radians = degrees * kPi / 180.0;
        const double c = std::cos(radians);
        const double sn = std::sin(radians);
        for (Vec2& point : points) {
            const double dx = point.x - x;
            const double dy = point.y - y;
            point = {x + dx * c - dy * sn, y + dx * sn + dy * c};
        }
    };

    for (const Json& object : objects.items()) {
        if (!object.isObject()) continue;
        const double x = object["x"].asDouble();
        const double y = object["y"].asDouble();

        // An open path, not an area. There is no honest way to collide with
        // one -- "inside" is undefined and a zero-width sliver would push a
        // body to whichever side rounding chose -- so it is skipped, loudly.
        if (object["polyline"].isArray()) {
            std::fprintf(stderr, "[map] %s: tile \"%s\" has a POLYLINE collision shape; only "
                                 "closed shapes collide, so it is ignored\n",
                         path.c_str(), tileName.c_str());
            continue;
        }

        const Json& outline = object["polygon"];
        if (outline.isArray()) {
            // Tiled writes a polygon's points relative to the object's own
            // origin, so a dragged shape moves as one.
            std::vector<Vec2> points;
            points.reserve(outline.size());
            for (const Json& point : outline.items()) {
                points.push_back({x + point["x"].asDouble(), y + point["y"].asDouble()});
            }
            // `rotation` on a polygon turns its points about the object origin,
            // exactly as it turns a rectangle's corners.
            turnAbout(points, x, y, object["rotation"].asDouble());
            addRing(std::move(points), scaleX, scaleY, out);
            continue;
        }

        const double w = object["width"].asDouble();
        const double h = object["height"].asDouble();
        if (object["ellipse"].asBool()) {
            // Tiled's ellipse is its bounding box. Inscribed, at enough
            // segments that the ring never falls more than kEllipseTolerance
            // inside the curve.
            const double rx = w * 0.5;
            const double ry = h * 0.5;
            const double r = std::max(rx, ry);
            int segments = kMaxEllipseSegments;
            if (r > kEllipseTolerance) {
                const double cosine = 1.0 - kEllipseTolerance / r;
                segments = static_cast<int>(std::ceil(kPi / std::acos(clamp(cosine, -1.0, 1.0))));
            }
            segments = clamp(segments, kMinEllipseSegments, kMaxEllipseSegments);
            std::vector<Vec2> points;
            points.reserve(static_cast<std::size_t>(segments));
            for (int i = 0; i < segments; ++i) {
                const double t = 2.0 * kPi * i / segments;
                points.push_back({x + rx + rx * std::cos(t), y + ry + ry * std::sin(t)});
            }
            // Rotated about (x, y), the top-left of the bounding box -- the
            // same anchor a rectangle turns about, because in Tiled an ellipse
            // IS a rectangle with a tick box set.
            turnAbout(points, x, y, object["rotation"].asDouble());
            addRing(std::move(points), scaleX, scaleY, out);
            continue;
        }

        // A plain rectangle. `rotation` turns it about its own top-left corner,
        // which is the point Tiled anchors an object's transform to.
        std::vector<Vec2> points = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
        turnAbout(points, x, y, object["rotation"].asDouble());
        addRing(std::move(points), scaleX, scaleY, out);
    }
}

} // namespace

TileOrientation tileOrientation(std::uint8_t flags) {
    // Derived from Tiled's own definition -- the anti-diagonal flip first, then
    // horizontal, then vertical -- and checked against it: an asymmetric glyph
    // was rendered through this function and compared, pixel for pixel, with
    // the same glyph's unturned rasterisation permuted by V^v * H^h * D^d.
    // Every one of the eight matched exactly, none of the eight drew the same
    // picture as any other, and swapping D|H with D|V -- the likeliest way to
    // get this wrong -- was caught. If you change a row, redo that: the map is
    // painted almost entirely from rotations of a handful of Wang edge tiles,
    // so a wrong row here is wrong on most of the screen and, now that the
    // collision shapes come through here too, blocks in the wrong corner of
    // most of the cells as well.
    static constexpr TileOrientation kOrientations[8] = {
        {0.0, false},          // 0:   as drawn
        {0.0, true},           // H:   mirrored
        {kPi, true},           // V:   mirrored, half turn
        {kPi, false},          // HV:  half turn
        {-kPi * 0.5, true},    // D:   transpose
        {kPi * 0.5, false},    // DH:  quarter turn clockwise
        {-kPi * 0.5, false},   // DV:  quarter turn anticlockwise
        {kPi * 0.5, true},     // DHV: anti-transpose
    };
    return kOrientations[flags & 7u];
}

std::vector<TiledShape> orientTileShapes(const std::vector<TiledShape>& shapes,
                                         std::uint8_t flags) {
    std::vector<TiledShape> out;
    out.reserve(shapes.size());
    for (const TiledShape& shape : shapes) {
        TiledShape turned;
        turned.points.reserve(shape.points.size());
        for (const Vec2& point : shape.points) {
            turned.points.push_back(orientInTile(point, flags, kTileSize));
        }
        // A mirroring flip reverses a ring, and half of the eight orientations
        // mirror. Wound positive again here so that (dy, -dx) of an edge still
        // points OUT of the shape, which is what a push-out off a boundary
        // relies on.
        if (doubleSignedArea(turned.points) < 0.0) {
            std::reverse(turned.points.begin(), turned.points.end());
        }
        out.push_back(std::move(turned));
    }
    return out;
}

Rect orientedShapeBounds(const std::vector<TiledShape>& shapes, std::uint8_t flags) {
    bool first = true;
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    for (const TiledShape& turned : orientTileShapes(shapes, flags)) {
        for (const Vec2& point : turned.points) {
            if (first) {
                minX = maxX = point.x;
                minY = maxY = point.y;
                first = false;
                continue;
            }
            minX = std::min(minX, point.x);
            maxX = std::max(maxX, point.x);
            minY = std::min(minY, point.y);
            maxY = std::max(maxY, point.y);
        }
    }
    if (first) return Rect{};
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

bool tileDecksWholeCell(const std::vector<TiledShape>& shapes, std::uint8_t flags) {
    // No shapes IS a whole-cell deck; see the declaration for why that is the
    // opposite of what no shapes means on a colliding layer.
    if (shapes.empty()) return true;
    // A hair of slack, because the ring came through a scale (the tileset's
    // tile space onto the cell) and a rotation. Both are exact for the shapes
    // anyone actually draws, and a tenth of a world unit is far below anything
    // an author can mean.
    constexpr double kSlack = 0.1;
    for (const TiledShape& turned : orientTileShapes(shapes, flags)) {
        // Only a rectangle can be read off its bounding box. Four points, each
        // on a different corner of the box they span -- which is how Tiled
        // writes a rectangle object, in every one of the eight orientations.
        if (turned.points.size() != 4) continue;
        double minX = turned.points[0].x, maxX = minX;
        double minY = turned.points[0].y, maxY = minY;
        for (const Vec2& p : turned.points) {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        const Rect box{minX, minY, maxX - minX, maxY - minY};
        int corners = 0;
        bool onBox = true;
        for (const Vec2& p : turned.points) {
            const bool left = std::abs(p.x - box.left()) <= kSlack;
            const bool right = std::abs(p.x - box.right()) <= kSlack;
            const bool top = std::abs(p.y - box.top()) <= kSlack;
            const bool bottom = std::abs(p.y - box.bottom()) <= kSlack;
            if (!(left || right) || !(top || bottom)) { onBox = false; break; }
            corners |= 1 << ((right ? 1 : 0) | (bottom ? 2 : 0));
        }
        if (!onBox || corners != 0b1111) continue;
        if (box.left() <= kSlack && box.top() <= kSlack && box.right() >= kTileSize - kSlack &&
            box.bottom() >= kTileSize - kSlack) {
            return true;
        }
    }
    return false;
}

ShapeReach shapeReach(const Rect& bounds) {
    ShapeReach reach;
    if (bounds.w < 0.0 || bounds.h < 0.0) return reach;
    const auto firstCell = [](double v) {
        return static_cast<int>(std::floor(v / kTileSize));
    };
    // The cell the far edge is IN, not the one it merely reaches: a box ending
    // exactly on a boundary belongs to the cell before it. See ShapeReach.
    const auto lastCell = [](double v) {
        const double cells = v / kTileSize;
        const double floored = std::floor(cells);
        return static_cast<int>(cells == floored ? floored - 1.0 : floored);
    };
    reach.dxMin = firstCell(bounds.left());
    reach.dyMin = firstCell(bounds.top());
    reach.dxMax = std::max(reach.dxMin, lastCell(bounds.right()));
    reach.dyMax = std::max(reach.dyMin, lastCell(bounds.bottom()));
    return reach;
}

Vec2 orientInTile(Vec2 local, std::uint8_t flags, double side) {
    const TileOrientation orientation = tileOrientation(flags);
    // The renderer's transform, on a point: translate to the cell's centre,
    // mirror x, rotate. A canvas composes rotate-then-scale as R * S, so the
    // mirror applies FIRST to the point, which is the order below.
    const double half = side * 0.5;
    double x = local.x - half;
    double y = local.y - half;
    if (orientation.mirror) x = -x;
    // Every angle is a multiple of a quarter turn, so the sine and cosine are
    // whole numbers; snapping them keeps a shape's corner exactly on the
    // cell's corner instead of 1e-14 off it.
    const double c = std::round(std::cos(orientation.radians));
    const double sn = std::round(std::sin(orientation.radians));
    return {half + x * c - y * sn, half + x * sn + y * c};
}

bool TiledMap::load(const std::string& path, std::string& errorOut) {
    artFiles_.clear();
    layers_.clear();
    tiles_.clear();
    palette_.clear();
    elements_ = Json::array();
    properties_ = Json::object();
    paintedOnBlocker_.clear();
    paintedOnScenery_.clear();
    unshapedBlockingCells_ = 0;
    negatedCells_ = 0;
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
                // The shapes the author drew on this tile, scaled onto one cell
                // but NOT yet turned: the flip bits belong to the cell, not to
                // the tile, and one tile serves all eight orientations.
                readTileShapes(tile, tileset.json,
                               entry.art.empty() ? ("gid " + std::to_string(entry.gid))
                                                 : entry.art,
                               path, entry.shapes);
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

    // A LAYER decides whether a cell can collide, the TILE's shapes decide
    // whether it does. `blockedCell` accumulates over the layers that carry
    // `has_collision`, but only where the tile there actually has a shape;
    // `waterCell` is overwritten by each contributing cell, so the last such
    // layer to paint one -- the topmost contributor -- decides what KIND of
    // blocker the cell is. Layers without the property are art and touch
    // neither array. See the header.
    std::vector<std::uint8_t> blockedCell(cellCount, 0);
    std::vector<std::uint8_t> waterCell(cellCount, 0);
    // A cell is counted ONCE however many colliding layers paint an unshaped
    // tile into it: the warning it feeds is "this many cells of your map look
    // solid and are not", and a number that grew with the layer count could
    // not be compared with the map's size at all.
    std::vector<std::uint8_t> unshapedCell(cellCount, 0);
    // Cells a negating layer took collision away from. Counted once per cell
    // however many decks are stacked over it, for the same reason as above.
    std::vector<std::uint8_t> negatedCell(cellCount, 0);
    // A shape the author dragged or turned out of its tile blocks in a cell
    // its tile was never painted in (shapeReach). Those writes are collected
    // per layer and applied after it, so that a cell's OWN tile on a layer
    // still decides that cell's kind -- an overhang only ever adds a blocker,
    // it never renames one.
    struct Overhang {
        std::size_t cell;
        bool water;
    };
    std::vector<Overhang> overhangs;
    std::vector<std::uint32_t> ownedOnLayer(cellCount, 0);
    std::uint32_t layerStamp = 0;
    // Reach is a property of a (tile, orientation) pair, not of a cell, and
    // computing it means turning the tile's rings; done once per pair.
    std::unordered_map<std::uint64_t, ShapeReach> reachOfPair;
    const auto reachFor = [&](std::size_t typeIndex, std::uint8_t flags) -> const ShapeReach& {
        const std::uint64_t key = (static_cast<std::uint64_t>(typeIndex) << 3) | (flags & 7u);
        const auto found = reachOfPair.find(key);
        if (found != reachOfPair.end()) return found->second;
        const ShapeReach reach =
            shapeReach(orientedShapeBounds(palette_[typeIndex].shapes, flags));
        return reachOfPair.emplace(key, reach).first->second;
    };
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
        const Json layerProperties = propertiesOf(*layer);
        const bool ticked = layerProperties[kLayerCollisionProperty].asBool();
        out.negates = layerProperties[kLayerNegateProperty].asBool();
        // The two properties are opposites. A layer carrying both is incoherent
        // and NEGATION WINS -- it is the property an author has to go looking
        // for, so it is the one they meant -- but the choice is recorded rather
        // than made quietly, and the load report names the layer.
        out.conflicting = ticked && out.negates;
        out.collides = ticked && !out.negates;
        out.cells.resize(cellCount);
        ++layerStamp;
        overhangs.clear();
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
            cell.type = static_cast<std::int32_t>(typeIndex);
            // The flips travel with the cell, and BOTH the art and the tile's
            // collision shapes are turned by them (orientInTile). A rotated
            // Wang edge tile blocks along the edge it draws.
            if (raw & kGidFlipHorizontal) cell.flags |= kTileFlipHorizontal;
            if (raw & kGidFlipVertical) cell.flags |= kTileFlipVertical;
            if (raw & kGidFlipDiagonal) cell.flags |= kTileFlipDiagonal;
            if (type.coversEverything) cell.flags |= kTileCoversEverything;
            ++out.paintedCells;
            (out.collides ? paintedOnBlocker_ : paintedOnScenery_)[typeIndex] = 1;
            if (out.negates) {
                // A DECK. It cancels what the layers BELOW it put here, which
                // in this coarse view is simply whatever has accumulated so
                // far; layers above are read after this one and block again.
                //
                // Over the negating tile's OWN shapes when it has any, over
                // the WHOLE CELL when it has none -- the opposite of the
                // blocking rule, and deliberately so: see tiled_map.h. A tile
                // whose shapes COVER the cell is the same whole-cell deck said
                // explicitly (tileDecksWholeCell), and is resolved here so that
                // the two spellings behave identically everywhere.
                if (tileDecksWholeCell(type.shapes, cell.flags)) {
                    // Counted from what was CANCELLED, not from what was
                    // there: a deck over open ground clears nothing, and the
                    // load report's whole job is to say so.
                    if (blockedCell[i] != 0) {
                        ++out.clearedCells;
                        negatedCell[i] = 1;
                    }
                    blockedCell[i] = 0;
                    waterCell[i] = 0;
                    continue;
                }
                // A PARTIAL deck. One Tile per cell cannot say "half of this
                // cell", so the coarse cell stays blocked and only the exact
                // store cancels anything -- and only for the point tests. That
                // is the half-supported case the load report warns about; it
                // is NOT counted as cleared, because from here nothing was.
                ++out.partialDeckCells;
                continue;
            }
            if (!out.collides) continue;
            // A tile with no authored shape contributes no collision, even
            // here. Counted, because it is the one way a map can look solid in
            // the editor and be walkable in the game.
            if (type.shapes.empty()) {
                unshapedCell[i] = 1;
                continue;
            }
            blockedCell[i] = 1;
            waterCell[i] = type.water ? 1 : 0;
            ownedOnLayer[i] = layerStamp;
            const ShapeReach& reach = reachFor(typeIndex, cell.flags);
            if (reach.ownCellOnly()) continue;
            const int tx = static_cast<int>(i % static_cast<std::size_t>(width_));
            const int ty = static_cast<int>(i / static_cast<std::size_t>(width_));
            for (int dy = reach.dyMin; dy <= reach.dyMax; ++dy) {
                for (int dx = reach.dxMin; dx <= reach.dxMax; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = tx + dx;
                    const int ny = ty + dy;
                    if (nx < 0 || ny < 0 || nx >= width_ || ny >= height_) continue;
                    overhangs.push_back(
                        {static_cast<std::size_t>(ny) * static_cast<std::size_t>(width_) +
                             static_cast<std::size_t>(nx),
                         type.water});
                }
            }
        }
        for (const Overhang& reached : overhangs) {
            if (ownedOnLayer[reached.cell] == layerStamp) continue;
            blockedCell[reached.cell] = 1;
            waterCell[reached.cell] = reached.water ? 1 : 0;
        }
        layers_.push_back(std::move(out));
    }
    if (layers_.empty()) {
        errorOut = path + " has no tile layer";
        return false;
    }
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (unshapedCell[i] != 0) ++unshapedBlockingCells_;
        if (negatedCell[i] != 0) ++negatedCells_;
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

std::vector<std::string> TiledMap::unshapedBlockingTiles() const {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < palette_.size(); ++i) {
        if (i >= paintedOnBlocker_.size() || paintedOnBlocker_[i] == 0) continue;
        if (!palette_[i].shapes.empty()) continue;
        out.push_back(palette_[i].name);
    }
    return out;
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
