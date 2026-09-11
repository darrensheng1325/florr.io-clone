#pragma once
// Registration and login abuse limits.
//
// Someone spam-created over 9000 accounts against the production browser
// server, which had no limit of any kind on account creation. This is the same
// defence, on this side: see src/server/accountLimiter.ts -- the constants and
// the layering are deliberately identical, so an operator reasoning about one
// server is reasoning about both.
//
// The per-session budget in session.h does NOT cover this. A session is a
// socket, and a socket is free: reconnecting hands the client a brand-new
// allowance, so a per-session counter bounds how fast ONE connection may try
// and says nothing about how many accounts a client may create. These limits
// are keyed on the address instead, and outlive any one connection.
//
// Three layers, each covering the others' blind spot:
//   * a per-address token bucket -- the shape of a real player's behaviour;
//   * a persisted 24h per-address cap, so a restart does not hand an attacker
//     a fresh allowance (Database::countAccountsCreatedBy);
//   * a global bucket, keyed on nothing the client controls, as the backstop
//     for an address that cannot be trusted.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace flix {

/// Registrations one address may make back-to-back, then one per ten minutes.
inline constexpr double kRegisterBurst = 3;
inline constexpr double kRegisterRefillPerSecond = 1.0 / 600.0;

/// Accounts one address may create in a rolling 24h, counted from the database.
inline constexpr int kRegisterDailyPerAddress = 10;
inline constexpr std::int64_t kRegisterDailyWindowMillis = 24LL * 60 * 60 * 1000;

/// Server-wide ceiling: a burst, then one account every twenty seconds.
inline constexpr double kGlobalRegisterBurst = 50;
inline constexpr double kGlobalRegisterRefillPerSecond = 1.0 / 20.0;

/// Login attempts per address: room for typos, then one every six seconds.
inline constexpr double kLoginBurst = 10;
inline constexpr double kLoginRefillPerSecond = 1.0 / 6.0;

/// At most one refusal line per this long -- see AccountLimiter::refusalLogLine.
inline constexpr double kRefusalLogIntervalMillis = 10000;

/// Most addresses tracked at once. A flood from many sources must not turn the
/// defence into the memory exhaustion it is defending against.
inline constexpr std::size_t kMaxTrackedAddresses = 20000;

/// Whether these limits are ENFORCED at all.
///
/// A DEVELOPMENT build does not enforce them. Every limit here is keyed on the
/// address, and everything that talks to a dev server comes from one: the
/// fifth headless login of an afternoon is indistinguishable from the fifth of
/// an attack, and a screenshot rig cannot wait out a ten-minute refill. A
/// RELEASE build always enforces -- this is the defence a nine-thousand
/// account spam run bought (see the header note), so the switch is a compile
/// definition set only by the dev flavour (cpp/CMakeLists.txt) and there is no
/// way to ask a shipping server to go without it.
#ifdef FLIX_DEV_BUILD
inline constexpr bool kAccountLimitsEnforcedByDefault = false;
#else
inline constexpr bool kAccountLimitsEnforcedByDefault = true;
#endif

/// Which limit refused an attempt.
enum class LimitScope { None, Address, Daily, Global };

struct LimitVerdict {
    bool allowed = true;
    /// Seconds until one more attempt would be granted, for the player-facing text.
    int retryAfterSeconds = 0;
    LimitScope scope = LimitScope::None;
    /// Player-facing reason. Deliberately vague about which limit tripped.
    std::string message;
};

/// The key an address is limited under.
///
/// Takes the transport's `host:port` peer string, drops the port, and folds
/// IPv4-mapped IPv6 back to the IPv4 it carries so one client gets one key
/// however its address is spelled. IPv6 keys on the /64 prefix: a residential
/// allocation is 2^64 addresses, so limiting the full address is no limit.
std::string addressKey(const std::string& peer);

class AccountLimiter {
public:
    /// Says once, on stderr, when this build is not enforcing anything.
    AccountLimiter();

    /// Whether a spend costs anything. False in a development build; a test
    /// that exercises the limits themselves turns it back on, which is also
    /// what makes those tests say out loud that they are about enforcement.
    bool enforced() const { return enforced_; }
    void setEnforced(bool on) { enforced_ = on; }

    /// Spend one registration attempt's budget.
    ///
    /// `accountsToday` reports what the database says this address already
    /// created in the last 24h. It is a callback, not a number, because that
    /// count walks every account: running it for requests the two O(1) buckets
    /// have already refused would hand an attacker a full table scan per
    /// packet, which is a cheaper attack than the one being defended against.
    ///
    /// Nothing is consumed unless all three limits agree. A failed attempt
    /// (name taken, weak password) still costs its token: otherwise probing
    /// which names exist is free.
    LimitVerdict spendRegistration(const std::string& key,
                                   const std::function<int()>& accountsToday, double nowMillis);

    /// Spend one login attempt's budget. A password guess costs the guesser
    /// nothing and costs this server a bcrypt verify.
    LimitVerdict spendLoginAttempt(const std::string& key, double nowMillis);

    /// Hand back a login token after the credentials turned out to be right,
    /// so a busy legitimate player is never held back by this.
    void refundLoginAttempt(const std::string& key);

    /// A log line for a refused attempt, or empty when one was written too
    /// recently.
    ///
    /// Refusals arrive at exactly the rate of the attack, so logging each one
    /// turns a registration flood into a disk flood. One line per window says
    /// the same thing -- that an attack is happening, and where from -- and
    /// carries how many refusals it stands for.
    std::string refusalLogLine(const std::string& address, LimitScope scope, double nowMillis);

    /// How many addresses are currently tracked, for tests and reporting.
    std::size_t trackedAddresses() const { return registerBuckets_.size() + loginBuckets_.size(); }

    /// Test hook: forget everything.
    void reset(double nowMillis);

private:
    struct Bucket {
        double tokens = 0;
        double updatedAtMillis = 0;
    };

    Bucket& bucketFor(std::unordered_map<std::string, Bucket>& table, const std::string& key,
                      double burst, double refillPerSecond, double nowMillis);
    void sweep(std::unordered_map<std::string, Bucket>& table, double burst,
               double refillPerSecond, double nowMillis);

    bool enforced_ = kAccountLimitsEnforcedByDefault;
    double lastRefusalLogMillis_ = 0;
    int suppressedRefusals_ = 0;

    std::unordered_map<std::string, Bucket> registerBuckets_;
    std::unordered_map<std::string, Bucket> loginBuckets_;
    Bucket globalRegister_{kGlobalRegisterBurst, 0};
};

} // namespace flix
