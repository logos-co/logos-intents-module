#include <logos_test.h>

#include "intent_broker.h"
#include "intent_rules.h"
#include "test_support.h"

#include <map>

using namespace intents;
using test::intentList;
using test::ofKind;
using test::pkg;
using test::workOfKind;

namespace {

using K = Effect::Kind;

// What the test's presenter does with a request waiting as pending.
enum class Presenter {
    None,              // nobody has claimed
    OpensEverything,   // as a terminal does, and Basecamp until its Activity view
    OpensWhenTold,     // leaves it in the pending list for the test to open
};

// wallet_module asks for signing.approve; signer_ui_backend (an app's backend)
// and cli_signer provide it, signer_ui_backend with declared params.
struct World {
    IntentRegistry registry;
    IntentBroker broker;
    Presenter presenter;
    std::int64_t now = 1000;
    json held = json::array();   // work the presenter pulled and hasn't handed out

    explicit World(Presenter presenter = Presenter::OpensEverything, bool twoProviders = false,
                   BrokerConfig config = {})
        : broker(registry, test::counterIds(), config), presenter(presenter)
    {
        json signerProvides = json::array({json{
            {"intent", "signing.approve"},
            {"params", json::array({json{{"name", "handle"}, {"type", "string"}, {"required", true}}})},
        }});
        registry.declareShell("basecamp", "basecamp.", {}, {"basecamp.settings.open"}, {}, "Basecamp", "");
        registry.ingest("wallet_module", pkg("wallet_module", "core", json::array(),
                                             intentList({"signing.approve", "basecamp.settings.open",
                                                         "nobody.provides"})), "", "");
        registry.ingest("signer_ui_backend", pkg("signer_ui_backend", "core", signerProvides), "Signer", "");
        if (twoProviders)
            registry.ingest("cli_signer", pkg("cli_signer", "core", intentList({"signing.approve"})), "", "");
        if (presenter != Presenter::None) broker.setClaimed();
    }

    IntentBroker::Ticket ask(const std::string& requester = "wallet_module",
                             const std::string& intent = "signing.approve",
                             json params = json{{"handle", "h1"}})
    {
        const auto t = broker.submit(requester, intent, params, /*fromPresenter=*/false, now);
        if (presenter == Presenter::OpensEverything) pull();
        return t;
    }

    // The presenter's work. One that opens everything pulls at once and opens
    // each pending request; the rest is handed out here.
    json takeWork()
    {
        pull();
        json out = std::move(held);
        held = json::array();
        return out;
    }

    void pull()
    {
        for (json items = broker.takeWork(); !items.empty(); items = broker.takeWork()) {
            bool opened = false;
            for (json& item : items) {
                if (presenter == Presenter::OpensEverything && item.value("kind", "") == "pending") {
                    broker.answer(item["id"].get<std::string>(), json{{"action", "open"}}, now);
                    opened = true;
                } else {
                    held.push_back(std::move(item));
                }
            }
            if (!opened) break;
        }
    }

    std::string settledError(const IntentBroker::Ticket& t, const std::string& requester = "wallet_module")
    {
        const json r = broker.result(requester, t.handle, t.receipt);
        if (r.value("state", "") != "settled") return "<" + r.value("state", r.value("error", "?")) + ">";
        return r["result"]["error"].get<std::string>();
    }

    void advance(std::int64_t ms)
    {
        now += ms;
        broker.tick(now);
    }
};

// The person picks `provider` in the chooser on screen.
void choose(World& w, const std::string& provider)
{
    const auto shown = workOfKind(w.takeWork(), "choose");
    w.broker.answer(shown.at(0)["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", provider}}, w.now);
}

// ... and the delivery that follows.
Effect pick(World& w, const std::string& provider)
{
    choose(w, provider);
    return ofKind(w.broker.takeEffects(), K::Deliver).at(0);
}

} // namespace

LOGOS_TEST(an_undeclared_intent_settles_not_declared_at_once)
{
    World w;
    const auto t = w.ask("wallet_module", "share.text");
    LOGOS_ASSERT_EQ(w.settledError(t), err::notDeclared);
    LOGOS_ASSERT_EQ(ofKind(w.broker.takeEffects(), K::Settled).size(), std::size_t(1));
}

LOGOS_TEST(nothing_provides_it_is_unavailable_but_only_after_the_floor)
{
    World w;
    const auto t = w.ask("wallet_module", "nobody.provides", json::object());
    LOGOS_ASSERT_EQ(w.settledError(t), std::string("<pending>"));
    w.advance(399);
    LOGOS_ASSERT_EQ(w.settledError(t), std::string("<pending>"));
    w.advance(1);
    LOGOS_ASSERT_EQ(w.settledError(t), err::unavailable);
}

// Until a presenter claims, nobody can be asked: nothing is shown or delivered.
LOGOS_TEST(nothing_is_delivered_before_a_presenter_claims)
{
    World w(Presenter::None, /*twoProviders=*/true);
    const auto t = w.ask();
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Deliver).empty());
    LOGOS_ASSERT_TRUE(w.takeWork().empty());
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(t), err::unavailable);
}

// The presenter is told to raise the module delivered to, and when it is done;
// it acts only for a module with a screen.
LOGOS_TEST(a_delivery_to_another_module_is_presented_and_completed)
{
    World w;
    const auto t = w.ask();
    const auto d = pick(w, "signer_ui_backend");
    const auto present = workOfKind(w.takeWork(), "present");
    LOGOS_ASSERT_EQ(present.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(present[0]["provider"].get<std::string>(), std::string("signer_ui_backend"));

    LOGOS_ASSERT_TRUE(w.broker.respond("signer_ui_backend", d.dispatchId, true, json("signed"), "", w.now));
    LOGOS_ASSERT_EQ(w.broker.result("wallet_module", t.handle, t.receipt)["result"]["data"].get<std::string>(),
                    std::string("signed"));
    const auto completed = workOfKind(w.takeWork(), "completed");
    LOGOS_ASSERT_EQ(completed.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(completed[0]["by"].get<std::string>(), std::string("provider"));
}

LOGOS_TEST(one_provider_still_asks_the_person)
{
    World w;
    w.ask();
    const json work = w.takeWork();
    const auto choose = workOfKind(work, "choose");
    LOGOS_ASSERT_EQ(choose.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(choose[0]["providers"].size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Deliver).empty());
}

LOGOS_TEST(only_one_choice_is_on_screen_and_the_next_follows)
{
    World w(Presenter::OpensEverything, true);
    const auto first = w.ask();
    w.now += 1;
    w.ask();
    auto choose = workOfKind(w.takeWork(), "choose");
    LOGOS_ASSERT_EQ(choose.size(), std::size_t(1));

    LOGOS_ASSERT_TRUE(w.broker.answer(choose[0]["id"].get<std::string>(), json{{"action", "cancel"}}, w.now));
    LOGOS_ASSERT_EQ(w.settledError(first), err::cancelled);
    choose = workOfKind(w.takeWork(), "choose");
    LOGOS_ASSERT_EQ(choose.size(), std::size_t(1));
}

LOGOS_TEST(a_choice_must_be_one_of_the_providers_shown)
{
    World w(Presenter::OpensEverything, true);
    w.ask();
    const auto choose = workOfKind(w.takeWork(), "choose");
    const std::string id = choose[0]["id"].get<std::string>();
    LOGOS_ASSERT_FALSE(w.broker.answer(id, json{{"action", "choose"}, {"provider", "wallet_module"}}, w.now));
    LOGOS_ASSERT_TRUE(w.broker.answer(id, json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
    LOGOS_ASSERT_EQ(ofKind(w.broker.takeEffects(), K::Deliver).size(), std::size_t(1));
    // Answered once; a second answer is refused.
    LOGOS_ASSERT_FALSE(w.broker.answer(id, json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
}

namespace {

// What set_installed does: the registry rebuilt from these packages alone.
// As set_installed does: the registry rebuilt, then the broker told.
void reinstall(World& w, const std::vector<json>& packages, bool tellBroker = true)
{
    std::vector<LoadedPackage> loaded;
    for (const json& p : packages)
        loaded.push_back(LoadedPackage{p["name"].get<std::string>(), "", "", p, ""});
    w.registry.rebuild(loaded);
    if (tellBroker) w.broker.registryChanged(w.now);
}

json walletPackage()
{
    return pkg("wallet_module", "core", json::array(),
               intentList({"signing.approve", "basecamp.settings.open", "nobody.provides"}));
}

json signerUiPackage()
{
    return pkg("signer_ui_backend", "core", json::array({json{
        {"intent", "signing.approve"},
        {"params", json::array({json{{"name", "handle"}, {"type", "string"}, {"required", true}}})},
    }}));
}

json webProvider(const std::string& name, bool web)
{
    return pkg(name, "core", json::array({json{{"intent", "share.text"}, {"web", web}}}));
}

} // namespace

// A provider re-read with other metadata while the chooser was open isn't the
// one the person picked: they're asked again.
LOGOS_TEST(a_provider_changed_under_the_chooser_is_offered_again)
{
    World w(Presenter::OpensEverything, /*twoProviders=*/true);
    w.ask();
    const auto shown = workOfKind(w.takeWork(), "choose");
    json changed = pkg("cli_signer", "core", intentList({"signing.approve"}));
    changed["version"] = "2.0.0";
    reinstall(w, {walletPackage(), signerUiPackage(), changed});

    LOGOS_ASSERT_TRUE(w.broker.answer(shown[0]["id"].get<std::string>(),
                                      json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Deliver).empty());
    const auto again = workOfKind(w.takeWork(), "choose");
    LOGOS_ASSERT_EQ(again.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(again[0]["dispatch"].get<std::string>(), shown[0]["dispatch"].get<std::string>());

    LOGOS_ASSERT_TRUE(w.broker.answer(again[0]["id"].get<std::string>(),
                                      json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
    LOGOS_ASSERT_EQ(ofKind(w.broker.takeEffects(), K::Deliver).size(), std::size_t(1));
}

LOGOS_TEST(a_queued_chooser_shows_the_providers_of_the_moment_it_comes_up)
{
    World w(Presenter::OpensEverything, /*twoProviders=*/true);
    w.ask();
    w.now += 1;
    w.ask();
    const auto first = workOfKind(w.takeWork(), "choose");
    LOGOS_ASSERT_EQ(first.size(), std::size_t(1));
    reinstall(w, {walletPackage(), signerUiPackage()});   // cli_signer uninstalled

    w.broker.answer(first[0]["id"].get<std::string>(), json{{"action", "cancel"}}, w.now);
    const auto next = workOfKind(w.takeWork(), "choose");
    LOGOS_ASSERT_EQ(next.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(next[0]["providers"].size(), std::size_t(1));
    LOGOS_ASSERT_EQ(next[0]["providers"][0]["module"].get<std::string>(), std::string("signer_ui_backend"));
}

// A link reaches only providers that opted in, at the moment the person picks.
LOGOS_TEST(a_link_request_cannot_go_to_a_provider_that_dropped_web)
{
    World w(Presenter::OpensWhenTold);
    w.registry.registerLinkRequester("basecamp_link", {});
    reinstall(w, {walletPackage(), signerUiPackage(), webProvider("share_a", true), webProvider("share_b", true)});
    w.broker.submit("basecamp_link", "share.text", json::object(), /*fromPresenter=*/true, w.now);
    const auto shown = workOfKind(w.takeWork(), "choose");
    LOGOS_ASSERT_EQ(shown[0]["providers"].size(), std::size_t(2));

    reinstall(w, {walletPackage(), signerUiPackage(), webProvider("share_a", false), webProvider("share_b", true)});
    LOGOS_ASSERT_TRUE(w.broker.answer(shown[0]["id"].get<std::string>(),
                                      json{{"action", "choose"}, {"provider", "share_a"}}, w.now));
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Deliver).empty());
    const auto again = workOfKind(w.takeWork(), "choose");
    LOGOS_ASSERT_EQ(again.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(again[0]["providers"].size(), std::size_t(1));
    LOGOS_ASSERT_EQ(again[0]["providers"][0]["module"].get<std::string>(), std::string("share_b"));
}

// Uninstalled while its chooser is open: the request ends, and the pick goes nowhere.
LOGOS_TEST(a_requester_uninstalled_while_its_chooser_is_open_gets_nothing_delivered)
{
    World w(Presenter::OpensEverything, /*twoProviders=*/true);
    const auto t = w.ask();
    const auto shown = workOfKind(w.takeWork(), "choose");
    LOGOS_ASSERT_EQ(shown.size(), std::size_t(1));

    reinstall(w, {signerUiPackage(), pkg("cli_signer", "core", intentList({"signing.approve"}))});
    LOGOS_ASSERT_EQ(w.settledError(t), err::notDeclared);
    LOGOS_ASSERT_FALSE(w.broker.answer(shown[0]["id"].get<std::string>(),
                                       json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Deliver).empty());
    const auto dismissed = workOfKind(w.takeWork(), "dismiss");
    LOGOS_ASSERT_EQ(dismissed.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(dismissed[0]["work"].get<std::string>(), shown[0]["id"].get<std::string>());
}

// Updated to a version that no longer asks for it, while a provider works on it.
LOGOS_TEST(a_requester_that_stops_declaring_the_intent_loses_its_open_request)
{
    World w;
    const auto t = w.ask();
    const Effect d = pick(w, "signer_ui_backend");
    reinstall(w, {pkg("wallet_module", "core"), signerUiPackage()});
    LOGOS_ASSERT_EQ(w.settledError(t), err::notDeclared);
    LOGOS_ASSERT_FALSE(w.broker.respond("signer_ui_backend", d.dispatchId, true, json::object(), "", w.now));
}

LOGOS_TEST(a_requester_a_new_restriction_leaves_out_loses_its_open_request)
{
    World w(Presenter::OpensEverything, /*twoProviders=*/true);
    const auto t = w.ask();
    workOfKind(w.takeWork(), "choose");
    w.registry.setRestrictions({{"signing.approve", {"other_module"}}});
    w.broker.registryChanged(w.now);
    LOGOS_ASSERT_EQ(w.settledError(t), std::string("<pending>"));   // on the refusal floor
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(t), err::unavailable);
}

// Delivery checks the requester itself, whatever the broker was told.
LOGOS_TEST(nothing_is_delivered_for_a_requester_that_may_no_longer_ask)
{
    World w(Presenter::OpensEverything, /*twoProviders=*/true);
    const auto t = w.ask();
    const auto shown = workOfKind(w.takeWork(), "choose");
    reinstall(w, {signerUiPackage(), pkg("cli_signer", "core", intentList({"signing.approve"}))},
              /*tellBroker=*/false);
    LOGOS_ASSERT_TRUE(w.broker.answer(shown[0]["id"].get<std::string>(),
                                      json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Deliver).empty());
    LOGOS_ASSERT_EQ(w.settledError(t), err::notDeclared);
}

// Every requester's open params together, not only each one's.
LOGOS_TEST(open_params_have_a_budget_across_every_requester)
{
    const json params{{"handle", std::string(1000, 'x')}};
    BrokerConfig config;
    config.maxOpenBytesTotal = approxBytes(params) * 3 / 2;
    World w(Presenter::OpensWhenTold, /*twoProviders=*/false, config);
    w.registry.ingest("other_module", pkg("other_module", "core", json::array(), intentList({"signing.approve"})),
                      "", "");
    const auto first = w.ask("wallet_module", "signing.approve", params);
    const auto second = w.ask("other_module", "signing.approve", params);
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(first), std::string("<pending>"));
    LOGOS_ASSERT_EQ(w.settledError(second, "other_module"), err::failed);
}

LOGOS_TEST(only_the_module_delivered_to_may_respond)
{
    World w(Presenter::OpensEverything, true);
    const auto t = w.ask();
    const auto d = pick(w, "cli_signer");
    LOGOS_ASSERT_FALSE(w.broker.respond("wallet_module", d.dispatchId, true, json("forged"), "", w.now));
    LOGOS_ASSERT_FALSE(w.broker.respond("cli_signer", "not-a-dispatch", true, json(), "", w.now));
    LOGOS_ASSERT_EQ(w.settledError(t), std::string("<pending>"));
}

LOGOS_TEST(a_new_request_waits_until_the_presenter_opens_it)
{
    World w(Presenter::OpensWhenTold);
    const auto t = w.ask();
    const json work = w.takeWork();
    LOGOS_ASSERT_TRUE(workOfKind(work, "choose").empty());
    const auto pending = workOfKind(work, "pending");
    LOGOS_ASSERT_EQ(pending.size(), std::size_t(1));

    LOGOS_ASSERT_TRUE(w.broker.answer(pending[0]["id"].get<std::string>(), json{{"action", "open"}}, w.now));
    LOGOS_ASSERT_EQ(workOfKind(w.takeWork(), "choose").size(), std::size_t(1));
    LOGOS_ASSERT_EQ(w.settledError(t), std::string("<pending>"));
}

LOGOS_TEST(a_dismissed_or_stale_pending_request_is_cancelled)
{
    World w(Presenter::OpensWhenTold);
    const auto a = w.ask();
    const auto pending = workOfKind(w.takeWork(), "pending");
    w.broker.answer(pending[0]["id"].get<std::string>(), json{{"action", "dismiss"}}, w.now);
    LOGOS_ASSERT_EQ(w.settledError(a), err::cancelled);

    const auto b = w.ask();
    w.advance(30 * 60 * 1000 + 1);
    LOGOS_ASSERT_EQ(w.settledError(b), err::cancelled);
}

LOGOS_TEST(the_pending_list_is_bounded_per_requester)
{
    World w(Presenter::OpensWhenTold);
    for (int i = 0; i < 4; ++i) w.ask();
    const auto fifth = w.ask();
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(fifth), err::unavailable);
}

LOGOS_TEST(open_requests_are_bounded_per_requester)
{
    World w;
    for (int i = 0; i < 16; ++i) w.ask();
    const auto over = w.ask();
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(over), err::failed);
}

LOGOS_TEST(params_that_are_not_plain_data_or_miss_the_spec_are_bad_requests)
{
    World w(Presenter::OpensEverything);
    const auto notObject = w.ask("wallet_module", "signing.approve", json::array({1}));
    const auto missing = w.ask("wallet_module", "signing.approve", json{{"other", 1}});
    for (const json& c : workOfKind(w.takeWork(), "choose"))
        w.broker.answer(c["id"].get<std::string>(), json{{"action", "choose"}, {"provider", "signer_ui_backend"}}, w.now);
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(notObject), err::badRequest);
    LOGOS_ASSERT_EQ(w.settledError(missing), err::badRequest);
}

// A provider that declares the shape of its answer is held to it: the
// requester gets `failed` rather than data it can't read.
LOGOS_TEST(an_answer_that_misses_its_declared_result_is_failed)
{
    World w;
    w.registry.ingest("stamp_module", pkg("stamp_module", "core", json::array({json{
        {"intent", "stamp.make"},
        {"result", json::array({json{{"name", "stamp"}, {"type", "string"}, {"required", true}}})},
    }})), "", "");
    w.registry.ingest("asker_module", pkg("asker_module", "core", json::array(), intentList({"stamp.make"})), "", "");
    const auto ask = [&]() {
        return w.broker.submit("asker_module", "stamp.make", json::object(), false, w.now);
    };
    const auto delivered = [&]() { return pick(w, "stamp_module").dispatchId; };

    const auto missing = ask();
    LOGOS_ASSERT_FALSE(w.broker.respond("stamp_module", delivered(), true, json::object(), "", w.now));
    LOGOS_ASSERT_EQ(w.settledError(missing, "asker_module"), err::failed);

    const auto wrongType = ask();
    LOGOS_ASSERT_FALSE(w.broker.respond("stamp_module", delivered(), true, json{{"stamp", 5}}, "", w.now));
    LOGOS_ASSERT_EQ(w.settledError(wrongType, "asker_module"), err::failed);

    const auto notAnObject = ask();
    LOGOS_ASSERT_FALSE(w.broker.respond("stamp_module", delivered(), true, json("ab"), "", w.now));
    LOGOS_ASSERT_EQ(w.settledError(notAnObject, "asker_module"), err::failed);

    // Fields it didn't describe pass: an older description mustn't break a newer provider.
    const auto good = ask();
    LOGOS_ASSERT_TRUE(w.broker.respond("stamp_module", delivered(), true, json{{"stamp", "ab"}, {"at", 1}}, "", w.now));
    LOGOS_ASSERT_EQ(w.settledError(good, "asker_module"), std::string());

    // Only answers are checked: a refusal carries no result.
    const auto refused = ask();
    LOGOS_ASSERT_TRUE(w.broker.respond("stamp_module", delivered(), false, json(), err::cancelled, w.now));
    LOGOS_ASSERT_EQ(w.settledError(refused, "asker_module"), err::cancelled);
}

LOGOS_TEST(a_provider_cannot_send_back_a_broker_only_code_or_a_bad_payload)
{
    World w(Presenter::OpensEverything, true);
    const auto a = w.ask();
    const auto da = pick(w, "cli_signer");
    w.broker.respond("cli_signer", da.dispatchId, false, json(), "unavailable", w.now);
    LOGOS_ASSERT_EQ(w.settledError(a), err::failed);

    const auto b = w.ask();
    const auto db = pick(w, "cli_signer");
    LOGOS_ASSERT_FALSE(w.broker.respond("cli_signer", db.dispatchId, true, json::binary({1}), "", w.now));
    LOGOS_ASSERT_EQ(w.settledError(b), err::failed);
}

LOGOS_TEST(the_result_is_for_the_requester_holding_the_receipt)
{
    World w(Presenter::OpensEverything, true);
    const auto t = w.ask();
    const auto d = pick(w, "cli_signer");
    w.broker.respond("cli_signer", d.dispatchId, true, json("secret-signature"), "", w.now);

    LOGOS_ASSERT_EQ(w.broker.result("other_module", t.handle, t.receipt)["error"].get<std::string>(),
                    std::string("unknown_handle"));
    LOGOS_ASSERT_EQ(w.broker.result("wallet_module", t.handle, "wrong")["error"].get<std::string>(),
                    std::string("unknown_handle"));
    LOGOS_ASSERT_EQ(w.broker.result("wallet_module", "nope", t.receipt)["error"].get<std::string>(),
                    std::string("unknown_handle"));
    LOGOS_ASSERT_TRUE(w.broker.result("wallet_module", t.handle, t.receipt)["ok"].get<bool>());

    // Dropped once acknowledged, and after the retention time otherwise.
    LOGOS_ASSERT_TRUE(w.broker.cancel("wallet_module", t.handle, t.receipt, w.now));
    LOGOS_ASSERT_FALSE(w.broker.result("wallet_module", t.handle, t.receipt)["ok"].get<bool>());
    const auto u = w.ask();
    const auto du = pick(w, "cli_signer");
    w.broker.respond("cli_signer", du.dispatchId, true, json(1), "", w.now);
    w.advance(10 * 60 * 1000 + 1);
    LOGOS_ASSERT_FALSE(w.broker.result("wallet_module", u.handle, u.receipt)["ok"].get<bool>());
}

LOGOS_TEST(a_provider_that_never_takes_the_delivery_times_out)
{
    World w(Presenter::OpensEverything, true);
    const auto t = w.ask();
    const auto d = pick(w, "cli_signer");
    w.broker.deliveryOutcome(d.dispatchId, /*reached=*/true, /*accepted=*/false, w.now);
    w.advance(20001);
    LOGOS_ASSERT_EQ(w.settledError(t), err::timeout);
}

LOGOS_TEST(an_unreachable_provider_is_unavailable)
{
    World w(Presenter::OpensEverything, true);
    const auto t = w.ask();
    const auto d = pick(w, "cli_signer");
    w.broker.deliveryOutcome(d.dispatchId, /*reached=*/false, false, w.now);
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(t), err::unavailable);
}

LOGOS_TEST(the_shell_answers_its_own_intents_through_the_presenter)
{
    World w;
    const auto t = w.ask("wallet_module", "basecamp.settings.open", json::object());
    const auto deliver = workOfKind(w.takeWork(), "deliver");
    LOGOS_ASSERT_EQ(deliver.size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(w.broker.answer(deliver[0]["id"].get<std::string>(),
                                      json{{"action", "respond"}, {"ok", true}, {"data", json()}}, w.now));
    LOGOS_ASSERT_EQ(w.settledError(t), std::string());
}

LOGOS_TEST(the_shells_own_requests_come_back_as_work)
{
    World w(Presenter::OpensWhenTold, /*twoProviders=*/true);
    w.registry.declareShell("basecamp", "basecamp.", {"signing.approve"}, {"basecamp.settings.open"},
                            {}, "Basecamp", "");
    w.broker.submit("basecamp", "signing.approve", json{{"handle", "h"}}, /*fromPresenter=*/true, w.now);
    // The shell's own request is open already: no pending list.
    const auto choose = workOfKind(w.takeWork(), "choose");
    LOGOS_ASSERT_EQ(choose.size(), std::size_t(1));
    w.broker.answer(choose[0]["id"].get<std::string>(), json{{"action", "cancel"}}, w.now);
    const auto results = workOfKind(w.takeWork(), "result");
    LOGOS_ASSERT_EQ(results.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(results[0]["result"]["error"].get<std::string>(), err::cancelled);
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Settled).empty());
}

// The shell's own request is open already, but sending its params to a package
// is still the person's choice, even when only one package provides the intent.
LOGOS_TEST(a_shell_request_with_one_package_provider_still_asks)
{
    World w(Presenter::OpensWhenTold);
    w.registry.declareShell("basecamp", "basecamp.", {"signing.approve"}, {"basecamp.settings.open"},
                            {}, "Basecamp", "");
    w.broker.submit("basecamp", "signing.approve", json{{"handle", "h"}}, /*fromPresenter=*/true, w.now);
    LOGOS_ASSERT_EQ(workOfKind(w.takeWork(), "choose").size(), std::size_t(1));
}

LOGOS_TEST(a_requester_providing_its_own_intent_skips_the_choice)
{
    World w(Presenter::OpensWhenTold, true);
    w.registry.ingest("self_module", pkg("self_module", "core", intentList({"self.thing"}),
                                         intentList({"self.thing"})), "", "");
    w.ask("self_module", "self.thing", json::object());
    LOGOS_ASSERT_TRUE(w.takeWork().empty());
    const auto d = ofKind(w.broker.takeEffects(), K::Deliver);
    LOGOS_ASSERT_EQ(d.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(d[0].module, std::string("self_module"));
}

LOGOS_TEST(work_items_show_display_safe_names)
{
    World w(Presenter::OpensEverything);
    w.registry.ingest("evil_ui", pkg("evil_ui", "core", intentList({"signing.approve"})),
                      "Evil\x1b]0;owned\x07 Signer", "");
    w.ask();
    const auto choose = workOfKind(w.takeWork(), "choose");
    for (const json& p : choose[0]["providers"]) {
        const std::string label = p["displayName"].get<std::string>();
        LOGOS_ASSERT_TRUE(label.find('\x1b') == std::string::npos);
        LOGOS_ASSERT_TRUE(label.find('\x07') == std::string::npos);
    }
}

// ── Hardening ────────────────────────────────────────────────────────────────

LOGOS_TEST(a_request_under_the_shells_name_is_the_shells_only_from_the_presenter)
{
    World w(Presenter::OpensWhenTold);
    w.registry.declareShell("basecamp", "basecamp.", {"signing.approve"}, {"basecamp.settings.open"},
                            {}, "Basecamp", "");
    w.broker.submit("basecamp", "signing.approve", json{{"handle", "h"}}, /*fromPresenter=*/false, w.now);
    LOGOS_ASSERT_EQ(workOfKind(w.takeWork(), "pending").size(), std::size_t(1));
}

// Whether anything provides an intent is decided only once the request is
// opened, so a module can't find out by asking.
LOGOS_TEST(unopened_requests_wait_alike_whatever_is_installed)
{
    World w(Presenter::OpensWhenTold);
    const auto provided = w.ask();
    const auto absent = w.ask("wallet_module", "nobody.provides", json::object());
    w.advance(1000);
    LOGOS_ASSERT_EQ(w.settledError(provided), std::string("<pending>"));
    LOGOS_ASSERT_EQ(w.settledError(absent), std::string("<pending>"));
    LOGOS_ASSERT_EQ(workOfKind(w.takeWork(), "pending").size(), std::size_t(2));
}

LOGOS_TEST(an_install_offer_comes_once_opened_and_once_a_minute)
{
    World w(Presenter::OpensWhenTold);
    for (int i = 0; i < 3; ++i) w.ask("wallet_module", "nobody.provides", json::object());
    json work = w.takeWork();
    LOGOS_ASSERT_TRUE(workOfKind(work, "offer_install").empty());

    const auto pending = workOfKind(work, "pending");
    LOGOS_ASSERT_EQ(pending.size(), std::size_t(3));
    for (const json& p : pending)
        w.broker.answer(p["id"].get<std::string>(), json{{"action", "open"}}, w.now);
    LOGOS_ASSERT_EQ(workOfKind(w.takeWork(), "offer_install").size(), std::size_t(1));

    w.advance(60 * 1000);
    w.ask("wallet_module", "nobody.provides", json::object());
    const auto later = workOfKind(w.takeWork(), "pending");
    w.broker.answer(later.at(0)["id"].get<std::string>(), json{{"action", "open"}}, w.now);
    LOGOS_ASSERT_EQ(workOfKind(w.takeWork(), "offer_install").size(), std::size_t(1));
}

LOGOS_TEST(a_request_withdrawn_before_the_presenter_pulls_leaves_nothing_queued)
{
    World w(Presenter::OpensWhenTold);
    for (int i = 0; i < 50; ++i) {
        const auto t = w.ask();
        w.broker.cancel("wallet_module", t.handle, t.receipt, w.now);
    }
    LOGOS_ASSERT_TRUE(w.takeWork().empty());

    // Once pulled, the presenter is told to take it down.
    const auto t = w.ask();
    LOGOS_ASSERT_EQ(workOfKind(w.takeWork(), "pending").size(), std::size_t(1));
    w.broker.cancel("wallet_module", t.handle, t.receipt, w.now);
    LOGOS_ASSERT_EQ(workOfKind(w.takeWork(), "dismiss").size(), std::size_t(1));
}

// Notices give way when the queue is full; an item someone must answer never does.
LOGOS_TEST(a_flood_of_notices_cannot_push_out_an_item_awaiting_the_person)
{
    World w(Presenter::OpensWhenTold);
    const auto victim = w.ask();
    std::vector<std::string> flood;
    for (int i = 0; i < 1088; ++i) flood.push_back("flood.k" + std::to_string(i));
    w.registry.declareShell("basecamp", "basecamp.", flood, {"basecamp.settings.open"}, {}, "Basecamp", "");
    for (std::size_t i = 0; i < flood.size(); ++i) {
        w.broker.submit("basecamp", flood[i], json::object(), /*fromPresenter=*/true, w.now);
        if (i % 16 == 15) w.advance(400);
    }
    const json work = w.takeWork();
    LOGOS_ASSERT_LE(work.size(), std::size_t(1000));
    const auto pending = workOfKind(work, "pending");
    LOGOS_ASSERT_EQ(pending.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(pending[0]["dispatch"].get<std::string>(), std::string("id3"));
    LOGOS_ASSERT_EQ(w.settledError(victim), std::string("<pending>"));
}

LOGOS_TEST(a_choice_gets_its_full_time_once_shown)
{
    World w(Presenter::OpensEverything, /*twoProviders=*/true);
    w.ask();
    w.now += 1;
    const auto second = w.ask();
    const auto first = workOfKind(w.takeWork(), "choose");
    LOGOS_ASSERT_EQ(first.size(), std::size_t(1));
    w.advance(10 * 60 * 1000 - 1000);   // the person takes 9m59s over the first
    w.broker.answer(first[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now);
    LOGOS_ASSERT_EQ(workOfKind(w.takeWork(), "choose").size(), std::size_t(1));
    w.advance(2000);
    LOGOS_ASSERT_EQ(w.settledError(second), std::string("<pending>"));
}

LOGOS_TEST(a_malformed_presenter_answer_changes_nothing)
{
    World w(Presenter::OpensEverything, /*twoProviders=*/true);
    const auto t = w.ask();
    const std::string id = workOfKind(w.takeWork(), "choose")[0]["id"].get<std::string>();
    LOGOS_ASSERT_FALSE(w.broker.answer(id, json{{"action", "choose"}, {"provider", 5}}, w.now));
    LOGOS_ASSERT_FALSE(w.broker.answer(id, json{{"action", nullptr}}, w.now));
    LOGOS_ASSERT_FALSE(w.broker.answer(id, json::array(), w.now));
    LOGOS_ASSERT_TRUE(w.broker.answer(id, json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
    const auto d = ofKind(w.broker.takeEffects(), K::Deliver);
    LOGOS_ASSERT_EQ(d.size(), std::size_t(1));
    w.broker.respond("cli_signer", d[0].dispatchId, true, json("signed"), "", w.now);
    LOGOS_ASSERT_EQ(w.settledError(t), std::string());
}

LOGOS_TEST(a_shell_answer_reads_a_null_error_as_none)
{
    World w;
    const auto t = w.ask("wallet_module", "basecamp.settings.open", json::object());
    const auto deliver = workOfKind(w.takeWork(), "deliver");
    LOGOS_ASSERT_TRUE(w.broker.answer(deliver[0]["id"].get<std::string>(),
                                      json{{"action", "respond"}, {"ok", true}, {"data", json::object()},
                                           {"error", nullptr}},
                                      w.now));
    LOGOS_ASSERT_EQ(w.settledError(t), std::string());
}

namespace {

// A core module that uses and provides its own intent: it can store results
// without anyone else involved.
IntentBroker::Ticket selfStore(World& w, const std::string& module, const json& data)
{
    const auto t = w.ask(module, "self.store", json::object());
    const auto d = ofKind(w.broker.takeEffects(), K::Deliver);
    w.broker.respond(module, d.at(0).dispatchId, true, data, "", w.now);
    return t;
}

void addSelfStore(World& w, const std::string& module)
{
    w.registry.ingest(module, pkg(module, "core", intentList({"self.store"}), intentList({"self.store"})), "", "");
}

} // namespace

LOGOS_TEST(results_stay_within_each_requesters_byte_budget)
{
    BrokerConfig config;
    config.maxResultBytesPerRequester = 10 * 1024;
    World w(Presenter::OpensEverything, false, config);
    addSelfStore(w, "hog_module");
    std::vector<IntentBroker::Ticket> tickets;
    for (int i = 0; i < 6; ++i) {
        tickets.push_back(selfStore(w, "hog_module", json(std::string(3000, 'a'))));
        w.now += 1;
    }
    LOGOS_ASSERT_LE(w.broker.resultBytes(), std::size_t(10 * 1024));
    LOGOS_ASSERT_EQ(w.settledError(tickets.front(), "hog_module"), std::string("<unknown_handle>"));
    LOGOS_ASSERT_EQ(w.settledError(tickets.back(), "hog_module"), std::string());
}

LOGOS_TEST(results_stay_within_the_global_byte_budget)
{
    BrokerConfig config;
    config.maxResultBytesTotal = 8 * 1024;
    World w(Presenter::OpensEverything, false, config);
    addSelfStore(w, "hog_a");
    addSelfStore(w, "hog_b");
    for (int i = 0; i < 6; ++i) {
        selfStore(w, i % 2 ? "hog_a" : "hog_b", json(std::string(3000, 'a')));
        w.now += 1;
    }
    LOGOS_ASSERT_LE(w.broker.resultBytes(), std::size_t(8 * 1024));
}

LOGOS_TEST(open_requests_stay_within_each_requesters_byte_budget)
{
    BrokerConfig config;
    config.maxOpenBytesPerRequester = 4 * 1024;
    World w(Presenter::OpensEverything, false, config);
    const json big{{"handle", std::string(3000, 'a')}};
    const auto first = w.ask("wallet_module", "signing.approve", big);
    const auto second = w.ask("wallet_module", "signing.approve", big);
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(first), std::string("<pending>"));
    LOGOS_ASSERT_EQ(w.settledError(second), err::failed);
}

// Results the presenter never pulls can't pile up without end, and can't push
// out a request waiting on the person.
LOGOS_TEST(the_work_queue_stays_bounded_when_the_presenter_stops_pulling)
{
    World w(Presenter::OpensWhenTold);
    w.ask();
    for (int i = 0; i < 3000; ++i)
        w.broker.submit("basecamp", "nobody.provides", json::object(), /*fromPresenter=*/true, w.now);
    const json work = w.takeWork();
    LOGOS_ASSERT_LE(work.size(), std::size_t(1000));
    LOGOS_ASSERT_EQ(workOfKind(work, "pending").size(), std::size_t(1));
}

// A refusal waiting out its floor holds no open-request slot.
LOGOS_TEST(the_open_request_limit_holds_while_refusals_wait_out_their_floor)
{
    BrokerConfig config;
    config.maxOpenTotal = 8;
    World w(Presenter::OpensEverything, false, config);
    std::vector<IntentBroker::Ticket> over;
    for (int i = 0; i < 12; ++i) {
        const auto t = w.ask();
        if (i >= 8) over.push_back(t);
        LOGOS_ASSERT_LE(w.broker.openCount(), std::size_t(8));
    }
    LOGOS_ASSERT_EQ(w.settledError(over.front()), std::string("<pending>"));
    w.advance(400);
    for (const auto& t : over) LOGOS_ASSERT_EQ(w.settledError(t), err::failed);

    // Withdrawn like an open request.
    const auto withdrawn = w.ask();
    LOGOS_ASSERT_TRUE(w.broker.cancel("wallet_module", withdrawn.handle, withdrawn.receipt, w.now));
    LOGOS_ASSERT_EQ(w.settledError(withdrawn), err::cancelled);
}

// Held refusals are bounded too: past as many as open requests, they settle at once.
LOGOS_TEST(refusals_waiting_out_their_floor_are_bounded)
{
    BrokerConfig config;
    config.maxOpenTotal = 2;
    World w(Presenter::OpensEverything, false, config);
    w.ask();
    w.ask();
    const auto held = w.ask();
    w.ask();
    const auto atOnce = w.ask();
    LOGOS_ASSERT_EQ(w.settledError(held), std::string("<pending>"));
    LOGOS_ASSERT_EQ(w.settledError(atOnce), err::failed);
    LOGOS_ASSERT_EQ(w.broker.openCount(), std::size_t(2));
}

LOGOS_TEST(requests_beyond_the_rate_wait_for_tokens)
{
    BrokerConfig config;
    config.rateBurst = 3;
    config.ratePerSecond = 1;
    World w(Presenter::OpensEverything, false, config);
    for (int i = 0; i < 3; ++i) LOGOS_ASSERT_TRUE(w.broker.admit("wallet_module", w.now));
    LOGOS_ASSERT_FALSE(w.broker.admit("wallet_module", w.now));
    LOGOS_ASSERT_TRUE(w.broker.admit("other_module", w.now));   // per requester
    w.now += 1000;
    LOGOS_ASSERT_TRUE(w.broker.admit("wallet_module", w.now));
    LOGOS_ASSERT_FALSE(w.broker.admit("wallet_module", w.now));
}

// Every declared type is held, not only strings.
LOGOS_TEST(an_answer_is_checked_against_every_declared_type)
{
    World w;
    const auto field = [](const char* name, const char* type, bool required) {
        return json{{"name", name}, {"type", type}, {"required", required}};
    };
    w.registry.ingest("stamp_module", pkg("stamp_module", "core", json::array({json{
        {"intent", "stamp.make"},
        {"result", json::array({field("count", "number", true), field("final", "bool", true),
                                field("meta", "object", true), field("parts", "array", true),
                                field("note", "string", false), field("at", "timestamp", true)})},
    }})), "", "");
    w.registry.ingest("asker_module", pkg("asker_module", "core", json::array(), intentList({"stamp.make"})), "", "");
    const auto answered = [&](const json& data) {
        const auto t = w.broker.submit("asker_module", "stamp.make", json::object(), false, w.now);
        const auto d = pick(w, "stamp_module").dispatchId;
        w.broker.respond("stamp_module", d, true, data, "", w.now);
        return w.settledError(t, "asker_module");
    };

    // An optional field may be left out; a type this module doesn't know passes.
    const json good{{"count", 1.5}, {"final", false}, {"meta", json::object()},
                    {"parts", json::array()}, {"at", "2026-11-23"}};
    LOGOS_ASSERT_EQ(answered(good), std::string());

    for (const char* name : {"count", "final", "meta", "parts", "note"}) {
        json bad = good;
        bad[name] = name == std::string("note") ? json(5) : json("x");
        LOGOS_ASSERT_EQ(answered(bad), err::failed);
    }
}

LOGOS_TEST(params_of_the_wrong_declared_type_are_bad_requests)
{
    World w;
    w.registry.ingest("stamp_module", pkg("stamp_module", "core", json::array({json{
        {"intent", "stamp.make"},
        {"params", json::array({json{{"name", "count"}, {"type", "number"}, {"required", true}},
                                json{{"name", "note"}, {"type", "string"}, {"required", false}}})},
    }})), "", "");
    w.registry.ingest("asker_module", pkg("asker_module", "core", json::array(), intentList({"stamp.make"})), "", "");
    const auto ask = [&](const json& params) {
        const auto t = w.broker.submit("asker_module", "stamp.make", params, false, w.now);
        choose(w, "stamp_module");
        return t;
    };

    const auto wrongType = ask(json{{"count", "3"}});
    const auto wrongOptional = ask(json{{"count", 3}, {"note", 1}});
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Deliver).empty());
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(wrongType, "asker_module"), err::badRequest);
    LOGOS_ASSERT_EQ(w.settledError(wrongOptional, "asker_module"), err::badRequest);

    ask(json{{"count", 3}});   // the optional one left out
    LOGOS_ASSERT_EQ(ofKind(w.broker.takeEffects(), K::Deliver).size(), std::size_t(1));
}

// The backstop only catches a chooser that went away.
LOGOS_TEST(a_chooser_left_open_past_the_backstop_is_cancelled)
{
    World w(Presenter::OpensEverything, /*twoProviders=*/true);
    const auto t = w.ask();
    const auto shown = workOfKind(w.takeWork(), "choose");
    w.advance(10 * 60 * 1000 + 1);
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(t), err::cancelled);
    LOGOS_ASSERT_FALSE(w.broker.answer(shown[0]["id"].get<std::string>(),
                                       json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
}

LOGOS_TEST(a_queued_chooser_with_no_provider_left_is_unavailable)
{
    World w(Presenter::OpensEverything, /*twoProviders=*/true);
    w.ask();
    w.now += 1;
    const auto second = w.ask();
    const auto first = workOfKind(w.takeWork(), "choose");
    reinstall(w, {walletPackage()});   // every provider uninstalled

    w.broker.answer(first[0]["id"].get<std::string>(), json{{"action", "cancel"}}, w.now);
    LOGOS_ASSERT_TRUE(workOfKind(w.takeWork(), "choose").empty());
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(second), err::unavailable);
}

// An answer is held to what the provider declared when it was given the
// request, not to metadata installed since.
LOGOS_TEST(an_answer_is_checked_against_the_result_declared_at_delivery)
{
    World w;
    const auto stamp = [](const char* field) {
        return pkg("stamp_module", "core", json::array({json{
            {"intent", "stamp.make"},
            {"result", json::array({json{{"name", field}, {"type", "string"}, {"required", true}}})},
        }}));
    };
    const json asker = pkg("asker_module", "core", json::array(), intentList({"stamp.make"}));
    reinstall(w, {walletPackage(), asker, stamp("signature")});
    const auto t = w.broker.submit("asker_module", "stamp.make", json::object(), false, w.now);
    const auto d = pick(w, "stamp_module").dispatchId;

    reinstall(w, {walletPackage(), asker, stamp("approvalId")});
    LOGOS_ASSERT_TRUE(w.broker.respond("stamp_module", d, true, json{{"signature", "ab"}}, "", w.now));
    LOGOS_ASSERT_EQ(w.settledError(t, "asker_module"), std::string());
}
