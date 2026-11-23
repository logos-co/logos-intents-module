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
    bool opensEverything;

    // A presenter that opens every request at once, as Basecamp does until its
    // Activity view shows the pending list, or one that leaves them pending.
    explicit Harness(bool opensEverything = true, bool claim = true) : opensEverything(opensEverything)
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
        secret = json::parse(service->claim()).value("secret", std::string());
        install();
    }

    void install()
    {
        const CallCaller host = CallCaller::host();
        json packages = json::array({
            json{{"name", "wallet_ui"}, {"installDir", dir.add("wallet_ui", pkg("wallet_ui", "ui_qml"))}},
            json{{"name", "wallet_ui_backend"},
                 {"installDir", dir.add("wallet_ui_backend", pkg("wallet_ui_backend", "core", json::array(),
                                                                 intentList({"signing.approve"})))}},
            json{{"name", "signer_ui_backend"},
                 {"installDir", dir.add("signer_ui_backend", pkg("signer_ui_backend", "core",
                                                                 intentList({"signing.approve"})))}},
            json{{"name", "cli_signer"}, {"installDir", dir.add("cli_signer", pkg("cli_signer", "core",
                                                                                 intentList({"signing.approve"})))}},
            json{{"name", "chain_module"}, {"installDir", dir.add("chain_module", pkg("chain_module", "core", json::array(),
                                                                                     intentList({"signing.approve"})))}},
        });
        service->setInstalled(secret, packages.dump());
    }

    json call(const std::string& out) { return json::parse(out); }

    // The presenter's work; one that opens everything opens each pending request first.
    json work()
    {
        const CallCaller host = CallCaller::host();
        json out = json::array();
        for (;;) {
            const json items = call(service->nextWork(secret)).value("items", json::array());
            bool opened = false;
            for (const json& item : items) {
                if (opensEverything && item.value("kind", "") == "pending") {
                    service->answer(secret, item["id"], R"({"action":"open"})");
                    opened = true;
                } else {
                    out.push_back(item);
                }
            }
            if (!opened) return out;
        }
    }

    // The person picks `provider` in the chooser on screen.
    void choose(const std::string& provider)
    {
        const json shown = workOfKind(work(), "choose").at(0);
        const CallCaller host = CallCaller::host();
        service->answer(secret, shown["id"], json{{"action", "choose"}, {"provider", provider}}.dump());
    }
};

} // namespace

// ── Presenter claim ──────────────────────────────────────────────────────────

LOGOS_TEST(a_module_cannot_claim_the_presenter_role)
{
    Harness h(/*opensEverything=*/true, /*claim=*/false);
    const CallCaller module = CallCaller::module("wallet_ui_backend");
    LOGOS_ASSERT_FALSE(h.call(h.service->claim())["ok"].get<bool>());
}

LOGOS_TEST(an_unknown_caller_cannot_claim_the_presenter_role)
{
    Harness h(/*opensEverything=*/true, /*claim=*/false);
    LOGOS_ASSERT_FALSE(h.call(h.service->claim())["ok"].get<bool>());
}

LOGOS_TEST(the_presenter_role_is_claimed_once)
{
    // A logosctl user arrives as Host too; the host's own early claim is what keeps them out.
    Harness h;
    const CallCaller host = CallCaller::host();
    LOGOS_ASSERT_FALSE(h.secret.empty());
    LOGOS_ASSERT_FALSE(h.call(h.service->claim())["ok"].get<bool>());
}

LOGOS_TEST(every_presenter_method_needs_the_secret)
{
    Harness h;
    const CallCaller host = CallCaller::host();
    const std::string wrong(h.secret.size(), 'x');
    LOGOS_ASSERT_FALSE(h.call(h.service->nextWork(wrong))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->nextWork(""))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->answer(wrong, "id1", "{}"))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->setInstalled(wrong, "[]"))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->declareShell(wrong, R"({"name":"x"})"))["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(h.call(h.service->shellRequest(wrong, "signing.approve", "{}", false))["ok"].get<bool>());
}

LOGOS_TEST(the_secret_is_useless_to_a_module)
{
    Harness h;
    const CallCaller module = CallCaller::module("wallet_ui_backend");
    LOGOS_ASSERT_FALSE(h.call(h.service->nextWork(h.secret))["ok"].get<bool>());
    // The secret is right: only the Host check can refuse this.
    LOGOS_ASSERT_FALSE(h.call(h.service->setInstalled(h.secret, "[]"))["ok"].get<bool>());
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
    const json r = h.call(h.service->request("signing.approve", "{}"));
    LOGOS_ASSERT_FALSE(r["ok"].get<bool>());
    LOGOS_ASSERT_EQ(r["error"].get<std::string>(), err::notDeclared);
}

// An app's backend asks as itself, with the intents its own metadata.json declares.
LOGOS_TEST(an_apps_backend_asks_as_itself)
{
    Harness h;
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    const json r = h.call(h.service->request("signing.approve", "{}"));
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
    const json r = h.call(h.service->request("signing.approve", "{}"));
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
        r = h.call(h.service->request("signing.approve", "{}"));
    }
    const CallCaller other = CallCaller::module("chain_module");
    LOGOS_ASSERT_EQ(h.call(h.service->result(r["handle"], r["receipt"]))["error"].get<std::string>(),
                    std::string("unknown_handle"));
    LOGOS_ASSERT_FALSE(h.call(h.service->cancel(r["handle"], r["receipt"]))["ok"].get<bool>());
}

// ── The pending list ─────────────────────────────────────────────────────────

LOGOS_TEST(a_request_waits_in_the_pending_list_until_the_presenter_opens_it)
{
    Harness h(/*opensEverything=*/false);
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    const json r = h.call(h.service->request("signing.approve", "{}"));
    LOGOS_ASSERT_TRUE(r["ok"].get<bool>());   // never a failure
    const json work = h.work();
    LOGOS_ASSERT_TRUE(workOfKind(work, "choose").empty());
    LOGOS_ASSERT_EQ(workOfKind(work, "pending").size(), std::size_t(1));
}

// ── Providers ────────────────────────────────────────────────────────────────

LOGOS_TEST(respond_is_accepted_only_from_the_module_delivered_to)
{
    Harness h;
    json r;
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        r = h.call(h.service->request("signing.approve", "{}"));
    }
    const json choose = workOfKind(h.work(), "choose")[0];
    {
        const CallCaller host = CallCaller::host();
        h.service->answer(h.secret, choose["id"], R"({"action":"choose","provider":"cli_signer"})");
    }
    LOGOS_ASSERT_EQ(h.deliveries.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(h.deliveries[0].module, std::string("cli_signer"));
    const std::string dispatch = h.deliveries[0].args[0].get<std::string>();
    // deliver(dispatch, intent, params_json, requester): the module that asked.
    LOGOS_ASSERT_EQ(h.deliveries[0].args[3].get<std::string>(), std::string("wallet_ui_backend"));
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

LOGOS_TEST(the_settled_event_carries_only_the_handle)
{
    Harness h;
    json r;
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        r = h.call(h.service->request("signing.approve", R"({"amount":"1 ETH"})"));
    }
    h.choose("cli_signer");
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
    Harness h;
    json r;
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        r = h.call(h.service->request("signing.approve", "{}"));
    }
    h.choose("cli_signer");
    h.deliveries.at(0).done(false, json());
    h.now += 400;
    h.service->tick();
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    LOGOS_ASSERT_EQ(h.call(h.service->result(r["handle"], r["receipt"]))["result"]["error"].get<std::string>(),
                    err::unavailable);
}

namespace {

// wallet_ui asks, the person picks cli_signer, and the delivery is outstanding.
Delivery delivered(Harness& h)
{
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        h.service->request("signing.approve", "{}");
    }
    h.choose("cli_signer");
    return h.deliveries.at(0);
}

} // namespace

// A reply arrives as JSON text or already parsed; either way {"ok":true} takes it,
// and a taken delivery waits for the person past the 20 s an untaken one gets.
LOGOS_TEST(a_delivery_taken_with_a_parsed_reply_waits_past_the_response_deadline)
{
    Harness h;
    delivered(h).done(true, json{{"ok", true}});
    h.now += 20001;
    h.service->tick();
    LOGOS_ASSERT_TRUE(h.settled.empty());
}

LOGOS_TEST(a_delivery_not_taken_times_out_at_the_response_deadline)
{
    Harness h;
    delivered(h).done(true, json{{"ok", false}});
    h.now += 20001;
    h.service->tick();
    LOGOS_ASSERT_EQ(h.settled.size(), std::size_t(1));
}

// The host may call set_installed from more than one thread. A call that
// started later must land later, however long the earlier one reads.
LOGOS_TEST(overlapping_set_installed_calls_land_in_the_order_they_started)
{
    test::TempPackages dir;
    const auto asker = [&](const char* name) {
        return json::array({json{{"name", name},
                                 {"installDir", dir.add(name, pkg(name, "core", json::array(), intentList({"x.y"})))}}});
    };
    const json older = asker("old_module");
    const json newer = asker("new_module");

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
        secret = json::parse(s.claim())["secret"].get<std::string>();
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

    {
        const CallCaller module = CallCaller::module("new_module");
        LOGOS_ASSERT_TRUE(json::parse(s.request("x.y", "{}"))["ok"].get<bool>());
    }
    const CallCaller module = CallCaller::module("old_module");
    LOGOS_ASSERT_EQ(json::parse(s.request("x.y", "{}"))["error"].get<std::string>(), err::notDeclared);
}
