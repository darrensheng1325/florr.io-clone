#pragma once
// The account database: accounts, login sessions, and per-player progress,
// persisted as one JSON file.
//
// The file is `inventory.json` in the shape the old TypeScript server wrote,
// because live player data exists in that shape and nothing here is allowed to
// invalidate it. Two consequences run through the whole implementation:
//
//  * Fields this build does not model are NOT dropped. Every record carries an
//    `extra` object holding the keys we do not understand, and they are
//    written back out untouched. A rewrite that quietly ate the maze
//    progression on first save would be worse than no rewrite at all.
//
//  * A file that exists but does not parse is fatal for writes, not for
//    startup reads. Overwriting a corrupt database with our empty default
//    converts a problem an operator can recover from into total account loss.
//
// Key order is preserved on the round trip. `Json` keeps insertion order and
// the tables below keep theirs, so a save produces a file that diffs cleanly
// against the one it was loaded from instead of reshuffling every account.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "server/crypto.h"
#include "shared/core/json.h"
#include "shared/game/rarity.h"
#include "shared/game/skills.h"

namespace flix {

/// One item as the database stores it -- an inventory entry that has been
/// equipped, not the live petal entity. `type` is the old server's item
/// discriminator ("petal" for everything that is currently obtainable).
struct StoredItem {
    std::string type = "petal";
    Rarity rarity = Rarity::Common;
    /// Petal name WITHOUT the `petal_` prefix that inventory keys carry --
    /// "basic", "rose". The two spellings of the same petal is the old
    /// schema's one real wart, and it is load-bearing: changing either side
    /// orphans every stored loadout.
    std::string petalType;

    double health = 0;
    double maxHealth = 0;
    bool hasHealth = false;
    bool hasMaxHealth = false;

    /// Per-slot state this build does not model: cooldown timestamps, custom
    /// damage/size overrides, the per-instance arrays for clumped petals.
    Json extra = Json::object();

    static StoredItem fromJson(const Json& value);
    Json toJson() const;
};

/// Everything an account owns. Keyed by userId, not by username, so renaming
/// an account never has to move its progress.
struct PlayerRecord {
    double totalXp = 0;
    int stars = 0;
    int dailyStreak = 0;
    /// "YYYY-MM-DD" in UTC. A date string rather than a timestamp because the
    /// reward is per calendar day, and comparing days is what the rule is.
    std::string lastStreakDate;
    std::uint32_t renderFlags = 0;
    std::string equippedSkinId;

    /// rarity name -> item type -> count. Left as JSON rather than mirrored
    /// into a typed map: it is genuinely a sparse dictionary over two open
    /// string domains, and a typed copy would only be a lossy one that drops
    /// keys it has no enum for.
    Json inventory = Json::object();
    /// mob type -> rarity name -> count.
    Json mobKills = Json::object();

    /// The talent tree. Stored as tiers only: the point balance is derived
    /// from the level, so there is nothing here that can disagree with it.
    SkillSet skills;

    /// One entry per loadout slot; an empty slot is a JSON null.
    std::vector<std::optional<StoredItem>> loadout;

    /// The maze's own progression track. XP earned inside the maze lands here
    /// and its talents are bought from here, so a maze run can neither inflate
    /// nor spend the outside level (src/server/playerManager.ts:231-245). The
    /// same JSON shape as `totalXP`/`skills`, under `mazeTotalXP`/`mazeSkills`.
    double mazeTotalXp = 0;
    SkillSet mazeSkills;
    /// Maze-tree branches this build does not know, round-tripped verbatim.
    /// The outside tree drops unknown branches -- a tier only means something
    /// to the code that applies it -- but the maze tree is also read by the
    /// TypeScript server sharing this file, and its maze talents are not all
    /// modelled here yet; losing them would strip a player's maze build.
    Json mazeSkillsExtra = Json::object();

    /// Keys this build does not model, round-tripped verbatim (`mazeLoadout`,
    /// the reference's separately-authored maze preset).
    Json extra = Json::object();

    int itemCount(Rarity rarity, const std::string& itemType) const;
    void setItemCount(Rarity rarity, const std::string& itemType, int count);
    /// Adds `delta` (may be negative). Counts clamp at zero and an entry that
    /// reaches zero is erased, so the inventory does not accumulate rubble.
    void addItem(Rarity rarity, const std::string& itemType, int delta);

    int killCount(const std::string& mobType, Rarity rarity) const;
    void recordKill(const std::string& mobType, Rarity rarity);

    /// Unspent talent points, from the level this record's XP buys.
    int talentPoints() const;
    /// The same, on the maze track.
    int mazeTalentPoints() const;
};

/// A registered account. `passwordHash` is a bcrypt string; see verifyPassword
/// for the one legacy case where it is not.
struct Account {
    std::string id;
    std::string username;
    std::string passwordHash;
    /// Set by an ancient import that stored passwords in the clear. Advisory
    /// only -- the stored value's shape decides how it is checked.
    bool isPlainText = false;
    bool admin = false;
    /// Wall-clock millis of the last successful authentication. Drives the
    /// daily-active count, so it must survive restarts: wall clock, not the
    /// server's monotonic tick clock.
    std::int64_t lastActiveAtMillis = 0;

    bool muted = false;
    std::int64_t mutedAtMillis = 0;
    std::string mutedBy;

    /// Wall-clock millis the account was created. Zero on accounts that predate
    /// the field. Wall clock, not the tick clock: it is compared against a
    /// 24-hour window across restarts.
    std::int64_t createdAtMillis = 0;
    /// Salted hash of the address that created the account -- see
    /// Database::accountAddressHash. Never the address itself: this file is
    /// backed up and read by operators, and a rolling count is all the abuse
    /// limit needs. Empty when the creator could not be identified.
    std::string createdFromHash;

    Json extra = Json::object();
};

/// What one authentication's daily-login award came to.
///
/// The rule is the browser build's: the first login of each UTC day pays
/// `((streak - 1) % 5) + 1` stars, a missed day resets the streak to 1, and the
/// two timestamps are what the title screen's streak card counts down to.
struct DailyStreakResult {
    int starsAwarded = 0;
    int streak = 0;
    /// True when THIS call is the one that claimed today.
    bool newDay = false;
    /// Unix millis when the next claim window opens: the next UTC midnight if
    /// today is already claimed, otherwise now.
    std::int64_t nextClaimAtMillis = 0;
    /// Unix millis when an unclaimed streak lapses -- the end of the day after
    /// the next claim window.
    std::int64_t streakExpiresAtMillis = 0;
};

enum class CreateStatus : std::uint8_t {
    Ok = 0,
    UsernameInvalid,
    UsernameTaken,
    PasswordInvalid,
    /// Could not allocate an account id. Not reachable in practice; a
    /// distinct status because silently aliasing two accounts onto one id
    /// would be far worse than refusing the registration.
    ServerError,
};

struct CreateResult {
    CreateStatus status = CreateStatus::Ok;
    /// Valid only when status is Ok. Stable: accounts live in a node-based
    /// map, so later insertions never move this.
    Account* account = nullptr;
    /// Player-facing explanation for a rejection.
    std::string reason;

    bool ok() const { return status == CreateStatus::Ok; }
};

class Database {
public:
    /// One logged-in client. Nested rather than `flix::Session`, which is the
    /// per-connection state in server/session.h -- a socket and a credential
    /// are different things and both deserve the obvious name in their own
    /// scope.
    ///
    /// The raw token exists exactly twice: in the reply to the client, and in
    /// that client's local storage. Only its SHA-256 is stored here, so a
    /// leaked database hands out no live sessions.
    struct Session {
        std::string tokenHash;
        std::string userId;
        std::string username;
        std::int64_t createdAtMillis = 0;
        std::int64_t expiresAtMillis = 0;
    };

    /// A dictionary that remembers insertion order.
    ///
    /// The database is a file people read and back up; rebuilding it from an
    /// unordered_map would reshuffle every key on the first save and make the
    /// diff between two backups useless. Order lives in a side vector, so
    /// lookup stays O(1) and only the rare erase pays for it.
    template <class T>
    class Table {
    public:
        T* find(const std::string& key) {
            auto it = byKey_.find(key);
            return it == byKey_.end() ? nullptr : &it->second;
        }
        const T* find(const std::string& key) const {
            auto it = byKey_.find(key);
            return it == byKey_.end() ? nullptr : &it->second;
        }
        T& insert(const std::string& key) {
            auto it = byKey_.find(key);
            if (it == byKey_.end()) {
                order_.push_back(key);
                it = byKey_.emplace(key, T{}).first;
            }
            return it->second;
        }
        bool erase(const std::string& key) {
            if (byKey_.erase(key) == 0) return false;
            for (std::size_t i = 0; i < order_.size(); ++i) {
                if (order_[i] == key) { order_.erase(order_.begin() + static_cast<long>(i)); break; }
            }
            return true;
        }
        const std::vector<std::string>& keys() const { return order_; }
        std::size_t size() const { return byKey_.size(); }
        void clear() { order_.clear(); byKey_.clear(); }

    private:
        std::vector<std::string> order_;
        std::unordered_map<std::string, T> byKey_;
    };

    /// Wall-clock source, in Unix milliseconds. Overridable so a test can age
    /// a 30-day session TTL without waiting 30 days; production never sets it.
    using ClockFn = std::int64_t (*)();

    /// Sessions live 30 days. Long enough that a player is not re-typing a
    /// password every week, short enough that an abandoned machine stops
    /// being a way in.
    static constexpr std::int64_t kSessionTtlMillis = 30LL * 24 * 60 * 60 * 1000;

    /// At most one disk write per this interval. A save serialises the whole
    /// database, and at a few megabytes that is not something to do on every
    /// petal pickup.
    static constexpr std::int64_t kMinWriteIntervalMillis = 2000;

    Database() = default;
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// Reads `path`. A missing file is not an error -- it is a first run, and
    /// the empty database that results is marked dirty so the file appears on
    /// the first save. A file that exists but cannot be read or parsed IS an
    /// error, and latches loadFailed() so no save can ever overwrite it.
    bool load(const std::string& path, std::string& errorOut);

    /// Writes now, atomically: staged to `path + ".tmp"`, size-verified, then
    /// renamed over the original. A crash at any point leaves the previous
    /// good file intact. Returns false without touching the live file if
    /// anything went wrong, and leaves the database dirty so the change is
    /// retried rather than lost.
    bool save();

    /// The per-tick pump. Writes only when something changed and the rate
    /// limit allows it; because dirty state is only cleared by a successful
    /// write, the last change in a burst is always written eventually.
    /// `nowMillis` must come from one monotonic clock across all calls.
    bool maybeSave(std::int64_t nowMillis);

    void markDirty() { dirty_ = true; }
    bool dirty() const { return dirty_; }

    /// True when the file on disk could not be understood. Every save is
    /// refused while this holds.
    bool loadFailed() const { return loadFailed_; }
    const std::string& path() const { return path_; }

    // -- accounts ----------------------------------------------------------

    /// Case-insensitive: accounts keep the spelling they registered with, but
    /// nobody remembers their own capitalisation, and letting "Bob" and "bob"
    /// coexist is a free impersonation vector.
    Account* findUser(const std::string& username);
    const Account* findUser(const std::string& username) const;
    Account* findUserById(const std::string& userId);

    /// The stored spelling of `username`, or empty if there is no such
    /// account.
    std::string canonicalUsername(const std::string& username) const;

    /// `createdFromHash` stamps the account with accountAddressHash(...) of the
    /// address that asked for it, so countAccountsCreatedBy can hold one source
    /// to a daily cap across restarts. Empty means "creator unknown", which
    /// counts against nobody -- the in-memory and global limits still apply.
    CreateResult createUser(const std::string& username, const std::string& password,
                            const std::string& createdFromHash = {});

    /// A stable, non-reversible label for an address, for the per-address
    /// registration cap.
    ///
    /// Salted with a secret stored in the database file, so the value cannot be
    /// matched back to an address by hashing a candidate list -- the file and
    /// every backup of it would otherwise be a de-anonymisable log of who
    /// played from where. The salt is generated once and persisted: a fresh one
    /// each boot would silently reset the cap it exists to enforce, and two
    /// servers sharing a file have to agree on the hashes.
    std::string accountAddressHash(const std::string& addressKey);

    /// How many accounts `addressHash` created within the last `windowMillis`.
    int countAccountsCreatedBy(const std::string& addressHash, std::int64_t windowMillis) const;

    /// Deletes an account and the progress record behind it, by any spelling
    /// of the name. Returns false when there is no such account.
    ///
    /// Sessions go with it: a token that outlived its account would resolve to
    /// nothing on the next resume, and resolveSession would drop it anyway --
    /// but doing it here means the file never carries the orphans at all.
    bool eraseUser(const std::string& username);

    /// True when this record is a brand-new account that has done nothing: no
    /// XP, no skin, and nothing in the inventory or loadout beyond the starter
    /// kit. The guest sweep keeps anything that fails this, because a guest
    /// who actually played is a player.
    static bool isStarterProgress(const PlayerRecord& record);

    /// Checks a password and, on success, stamps lastActiveAt. An account
    /// still holding a plaintext password from the old import is upgraded to
    /// bcrypt here, on the one occasion the password is available.
    bool verifyPassword(const std::string& username, const std::string& password);

    static bool validUsername(const std::string& username, std::string& reasonOut);
    static bool validPassword(const std::string& password, std::string& reasonOut);

    /// bcrypt cost for new and upgraded passwords. Clamped to bcrypt's legal
    /// range. Tests lower it; a production server has no reason to.
    void setPasswordCost(int cost);
    int passwordCost() const { return passwordCost_; }

    const std::vector<std::string>& usernames() const { return users_.keys(); }
    std::size_t userCount() const { return users_.size(); }

    // -- sessions ----------------------------------------------------------

    /// Mints a session and returns the RAW token. This is the only moment the
    /// raw token exists server-side; only its hash is kept, so it cannot be
    /// recovered from the database or from a backup.
    std::string createSession(const std::string& userId, const std::string& username);

    /// Resolves a raw token. Returns null for unknown, expired, or orphaned
    /// tokens -- and drops the stored record in the latter two cases, so a
    /// deleted or renamed account can never be resumed into.
    const Session* resolveSession(const std::string& token);

    void revokeSession(const std::string& token);
    /// Revokes every session of one account (password change, ban, logout
    /// everywhere). Returns how many were dropped.
    int revokeSessionsForUser(const std::string& username);
    int pruneExpiredSessions();
    std::size_t sessionCount() const { return sessions_.size(); }

    // -- progress ----------------------------------------------------------

    /// The record for `userId`, created empty if absent. Mutating the result
    /// does not mark the database dirty -- the caller knows whether it changed
    /// anything and must call markDirty().
    PlayerRecord& progress(const std::string& userId);
    const PlayerRecord* findProgress(const std::string& userId) const;

    /// Claims today's daily-login reward for `userId`, if it is not claimed
    /// already, and reports the resulting streak. Called once per successful
    /// authentication; calling it twice on the same UTC day is a no-op that
    /// still reports the current state, which is what a reconnect needs.
    DailyStreakResult processDailyStreak(const std::string& userId);
    std::size_t playerCount() const { return players_.size(); }

    void setClock(ClockFn fn) { clock_ = fn; }
    /// Unix milliseconds from the active clock.
    std::int64_t nowMillis() const;

    /// The whole database as JSON, in the shape and key order it is stored in.
    /// Exposed because it is exactly what a backup wants.
    Json toJson() const;

    // -- backups -----------------------------------------------------------
    //
    // Timestamped snapshots kept ONE LEVEL ABOVE the runtime directory, in
    // db_backups/, so a redeploy that replaces dist/ cannot delete them. The
    // `update` command takes one before it touches a single file and aborts if
    // it cannot, which is the only reason a self-installing server is safe to
    // have at all.

    struct BackupInfo {
        std::string file;
        std::size_t bytes = 0;
        std::int64_t modifiedMillis = 0;
    };

    /// How many snapshots are kept. The reference's MAX_DB_BACKUPS: enough to
    /// reach back past a bad deploy, few enough that they cannot fill a disk.
    static constexpr std::size_t kMaxDatabaseBackups = 30;

    /// Writes one snapshot, labelled for whoever asked for it. False with
    /// `errorOut` set on any failure -- a backup that half-wrote is not a
    /// backup, so the file is verified by length before it counts.
    bool backup(const std::string& label, BackupInfo& out, std::string& errorOut);

    /// Existing snapshots, newest first.
    std::vector<BackupInfo> listBackups() const;

    /// Where those snapshots live, derived from the database's own path.
    std::string backupDirectory() const;

    /// One unmodelled top-level table, read exactly as the file stores it.
    ///
    /// Separate from rawTable() because that one COERCES its value to an
    /// object. That is right for the dictionaries it was written for and fatal
    /// for `notifications`, which the browser build stores as an ARRAY: one
    /// coercing read would replace the whole feed with `{}`. Returns a null
    /// Json when the table is absent.
    const Json& storedTable(const std::string& key) const { return otherTop_[key]; }

    /// The array-shaped counterpart of rawTable(), for `notifications`.
    ///
    /// rawTable() COERCES its value to an object, which is right for the
    /// dictionaries it was written for and fatal here: one coercing read would
    /// replace the whole feed with `{}`. Creates the table -- and its place in
    /// the key order -- on first access; the caller marks the database dirty.
    Json& rawArrayTable(const std::string& key);

    /// One of the top-level tables this build does not model, as raw JSON.
    ///
    /// The star codes are genuinely a dictionary of open-ended records that
    /// only one handler ever reads, and a typed mirror of them would be a
    /// lossy copy of the file's own shape. Creates the table -- and its place
    /// in the key order, or a save would drop it again -- on first access; the
    /// caller marks the database dirty.
    Json& rawTable(const std::string& key);

private:
    void reset();
    bool parseRoot(const Json& root, std::string& errorOut);
    std::string newUserId();
    void indexUser(const std::string& storedKey, const Account& account);

    Table<Account> users_;
    Table<PlayerRecord> players_;
    Table<Session> sessions_;

    /// lowercase username -> the key it is stored under, and userId -> the
    /// same. Both are derived indices, rebuilt on load and maintained on
    /// insert; neither is serialised.
    std::unordered_map<std::string, std::string> usersByLowerName_;
    std::unordered_map<std::string, std::string> usersById_;

    /// Top-level keys we do not model -- codes, notifications, guilds,
    /// apiKeys, customSkins -- held as raw JSON and written back untouched.
    Json otherTop_ = Json::object();
    /// Original top-level key order, so a save does not move `players` to the
    /// front of a file that had it elsewhere.
    std::vector<std::string> topKeyOrder_;

    std::string path_;
    bool loadFailed_ = false;
    bool dirty_ = false;
    bool everWrote_ = false;
    std::int64_t lastWriteMillis_ = 0;
    int passwordCost_ = crypto::kBcryptDefaultCost;
    ClockFn clock_ = nullptr;
};

} // namespace flix
