#include "server/account_limits.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

namespace flix {

namespace {

std::string toLowerAscii(const std::string& text) {
    std::string out = text;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool allDigits(const std::string& text) {
    if (text.empty()) return false;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::vector<std::string> split(const std::string& text, char sep) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = text.find(sep, start);
        if (at == std::string::npos) { parts.push_back(text.substr(start)); break; }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
    return parts;
}

/// The four octets of a dotted quad, or false if it is not one.
bool parseIPv4(const std::string& text, int out[4]) {
    const std::vector<std::string> parts = split(text, '.');
    if (parts.size() != 4) return false;
    for (int i = 0; i < 4; ++i) {
        if (!allDigits(parts[i]) || parts[i].size() > 3) return false;
        const int value = std::stoi(parts[i]);
        if (value > 255) return false;
        out[i] = value;
    }
    return true;
}

bool parseHexGroup(const std::string& text, int& out) {
    if (text.empty() || text.size() > 4) return false;
    int value = 0;
    for (const char c : text) {
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else return false;
        value = value * 16 + digit;
    }
    out = value;
    return true;
}

/// An IPv6 address as its eight 16-bit groups. Handles the `::`-compressed
/// form and a trailing dotted quad (`::ffff:1.2.3.4`), which is how an
/// IPv4 peer on a dual-stack listener is usually spelled. Parsing to numbers
/// rather than comparing text is what makes every spelling agree on one key.
bool expandIPv6(const std::string& input, int out[8]) {
    std::string text = input;
    std::vector<int> embedded;

    const std::size_t lastColon = text.rfind(':');
    if (lastColon != std::string::npos && text.find('.', lastColon) != std::string::npos) {
        int octets[4];
        if (!parseIPv4(text.substr(lastColon + 1), octets)) return false;
        embedded = {(octets[0] << 8) | octets[1], (octets[2] << 8) | octets[3]};
        text = text.substr(0, lastColon);
        // `::1.2.3.4` leaves a lone ':' behind; restore the `::` marker.
        if (text == ":") text = "::";
    }

    const std::size_t doubleColon = text.find("::");
    if (doubleColon != std::string::npos && text.find("::", doubleColon + 1) != std::string::npos) {
        return false;  // more than one `::` is not an address
    }

    const std::string headText = doubleColon == std::string::npos ? text : text.substr(0, doubleColon);
    const std::string tailText = doubleColon == std::string::npos ? "" : text.substr(doubleColon + 2);

    std::vector<int> head, tail;
    auto collect = [](const std::string& part, std::vector<int>& into) {
        if (part.empty()) return true;
        for (const std::string& group : split(part, ':')) {
            int value = 0;
            if (!parseHexGroup(group, value)) return false;
            into.push_back(value);
        }
        return true;
    };
    if (!collect(headText, head) || !collect(tailText, tail)) return false;

    const std::size_t explicitGroups = head.size() + tail.size() + embedded.size();
    if (explicitGroups > 8) return false;
    if (doubleColon == std::string::npos && explicitGroups != 8) return false;

    std::vector<int> groups = head;
    if (doubleColon != std::string::npos) groups.resize(8 - tail.size() - embedded.size(), 0);
    groups.insert(groups.end(), tail.begin(), tail.end());
    groups.insert(groups.end(), embedded.begin(), embedded.end());
    if (groups.size() != 8) return false;

    for (int i = 0; i < 8; ++i) out[i] = groups[static_cast<std::size_t>(i)];
    return true;
}

std::string hexGroup(int value) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const int digit = (value >> shift) & 0xF;
        if (!out.empty() || digit != 0 || shift == 0) out.push_back(kDigits[digit]);
    }
    return out;
}

std::string joinOctets(int a, int b, int c, int d) {
    return std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." +
           std::to_string(d);
}

LimitVerdict denied(LimitScope scope, int retryAfterSeconds, const char* message) {
    LimitVerdict verdict;
    verdict.allowed = false;
    verdict.scope = scope;
    verdict.retryAfterSeconds = retryAfterSeconds;
    verdict.message = message;
    return verdict;
}

/// Seconds until a bucket holds a whole token again.
int waitSeconds(double tokens, double refillPerSecond) {
    if (tokens >= 1) return 0;
    return static_cast<int>(std::ceil((1.0 - tokens) / refillPerSecond));
}

} // namespace

std::string addressKey(const std::string& peer) {
    std::string host = toLowerAscii(peer);
    // The transport formats a peer as `host:port`, unbracketed, so the port is
    // whatever follows the LAST colon -- an IPv6 host is full of the others.
    const std::size_t lastColon = host.rfind(':');
    if (lastColon != std::string::npos && allDigits(host.substr(lastColon + 1))) {
        // ...unless the whole thing is a bare IPv6 address, where the trailing
        // group is a group and not a port. Only trim when what remains is still
        // an address in its own right, which `2001:db8:` (from `2001:db8::1`)
        // is not.
        const std::string trimmed = host.substr(0, lastColon);
        int probe[8];
        int octets[4];
        const bool stillAnAddress = !trimmed.empty() && trimmed.back() != ':' &&
                                    (parseIPv4(trimmed, octets) || expandIPv6(trimmed, probe) ||
                                     trimmed.find(':') == std::string::npos);
        if (stillAnAddress) host = trimmed;
    }
    while (!host.empty() && (host.front() == '[' || host.back() == ']')) {
        if (host.front() == '[') host.erase(host.begin());
        if (!host.empty() && host.back() == ']') host.pop_back();
    }
    if (host.empty()) return "unknown";

    int octets[4];
    if (parseIPv4(host, octets)) return host;
    if (host.find(':') == std::string::npos) return host;

    int groups[8];
    if (!expandIPv6(host, groups)) return host;

    // IPv4-mapped (::ffff:a.b.c.d): the octets are packed into the last two groups.
    const bool mapped = groups[0] == 0 && groups[1] == 0 && groups[2] == 0 && groups[3] == 0 &&
                        groups[4] == 0 && groups[5] == 0xffff;
    if (mapped) {
        return joinOctets(groups[6] >> 8, groups[6] & 0xff, groups[7] >> 8, groups[7] & 0xff);
    }

    return hexGroup(groups[0]) + ":" + hexGroup(groups[1]) + ":" + hexGroup(groups[2]) + ":" +
           hexGroup(groups[3]) + "::/64";
}

void AccountLimiter::sweep(std::unordered_map<std::string, Bucket>& table, double burst,
                           double refillPerSecond, double nowMillis) {
    if (table.size() <= kMaxTrackedAddresses) return;

    // A bucket that has refilled to full is indistinguishable from an address
    // never seen, so forgetting it costs nothing.
    for (auto it = table.begin(); it != table.end();) {
        const double elapsed = (nowMillis - it->second.updatedAtMillis) / 1000.0;
        const double tokens = std::min(burst, it->second.tokens + elapsed * refillPerSecond);
        if (tokens >= burst) it = table.erase(it);
        else ++it;
    }
    if (table.size() <= kMaxTrackedAddresses) return;

    // Still over budget means every tracked address is actively spending: a
    // distributed flood. Drop the least recently seen half rather than grow
    // without bound -- the global bucket is what holds that line anyway.
    std::vector<std::pair<double, std::string>> byAge;
    byAge.reserve(table.size());
    for (const auto& entry : table) byAge.emplace_back(entry.second.updatedAtMillis, entry.first);
    std::sort(byAge.begin(), byAge.end());
    for (std::size_t i = 0; i < byAge.size() / 2; ++i) table.erase(byAge[i].second);
}

AccountLimiter::Bucket& AccountLimiter::bucketFor(std::unordered_map<std::string, Bucket>& table,
                                                  const std::string& key, double burst,
                                                  double refillPerSecond, double nowMillis) {
    auto it = table.find(key);
    if (it == table.end()) {
        sweep(table, burst, refillPerSecond, nowMillis);
        it = table.emplace(key, Bucket{burst, nowMillis}).first;
    }
    Bucket& bucket = it->second;
    const double elapsed = (nowMillis - bucket.updatedAtMillis) / 1000.0;
    if (elapsed > 0) {
        bucket.updatedAtMillis = nowMillis;
        bucket.tokens = std::min(burst, bucket.tokens + elapsed * refillPerSecond);
    }
    return bucket;
}

AccountLimiter::AccountLimiter() {
    if (enforced_) return;
    // Once per process, however many limiters a test builds: a server running
    // with its abuse defences off is something an operator has to be able to
    // see in the log, and a dev build that reached a machine it should not
    // have is worth shouting about.
    static bool announced = false;
    if (announced) return;
    announced = true;
    std::fprintf(stderr,
                 "[ABUSE] development build: account creation and login limits are NOT enforced\n");
}

LimitVerdict AccountLimiter::spendRegistration(const std::string& key,
                                               const std::function<int()>& accountsToday,
                                               double nowMillis) {
    // Nothing is counted, so nothing is refused and no bucket is touched: a
    // dev build that is later switched back on starts from a full allowance
    // rather than from whatever an afternoon of testing had drained.
    if (!enforced_) return LimitVerdict{};

    Bucket& bucket =
        bucketFor(registerBuckets_, key, kRegisterBurst, kRegisterRefillPerSecond, nowMillis);

    if (globalRegister_.updatedAtMillis == 0) globalRegister_.updatedAtMillis = nowMillis;
    const double globalElapsed = (nowMillis - globalRegister_.updatedAtMillis) / 1000.0;
    if (globalElapsed > 0) {
        globalRegister_.updatedAtMillis = nowMillis;
        globalRegister_.tokens = std::min(
            kGlobalRegisterBurst, globalRegister_.tokens + globalElapsed * kGlobalRegisterRefillPerSecond);
    }

    // Checked before either is spent: an attempt one limit refuses must not
    // also burn the other's budget.
    if (bucket.tokens < 1) {
        return denied(LimitScope::Address, waitSeconds(bucket.tokens, kRegisterRefillPerSecond),
                      "Too many accounts created from here. Please wait a few minutes.");
    }
    if (globalRegister_.tokens < 1) {
        return denied(LimitScope::Global,
                      waitSeconds(globalRegister_.tokens, kGlobalRegisterRefillPerSecond),
                      "The server is creating accounts as fast as it will right now. Try again shortly.");
    }
    if (accountsToday && accountsToday() >= kRegisterDailyPerAddress) {
        return denied(LimitScope::Daily, 3600,
                      "This network has created too many accounts today. Try again tomorrow.");
    }

    bucket.tokens -= 1;
    globalRegister_.tokens -= 1;
    return LimitVerdict{};
}

LimitVerdict AccountLimiter::spendLoginAttempt(const std::string& key, double nowMillis) {
    if (!enforced_) return LimitVerdict{};
    Bucket& bucket = bucketFor(loginBuckets_, key, kLoginBurst, kLoginRefillPerSecond, nowMillis);
    if (bucket.tokens < 1) {
        return denied(LimitScope::Address, waitSeconds(bucket.tokens, kLoginRefillPerSecond),
                      "Too many login attempts. Please wait a moment.");
    }
    bucket.tokens -= 1;
    return LimitVerdict{};
}

void AccountLimiter::refundLoginAttempt(const std::string& key) {
    auto it = loginBuckets_.find(key);
    if (it != loginBuckets_.end()) {
        it->second.tokens = std::min(kLoginBurst, it->second.tokens + 1.0);
    }
}

std::string AccountLimiter::refusalLogLine(const std::string& address, LimitScope scope,
                                           double nowMillis) {
    ++suppressedRefusals_;
    if (lastRefusalLogMillis_ != 0 && nowMillis - lastRefusalLogMillis_ < kRefusalLogIntervalMillis) {
        return {};
    }
    const int suppressed = suppressedRefusals_ - 1;
    lastRefusalLogMillis_ = nowMillis;
    suppressedRefusals_ = 0;

    const char* which = scope == LimitScope::Daily    ? "daily"
                        : scope == LimitScope::Global ? "global"
                                                      : "address";
    std::string line = std::string("[ABUSE] registration refused (") + which + ") for " + address;
    if (suppressed > 0) {
        line += " (+" + std::to_string(suppressed) + " more since the last line)";
    }
    return line;
}

void AccountLimiter::reset(double nowMillis) {
    registerBuckets_.clear();
    loginBuckets_.clear();
    globalRegister_ = Bucket{kGlobalRegisterBurst, nowMillis};
    lastRefusalLogMillis_ = 0;
    suppressedRefusals_ = 0;
}

} // namespace flix
