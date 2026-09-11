// The chat command surface, end to end over loopback.
//
// The bug these exist to keep fixed: handleChat used to sanitise a line and
// broadcast it, so every command the client offers in its autocomplete --
// "/help", "/admin spawn ..." -- reached the world as ordinary chat. The first
// test here is that one directly; the rest cover the commands that change
// server state, which are the ones a silent regression would hide.

#include "test.h"

#include <cmath>
#include <string>
#include <vector>

#include "server_harness.h"
#include "server/bot_identity.h"
#include "server/db.h"
#include "shared/game/terrain.h"

using namespace flix;
using namespace flix::testsupport;

namespace {

void seedUser(const std::string& path, const std::string& username, const std::string& password,
              bool admin = false) {
    Database db;
    std::string error;
    db.load(path, error);
    db.setPasswordCost(4);   // the default cost makes this the slowest test
    CreateResult created = db.createUser(username, password);
    if (created.ok() && admin) created.account->admin = true;
    db.markDirty();
    db.save();
}

bool loginAs(Harness& h, NetClient& client, const char* name, const char* password) {
    if (!connectClient(h, client)) return false;
    client.requestLogin(name, password);
    return h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; });
}

/// Every chat line this client holds, joined -- the whole transcript as one
/// haystack, because command output is many lines and a test cares that the
/// answer is somewhere in it, not which line carried it.
std::string transcript(const NetClient& client) {
    std::string all;
    for (const ChatLine& line : client.chat()) {
        all += line.author;
        all += ": ";
        all += line.text;
        all += '\n';
    }
    return all;
}

bool sawText(const NetClient& client, const std::string& needle) {
    return transcript(client).find(needle) != std::string::npos;
}

/// Sends `text` and steps until the transcript grows, so a test does not have
/// to guess how many ticks a reply takes.
bool say(Harness& h, NetClient& client, const std::string& text, int maxTicks = 120) {
    const std::size_t before = client.chat().size();
    client.sendChat(text);
    return h.stepUntil({&client}, [&] { return client.chat().size() > before; }, maxTicks);
}

/// The snapshots this harness's database wrote, read back off disk. A backup
/// is only a backup if it loads, so the test that makes one opens it.
std::vector<Database::BackupInfo> reopenBackups(Harness& h) {
    Database probe;
    std::string error;
    probe.load(h.dbPath, error);
    return probe.listBackups();
}

} // namespace

TEST(a_slash_command_is_answered_rather_than_broadcast) {
    Harness h("cmd-not-broadcast", [](const std::string& path) {
        seedUser(path, "asker", "password7");
        seedUser(path, "bystander", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient asker;
    NetClient bystander;
    CHECK(loginAs(h, asker, "asker", "password7"));
    CHECK(loginAs(h, bystander, "bystander", "password7"));

    const std::size_t before = asker.chat().size();
    asker.sendChat("/help");
    CHECK(h.stepUntil({&asker, &bystander},
                      [&] { return asker.chat().size() > before; }, 120));

    // The asker got the listing...
    CHECK(sawText(asker, "/biome"));
    // ...and it went to the asker ALONE. The regression this guards is the
    // whole reason the file exists: "/help" used to arrive at everybody as a
    // chat message reading "/help".
    CHECK(!sawText(bystander, "/help"));
    CHECK(!sawText(bystander, "/biome"));
}

TEST(an_unknown_command_is_refused_and_still_not_broadcast) {
    Harness h("cmd-unknown", [](const std::string& path) {
        seedUser(path, "asker", "password7");
        seedUser(path, "bystander", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient asker;
    NetClient bystander;
    CHECK(loginAs(h, asker, "asker", "password7"));
    CHECK(loginAs(h, bystander, "bystander", "password7"));

    const std::size_t before = asker.chat().size();
    asker.sendChat("/notacommand");
    CHECK(h.stepUntil({&asker, &bystander},
                      [&] { return asker.chat().size() > before; }, 120));

    CHECK(sawText(asker, "Unknown command"));
    CHECK(!sawText(bystander, "/notacommand"));
}

TEST(an_ordinary_line_still_reaches_everyone) {
    Harness h("cmd-plain-chat", [](const std::string& path) {
        seedUser(path, "talker", "password7");
        seedUser(path, "listener", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient talker;
    NetClient listener;
    CHECK(loginAs(h, talker, "talker", "password7"));
    CHECK(loginAs(h, listener, "listener", "password7"));

    const std::size_t before = listener.chat().size();
    talker.sendChat("hello everyone");
    CHECK(h.stepUntil({&talker, &listener},
                      [&] { return listener.chat().size() > before; }, 120));
    CHECK(sawText(listener, "hello everyone"));
}

TEST(the_admin_console_is_denied_to_a_non_admin) {
    Harness h("cmd-admin-denied", [](const std::string& path) {
        seedUser(path, "nobody", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "nobody", "password7"));
    CHECK(say(h, client, "/admin list-players"));

    // The command's EXISTENCE is denied, not the permission: telling a
    // stranger a console is there is half of finding a way in.
    CHECK(sawText(client, "Command does not exist."));
    CHECK(!sawText(client, "level"));
}

TEST(an_admin_can_run_the_console) {
    Harness h("cmd-admin-allowed", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "boss", "password7"));
    client.joinGame(1920, 1080, {}, "Boss");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }, 200));

    CHECK(say(h, client, "/admin list-players"));
    CHECK(sawText(client, "[ADMIN] boss executed: list-players"));
    CHECK(sawText(client, "boss"));
}

TEST(give_writes_the_petal_into_the_account) {
    Harness h("cmd-give", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "boss", "password7"));
    client.joinGame(1920, 1080, {}, "Boss");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }, 200));

    const std::uint16_t rose = content().petalIndex("rose");
    CHECK(rose != kInvalidIndex);

    const auto roseCount = [&] {
        std::uint32_t total = 0;
        for (const Profile::Stack& stack : client.profile().inventory) {
            if (stack.petalIndex == rose && stack.rarity == Rarity::Legendary) total += stack.count;
        }
        return total;
    };
    CHECK_EQ(roseCount(), 0u);

    client.sendChat("/admin give boss rose legendary 3");
    // The give re-sends the profile, so waiting on the inventory is waiting on
    // the command rather than on a fixed number of ticks.
    CHECK(h.stepUntil({&client}, [&] { return roseCount() == 3u; }, 200));
    CHECK(sawText(client, "Gave 3x legendary rose"));
}

TEST(give_rejects_an_unknown_petal_and_an_unknown_rarity) {
    Harness h("cmd-give-bad", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "boss", "password7"));

    CHECK(say(h, client, "/admin give boss notapetal legendary"));
    CHECK(sawText(client, "Petal type \"notapetal\" does not exist"));

    CHECK(say(h, client, "/admin give boss rose notararity"));
    CHECK(sawText(client, "Invalid rarity. Valid rarities:"));
}

TEST(spawn_places_a_mob_and_killall_clears_it) {
    Harness h("cmd-spawn", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "boss", "password7"));
    client.joinGame(1920, 1080, {}, "Boss");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }, 200));

    const auto liveMobs = [&] {
        int count = 0;
        Query<MobTag> mobs{h.server.world()};
        mobs.each([&](Entity, MobTag&) { ++count; });
        return count;
    };

    const int before = liveMobs();
    CHECK(say(h, client, "/admin spawn bee rare 3 stack"));
    CHECK(sawText(client, "rare bee"));
    CHECK(liveMobs() > before);

    CHECK(say(h, client, "/admin killall"));
    // The destroy runs through the command buffer, so it lands on the tick
    // after the command rather than inside it.
    h.step(2, {&client});
    CHECK_EQ(liveMobs(), 0);
    CHECK(sawText(client, "pets left intact"));
}

TEST(spawn_with_a_bad_mob_type_spawns_nothing) {
    Harness h("cmd-spawn-bad", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "boss", "password7"));

    const auto liveMobs = [&] {
        int count = 0;
        Query<MobTag> mobs{h.server.world()};
        mobs.each([&](Entity, MobTag&) { ++count; });
        return count;
    };

    // A bad mob type is a server-console diagnostic and nothing else -- the
    // reference logs it inside spawnMob and still acknowledges in chat -- so
    // the only thing a player can observe is that nothing was spawned.
    const int before = liveMobs();
    CHECK(say(h, client, "/admin spawn notamob rare"));
    CHECK(sawText(client, "Spawned rare notamob"));
    CHECK_EQ(liveMobs(), before);
}

TEST(teleport_moves_the_flower_and_refuses_a_point_off_the_map) {
    Harness h("cmd-teleport", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "boss", "password7"));
    client.joinGame(1920, 1080, {}, "Boss");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }, 200));

    // A point taken from the OVERWORLD'S OWN extent, not a pair of round
    // numbers: every map states its size now, and the command refuses a
    // coordinate past the map -- so a hard-coded point is a test that starts
    // failing the moment an author resizes the world.
    //
    // And an OPEN one. The shipped map is about three fifths solid now --
    // collision is a layer property and water, dirt and castle all collide --
    // so the middle of the map is as likely to be inside a castle wall as not.
    // `tp` puts the body exactly where it is told, and the very next movement
    // substep shoves it out of a wall, which would read here as the command
    // having missed. The nearest open tile to the geometric target is still a
    // point nobody typed into this file, so the test still follows an author
    // resizing the world.
    const Terrain& terrain = h.server.terrain();
    const Vec2 extent = terrain.realmExtent(Realm::Overworld);
    int openTx = 0;
    int openTy = 0;
    CHECK(terrain.nearestOpenTile({extent.x * 0.5, extent.y * 0.25}, openTx, openTy,
                                  Realm::Overworld));
    const Vec2 openCentre = Terrain::tileCenter(openTx, openTy);
    const double targetX = std::floor(openCentre.x);
    const double targetY = std::floor(openCentre.y);
    CHECK(!terrain.blocked({targetX, targetY}, Realm::Overworld));
    CHECK(say(h, client, "/admin tp boss " + std::to_string(static_cast<long>(targetX)) + " " +
                             std::to_string(static_cast<long>(targetY))));
    CHECK(sawText(client, "Teleported"));

    bool landed = false;
    Query<PlayerTag, Transform, PlayerAccount> flowers{h.server.world()};
    flowers.each([&](Entity, PlayerTag&, Transform& transform, PlayerAccount& account) {
        if (account.username != "Boss") return;
        landed = std::abs(transform.position.x - targetX) < 1.0 &&
                 std::abs(transform.position.y - targetY) < 1.0;
    });
    CHECK(landed);

    // A coordinate past the map is a typo, and typing one is how the browser
    // build used to hang its tick loop. It is refused, not clamped.
    CHECK(say(h, client, "/admin tp boss 1e20 1e20"));
    CHECK(sawText(client, "Coordinates out of range"));
}

TEST(mute_stops_chat_but_not_commands) {
    Harness h("cmd-mute", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
        seedUser(path, "loud", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient boss;
    NetClient loud;
    CHECK(loginAs(h, boss, "boss", "password7"));
    CHECK(loginAs(h, loud, "loud", "password7"));

    CHECK(say(h, boss, "/admin mute loud"));
    CHECK(sawText(boss, "Muted loud"));

    const std::size_t bossLines = boss.chat().size();
    loud.sendChat("can you hear me");
    h.step(30, {&boss, &loud});
    CHECK_EQ(boss.chat().size(), bossLines);
    CHECK(sawText(loud, "You are muted"));

    // A mute bars a player from talking to other players, not from asking the
    // server about their own account.
    CHECK(say(h, loud, "/biome"));
    CHECK(sawText(loud, "populated biome"));

    CHECK(say(h, boss, "/admin unmute loud"));
    const std::size_t after = boss.chat().size();
    loud.sendChat("and now");
    CHECK(h.stepUntil({&boss, &loud}, [&] { return boss.chat().size() > after; }, 120));
    CHECK(sawText(boss, "and now"));
}

TEST(a_full_admin_cannot_be_muted) {
    Harness h("cmd-mute-admin", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
        seedUser(path, "other", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient boss;
    CHECK(loginAs(h, boss, "boss", "password7"));
    CHECK(say(h, boss, "/admin mute other"));
    CHECK(sawText(boss, "is a full admin and cannot be muted"));
}

TEST(a_temporary_grant_opens_the_console_and_closes_on_respawn) {
    Harness h("cmd-grant", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
        seedUser(path, "helper", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient boss;
    NetClient helper;
    CHECK(loginAs(h, boss, "boss", "password7"));
    CHECK(loginAs(h, helper, "helper", "password7"));
    helper.joinGame(1920, 1080, {}, "Helper");
    CHECK(h.stepUntil({&boss, &helper}, [&] { return helper.status() == NetClient::Status::Playing; }, 200));

    CHECK(!helper.isSkinAdmin());
    CHECK(say(h, boss, "/admin grant_admin helper"));
    CHECK(sawText(boss, "Granted temporary admin"));
    // The grantee's client is told, so its command autocomplete stops hiding
    // the admin rows.
    CHECK(h.stepUntil({&boss, &helper}, [&] { return helper.isSkinAdmin(); }, 120));

    CHECK(say(h, helper, "/admin list-players"));
    CHECK(sawText(helper, "[ADMIN] helper executed"));

    // The loan does not extend itself: a grantee may not hand out a successor.
    CHECK(say(h, helper, "/admin grant_admin boss"));
    CHECK(sawText(helper, "Only a full admin can grant or revoke admin access."));

    // And it ends with the life it was lent for.
    helper.requestRespawn();
    CHECK(h.stepUntil({&boss, &helper}, [&] { return !helper.isSkinAdmin(); }, 200));
    const std::size_t before = helper.chat().size();
    helper.sendChat("/admin list-players");
    CHECK(h.stepUntil({&boss, &helper}, [&] { return helper.chat().size() > before; }, 120));
    CHECK(sawText(helper, "Command does not exist."));
}

TEST(set_bot_count_clamps_and_applies) {
    Harness h("cmd-botcount", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "boss", "password7"));

    // The browser build reported the clamp and then returned WITHOUT applying
    // anything, so a count over the cap said it had capped and did nothing.
    // Both halves are asserted: the wording, and that it applied. The number
    // is derived from the cap rather than written out, so raising the ceiling
    // does not silently turn this into a test of the un-clamped path.
    CHECK(say(h, client, "/admin set_bot_count " + std::to_string(kMaxBots + 50)));
    CHECK(sawText(client, "capped at " + std::to_string(kMaxBots)));

    CHECK(say(h, client, "/admin set_bot_count default"));
    CHECK(sawText(client, "override cleared"));

    // A double space is not an error worth a diagnostic. The reply carries the
    // previous population after the target, so this matches the target alone.
    CHECK(say(h, client, "/admin set_bot_count  4"));
    CHECK(sawText(client, "Bot count target set to 4 (was "));
}

TEST(generate_code_mints_a_redeemable_code) {
    Harness h("cmd-code", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "boss", "password7"));
    CHECK(say(h, client, "/admin generate_code 250 3"));
    CHECK(sawText(client, "[CODE GENERATED]"));
    CHECK(sawText(client, "Stars: 250"));
    CHECK(sawText(client, "Max Uses: 3"));

    CHECK(say(h, client, "/admin list_codes"));
    CHECK(sawText(client, "Stars: 250"));

    // A refused mint says so rather than writing a zero-star code.
    CHECK(say(h, client, "/admin generate_code notanumber"));
    CHECK(sawText(client, "Usage: generate_code"));
}

TEST(notifications_are_appended_to_the_array_table_not_over_it) {
    Harness h("cmd-notify", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "boss", "password7"));

    CHECK(say(h, client, "/admin notify star_code Event starting now"));
    CHECK(sawText(client, "Notification created: Event starting now"));

    // The feed is the one unmodelled table stored as an ARRAY. Reading it back
    // through the notification request is what proves it was not coerced to an
    // empty object on the way in.
    client.requestNotifications(20, 0);
    CHECK(h.stepUntil({&client}, [&] { return !client.notifications().empty(); }, 200));
    CHECK_EQ(client.notifications().size(), 1u);
    CHECK_EQ(client.notifications()[0].message, std::string("Event starting now"));

    CHECK(say(h, client, "/admin notify nosuchtype hello"));
    CHECK(sawText(client, "Valid types:"));

    CHECK(say(h, client, "/admin clear_notifications"));
    CHECK(sawText(client, "Cleared 1 notification(s)"));
}

TEST(loadout_from_string_reports_the_build_the_world_would_spawn) {
    Harness h("cmd-fromstring", [](const std::string& path) {
        seedUser(path, "asker", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "asker", "password7"));

    CHECK(say(h, client, "/level-from-string m28"));
    // Whatever the level is, it is the SAME level the bot factory rolls -- one
    // shared roll, so the console cannot describe a bot the world would not
    // build.
    const BotIdentity expected = botIdentityForName("m28", kLoadoutActiveSlots, kMaxLevel);
    CHECK(sawText(client, "would be level " + std::to_string(expected.level)));

    CHECK(say(h, client, "/loadout-from-string m28"));
    CHECK(sawText(client, "Slot 1: " + std::string(rarityName(expected.slots[0].rarity)) + " " +
                              content().petal(expected.slots[0].petalIndex).id));

    CHECK(say(h, client, "/level-from-string"));
    CHECK(sawText(client, "Usage: /level-from-string &lt;name&gt;"));
}

TEST(a_command_this_build_lacks_is_left_unanswered) {
    Harness h("cmd-unsupported", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "boss", "password7"));

    // The reference's command chain falls off its last branch for an admin
    // verb it does not know, and the echo is all the operator sees. This build
    // says exactly as much -- no apology of its own about what it lacks.
    CHECK(say(h, client, "/admin nosuchverb"));
    CHECK(sawText(client, "[ADMIN] boss executed: nosuchverb"));
    CHECK(!sawText(client, "not available"));

    // `update` is the one command that answers with what this build cannot do,
    // because the thing it installs is a JavaScript build and a native server
    // has nowhere to put one. Note it does NOT back the database up first:
    // refusing has to happen before the step that writes a file.
    CHECK(say(h, client, "/admin update now"));
    CHECK(sawText(client, "cannot update itself"));
    CHECK(!sawText(client, "Step 1/4"));
}

TEST(a_squad_forms_talks_and_disbands) {
    Harness h("cmd-squad", [](const std::string& path) {
        seedUser(path, "lead", "password7");
        seedUser(path, "mate", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient lead;
    NetClient mate;
    CHECK(loginAs(h, lead, "lead", "password7"));
    CHECK(loginAs(h, mate, "mate", "password7"));

    CHECK(say(h, lead, "/squad-create"));
    CHECK(sawText(lead, "private squad created!"));
    CHECK(lead.squad().inSquad);
    CHECK(lead.squad().members.size() == 1);

    // The hyphen form and the space form are the same command: the reference
    // rewrites one into the other, and so does this.
    CHECK(say(h, lead, "/squad invite mate"));
    CHECK(sawText(lead, "Invite sent to mate."));
    // say() only polls the client it spoke for, and the invitation is a line
    // sent to the OTHER one.
    h.step(3, {&lead, &mate});
    CHECK(sawText(mate, "@lead has invited you to their squad."));

    CHECK(say(h, mate, "/squad-accept"));
    h.step(3, {&lead, &mate});
    CHECK(sawText(lead, "mate has joined the squad."));
    CHECK(lead.squad().members.size() == 2);
    CHECK(mate.squad().members.size() == 2);
    CHECK(mate.squad().id == lead.squad().id);

    CHECK(say(h, lead, "/squad-info"));
    CHECK(sawText(lead, "@lead [lead] (Leader)"));
    CHECK(sawText(lead, "@mate [mate]"));

    const std::size_t before = mate.chat().size();
    lead.sendChat("/s regroup");
    CHECK(h.stepUntil({&lead, &mate}, [&] { return mate.chat().size() > before; }));
    CHECK(sawText(mate, "regroup"));
    CHECK(sawText(lead, "regroup"));

    // The leader leaving promotes the next member and tells them both things.
    CHECK(say(h, lead, "/squad-leave"));
    h.step(3, {&lead, &mate});
    CHECK(sawText(lead, "You have left the squad."));
    CHECK(!lead.squad().inSquad);
    CHECK(sawText(mate, "mate is now the squad leader."));
    CHECK(sawText(mate, "lead has left the squad."));
    CHECK(mate.squad().members.size() == 1);
}

TEST(a_squad_invite_is_refused_when_it_should_be) {
    Harness h("cmd-squad-refuse", [](const std::string& path) {
        seedUser(path, "one", "password7");
        seedUser(path, "two", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient one;
    NetClient two;
    CHECK(loginAs(h, one, "one", "password7"));
    CHECK(loginAs(h, two, "two", "password7"));

    CHECK(say(h, one, "/squad-invite nobody"));
    CHECK(sawText(one, "Player \"nobody\" not found."));

    CHECK(say(h, one, "/squad-create"));
    CHECK(say(h, one, "/squad-invite one"));
    CHECK(sawText(one, "You cannot invite yourself."));

    // A member who is not the leader may not invite, which is the rule a squad
    // filling itself up from four directions at once would break.
    CHECK(say(h, one, "/squad-invite two"));
    CHECK(say(h, two, "/squad-accept"));
    h.step(3, {&one, &two});
    CHECK(say(h, two, "/squad-invite one"));
    CHECK(sawText(two, "Only the squad leader can invite players."));

    CHECK(say(h, one, "/squad-invite two"));
    CHECK(sawText(one, "That player is already in a squad."));
}

TEST(a_public_squad_is_listed_and_joinable) {
    Harness h("cmd-squad-public", [](const std::string& path) {
        seedUser(path, "host", "password7");
        seedUser(path, "guest", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient host;
    NetClient guest;
    CHECK(loginAs(h, host, "host", "password7"));
    CHECK(loginAs(h, guest, "guest", "password7"));

    CHECK(say(h, host, "/squad-create public"));
    CHECK(sawText(host, "public squad created!"));

    CHECK(say(h, guest, "/squad-find-public"));
    CHECK(sawText(guest, "Public squads:"));
    CHECK(sawText(guest, "leader: host"));

    const std::string squadId = host.squad().id;
    CHECK(!squadId.empty());
    CHECK(say(h, guest, "/squad-join " + squadId));
    h.step(3, {&host, &guest});
    CHECK(sawText(host, "guest has joined the squad."));
    CHECK(guest.squad().members.size() == 2);

    // A private squad drops off the listing, which is the whole difference
    // between the two.
    CHECK(say(h, host, "/squad-private"));
    h.step(3, {&host, &guest});
    CHECK(sawText(guest, "Squad is now private."));
    CHECK(!host.squad().isPublic);
}

TEST(a_squadmate_is_streamed_across_the_map) {
    Harness h("cmd-squad-view", [](const std::string& path) {
        seedUser(path, "near", "password7", true);
        seedUser(path, "far", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient near;
    NetClient far;
    CHECK(loginAs(h, near, "near", "password7"));
    CHECK(loginAs(h, far, "far", "password7"));
    near.joinGame(1280, 720, {}, "near");
    far.joinGame(1280, 720, {}, "far");
    CHECK(h.stepUntil({&near, &far}, [&] {
        return near.status() == NetClient::Status::Playing &&
               far.status() == NetClient::Status::Playing;
    }));

    // Half a world apart: well outside the four-screen box the replicator
    // streams, so nothing but a squad could put them in each other's view.
    CHECK(say(h, near, "/admin teleport near 1000 1000"));
    CHECK(say(h, near, "/admin teleport far 30000 30000"));
    h.step(20, {&near, &far});

    const auto sees = [](const NetClient& viewer, const NetClient& other) {
        return viewer.view().entities().count(other.view().self().netId) != 0;
    };
    CHECK(!sees(near, far));

    CHECK(say(h, near, "/squad-create"));
    CHECK(say(h, near, "/squad-invite far"));
    h.step(3, {&near, &far});
    CHECK(say(h, far, "/squad-accept"));
    h.step(20, {&near, &far});

    // Both directions: the exemption is the VIEWER's squad, so it has to be
    // applied per recipient rather than to the entity being looked at.
    CHECK(sees(near, far));
    CHECK(sees(far, near));
    CHECK(near.squad().members.size() == 2);
    for (const SquadState::Member& member : near.squad().members) {
        // A member in the world carries the wire id the minimap and the party
        // bars find their body by.
        CHECK(member.netId != 0);
    }

    // And it stops when the squad does.
    CHECK(say(h, far, "/squad-leave"));
    h.step(20, {&near, &far});
    CHECK(!sees(near, far));
}

TEST(a_restart_warns_and_can_be_called_off) {
    Harness h("cmd-restart", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
        seedUser(path, "player", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient boss;
    NetClient player;
    CHECK(loginAs(h, boss, "boss", "password7"));
    CHECK(loginAs(h, player, "player", "password7"));

    CHECK(say(h, boss, "/admin restart cancel"));
    CHECK(sawText(boss, "No pending restart to cancel."));

    CHECK(say(h, boss, "/admin restart 12s"));
    CHECK(sawText(boss, "Restart scheduled in 12s."));

    CHECK(say(h, boss, "/admin restart status"));
    CHECK(sawText(boss, "(reason: admin)."));

    // Every player is warned, not just the one who typed it. Two seconds of
    // ticks reaches the ten-second mark and no other.
    h.step(90, {&boss, &player});
    CHECK(sawText(player, "Server restarting in 10 seconds!"));
    CHECK(!sawText(player, "Server will restart in 1 minute"));

    CHECK(say(h, boss, "/admin restart cancel"));
    CHECK(sawText(boss, "Pending restart cancelled."));

    // And nothing fires afterwards: fifteen more seconds of ticks past what
    // was the deadline.
    const std::size_t before = player.chat().size();
    h.step(450, {&boss, &player});
    CHECK(player.chat().size() == before);
}

TEST(the_database_backs_up_and_lists_its_backups) {
    Harness h("cmd-backup", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient boss;
    CHECK(loginAs(h, boss, "boss", "password7"));

    CHECK(say(h, boss, "/admin backup_db"));
    CHECK(sawText(boss, "Database backed up to"));
    CHECK(sawText(boss, "-manual-boss.json"));

    CHECK(say(h, boss, "/admin backup_db list"));
    CHECK(sawText(boss, "Database backups (1, newest first):"));
    CHECK(sawText(boss, "To restore: copy a backup over inventory.json"));

    CHECK(say(h, boss, "/admin backup_db everything"));
    CHECK(sawText(boss, "Usage: backup_db [list]"));

    // The snapshot is a real, complete file: anything less is worse than none,
    // because it looks like a rescue and is not one.
    const std::vector<Database::BackupInfo> backups = reopenBackups(h);
    CHECK(backups.size() == 1);
    for (const Database::BackupInfo& info : backups) {
        Database written;
        std::string error;
        CHECK(written.load(info.file, error));
        CHECK(written.findUser("boss") != nullptr);
        std::remove(info.file.c_str());
    }
}

TEST(change_maze_rotates_the_active_maze) {
    Harness h("cmd-maze", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient boss;
    CHECK(loginAs(h, boss, "boss", "password7"));

    const std::int64_t started = activeMaze().day();
    CHECK(say(h, boss, "/admin change-maze next"));
    CHECK(sawText(boss, "Maze changed to day " + std::to_string(started + 1)));
    CHECK(activeMaze().day() == started + 1);

    CHECK(say(h, boss, "/admin change-maze rubbish"));
    CHECK(sawText(boss, "Usage: change-maze [next|garden|desert|ocean|"));

    // Back where the rest of the suite expects it: the active maze is one
    // process-wide object, so a test that moves it has to put it back.
    CHECK(say(h, boss, "/admin change-maze " + std::to_string(started)));
    CHECK(activeMaze().day() == started);
}

TEST(api_keys_are_minted_and_revoked_per_account) {
    Harness h("cmd-apikey", [](const std::string& path) {
        seedUser(path, "owner", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(h, client, "owner", "password7"));

    CHECK(say(h, client, "/create-api-key discord-bot"));
    CHECK(sawText(client, "[API KEY CREATED]"));
    CHECK(sawText(client, "Label: discord-bot"));
    CHECK(sawText(client, "sk_"));

    CHECK(say(h, client, "/delete-api-key sk_"));
    CHECK(sawText(client, "Deleted API key \"discord-bot\""));

    CHECK(say(h, client, "/delete-api-key sk_"));
    CHECK(sawText(client, "No API key of yours matched"));
}

TEST(guild_commands_reach_the_same_logic_as_the_guild_panel) {
    Harness h("cmd-guild", [](const std::string& path) {
        seedUser(path, "leader", "password7");
        seedUser(path, "member", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient leader;
    NetClient member;
    CHECK(loginAs(h, leader, "leader", "password7"));
    CHECK(loginAs(h, member, "member", "password7"));

    CHECK(say(h, leader, "/guild-create AB12"));
    // The same validation the binary GuildCreate applies: exactly five
    // alphanumerics, because the tag hangs under a nameplate.
    CHECK(sawText(leader, "exactly 5 alphanumeric characters"));

    CHECK(say(h, leader, "/guild-create ABC12"));
    CHECK(sawText(leader, "created"));

    CHECK(say(h, leader, "/guild-invite member"));
    CHECK(sawText(leader, "Guild invite sent to member"));

    CHECK(say(h, member, "/guild-accept"));
    CHECK(say(h, leader, "/guild-info"));
    CHECK(sawText(leader, "ABC12"));
    CHECK(sawText(leader, "member"));

    // A guild message reaches the guild.
    const std::size_t before = member.chat().size();
    leader.sendChat("/g meeting at the lake");
    CHECK(h.stepUntil({&leader, &member},
                      [&] { return member.chat().size() > before; }, 120));
    CHECK(sawText(member, "meeting at the lake"));
}

TEST(delete_guests_keeps_a_guest_that_actually_played) {
    Harness h("cmd-guests", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
        seedUser(path, "User12345678", "password7");
        seedUser(path, "User87654321", "password7");
    });
    if (!h.ready) { CHECK(false); return; }

    // One of the two guests has XP, which makes it a player whose account
    // happens to be named like a guest.
    {
        Database db;
        std::string error;
        db.load(h.dbPath, error);
        if (const Account* played = db.findUser("User87654321")) {
            db.progress(played->id).totalXp = 5000;
            db.markDirty();
            db.save();
        }
    }
    // The server already loaded the file this test just edited, so restart the
    // check against a server that sees the edit.
    Harness fresh("cmd-guests-2", [&](const std::string& path) {
        Json root;
        std::string error;
        if (Json::parseFile(h.dbPath, root, error)) root.writeFile(path, 2);
    });
    if (!fresh.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginAs(fresh, client, "boss", "password7"));
    CHECK(say(fresh, client, "/admin delete_guests"));
    CHECK(sawText(client, "Deleted 1 guest account(s) and their player data."));
}
