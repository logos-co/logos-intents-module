// Regression tests for the security review of 2026-11-18: each attack, run
// through the service the way a hostile package would, and the safe outcome.

#include <logos_test.h>

#include "intent_rules.h"
#include "intents_service.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

// Basecamp's shell as app/ShellIntents.cpp and MainUIBackend.cpp declare it today.
const char* kBasecampShell = R"({
  "name": "main_ui",
  "reservedPrefix": "basecamp.",
  "uses": ["packages.show", "packages.install"],
  "provides": ["basecamp.repositories.manage", "basecamp.settings.open", "basecamp.apps.open",
               "basecamp.apps.launch", "basecamp.packages.open",
               "basecamp.packages.confirm_install", "basecamp.packages.confirm_uninstall",
               "basecamp.packages.confirm_upgrade"],
  "handoff": ["basecamp.repositories.manage", "basecamp.settings.open", "basecamp.apps.open",
              "basecamp.apps.launch", "basecamp.packages.open"],
  "displayName": "Logos",
  "restricted": {"basecamp.packages.confirm_uninstall": ["package_manager_ui_backend"],
                 "basecamp.packages.confirm_upgrade": ["package_manager_ui_backend"]},
  "link": {"name": "logos_link",
           "webIntents": ["basecamp.repositories.manage", "basecamp.settings.open", "basecamp.apps.open",
                          "basecamp.apps.launch", "basecamp.packages.open"]}
})";

struct Basecamp {
    test::TempPackages dir;
    std::int64_t now = 1000;
    std::vector<Delivery> deliveries;
    std::vector<std::string> settled;
    std::unique_ptr<IntentsService> service;
    std::string secret;
    bool opensEverything;

    // A presenter that opens every request at once, as Basecamp does until its
    // Activity view shows the pending list, or one that leaves them pending.
    explicit Basecamp(bool opensEverything, BrokerConfig config = {}) : opensEverything(opensEverything)
    {
        IntentsService::Outbound out;
        out.deliver = [this](const std::string& module, const json& args,
                             std::function<void(bool, const json&)> done) {
            deliveries.push_back({module, args, std::move(done)});
        };
        out.settled = [this](const std::string& h) { settled.push_back(h); };
        out.workAvailable = [](std::uint64_t) {};
        service = std::make_unique<IntentsService>(std::move(out), [this]() { return now; },
                                                   test::counterIds(), config);
        const CallCaller host = CallCaller::host();
        secret = json::parse(service->claim()).value("secret", std::string());
        install(json::array());
        service->declareShell(secret, kBasecampShell);
    }

    void install(const json& extra, const std::string& without = "")
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
                                                                                 intentList({"signing.approve", "absent.thing"})))}},
            json{{"name", "package_manager_ui"}, {"installDir", dir.add("package_manager_ui", pkg("package_manager_ui", "ui_qml"))}},
            json{{"name", "package_manager_ui_backend"},
                 {"installDir", dir.add("package_manager_ui_backend",
                                        pkg("package_manager_ui_backend", "core",
                                            intentList({"packages.install", "packages.show"}),
                                            intentList({"basecamp.packages.confirm_install"})))}},
        });
        for (const json& e : extra) packages.push_back(e);
        if (!without.empty())
            packages.erase(std::remove_if(packages.begin(), packages.end(),
                                          [&](const json& p) { return p["name"] == without; }),
                           packages.end());
        service->setInstalled(secret, packages.dump());
    }

    json call(const std::string& s) { return json::parse(s); }
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
    // The person opens what waits in the pending list; what that queues stays unpulled.
    void openPending()
    {
        const CallCaller host = CallCaller::host();
        for (const json& item : call(service->nextWork(secret)).value("items", json::array()))
            if (item.value("kind", "") == "pending") service->answer(secret, item["id"], R"({"action":"open"})");
    }
    void advance(std::int64_t ms)
    {
        now += ms;
        service->tick();
    }
};

long maxRssKb()
{
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

// Runs `fn` in a child process: "exit 0 | <output>", or "signal N" if it crashed.
// `fn` in a forked child; a child still running after `timeoutSeconds` (0: no
// limit) is killed and reported as "signal 14".
std::string inChild(const std::function<std::string()>& fn, unsigned timeoutSeconds = 0)
{
    int fds[2];
    if (pipe(fds) != 0) return "pipe failed";
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        if (timeoutSeconds) alarm(timeoutSeconds);
        std::string out;
        try { out = fn(); } catch (const std::exception& e) { out = std::string("threw: ") + e.what(); }
        (void)!write(fds[1], out.data(), out.size());
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    std::string got;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) got.append(buf, static_cast<std::size_t>(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    std::ostringstream s;
    if (WIFSIGNALED(status)) s << "signal " << WTERMSIG(status);
    else s << "exit " << WEXITSTATUS(status);
    s << " | " << got;
    return s.str();
}

// Everything written to stderr while `fn` runs.
std::string capturedStderr(const std::function<void()>& fn)
{
    std::fflush(stderr);
    const int saved = dup(2);
    std::FILE* tmp = std::tmpfile();
    dup2(fileno(tmp), 2);
    fn();
    std::fflush(stderr);
    dup2(saved, 2);
    close(saved);
    std::rewind(tmp);
    std::string log;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, tmp)) > 0) log.append(buf, n);
    std::fclose(tmp);
    return log;
}

std::size_t occurrences(const std::string& text, const std::string& needle)
{
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

long long msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

// ── H1: identity is the module's, never a name it happens to have ────────────

LOGOS_TEST(a_module_named_like_the_link_requester_is_refused)
{
    Basecamp h(/*opensEverything=*/false);
    h.work();
    json r;
    {
        const CallCaller impostor = CallCaller::module("logos_link");
        r = h.call(h.service->request("basecamp.apps.launch", R"({"app":"wallet_ui"})"));
        for (int i = 0; i < 16; ++i) h.service->request("basecamp.settings.open", "{}");
    }
    LOGOS_ASSERT_FALSE(r["ok"].get<bool>());
    LOGOS_ASSERT_TRUE(h.work().empty());

    // Real links still get through: the impostor took none of their slots.
    std::string handle;
    {
        const CallCaller host = CallCaller::host();
        handle = h.call(h.service->shellRequest(h.secret, "basecamp.apps.open", "{}", /*asLink=*/true))["handle"];
    }
    const auto deliver = workOfKind(h.work(), "deliver");
    LOGOS_ASSERT_EQ(deliver.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(deliver[0]["intent"].get<std::string>(), std::string("basecamp.apps.open"));
}

LOGOS_TEST(a_module_named_like_the_shell_is_refused)
{
    Basecamp h(/*opensEverything=*/false);
    h.work();
    json r;
    {
        const CallCaller impostor = CallCaller::module("main_ui");
        r = h.call(h.service->request("packages.install", R"({"name":"evil_pkg"})"));
    }
    LOGOS_ASSERT_FALSE(r["ok"].get<bool>());
    LOGOS_ASSERT_TRUE(h.deliveries.empty());
    LOGOS_ASSERT_TRUE(h.work().empty());
}

LOGOS_TEST(a_module_named_like_an_app_is_refused)
{
    Basecamp h(/*opensEverything=*/false);
    h.work();
    json r;
    // Refused for its name, not only for declaring nothing: a chooser could show it as the app.
    const std::string log = capturedStderr([&]() {
        const CallCaller impostor = CallCaller::module("wallet_ui");   // not wallet_ui_backend
        r = h.call(h.service->request("signing.approve", R"({"tx":"attacker-chosen"})"));
        LOGOS_ASSERT_EQ(h.call(h.service->result("id1", "id2"))["error"].get<std::string>(),
                        std::string("unknown_handle"));
    });
    LOGOS_ASSERT_TRUE(log.find("refused module wallet_ui") != std::string::npos);
    LOGOS_ASSERT_FALSE(r["ok"].get<bool>());
    LOGOS_ASSERT_TRUE(workOfKind(h.work(), "pending").empty());

    // The app itself, through its backend, is unaffected.
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    LOGOS_ASSERT_TRUE(h.call(h.service->request("signing.approve", "{}"))["ok"].get<bool>());
}

// ── H2: a package's metadata.json can't crash the module ─────────────────────

LOGOS_TEST(deeply_nested_metadata_cannot_crash_set_installed)
{
    const std::string outcome = inChild([]() -> std::string {
        test::TempPackages dir;
        const int depth = 300000;
        const std::string meta = R"({"name":"deep_ui","type":"ui_qml","provides":[{"intent":"a.b","params":[{"name":"x","s":)"
                                 + std::string(depth, '[') + std::string(depth, ']') + "}]}]}";
        const std::string d = dir.add("deep_ui", json::object());
        {
            std::FILE* f = std::fopen((d + "/metadata.json").c_str(), "w");
            std::fwrite(meta.data(), 1, meta.size(), f);
            std::fclose(f);
        }
        IntentsService s(IntentsService::Outbound{}, []() { return std::int64_t(0); }, test::counterIds());
        const CallCaller host = CallCaller::host();
        const std::string secret = json::parse(s.claim())["secret"].get<std::string>();
        return s.setInstalled(secret, json::array({json{{"name", "deep_ui"}, {"installDir", d}}}).dump());
    });
    LOGOS_ASSERT_TRUE(outcome.rfind("exit 0", 0) == 0);
    LOGOS_ASSERT_TRUE(outcome.find("too deeply nested") != std::string::npos);
}

// A provider shapes its own `deliver` reply; reading it can't crash the module either.
LOGOS_TEST(a_deeply_nested_deliver_reply_cannot_crash_the_module)
{
    const std::string outcome = inChild([]() -> std::string {
        // Built straight from text: copying a value this deep would overflow the
        // test's own stack, which is exactly what the module must never do.
        const std::string nest = std::string(300000, '[') + std::string(300000, ']');
        const json deepObject = json::parse("{\"ok\":" + nest + "}");
        const json deepText(nest);
        std::int64_t now = 1000;
        IntentsService::Outbound out;
        out.deliver = [&](const std::string&, const json&, std::function<void(bool, const json&)> done) {
            done(true, deepObject);
            done(true, deepText);
        };
        IntentsService s(std::move(out), [&now]() { return now; }, test::counterIds());
        test::TempPackages dir;
        const CallCaller host = CallCaller::host();
        const std::string secret = json::parse(s.claim())["secret"].get<std::string>();
        s.setInstalled(secret, json::array({
            json{{"name", "asker"}, {"installDir", dir.add("asker", pkg("asker", "core", json::array(), intentList({"x.y"})))}},
            json{{"name", "evil_provider"}, {"installDir", dir.add("evil", pkg("evil_provider", "core", intentList({"x.y"})))}},
        }).dump());
        {
            const CallCaller asker = CallCaller::module("asker");
            s.request("x.y", "{}");
        }
        // The person opens it and picks evil_provider; delivering to it is where its replies arrive.
        const auto first = [&](const char* kind) {
            const json items = json::parse(s.nextWork(secret)).value("items", json::array());
            for (const json& item : items)
                if (item.value("kind", "") == kind) return item;
            return json();
        };
        const json pending = first("pending");
        if (pending.is_null()) return std::string("no pending request");
        s.answer(secret, pending["id"], R"({"action":"open"})");
        const json choose = first("choose");
        if (choose.is_null()) return std::string("no chooser");
        return s.answer(secret, choose["id"], R"({"action":"choose","provider":"evil_provider"})");
    });
    LOGOS_ASSERT_TRUE(outcome.rfind("exit 0", 0) == 0);
    LOGOS_ASSERT_TRUE(outcome.find("\"ok\":true") != std::string::npos);   // delivered, replies read
}

// ── M1, M2: nothing installed shows before a request is opened, and offers don't stream ─

LOGOS_TEST(unopened_requests_reveal_nothing_about_what_is_installed)
{
    Basecamp h(/*opensEverything=*/false);
    h.work();
    json present, absent;
    {
        const CallCaller probe = CallCaller::module("chain_module");
        present = h.call(h.service->request("signing.approve", "{}"));
        absent = h.call(h.service->request("absent.thing", "{}"));
    }
    h.advance(500);
    const CallCaller probe = CallCaller::module("chain_module");
    const json a = h.call(h.service->result(present["handle"], present["receipt"]));
    const json b = h.call(h.service->result(absent["handle"], absent["receipt"]));
    LOGOS_ASSERT_EQ(a.value("state", ""), std::string("pending"));
    LOGOS_ASSERT_EQ(b.value("state", ""), std::string("pending"));
}

LOGOS_TEST(install_offers_need_an_opened_request)
{
    Basecamp h(/*opensEverything=*/false);
    h.work();
    {
        const CallCaller attacker = CallCaller::module("chain_module");
        for (int i = 0; i < 50; ++i) {
            const json t = h.call(h.service->request("absent.thing", "{}"));
            if (t["ok"].get<bool>()) h.service->cancel(t["handle"], t["receipt"]);
        }
    }
    LOGOS_ASSERT_TRUE(workOfKind(h.work(), "offer_install").empty());
}

// ── M3: one module can't crowd another's chooser out ────────────────────────

LOGOS_TEST(withdrawn_requests_cannot_crowd_out_another_apps_chooser)
{
    Basecamp h(/*opensEverything=*/false);
    h.work();
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        h.service->request("signing.approve", "{}");
    }
    h.openPending();   // the person opens Wallet's request
    int admitted = 0;
    {
        const CallCaller attacker = CallCaller::module("chain_module");
        for (int i = 0; i < 520; ++i) {
            const json t = h.call(h.service->request("signing.approve", "{}"));
            if (!t["ok"].get<bool>()) continue;
            ++admitted;
            h.service->cancel(t["handle"], t["receipt"]);
        }
    }
    LOGOS_ASSERT_LE(admitted, 20);
    const json work = h.work();
    LOGOS_ASSERT_EQ(workOfKind(work, "choose").size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(workOfKind(work, "pending").empty());
    LOGOS_ASSERT_TRUE(workOfKind(work, "dismiss").empty());
}

// ── M4: a stranger's payload is never read ──────────────────────────────────

LOGOS_TEST(a_response_from_a_stranger_reads_nothing)
{
    Basecamp h(/*opensEverything=*/true);
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        h.service->request("signing.approve", "{}");
    }
    const json choose = workOfKind(h.work(), "choose").at(0);
    {
        const CallCaller host = CallCaller::host();
        h.service->answer(h.secret, choose["id"], R"({"action":"choose","provider":"cli_signer"})");
    }
    const std::string dispatch = h.deliveries.at(0).args[0].get<std::string>();
    // Refused before parsing: the stranger's payload isn't even read, and the
    // dispatch still waits for the provider it went to.
    const std::string log = capturedStderr([&]() {
        const CallCaller stranger = CallCaller::module("chain_module");
        LOGOS_ASSERT_FALSE(h.call(h.service->respond(dispatch, true, R"({"forged":true})", ""))["ok"].get<bool>());
    });
    LOGOS_ASSERT_TRUE(log.find("payload unread") != std::string::npos);
    const CallCaller provider = CallCaller::module("cli_signer");
    LOGOS_ASSERT_TRUE(h.call(h.service->respond(dispatch, true, "\"sig\"", ""))["ok"].get<bool>());
}

// A `deliver` reply over 64 KiB isn't read, so it isn't a "took it": the
// provider gets the short response deadline, not the 10-minute one.
LOGOS_TEST(an_oversized_deliver_reply_is_not_an_acceptance)
{
    Basecamp h(/*opensEverything=*/true);
    json ticket;
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        ticket = h.call(h.service->request("signing.approve", "{}"));
    }
    const json choose = workOfKind(h.work(), "choose").at(0);
    {
        const CallCaller host = CallCaller::host();
        h.service->answer(h.secret, choose["id"], R"({"action":"choose","provider":"cli_signer"})");
    }
    const std::string padded = json{{"ok", true}, {"pad", std::string(70 * 1024, 'x')}}.dump();
    h.deliveries.at(0).done(true, json(padded));
    h.advance(BrokerConfig{}.responseMs + 1);
    const CallCaller backend = CallCaller::module("wallet_ui_backend");
    const json r = h.call(h.service->result(ticket["handle"], ticket["receipt"]));
    LOGOS_ASSERT_EQ(r.value("state", std::string()), std::string("settled"));
    LOGOS_ASSERT_EQ(r["result"]["error"].get<std::string>(), std::string("timeout"));
}

// ── M5: one package can't stall everyone's calls ───────────────────────────

LOGOS_TEST(a_huge_manifest_is_refused_quickly)
{
    test::TempPackages dir;
    json provides = json::array();
    for (int i = 0; i < 44000; ++i) provides.push_back(json{{"intent", "x.a" + std::to_string(i)}});
    const std::string d = dir.add("wide_module", json{{"name", "wide_module"}, {"type", "core"}, {"provides", provides}});
    IntentsService s(IntentsService::Outbound{}, []() { return std::int64_t(0); }, test::counterIds());
    const CallCaller host = CallCaller::host();
    const std::string secret = json::parse(s.claim())["secret"].get<std::string>();
    const auto t0 = std::chrono::steady_clock::now();
    s.setInstalled(secret, json::array({json{{"name", "wide_module"}, {"installDir", d}}}).dump());
    LOGOS_ASSERT_LT(msSince(t0), 1000);
}

// The link requester's `uses` used to be re-derived after every package, in
// time quadratic in all the web opt-ins so far.
LOGOS_TEST(web_opt_ins_do_not_slow_down_rebuilds)
{
    Basecamp h(/*opensEverything=*/false);
    json extra = json::array();
    for (int p = 0; p < 120; ++p) {
        json provides = json::array();
        for (int i = 0; i < 64; ++i)
            provides.push_back(json{{"intent", "w" + std::to_string(p) + ".a" + std::to_string(i)}, {"web", true}});
        const std::string n = "web" + std::to_string(p) + "_module";
        extra.push_back(json{{"name", n}, {"installDir", h.dir.add(n, json{{"name", n}, {"type", "core"},
                                                                          {"provides", provides}})}});
    }
    const auto t0 = std::chrono::steady_clock::now();
    h.install(extra);
    LOGOS_ASSERT_LT(msSince(t0), 1000);
}

// ── M6: what the module holds is bounded ────────────────────────────────────

LOGOS_TEST(an_oversized_result_is_refused)
{
    Basecamp h(/*opensEverything=*/false);
    h.install(json::array({json{{"name", "hog_module"},
                                {"installDir", h.dir.add("hog_module", pkg("hog_module", "core",
                                                                           intentList({"hog.store"}),
                                                                           intentList({"hog.store"})))}}}));
    std::string big = "[";
    for (int i = 0; i < 20; ++i) big += (i ? ",\"" : "\"") + std::string(65536, 'a') + "\"";
    big += "]";
    int stored = 0;
    for (int n = 0; n < 4; ++n) {
        {
            const CallCaller hog = CallCaller::module("hog_module");
            h.service->request("hog.store", "{}");
        }
        const std::string dispatch = h.deliveries.back().args[0].get<std::string>();
        const CallCaller hog = CallCaller::module("hog_module");
        if (h.call(h.service->respond(dispatch, true, big, ""))["ok"].get<bool>()) ++stored;
    }
    LOGOS_ASSERT_EQ(stored, 0);
}

LOGOS_TEST(the_work_queue_does_not_keep_withdrawn_payloads)
{
    Basecamp h(/*opensEverything=*/false);
    h.install(json::array({json{{"name", "shellish_module"},
                                {"installDir", h.dir.add("shellish_module", pkg("shellish_module", "core", json::array(),
                                                                                intentList({"basecamp.settings.open"})))}}}));
    h.work();
    std::string params = "{";
    for (int i = 0; i < 13; ++i) params += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":\"" + std::string(65536, 'a') + "\"";
    params += "}";
    for (int i = 0; i < 4; ++i) {
        json t;
        {
            const CallCaller m = CallCaller::module("shellish_module");
            t = h.call(h.service->request("basecamp.settings.open", params));
        }
        h.openPending();   // queues the shell's `deliver`, params and all, unpulled
        if (!t["ok"].get<bool>()) continue;
        const CallCaller m = CallCaller::module("shellish_module");
        h.service->cancel(t["handle"], t["receipt"]);
    }
    const CallCaller host = CallCaller::host();
    LOGOS_ASSERT_LT(h.service->nextWork(h.secret).size(), std::size_t(64 * 1024));
}

// ── L1, L2: no event storms, and no event before its reply ──────────────────

LOGOS_TEST(undeclared_requests_raise_no_events)
{
    Basecamp h(/*opensEverything=*/false);
    h.settled.clear();
    const CallCaller spam = CallCaller::module("spam_module");
    for (int i = 0; i < 20000; ++i) h.service->request("x.y", "{}");
    LOGOS_ASSERT_TRUE(h.settled.empty());
}

LOGOS_TEST(requests_past_the_rate_are_refused_in_the_reply)
{
    Basecamp h(/*opensEverything=*/true);
    int admitted = 0;
    const CallCaller m = CallCaller::module("chain_module");
    for (int i = 0; i < 5000; ++i)
        if (h.call(h.service->request("absent.thing", "{}"))["ok"].get<bool>()) ++admitted;
    LOGOS_ASSERT_EQ(admitted, 20);
}

// Refused with `failed` like any failure, so the log says why: once a minute
// per requester, however fast it asks.
LOGOS_TEST(requests_past_the_rate_are_logged_once_a_minute)
{
    Basecamp h(/*opensEverything=*/true);
    const std::string log = capturedStderr([&]() {
        const CallCaller m = CallCaller::module("chain_module");
        for (int i = 0; i < 100; ++i) h.service->request("absent.thing", "{}");
    });
    LOGOS_ASSERT_EQ(occurrences(log, "refused absent.thing from chain_module: over its request rate"), std::size_t(1));
}

// A new shell declaration replaces the restrictions whole, never adds to them,
// and one that can't be read changes nothing.
LOGOS_TEST(a_shell_declaration_replaces_its_restrictions_whole)
{
    Basecamp h(/*opensEverything=*/true);
    h.install(json::array({json{{"name", "cleanup_module"},
                                {"installDir", h.dir.add("cleanup_module",
                                                         pkg("cleanup_module", "core", json::array(),
                                                             intentList({"basecamp.packages.confirm_uninstall"})))}}}));
    const auto ask = [&]() {
        const CallCaller m = CallCaller::module("cleanup_module");
        h.service->request("basecamp.packages.confirm_uninstall", "{}");
    };
    const auto declare = [&](const json& shell) {
        const CallCaller host = CallCaller::host();
        return h.call(h.service->declareShell(h.secret, shell.dump()))["ok"].get<bool>();
    };
    ask();
    LOGOS_ASSERT_TRUE(workOfKind(h.work(), "deliver").empty());   // only package_manager_ui_backend may

    json shell = json::parse(kBasecampShell);
    shell["restricted"] = json{{"basecamp.packages.confirm_upgrade", {"package_manager_ui_backend"}}};
    LOGOS_ASSERT_TRUE(declare(shell));
    ask();
    LOGOS_ASSERT_EQ(workOfKind(h.work(), "deliver").size(), std::size_t(1));

    json empty = shell;
    empty["restricted"] = json{{"basecamp.packages.confirm_upgrade", json::array()}};
    LOGOS_ASSERT_FALSE(declare(empty));
    json notAnObject = shell;
    notAnObject["restricted"] = "everyone";
    LOGOS_ASSERT_FALSE(declare(notAnObject));
    ask();
    LOGOS_ASSERT_EQ(workOfKind(h.work(), "deliver").size(), std::size_t(1));
}

// Each of the presenter's own requests ends as a work item, so they have a rate too.
LOGOS_TEST(the_presenters_own_requests_have_a_rate_too)
{
    Basecamp h(/*opensEverything=*/false);
    int admitted = 0;
    const CallCaller host = CallCaller::host();
    for (int i = 0; i < 100; ++i)
        if (h.call(h.service->shellRequest(h.secret, "packages.show", "{}", false))["ok"].get<bool>()) ++admitted;
    LOGOS_ASSERT_EQ(admitted, 20);
}

LOGOS_TEST(settled_is_never_sent_before_the_request_returns)
{
    BrokerConfig instant;
    instant.errorFloorMs = 0;   // a refusal that would settle inside the call
    Basecamp h(/*opensEverything=*/true, instant);
    h.settled.clear();
    json t;
    {
        const CallCaller m = CallCaller::module("chain_module");
        t = h.call(h.service->request("absent.thing", "{}"));
    }
    LOGOS_ASSERT_TRUE(h.settled.empty());
    h.work();   // opened: nothing provides it
    h.advance(100);
    LOGOS_ASSERT_EQ(h.settled.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(h.settled[0], t["handle"].get<std::string>());
}

// ── L3: nothing a package controls can forge a log line ─────────────────────

LOGOS_TEST(an_intent_name_cannot_forge_log_lines)
{
    Basecamp h(/*opensEverything=*/false);
    const std::string log = capturedStderr([&]() {
        const CallCaller m = CallCaller::module("chain_module");
        h.service->request("x\n[intents_module] dropped a response from cli_signer: not waiting for it\n",
                           "not json");
        h.service->request("signing.approve", "not json");
    });
    LOGOS_ASSERT_TRUE(log.find("\n[intents_module] dropped a response") == std::string::npos);
}

LOGOS_TEST(metadata_cannot_forge_log_lines)
{
    Basecamp h(/*opensEverything=*/false);
    const json meta = json{{"name", "liar_module"}, {"type", "core"},
                           {"provides", json::array({json{{"intent", "X\n[intents_module] presenter claimed (immediate)"}}})},
                           {"uses", json::array({json{{"intent", "y\r\n[intents_module] refused claim"}}})}};
    const std::string log = capturedStderr([&]() {
        h.install(json::array({json{{"name", "liar_module"}, {"installDir", h.dir.add("liar_module", meta)}}}));
    });
    LOGOS_ASSERT_TRUE(log.find("\n[intents_module] presenter claimed") == std::string::npos);
    LOGOS_ASSERT_TRUE(log.find("\n[intents_module] refused claim") == std::string::npos);
}

// A host sends what is installed again and again: a module whose directory
// holds no metadata.json declares nothing, and says nothing.
LOGOS_TEST(a_package_without_metadata_json_declares_nothing_quietly)
{
    Basecamp h(/*opensEverything=*/false);
    const std::string bare = h.dir.add("bare_module", json::object());
    std::remove((bare + "/metadata.json").c_str());
    const std::string log = capturedStderr([&]() {
        h.install(json::array({json{{"name", "bare_module"}, {"installDir", bare}}}));
    });
    LOGOS_ASSERT_TRUE(log.find("bare_module") == std::string::npos);
}

// One that can't be read is said once while it lasts, and again if it comes back.
LOGOS_TEST(a_diagnostic_is_logged_once_while_it_lasts)
{
    Basecamp h(/*opensEverything=*/false);
    const json liar = json::array({json{{"name", "liar_module"},
                                        {"installDir", h.dir.add("liar_module", json{{"name", "someone_else"}})}}});
    const std::string twice = capturedStderr([&]() {
        h.install(liar);
        h.install(liar);
    });
    LOGOS_ASSERT_EQ(occurrences(twice, "liar_module: metadata.json names another package"), std::size_t(1));
    const std::string back = capturedStderr([&]() {
        h.install(json::array());
        h.install(liar);
    });
    LOGOS_ASSERT_EQ(occurrences(back, "liar_module: metadata.json names another package"), std::size_t(1));
}

// ── L4: bad presenter input is refused, not thrown ──────────────────────────

LOGOS_TEST(a_shell_answer_with_a_null_error_completes)
{
    Basecamp h(/*opensEverything=*/false);
    h.work();
    std::string handle;
    {
        const CallCaller host = CallCaller::host();
        handle = h.call(h.service->shellRequest(h.secret, "basecamp.settings.open", "{}", /*asLink=*/true))["handle"];
    }
    const auto deliver = workOfKind(h.work(), "deliver");
    LOGOS_ASSERT_EQ(deliver.size(), std::size_t(1));
    const CallCaller host = CallCaller::host();
    const json first = h.call(h.service->answer(h.secret, deliver[0]["id"],
                                                R"({"action":"respond","ok":true,"data":{},"error":null})"));
    LOGOS_ASSERT_TRUE(first["ok"].get<bool>());
    const auto results = workOfKind(h.work(), "result");
    LOGOS_ASSERT_EQ(results.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(results[0]["handle"].get<std::string>(), handle);
    LOGOS_ASSERT_TRUE(results[0]["result"]["ok"].get<bool>());
}

// ── L5: a queued chooser gets its full time once it's on screen ─────────────

LOGOS_TEST(a_queued_choice_gets_its_full_time_once_shown)
{
    Basecamp h(/*opensEverything=*/true);
    h.work();
    {
        const CallCaller backend = CallCaller::module("wallet_ui_backend");
        h.service->request("signing.approve", "{}");
    }
    h.now += 1;
    json second;
    {
        const CallCaller m = CallCaller::module("chain_module");
        second = h.call(h.service->request("signing.approve", "{}"));
    }
    const auto first = workOfKind(h.work(), "choose");
    LOGOS_ASSERT_EQ(first.size(), std::size_t(1));
    h.advance(10 * 60 * 1000 - 1000);
    {
        const CallCaller host = CallCaller::host();
        h.service->answer(h.secret, first[0]["id"], R"({"action":"choose","provider":"cli_signer"})");
    }
    LOGOS_ASSERT_EQ(workOfKind(h.work(), "choose").size(), std::size_t(1));
    h.advance(2000);
    const CallCaller m = CallCaller::module("chain_module");
    LOGOS_ASSERT_EQ(h.call(h.service->result(second["handle"], second["receipt"])).value("state", ""),
                    std::string("pending"));
}

// ── 2026-11-19 review ────────────────────────────────────────────────────────

// A package may replace its metadata.json with a FIFO, a socket or a device.
// Opening a FIFO with no writer blocks, and the host runs one call at a time.
LOGOS_TEST(a_fifo_manifest_cannot_stall_set_installed)
{
    const std::string outcome = inChild([]() -> std::string {
        test::TempPackages dir;
        const std::string d = dir.add("fifo_ui", json::object());
        std::remove((d + "/metadata.json").c_str());
        if (mkfifo((d + "/metadata.json").c_str(), 0600) != 0) return "mkfifo failed";
        IntentsService s(IntentsService::Outbound{}, []() { return std::int64_t(0); }, test::counterIds());
        const CallCaller host = CallCaller::host();
        const std::string secret = json::parse(s.claim())["secret"].get<std::string>();
        return s.setInstalled(secret, json::array({
            json{{"name", "fifo_ui"}, {"installDir", d}},
            json{{"name", "fine_ui"}, {"installDir", dir.add("fine_ui", pkg("fine_ui", "ui_qml"))}},
        }).dump());
    }, /*timeoutSeconds=*/10);
    LOGOS_ASSERT_TRUE(outcome.rfind("exit 0", 0) == 0);
    LOGOS_ASSERT_TRUE(outcome.find("not a regular file") != std::string::npos);
}
