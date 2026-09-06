#pragma once
// Squads: the four-flower party the browser build's `/squad` commands make.
//
// A squad is a conversation, not a record. It lives in memory, it is keyed by
// the CONNECTION rather than by the account, and nothing about it survives a
// restart -- exactly as src/server/squadManager.ts holds it. Split out of
// GameServer because every rule in here ("only the leader may invite", "an
// invite lapses after thirty seconds", "the leader leaving promotes the next
// member") is testable without a socket, and because two roads reach it: the
// `/squad-*` chat commands, and the guild panel's own squad buttons.
//
// Members are named by SquadMemberId rather than by entity: a human's body is
// destroyed and rebuilt on every death, and a party that lost its members each
// time somebody died would not be a party. A bot has no connection, so it is
// named by the one thing it does have, its body.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "shared/core/entity.h"
#include "shared/core/types.h"
#include "shared/net/protocol.h"

namespace flix {

/// The reference's MAX_SQUAD_SIZE. Quoted back in half the refusals below, so
/// it is named rather than written out four times.
inline constexpr std::size_t kMaxSquadSize = 4;

/// An invitation lapses after thirty seconds, as the reference's does.
inline constexpr std::int64_t kSquadInviteMillis = 30000;

/// One squad member: a connected session, or a server-owned bot.
///
/// Connection ids start at 1 (net::Listener::nextId_), so 0 is free to mean
/// "not a connection" and a member is a bot exactly when it has none.
struct SquadMemberId {
    net::ConnectionId connection = 0;
    Entity entity = NULL_ENTITY;

    static SquadMemberId ofSession(net::ConnectionId id) { return {id, NULL_ENTITY}; }
    static SquadMemberId ofBot(Entity body) { return {0, body}; }

    bool bot() const { return connection == 0; }
    bool valid() const { return connection != 0 || entity != NULL_ENTITY; }
};

inline bool operator==(const SquadMemberId& a, const SquadMemberId& b) {
    return a.connection == b.connection && a.entity == b.entity;
}
inline bool operator!=(const SquadMemberId& a, const SquadMemberId& b) { return !(a == b); }

struct Squad {
    /// "squad_" plus nine base-36 characters, which is the shape the reference
    /// mints -- `/squad-join <id>` has to be typeable, and the id is printed
    /// in the public listing for exactly that.
    std::string id;
    SquadMemberId leader;
    std::vector<SquadMemberId> members;
    bool isPublic = false;

    bool full() const { return members.size() >= kMaxSquadSize; }
    bool contains(SquadMemberId who) const;
};

/// Every squad on the server, and the invitations waiting on an answer.
///
/// Every mutator returns the reference's own refusal text, or an empty string
/// on success: the wording is part of the command surface and belongs beside
/// the rule that produced it, not spread over the four call sites that print
/// it.
class SquadRoster {
public:
    /// Creates a squad led by `leader`. Null when they are already in one,
    /// which is the reference's "You are already in a squad."
    Squad* create(SquadMemberId leader, bool isPublic, Rng& rng);

    Squad* find(const std::string& id);
    Squad* forMember(SquadMemberId who);
    const Squad* forMember(SquadMemberId who) const;

    /// Joins a public squad without an invite. Bot-led squads are always
    /// public, which is what makes them joinable at all.
    std::string joinPublic(const std::string& squadId, SquadMemberId who);

    /// Adds a bot straight to `squadId`, bypassing the invite flow: a bot has
    /// nobody to answer an invitation.
    std::string addBot(const std::string& squadId, SquadMemberId bot);

    /// Leader-only visibility toggle. On success `out` is the squad.
    std::string setVisibility(SquadMemberId leader, bool isPublic, Squad** out);

    /// Public squads with room left, in creation order.
    std::vector<const Squad*> publicSquads() const;

    /// Invites `to` into `from`'s squad. Returns the refusal, or "" once the
    /// invitation is pending.
    std::string invite(SquadMemberId from, SquadMemberId to, const std::string& fromUsername,
                       std::int64_t nowMillis);

    /// Answers the one pending invitation. On success `squadIdOut` names the
    /// squad joined.
    std::string accept(SquadMemberId target, std::int64_t nowMillis, std::string& squadIdOut);
    bool decline(SquadMemberId target);

    /// What leaving did, so the caller can say it out loud. `wasMember` false
    /// means there was nothing to leave.
    struct Departure {
        bool wasMember = false;
        std::string squadId;
        /// The squad emptied and is gone: nobody is left to be told.
        bool disbanded = false;
        /// Set when the leader left and the next member was promoted.
        SquadMemberId promoted;
    };
    Departure leave(SquadMemberId who);

    /// Drops invitations that have lapsed. Called on the server's own clock
    /// rather than from a timer per invite, which is a timer that has to be
    /// cancelled when the target disconnects.
    void expire(std::int64_t nowMillis);

    const std::unordered_map<std::string, Squad>& all() const { return squads_; }
    bool empty() const { return squads_.empty(); }

private:
    struct Invite {
        std::string squadId;
        std::string fromUsername;
        std::int64_t expiresAtMillis = 0;
    };

    std::unordered_map<std::string, Squad> squads_;
    /// Keyed by connection: only a human is ever invited, because only a human
    /// can answer.
    std::unordered_map<net::ConnectionId, Invite> invites_;
};

/// Who ranks together when a mob's rewards are shared out.
///
/// The loot and XP rules cap their payouts in PLAYERS but rank in CONTENDERS,
/// and a squad is one contender -- see cpp/server/loot_eligibility.h. Both
/// systems read this by pointer once per corpse, so it is a flat table rebuilt
/// from the roster rather than the roster itself, which is keyed by connection
/// and knows nothing about bodies.
struct SquadEntityIndex {
    std::unordered_map<Entity, std::size_t> group;
    std::vector<std::vector<Entity>> groups;

    void clear() {
        group.clear();
        groups.clear();
    }
    bool empty() const { return groups.empty(); }

    /// The bodies sharing `player`'s squad, or null when they squad alone.
    const std::vector<Entity>* membersOf(Entity player) const {
        const auto it = group.find(player);
        return it == group.end() ? nullptr : &groups[it->second];
    }
};

} // namespace flix
