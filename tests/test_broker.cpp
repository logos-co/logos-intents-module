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

// wallet_module asks for signing.approve; signer_ui (an app) and
// cli_signer (a core module) provide it, signer_ui with declared params.
struct World {
    IntentRegistry registry;
    IntentBroker broker;
    std::int64_t now = 1000;

    explicit World(Attention attention = Attention::Immediate, bool twoProviders = false,
                   BrokerConfig config = {})
        : broker(registry, test::counterIds(), config)
    {
        json signerProvides = json::array({json{
            {"intent", "signing.approve"},
            {"params", json::array({json{{"name", "handle"}, {"type", "string"}, {"required", true}}})},
        }});
        registry.declareShell("basecamp", "basecamp.", {}, {"basecamp.settings.open"}, {}, "Basecamp", "");
        registry.ingest("wallet_module", pkg("wallet_module", "core", json::array(),
                                             intentList({"signing.approve", "basecamp.settings.open",
                                                         "nobody.provides"})), "", "");
        registry.ingest("signer_ui", pkg("signer_ui", "ui_qml", signerProvides), "Signer", "");
        if (twoProviders)
            registry.ingest("cli_signer", pkg("cli_signer", "core", intentList({"signing.approve"})), "", "");
        broker.setAttention(attention);
    }

    IntentBroker::Ticket ask(const std::string& requester = "wallet_module",
                             const std::string& intent = "signing.approve",
                             json params = json{{"handle", "h1"}}, bool clicked = false)
    {
        return broker.submit(requester, intent, params, clicked, /*fromPresenter=*/false, now);
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

LOGOS_TEST(headless_uses_only_a_configured_provider)
{
    World w(Attention::Headless);
    const auto refused = w.ask();
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(refused), err::unavailable);

    // An app needs a person to raise it; headless can't.
    w.broker.configureProvider("signing.approve", "signer_ui");
    const auto stillRefused = w.ask();
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(stillRefused), err::unavailable);
    LOGOS_ASSERT_TRUE(w.broker.takeWork().empty());
}

LOGOS_TEST(headless_delivers_to_a_configured_core_module)
{
    World w(Attention::Headless, /*twoProviders=*/true);
    w.broker.configureProvider("signing.approve", "cli_signer");
    const auto t = w.ask();
    const auto delivers = ofKind(w.broker.takeEffects(), K::Deliver);
    LOGOS_ASSERT_EQ(delivers.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(delivers[0].module, std::string("cli_signer"));
    LOGOS_ASSERT_EQ(delivers[0].requester, std::string("wallet_module"));

    LOGOS_ASSERT_TRUE(w.broker.respond("cli_signer", delivers[0].dispatchId, true, json{{"sig", "ab"}}, "", w.now));
    const json r = w.broker.result("wallet_module", t.handle, t.receipt);
    LOGOS_ASSERT_EQ(r["result"]["data"]["sig"].get<std::string>(), std::string("ab"));
}

LOGOS_TEST(one_provider_still_asks_the_person)
{
    World w;
    w.ask();
    const json work = w.broker.takeWork();
    const auto choose = workOfKind(work, "choose");
    LOGOS_ASSERT_EQ(choose.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(choose[0]["providers"].size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Deliver).empty());
}

LOGOS_TEST(only_one_choice_is_on_screen_and_the_next_follows)
{
    World w(Attention::Immediate, true);
    const auto first = w.ask();
    w.now += 1;
    w.ask();
    auto choose = workOfKind(w.broker.takeWork(), "choose");
    LOGOS_ASSERT_EQ(choose.size(), std::size_t(1));

    LOGOS_ASSERT_TRUE(w.broker.answer(choose[0]["id"].get<std::string>(), json{{"action", "cancel"}}, w.now));
    LOGOS_ASSERT_EQ(w.settledError(first), err::cancelled);
    choose = workOfKind(w.broker.takeWork(), "choose");
    LOGOS_ASSERT_EQ(choose.size(), std::size_t(1));
}

LOGOS_TEST(a_choice_must_be_one_of_the_providers_shown)
{
    World w(Attention::Immediate, true);
    w.ask();
    const auto choose = workOfKind(w.broker.takeWork(), "choose");
    const std::string id = choose[0]["id"].get<std::string>();
    LOGOS_ASSERT_FALSE(w.broker.answer(id, json{{"action", "choose"}, {"provider", "wallet_module"}}, w.now));
    LOGOS_ASSERT_TRUE(w.broker.answer(id, json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
    LOGOS_ASSERT_EQ(ofKind(w.broker.takeEffects(), K::Deliver).size(), std::size_t(1));
    // Answered once; a second answer is refused.
    LOGOS_ASSERT_FALSE(w.broker.answer(id, json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
}

LOGOS_TEST(an_app_provider_is_activated_then_delivered_to_its_backend)
{
    World w;
    const auto t = w.ask();
    const auto choose = workOfKind(w.broker.takeWork(), "choose");
    w.broker.answer(choose[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "signer_ui"}}, w.now);
    const json work = w.broker.takeWork();
    LOGOS_ASSERT_EQ(workOfKind(work, "activate").size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Deliver).empty());

    LOGOS_ASSERT_TRUE(w.broker.registerBackend("signer_ui", "signer_ui_backend", w.now));
    const auto delivers = ofKind(w.broker.takeEffects(), K::Deliver);
    LOGOS_ASSERT_EQ(delivers.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(delivers[0].module, std::string("signer_ui_backend"));
    LOGOS_ASSERT_EQ(workOfKind(w.broker.takeWork(), "present").size(), std::size_t(1));

    LOGOS_ASSERT_TRUE(w.broker.respond("signer_ui_backend", delivers[0].dispatchId, true, json("signed"), "", w.now));
    LOGOS_ASSERT_EQ(w.broker.result("wallet_module", t.handle, t.receipt)["result"]["data"].get<std::string>(),
                    std::string("signed"));
    // The frontend is told, and decides whether to take the person back.
    const auto completed = workOfKind(w.broker.takeWork(), "completed");
    LOGOS_ASSERT_EQ(completed.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(completed[0]["by"].get<std::string>(), std::string("provider"));
}

namespace {

// What set_installed does: the registry rebuilt from these packages alone.
void reinstall(World& w, const std::vector<json>& packages)
{
    std::vector<LoadedPackage> loaded;
    for (const json& p : packages)
        loaded.push_back(LoadedPackage{p["name"].get<std::string>(), "", "", p, ""});
    w.registry.rebuild(loaded);
}

json walletPackage()
{
    return pkg("wallet_module", "core", json::array(),
               intentList({"signing.approve", "basecamp.settings.open", "nobody.provides"}));
}

json signerUiPackage()
{
    return pkg("signer_ui", "ui_qml", json::array({json{
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
    World w(Attention::Immediate, /*twoProviders=*/true);
    w.ask();
    const auto shown = workOfKind(w.broker.takeWork(), "choose");
    json changed = pkg("cli_signer", "core", intentList({"signing.approve"}));
    changed["version"] = "2.0.0";
    reinstall(w, {walletPackage(), signerUiPackage(), changed});

    LOGOS_ASSERT_TRUE(w.broker.answer(shown[0]["id"].get<std::string>(),
                                      json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Deliver).empty());
    const auto again = workOfKind(w.broker.takeWork(), "choose");
    LOGOS_ASSERT_EQ(again.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(again[0]["dispatch"].get<std::string>(), shown[0]["dispatch"].get<std::string>());

    LOGOS_ASSERT_TRUE(w.broker.answer(again[0]["id"].get<std::string>(),
                                      json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now));
    LOGOS_ASSERT_EQ(ofKind(w.broker.takeEffects(), K::Deliver).size(), std::size_t(1));
}

LOGOS_TEST(a_queued_chooser_shows_the_providers_of_the_moment_it_comes_up)
{
    World w(Attention::Immediate, /*twoProviders=*/true);
    w.ask();
    w.now += 1;
    w.ask();
    const auto first = workOfKind(w.broker.takeWork(), "choose");
    LOGOS_ASSERT_EQ(first.size(), std::size_t(1));
    reinstall(w, {walletPackage(), signerUiPackage()});   // cli_signer uninstalled

    w.broker.answer(first[0]["id"].get<std::string>(), json{{"action", "cancel"}}, w.now);
    const auto next = workOfKind(w.broker.takeWork(), "choose");
    LOGOS_ASSERT_EQ(next.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(next[0]["providers"].size(), std::size_t(1));
    LOGOS_ASSERT_EQ(next[0]["providers"][0]["module"].get<std::string>(), std::string("signer_ui"));
}

// A link reaches only providers that opted in, at the moment the person picks.
LOGOS_TEST(a_link_request_cannot_go_to_a_provider_that_dropped_web)
{
    World w(Attention::Interactive);
    w.registry.registerLinkRequester("basecamp_link", {});
    reinstall(w, {walletPackage(), signerUiPackage(), webProvider("share_a", true), webProvider("share_b", true)});
    w.broker.submit("basecamp_link", "share.text", json::object(), false, /*fromPresenter=*/true, w.now);
    const auto shown = workOfKind(w.broker.takeWork(), "choose");
    LOGOS_ASSERT_EQ(shown[0]["providers"].size(), std::size_t(2));

    reinstall(w, {walletPackage(), signerUiPackage(), webProvider("share_a", false), webProvider("share_b", true)});
    LOGOS_ASSERT_TRUE(w.broker.answer(shown[0]["id"].get<std::string>(),
                                      json{{"action", "choose"}, {"provider", "share_a"}}, w.now));
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Deliver).empty());
    const auto again = workOfKind(w.broker.takeWork(), "choose");
    LOGOS_ASSERT_EQ(again.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(again[0]["providers"].size(), std::size_t(1));
    LOGOS_ASSERT_EQ(again[0]["providers"][0]["module"].get<std::string>(), std::string("share_b"));
}

// Nothing may raise an app once no request waits on it.
LOGOS_TEST(an_activation_nobody_waits_for_is_taken_back)
{
    World w;
    const auto unpulled = w.ask();
    w.broker.answer(workOfKind(w.broker.takeWork(), "choose")[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "signer_ui"}}, w.now);
    w.broker.cancel("wallet_module", unpulled.handle, unpulled.receipt, w.now);
    LOGOS_ASSERT_TRUE(workOfKind(w.broker.takeWork(), "activate").empty());

    // Pulled: the presenter is told to take it down, and it can't be answered.
    const auto pulled = w.ask();
    w.broker.answer(workOfKind(w.broker.takeWork(), "choose")[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "signer_ui"}}, w.now);
    const auto activate = workOfKind(w.broker.takeWork(), "activate");
    LOGOS_ASSERT_EQ(activate.size(), std::size_t(1));
    w.broker.cancel("wallet_module", pulled.handle, pulled.receipt, w.now);
    const auto dismiss = workOfKind(w.broker.takeWork(), "dismiss");
    LOGOS_ASSERT_EQ(dismiss.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(dismiss[0]["work"].get<std::string>(), activate[0]["id"].get<std::string>());
    LOGOS_ASSERT_FALSE(w.broker.answer(activate[0]["id"].get<std::string>(), json{{"action", "failed"}}, w.now));
}

LOGOS_TEST(an_activation_stays_while_a_request_still_waits_on_it)
{
    World w;
    const auto first = w.ask();
    w.broker.answer(workOfKind(w.broker.takeWork(), "choose")[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "signer_ui"}}, w.now);
    w.now += 1;
    const auto second = w.ask();
    const json work = w.broker.takeWork();
    const auto activate = workOfKind(work, "activate");
    LOGOS_ASSERT_EQ(activate.size(), std::size_t(1));
    w.broker.answer(workOfKind(work, "choose")[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "signer_ui"}}, w.now);
    LOGOS_ASSERT_TRUE(workOfKind(w.broker.takeWork(), "activate").empty());   // asked once

    w.broker.cancel("wallet_module", first.handle, first.receipt, w.now);
    LOGOS_ASSERT_TRUE(workOfKind(w.broker.takeWork(), "dismiss").empty());
    w.broker.cancel("wallet_module", second.handle, second.receipt, w.now);
    const auto dismiss = workOfKind(w.broker.takeWork(), "dismiss");
    LOGOS_ASSERT_EQ(dismiss.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(dismiss[0]["work"].get<std::string>(), activate[0]["id"].get<std::string>());
}

LOGOS_TEST(a_backend_that_comes_up_first_leaves_no_activation_behind)
{
    World w;
    w.ask();
    w.broker.answer(workOfKind(w.broker.takeWork(), "choose")[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "signer_ui"}}, w.now);
    LOGOS_ASSERT_TRUE(w.broker.registerBackend("signer_ui", "signer_ui_backend", w.now));
    LOGOS_ASSERT_EQ(ofKind(w.broker.takeEffects(), K::Deliver).size(), std::size_t(1));
    const json work = w.broker.takeWork();
    LOGOS_ASSERT_TRUE(workOfKind(work, "activate").empty());
    LOGOS_ASSERT_EQ(workOfKind(work, "present").size(), std::size_t(1));
}

LOGOS_TEST(only_the_module_delivered_to_may_respond)
{
    World w(Attention::Headless, true);
    w.broker.configureProvider("signing.approve", "cli_signer");
    const auto t = w.ask();
    const auto d = ofKind(w.broker.takeEffects(), K::Deliver)[0];
    LOGOS_ASSERT_FALSE(w.broker.respond("wallet_module", d.dispatchId, true, json("forged"), "", w.now));
    LOGOS_ASSERT_FALSE(w.broker.respond("cli_signer", "not-a-dispatch", true, json(), "", w.now));
    LOGOS_ASSERT_EQ(w.settledError(t), std::string("<pending>"));
}

LOGOS_TEST(a_reloaded_backend_cannot_answer_its_predecessors_request)
{
    World w;
    w.broker.registerBackend("signer_ui", "signer_ui_backend", w.now);
    const auto t = w.ask();
    const auto choose = workOfKind(w.broker.takeWork(), "choose");
    w.broker.answer(choose[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "signer_ui"}}, w.now);
    const auto d = ofKind(w.broker.takeEffects(), K::Deliver)[0];

    w.broker.registerBackend("signer_ui", "signer_ui_backend", w.now);   // reload
    LOGOS_ASSERT_FALSE(w.broker.respond("signer_ui_backend", d.dispatchId, true, json(), "", w.now));
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(t), err::unavailable);
}

// The same run of a backend registered again (a second view of the app)
// changes nothing; another run starts afresh.
LOGOS_TEST(registering_the_same_run_of_a_backend_again_changes_nothing)
{
    World w;
    w.broker.registerBackend("signer_ui", "signer_ui_backend", w.now, "run1");
    const auto t = w.ask();
    w.broker.answer(workOfKind(w.broker.takeWork(), "choose")[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "signer_ui"}}, w.now);
    const auto d = ofKind(w.broker.takeEffects(), K::Deliver)[0];
    LOGOS_ASSERT_TRUE(w.broker.registerBackend("signer_ui", "signer_ui_backend", w.now, "run1"));
    LOGOS_ASSERT_TRUE(w.broker.respond("signer_ui_backend", d.dispatchId, true, json("signed"), "", w.now));
    LOGOS_ASSERT_EQ(w.settledError(t), std::string());

    const auto u = w.ask();
    w.broker.answer(workOfKind(w.broker.takeWork(), "choose")[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "signer_ui"}}, w.now);
    const auto e = ofKind(w.broker.takeEffects(), K::Deliver)[0];
    LOGOS_ASSERT_TRUE(w.broker.registerBackend("signer_ui", "signer_ui_backend", w.now, "run2"));
    LOGOS_ASSERT_FALSE(w.broker.respond("signer_ui_backend", e.dispatchId, true, json(), "", w.now));
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(u), err::unavailable);
}

LOGOS_TEST(unregistering_a_backend_fails_what_it_was_given)
{
    World w;
    w.broker.registerBackend("signer_ui", "signer_ui_backend", w.now);
    const auto t = w.ask();
    const auto choose = workOfKind(w.broker.takeWork(), "choose");
    w.broker.answer(choose[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "signer_ui"}}, w.now);
    const auto d = ofKind(w.broker.takeEffects(), K::Deliver)[0];
    w.broker.unregisterBackend("signer_ui", w.now);
    LOGOS_ASSERT_FALSE(w.broker.respond("signer_ui_backend", d.dispatchId, true, json(), "", w.now));
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(t), err::unavailable);
}

LOGOS_TEST(a_backend_name_that_belongs_to_a_package_is_refused)
{
    World w(Attention::Immediate, true);
    LOGOS_ASSERT_FALSE(w.broker.registerBackend("signer_ui", "cli_signer", w.now));
    LOGOS_ASSERT_FALSE(w.broker.registerBackend("signer_ui", "basecamp", w.now));
    LOGOS_ASSERT_FALSE(w.broker.registerBackend("wallet_module", "x_backend", w.now));   // not an app
    LOGOS_ASSERT_TRUE(w.broker.registerBackend("signer_ui", "signer_ui_backend", w.now));
    w.registry.ingest("other_ui", pkg("other_ui", "ui_qml"), "", "");
    LOGOS_ASSERT_FALSE(w.broker.registerBackend("other_ui", "signer_ui_backend", w.now));   // serves another app
}

LOGOS_TEST(interactive_without_a_click_waits_in_the_pending_list)
{
    World w(Attention::Interactive);
    const auto t = w.ask();
    const json work = w.broker.takeWork();
    LOGOS_ASSERT_TRUE(workOfKind(work, "choose").empty());
    const auto pending = workOfKind(work, "pending");
    LOGOS_ASSERT_EQ(pending.size(), std::size_t(1));

    // Opening it is the click.
    LOGOS_ASSERT_TRUE(w.broker.answer(pending[0]["id"].get<std::string>(), json{{"action", "open"}}, w.now));
    LOGOS_ASSERT_EQ(workOfKind(w.broker.takeWork(), "choose").size(), std::size_t(1));
    LOGOS_ASSERT_EQ(w.settledError(t), std::string("<pending>"));
}

LOGOS_TEST(interactive_with_a_click_pops_up_at_once)
{
    World w(Attention::Interactive);
    w.ask("wallet_module", "signing.approve", json{{"handle", "h"}}, /*clicked=*/true);
    LOGOS_ASSERT_EQ(workOfKind(w.broker.takeWork(), "choose").size(), std::size_t(1));
}

LOGOS_TEST(a_dismissed_or_stale_pending_request_is_cancelled)
{
    World w(Attention::Interactive);
    const auto a = w.ask();
    const auto pending = workOfKind(w.broker.takeWork(), "pending");
    w.broker.answer(pending[0]["id"].get<std::string>(), json{{"action", "dismiss"}}, w.now);
    LOGOS_ASSERT_EQ(w.settledError(a), err::cancelled);

    const auto b = w.ask();
    w.advance(30 * 60 * 1000 + 1);
    LOGOS_ASSERT_EQ(w.settledError(b), err::cancelled);
}

LOGOS_TEST(the_pending_list_is_bounded_per_requester)
{
    World w(Attention::Interactive);
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
    World w(Attention::Immediate);
    const auto notObject = w.ask("wallet_module", "signing.approve", json::array({1}));
    w.broker.registerBackend("signer_ui", "signer_ui_backend", w.now);
    const auto missing = w.ask("wallet_module", "signing.approve", json{{"other", 1}});
    for (const json& c : workOfKind(w.broker.takeWork(), "choose"))
        w.broker.answer(c["id"].get<std::string>(), json{{"action", "choose"}, {"provider", "signer_ui"}}, w.now);
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(notObject), err::badRequest);
    LOGOS_ASSERT_EQ(w.settledError(missing), err::badRequest);
}

// A provider that declares the shape of its answer is held to it: the
// requester gets `failed` rather than data it can't read.
LOGOS_TEST(an_answer_that_misses_its_declared_result_is_failed)
{
    World w(Attention::Headless);
    w.registry.ingest("stamp_module", pkg("stamp_module", "core", json::array({json{
        {"intent", "stamp.make"},
        {"result", json::array({json{{"name", "stamp"}, {"type", "string"}, {"required", true}}})},
    }})), "", "");
    w.registry.ingest("asker_module", pkg("asker_module", "core", json::array(), intentList({"stamp.make"})), "", "");
    w.broker.configureProvider("stamp.make", "stamp_module");
    const auto ask = [&]() {
        return w.broker.submit("asker_module", "stamp.make", json::object(), false, false, w.now);
    };
    const auto delivered = [&]() { return ofKind(w.broker.takeEffects(), K::Deliver).at(0).dispatchId; };

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
    World w(Attention::Headless, true);
    w.broker.configureProvider("signing.approve", "cli_signer");
    const auto a = w.ask();
    const auto da = ofKind(w.broker.takeEffects(), K::Deliver)[0];
    w.broker.respond("cli_signer", da.dispatchId, false, json(), "unavailable", w.now);
    LOGOS_ASSERT_EQ(w.settledError(a), err::failed);

    const auto b = w.ask();
    const auto db = ofKind(w.broker.takeEffects(), K::Deliver)[0];
    LOGOS_ASSERT_FALSE(w.broker.respond("cli_signer", db.dispatchId, true, json::binary({1}), "", w.now));
    LOGOS_ASSERT_EQ(w.settledError(b), err::failed);
}

LOGOS_TEST(the_result_is_for_the_requester_holding_the_receipt)
{
    World w(Attention::Headless, true);
    w.broker.configureProvider("signing.approve", "cli_signer");
    const auto t = w.ask();
    const auto d = ofKind(w.broker.takeEffects(), K::Deliver)[0];
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
    const auto du = ofKind(w.broker.takeEffects(), K::Deliver)[0];
    w.broker.respond("cli_signer", du.dispatchId, true, json(1), "", w.now);
    w.advance(10 * 60 * 1000 + 1);
    LOGOS_ASSERT_FALSE(w.broker.result("wallet_module", u.handle, u.receipt)["ok"].get<bool>());
}

LOGOS_TEST(a_provider_that_never_takes_the_delivery_times_out)
{
    World w(Attention::Headless, true);
    w.broker.configureProvider("signing.approve", "cli_signer");
    const auto t = w.ask();
    const auto d = ofKind(w.broker.takeEffects(), K::Deliver)[0];
    w.broker.deliveryOutcome(d.dispatchId, /*reached=*/true, /*accepted=*/false, w.now);
    w.advance(20001);
    LOGOS_ASSERT_EQ(w.settledError(t), err::timeout);
}

LOGOS_TEST(an_unreachable_provider_is_unavailable)
{
    World w(Attention::Headless, true);
    w.broker.configureProvider("signing.approve", "cli_signer");
    const auto t = w.ask();
    const auto d = ofKind(w.broker.takeEffects(), K::Deliver)[0];
    w.broker.deliveryOutcome(d.dispatchId, /*reached=*/false, false, w.now);
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(t), err::unavailable);
}

LOGOS_TEST(the_shell_answers_its_own_intents_through_the_presenter)
{
    World w(Attention::Interactive);
    const auto t = w.ask("wallet_module", "basecamp.settings.open", json::object(), /*clicked=*/true);
    const auto deliver = workOfKind(w.broker.takeWork(), "deliver");
    LOGOS_ASSERT_EQ(deliver.size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(w.broker.answer(deliver[0]["id"].get<std::string>(),
                                      json{{"action", "respond"}, {"ok", true}, {"data", json()}}, w.now));
    LOGOS_ASSERT_EQ(w.settledError(t), std::string());
}

LOGOS_TEST(the_shells_own_requests_come_back_as_work)
{
    World w(Attention::Interactive, /*twoProviders=*/true);
    w.registry.declareShell("basecamp", "basecamp.", {"signing.approve"}, {"basecamp.settings.open"},
                            {}, "Basecamp", "");
    w.broker.submit("basecamp", "signing.approve", json{{"handle", "h"}}, false, /*fromPresenter=*/true, w.now);
    // A shell request is a click in the shell: no pending list.
    const auto choose = workOfKind(w.broker.takeWork(), "choose");
    LOGOS_ASSERT_EQ(choose.size(), std::size_t(1));
    w.broker.answer(choose[0]["id"].get<std::string>(), json{{"action", "cancel"}}, w.now);
    const auto results = workOfKind(w.broker.takeWork(), "result");
    LOGOS_ASSERT_EQ(results.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(results[0]["result"]["error"].get<std::string>(), err::cancelled);
    LOGOS_ASSERT_TRUE(ofKind(w.broker.takeEffects(), K::Settled).empty());
}

// The shell's own request is clicked, but sending its params to a package is
// still the person's choice, even when only one package provides the intent.
LOGOS_TEST(a_shell_request_with_one_package_provider_still_asks)
{
    World w(Attention::Interactive);
    w.registry.declareShell("basecamp", "basecamp.", {"signing.approve"}, {"basecamp.settings.open"},
                            {}, "Basecamp", "");
    w.broker.submit("basecamp", "signing.approve", json{{"handle", "h"}}, false, /*fromPresenter=*/true, w.now);
    const json work = w.broker.takeWork();
    LOGOS_ASSERT_EQ(workOfKind(work, "choose").size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(workOfKind(work, "activate").empty());
}

LOGOS_TEST(a_requester_providing_its_own_intent_skips_the_choice)
{
    World w(Attention::Interactive, true);
    w.registry.ingest("self_module", pkg("self_module", "core", intentList({"self.thing"}),
                                         intentList({"self.thing"})), "", "");
    w.ask("self_module", "self.thing", json::object());
    LOGOS_ASSERT_TRUE(w.broker.takeWork().empty());
    const auto d = ofKind(w.broker.takeEffects(), K::Deliver);
    LOGOS_ASSERT_EQ(d.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(d[0].module, std::string("self_module"));
}

LOGOS_TEST(work_items_show_display_safe_names)
{
    World w(Attention::Immediate);
    w.registry.ingest("evil_ui", pkg("evil_ui", "ui_qml", intentList({"signing.approve"})),
                      "Evil\x1b]0;owned\x07 Signer", "");
    w.ask();
    const auto choose = workOfKind(w.broker.takeWork(), "choose");
    for (const json& p : choose[0]["providers"]) {
        const std::string label = p["displayName"].get<std::string>();
        LOGOS_ASSERT_TRUE(label.find('\x1b') == std::string::npos);
        LOGOS_ASSERT_TRUE(label.find('\x07') == std::string::npos);
    }
}

// ── Hardening ────────────────────────────────────────────────────────────────

LOGOS_TEST(a_request_under_the_shells_name_is_the_shells_only_from_the_presenter)
{
    World w(Attention::Interactive);
    w.registry.declareShell("basecamp", "basecamp.", {"signing.approve"}, {"basecamp.settings.open"},
                            {}, "Basecamp", "");
    w.broker.submit("basecamp", "signing.approve", json{{"handle", "h"}}, false, /*fromPresenter=*/false, w.now);
    const json work = w.broker.takeWork();
    LOGOS_ASSERT_TRUE(workOfKind(work, "activate").empty());
    LOGOS_ASSERT_EQ(workOfKind(work, "pending").size(), std::size_t(1));
}

// Whether anything provides an intent is decided only once the person opens
// the request, so an app can't find out by asking without a click.
LOGOS_TEST(unclicked_requests_wait_alike_whatever_is_installed)
{
    World w(Attention::Interactive);
    const auto provided = w.ask();
    const auto absent = w.ask("wallet_module", "nobody.provides", json::object());
    w.advance(1000);
    LOGOS_ASSERT_EQ(w.settledError(provided), std::string("<pending>"));
    LOGOS_ASSERT_EQ(w.settledError(absent), std::string("<pending>"));
    LOGOS_ASSERT_EQ(workOfKind(w.broker.takeWork(), "pending").size(), std::size_t(2));
}

LOGOS_TEST(an_install_offer_needs_a_click_and_comes_once_a_minute)
{
    World w(Attention::Interactive);
    w.registry.setInstallable({{"sharer_ui", {"nobody.provides"}}});
    for (int i = 0; i < 3; ++i) w.ask("wallet_module", "nobody.provides", json::object());
    json work = w.broker.takeWork();
    LOGOS_ASSERT_TRUE(workOfKind(work, "offer_install").empty());

    const auto pending = workOfKind(work, "pending");
    LOGOS_ASSERT_EQ(pending.size(), std::size_t(3));
    for (const json& p : pending)
        w.broker.answer(p["id"].get<std::string>(), json{{"action", "open"}}, w.now);
    LOGOS_ASSERT_EQ(workOfKind(w.broker.takeWork(), "offer_install").size(), std::size_t(1));

    w.advance(60 * 1000);
    w.ask("wallet_module", "nobody.provides", json::object(), /*clicked=*/true);
    LOGOS_ASSERT_EQ(workOfKind(w.broker.takeWork(), "offer_install").size(), std::size_t(1));
}

LOGOS_TEST(a_request_withdrawn_before_the_presenter_pulls_leaves_nothing_queued)
{
    World w(Attention::Interactive);
    for (int i = 0; i < 50; ++i) {
        const auto t = w.ask();
        w.broker.cancel("wallet_module", t.handle, t.receipt, w.now);
    }
    LOGOS_ASSERT_TRUE(w.broker.takeWork().empty());

    // Once pulled, the presenter is told to take it down.
    const auto t = w.ask();
    LOGOS_ASSERT_EQ(workOfKind(w.broker.takeWork(), "pending").size(), std::size_t(1));
    w.broker.cancel("wallet_module", t.handle, t.receipt, w.now);
    LOGOS_ASSERT_EQ(workOfKind(w.broker.takeWork(), "dismiss").size(), std::size_t(1));
}

// Notices give way when the queue is full; an item someone must answer never does.
LOGOS_TEST(a_flood_of_notices_cannot_push_out_a_chooser)
{
    World w(Attention::Immediate);
    const auto victim = w.ask();
    std::map<std::string, std::vector<std::string>> installable;
    for (int m = 0; m < 17; ++m) {
        const std::string requester = "spam" + std::to_string(m);
        json uses = json::array();
        for (int i = 0; i < 64; ++i) {
            const std::string intent = "flood.k" + std::to_string(m * 64 + i);
            uses.push_back(json{{"intent", intent}});
            installable["store" + std::to_string(m)].push_back(intent);
        }
        w.registry.ingest(requester, pkg(requester, "core", json::array(), uses), "", "");
    }
    w.registry.setInstallable(installable);
    for (int m = 0; m < 17; ++m) {
        for (int i = 0; i < 64; ++i) {
            w.ask("spam" + std::to_string(m), "flood.k" + std::to_string(m * 64 + i), json::object());
            if (i % 16 == 15) w.advance(400);
        }
    }
    const json work = w.broker.takeWork();
    LOGOS_ASSERT_LE(work.size(), std::size_t(1000));
    const auto choose = workOfKind(work, "choose");
    LOGOS_ASSERT_EQ(choose.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(choose[0]["dispatch"].get<std::string>(), std::string("id3"));
    LOGOS_ASSERT_EQ(w.settledError(victim), std::string("<pending>"));
}

LOGOS_TEST(a_choice_gets_its_full_time_once_shown)
{
    World w(Attention::Immediate, /*twoProviders=*/true);
    w.ask();
    w.now += 1;
    const auto second = w.ask();
    const auto first = workOfKind(w.broker.takeWork(), "choose");
    LOGOS_ASSERT_EQ(first.size(), std::size_t(1));
    w.advance(10 * 60 * 1000 - 1000);   // the person takes 9m59s over the first
    w.broker.answer(first[0]["id"].get<std::string>(),
                    json{{"action", "choose"}, {"provider", "cli_signer"}}, w.now);
    LOGOS_ASSERT_EQ(workOfKind(w.broker.takeWork(), "choose").size(), std::size_t(1));
    w.advance(2000);
    LOGOS_ASSERT_EQ(w.settledError(second), std::string("<pending>"));
}

LOGOS_TEST(a_malformed_presenter_answer_changes_nothing)
{
    World w(Attention::Immediate, /*twoProviders=*/true);
    const auto t = w.ask();
    const std::string id = workOfKind(w.broker.takeWork(), "choose")[0]["id"].get<std::string>();
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
    World w(Attention::Interactive);
    const auto t = w.ask("wallet_module", "basecamp.settings.open", json::object(), /*clicked=*/true);
    const auto deliver = workOfKind(w.broker.takeWork(), "deliver");
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
    World w(Attention::Immediate, false, config);
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
    World w(Attention::Immediate, false, config);
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
    World w(Attention::Immediate, false, config);
    const json big{{"handle", std::string(3000, 'a')}};
    const auto first = w.ask("wallet_module", "signing.approve", big);
    const auto second = w.ask("wallet_module", "signing.approve", big);
    w.advance(400);
    LOGOS_ASSERT_EQ(w.settledError(first), std::string("<pending>"));
    LOGOS_ASSERT_EQ(w.settledError(second), err::failed);
}

// Results the presenter never pulls can't pile up without end, and can't push
// out a chooser waiting on the person.
LOGOS_TEST(the_work_queue_stays_bounded_when_the_presenter_stops_pulling)
{
    World w(Attention::Immediate);
    w.ask();
    for (int i = 0; i < 3000; ++i)
        w.broker.submit("basecamp", "nobody.provides", json::object(), true, /*fromPresenter=*/true, w.now);
    const json work = w.broker.takeWork();
    LOGOS_ASSERT_LE(work.size(), std::size_t(1000));
    LOGOS_ASSERT_EQ(workOfKind(work, "choose").size(), std::size_t(1));
}

// A refusal waiting out its floor holds no open-request slot.
LOGOS_TEST(the_open_request_limit_holds_while_refusals_wait_out_their_floor)
{
    BrokerConfig config;
    config.maxOpenTotal = 8;
    World w(Attention::Immediate, false, config);
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
    World w(Attention::Immediate, false, config);
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
    World w(Attention::Immediate, false, config);
    for (int i = 0; i < 3; ++i) LOGOS_ASSERT_TRUE(w.broker.admit("wallet_module", w.now));
    LOGOS_ASSERT_FALSE(w.broker.admit("wallet_module", w.now));
    LOGOS_ASSERT_TRUE(w.broker.admit("other_module", w.now));   // per requester
    w.now += 1000;
    LOGOS_ASSERT_TRUE(w.broker.admit("wallet_module", w.now));
    LOGOS_ASSERT_FALSE(w.broker.admit("wallet_module", w.now));
}
