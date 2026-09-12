#include "test.h"

#include "server_harness.h"
#include "shared/game/shop.h"
#include "shared/game/skills.h"

#include <fstream>

using namespace flix;
using flix::testsupport::connectClient;
using flix::testsupport::dataDir;
using flix::testsupport::Harness;
using flix::testsupport::loginNew;

// What the menus can actually do to an account: spend talent points, buy a
// petal, wear a skin, and read the board. Every one of these is a request the
// client sends and a Profile it must get back, so they are tested over a real
// socket rather than against the handlers directly.

namespace {

/// What the first authentication of a UTC day pays. Every account these tests
/// create or seed has never claimed, so its streak starts at one and the
/// daily-login reward is one star -- on top of whatever the test put there.
constexpr int kFirstLoginStars = 1;

/// Waits for a profile whose `predicate` holds. The server answers every one
/// of these requests with a fresh Profile, so a state change is observable
/// without polling anything else.
template <class F>
bool awaitProfile(Harness& h, NetClient& client, F predicate) {
    return h.stepUntil({&client}, [&] { return predicate(client.profile()); }, 200);
}

/// Writes an account with the given stars and kills straight into a database
/// file, for the cases a test cannot reach by playing.
void seedAccount(const std::string& path, const std::string& username,
                 const std::string& password, int stars, double totalXp) {
    Database db;
    std::string error;
    db.load(path, error);
    db.setPasswordCost(4);   // the default cost makes this the slowest test
    const CreateResult created = db.createUser(username, password);
    if (!created.ok()) return;
    PlayerRecord& record = db.progress(created.account->id);
    record.stars = stars;
    record.totalXp = totalXp;
    record.recordKill("bee", Rarity::Common);
    record.recordKill("bee", Rarity::Common);
    record.recordKill("bee", Rarity::Rare);
    db.markDirty();
    db.save();
}

/// Puts a star code into the database's own `codes` table -- the same key the
/// browser build's admin commands write and this one round-trips.
void seedCode(const std::string& path, const std::string& code, int stars, int maxUses) {
    Database db;
    std::string error;
    db.load(path, error);
    Json entry = Json::object();
    entry["code"] = Json(code);
    entry["stars"] = Json(stars);
    entry["uses"] = Json(0);
    if (maxUses > 0) entry["maxUses"] = Json(maxUses);
    db.rawTable("codes")[code] = std::move(entry);
    db.markDirty();
    db.save();
}

/// Gives an account a stack to craft with, which no amount of playing would
/// hand out reliably.
void seedStack(const std::string& path, const std::string& username, const char* itemKey,
               Rarity rarity, int count) {
    Database db;
    std::string error;
    db.load(path, error);
    const Account* account = db.findUser(username);
    if (account == nullptr) return;
    db.progress(account->id).addItem(rarity, itemKey, count);
    db.markDirty();
    db.save();
}

/// Waits for the shop's reply channel to answer, and hands the answer back
/// with `pending` cleared, as the panel reads it.
bool awaitShopAnswer(Harness& h, NetClient& client, ShopOutcome& out) {
    if (!h.stepUntil({&client}, [&] { return client.shopOutcome().pending; }, 200)) return false;
    out = client.shopOutcome();
    client.shopOutcome().pending = false;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Talent points
// ---------------------------------------------------------------------------

TEST(talent_points_are_the_level_minus_what_the_tree_holds) {
    SkillSet skills;
    CHECK_EQ(skills.spent(), 0);
    CHECK_EQ(availableTalentPoints(10, skills), 10);

    // Buying rare means having bought common and uncommon too, so the cost is
    // the sum of the ladder, not the price of the top tier.
    skills.set(SkillId::Damage, rarityIndex(Rarity::Rare));
    CHECK_EQ(skills.spent(), 1 + 2 + 3);
    CHECK_EQ(availableTalentPoints(10, skills), 4);

    // A tree worth more than the level can never mint negative points.
    CHECK_EQ(availableTalentPoints(2, skills), 0);
}

TEST(a_talent_multiplier_is_neutral_until_a_tier_is_bought) {
    SkillSet skills;
    CHECK_NEAR(skills.statScale(SkillId::PlayerHealth), 1.0, 1e-9);
    skills.set(SkillId::PlayerHealth, rarityIndex(Rarity::Mythic));
    CHECK_NEAR(skills.statScale(SkillId::PlayerHealth), 1.5, 1e-9);
    CHECK_NEAR(skills.effectScale(SkillId::Healing), 1.0, 1e-9);
}

TEST(talents_are_bought_one_tier_at_a_time) {
    Harness h("talent-order");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "talentine", "password7"));
    CHECK_EQ(client.profile().talentPoints(), 1);

    // Skipping to a higher tier is refused outright: accepting it would sell
    // three tiers for the price of one.
    client.requestUpgradeSkill(SkillId::Damage, rarityIndex(Rarity::Rare));
    h.step(20, {&client});
    CHECK_EQ(client.profile().skills.level(SkillId::Damage), -1);

    client.requestUpgradeSkill(SkillId::Damage, 0);
    CHECK(awaitProfile(h, client, [](const Profile& p) {
        return p.skills.level(SkillId::Damage) == 0;
    }));
    CHECK_EQ(client.profile().talentPoints(), 0);

    // The second tier costs two, and a level-1 account has spent its only one.
    client.requestUpgradeSkill(SkillId::Damage, 1);
    h.step(20, {&client});
    CHECK_EQ(client.profile().skills.level(SkillId::Damage), 0);
}

TEST(second_chance_stays_locked_until_flower_health_is_rare) {
    Harness h("talent-fork");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "forkner", "password7"));
    client.requestUpgradeSkill(SkillId::SecondChance, 0);
    h.step(20, {&client});
    CHECK_EQ(client.profile().skills.level(SkillId::SecondChance), -1);
    // The prerequisite is a property of the tree, not of the panel: the same
    // answer has to come back from a client that never drew a node.
    CHECK(!client.profile().skills.secondChanceUnlocked());
}

TEST(resetting_talents_hands_every_point_back) {
    Harness h("talent-reset");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "resetta", "password7"));
    client.requestUpgradeSkill(SkillId::PetalHealth, 0);
    CHECK(awaitProfile(h, client, [](const Profile& p) {
        return p.skills.level(SkillId::PetalHealth) == 0;
    }));
    CHECK_EQ(client.profile().talentPoints(), 0);

    client.requestResetSkills();
    CHECK(awaitProfile(h, client, [](const Profile& p) {
        return p.skills.level(SkillId::PetalHealth) == -1;
    }));
    CHECK_EQ(client.profile().talentPoints(), 1);
}

TEST(a_bought_talent_survives_a_reconnect) {
    Harness h("talent-persist");
    if (!h.ready) { CHECK(false); return; }

    NetClient first;
    CHECK(loginNew(h, first, "lasting", "password7"));
    first.requestUpgradeSkill(SkillId::PlayerHealth, 0);
    CHECK(awaitProfile(h, first, [](const Profile& p) {
        return p.skills.level(SkillId::PlayerHealth) == 0;
    }));
    first.disconnect();
    h.step(5, {});

    NetClient second;
    CHECK(connectClient(h, second));
    second.requestLogin("lasting", "password7");
    CHECK(h.stepUntil({&second}, [&] { return second.status() == NetClient::Status::LoggedIn; }));
    CHECK(awaitProfile(h, second, [](const Profile& p) {
        return p.skills.level(SkillId::PlayerHealth) == 0;
    }));
}

// ---------------------------------------------------------------------------
// The shop
// ---------------------------------------------------------------------------

TEST(shop_prices_climb_by_tier_and_top_tiers_are_not_for_sale) {
    const double common = shopPrice("basic", Rarity::Common);
    const double uncommon = shopPrice("basic", Rarity::Uncommon);
    CHECK_NEAR(common, 10.0, 1e-9);
    CHECK_NEAR(uncommon, 35.0, 1e-9);
    CHECK(shopPrice("basic", Rarity::Legendary) > uncommon);
    // An unlisted petal still has a price, or it would be free.
    CHECK(shopPrice("a-petal-that-does-not-exist", Rarity::Common) > 0);

    CHECK(shopSellsRarity(Rarity::Super));
    CHECK(!shopSellsRarity(Rarity::Unique));
    CHECK(!shopSellsRarity(Rarity::Apex));
}

TEST(the_store_rotates_hourly_and_both_sides_derive_the_same_ten_cards) {
    const std::int64_t rotation = shopRotation(1'700'000'000);
    // The same hour is the same store, twice over: the generator is what the
    // client draws and what the server prices against, so a second call that
    // disagreed would be a purchase refused at random.
    const std::vector<ShopOffer> first = shopOffers(rotation);
    const std::vector<ShopOffer> again = shopOffers(rotation);
    CHECK_EQ(first.size(), static_cast<std::size_t>(kShopOfferCount));
    CHECK_EQ(again.size(), first.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK_EQ(again[i].petalIndex, first[i].petalIndex);
        CHECK_EQ(static_cast<int>(again[i].rarity), static_cast<int>(first[i].rarity));
        CHECK_NEAR(again[i].price, first[i].price, 1e-9);
    }

    // The next hour is a different store.
    const std::vector<ShopOffer> next = shopOffers(rotation + 1);
    bool moved = false;
    for (std::size_t i = 0; i < first.size() && i < next.size(); ++i) {
        if (next[i].petalIndex != first[i].petalIndex) moved = true;
    }
    CHECK(moved);

    // An hour is an hour, and a card never costs more than the ladder says.
    const std::int64_t started = shopRotationEnd(rotation) - kShopRotationSeconds;
    CHECK_EQ(shopRotation(started), rotation);
    CHECK_EQ(shopRotation(started + kShopRotationSeconds - 1), rotation);
    CHECK_EQ(shopRotation(started + kShopRotationSeconds), rotation + 1);
    for (const ShopOffer& offer : first) {
        CHECK(shopSellsPetal(offer.petalIndex));
        CHECK(shopSellsRarity(offer.rarity));
        CHECK(offer.price > 0);
        CHECK(offer.price <= shopPrice(offer.petalIndex, offer.rarity));
        CHECK(offer.discountPercent >= 0 && offer.discountPercent <= 30);
    }
}

TEST(an_offer_is_charged_its_discounted_price_and_a_wrong_slot_is_refused) {
    const std::vector<ShopOffer> offers = shopOffers(shopRotation(shopClockNow()));
    if (offers.empty()) { CHECK(false); return; }

    // The cheapest card, so one seeded balance covers whatever the hour rolled.
    std::size_t slot = 0;
    for (std::size_t i = 1; i < offers.size(); ++i) {
        if (offers[i].price < offers[slot].price) slot = i;
    }
    const ShopOffer& offer = offers[slot];
    const int price = static_cast<int>(offer.price);

    Harness h("shop-offer", [&](const std::string& path) {
        seedAccount(path, "olive", "password7", price + 5, 0);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("olive", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.stars == price + 5 + kFirstLoginStars;
    }));

    const std::uint32_t before = client.profile().stackCount(offer.petalIndex, offer.rarity);
    client.requestBuyPetal(offer.petalIndex, offer.rarity, static_cast<int>(slot));
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.stars == 5 + kFirstLoginStars;
    }));
    CHECK_EQ(client.profile().stackCount(offer.petalIndex, offer.rarity), before + 1);

    // A slot that does not hold what it claims buys nothing: the client names
    // the card, and the server checks that card against its own rotation.
    const int stars = client.profile().stars;
    const std::size_t other = (slot + 1) % offers.size();
    client.requestBuyPetal(offer.petalIndex, offer.rarity, static_cast<int>(other));
    ShopOutcome answer;
    CHECK(awaitShopAnswer(h, client, answer));
    CHECK(!answer.ok);
    CHECK_EQ(client.profile().stars, stars);
}

TEST(a_purchase_without_the_stars_is_refused) {
    Harness h("shop-broke");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "pauline", "password7"));
    const std::uint16_t rose = content().petalIndex("rose");
    if (rose == kInvalidIndex) { CHECK(false); return; }
    const std::uint32_t before = client.profile().stackCount(rose, Rarity::Common);

    client.requestBuyPetal(rose, Rarity::Common);
    h.step(20, {&client});
    CHECK_EQ(client.profile().stackCount(rose, Rarity::Common), before);
    CHECK_EQ(client.profile().stars, kFirstLoginStars);
}

TEST(a_purchase_spends_the_servers_price_not_the_clients) {
    const std::uint16_t rose = content().petalIndex("rose");
    if (rose == kInvalidIndex) { CHECK(false); return; }
    const int price = static_cast<int>(shopPrice("rose", Rarity::Common));

    Harness h("shop-buy", [&](const std::string& path) {
        seedAccount(path, "richard", "password7", price + 5, 0);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("richard", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.stars == price + 5 + kFirstLoginStars;
    }));

    const std::uint32_t before = client.profile().stackCount(rose, Rarity::Common);
    client.requestBuyPetal(rose, Rarity::Common);
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.stars == 5 + kFirstLoginStars;
    }));
    CHECK_EQ(client.profile().stackCount(rose, Rarity::Common), before + 1);

    // Unique is not for sale at any balance.
    client.requestBuyPetal(rose, Rarity::Unique);
    h.step(20, {&client});
    CHECK_EQ(client.profile().stars, 5 + kFirstLoginStars);
    CHECK_EQ(client.profile().stackCount(rose, Rarity::Unique), 0u);
}

TEST(a_star_code_pays_once_and_names_its_refusals) {
    Harness h("shop-code", [](const std::string& path) {
        seedAccount(path, "coder", "password7", 0, 0);
        seedCode(path, "FREESTARS", 250, 0);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("coder", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    CHECK(awaitProfile(h, client, [&](const Profile& p) { return p.stars == kFirstLoginStars; }));

    // Typed the way a player types it: trimmed and upper-cased server-side.
    ShopOutcome answer;
    client.requestRedeemCode("  freestars ");
    CHECK(awaitShopAnswer(h, client, answer));
    CHECK(answer.redeem);
    CHECK(answer.ok);
    CHECK_EQ(answer.stars, 250);
    CHECK(awaitProfile(h, client, [&](const Profile& p) { return p.stars == 250 + kFirstLoginStars; }));

    // The same account cannot spend it twice, and an unknown code says so
    // rather than quietly doing nothing.
    client.requestRedeemCode("FREESTARS");
    CHECK(awaitShopAnswer(h, client, answer));
    CHECK(!answer.ok);
    CHECK_EQ(answer.message, std::string("Code already redeemed"));
    client.requestRedeemCode("NOTACODE");
    CHECK(awaitShopAnswer(h, client, answer));
    CHECK(!answer.ok);
    CHECK_EQ(answer.message, std::string("Invalid code"));
    CHECK_EQ(client.profile().stars, 250 + kFirstLoginStars);
}

TEST(a_refused_purchase_answers_on_the_shops_own_channel) {
    Harness h("shop-refuse", [](const std::string& path) {
        seedAccount(path, "pauper", "password7", 0, 0);
    });
    if (!h.ready) { CHECK(false); return; }

    const std::uint16_t rose = content().petalIndex("rose");
    if (rose == kInvalidIndex) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("pauper", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));

    ShopOutcome answer;
    client.requestBuyPetal(rose, Rarity::Common);
    CHECK(awaitShopAnswer(h, client, answer));
    CHECK(!answer.redeem);
    CHECK(!answer.ok);
    CHECK_EQ(answer.message, std::string("Not enough stars."));
}

TEST(a_craft_request_is_pooled_and_answered_once) {
    Harness h("craft-pool", [](const std::string& path) {
        seedAccount(path, "smith", "password7", 0, 0);
        seedStack(path, "smith", "petal_rose", Rarity::Common, 15);
    });
    if (!h.ready) { CHECK(false); return; }

    const std::uint16_t rose = content().petalIndex("rose");
    if (rose == kInvalidIndex) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("smith", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.stackCount(rose, Rarity::Common) == 15u;
    }));

    // Three batches in ONE request: the server crafts them as a pool, recycling
    // what a failure hands back until fewer than five are left, and answers
    // with a single result rather than one per batch.
    client.requestCraft(rose, Rarity::Common, 15);
    CHECK(h.stepUntil({&client}, [&] { return client.craftOutcome().pending; }, 200));
    const CraftOutcome outcome = client.craftOutcome();
    client.craftOutcome().pending = false;

    // Whatever the rolls were, the pool is spent down to a sub-batch tail, and
    // the two counts the panel draws from are the account's actual holdings.
    CHECK(outcome.petalsReturned < 5);
    CHECK_EQ(outcome.success, outcome.crafted > 0);
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.stackCount(rose, Rarity::Common) ==
                   static_cast<std::uint32_t>(outcome.petalsReturned) &&
               p.stackCount(rose, Rarity::Uncommon) ==
                   static_cast<std::uint32_t>(outcome.crafted);
    }));

    // Nothing else follows: one request, one result.
    h.step(20, {&client});
    CHECK(!client.craftOutcome().pending);
}

TEST(a_mythic_kill_is_worth_stars_and_a_common_one_is_not) {
    CHECK_EQ(starsForKill(Rarity::Common), 0);
    CHECK_EQ(starsForKill(Rarity::Legendary), 0);
    CHECK_EQ(starsForKill(Rarity::Mythic), 1);
    CHECK_EQ(starsForKill(Rarity::Apex), 250);
}

// ---------------------------------------------------------------------------
// Skins, the kill ledger and the board
// ---------------------------------------------------------------------------

TEST(a_skin_is_stored_and_an_unknown_one_is_ignored) {
    Harness h("skins");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "skinner", "password7"));
    CHECK_EQ(client.profile().renderFlags, 0u);

    client.requestSkin(PlayerRenderPumpkin);
    CHECK(awaitProfile(h, client, [](const Profile& p) {
        return p.renderFlags == PlayerRenderPumpkin;
    }));

    // Two bits at once, and the transient glitch bit, are both refused: a skin
    // is a choice of one, and glitch is an effect rather than a cosmetic.
    client.requestSkin(PlayerRenderPumpkin | PlayerRenderRobot);
    h.step(20, {&client});
    CHECK_EQ(client.profile().renderFlags, static_cast<std::uint32_t>(PlayerRenderPumpkin));
    client.requestSkin(PlayerRenderGlitch);
    h.step(20, {&client});
    CHECK_EQ(client.profile().renderFlags, static_cast<std::uint32_t>(PlayerRenderPumpkin));

    client.requestSkin(PlayerRenderNone);
    CHECK(awaitProfile(h, client, [](const Profile& p) { return p.renderFlags == 0u; }));
}

TEST(the_kill_ledger_reaches_the_gallery) {
    Harness h("kills", [](const std::string& path) {
        seedAccount(path, "hunter", "password7", 0, 500);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("hunter", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));

    const std::uint16_t bee = content().mobIndex("bee");
    if (bee == kInvalidIndex) { CHECK(false); return; }
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.killCount(bee, Rarity::Common) == 2;
    }));
    CHECK_EQ(client.profile().killCount(bee, Rarity::Rare), 1u);
    CHECK_EQ(client.profile().killCount(bee, Rarity::Mythic), 0u);
}

TEST(the_leaderboard_ranks_accounts_by_lifetime_xp) {
    Harness h("board", [](const std::string& path) {
        seedAccount(path, "small", "password7", 0, 10);
        seedAccount(path, "large", "password7", 0, 900000);
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("small", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));

    client.requestLeaderboard();
    CHECK(h.stepUntil({&client}, [&] { return !client.leaderboard().empty(); }, 200));
    CHECK_EQ(client.leaderboard().size(), static_cast<std::size_t>(2));
    CHECK_EQ(client.leaderboard()[0].name, std::string("large"));
    CHECK(client.leaderboard()[0].totalXp > client.leaderboard()[1].totalXp);
    // The level travels with the row: the panel must not have to re-derive it
    // from XP with a curve that could drift from the server's.
    CHECK(client.leaderboard()[0].level > client.leaderboard()[1].level);
}

TEST(killing_a_mob_credits_the_ledger_and_pays_its_stars) {
    Harness h("kill-credit");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "slayer", "password7"));
    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    // Reaching into the world is the only way to make a kill deterministic:
    // waiting for a real fight to resolve is a test that fails on a slow
    // machine. Everything from the damage onward is the shipping path.
    World& world = h.server.world();
    Entity player = NULL_ENTITY;
    Query<PlayerTag, Transform> players{world};
    players.each([&](Entity e, PlayerTag&, Transform&) { player = e; });
    CHECK(player != NULL_ENTITY);
    if (player == NULL_ENTITY) return;

    // Mobs arrive on the spawner's own schedule, and the starter petals need a
    // tick or two to reach the ring, so wait for both -- and take a mob IN THE
    // PLAYER'S REALM. Every map with spawn bands stocks its own, and one from
    // another map cannot be parked on this flower: the park below moves a
    // position, and the same position in another coordinate space is not the
    // same place, so the two never touch and the kill never happens.
    const Realm realm = world.get<Transform>(player).realm;
    Entity mob = NULL_ENTITY;
    Query<MobTag, MobType, Transform> mobs{world};
    Query<PetalInstance, Transform> petals{world};
    const bool armed = h.stepUntil({&client}, [&] {
        mob = NULL_ENTITY;
        mobs.each([&](Entity e, MobTag&, MobType&, Transform& at) {
            if (mob == NULL_ENTITY && at.realm == realm) mob = e;
        });
        bool anyPetal = false;
        petals.each([&](Entity, PetalInstance&, Transform&) { anyPetal = true; });
        return mob != NULL_ENTITY && anyPetal;
    }, 600);
    CHECK(armed);
    if (!armed) return;

    const std::uint16_t mobIndex = world.get<MobType>(mob).configIndex;
    // Mythic, so the kill is worth a star as well as a tally.
    world.get<MobType>(mob).rarity = Rarity::Mythic;

    const bool credited = h.stepUntil({&client}, [&] {
        if (client.profile().killCount(mobIndex, Rarity::Mythic) > 0) return true;
        if (!world.isAlive(mob) || world.has<Dead>(mob)) return false;
        // Park the mob on the flower, one hit from death. The TypeScript
        // player pipeline resolves this body contact before moveEnemies(), so
        // this is deterministic without predicting next tick's petal angle.
        world.get<Transform>(mob).position = world.get<Transform>(player).position;
        world.get<Health>(mob).current = 1.0;
        world.get<Health>(mob).invulnerableUntilMillis = 0;
        world.get<Health>(player).current = world.get<Health>(player).max;
        return false;
    }, 200);
    CHECK(credited);
    if (!credited) return;
    CHECK_EQ(client.profile().stars, starsForKill(Rarity::Mythic) + kFirstLoginStars);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

TEST(a_talent_tree_round_trips_through_the_database) {
    const std::string path = flix::testsupport::tempPath("skill-json");
    std::remove(path.c_str());
    {
        Database db;
        std::string error;
        db.load(path, error);
        db.setPasswordCost(4);
        const CreateResult created = db.createUser("treeholder", "password7");
        CHECK(created.ok());
        if (!created.ok()) return;
        PlayerRecord& record = db.progress(created.account->id);
        record.totalXp = 100000;
        record.skills.set(SkillId::Healing, rarityIndex(Rarity::Epic));
        record.skills.set(SkillId::Absorbing, rarityIndex(Rarity::Common));
        db.markDirty();
        CHECK(db.save());
    }

    Database reopened;
    std::string error;
    CHECK(reopened.load(path, error));
    const Account* account = reopened.findUser("treeholder");
    CHECK(account != nullptr);
    if (account != nullptr) {
        const PlayerRecord* record = reopened.findProgress(account->id);
        CHECK(record != nullptr);
        if (record != nullptr) {
            CHECK_EQ(record->skills.level(SkillId::Healing), rarityIndex(Rarity::Epic));
            CHECK_EQ(record->skills.level(SkillId::Absorbing), rarityIndex(Rarity::Common));
            CHECK_EQ(record->skills.level(SkillId::Damage), -1);
        }
    }
    std::remove(path.c_str());
}
