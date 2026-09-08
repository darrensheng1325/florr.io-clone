#include "shared/game/config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "shared/core/json.h"
#include "shared/net/protocol.h"

namespace flix {
namespace {

// ---------------------------------------------------------------------------
// Sanitisation limits
// ---------------------------------------------------------------------------
//
// The shipped JSON is hand-maintained. Non-finite values must not reach the
// simulation, but finite values retain TypeScript semantics even when they are
// unusual (zero-sized placeholder mobs, negative glitch damage, and sparkle's
// deliberately enormous damage are all observable game content).

/// Still far below the point where rarity multiplication threatens a double,
/// while admitting sparkle's authored 9,999,999,999 exactly.
constexpr double kMaxBaseStat = 1e12;

/// A petal whose slot takes longer than this to come back is a dead slot, not
/// a slow one. `yggdrasil` ships 2048000 (34 minutes).
constexpr double kMaxCooldownMillis = 60000.0;

constexpr double kMinSize = 0.01;
constexpr double kMaxSize = 1000.0;

/// Any farther than this and the value is not a draw offset -- it is someone
/// pushing a sprite off the world to hide it.
constexpr double kMaxVisualOffset = 500.0;

constexpr double kMaxSpeedUnits = 1e4;
constexpr double kMaxDurationMillis = 600000.0;

/// A spawner ticking faster than this is a spawn loop, not a nest.
constexpr double kMinSpawnIntervalMillis = 50.0;

/// Poison the JSON writes per millisecond; a full second's worth is the cap.
constexpr double kMaxPoisonPerMillis = 1000.0;

/// A bad file could otherwise turn into a million strings. The warnings are a
/// report for a human, and a human stops reading long before this.
constexpr std::size_t kMaxWarnings = 512;

/// TypeScript applies `speed * 2` once per 30 Hz mob step.
constexpr double kMobSpeedUnitsPerSecond = 60.0;

/// Config `size` to a petal's own hit radius, in world units.
///
/// The same number the client draws a petal at (ui::kPetalArtSize is 20 units
/// of DIAMETER per size unit), because a petal hits exactly what it looks like
/// it hits -- which is how gardn states a petal too: one `radius` field, used
/// for the body and for the artwork alike.
///
/// It was 20 here, twice the drawn radius, and a basic petal damaged mobs a
/// full body-width past its own edge. Mobs and flowers are unaffected: they
/// keep kMobBaseRadius / kPlayerBaseRadius, and only the petal was ever drawn
/// at a different scale from the thing it collides with.
constexpr double kPetalRadiusPerSize = 10.0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// Short-form number for a warning line. std::to_string would render -1e100
/// as a hundred and one digits.
std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

const char* typeName(const Json& v) {
    switch (v.type()) {
        case Json::Type::Null:   return "null";
        case Json::Type::Bool:   return "a boolean";
        case Json::Type::Number: return "a number";
        case Json::Type::String: return "a string";
        case Json::Type::Array:  return "an array";
        case Json::Type::Object: return "an object";
    }
    return "unknown";
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

std::string joinPath(const std::string& dir, const char* file) {
    if (dir.empty()) return file;
    return dir.back() == '/' ? dir + file : dir + "/" + file;
}

/// Reading context: which entry is being parsed, and where its complaints go.
struct Ctx {
    std::vector<std::string>& warnings;
    std::string subject;

    void warn(const std::string& text) {
        if (warnings.size() >= kMaxWarnings) return;
        warnings.push_back(subject + ": " + text);
    }

    /// A number forced into [lo, hi].
    ///
    /// An absent key is a default and says nothing. A key that is present but
    /// is not a finite number in range is a defect in the data, and is
    /// reported with the value that caused it.
    double range(const Json& obj, const char* key, double fallback, double lo, double hi) {
        if (!obj.contains(key)) return fallback;
        const Json& node = obj[key];
        if (!node.isNumber()) {
            warn(std::string(key) + " is " + typeName(node) + "; using " + num(fallback));
            return fallback;
        }
        const double v = node.asDouble();
        if (!std::isfinite(v)) {
            warn(std::string(key) + " is not a finite number; using " + num(fallback));
            return fallback;
        }
        if (v < lo) {
            warn(std::string(key) + " is " + num(v) + ", below the sane minimum " + num(lo) +
                 "; using " + num(lo));
            return lo;
        }
        if (v > hi) {
            warn(std::string(key) + " is " + num(v) + ", above the sane maximum " + num(hi) +
                 "; using " + num(hi));
            return hi;
        }
        return v;
    }

    int integer(const Json& obj, const char* key, int fallback, int lo, int hi) {
        const double v = range(obj, key, static_cast<double>(fallback),
                               static_cast<double>(lo), static_cast<double>(hi));
        return static_cast<int>(std::lround(v));
    }

    bool boolean(const Json& obj, const char* key, bool fallback = false) {
        if (!obj.contains(key)) return fallback;
        const Json& node = obj[key];
        if (!node.isBool()) {
            warn(std::string(key) + " is " + typeName(node) + ", not true or false; ignored");
            return fallback;
        }
        return node.asBool();
    }

    std::string text(const Json& obj, const char* key, const std::string& fallback = {}) {
        if (!obj.contains(key)) return fallback;
        const Json& node = obj[key];
        if (!node.isString()) {
            warn(std::string(key) + " is " + typeName(node) + ", not a string; ignored");
            return fallback;
        }
        return node.asString();
    }

    /// A speed field. A negative speed is a direction, and direction is the
    /// AI's business -- `moth` ships -2.4 to mean "runs away", which the flee
    /// behaviour already expresses. Keep the magnitude.
    double speed(const Json& obj, const char* key) {
        double v = range(obj, key, 0.0, -kMaxSpeedUnits, kMaxSpeedUnits);
        if (v < 0.0) {
            warn(std::string(key) + " is " + num(v) +
                 "; a speed is a magnitude, so using " + num(-v));
            v = -v;
        }
        return v;
    }

    Rarity rarity(const Json& obj, const char* key, Rarity fallback = Rarity::Common) {
        const std::string name = text(obj, key);
        if (name.empty()) return fallback;
        for (int i = 0; i < kRarityCount; ++i) {
            if (name == kRarityNames[static_cast<std::size_t>(i)]) return static_cast<Rarity>(i);
        }
        warn(std::string(key) + " names an unknown rarity '" + name + "'; using " +
             rarityName(fallback));
        return fallback;
    }

    /// Resolves a content id to its index, complaining when it names nothing.
    std::uint16_t link(const std::unordered_map<std::string, std::uint16_t>& table,
                       const std::string& id, const char* what) {
        if (id.empty()) return kInvalidIndex;
        auto it = table.find(id);
        if (it == table.end()) {
            warn(std::string(what) + " '" + id + "' is not defined; the reference is dropped");
            return kInvalidIndex;
        }
        return it->second;
    }
};

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Parses `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb(...)` and `rgba(...)` into
/// 0xRRGGBBAA. `bubble` is genuinely `rgba(255, 255, 255, 0)` and means it, so
/// alpha is carried rather than assumed opaque.
bool parseColor(const std::string& in, Rgba& out) {
    std::string s;
    s.reserve(in.size());
    for (const char c : in) {
        if (c != ' ' && c != '\t') s.push_back(c);
    }
    if (s.size() >= 2 && s[0] == '#') {
        const std::string body = s.substr(1);
        int d[8];
        if (body.size() != 3 && body.size() != 6 && body.size() != 8) return false;
        for (std::size_t i = 0; i < body.size(); ++i) {
            d[i] = hexDigit(body[i]);
            if (d[i] < 0) return false;
        }
        if (body.size() == 3) {
            const auto expand = [&](int i) { return static_cast<std::uint32_t>(d[i] * 17); };
            out = (expand(0) << 24) | (expand(1) << 16) | (expand(2) << 8) | 0xFFu;
        } else {
            const std::uint32_t r = static_cast<std::uint32_t>(d[0] * 16 + d[1]);
            const std::uint32_t g = static_cast<std::uint32_t>(d[2] * 16 + d[3]);
            const std::uint32_t b = static_cast<std::uint32_t>(d[4] * 16 + d[5]);
            const std::uint32_t a = body.size() == 8
                ? static_cast<std::uint32_t>(d[6] * 16 + d[7]) : 0xFFu;
            out = (r << 24) | (g << 16) | (b << 8) | a;
        }
        return true;
    }

    const bool hasAlpha = s.rfind("rgba(", 0) == 0;
    if (!hasAlpha && s.rfind("rgb(", 0) != 0) return false;
    const std::size_t open = s.find('(');
    const std::size_t close = s.find(')', open);
    if (close == std::string::npos) return false;
    double parts[4] = {0, 0, 0, 1};
    int count = 0;
    std::stringstream fields(s.substr(open + 1, close - open - 1));
    std::string field;
    while (std::getline(fields, field, ',') && count < 4) {
        char* end = nullptr;
        const double v = std::strtod(field.c_str(), &end);
        if (end == field.c_str() || !std::isfinite(v)) return false;
        parts[count++] = v;
    }
    if (count < 3 || (hasAlpha && count < 4)) return false;
    const auto byte = [](double v, double scale) {
        return static_cast<std::uint32_t>(clamp(v * scale, 0.0, 255.0) + 0.5);
    };
    out = (byte(parts[0], 1.0) << 24) | (byte(parts[1], 1.0) << 16) |
          (byte(parts[2], 1.0) << 8) | byte(parts[3], 255.0);
    return true;
}

Rgba readColor(Ctx& ctx, const Json& obj, const char* key, std::string& raw) {
    raw = ctx.text(obj, key);
    if (raw.empty()) return kOpaqueWhite;
    Rgba packed = kOpaqueWhite;
    if (!parseColor(raw, packed)) {
        ctx.warn(std::string(key) + " '" + raw + "' is not a colour; using white");
        return kOpaqueWhite;
    }
    return packed;
}

// ---------------------------------------------------------------------------
// Enum parsing
// ---------------------------------------------------------------------------

AiKind parseAi(Ctx& ctx, const std::string& text) {
    if (text == "passive") return AiKind::Passive;
    if (text == "neutral") return AiKind::Neutral;
    if (text == "hostile") return AiKind::Hostile;
    if (text == "sandstorm") return AiKind::Sandstorm;
    if (text == "stationary") return AiKind::Stationary;
    ctx.warn("ai_type '" + text + "' is not a behaviour this build knows; treating it as neutral");
    return AiKind::Neutral;
}

std::uint8_t parseEquipFlags(const std::string& text) {
    if (text == "Cutter") return EquipCutter;
    if (text == "ThirdEye") return EquipThirdEye;
    if (text == "Observer") return EquipObserver;
    if (text == "Antennae") return EquipAntennae;
    if (text == "Test1") return EquipTest1;
    // No flag is a perfectly good outcome for an ordinary petal.
    return EquipNone;
}

// ---------------------------------------------------------------------------
// Nested specs
// ---------------------------------------------------------------------------

ProjectileSpec parseProjectile(Ctx& ctx, const Json& owner,
                               const std::unordered_map<std::string, std::uint16_t>* petalIds) {
    ProjectileSpec spec;
    if (!owner.contains("projectile")) return spec;
    const Json& node = owner["projectile"];
    if (!node.isObject()) {
        ctx.warn(std::string("projectile is ") + typeName(node) + ", not an object; ignored");
        return spec;
    }
    spec.present = true;
    spec.count = ctx.integer(node, "count", 1, 1, 64);
    spec.distance = ctx.range(node, "distance", 0.0, 0.0, kWorldSize);
    spec.speed = ctx.range(node, "speed", 0.0, 0.0, kMaxSpeedUnits);
    spec.seekRange = ctx.range(node, "seekRange", 0.0, 0.0, kWorldSize);
    // An omitted cone is a quarter turn, not "home on anything": the reference
    // defaults it at the firing site and a seeking shot only ever corrects
    // toward something already roughly ahead of it.
    spec.seekCone = ctx.range(node, "seekCone", kPi * 0.25, 0.0, kPi);

    // The step between adjacent shots, in RADIANS, used exactly as written.
    // `flower` ships 72, which looks like degrees and is not: the reference
    // feeds the raw number to cos/sin, so its five shots wrap to roughly
    // {+0.51, -2.89, 0, +2.89, -0.51} rad off the bearing. Reading it as
    // degrees produces a tidy five-way star that hits different mobs.
    spec.spreadAngle = ctx.range(node, "spreadAngle", 0.2, -1e4, 1e4);

    if (petalIds != nullptr) {
        spec.ammoPetalId = ctx.text(node, "petalType");
        spec.ammoPetalIndex = ctx.link(*petalIds, spec.ammoPetalId, "projectile petalType");
        spec.ammoRarity = ctx.rarity(node, "petalRarity");
    }
    return spec;
}

PetalRingSpec parsePetalRing(Ctx& ctx, const Json& owner,
                             const std::unordered_map<std::string, std::uint16_t>& petalIds) {
    PetalRingSpec spec;
    if (!owner.contains("petal_ring")) return spec;
    const Json& node = owner["petal_ring"];
    if (!node.isObject()) {
        ctx.warn(std::string("petal_ring is ") + typeName(node) + ", not an object; ignored");
        return spec;
    }
    spec.petalId = ctx.text(node, "petalType");
    spec.petalIndex = ctx.link(petalIds, spec.petalId, "petal_ring petalType");
    spec.count = ctx.integer(node, "count", 0, 0, 64);
    spec.present = spec.petalIndex != kInvalidIndex && spec.count > 0;
    return spec;
}

PeriodicSpawnSpec parsePeriodicSpawn(Ctx& ctx, const Json& owner,
                                     const std::unordered_map<std::string, std::uint16_t>& mobIds) {
    PeriodicSpawnSpec spec;
    if (!owner.contains("periodic_spawn")) return spec;
    const Json& node = owner["periodic_spawn"];
    if (!node.isObject()) {
        ctx.warn(std::string("periodic_spawn is ") + typeName(node) + ", not an object; ignored");
        return spec;
    }
    spec.mobId = ctx.text(node, "mobType");
    spec.mobIndex = ctx.link(mobIds, spec.mobId, "periodic_spawn mobType");
    spec.intervalMillis = ctx.range(node, "intervalMs", kMinSpawnIntervalMillis,
                                    kMinSpawnIntervalMillis, kMaxDurationMillis);
    spec.lifetimeMillis = ctx.range(node, "lifetimeMs", 0.0, 0.0, kMaxDurationMillis);
    spec.maxAlive = ctx.integer(node, "maxAlive", 0, 0, 1000);
    spec.rarityOffset = ctx.integer(node, "spawnRarityOffset", 0,
                                    -(kRarityCount - 1), kRarityCount - 1);
    spec.present = spec.mobIndex != kInvalidIndex && spec.maxAlive > 0;
    return spec;
}

RadiationSpec parseRadiation(Ctx& ctx, const Json& owner) {
    RadiationSpec spec;
    if (!owner.contains("radiation")) return spec;
    const Json& node = owner["radiation"];
    if (!node.isObject()) {
        ctx.warn(std::string("radiation is ") + typeName(node) + ", not an object; ignored");
        return spec;
    }
    spec.radius = ctx.range(node, "radius", 0.0, 0.0, kWorldSize);
    spec.intervalMillis = ctx.range(node, "intervalMs", kMinSpawnIntervalMillis,
                                    kMinSpawnIntervalMillis, kMaxDurationMillis);
    spec.present = spec.radius > 0.0;
    return spec;
}

PetalModifiers parseModifiers(Ctx& ctx, const Json& owner) {
    PetalModifiers mods;
    if (!owner.contains("playerModifiers")) return mods;
    const Json& node = owner["playerModifiers"];
    if (!node.isObject()) {
        ctx.warn(std::string("playerModifiers is ") + typeName(node) + ", not an object; ignored");
        return mods;
    }
    mods.any = true;
    // Multipliers may be negative on purpose: yin_yang's -1 rotationSpeed
    // reverses the ring rather than slowing it.
    mods.maxHealth     = ctx.range(node, "maxHealth", 1.0, -100.0, 100.0);
    mods.speed         = ctx.range(node, "speed", 1.0, -100.0, 100.0);
    mods.range         = ctx.range(node, "range", 1.0, -100.0, 100.0);
    mods.rotationSpeed = ctx.range(node, "rotationSpeed", 1.0, -100.0, 100.0);
    mods.playerRadius  = ctx.range(node, "playerRadius", 1.0, -100.0, 100.0);
    mods.damage        = ctx.range(node, "damage", 1.0, -100.0, 100.0);

    mods.luck                  = ctx.range(node, "luck", 0.0, -100.0, 100.0);
    mods.magnetism             = ctx.range(node, "magnetism", 0.0, 0.0, kWorldSize);
    mods.aggroRadius           = ctx.range(node, "aggroRadius", 0.0, -kWorldSize, kWorldSize);
    mods.petalAttractionRadius = ctx.range(node, "petalAttractionRadius", 0.0, 0.0, kWorldSize);
    mods.poisonArmor           = ctx.range(node, "poisonArmor", 0.0, 0.0, kMaxBaseStat);

    for (const std::string& key : node.keys()) {
        static const char* kKnown[] = {
            "maxHealth", "speed", "range", "rotationSpeed", "playerRadius", "damage",
            "luck", "magnetism", "aggroRadius", "petalAttractionRadius", "poisonArmor",
        };
        bool known = false;
        for (const char* k : kKnown) known = known || key == k;
        if (!known) ctx.warn("playerModifiers has an unknown key '" + key + "'; ignored");
    }
    return mods;
}

// ---------------------------------------------------------------------------
// Entries
// ---------------------------------------------------------------------------

MobConfig parseMob(Ctx& ctx, const std::string& id, const Json& src,
                   const std::unordered_map<std::string, std::uint16_t>& mobIds,
                   const std::unordered_map<std::string, std::uint16_t>& petalIds) {
    MobConfig m;
    m.id = id;
    m.name = ctx.text(src, "name", id);
    m.description = ctx.text(src, "description");
    m.image = ctx.text(src, "image");
    m.colorRgba = readColor(ctx, src, "color", m.color);

    m.damage = ctx.range(src, "damage", 0.0, 0.0, kMaxBaseStat);
    m.health = ctx.range(src, "health", 1.0, 0.0, kMaxBaseStat);
    m.size = ctx.range(src, "size", 1.0, 0.0, kMaxSize);
    m.speed = ctx.speed(src, "speed");
    m.cooldownMillis = ctx.range(src, "cooldown", 0.0, 0.0, kMaxDurationMillis);
    m.range = ctx.range(src, "range", 0.0, 0.0, kWorldSize);
    m.visualScale = ctx.range(src, "visual_scale", 1.0, 0.0, kMaxSize);

    m.ai = parseAi(ctx, ctx.text(src, "ai_type", "neutral"));

    if (src.contains("section")) {
        const Json& sections = src["section"];
        if (!sections.isArray()) {
            ctx.warn(std::string("section is ") + typeName(sections) + ", not a list; the mob will not spawn");
        } else {
            for (const Json& entry : sections.items()) {
                const int index = entry.isNumber() ? entry.asInt(-1) : -1;
                if (index < 0 || index >= kSectionCount) {
                    ctx.warn("section " + (entry.isNumber() ? num(entry.asDouble()) : std::string(typeName(entry))) +
                             " is outside the 3x3 biome grid; ignored");
                    continue;
                }
                m.sectionMask |= static_cast<std::uint16_t>(1u << index);
            }
        }
    }
    m.spawnWeight = ctx.range(src, "spawn_weight", 1.0, 0.0, 1000.0);
    m.minRarity = ctx.rarity(src, "min_rarity");

    m.hideRotation = ctx.boolean(src, "hideRotation");
    m.noEggDrop = ctx.boolean(src, "noEggDrop");
    m.reversed = ctx.boolean(src, "reversed");
    m.noMobCollision = ctx.boolean(src, "no_mob_collision");

    // Three rules the reference states by NAME rather than in the JSON. They
    // are resolved once here so no spawner, no combat path and no despawn
    // sweep has to repeat a string compare per tick.
    m.neverAmbient = id == "target_dummy";
    m.glitchInfecting = id == "glitch" || id == "glitch_flower";
    if (id == "digger") {
        m.petHealthScale = 0.5;
        m.petDamageScale = 0.5;
    }
    if (id == "centipede" || id == "desert_centipede" || id == "evil_centipede") {
        m.segmentBodyIndex = ctx.link(mobIds, id + "_body", "centipede body");
        m.segmentCount = m.segmentBodyIndex != kInvalidIndex ? kCentipedeSegmentCount : 0;
    }

    if (src.contains("random_size")) {
        const Json& jitter = src["random_size"];
        if (!jitter.isArray() || jitter.size() != 2 ||
            !jitter[std::size_t(0)].isNumber() || !jitter[std::size_t(1)].isNumber()) {
            ctx.warn("random_size is not a [min, max] pair; the mob spawns at its base size");
        } else {
            double lo = jitter[std::size_t(0)].asDouble();
            double hi = jitter[std::size_t(1)].asDouble();
            if (!std::isfinite(lo) || !std::isfinite(hi) || lo <= 0.0 || hi <= 0.0) {
                ctx.warn("random_size [" + num(lo) + ", " + num(hi) + "] is not a usable range; ignored");
            } else {
                if (lo > hi) std::swap(lo, hi);
                m.randomSizeMin = clamp(lo, kMinSize, kMaxSize);
                m.randomSizeMax = clamp(hi, kMinSize, kMaxSize);
            }
        }
    }

    const auto readMobList = [&](const Json& list, std::vector<std::uint16_t>& out) {
        for (const Json& entry : list.items()) {
            if (!entry.isString()) {
                ctx.warn(std::string("a spawn list holds ") + typeName(entry) + " where a mob id belongs; ignored");
                continue;
            }
            const std::uint16_t index = ctx.link(mobIds, entry.asString(), "spawned mob");
            if (index != kInvalidIndex) out.push_back(index);
        }
    };

    if (src.contains("initial_spawns")) {
        const Json& list = src["initial_spawns"];
        if (!list.isArray()) ctx.warn(std::string("initial_spawns is ") + typeName(list) + ", not a list; ignored");
        else readMobList(list, m.initialSpawns);
    }
    if (src.contains("spawn_waves")) {
        const Json& waves = src["spawn_waves"];
        if (!waves.isArray()) {
            ctx.warn(std::string("spawn_waves is ") + typeName(waves) + ", not a list; ignored");
        } else {
            for (const Json& wave : waves.items()) {
                if (!wave.isArray()) {
                    ctx.warn(std::string("a spawn wave is ") + typeName(wave) + ", not a list; ignored");
                    continue;
                }
                std::vector<std::uint16_t> members;
                readMobList(wave, members);
                m.spawnWaves.push_back(std::move(members));
            }
        }
    }

    m.projectile = parseProjectile(ctx, src, &petalIds);
    m.petalRing = parsePetalRing(ctx, src, petalIds);
    m.periodicSpawn = parsePeriodicSpawn(ctx, src, mobIds);

    // The JSON states poison as damage per millisecond; the simulation thinks
    // in seconds, and converting once here keeps that unit out of every
    // affliction site.
    m.poisonPerSecond = ctx.range(src, "poison", 0.0, 0.0, kMaxPoisonPerMillis) * 1000.0;
    m.poisonDurationMillis = ctx.range(src, "poisonDuration", 0.0, 0.0, kMaxDurationMillis);

    m.emissive = ctx.boolean(src, "emissive");
    m.lightColorRgba = readColor(ctx, src, "light_color", m.lightColor);
    m.lightRadius = ctx.range(src, "light_radius", 0.0, 0.0, kWorldSize);

    m.xp.fill(1.0);
    return m;
}

PetalConfig parsePetal(Ctx& ctx, const std::string& id, const Json& src,
                       const std::unordered_map<std::string, std::uint16_t>& mobIds) {
    PetalConfig p;
    p.id = id;
    p.name = ctx.text(src, "name", id);
    p.description = ctx.text(src, "description");
    p.image = ctx.text(src, "image");
    p.colorRgba = readColor(ctx, src, "color", p.color);

    p.damage = ctx.range(src, "damage", 0.0, -kMaxBaseStat, kMaxBaseStat);

    // A missing or null health is not a petal with no hit points -- ten
    // entries use it to mean "this thing has no health pool at all" (pure
    // modifiers, projectile emitters). Zeroing it would break them on the
    // first tick, so the absence becomes a flag instead of a number.
    const bool hasHealthPool = src.contains("health") && src["health"].isNumber();
    if (src.contains("health") && !src["health"].isNumber()) {
        ctx.warn(std::string("health is ") + typeName(src["health"]) +
                 "; treating the petal as unbreakable");
    }
    p.breakable = hasHealthPool;
    p.health = hasHealthPool ? ctx.range(src, "health", 1.0, 0.0, kMaxBaseStat) : 0.0;

    p.size = ctx.range(src, "size", 1.0, kMinSize, kMaxSize);
    // Same key and same bounds as a mob's: a petal's artwork can be grown or
    // shrunk without moving the body the server damages from.
    p.visualScale = ctx.range(src, "visual_scale", 1.0, 0.0, kMaxSize);
    p.cooldownMillis = ctx.range(src, "cooldown", kDefaultPetalReloadMillis, 0.0, kMaxCooldownMillis);
    p.count = ctx.integer(src, "count", 1, 0, 64);
    p.isAdminPetal = ctx.boolean(src, "isAdminPetal");

    p.modifiers = parseModifiers(ctx, src);

    p.knockback = ctx.range(src, "knockback", p.knockback, -kMaxBaseStat, kMaxBaseStat);
    p.projectile = parseProjectile(ctx, src, nullptr);
    p.range = ctx.range(src, "range", 0.0, 0.0, kWorldSize);
    p.equipFlags = parseEquipFlags(ctx.text(src, "equipFlags"));

    p.poisonPerSecond = ctx.range(src, "poison", 0.0, 0.0, kMaxPoisonPerMillis) * 1000.0;
    p.poisonDurationMillis = ctx.range(src, "poisonDuration", 0.0, 0.0, kMaxDurationMillis);

    p.speed = ctx.range(src, "speed", 0.0, -kMaxSpeedUnits, kMaxSpeedUnits);
    p.noPhysics = ctx.boolean(src, "noPhysics");
    p.defendOnly = ctx.boolean(src, "defendOnly");
    p.clumped = ctx.boolean(src, "clumped");
    p.independentHealth = ctx.boolean(src, "independentHealth");
    p.wallCollide = ctx.boolean(src, "wallCollide");
    p.emissive = ctx.boolean(src, "emissive");

    p.burstHeal = ctx.range(src, "burstHeal", 0.0, 0.0, kMaxBaseStat);
    p.burstHealChargeMillis = ctx.range(src, "burstHealChargeMs", 0.0, 0.0, kMaxDurationMillis);
    p.passiveHeal = ctx.range(src, "passiveHeal", 0.0, 0.0, kMaxBaseStat);
    p.burstShield = ctx.range(src, "burstShield", 0.0, 0.0, kMaxBaseStat);

    if (src.contains("fixedDirection")) {
        p.hasFixedDirection = true;
        p.fixedDirection = wrapAngle(ctx.range(src, "fixedDirection", 0.0, -1e4, 1e4));
    }

    p.visualOffsetX = ctx.range(src, "visualOffsetX", 0.0, -kMaxVisualOffset, kMaxVisualOffset);
    if (src.contains("visualOffsetY")) {
        const Json& node = src["visualOffsetY"];
        const double raw = node.isNumber() ? node.asDouble() : 0.0;
        if (!node.isNumber()) {
            ctx.warn(std::string("visualOffsetY is ") + typeName(node) + "; ignored");
        } else if (!std::isfinite(raw) || std::fabs(raw) > kMaxVisualOffset) {
            // -1e100 is not an offset, it is a way of shoving a sprite off the
            // world to hide it. Say what it meant; propagating the number
            // would turn every transform it touches into an infinity.
            ctx.warn("visualOffsetY " + num(raw) +
                     " is not a drawable offset; hiding the petal instead");
            p.hidden = true;
        } else {
            p.visualOffsetY = raw;
        }
    }

    p.damageIntervalMillis = ctx.range(src, "damageCooldown", kPetalHitIntervalMillis,
                                       0.0, kMaxDurationMillis);
    p.cameraZoom = ctx.range(src, "cameraZoom", 1.0, 0.1, 10.0);
    p.lightRadius = ctx.range(src, "lightRadius", 0.0, 0.0, kWorldSize);
    p.lightColorRgba = readColor(ctx, src, "lightColor", p.lightColor);

    p.petMobId = ctx.text(src, "petMobType");
    p.petMobIndex = ctx.link(mobIds, p.petMobId, "petMobType");
    p.petMobRarity = ctx.rarity(src, "petMobRarity");
    p.petCount = ctx.integer(src, "petCount", 1, 1, 64);

    p.slowFactor = ctx.range(src, "slowFactor", 1.0, 0.0, 1.0);
    p.slowDurationMillis = ctx.range(src, "slowDuration", 0.0, 0.0, kMaxDurationMillis);

    p.spongeDamageDurationMillis = ctx.range(src, "spongeDamageDuration", 0.0, 0.0, kMaxDurationMillis);
    p.attractionForce = ctx.range(src, "attractionForce", 0.0, -kMaxBaseStat, kMaxBaseStat);
    p.webRadius = ctx.range(src, "webRadius", 0.0, 0.0, kWorldSize);
    p.radiation = parseRadiation(ctx, src);
    return p;
}

/// The object keys whose value is an object, twice over.
///
/// `sorted` is what indices are assigned from, so that a server and a client
/// reading the same file necessarily agree on what entry 17 is. `source` is
/// the file's own key order, which is what the browser iterates when it lays
/// out a catalogue -- `Object.keys(PETAL_CONFIG)` is insertion order, and a
/// shop sorted alphabetically is a different shop.
struct KeySet {
    std::vector<std::string> sorted;
    std::vector<std::string> source;
};

KeySet usableKeys(const Json& doc, const char* what, Ctx& ctx) {
    KeySet keys;
    keys.source.reserve(doc.keys().size());
    for (const std::string& key : doc.keys()) {
        if (doc[key].isObject()) {
            keys.source.push_back(key);
        } else {
            ctx.subject = std::string(what) + " '" + key + "'";
            ctx.warn(std::string("is ") + typeName(doc[key]) + ", not an object; skipped");
        }
    }
    keys.sorted = keys.source;
    std::sort(keys.sorted.begin(), keys.sorted.end());
    return keys;
}

bool endsWith(const std::string& text, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

/// The browser's darkenColor(hex, 0.7) (src/petals.ts:757-775): each channel
/// floored to 70%, re-emitted as `#rrggbb`. Only ever fed a mob's own colour,
/// which every entry in mobs.json writes as six hex digits; anything else
/// keeps its own value rather than becoming the string "NaN" as the reference
/// would.
std::string darkenHex(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') return hex;
    char out[8] = "#000000";
    for (int i = 0; i < 3; ++i) {
        const int hi = hexDigit(hex[1 + i * 2]);
        const int lo = hexDigit(hex[2 + i * 2]);
        if (hi < 0 || lo < 0) return hex;
        const int channel = static_cast<int>((hi * 16 + lo) * 0.7);
        static const char kDigits[] = "0123456789abcdef";
        out[1 + i * 2] = kDigits[(channel >> 4) & 0xF];
        out[2 + i * 2] = kDigits[channel & 0xF];
    }
    return std::string(out);
}

std::string toLower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

/// One synthesised `<mob>_egg`, exactly as src/petals.ts:784-820 builds it.
///
/// The eggs are not in petals.json and never have been: the browser generates
/// one per non-pet mob at import time, and every surface that lists petals --
/// the shop, the gallery, the drop tables, the crafting grid -- treats them as
/// ordinary entries. Generating them here rather than editing the JSON keeps
/// both builds reading one file.
PetalConfig eggPetal(const std::string& mobId, const MobConfig& mob,
                     const std::unordered_map<std::string, std::uint16_t>& mobIds) {
    PetalConfig p;
    p.id = mobId + "_egg";
    p.name = mob.name + " Egg";
    p.description = "A petal that spawns a " + toLower(mob.name) + " pet";
    p.color = "#000000";
    p.colorRgba = 0x000000FFu;
    p.image = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"32\" "
              "viewBox=\"0 0 32 32\" fill=\"none\">\n<circle r=\"13\" cx=\"16\" cy=\"16\" fill=\"" +
              mob.color + "\" stroke=\"" + darkenHex(mob.color) + "\" stroke-width=\"4\"/>\n</svg>";
    p.damage = 10.0;
    p.health = 10.0;
    p.size = 1.0;
    p.cooldownMillis = 5000.0;
    p.count = 1;
    // A pet variant is preferred where the mob has one -- two of them do --
    // and otherwise the egg hatches the mob itself.
    const std::string petId = mobId + "_pet";
    p.petMobId = mobIds.count(petId) != 0 ? petId : mobId;
    const auto it = mobIds.find(p.petMobId);
    p.petMobIndex = it == mobIds.end() ? kInvalidIndex : it->second;
    p.petMobRarity = Rarity::Common;
    return p;
}

/// Multiplicative modifier scaled for a tier.
///
/// The bonus is the distance from 1.0, so a +10% at common is +40% at unique.
/// A multiplier at or below zero is a sign flip or an off switch and is left
/// alone -- yin_yang reverses the ring with -1, and scaling that would spin
/// the ring at -4x by unique.
double scaledMultiplier(double value, double scale) {
    if (value <= 0.0) return value;
    return 1.0 + (value - 1.0) * scale;
}

} // namespace

// ---------------------------------------------------------------------------
// ContentRegistry
// ---------------------------------------------------------------------------

bool ContentRegistry::load(const std::string& dataDir, std::string& errorOut) {
    return loadFiles(joinPath(dataDir, "mobs.json"),
                     joinPath(dataDir, "petals.json"),
                     joinPath(dataDir, "mob_xp.json"),
                     errorOut);
}

bool ContentRegistry::loadFiles(const std::string& mobsPath, const std::string& petalsPath,
                                const std::string& xpPath, std::string& errorOut) {
    // Everything is built into locals and only committed at the end: a failed
    // reload has to leave a running server with the content it already had.
    std::string mobsText, petalsText, xpText;
    if (!readFile(mobsPath, mobsText)) {
        errorOut = "cannot read " + mobsPath;
        return false;
    }
    if (!readFile(petalsPath, petalsText)) {
        errorOut = "cannot read " + petalsPath;
        return false;
    }
    const bool haveXp = !xpPath.empty() && readFile(xpPath, xpText);

    Json mobsDoc, petalsDoc, xpDoc;
    std::string parseError;
    if (!Json::parse(mobsText, mobsDoc, parseError)) {
        errorOut = mobsPath + ": " + parseError;
        return false;
    }
    if (!Json::parse(petalsText, petalsDoc, parseError)) {
        errorOut = petalsPath + ": " + parseError;
        return false;
    }
    if (!mobsDoc.isObject()) { errorOut = mobsPath + ": top level is not an object"; return false; }
    if (!petalsDoc.isObject()) { errorOut = petalsPath + ": top level is not an object"; return false; }

    std::vector<std::string> warnings;
    Ctx ctx{warnings, {}};

    if (haveXp && !Json::parse(xpText, xpDoc, parseError)) {
        // XP is a balance table, not a structural dependency: losing it costs
        // every mob the same 1 XP, which is visible and survivable.
        ctx.subject = xpPath;
        ctx.warn(parseError + "; every mob will award 1 XP");
        xpDoc = Json::object();
        xpText.clear();
    }

    // Read through const references: Json's non-const operator[] INSERTS, and
    // a missing XP row would otherwise grow the document while it is scanned.
    const Json& mobsRoot = mobsDoc;
    const Json& petalsRoot = petalsDoc;
    const Json& xpRoot = xpDoc;

    const KeySet mobKeys = usableKeys(mobsRoot, "mob", ctx);
    const KeySet petalKeys = usableKeys(petalsRoot, "petal", ctx);
    if (mobKeys.sorted.empty()) { errorOut = mobsPath + ": no mob definitions"; return false; }
    if (petalKeys.sorted.empty()) { errorOut = petalsPath + ": no petal definitions"; return false; }

    // The mobs whose egg petal has to be synthesised, in the mob file's own
    // order. A pet is not something that lays an egg, and a hand-written
    // `<mob>_egg` in petals.json wins over a generated one.
    std::vector<std::string> eggMobIds;
    for (const std::string& mobId : mobKeys.source) {
        if (endsWith(mobId, "_pet")) continue;
        if (std::binary_search(petalKeys.sorted.begin(), petalKeys.sorted.end(), mobId + "_egg")) {
            continue;
        }
        eggMobIds.push_back(mobId);
    }

    // One index space over the file's petals and the generated eggs together,
    // still assigned in sorted order so that both ends of the wire agree.
    std::vector<std::string> petalIdList = petalKeys.sorted;
    petalIdList.reserve(petalIdList.size() + eggMobIds.size());
    for (const std::string& mobId : eggMobIds) petalIdList.push_back(mobId + "_egg");
    std::sort(petalIdList.begin(), petalIdList.end());

    if (mobKeys.sorted.size() > kInvalidIndex || petalIdList.size() > kInvalidIndex) {
        errorOut = "content has more entries than an index can name";
        return false;
    }

    // Ids are mapped before anything is parsed so that cross-references
    // (a nest's escorts, a petal's pet, a mob's ammunition) resolve in one
    // pass instead of needing a fixup afterwards.
    std::unordered_map<std::string, std::uint16_t> mobIds, petalIds;
    for (std::size_t i = 0; i < mobKeys.sorted.size(); ++i) {
        mobIds[mobKeys.sorted[i]] = static_cast<std::uint16_t>(i);
    }
    for (std::size_t i = 0; i < petalIdList.size(); ++i) {
        petalIds[petalIdList[i]] = static_cast<std::uint16_t>(i);
    }

    std::vector<MobConfig> mobs;
    std::vector<PetalConfig> petals;
    mobs.reserve(mobKeys.sorted.size());
    petals.reserve(petalIdList.size());

    for (const std::string& key : mobKeys.sorted) {
        ctx.subject = "mob '" + key + "'";
        mobs.push_back(parseMob(ctx, key, mobsRoot[key], mobIds, petalIds));
    }
    for (const std::string& key : petalIdList) {
        if (std::binary_search(petalKeys.sorted.begin(), petalKeys.sorted.end(), key)) {
            ctx.subject = "petal '" + key + "'";
            petals.push_back(parsePetal(ctx, key, petalsRoot[key], mobIds));
            continue;
        }
        // Generated: the id is `<mob>_egg` and the mob is one this build has,
        // because that is the only way the id got onto the list.
        const std::string mobId = key.substr(0, key.size() - 4);
        petals.push_back(eggPetal(mobId, mobs[mobIds[mobId]], mobIds));
    }

    // Catalogue order: the file's own keys first, then the eggs in mob-file
    // order, because that is where the browser appends them to
    // BASE_PETAL_CONFIGS and therefore where Object.keys() reports them.
    std::vector<std::uint16_t> petalOrder;
    petalOrder.reserve(petalIdList.size());
    for (const std::string& key : petalKeys.source) petalOrder.push_back(petalIds[key]);
    for (const std::string& mobId : eggMobIds) petalOrder.push_back(petalIds[mobId + "_egg"]);

    // XP. A tier the table omits awards 1, except apex: the tables stop at
    // unique, and apex stats are a 3x step above unique everywhere else, so
    // deriving it keeps an apex kill from paying the same as a common one.
    constexpr int kUnique = static_cast<int>(Rarity::Unique);
    constexpr int kApex = static_cast<int>(Rarity::Apex);
    for (MobConfig& m : mobs) {
        const Json& row = xpRoot[m.id];
        if (!row.isObject()) {
            if (haveXp) {
                ctx.subject = "mob '" + m.id + "'";
                ctx.warn("has no XP table; every tier awards 1");
            }
            continue;
        }
        for (int i = 0; i < kRarityCount; ++i) {
            const Json& value = row[kRarityNames[static_cast<std::size_t>(i)]];
            if (!value.isNumber()) continue;
            const double xp = value.asDouble();
            if (!std::isfinite(xp) || xp < 0.0) {
                ctx.subject = "mob '" + m.id + "'";
                ctx.warn(std::string("XP for ") + kRarityNames[static_cast<std::size_t>(i)] +
                         " is " + num(xp) + "; using 1");
                continue;
            }
            m.xp[static_cast<std::size_t>(i)] = xp;
        }
        if (!row.contains("apex") && row.contains("unique")) {
            m.xp[kApex] = m.xp[kUnique] * 3.0;
        }
    }

    std::uint32_t hash = net::contentHash(mobsText);
    hash = net::contentHash(petalsText, hash);
    // A missing XP file folds nothing in, so a build that ships one and a
    // build that does not disagree at the handshake -- which is correct: they
    // do not have the same content.
    if (!xpText.empty()) hash = net::contentHash(xpText, hash);

    mobs_ = std::move(mobs);
    petals_ = std::move(petals);
    mobIds_ = std::move(mobIds);
    petalIds_ = std::move(petalIds);
    petalOrder_ = std::move(petalOrder);
    warnings_ = std::move(warnings);
    hash_ = hash;
    errorOut.clear();
    return true;
}

const MobConfig& ContentRegistry::mob(std::uint16_t index) const {
    static const MobConfig kMissing = [] {
        MobConfig m;
        m.id = "<unknown>";
        m.name = "Unknown";
        return m;
    }();
    return index < mobs_.size() ? mobs_[index] : kMissing;
}

const PetalConfig& ContentRegistry::petal(std::uint16_t index) const {
    static const PetalConfig kMissing = [] {
        PetalConfig p;
        p.id = "<unknown>";
        p.name = "Unknown";
        p.breakable = false;
        return p;
    }();
    return index < petals_.size() ? petals_[index] : kMissing;
}

std::uint16_t ContentRegistry::mobIndex(const std::string& id) const {
    auto it = mobIds_.find(id);
    return it == mobIds_.end() ? kInvalidIndex : it->second;
}

std::uint16_t ContentRegistry::petalIndex(const std::string& id) const {
    auto it = petalIds_.find(id);
    return it == petalIds_.end() ? kInvalidIndex : it->second;
}

MobStats ContentRegistry::mobStats(std::uint16_t index, Rarity r) const {
    const MobConfig& c = mob(index);
    const int tier = clamp(rarityIndex(r), 0, kRarityCount - 1);
    const std::size_t t = static_cast<std::size_t>(tier);

    MobStats s;
    // `size` already carries the rarity step, so mass -- which is area -- gets
    // the rarity growth for free and a mythic shrugs off what launches a bee.
    const double scaledSize = c.size * kMobSizeScale[t];
    s.health = c.health * kMobHealthScale[t];
    s.damage = c.damage * kMobDamageScale[t];
    s.radius = scaledSize * kMobBaseRadius;
    s.mass = scaledSize * scaledSize;
    s.speed = c.speed * kMobSpeedUnitsPerSecond;
    // These PURSUE at the flower's 300 u/s so a fleeing player cannot outrun
    // them -- but only while pursuing. Every other branch (the idle drift, a
    // flee) reads the authored speed, so the override belongs on its own field
    // rather than on `speed`: folded in, an unprovoked bee cruises at 135 u/s
    // instead of ~36, and a ladybug at twenty times its reference drift.
    s.playerSpeedChaser =
        c.id == "bee" || c.id == "ladybug" || c.id == "shiny_ladybug" ||
        c.id == "dark_ladybug" || c.id == "soldier_ant" || c.id == "worker_ant" ||
        c.id == "baby_ant" || c.id == "soldier_fire_ant" ||
        c.id == "worker_fire_ant" || c.id == "baby_fire_ant";
    s.chaseSpeed = s.playerSpeedChaser ? kPlayerMaxSpeed : s.speed;
    s.xp = c.xp[t];
    s.aggroRange = c.range;
    const auto tieredRange = [&](const std::array<double, kRarityCount>& values) {
        s.aggroRange = values[t];
    };
    if (c.id == "soldier_ant" || c.id == "worker_ant" || c.id == "shiny_ladybug" ||
        c.id == "beetle" || c.id == "hel_beetle" || c.id == "starfish" ||
        c.id == "hornet" || c.id == "mantis" || c.id == "glitch") {
        tieredRange({c.range, 500, 600, 750, 900, 1100, 1300, 1500, 1700, c.range});
    } else if (c.id == "soldier_fire_ant") {
        tieredRange({c.range, 700, 900, 1100, 1300, 1500, 1700, 1900, 2100, c.range});
    } else if (c.id == "jellyfish") {
        tieredRange({c.range, 700, 800, 950, 1100, 1300, 1500, 1700, 1900, c.range});
    } else if (c.id == "spider") {
        tieredRange({c.range, 800, 1000, 1200, 1400, 1600, 1800, 2000, 2200, c.range});
    } else if (c.id == "glitch_flower") {
        tieredRange({c.range, 850, 1000, 1150, 1300, 1500, 1700, 1900, 2100, c.range});
    } else if (c.id == "ladybug" && tier >= rarityIndex(Rarity::Rare) &&
               tier < rarityIndex(Rarity::Apex)) {
        static constexpr std::array<double, 7> kLadybugRange = {
            350, 500, 700, 900, 1100, 1300, 1500,
        };
        s.aggroRange = kLadybugRange[static_cast<std::size_t>(tier - rarityIndex(Rarity::Rare))];
    }
    s.attackCooldownMillis = c.cooldownMillis;
    // Poison is damage, so it rides the damage ladder; otherwise an apex
    // centipede's bite would tick for exactly what a common one's does.
    s.poisonPerSecond = c.poisonPerSecond * kMobDamageScale[t];
    s.poisonDurationMillis = c.poisonDurationMillis;
    s.visualScale = c.visualScale;
    s.spawnWeight = c.spawnWeight;
    s.ai = c.ai;
    if (c.id == "bee" && tier >= rarityIndex(Rarity::Rare)) s.ai = AiKind::Neutral;
    if (c.id == "ladybug" && tier >= rarityIndex(Rarity::Rare) &&
        tier < rarityIndex(Rarity::Apex)) s.ai = AiKind::Neutral;
    if ((c.id == "centipede" || c.id == "centipede_body" ||
         c.id == "desert_centipede" || c.id == "desert_centipede_body") &&
        tier >= rarityIndex(Rarity::Epic)) {
        s.ai = AiKind::Neutral;
    }
    // min_rarity is enforced in exactly one place: below its tier the mob
    // belongs to no section, and every spawner already filters on that.
    s.sectionMask = tier < rarityIndex(c.minRarity) ? 0 : c.sectionMask;
    return s;
}

PetalStats ContentRegistry::petalStats(std::uint16_t index, Rarity r) const {
    const PetalConfig& c = petal(index);
    const Rarity tier = clampRarity(rarityIndex(r));
    const double stat = petalStatScale(tier);
    const double heal = petalHealScale(tier);
    const double modifier = petalModifierScale(tier);

    PetalStats s;
    s.damage = c.damage * stat;
    s.health = c.health * stat;
    s.reloadMillis = c.cooldownMillis;
    if (c.id == "yggdrasil") {
        // TypeScript overrides every row: it is always a 1/1 petal and its
        // cooldown halves at each rarity from 512 seconds.
        s.damage = 1.0;
        s.health = 1.0;
        s.reloadMillis = 512000.0 / std::pow(2.0, rarityIndex(tier));
    } else if (c.id == "lightning") {
        s.health = 10.0;
    } else if (c.id == "bubble") {
        s.reloadMillis *= std::pow(0.85, rarityIndex(tier));
        s.reloadMillis = std::max(50.0, s.reloadMillis);
    }
    s.poisonPerSecond = c.poisonPerSecond * stat;
    s.poisonDurationMillis = c.poisonDurationMillis;
    s.heal = c.burstHeal * heal;
    s.healChargeMillis = c.burstHealChargeMillis;
    s.passiveHealPerSecond = c.passiveHeal * heal;
    // TypeScript keeps ordinary petal knockback flat across rarities. Jelly is
    // the one intentional exception: its per-rarity values are literal
    // overrides in petals.ts, not another copy of the damage multiplier.
    s.knockback = c.knockback;
    if (c.id == "jelly") {
        static constexpr std::array<double, kRarityCount> kJellyKnockback = {
            15.0, 50.0, 100.0, 250.0, 500.0,
            1800.0, 10000.0, 25000.0, 50000.0, 100000.0,
        };
        s.knockback = kJellyKnockback[static_cast<std::size_t>(rarityIndex(tier))];
    }
    s.shield = c.burstShield * heal;
    // A slow's rarity is spent on landing it at all (stallPower), not on
    // making it deeper, so the factor and its duration are flat.
    s.slowFactor = c.slowFactor;
    s.slowDurationMillis = c.slowDurationMillis;
    if (c.id == "pincer" || c.id == "honey") {
        s.slowDurationMillis = c.slowDurationMillis * (1.0 + rarityIndex(tier) * 0.375);
    }
    s.webRadius = c.webRadius * (1.0 + (rarityIndex(tier) / 8.0) * 1.2);
    s.spongeDamageDurationMillis =
        c.spongeDamageDurationMillis * (1.0 + rarityIndex(tier) * 0.5);
    s.attractionForce = c.attractionForce;
    s.radius = c.size * kPetalRadiusPerSize;
    s.size = c.size;
    s.visualScale = c.visualScale;
    s.damageIntervalMillis = c.damageIntervalMillis;
    // `count` is flat for almost every petal, and the two exceptions are
    // literal per-rarity overrides in the reference's RARITY_OVERRIDES table
    // (src/petals.ts:307-334 and :699-726) rather than another scaling rule.
    // They ride here rather than in petals.json because that file is shared
    // verbatim with the browser build, which reads its overrides from
    // TypeScript.
    s.count = c.count;
    if (c.id == "light" || c.id == "pollen") {
        static constexpr std::array<int, kRarityCount> kLightCount = {
            1, 2, 2, 3, 3, 5, 5, 5, 5, 5,
        };
        static constexpr std::array<int, kRarityCount> kPollenCount = {
            1, 2, 2, 2, 3, 3, 5, 5, 5, 7,
        };
        const auto t = static_cast<std::size_t>(rarityIndex(tier));
        s.count = c.id == "light" ? kLightCount[t] : kPollenCount[t];
    }
    s.breakable = c.breakable;
    s.cameraZoom = scaledMultiplier(c.cameraZoom, modifier);

    s.modifiers = c.modifiers;
    s.modifiers.maxHealth = scaledMultiplier(c.modifiers.maxHealth, modifier);
    s.modifiers.speed = scaledMultiplier(c.modifiers.speed, modifier);
    s.modifiers.range = scaledMultiplier(c.modifiers.range, modifier);
    s.modifiers.rotationSpeed = scaledMultiplier(c.modifiers.rotationSpeed, modifier);
    s.modifiers.playerRadius = scaledMultiplier(c.modifiers.playerRadius, modifier);
    s.modifiers.damage = scaledMultiplier(c.modifiers.damage, modifier);
    s.modifiers.luck = c.modifiers.luck * modifier;
    s.modifiers.magnetism = c.modifiers.magnetism * modifier;
    s.modifiers.aggroRadius = c.modifiers.aggroRadius * modifier;
    s.modifiers.petalAttractionRadius = c.modifiers.petalAttractionRadius * modifier;
    s.modifiers.poisonArmor = c.modifiers.poisonArmor * modifier;

    const std::size_t ti = static_cast<std::size_t>(rarityIndex(tier));
    if (c.id == "clover") {
        static constexpr std::array<double, kRarityCount> values = {
            0.08, 0.12, 0.17, 0.24, 0.35, 0.5, 0.72, 1.04, 1.5, 2.0,
        };
        s.modifiers.luck = values[ti];
    } else if (c.id == "faster") {
        static constexpr std::array<double, kRarityCount> values = {
            1.1, 1.2, 1.3, 1.4, 1.6, 1.8, 2.1, 2.7, 3.5, 4.5,
        };
        s.modifiers.rotationSpeed = values[ti];
    } else if (c.id == "powder") {
        static constexpr std::array<double, kRarityCount> values = {
            1.1, 1.1, 1.4, 1.6, 1.8, 2.0, 2.2, 2.4, 2.6, 2.8,
        };
        s.modifiers.speed = values[ti];
    } else if (c.id == "soil") {
        static constexpr std::array<double, kRarityCount> health = {
            1.1, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9,
        };
        static constexpr std::array<double, kRarityCount> speed = {
            0.95, 0.95, 0.9, 0.85, 0.8, 0.75, 0.7, 0.65, 0.6, 0.6,
        };
        static constexpr std::array<double, kRarityCount> radius = {
            1.05, 1.05, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8,
        };
        s.modifiers.maxHealth = health[ti];
        s.modifiers.speed = speed[ti];
        s.modifiers.playerRadius = radius[ti];
    } else if (c.id == "air") {
        static constexpr std::array<double, kRarityCount> values = {
            1.1, 1.2, 1.6, 1.8, 2.0, 2.2, 2.4, 2.6, 2.8, 3.0,
        };
        s.modifiers.playerRadius = values[ti];
    } else if (c.id == "lotus") {
        s.modifiers.poisonArmor = 5.0 * stat;
    } else if (c.id == "lentil") {
        static constexpr std::array<double, kRarityCount> radius = {
            20, 29, 38, 47, 56, 64, 73, 82, 91, 100,
        };
        static constexpr std::array<double, kRarityCount> force = {
            2000, 2889, 3778, 4667, 5556, 6444, 7333, 8222, 9111, 10000,
        };
        s.modifiers.petalAttractionRadius = radius[ti];
        s.attractionForce = force[ti];
    }
    return s;
}

// ---------------------------------------------------------------------------
// Process-wide content
// ---------------------------------------------------------------------------

namespace {
ContentRegistry& registry() {
    static ContentRegistry instance;
    return instance;
}
} // namespace

const ContentRegistry& content() { return registry(); }

bool loadContent(const std::string& dataDir, std::string& errorOut) {
    return registry().load(dataDir, errorOut);
}

} // namespace flix
