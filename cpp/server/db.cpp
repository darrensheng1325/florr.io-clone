#include "server/db.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shared/game/constants.h"

namespace flix {
namespace {

std::string toLower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

/// Days since the Unix epoch for a proleptic-Gregorian y/m/d, and back again.
/// Hinnant's civil-calendar algorithms: the streak rule is per UTC calendar
/// day, and going through the C library's time functions would drag in a
/// timezone and a locale to answer a question that is pure arithmetic.
std::int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::string civilFromDays(std::int64_t days) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<std::uint64_t>(days - era * 146097);
    const std::uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const std::uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::uint64_t mp = (5 * doy + 2) / 153;
    const std::uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::uint64_t m = mp + (mp < 10 ? 3 : -9);
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%04lld-%02llu-%02llu",
                  static_cast<long long>(y + (m <= 2)),
                  static_cast<unsigned long long>(m), static_cast<unsigned long long>(d));
    return buffer;
}

/// Days since the epoch for a stored "YYYY-MM-DD", or nullopt when the record
/// holds something else. A record written by hand, or by a version that stored
/// a timestamp, must reset the streak rather than crash the login.
std::optional<std::int64_t> parseUtcDate(const std::string& text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return std::nullopt;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (text[i] < '0' || text[i] > '9') return std::nullopt;
    }
    const int year = std::stoi(text.substr(0, 4));
    const int month = std::stoi(text.substr(5, 2));
    const int day = std::stoi(text.substr(8, 2));
    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
    return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
}

bool fileExists(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

bool readWholeFile(const std::string& path, std::string& out, std::string& errorOut) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { errorOut = path + ": cannot open for reading"; return false; }
    out.clear();
    char chunk[64 * 1024];
    while (std::size_t got = std::fread(chunk, 1, sizeof(chunk), f)) out.append(chunk, got);
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) { errorOut = path + ": read error"; return false; }
    return true;
}

bool isOneOf(const std::string& key, std::initializer_list<const char*> known) {
    for (const char* k : known) {
        if (key == k) return true;
    }
    return false;
}

/// Copies every key of `source` that `known` does not claim into `out`. This
/// is what keeps a field the rewrite has never heard of alive across a save.
void collectExtras(const Json& source, std::initializer_list<const char*> known, Json& out) {
    out = Json::object();
    if (!source.isObject()) return;
    for (const std::string& key : source.keys()) {
        if (!isOneOf(key, known)) out[key] = source[key];
    }
}

void appendExtras(const Json& extra, Json& out) {
    if (!extra.isObject()) return;
    for (const std::string& key : extra.keys()) out[key] = extra[key];
}

/// Timestamps go out as doubles rather than through Json's integer
/// constructors: std::int64_t is `long` on Linux and `long long` on macOS, and
/// only one of those has an exact overload. Unix millis are well inside a
/// double's exact integer range for the next 200,000 years.
Json millisJson(std::int64_t millis) { return Json(static_cast<double>(millis)); }

// -- record <-> JSON ---------------------------------------------------------

PlayerRecord playerFromJson(const Json& value) {
    PlayerRecord record;
    if (!value.isObject()) return record;

    record.totalXp = value["totalXP"].asDouble(0);
    record.stars = value["stars"].asInt(0);
    record.dailyStreak = value["dailyStreak"].asInt(0);
    record.lastStreakDate = value["lastStreakDate"].asString();
    record.renderFlags = static_cast<std::uint32_t>(value["renderFlags"].asDouble(0));
    record.equippedSkinId = value["equippedSkinId"].asString();

    if (value["inventory"].isObject()) record.inventory = value["inventory"];
    if (value["mobKills"].isObject()) record.mobKills = value["mobKills"];
    if (value["skills"].isObject()) {
        // Branches this build does not know are dropped rather than kept as
        // raw JSON: a tier only means something to the code that applies it,
        // and round-tripping one would let it silently reappear as a bonus.
        const Json& skills = value["skills"];
        for (const std::string& key : skills.keys()) {
            const SkillId id = skillFromKey(key);
            if (id == SkillId::Count) continue;
            const int tier = rarityIndex(parseRarity(skills[key].asString()));
            if (tier < skillTierCount(id)) record.skills.set(id, tier);
        }
    }
    if (value["loadout"].isArray()) {
        for (const Json& slot : value["loadout"].items()) {
            if (slot.isObject()) record.loadout.push_back(StoredItem::fromJson(slot));
            else record.loadout.push_back(std::nullopt);
        }
    }

    collectExtras(value, {"totalXP", "stars", "dailyStreak", "lastStreakDate", "renderFlags",
                          "equippedSkinId", "inventory", "mobKills", "loadout", "skills", "tp"},
                  record.extra);
    return record;
}

Json playerToJson(const PlayerRecord& record) {
    Json out = Json::object();
    out["totalXP"] = record.totalXp;
    out["inventory"] = record.inventory;
    if (!record.loadout.empty()) {
        Json slots = Json::array();
        for (const std::optional<StoredItem>& slot : record.loadout) {
            slots.push(slot ? slot->toJson() : Json());
        }
        out["loadout"] = slots;
    }
    if (record.mobKills.isObject() && record.mobKills.size() > 0) out["mobKills"] = record.mobKills;
    Json skills = Json::object();
    for (int i = 0; i < kSkillCount; ++i) {
        const int tier = record.skills.tier[static_cast<std::size_t>(i)];
        if (tier < 0) continue;
        skills[kSkillKeys[static_cast<std::size_t>(i)]] = std::string(rarityName(clampRarity(tier)));
    }
    if (skills.size() > 0) {
        out["skills"] = skills;
        // `tp` is derived here and never read back, but the TypeScript build
        // and every backup tool expect to find it beside `skills`.
        out["tp"] = record.talentPoints();
    }
    // Optional fields are written only when they carry information. A missing
    // key and an explicit 0/false read identically to every consumer, and this
    // file has one record per account -- the noise is not free.
    if (record.stars != 0) out["stars"] = record.stars;
    if (record.renderFlags != 0) out["renderFlags"] = static_cast<double>(record.renderFlags);
    if (!record.equippedSkinId.empty()) out["equippedSkinId"] = record.equippedSkinId;
    if (record.dailyStreak != 0) out["dailyStreak"] = record.dailyStreak;
    if (!record.lastStreakDate.empty()) out["lastStreakDate"] = record.lastStreakDate;
    appendExtras(record.extra, out);
    return out;
}

Account accountFromJson(const Json& value, const std::string& storedKey) {
    Account account;
    if (!value.isObject()) { account.username = storedKey; return account; }

    account.id = value["id"].asString();
    account.username = value["username"].asString(storedKey);
    account.passwordHash = value["password"].asString();
    account.isPlainText = value["isPlainText"].asBool(false);
    account.admin = value["admin"].asBool(false);
    account.lastActiveAtMillis = static_cast<std::int64_t>(value["lastActiveAt"].asDouble(0));
    account.muted = value["muted"].asBool(false);
    account.mutedAtMillis = static_cast<std::int64_t>(value["mutedAt"].asDouble(0));
    account.mutedBy = value["mutedBy"].asString();
    account.createdAtMillis = static_cast<std::int64_t>(value["createdAt"].asDouble(0));
    account.createdFromHash = value["createdFromHash"].asString();

    collectExtras(value, {"id", "username", "password", "isPlainText", "admin", "lastActiveAt",
                          "muted", "mutedAt", "mutedBy", "createdAt", "createdFromHash"},
                  account.extra);
    return account;
}

Json accountToJson(const Account& account) {
    Json out = Json::object();
    out["id"] = account.id;
    out["username"] = account.username;
    out["password"] = account.passwordHash;
    if (account.isPlainText) out["isPlainText"] = true;
    if (account.admin) out["admin"] = true;
    if (account.lastActiveAtMillis != 0) out["lastActiveAt"] = millisJson(account.lastActiveAtMillis);
    if (account.muted) {
        out["muted"] = true;
        if (account.mutedAtMillis != 0) out["mutedAt"] = millisJson(account.mutedAtMillis);
        if (!account.mutedBy.empty()) out["mutedBy"] = account.mutedBy;
    }
    // Both omitted when unset, so accounts written before the registration
    // limits existed round-trip through this build unchanged.
    if (account.createdAtMillis != 0) out["createdAt"] = millisJson(account.createdAtMillis);
    if (!account.createdFromHash.empty()) out["createdFromHash"] = account.createdFromHash;
    appendExtras(account.extra, out);
    return out;
}

Database::Session sessionFromJson(const Json& value, const std::string& storedKey) {
    Database::Session session;
    session.tokenHash = value["tokenHash"].asString(storedKey);
    session.userId = value["userId"].asString();
    session.username = value["username"].asString();
    session.createdAtMillis = static_cast<std::int64_t>(value["createdAt"].asDouble(0));
    session.expiresAtMillis = static_cast<std::int64_t>(value["expiresAt"].asDouble(0));
    return session;
}

Json sessionToJson(const Database::Session& session) {
    Json out = Json::object();
    out["tokenHash"] = session.tokenHash;
    out["userId"] = session.userId;
    out["username"] = session.username;
    out["createdAt"] = millisJson(session.createdAtMillis);
    out["expiresAt"] = millisJson(session.expiresAtMillis);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// StoredItem
// ---------------------------------------------------------------------------

StoredItem StoredItem::fromJson(const Json& value) {
    StoredItem item;
    if (!value.isObject()) return item;

    item.type = value["type"].asString("petal");
    item.rarity = parseRarity(value["rarity"].asString("common"));
    item.petalType = value["petalType"].asString();
    if (value["health"].isNumber()) {
        item.health = value["health"].asDouble();
        item.hasHealth = true;
    }
    if (value["maxHealth"].isNumber()) {
        item.maxHealth = value["maxHealth"].asDouble();
        item.hasMaxHealth = true;
    }
    collectExtras(value, {"type", "rarity", "petalType", "health", "maxHealth"}, item.extra);
    return item;
}

Json StoredItem::toJson() const {
    Json out = Json::object();
    out["type"] = type;
    out["rarity"] = std::string(rarityName(rarity));
    if (!petalType.empty()) out["petalType"] = petalType;
    if (hasHealth) out["health"] = health;
    if (hasMaxHealth) out["maxHealth"] = maxHealth;
    appendExtras(extra, out);
    return out;
}

// ---------------------------------------------------------------------------
// PlayerRecord
// ---------------------------------------------------------------------------

int PlayerRecord::itemCount(Rarity rarity, const std::string& itemType) const {
    const Json& bucket = inventory[std::string(rarityName(rarity))];
    if (!bucket.isObject()) return 0;
    return bucket[itemType].asInt(0);
}

void PlayerRecord::setItemCount(Rarity rarity, const std::string& itemType, int count) {
    const std::string tier = rarityName(rarity);
    if (count <= 0) {
        if (!inventory.contains(tier)) return;
        Json& bucket = inventory[tier];
        if (!bucket.isObject()) return;
        bucket.erase(itemType);
        // An empty tier is dropped so the file does not accumulate rubble from
        // every petal a player has ever crafted away.
        if (bucket.size() == 0) inventory.erase(tier);
        return;
    }
    // operator[] promotes a missing or non-object tier into an object.
    inventory[tier][itemType] = count;
}

void PlayerRecord::addItem(Rarity rarity, const std::string& itemType, int delta) {
    setItemCount(rarity, itemType, itemCount(rarity, itemType) + delta);
}

int PlayerRecord::killCount(const std::string& mobType, Rarity rarity) const {
    const Json& bucket = mobKills[mobType];
    if (!bucket.isObject()) return 0;
    return bucket[std::string(rarityName(rarity))].asInt(0);
}

void PlayerRecord::recordKill(const std::string& mobType, Rarity rarity) {
    const std::string tier = rarityName(rarity);
    mobKills[mobType][tier] = killCount(mobType, rarity) + 1;
}

int PlayerRecord::talentPoints() const {
    return availableTalentPoints(levelFromTotalXp(totalXp).level, skills);
}

// ---------------------------------------------------------------------------
// Database: lifecycle and persistence
// ---------------------------------------------------------------------------

Database::~Database() {
    // The rate limit means the last change of a session is usually still in
    // memory when the process is asked to stop. Flushing here is what turns
    // an orderly shutdown into a lossless one.
    if (dirty_ && !loadFailed_ && !path_.empty()) save();
}

void Database::reset() {
    users_.clear();
    players_.clear();
    sessions_.clear();
    usersByLowerName_.clear();
    usersById_.clear();
    otherTop_ = Json::object();
    topKeyOrder_.clear();
    path_.clear();
    loadFailed_ = false;
    dirty_ = false;
    everWrote_ = false;
    lastWriteMillis_ = 0;
}

bool Database::load(const std::string& path, std::string& errorOut) {
    reset();
    path_ = path;

    if (!fileExists(path)) {
        // A first run, not a failure. Dirty so the file appears on the first
        // save rather than only once a player happens to earn something.
        dirty_ = true;
        return true;
    }

    std::string text;
    if (!readWholeFile(path, text, errorOut)) {
        loadFailed_ = true;
        return false;
    }

    Json root;
    std::string parseError;
    if (!Json::parse(text, root, parseError)) {
        loadFailed_ = true;
        errorOut = path + ": " + parseError;
        return false;
    }
    if (!parseRoot(root, errorOut)) {
        loadFailed_ = true;
        return false;
    }
    pruneExpiredSessions();
    return true;
}

bool Database::parseRoot(const Json& root, std::string& errorOut) {
    if (!root.isObject()) {
        errorOut = path_ + ": top level is not a JSON object";
        return false;
    }
    topKeyOrder_ = root.keys();

    for (const std::string& key : root.keys()) {
        const Json& value = root[key];
        // A collection that is present but is not an object is corruption, not
        // an old schema. Treating it as "no accounts" and carrying on is how a
        // damaged file becomes an empty one.
        if (key == "players" || key == "users" || key == "sessions") {
            if (!value.isObject()) {
                errorOut = path_ + ": \"" + key + "\" is not a JSON object";
                return false;
            }
        }

        if (key == "players") {
            for (const std::string& userId : value.keys()) {
                players_.insert(userId) = playerFromJson(value[userId]);
            }
        } else if (key == "users") {
            for (const std::string& name : value.keys()) {
                Account& account = users_.insert(name);
                account = accountFromJson(value[name], name);
                indexUser(name, account);
            }
        } else if (key == "sessions") {
            for (const std::string& tokenHash : value.keys()) {
                sessions_.insert(tokenHash) = sessionFromJson(value[tokenHash], tokenHash);
            }
        } else {
            otherTop_[key] = value;
        }
    }
    return true;
}

Json& Database::rawArrayTable(const std::string& key) {
    Json& table = otherTop_[key];
    if (!table.isArray()) table = Json::array();
    if (std::find(topKeyOrder_.begin(), topKeyOrder_.end(), key) == topKeyOrder_.end()) {
        topKeyOrder_.push_back(key);
    }
    return table;
}

Json& Database::rawTable(const std::string& key) {
    Json& table = otherTop_[key];
    if (!table.isObject()) table = Json::object();
    if (std::find(topKeyOrder_.begin(), topKeyOrder_.end(), key) == topKeyOrder_.end()) {
        topKeyOrder_.push_back(key);
    }
    return table;
}

Json Database::toJson() const {
    Json players = Json::object();
    for (const std::string& userId : players_.keys()) {
        players[userId] = playerToJson(*players_.find(userId));
    }
    Json users = Json::object();
    for (const std::string& name : users_.keys()) {
        users[name] = accountToJson(*users_.find(name));
    }
    Json sessions = Json::object();
    for (const std::string& tokenHash : sessions_.keys()) {
        sessions[tokenHash] = sessionToJson(*sessions_.find(tokenHash));
    }

    // Rebuild in the order the file was read in, so a save diffs cleanly
    // against the backup it came from.
    Json root = Json::object();
    bool wrotePlayers = false, wroteUsers = false, wroteSessions = false;
    for (const std::string& key : topKeyOrder_) {
        if (key == "players") { root["players"] = players; wrotePlayers = true; }
        else if (key == "users") { root["users"] = users; wroteUsers = true; }
        else if (key == "sessions") { root["sessions"] = sessions; wroteSessions = true; }
        else { root[key] = otherTop_[key]; }
    }
    if (!wrotePlayers) root["players"] = players;
    if (!wroteUsers) root["users"] = users;
    // `sessions` is created lazily, matching the old server: a database that
    // has never issued one does not grow an empty key for it.
    if (!wroteSessions && sessions.size() > 0) root["sessions"] = sessions;
    return root;
}

bool Database::save() {
    // The whole point of the flag. The in-memory database is the empty
    // default when a load fails, and writing that out turns a file an operator
    // can still repair into total, permanent account loss.
    if (loadFailed_ || path_.empty()) return false;

    const std::string text = toJson().dump(0);
    const std::string tmp = path_ + ".tmp";

    std::FILE* file = std::fopen(tmp.c_str(), "wb");
    if (!file) return false;
    const std::size_t written = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), file);
    const bool flushed = std::fflush(file) == 0;
    if (flushed) ::fsync(::fileno(file));
    const bool closed = std::fclose(file) == 0;
    if (written != text.size() || !flushed || !closed) {
        std::remove(tmp.c_str());
        return false;
    }

    // Verify before promoting. The failure this actually guards against is
    // truncation -- the process dying mid-write, or a full disk -- and a
    // truncated file is a SHORT one, which a length compare catches for free.
    // Re-parsing the file instead would cost megabytes of work per save to
    // catch a corruption mode a healthy filesystem does not produce.
    struct stat info {};
    if (::stat(tmp.c_str(), &info) != 0 ||
        static_cast<std::size_t>(info.st_size) != text.size()) {
        std::remove(tmp.c_str());
        return false;
    }

    // rename(2) is atomic within a filesystem: either the old file or the new
    // one is there, never a half of either.
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    dirty_ = false;
    return true;
}

std::string Database::backupDirectory() const {
    // One level above the runtime directory, which is what puts snapshots
    // outside dist/ and therefore out of reach of a redeploy that replaces it.
    const std::size_t slash = path_.find_last_of('/');
    // A bare `inventory.json`: the working directory IS the level above.
    if (slash == std::string::npos) return "db_backups";
    const std::string directory = path_.substr(0, slash);
    const std::size_t parent = directory.find_last_of('/');
    // A single relative component ("dist/inventory.json"), so again the
    // working directory.
    if (parent == std::string::npos) return "db_backups";
    // A single ABSOLUTE component ("/tmp/inventory.json"). The level above is
    // the filesystem root, which is not a place anything writes; the database's
    // own directory is as far up as this can usefully go.
    if (parent == 0) return directory + "/db_backups";
    return directory.substr(0, parent) + "/db_backups";
}

bool Database::backup(const std::string& label, BackupInfo& out, std::string& errorOut) {
    // Same latch as save(): the in-memory database is the empty default when a
    // load fails, and a "backup" of that is a file that looks like a rescue and
    // is total account loss.
    if (loadFailed_) {
        errorOut = "the database failed to load; refusing to snapshot an empty one";
        return false;
    }

    std::string safeLabel;
    for (const char c : toLower(label)) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (ok) safeLabel.push_back(c);
        else if (!safeLabel.empty() && safeLabel.back() != '-') safeLabel.push_back('-');
        if (safeLabel.size() >= 40) break;
    }
    while (!safeLabel.empty() && safeLabel.back() == '-') safeLabel.pop_back();
    if (safeLabel.empty()) safeLabel = "manual";

    const std::string directory = backupDirectory();
    if (::mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST) {
        errorOut = "could not create " + directory;
        return false;
    }

    // The reference's `new Date().toISOString()` with ':' and '.' replaced,
    // because a colon is not a filename character everywhere this runs.
    const std::int64_t millis = nowMillis();
    const std::int64_t days = millis / 86400000 - (millis % 86400000 < 0 ? 1 : 0);
    std::int64_t rest = millis - days * 86400000;
    char stamp[64];
    std::snprintf(stamp, sizeof stamp, "%sT%02d-%02d-%02d-%03dZ", civilFromDays(days).c_str(),
                  static_cast<int>(rest / 3600000), static_cast<int>(rest / 60000 % 60),
                  static_cast<int>(rest / 1000 % 60), static_cast<int>(rest % 1000));

    const std::string file = directory + "/inventory-" + stamp + "-" + safeLabel + ".json";
    // Pretty-printed, as the reference writes it: a backup is read by a person
    // trying to rescue an account, not by the loader's hot path.
    const std::string text = toJson().dump(2);

    std::FILE* handle = std::fopen(file.c_str(), "wb");
    if (handle == nullptr) {
        errorOut = "could not open " + file;
        return false;
    }
    const std::size_t written = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), handle);
    const bool flushed = std::fflush(handle) == 0;
    if (flushed) ::fsync(::fileno(handle));
    const bool closed = std::fclose(handle) == 0;

    struct stat info {};
    if (written != text.size() || !flushed || !closed || ::stat(file.c_str(), &info) != 0 ||
        static_cast<std::size_t>(info.st_size) != text.size()) {
        std::remove(file.c_str());
        errorOut = "the snapshot did not write in full";
        return false;
    }

    out.file = file;
    out.bytes = text.size();
    out.modifiedMillis = millis;

    // Pruning is best-effort: it must never turn a good backup into a failure.
    std::vector<BackupInfo> existing = listBackups();
    for (std::size_t i = kMaxDatabaseBackups; i < existing.size(); ++i) {
        std::remove(existing[i].file.c_str());
    }
    return true;
}

std::vector<Database::BackupInfo> Database::listBackups() const {
    std::vector<BackupInfo> out;
    const std::string directory = backupDirectory();
    DIR* dir = ::opendir(directory.c_str());
    if (dir == nullptr) return out;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind("inventory-", 0) != 0) continue;
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
        const std::string full = directory + "/" + name;
        struct stat info {};
        if (::stat(full.c_str(), &info) != 0) continue;
        out.push_back({full, static_cast<std::size_t>(info.st_size),
                       static_cast<std::int64_t>(info.st_mtime) * 1000});
    }
    ::closedir(dir);
    // Newest first, which is the order somebody looking for the last good one
    // wants to read.
    std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) {
        if (a.modifiedMillis != b.modifiedMillis) return a.modifiedMillis > b.modifiedMillis;
        return a.file > b.file;
    });
    return out;
}

bool Database::maybeSave(std::int64_t nowMillis) {
    if (!dirty_ || loadFailed_ || path_.empty()) return false;
    if (everWrote_ && nowMillis - lastWriteMillis_ < kMinWriteIntervalMillis) return false;
    const bool ok = save();
    // The stamp moves even on failure: a disk that is refusing writes must not
    // be retried every tick. The record stays dirty, so the next window tries
    // again and nothing is lost.
    lastWriteMillis_ = nowMillis;
    everWrote_ = true;
    return ok;
}

std::int64_t Database::nowMillis() const {
    if (clock_) return clock_();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
// Database: accounts
// ---------------------------------------------------------------------------

void Database::indexUser(const std::string& storedKey, const Account& account) {
    // emplace, not assignment: a legacy file may contain both "Bob" and "bob"
    // as separate accounts. Both records are kept -- deleting one on load
    // would be data loss -- and the first spelling wins the lookup. New
    // registrations can no longer create that situation.
    usersByLowerName_.emplace(toLower(storedKey), storedKey);
    if (!account.id.empty()) usersById_.emplace(account.id, storedKey);
}

Account* Database::findUser(const std::string& username) {
    if (username.empty()) return nullptr;
    if (Account* exact = users_.find(username)) return exact;
    auto it = usersByLowerName_.find(toLower(username));
    if (it == usersByLowerName_.end()) return nullptr;
    return users_.find(it->second);
}

const Account* Database::findUser(const std::string& username) const {
    return const_cast<Database*>(this)->findUser(username);
}

Account* Database::findUserById(const std::string& userId) {
    auto it = usersById_.find(userId);
    if (it == usersById_.end()) return nullptr;
    return users_.find(it->second);
}

std::string Database::canonicalUsername(const std::string& username) const {
    const Account* account = findUser(username);
    return account ? account->username : std::string();
}

bool Database::validUsername(const std::string& username, std::string& reasonOut) {
    if (username.size() < 3 || username.size() > 20) {
        reasonOut = "usernames must be 3 to 20 characters";
        return false;
    }
    // Names are drawn on nameplates and typed at admin commands, so anything
    // that can be mistaken for a different name -- spaces, combining marks,
    // control characters, right-to-left overrides -- is refused at the door
    // rather than sanitised at every place a name is used.
    for (char c : username) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '_';
        if (!allowed) {
            reasonOut = "usernames may use letters, digits and underscore only";
            return false;
        }
    }
    if (username[0] == '_') {
        reasonOut = "usernames must start with a letter or a digit";
        return false;
    }
    return true;
}

bool Database::validPassword(const std::string& password, std::string& reasonOut) {
    if (password.size() < 8) {
        reasonOut = "passwords must be at least 8 characters";
        return false;
    }
    // bcrypt silently ignores everything past its 72nd byte. Accepting a
    // longer password would mean anyone who typed the first 72 characters got
    // in -- so the limit is enforced here, where it can be explained, rather
    // than hidden inside the hash.
    if (password.size() > crypto::kBcryptMaxPasswordBytes) {
        reasonOut = "passwords must be at most 72 bytes";
        return false;
    }
    for (char c : password) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            reasonOut = "passwords may not contain control characters";
            return false;
        }
    }
    return true;
}

void Database::setPasswordCost(int cost) {
    passwordCost_ = clamp(cost, crypto::kBcryptMinCost, crypto::kBcryptMaxCost);
}

std::string Database::newUserId() {
    static const char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::uint8_t raw[12];
        crypto::secureRandom().bytes(raw, sizeof(raw));
        std::string id;
        id.reserve(sizeof(raw));
        // The modulo bias is real and irrelevant: an id has to be unique, not
        // uniform, and 12 base-36 characters is ~62 bits either way.
        for (std::uint8_t byte : raw) id.push_back(kAlphabet[byte % 36]);
        if (usersById_.find(id) == usersById_.end()) return id;
    }
    return {};
}

std::string Database::accountAddressHash(const std::string& addressKey) {
    if (addressKey.empty()) return {};
    std::string salt = otherTop_["ipSalt"].asString();
    if (salt.empty()) {
        salt = crypto::secureRandom().hex(16);
        otherTop_["ipSalt"] = Json(salt);
        if (std::find(topKeyOrder_.begin(), topKeyOrder_.end(), "ipSalt") == topKeyOrder_.end()) {
            topKeyOrder_.push_back("ipSalt");
        }
        markDirty();
    }
    // Same construction as the browser server's database.accountAddressHash,
    // so both builds reading one file agree on which accounts an address made.
    return crypto::sha256Hex(salt + "|" + addressKey).substr(0, 24);
}

int Database::countAccountsCreatedBy(const std::string& addressHash,
                                     std::int64_t windowMillis) const {
    if (addressHash.empty()) return 0;
    const std::int64_t cutoff = nowMillis() - windowMillis;
    int count = 0;
    // A scan of the whole user table. Registration is rate-limited long before
    // this is hot, and a second index kept in step with the accounts is more to
    // get wrong than walking a few thousand entries is to run.
    for (const std::string& name : users_.keys()) {
        const Account* account = users_.find(name);
        if (account != nullptr && account->createdFromHash == addressHash &&
            account->createdAtMillis >= cutoff) {
            ++count;
        }
    }
    return count;
}

CreateResult Database::createUser(const std::string& username, const std::string& password,
                                  const std::string& createdFromHash) {
    CreateResult result;
    if (!validUsername(username, result.reason)) {
        result.status = CreateStatus::UsernameInvalid;
        return result;
    }
    if (!validPassword(password, result.reason)) {
        result.status = CreateStatus::PasswordInvalid;
        return result;
    }
    if (findUser(username) != nullptr) {
        result.status = CreateStatus::UsernameTaken;
        result.reason = "that name is already registered";
        return result;
    }
    const std::string id = newUserId();
    if (id.empty()) {
        result.status = CreateStatus::ServerError;
        result.reason = "could not allocate an account id";
        return result;
    }

    Account& account = users_.insert(username);
    account.id = id;
    account.username = username;
    account.passwordHash = crypto::bcryptHash(password, passwordCost_);
    account.createdAtMillis = nowMillis();
    account.createdFromHash = createdFromHash;
    indexUser(username, account);
    // Give the account its (empty) progress row now, so `players` and `users`
    // never disagree about who exists.
    players_.insert(id);
    markDirty();

    result.account = &account;
    return result;
}

bool Database::eraseUser(const std::string& username) {
    // Resolve through findUser so any spelling works, then delete by the key
    // the account is actually STORED under -- a legacy file may hold both
    // "Bob" and "bob", and erasing the typed spelling would take the wrong one
    // or nothing at all.
    const Account* account = findUser(username);
    if (account == nullptr) return false;
    const std::string storedKey = account->username;
    const std::string userId = account->id;

    // Sessions first, while the account is still there to match against: a
    // token outliving its account resolves to nothing anyway, and leaving the
    // orphans in the file only means writing rubbish that the next load has to
    // step over.
    std::vector<std::string> doomedTokens;
    for (const std::string& tokenHash : sessions_.keys()) {
        const Session* session = sessions_.find(tokenHash);
        if (session != nullptr && session->userId == userId) doomedTokens.push_back(tokenHash);
    }
    for (const std::string& tokenHash : doomedTokens) sessions_.erase(tokenHash);

    users_.erase(storedKey);
    if (!userId.empty()) players_.erase(userId);
    usersByLowerName_.erase(toLower(storedKey));
    if (!userId.empty()) usersById_.erase(userId);
    markDirty();
    return true;
}

bool Database::isStarterProgress(const PlayerRecord& record) {
    if (record.totalXp > 0) return false;
    // A skin is account content somebody chose; it outranks the sweep.
    if (record.renderFlags != 0) return false;
    if (!record.equippedSkinId.empty()) return false;

    // The starter inventory is exactly {common: {petal_basic: 5}}. Anything
    // else -- a second rarity, a second petal, or a different count -- means
    // this account picked something up.
    if (record.inventory.isObject() && !record.inventory.keys().empty()) {
        if (record.inventory.keys().size() > 1) return false;
        const std::string& rarity = record.inventory.keys().front();
        if (rarity != "common") return false;
        const Json& items = record.inventory[rarity];
        if (!items.isObject()) return false;
        if (items.keys().size() > 1) return false;
        if (items.keys().size() == 1) {
            const std::string& key = items.keys().front();
            if (key != "petal_basic" || items[key].asInt(0) != 5) return false;
        }
    }

    for (const std::optional<StoredItem>& slot : record.loadout) {
        if (!slot.has_value()) continue;
        if (slot->petalType != "basic" || slot->rarity != Rarity::Common) return false;
    }
    return true;
}

bool Database::verifyPassword(const std::string& username, const std::string& password) {
    Account* account = findUser(username);
    if (!account) return false;

    bool ok = false;
    if (crypto::isBcryptHash(account->passwordHash)) {
        // The stored value's shape decides, not the isPlainText flag: the old
        // server tested `startsWith('$2b$')` and so mistook every $2a$ hash
        // for a plaintext password. A bcrypt hash is never something a player
        // typed, so this branch is always the right one when it matches.
        ok = crypto::bcryptVerify(password, account->passwordHash);
    } else {
        ok = crypto::constantTimeEquals(account->passwordHash, password);
        if (ok) {
            // The one moment the plaintext is available. Upgrade in place; the
            // account never stores a bare password again.
            account->passwordHash = crypto::bcryptHash(password, passwordCost_);
            account->isPlainText = false;
        }
    }
    if (!ok) return false;

    account->lastActiveAtMillis = nowMillis();
    markDirty();
    return true;
}

// ---------------------------------------------------------------------------
// Database: sessions
// ---------------------------------------------------------------------------

std::string Database::createSession(const std::string& userId, const std::string& username) {
    // 32 bytes: a token has to be unguessable in the whole keyspace, since a
    // guess is a login. Hex rather than base64 so it survives being pasted
    // into a URL, a header, or a support ticket unchanged.
    const std::string token = crypto::secureRandom().hex(32);
    const std::string tokenHash = crypto::sha256Hex(token);
    const std::int64_t now = nowMillis();

    pruneExpiredSessions();

    Session& session = sessions_.insert(tokenHash);
    session.tokenHash = tokenHash;
    session.userId = userId;
    session.username = username;
    session.createdAtMillis = now;
    session.expiresAtMillis = now + kSessionTtlMillis;
    markDirty();
    return token;
}

const Database::Session* Database::resolveSession(const std::string& token) {
    if (token.empty()) return nullptr;
    const std::string tokenHash = crypto::sha256Hex(token);
    Session* session = sessions_.find(tokenHash);
    if (!session) return nullptr;

    const std::int64_t now = nowMillis();
    if (session->expiresAtMillis <= now) {
        sessions_.erase(tokenHash);
        markDirty();
        return nullptr;
    }
    // The account may have been renamed or deleted since the token was issued.
    // A session must never resurrect an account or land on a different one
    // that has since taken the name.
    Account* account = findUser(session->username);
    if (!account || account->id != session->userId) {
        sessions_.erase(tokenHash);
        markDirty();
        return nullptr;
    }

    account->lastActiveAtMillis = now;
    markDirty();
    return session;
}

void Database::revokeSession(const std::string& token) {
    if (token.empty()) return;
    if (sessions_.erase(crypto::sha256Hex(token))) markDirty();
}

int Database::revokeSessionsForUser(const std::string& username) {
    std::vector<std::string> doomed;
    for (const std::string& tokenHash : sessions_.keys()) {
        const Session* session = sessions_.find(tokenHash);
        if (session && session->username == username) doomed.push_back(tokenHash);
    }
    for (const std::string& tokenHash : doomed) sessions_.erase(tokenHash);
    if (!doomed.empty()) markDirty();
    return static_cast<int>(doomed.size());
}

int Database::pruneExpiredSessions() {
    const std::int64_t now = nowMillis();
    std::vector<std::string> doomed;
    for (const std::string& tokenHash : sessions_.keys()) {
        const Session* session = sessions_.find(tokenHash);
        if (session && session->expiresAtMillis <= now) doomed.push_back(tokenHash);
    }
    for (const std::string& tokenHash : doomed) sessions_.erase(tokenHash);
    if (!doomed.empty()) markDirty();
    return static_cast<int>(doomed.size());
}

// ---------------------------------------------------------------------------
// Database: progress
// ---------------------------------------------------------------------------

PlayerRecord& Database::progress(const std::string& userId) {
    const bool fresh = players_.find(userId) == nullptr;
    PlayerRecord& record = players_.insert(userId);
    if (fresh) markDirty();
    return record;
}

const PlayerRecord* Database::findProgress(const std::string& userId) const {
    return players_.find(userId);
}

DailyStreakResult Database::processDailyStreak(const std::string& userId) {
    constexpr std::int64_t kMillisPerDay = 86400000;
    PlayerRecord& record = progress(userId);

    const std::int64_t now = nowMillis();
    // Floor division: before 1970 the truncating form would round the wrong
    // way and hand out a second reward on the same day.
    const std::int64_t today = (now >= 0 ? now : now - (kMillisPerDay - 1)) / kMillisPerDay;
    const std::int64_t todayMillis = today * kMillisPerDay;
    const std::string todayText = civilFromDays(today);

    DailyStreakResult result;
    result.streak = record.dailyStreak;

    if (record.lastStreakDate != todayText) {
        const std::optional<std::int64_t> last = parseUtcDate(record.lastStreakDate);
        // Exactly one day ago continues the run; anything else -- a gap, a
        // first login, or an unreadable date -- starts a new one.
        result.streak = (last && *last == today - 1) ? record.dailyStreak + 1 : 1;
        result.starsAwarded = ((result.streak - 1) % 5) + 1;
        record.dailyStreak = result.streak;
        record.lastStreakDate = todayText;
        record.stars += result.starsAwarded;
        result.newDay = true;
        markDirty();
    }

    result.nextClaimAtMillis = todayMillis + kMillisPerDay;
    result.streakExpiresAtMillis = todayMillis + 2 * kMillisPerDay;
    return result;
}

} // namespace flix
