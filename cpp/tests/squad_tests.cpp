// The squad rules, and the reward ranking a squad changes.
//
// Both are held here rather than in chat_command_tests.cpp because neither
// needs a socket: SquadRoster is pure state, and selectLootRecipients is a pure
// function over a damage tally. The command surface over them is covered end to
// end there.

#include "test.h"

#include <string>
#include <vector>

#include "server/loot_eligibility.h"
#include "server/squads.h"

using namespace flix;

namespace {

SquadMemberId human(net::ConnectionId id) { return SquadMemberId::ofSession(id); }
SquadMemberId bot(Entity body) { return SquadMemberId::ofBot(body); }

/// A body id that is not a connection id, so a test cannot pass by confusing
/// the two.
Entity body(std::uint32_t n) { return static_cast<Entity>(1000 + n); }

} // namespace

TEST(a_squad_holds_one_leader_and_at_most_four) {
    Rng rng(1);
    SquadRoster roster;

    Squad* squad = roster.create(human(1), false, rng);
    CHECK(squad != nullptr);
    CHECK(squad->leader == human(1));
    CHECK(squad->members.size() == 1);
    CHECK(!squad->isPublic);
    CHECK(squad->id.rfind("squad_", 0) == 0);

    // Already in one: the reference answers this with "You are already in a
    // squad." and creates nothing.
    CHECK(roster.create(human(1), true, rng) == nullptr);

    for (net::ConnectionId id = 2; id <= 4; ++id) {
        CHECK(roster.invite(human(1), human(id), "one", 0).empty());
        std::string joined;
        CHECK(roster.accept(human(id), 0, joined).empty());
        CHECK(joined == squad->id);
    }
    CHECK(squad->members.size() == kMaxSquadSize);
    CHECK(roster.invite(human(1), human(5), "one", 0) == "Squad is full (max 4 players).");
}

TEST(only_the_leader_invites_and_only_once_per_target) {
    Rng rng(2);
    SquadRoster roster;
    Squad* squad = roster.create(human(1), false, rng);
    CHECK(squad != nullptr);

    CHECK(roster.invite(human(1), human(2), "one", 0).empty());
    std::string joined;
    CHECK(roster.accept(human(2), 0, joined).empty());

    CHECK(roster.invite(human(2), human(3), "two", 0) ==
          "Only the squad leader can invite players.");
    CHECK(roster.invite(human(9), human(3), "nine", 0) == "You are not in a squad.");
    CHECK(roster.invite(human(1), human(2), "one", 0) == "That player is already in a squad.");

    CHECK(roster.invite(human(1), human(3), "one", 0).empty());
    CHECK(roster.invite(human(1), human(3), "one", 0) == "That player already has a pending "
                                                         "invite.");
}

TEST(an_invitation_lapses_after_thirty_seconds) {
    Rng rng(3);
    SquadRoster roster;
    CHECK(roster.create(human(1), false, rng) != nullptr);
    CHECK(roster.invite(human(1), human(2), "one", 0).empty());

    std::string joined;
    CHECK(roster.accept(human(2), kSquadInviteMillis + 1, joined) == "Invite has expired.");
    // And it is gone, not merely refused once.
    CHECK(roster.accept(human(2), 0, joined) == "No pending invite.");

    // The sweep drops one nobody ever answered, so a target who disconnects
    // does not leave a claim on a seat behind them.
    CHECK(roster.invite(human(1), human(3), "one", 0).empty());
    roster.expire(kSquadInviteMillis + 1);
    CHECK(roster.accept(human(3), kSquadInviteMillis + 1, joined) == "No pending invite.");
}

TEST(the_leader_leaving_promotes_the_next_member) {
    Rng rng(4);
    SquadRoster roster;
    Squad* squad = roster.create(human(1), false, rng);
    const std::string id = squad->id;
    CHECK(roster.invite(human(1), human(2), "one", 0).empty());
    std::string joined;
    CHECK(roster.accept(human(2), 0, joined).empty());

    const SquadRoster::Departure left = roster.leave(human(1));
    CHECK(left.wasMember);
    CHECK(!left.disbanded);
    CHECK(left.promoted == human(2));
    CHECK(roster.find(id)->leader == human(2));

    // The last member out takes the squad with them.
    const SquadRoster::Departure last = roster.leave(human(2));
    CHECK(last.wasMember);
    CHECK(last.disbanded);
    CHECK(roster.find(id) == nullptr);
    CHECK(!roster.leave(human(2)).wasMember);
}

TEST(a_bot_led_squad_is_always_public) {
    Rng rng(5);
    SquadRoster roster;

    // A private bot-led squad is one nobody could ever join, so there is no
    // such thing: the flag is forced on creation and again on promotion.
    Squad* squad = roster.create(bot(body(1)), false, rng);
    CHECK(squad != nullptr);
    CHECK(squad->isPublic);

    SquadRoster human_led;
    Squad* mine = human_led.create(human(1), false, rng);
    CHECK(human_led.addBot(mine->id, bot(body(2))).empty());
    CHECK(!mine->isPublic);
    CHECK(human_led.leave(human(1)).promoted == bot(body(2)));
    CHECK(mine->isPublic);
}

TEST(a_public_squad_is_joinable_until_it_is_full) {
    Rng rng(6);
    SquadRoster roster;
    Squad* open = roster.create(human(1), true, rng);
    Squad* shut = roster.create(human(2), false, rng);
    CHECK(open->isPublic);

    CHECK(roster.joinPublic(shut->id, human(3)) == "That squad is private.");
    CHECK(roster.joinPublic("squad_nothing", human(3)) == "Squad not found.");
    CHECK(roster.joinPublic(open->id, human(3)).empty());
    CHECK(roster.joinPublic(open->id, human(3)) == "You are already in a squad.");

    const std::vector<const Squad*> listed = roster.publicSquads();
    CHECK(listed.size() == 1);
    CHECK(listed.front()->id == open->id);

    // Visibility is the leader's to change, and only the leader's.
    Squad* changed = nullptr;
    CHECK(roster.setVisibility(human(3), false, &changed) ==
          "Only the squad leader can change visibility.");
    CHECK(roster.setVisibility(human(1), false, &changed).empty());
    CHECK(roster.publicSquads().empty());
}

// ---------------------------------------------------------------------------
// The reward ranking
// ---------------------------------------------------------------------------

namespace {

/// A squad of `members`, as the loot rules read one.
SquadEntityIndex indexOf(const std::vector<std::vector<Entity>>& squads) {
    SquadEntityIndex index;
    for (const std::vector<Entity>& members : squads) {
        const std::size_t group = index.groups.size();
        for (const Entity member : members) index.group[member] = group;
        index.groups.push_back(members);
    }
    return index;
}

bool paid(const std::vector<Entity>& recipients, Entity who) {
    return std::find(recipients.begin(), recipients.end(), who) != recipients.end();
}

} // namespace

TEST(unsquadded_contributors_rank_by_damage_and_are_capped) {
    const std::vector<Bounty::Share> tally = {
        {body(1), 50}, {body(2), 40}, {body(3), 30}, {body(4), 20}, {body(5), 10},
    };
    std::vector<Entity> paidOut;
    selectLootRecipients(tally, lootSlotsForRarity(Rarity::Common), nullptr, paidOut);
    CHECK(paidOut.size() == 4);
    CHECK(paidOut[0] == body(1));
    CHECK(paidOut[3] == body(4));
    CHECK(!paid(paidOut, body(5)));

    // A boss opens up, because a boss takes a crowd to kill.
    selectLootRecipients(tally, lootSlotsForRarity(Rarity::Ultra), nullptr, paidOut);
    CHECK(paidOut.size() == 5);
}

TEST(a_squad_ranks_as_one_contender_and_cannot_fill_every_slot) {
    // Four squadmates chipping in against one solo player who out-damaged all
    // of them. Ranked separately they take every slot on an ordinary mob and
    // the solo player -- who did the most work of anyone -- gets nothing.
    const std::vector<Bounty::Share> tally = {
        {body(1), 20}, {body(2), 20}, {body(3), 20}, {body(4), 20}, {body(9), 60},
    };
    const SquadEntityIndex squads = indexOf({{body(1), body(2), body(3), body(4)}});

    std::vector<Entity> paidOut;
    selectLootRecipients(tally, lootSlotsForRarity(Rarity::Common), &squads, paidOut);
    CHECK(paidOut.size() == 4);
    // The squad's AVERAGE is 20 against the solo player's 60, so they lead.
    CHECK(paidOut.front() == body(9));
    CHECK(paid(paidOut, body(1)));
    CHECK(paid(paidOut, body(4)) || paid(paidOut, body(3)));
}

TEST(the_cap_is_spent_in_players_not_in_squads) {
    // Two full squads on an ordinary mob. Capping CONTENDERS and expanding
    // afterwards -- the bug this rule exists to have fixed -- pays all eight.
    std::vector<Bounty::Share> tally;
    for (std::uint32_t i = 1; i <= 8; ++i) tally.push_back({body(i), 10.0 + i});
    const SquadEntityIndex squads =
        indexOf({{body(1), body(2), body(3), body(4)}, {body(5), body(6), body(7), body(8)}});

    std::vector<Entity> paidOut;
    selectLootRecipients(tally, lootSlotsForRarity(Rarity::Common), &squads, paidOut);
    CHECK(paidOut.size() == 4);
}

TEST(a_squadmate_who_did_nothing_is_not_paid) {
    // body(3) never touched the mob. Standing next to somebody who did is not
    // a contribution, so its slot goes to the next player who earned one.
    const std::vector<Bounty::Share> tally = {{body(1), 40}, {body(2), 10}, {body(9), 5}};
    const SquadEntityIndex squads = indexOf({{body(1), body(2), body(3)}});

    std::vector<Entity> paidOut;
    selectLootRecipients(tally, lootSlotsForRarity(Rarity::Common), &squads, paidOut);
    CHECK(paidOut.size() == 3);
    CHECK(!paid(paidOut, body(3)));
    CHECK(paid(paidOut, body(9)));
    // Within the squad, the member who earned more is placed first.
    CHECK(paidOut[0] == body(1));
    CHECK(paidOut[1] == body(2));
}
