#include "test.h"

#include "server/account_limits.h"

#include <functional>
#include <string>

using namespace flix;

/// A database with nothing recorded against the address under test.
static const std::function<int()> noAccountsToday = [] { return 0; };

/// Counts how often the per-address daily scan is asked for, to prove the two
/// O(1) buckets are consulted first.
struct CountingScan {
    int calls = 0;
    int operator()() { ++calls; return 0; }
};

TEST(address_keys_agree_across_every_spelling_of_an_address) {
    // The transport hands over `host:port`; the port is not part of the key.
    CHECK(addressKey("1.2.3.4:51000") == "1.2.3.4");
    CHECK(addressKey("1.2.3.4") == "1.2.3.4");

    // An IPv4 peer on a dual-stack listener arrives mapped, in either spelling.
    CHECK(addressKey("::ffff:1.2.3.4:51000") == "1.2.3.4");
    CHECK(addressKey("0000:0000:0000:0000:0000:ffff:7f00:0001:443") == "127.0.0.1");

    // IPv6 keys on the /64: a residential allocation is 2^64 addresses, so
    // keying the whole address would be no limit at all.
    CHECK(addressKey("2001:db8:abcd:1234:5678:9abc:def0:1:9000") ==
          addressKey("2001:db8:abcd:1234:ffff:ffff:ffff:ffff:9000"));
    CHECK(addressKey("2001:db8:abcd:1234:5678:9abc:def0:1:9000") != addressKey("2001:db8:abcd:9999::1:9000"));

    // Compressed and expanded spellings of one network are one key.
    CHECK(addressKey("2001:0db8::1:9000") == addressKey("2001:db8:0:0:0:0:0:5:9000"));

    // Nothing usable still produces a stable key rather than an empty one.
    CHECK(addressKey("") == "unknown");
    CHECK(!addressKey("garbage").empty());
}

TEST(a_release_build_enforces_the_limits_and_a_dev_build_does_not) {
    // The switch is a compile definition precisely so it cannot be flipped at
    // runtime on a shipping server; this is what says the wiring is intact,
    // in whichever flavour the suite was built.
#ifdef FLIX_DEV_BUILD
    CHECK(!kAccountLimitsEnforcedByDefault);
#else
    CHECK(kAccountLimitsEnforcedByDefault);
#endif
    AccountLimiter fresh;
    CHECK_EQ(fresh.enforced(), kAccountLimitsEnforcedByDefault);

    // And an unenforcing limiter spends nothing: the buckets are untouched, so
    // turning enforcement back on starts from a full allowance rather than
    // from whatever a day of testing had drained.
    AccountLimiter off;
    off.setEnforced(false);
    for (int i = 0; i < kRegisterBurst * 10; ++i) {
        CHECK(off.spendRegistration("1.2.3.4", nullptr, 0).allowed);
        CHECK(off.spendLoginAttempt("1.2.3.4", 0).allowed);
    }
    off.setEnforced(true);
    CHECK(off.spendRegistration("1.2.3.4", nullptr, 0).allowed);
}

TEST(registration_allows_a_burst_then_drips) {
    AccountLimiter limiter;
    // A dev build does not enforce these by default, and this file is
    // precisely the place that is about enforcement.
    limiter.setEnforced(true);
    double now = 1000000;
    limiter.reset(now);

    CHECK(limiter.spendRegistration("9.9.9.9", noAccountsToday, now).allowed);
    CHECK(limiter.spendRegistration("9.9.9.9", noAccountsToday, now).allowed);
    CHECK(limiter.spendRegistration("9.9.9.9", noAccountsToday, now).allowed);

    const LimitVerdict fourth = limiter.spendRegistration("9.9.9.9", noAccountsToday, now);
    CHECK(!fourth.allowed);
    CHECK(fourth.scope == LimitScope::Address);
    CHECK(fourth.retryAfterSeconds > 0);
    CHECK(!fourth.message.empty());

    // One address being throttled says nothing about any other.
    CHECK(limiter.spendRegistration("8.8.8.8", noAccountsToday, now).allowed);

    now += 601000;  // ten minutes buys exactly one more
    CHECK(limiter.spendRegistration("9.9.9.9", noAccountsToday, now).allowed);
    CHECK(!limiter.spendRegistration("9.9.9.9", noAccountsToday, now).allowed);
}

TEST(one_ipv6_allocation_cannot_be_walked_for_more_accounts) {
    AccountLimiter limiter;
    // A dev build does not enforce these by default, and this file is
    // precisely the place that is about enforcement.
    limiter.setEnforced(true);
    const double now = 1000000;
    limiter.reset(now);

    int allowed = 0;
    for (int i = 0; i < 50; ++i) {
        const std::string peer = "2001:db8:1:2:0:0:0:" + std::to_string(i) + ":9000";
        if (limiter.spendRegistration(addressKey(peer), noAccountsToday, now).allowed) ++allowed;
    }
    CHECK(allowed == static_cast<int>(kRegisterBurst));
}

TEST(the_persisted_daily_count_refuses_before_the_bucket_is_consulted) {
    AccountLimiter limiter;
    // A dev build does not enforce these by default, and this file is
    // precisely the place that is about enforcement.
    limiter.setEnforced(true);
    const double now = 1000000;
    limiter.reset(now);

    const LimitVerdict verdict = limiter.spendRegistration(
        "7.7.7.7", [] { return kRegisterDailyPerAddress; }, now);
    CHECK(!verdict.allowed);
    CHECK(verdict.scope == LimitScope::Daily);

    // And the refusal cost the address nothing, so a bucket refusal and a
    // daily refusal do not compound into a longer lockout than either states.
    CHECK(limiter.spendRegistration("7.7.7.7", noAccountsToday, now).allowed);
    CHECK(limiter.spendRegistration("7.7.7.7", noAccountsToday, now).allowed);
    CHECK(limiter.spendRegistration("7.7.7.7", noAccountsToday, now).allowed);
}

TEST(the_global_ceiling_holds_when_every_request_claims_a_new_address) {
    // The attack this whole file exists for: 9000 accounts, one per source.
    AccountLimiter limiter;
    // A dev build does not enforce these by default, and this file is
    // precisely the place that is about enforcement.
    limiter.setEnforced(true);
    const double now = 1000000;
    limiter.reset(now);

    int allowed = 0;
    for (int i = 0; i < 9000; ++i) {
        const std::string peer = "10.0." + std::to_string((i >> 8) & 255) + "." +
                                 std::to_string(i & 255) + ":9000";
        if (limiter.spendRegistration(addressKey(peer), noAccountsToday, now).allowed) ++allowed;
    }
    CHECK(allowed == static_cast<int>(kGlobalRegisterBurst));
}

TEST(login_attempts_are_budgeted_and_a_success_hands_its_token_back) {
    AccountLimiter limiter;
    // A dev build does not enforce these by default, and this file is
    // precisely the place that is about enforcement.
    limiter.setEnforced(true);
    const double now = 1000000;
    limiter.reset(now);

    int allowed = 0;
    for (int i = 0; i < 20; ++i) {
        if (limiter.spendLoginAttempt("5.5.5.5", now).allowed) ++allowed;
    }
    CHECK(allowed == static_cast<int>(kLoginBurst));

    limiter.refundLoginAttempt("5.5.5.5");
    CHECK(limiter.spendLoginAttempt("5.5.5.5", now).allowed);
    CHECK(!limiter.spendLoginAttempt("5.5.5.5", now).allowed);
}

TEST(the_database_scan_runs_only_for_requests_the_cheap_limits_allow) {
    // The count walks every account. Running it for a request the buckets have
    // already refused would turn a registration flood into a table scan per
    // packet -- a worse denial of service than the account spam it exists to
    // stop.
    AccountLimiter limiter;
    // A dev build does not enforce these by default, and this file is
    // precisely the place that is about enforcement.
    limiter.setEnforced(true);
    const double now = 1000000;
    limiter.reset(now);

    CountingScan scan;
    const std::function<int()> counted = [&] { return scan(); };
    for (int i = 0; i < 25; ++i) limiter.spendRegistration("4.4.4.4", counted, now);

    CHECK(scan.calls == static_cast<int>(kRegisterBurst));
}

TEST(refusal_logging_is_throttled_so_a_flood_is_not_also_a_log_flood) {
    AccountLimiter limiter;
    // A dev build does not enforce these by default, and this file is
    // precisely the place that is about enforcement.
    limiter.setEnforced(true);
    double now = 1000000;
    limiter.reset(now);

    const std::string first = limiter.refusalLogLine("1.2.3.4", LimitScope::Address, now);
    CHECK(!first.empty());
    CHECK(first.find("1.2.3.4") != std::string::npos);

    for (int i = 0; i < 500; ++i) {
        CHECK(limiter.refusalLogLine("1.2.3.4", LimitScope::Address, now).empty());
    }

    now += kRefusalLogIntervalMillis + 1;
    const std::string next = limiter.refusalLogLine("1.2.3.4", LimitScope::Global, now);
    CHECK(!next.empty());
    // The line stands for everything it swallowed, so the log still shows scale.
    CHECK(next.find("+500 more") != std::string::npos);
    CHECK(next.find("global") != std::string::npos);
}
