// The module's security rules, driven through IntentsService with the caller
// identity the host would push (logos::CallCaller).

#include <logos_test.h>

#include "intent_rules.h"
#include "intents_service.h"
#include "test_support.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

using namespace intents;
using logos::CallCaller;
using test::intentList;
using test::pkg;
using test::workOfKind;

namespace {

struct Delivery {
    std::string module;
    json args;
    std::function<void(bool, const json&)> done;
};

struct Harness {
    test::TempPackages dir;
    std::int64_t now = 1000;
    std::vector<Delivery> deliveries;
    std::vector<std::string> settled;
    std::unique_ptr<IntentsService> service;
    std::string secret;

    explicit Harness(const std::string& attention = "immediate", bool claim = true)
    {
        IntentsService::Outbound out;
        out.deliver = [this](const std::string& module, const json& args,
                             std::function<void(bool, const json&)> done) {
            deliveries.push_back({module, args, std::move(done)});
        };
        out.settled = [this](const std::string& h) { settled.push_back(h); };
        out.workAvailable = [](std::uint64_t) {};
        service = std::make_unique<IntentsService>(std::move(out), [this]() { return now; },
                                                   test::counterIds());
        if (!claim) return;
        const CallCaller host = CallCaller::host();
        secret = json::parse(service->claim(json{{"attention", attention}}.dump()))
                     .value("secret", std::string());
        install();
    }

    void install()
    {
        const CallCaller host = CallCaller::host();
        json packages = json::array({
            json{{"name", "wallet_ui"}, {"installDir", dir.add("wallet_ui", pkg("wallet_ui", "ui_qml", json::array(),
                                                                               intentList({"signing.approve"})))}},
            json{{"name", "signer_ui"}, {"installDir", dir.add("signer_ui", pkg("signer_ui", "ui_qml",
                                                                               intentList({"signing.approve"})))}},
            json{{"name", "cli_signer"}, {"installDir", dir.add("cli_signer", pkg("cli_signer", "core",
                                                                                 intentList({"signing.approve"})))}},
            json{{"name", "chain_module"}, {"installDir", dir.add("chain_module", pkg("chain_module", "core", json::array(),
                                                                                     intentList({"signing.approve"})))}},
        });
        service->setInstalled(secret, packages.dump());
        service->backendRegistered(secret, "wallet_ui", "wallet_ui_backend");
    }

    json call(const std::string& out) { return json::parse(out); }

    json work()
    {
        const CallCaller host = CallCaller::host();
        return call(service->nextWork(secret)).value("items", json::array());
    }

    std::string clickToken(const std::string& app, const std::string& intent = "")
    {
        const CallCaller host = CallCaller::host();
        const std::string token = "tok-" + app + "-" + std::to_string(now) + "-0123456789";
        LOGOS_ASSERT_TRUE(call(service->registerClickToken(secret, token, app, intent))["ok"].get<bool>());
        return token;
    }
};

} // namespace

// ── Presenter claim ──────────────────────────────────────────────────────────

LOGOS_TEST(a_module_cannot_claim_the_presenter_role)
{
    Harness h("immediate", /*claim=*/false);
    const CallCaller module = CallCaller::module("wallet_ui_backend");
    LOGOS_ASSERT_FALSE(h.call(h.service->claim("{}"))["ok"].get<bool>());
}

LOGOS_TEST(an_unknown_caller_cannot_claim_the_presenter_role)
{
    Harness h("immediate", /*claim=*/false);
    LOGOS_ASSERT_FALSE(h.call(h.service->claim("{}"))["ok"].get<bool>());
}

LOGOS_TEST(the_presenter_role_is_claimed_once)
{
    // A logosctl user arrives as Host too; the host's own early claim is what keeps them out.
    Harness h;
    const CallCaller host = CallCaller::host();
    LOGOS_ASSERT_FALSE(h.secret.empty());
    LOGOS_ASSERT_FALSE(h.call(h.service->claim("{}"))["ok"].get<bool>());
}

LOGOS_TEST(a_claim_with_unknown_options_is_refused)
{
    Harness h("immediate", /*claim=*/false);
    const CallCaller host = CallCaller::host();
    LOGOS_ASSERT_FALSE(h.call(h.service->claim(R"({"attention":"loud"})"))["ok"].get<bool>());
    // The failed claim didn't use up the role.
    LOGOS_ASSERT_TRUE(h.call(h.service->claim(R"({"attention":"headless"})"))["ok"].get<bool>());
}

LOGOS_TEST(every_presenter_method_needs_the_secret)
{
    Harness h;
    const CallCaller host = CallCaller::host();
    const std::string wrong(h.secret.size(), 'x');
    LOGOS_ASSERT_FALSE(h.call(h.service->nextWork(wrong))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->nextWork(""))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->answer(wrong, "id1", "{}"))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->registerClickToken(wrong, "0123456789abcdef", "wallet_ui", ""))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->backendRegistered(wrong, "signer_ui", "signer_ui_backend"))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->backendUnregistered(wrong, "wallet_ui"))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->setInstalled(wrong, "[]"))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->setInstallable(wrong, "{}"))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->declareShell(wrong, R"({"name":"x"})"))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->configureProvider(wrong, "signing.approve", "cli_signer"))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->shellRequest(wrong, "signing.approve", "{}", false))["ok"].get<bool>());
}

LOGOS_TEST(the_secret_is_useless_to_a_module)
{
    Harness h;
    const CallCaller module = CallCaller::module("wallet_ui_backend");
    LOGOS_ASSERT_FALSE(h.call(h.service->nextWork(h.secret))["ok"].get<bool>());
    // A backend name nobody serves yet: only the Host check can refuse this.
    LOGOS_ASSERT_FALSE(h.call(h.service->backendRegistered(h.secret, "signer_ui", "signer_ui_backend"))["ok"].get<bool>());
}

LOGOS_TEST(a_shell_may_not_reserve_the_platform_namespace)
{
    Harness h;
    const CallCaller host = CallCaller::host();
    LOGOS_ASSERT_FALSE(h.call(h.service->declareShell(h.secret, R"({"name":"basecamp","reservedPrefix":"logos."})"))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->declareShell(h.secret, R"({"name":"basecamp","reservedPrefix":"basecamp"})"))["ok"].get<bool>());
    LOGOS_ASSERT_TRUE(h.call(h.service->declareShell(h.secret, R"({"name":"basecamp","reservedPrefix":"basecamp."})"))["ok"].get<bool>());
}

// ── Requesters ───────────────────────────────────────────────────────────────

LOGOS_TEST(a_host_caller_cannot_request_directly)
{
    // Under the daemon this is any logosctl user.
    Harness h;
    const CallCaller host = CallCaller::host();
    const json r = h.call(h.service->request("signing.approve", "{}", ""));
    LOGOS_ASSERT_FALSE(r["ok"].get<bool>());
    LOGOS_ASSERT_EQ(r["error"].get<std::string>(), err::notDeclared);
}

LOGOS_TEST(an_app_requests_through_its_registered_backend)
{
    Harness h;
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    const json r = h.call(h.service->request("signing.approve", "{}", ""));
    LOGOS_ASSERT_TRUE(r["ok"].get<bool>());
    const json state = h.call(h.service->result(r["handle"], r["receipt"]));
    LOGOS_ASSERT_EQ(state["state"].get<std::string>(), std::string("pending"));
    LOGOS_ASSERT_EQ(workOfKind(h.work(), "choose").size(), std::size_t(1));
}

LOGOS_TEST(a_module_that_never_declared_the_intent_is_refused_at_once)
{
    // Answered in the reply, with no handle and no `settled` broadcast.
    Harness h;
    const CallCaller module = CallCaller::module("random_module");
    const json r = h.call(h.service->request("signing.approve", "{}", ""));
    LOGOS_ASSERT_FALSE(r["ok"].get<bool>());
    LOGOS_ASSERT_EQ(r["error"].get<std::string>(), err::notDeclared);
    LOGOS_ASSERT_FALSE(r.contains("handle"));
    LOGOS_ASSERT_TRUE(h.settled.empty());
}

LOGOS_TEST(another_module_cannot_read_a_result)
{
    Harness h;
    json r;
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        r = h.call(h.service->request("signing.approve", "{}", ""));
    }
    const CallCaller other = CallCaller::module("chain_module");
    LOGOS_ASSERT_EQ(h.call(h.service->result(r["handle"], r["receipt"]))["error"].get<std::string>(),
                    std::string("unknown_handle"));
    LOGOS_ASSERT_FALSE(h.call(h.service->cancel(r["handle"], r["receipt"]))["ok"].get<bool>());
}

// ── Click tokens ─────────────────────────────────────────────────────────────

LOGOS_TEST(a_fresh_click_token_lets_the_request_pop_up)
{
    Harness h("interactive");
    const std::string token = h.clickToken("wallet_ui");
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    h.service->request("signing.approve", "{}", token);
    const json work = h.work();
    LOGOS_ASSERT_EQ(workOfKind(work, "choose").size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(workOfKind(work, "pending").empty());
}

LOGOS_TEST(no_click_token_means_the_pending_list)
{
    Harness h("interactive");
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    const json r = h.call(h.service->request("signing.approve", "{}", ""));
    LOGOS_ASSERT_TRUE(r["ok"].get<bool>());   // never a failure
    const json work = h.work();
    LOGOS_ASSERT_TRUE(workOfKind(work, "choose").empty());
    LOGOS_ASSERT_EQ(workOfKind(work, "pending").size(), std::size_t(1));
}

LOGOS_TEST(a_click_token_is_single_use)
{
    Harness h("interactive");
    const std::string token = h.clickToken("wallet_ui");
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    h.service->request("signing.approve", "{}", token);
    h.service->request("signing.approve", "{}", token);
    const json work = h.work();
    LOGOS_ASSERT_EQ(workOfKind(work, "choose").size(), std::size_t(1));
    LOGOS_ASSERT_EQ(workOfKind(work, "pending").size(), std::size_t(1));
}

LOGOS_TEST(an_expired_click_token_counts_as_no_click)
{
    Harness h("interactive");
    const std::string token = h.clickToken("wallet_ui");
    h.now += IntentsService::kClickTokenMs + 1;
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    h.service->request("signing.approve", "{}", token);
    LOGOS_ASSERT_EQ(workOfKind(h.work(), "pending").size(), std::size_t(1));
}

LOGOS_TEST(a_click_token_from_another_app_counts_as_no_click)
{
    Harness h("interactive");
    {
        const CallCaller host = CallCaller::host();
        h.service->backendRegistered(h.secret, "signer_ui", "signer_ui_backend");
    }
    const std::string signerToken = h.clickToken("signer_ui");
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    h.service->request("signing.approve", "{}", signerToken);
    LOGOS_ASSERT_EQ(workOfKind(h.work(), "pending").size(), std::size_t(1));
}

LOGOS_TEST(a_module_named_like_a_backend_cannot_spend_its_tokens)
{
    // signer_ui has a click token but no registered backend; a module that
    // merely calls itself signer_ui_backend is not it.
    // It declares the intent, so only the token decides whether it pops up.
    Harness h("interactive");
    {
        const CallCaller host = CallCaller::host();
        json packages = json::array({
            json{{"name", "signer_ui"}, {"installDir", h.dir.add("signer_ui", pkg("signer_ui", "ui_qml",
                                                                                 intentList({"signing.approve"})))}},
            json{{"name", "cli_signer"}, {"installDir", h.dir.add("cli_signer", pkg("cli_signer", "core",
                                                                                   intentList({"signing.approve"})))}},
            json{{"name", "signer_ui_backend"},
                 {"installDir", h.dir.add("impostor", pkg("signer_ui_backend", "core", json::array(),
                                                         intentList({"signing.approve"})))}},
        });
        h.service->setInstalled(h.secret, packages.dump());
        LOGOS_ASSERT_FALSE(h.call(h.service->backendRegistered(h.secret, "signer_ui", "signer_ui_backend"))["ok"].get<bool>());
    }
    const std::string token = h.clickToken("signer_ui");
    const CallCaller impostor = CallCaller::module("signer_ui_backend");
    LOGOS_ASSERT_TRUE(h.call(h.service->request("signing.approve", "{}", token))["ok"].get<bool>());
    const json work = h.work();
    LOGOS_ASSERT_TRUE(workOfKind(work, "choose").empty());
    LOGOS_ASSERT_EQ(workOfKind(work, "pending").size(), std::size_t(1));
}

LOGOS_TEST(a_click_token_bound_to_an_intent_works_only_for_it)
{
    Harness h("interactive");
    const std::string token = h.clickToken("wallet_ui", "wallets.unlock");
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    h.service->request("signing.approve", "{}", token);
    LOGOS_ASSERT_EQ(workOfKind(h.work(), "pending").size(), std::size_t(1));
}

LOGOS_TEST(click_tokens_are_only_for_installed_apps)
{
    Harness h("interactive");
    const CallCaller host = CallCaller::host();
    LOGOS_ASSERT_FALSE(h.call(h.service->registerClickToken(h.secret, "0123456789abcdef", "cli_signer", ""))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->registerClickToken(h.secret, "short", "wallet_ui", ""))["ok"].get<bool>());
}

// ── Providers ────────────────────────────────────────────────────────────────

LOGOS_TEST(respond_is_accepted_only_from_the_module_delivered_to)
{
    Harness h("immediate");
    json r;
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        r = h.call(h.service->request("signing.approve", "{}", ""));
    }
    const json choose = workOfKind(h.work(), "choose")[0];
    {
        const CallCaller host = CallCaller::host();
        h.service->answer(h.secret, choose["id"], R"({"action":"choose","provider":"cli_signer"})");
    }
    LOGOS_ASSERT_EQ(h.deliveries.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(h.deliveries[0].module, std::string("cli_signer"));
    const std::string dispatch = h.deliveries[0].args[0].get<std::string>();
    // deliver(dispatch, intent, params_json, requester): the requester is the app, not its backend.
    LOGOS_ASSERT_EQ(h.deliveries[0].args[3].get<std::string>(), std::string("wallet_ui"));
    h.deliveries[0].done(true, json(R"({"ok":true})"));

    {
        const CallCaller wrong = CallCaller::module("chain_module");
        LOGOS_ASSERT_FALSE(h.call(h.service->respond(dispatch, true, "\"forged\"", ""))["ok"].get<bool>());
    }
    {
        const CallCaller host = CallCaller::host();
        LOGOS_ASSERT_FALSE(h.call(h.service->respond(dispatch, true, "\"forged\"", ""))["ok"].get<bool>());
    }
    {
        const CallCaller provider = CallCaller::module("cli_signer");
        LOGOS_ASSERT_TRUE(h.call(h.service->respond(dispatch, true, "\"sig\"", ""))["ok"].get<bool>());
    }
    LOGOS_ASSERT_EQ(h.settled.size(), std::size_t(1));
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    const json state = h.call(h.service->result(r["handle"], r["receipt"]));
    LOGOS_ASSERT_EQ(state["result"]["data"].get<std::string>(), std::string("sig"));
}

LOGOS_TEST(a_backend_unregistered_after_delivery_cannot_respond)
{
    Harness h("immediate");
    {
        const CallCaller host = CallCaller::host();
        h.service->backendRegistered(h.secret, "signer_ui", "signer_ui_backend");
    }
    json r;
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        r = h.call(h.service->request("signing.approve", "{}", ""));
    }
    const json choose = workOfKind(h.work(), "choose")[0];
    {
        const CallCaller host = CallCaller::host();
        h.service->answer(h.secret, choose["id"], R"({"action":"choose","provider":"signer_ui"})");
        h.service->backendUnregistered(h.secret, "signer_ui");
    }
    const std::string dispatch = h.deliveries.at(0).args[0].get<std::string>();
    const CallCaller provider = CallCaller::module("signer_ui_backend");
    LOGOS_ASSERT_FALSE(h.call(h.service->respond(dispatch, true, "{}", ""))["ok"].get<bool>());
}

LOGOS_TEST(the_settled_event_carries_only_the_handle)
{
    Harness h("headless", true);
    {
        const CallCaller host = CallCaller::host();
        h.service->configureProvider(h.secret, "signing.approve", "cli_signer");
    }
    json r;
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        r = h.call(h.service->request("signing.approve", R"({"amount":"1 ETH"})", ""));
    }
    const std::string dispatch = h.deliveries.at(0).args[0].get<std::string>();
    {
        const CallCaller provider = CallCaller::module("cli_signer");
        h.service->respond(dispatch, true, R"({"signature":"0xsecret"})", "");
    }
    LOGOS_ASSERT_EQ(h.settled.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(h.settled[0], r["handle"].get<std::string>());
}

LOGOS_TEST(an_unreachable_provider_settles_unavailable_after_the_floor)
{
    Harness h("headless", true);
    {
        const CallCaller host = CallCaller::host();
        h.service->configureProvider(h.secret, "signing.approve", "cli_signer");
    }
    json r;
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        r = h.call(h.service->request("signing.approve", "{}", ""));
    }
    h.deliveries.at(0).done(false, json());
    h.now += 400;
    h.service->tick();
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    LOGOS_ASSERT_EQ(h.call(h.service->result(r["handle"], r["receipt"]))["result"]["error"].get<std::string>(),
                    err::unavailable);
}

// A backend registered again by the same run keeps what it asked for.
LOGOS_TEST(the_same_run_of_a_backend_registered_again_keeps_its_requests)
{
    Harness h("immediate");
    const auto registerRun = [&](const std::string& run) {
        const CallCaller host = CallCaller::host();
        return h.call(h.service->backendRegistered(h.secret, "wallet_ui", "wallet_ui_backend", run))["ok"].get<bool>();
    };
    LOGOS_ASSERT_TRUE(registerRun("run1"));
    json t;
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        t = h.call(h.service->request("signing.approve", "{}", ""));
    }
    LOGOS_ASSERT_TRUE(registerRun("run1"));
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        const json r = h.call(h.service->result(t["handle"].get<std::string>(), t["receipt"].get<std::string>()));
        LOGOS_ASSERT_EQ(r.value("state", std::string()), std::string("pending"));
    }
    LOGOS_ASSERT_TRUE(registerRun("run2"));
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        const json r = h.call(h.service->result(t["handle"].get<std::string>(), t["receipt"].get<std::string>()));
        LOGOS_ASSERT_EQ(r.value("error", std::string()), std::string("unknown_handle"));
    }
}

namespace {

// wallet_ui asks, the person picks cli_signer, and the delivery is outstanding.
Delivery delivered(Harness& h)
{
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        h.service->request("signing.approve", "{}", "");
    }
    const json choose = workOfKind(h.work(), "choose")[0];
    const CallCaller host = CallCaller::host();
    h.service->answer(h.secret, choose["id"], R"({"action":"choose","provider":"cli_signer"})");
    return h.deliveries.at(0);
}

} // namespace

// A reply arrives as JSON text or already parsed; either way {"ok":true} takes it,
// and a taken delivery waits for the person past the 20 s an untaken one gets.
LOGOS_TEST(a_delivery_taken_with_a_parsed_reply_waits_past_the_response_deadline)
{
    Harness h("immediate");
    delivered(h).done(true, json{{"ok", true}});
    h.now += 20001;
    h.service->tick();
    LOGOS_ASSERT_TRUE(h.settled.empty());
}

LOGOS_TEST(a_delivery_not_taken_times_out_at_the_response_deadline)
{
    Harness h("immediate");
    delivered(h).done(true, json{{"ok", false}});
    h.now += 20001;
    h.service->tick();
    LOGOS_ASSERT_EQ(h.settled.size(), std::size_t(1));
}

// Registering the backend's run again changes nothing, not even the click in flight.
LOGOS_TEST(registering_the_same_backend_run_again_keeps_the_click_token)
{
    Harness h("interactive");
    {
        const CallCaller host = CallCaller::host();
        h.service->backendRegistered(h.secret, "wallet_ui", "wallet_ui_backend", "run-1");
    }
    const std::string token = h.clickToken("wallet_ui");
    {
        const CallCaller host = CallCaller::host();
        h.service->backendRegistered(h.secret, "wallet_ui", "wallet_ui_backend", "run-1");
    }
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        h.service->request("signing.approve", "{}", token);
    }
    LOGOS_ASSERT_EQ(workOfKind(h.work(), "choose").size(), std::size_t(1));
}

LOGOS_TEST(a_new_backend_run_starts_without_the_last_runs_click)
{
    Harness h("interactive");
    const std::string token = h.clickToken("wallet_ui");
    {
        const CallCaller host = CallCaller::host();
        h.service->backendRegistered(h.secret, "wallet_ui", "wallet_ui_backend", "run-2");
    }
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        h.service->request("signing.approve", "{}", token);
    }
    LOGOS_ASSERT_EQ(workOfKind(h.work(), "pending").size(), std::size_t(1));
}

// The host may call set_installed from more than one thread. A call that
// started later must land later, however long the earlier one reads.
LOGOS_TEST(overlapping_set_installed_calls_land_in_the_order_they_started)
{
    test::TempPackages dir;
    const json older = json::array({json{{"name", "old_ui"}, {"installDir", dir.add("old_ui", pkg("old_ui", "ui_qml"))}}});
    const json newer = json::array({json{{"name", "new_ui"}, {"installDir", dir.add("new_ui", pkg("new_ui", "ui_qml"))}}});

    std::mutex m;
    std::condition_variable cv;
    int entered = 0;
    bool release = false;
    const auto load = [&](const std::vector<InstalledPackage>& list) {
        std::unique_lock<std::mutex> lock(m);
        const int call = ++entered;
        cv.notify_all();
        if (call == 1) cv.wait(lock, [&] { return release; });   // the first read is slow
        return loadPackages(list);
    };
    IntentsService s(IntentsService::Outbound{}, []() { return std::int64_t(0); }, test::counterIds(), {}, load);
    std::string secret;
    {
        const CallCaller host = CallCaller::host();
        secret = json::parse(s.claim(R"({"attention":"immediate"})"))["secret"].get<std::string>();
    }
    const auto install = [&](const json& packages) {
        const CallCaller host = CallCaller::host();
        s.setInstalled(secret, packages.dump());
    };

    std::thread first(install, older);
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return entered == 1; });
    }
    std::thread second(install, newer);
    {
        // Without the fix the second read runs now; with it, it waits its turn.
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return entered == 2; });
        release = true;
        cv.notify_all();
    }
    first.join();
    second.join();

    const CallCaller host = CallCaller::host();
    LOGOS_ASSERT_TRUE(json::parse(s.registerClickToken(secret, "0123456789abcdef", "new_ui", ""))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(json::parse(s.registerClickToken(secret, "0123456789abcdef", "old_ui", ""))["ok"].get<bool>());
}
