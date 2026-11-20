#include <logos_test.h>

#include "intent_registry.h"
#include "intent_rules.h"

#include <fstream>
#include "test_support.h"

using namespace intents;
using test::intentList;
using test::pkg;

namespace {

IntentRegistry withShell()
{
    IntentRegistry r;
    r.declareShell("basecamp", "basecamp.", {"basecamp.apps.launch"},
                   {"basecamp.apps.launch", "basecamp.settings.open"},
                   {"basecamp.settings.open"}, "Basecamp", "");
    return r;
}

bool hasDiagnosticContaining(const IntentRegistry& r, const std::string& needle)
{
    for (const std::string& d : r.diagnostics())
        if (d.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

LOGOS_TEST(core_modules_and_apps_both_declare)
{
    IntentRegistry r;
    r.ingest("signer_ui", pkg("signer_ui", "ui_qml", intentList({"signing.approve"})), "Signer", "");
    r.ingest("wallet_module", pkg("wallet_module", "core", json::array(), intentList({"signing.approve"})), "", "");
    r.ingest("some_ui_plugin", pkg("some_ui_plugin", "ui", intentList({"signing.approve"})), "", "");

    LOGOS_ASSERT_TRUE(r.declaresProvide("signer_ui", "signing.approve"));
    LOGOS_ASSERT_TRUE(r.declaresUse("wallet_module", "signing.approve"));
    LOGOS_ASSERT_EQ(r.typeOf("signer_ui"), std::string("ui_qml"));
    LOGOS_ASSERT_EQ(r.typeOf("wallet_module"), std::string("core"));
    // Only core and ui_qml packages take part.
    LOGOS_ASSERT_EQ(r.typeOf("some_ui_plugin"), std::string());
}

LOGOS_TEST(reserved_namespaces_are_refused_from_packages)
{
    IntentRegistry r = withShell();
    r.ingest("evil_ui", pkg("evil_ui", "ui_qml",
                            intentList({"logos.thing.do", "basecamp.apps.launch", "share.text"}),
                            intentList({"basecamp.apps.launch"})), "", "");
    LOGOS_ASSERT_FALSE(r.declaresProvide("evil_ui", "logos.thing.do"));
    LOGOS_ASSERT_FALSE(r.declaresProvide("evil_ui", "basecamp.apps.launch"));
    LOGOS_ASSERT_TRUE(r.declaresProvide("evil_ui", "share.text"));
    // Using a shell intent is fine; providing one isn't.
    LOGOS_ASSERT_TRUE(r.declaresUse("evil_ui", "basecamp.apps.launch"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "reserved intent 'basecamp.apps.launch'"));
}

LOGOS_TEST(a_package_cannot_take_the_shell_or_link_name)
{
    IntentRegistry r = withShell();
    r.registerLinkRequester("basecamp_link", {"basecamp.settings.open"});
    r.ingest("basecamp", pkg("basecamp", "core", intentList({"share.text"})), "", "");
    r.ingest("basecamp_link", pkg("basecamp_link", "core", json::array(), intentList({"share.text"})), "", "");
    LOGOS_ASSERT_FALSE(r.declaresProvide("basecamp", "share.text"));
    LOGOS_ASSERT_FALSE(r.declaresUse("basecamp_link", "share.text"));
    LOGOS_ASSERT_TRUE(r.isShell("basecamp"));
    LOGOS_ASSERT_TRUE(r.declaresProvide("basecamp", "basecamp.settings.open"));
}

LOGOS_TEST(handoff_and_web_must_be_real_booleans)
{
    IntentRegistry r;
    json provides = json::array({
        json{{"intent", "a.one"}, {"handoff", true}, {"web", true}},
        json{{"intent", "a.two"}, {"handoff", "true"}, {"web", "false"}},
    });
    r.ingest("app_ui", pkg("app_ui", "ui_qml", provides), "", "");
    LOGOS_ASSERT_TRUE(r.isHandoff("app_ui", "a.one"));
    LOGOS_ASSERT_TRUE(r.isWebReachable("app_ui", "a.one"));
    LOGOS_ASSERT_FALSE(r.isHandoff("app_ui", "a.two"));
    LOGOS_ASSERT_FALSE(r.isWebReachable("app_ui", "a.two"));
}

LOGOS_TEST(entries_must_be_objects_with_valid_names)
{
    IntentRegistry r;
    json provides = json::array({"bare.string", json{{"intent", "Bad.Name"}}, json{{"intent", "ok.name"}},
                                 json{{"intent", "ok.name"}}});
    r.ingest("app_ui", pkg("app_ui", "ui_qml", provides), "", "");
    LOGOS_ASSERT_TRUE(r.declaresProvide("app_ui", "ok.name"));
    LOGOS_ASSERT_FALSE(r.declaresProvide("app_ui", "bare.string"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "twice"));
}

LOGOS_TEST(resolve_is_sorted_and_counts_providers)
{
    IntentRegistry r;
    r.ingest("zeta_ui", pkg("zeta_ui", "ui_qml", intentList({"share.text"})), "", "");
    r.ingest("alpha_ui", pkg("alpha_ui", "ui_qml", intentList({"share.text"})), "", "");
    const auto res = r.resolve("share.text");
    LOGOS_ASSERT_TRUE(res.status == IntentRegistry::Status::Ambiguous);
    LOGOS_ASSERT_EQ(res.found.size(), std::size_t(2));
    LOGOS_ASSERT_EQ(res.found[0].moduleName, std::string("alpha_ui"));
    LOGOS_ASSERT_TRUE(r.resolve("nothing.here").status == IntentRegistry::Status::None);
}

LOGOS_TEST(a_link_reaches_only_providers_that_opted_in)
{
    IntentRegistry r = withShell();
    r.registerLinkRequester("basecamp_link", {"basecamp.settings.open"});
    r.ingest("open_ui", pkg("open_ui", "ui_qml",
                            json::array({json{{"intent", "share.text"}, {"web", true}}})), "", "");
    r.ingest("closed_ui", pkg("closed_ui", "ui_qml", intentList({"share.text"})), "", "");
    LOGOS_ASSERT_TRUE(r.declaresUse("basecamp_link", "share.text"));
    const auto res = r.resolveFor("basecamp_link", "share.text");
    LOGOS_ASSERT_EQ(res.found.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(res.found[0].moduleName, std::string("open_ui"));
    LOGOS_ASSERT_EQ(r.resolveFor("other_ui", "share.text").found.size(), std::size_t(2));
}

LOGOS_TEST(installable_never_lists_installed_or_reserved)
{
    IntentRegistry r = withShell();
    r.ingest("have_ui", pkg("have_ui", "ui_qml", intentList({"share.text"})), "", "");
    LOGOS_ASSERT_TRUE(r.setInstallable({{"have_ui", {"share.text"}},
                                        {"new_ui", {"share.text", "basecamp.apps.launch"}}}));
    const auto names = r.installableFor("x_ui", "share.text");
    LOGOS_ASSERT_EQ(names.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(names[0], std::string("new_ui"));
    LOGOS_ASSERT_TRUE(r.installableFor("x_ui", "basecamp.apps.launch").empty());
    LOGOS_ASSERT_FALSE(r.setInstallable({{"have_ui", {"share.text"}},
                                         {"new_ui", {"share.text", "basecamp.apps.launch"}}}));
}

LOGOS_TEST(restricted_intents_admit_only_their_requesters)
{
    IntentRegistry r;
    r.setRestrictions({{"pkg.remove", {"pm_ui"}}, {"pkg.other", {}}});
    LOGOS_ASSERT_TRUE(r.requesterAllowed("pkg.remove", "pm_ui"));
    LOGOS_ASSERT_FALSE(r.requesterAllowed("pkg.remove", "wallet_ui"));
    LOGOS_ASSERT_TRUE(r.requesterAllowed("pkg.other", "anyone"));   // the empty list was refused

    // The next set replaces this one.
    r.setRestrictions({{"pkg.other", {"pm_ui"}}});
    LOGOS_ASSERT_TRUE(r.requesterAllowed("pkg.remove", "wallet_ui"));
    LOGOS_ASSERT_FALSE(r.requesterAllowed("pkg.other", "wallet_ui"));
}

LOGOS_TEST(rebuild_reads_disk_and_checks_the_name)
{
    test::TempPackages dir;
    const std::string good = dir.add("signer_ui", pkg("signer_ui", "ui_qml", intentList({"signing.approve"})));
    const std::string liar = dir.add("liar", pkg("signer_core", "core", intentList({"wallets.unlock"})));
    IntentRegistry r = withShell();
    r.rebuild(loadPackages({{"signer_ui", good, "Signer", ""},
                            {"liar", liar, "", ""},
                            {"missing_ui", "/nonexistent/dir", "", ""}}));
    LOGOS_ASSERT_TRUE(r.declaresProvide("signer_ui", "signing.approve"));
    LOGOS_ASSERT_FALSE(r.declaresProvide("liar", "wallets.unlock"));
    LOGOS_ASSERT_FALSE(r.declaresProvide("signer_core", "wallets.unlock"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "names another package"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "missing_ui"));
    // The shell survives a rebuild.
    LOGOS_ASSERT_TRUE(r.declaresProvide("basecamp", "basecamp.settings.open"));
    LOGOS_ASSERT_TRUE(r.isHandoff("basecamp", "basecamp.settings.open"));
}

// The shell's namespace is reserved even for packages read before the shell
// was declared, and the link requester's name even for a package that had it.
LOGOS_TEST(a_late_shell_or_link_declaration_still_applies)
{
    IntentRegistry r;
    r.ingest("early_ui", pkg("early_ui", "ui_qml", intentList({"basecamp.apps.launch", "share.text"})), "", "");
    r.ingest("logos_link", pkg("logos_link", "core", intentList({"share.text"})), "", "");
    LOGOS_ASSERT_TRUE(r.declaresProvide("early_ui", "basecamp.apps.launch"));

    r.declareShell("main_ui", "basecamp.", {}, {"basecamp.apps.launch"}, {}, "Logos", "");
    r.registerLinkRequester("logos_link", {"basecamp.apps.launch"});
    LOGOS_ASSERT_FALSE(r.declaresProvide("early_ui", "basecamp.apps.launch"));
    LOGOS_ASSERT_TRUE(r.declaresProvide("early_ui", "share.text"));
    LOGOS_ASSERT_FALSE(r.declaresProvide("logos_link", "share.text"));
    LOGOS_ASSERT_EQ(r.typeOf("logos_link"), std::string());
    const auto res = r.resolve("basecamp.apps.launch");
    LOGOS_ASSERT_EQ(res.found.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(res.found[0].moduleName, std::string("main_ui"));
}

LOGOS_TEST(package_names_with_anything_but_plain_characters_are_skipped)
{
    IntentRegistry r;
    r.ingest("evil\x1b[2J_ui", pkg("evil\x1b[2J_ui", "ui_qml", intentList({"share.text"})), "", "");
    r.ingest("ok_ui", pkg("ok_ui", "ui_qml", intentList({"share.text"})), "", "");
    LOGOS_ASSERT_EQ(r.resolve("share.text").found.size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(r.setInstallable({{"bad\x07name", {"share.other"}}, {"good_ui", {"share.other"}}}));
    const auto names = r.installableFor("x", "share.other");
    LOGOS_ASSERT_EQ(names.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(names[0], std::string("good_ui"));
    for (const std::string& d : r.diagnostics()) LOGOS_ASSERT_TRUE(d.find('\x1b') == std::string::npos);
}

LOGOS_TEST(a_package_declares_at_most_64_entries)
{
    IntentRegistry r;
    json provides = json::array();
    for (int i = 0; i < 100; ++i) provides.push_back(json{{"intent", "x.a" + std::to_string(i)}});
    r.ingest("wide_ui", pkg("wide_ui", "ui_qml", provides), "", "");
    LOGOS_ASSERT_TRUE(r.declaresProvide("wide_ui", "x.a63"));
    LOGOS_ASSERT_FALSE(r.declaresProvide("wide_ui", "x.a64"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "more than 64 'provides' entries"));
}

LOGOS_TEST(an_oversized_or_deeply_nested_manifest_is_refused_whole)
{
    test::TempPackages dir;
    const std::string deep = dir.add("deep_ui", json::object());
    {
        const std::string nest(5000, '[');
        std::ofstream(deep + "/metadata.json")
            << R"({"name":"deep_ui","type":"ui_qml","provides":[{"intent":"a.b","params":[{"name":"x","s":)"
            << nest << std::string(5000, ']') << "}]}]}";
    }
    const std::string big = dir.add("big_ui", json::object());
    {
        std::ofstream out(big + "/metadata.json");
        out << R"({"name":"big_ui","type":"ui_qml","pad":")" << std::string(1100 * 1024, 'a') << "\"}";
    }
    IntentRegistry r;
    r.rebuild(loadPackages({{"deep_ui", deep, "", ""}, {"big_ui", big, "", ""}}));
    LOGOS_ASSERT_EQ(r.typeOf("deep_ui"), std::string());
    LOGOS_ASSERT_EQ(r.typeOf("big_ui"), std::string());
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "deep_ui: metadata.json is malformed, too deeply nested or too large"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "big_ui: metadata.json is larger than 1 MiB"));
}
