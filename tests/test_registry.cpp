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

// An app's screen is only a name: its backend module asks and answers for it.
LOGOS_TEST(an_apps_screen_is_only_a_name_and_its_backend_declares)
{
    IntentRegistry r;
    r.ingest("signer_ui", pkg("signer_ui", "ui_qml", intentList({"signing.approve"}),
                              intentList({"wallets.unlock"})), "Signer", "");
    r.ingest("signer_ui_backend", pkg("signer_ui_backend", "core", intentList({"signing.approve"})), "Signer", "");
    r.ingest("wallet_module", pkg("wallet_module", "core", json::array(), intentList({"signing.approve"})), "", "");
    r.ingest("some_ui_plugin", pkg("some_ui_plugin", "ui", intentList({"signing.approve"})), "", "");

    LOGOS_ASSERT_FALSE(r.declaresProvide("signer_ui", "signing.approve"));
    LOGOS_ASSERT_FALSE(r.declaresUse("signer_ui", "wallets.unlock"));
    LOGOS_ASSERT_TRUE(r.declaresProvide("signer_ui_backend", "signing.approve"));
    LOGOS_ASSERT_TRUE(r.declaresUse("wallet_module", "signing.approve"));
    LOGOS_ASSERT_EQ(r.resolve("signing.approve").found.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(r.typeOf("signer_ui"), std::string("ui_qml"));
    LOGOS_ASSERT_EQ(r.typeOf("signer_ui_backend"), std::string("core"));
    // Only core and ui_qml packages take part.
    LOGOS_ASSERT_EQ(r.typeOf("some_ui_plugin"), std::string());
}

LOGOS_TEST(reserved_namespaces_are_refused_from_packages)
{
    IntentRegistry r = withShell();
    r.ingest("evil_ui", pkg("evil_ui", "core",
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
    r.ingest("app_ui", pkg("app_ui", "core", provides), "", "");
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
    r.ingest("app_ui", pkg("app_ui", "core", provides), "", "");
    LOGOS_ASSERT_TRUE(r.declaresProvide("app_ui", "ok.name"));
    LOGOS_ASSERT_FALSE(r.declaresProvide("app_ui", "bare.string"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "twice"));
}

LOGOS_TEST(resolve_is_sorted_and_counts_providers)
{
    IntentRegistry r;
    r.ingest("zeta_ui", pkg("zeta_ui", "core", intentList({"share.text"})), "", "");
    r.ingest("alpha_ui", pkg("alpha_ui", "core", intentList({"share.text"})), "", "");
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
    r.ingest("open_ui", pkg("open_ui", "core",
                            json::array({json{{"intent", "share.text"}, {"web", true}}})), "", "");
    r.ingest("closed_ui", pkg("closed_ui", "core", intentList({"share.text"})), "", "");
    LOGOS_ASSERT_TRUE(r.declaresUse("basecamp_link", "share.text"));
    const auto res = r.resolveFor("basecamp_link", "share.text");
    LOGOS_ASSERT_EQ(res.found.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(res.found[0].moduleName, std::string("open_ui"));
    LOGOS_ASSERT_EQ(r.resolveFor("other_ui", "share.text").found.size(), std::size_t(2));
}

LOGOS_TEST(a_provides_entry_describes_its_result_like_its_params)
{
    IntentRegistry r;
    r.ingest("stamp_module", pkg("stamp_module", "core", json::array({json{
        {"intent", "stamp.make"},
        {"result", json::array({json{{"name", "stamp"}, {"type", "string"}, {"required", true}},
                                json{{"type", "number"}}})},
    }})), "", "");
    const auto& spec = r.resultSpecFor("stamp_module", "stamp.make");
    LOGOS_ASSERT_EQ(spec.size(), std::size_t(1));   // a field with no name is skipped
    LOGOS_ASSERT_EQ(spec[0].name, std::string("stamp"));
    LOGOS_ASSERT_EQ(spec[0].type, std::string("string"));
    LOGOS_ASSERT_TRUE(spec[0].required);
    LOGOS_ASSERT_TRUE(r.paramsSpecFor("stamp_module", "stamp.make").empty());
    LOGOS_ASSERT_TRUE(r.resultSpecFor("stamp_module", "other.intent").empty());
}

// A typo in a type isn't silent: the field goes unchecked, and the registry says so.
LOGOS_TEST(an_unknown_param_type_is_reported)
{
    IntentRegistry r;
    r.ingest("flag_module", pkg("flag_module", "core", json::array({json{
        {"intent", "flag.set"},
        {"params", json::array({json{{"name", "on"}, {"type", "boolean"}, {"required", true}},
                                json{{"name", "label"}, {"type", "string"}}})},
        {"result", json::array({json{{"name", "at"}, {"type", "int"}}})},
    }})), "", "");
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(
        r, "flag_module: 'flag.set' declares 'on' in 'params' as 'boolean', not one of string, number, bool"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "declares 'at' in 'result' as 'int'"));
    LOGOS_ASSERT_FALSE(hasDiagnosticContaining(r, "'label'"));
    const auto& params = r.paramsSpecFor("flag_module", "flag.set");
    LOGOS_ASSERT_EQ(params.size(), std::size_t(2));
    LOGOS_ASSERT_TRUE(params[0].required);
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
    const std::string good = dir.add("signer_ui", pkg("signer_ui", "core", intentList({"signing.approve"})));
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
    r.ingest("early_ui", pkg("early_ui", "core", intentList({"basecamp.apps.launch", "share.text"})), "", "");
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
    r.ingest("evil\x1b[2J_ui", pkg("evil\x1b[2J_ui", "core", intentList({"share.text"})), "", "");
    r.ingest("ok_ui", pkg("ok_ui", "core", intentList({"share.text"})), "", "");
    LOGOS_ASSERT_EQ(r.resolve("share.text").found.size(), std::size_t(1));
    for (const std::string& d : r.diagnostics()) LOGOS_ASSERT_TRUE(d.find('\x1b') == std::string::npos);
}

LOGOS_TEST(a_package_declares_at_most_64_entries)
{
    IntentRegistry r;
    json provides = json::array();
    for (int i = 0; i < 100; ++i) provides.push_back(json{{"intent", "x.a" + std::to_string(i)}});
    r.ingest("wide_ui", pkg("wide_ui", "core", provides), "", "");
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

// Names are identity: a package can't take one the shell or another package has.
LOGOS_TEST(a_package_cannot_take_the_shells_name_or_another_packages)
{
    IntentRegistry r;
    r.ingest("basecamp", pkg("basecamp", "core", intentList({"share.text"})), "", "");
    LOGOS_ASSERT_TRUE(r.declaresProvide("basecamp", "share.text"));
    r.declareShell("basecamp", "basecamp.", {}, {}, {}, "Basecamp", "");
    LOGOS_ASSERT_FALSE(r.declaresProvide("basecamp", "share.text"));
    LOGOS_ASSERT_TRUE(r.isShell("basecamp"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "the shell's name — the package is dropped"));

    r.ingest("sharer_ui", pkg("sharer_ui", "ui_qml", intentList({"share.text"})), "", "");
    r.ingest("sharer_ui", pkg("sharer_ui", "core", intentList({"share.text", "share.file"})), "", "");
    LOGOS_ASSERT_EQ(r.typeOf("sharer_ui"), std::string("ui_qml"));
    LOGOS_ASSERT_FALSE(r.declaresProvide("sharer_ui", "share.file"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "duplicate package name — first record wins"));
}

LOGOS_TEST(invalid_shell_and_link_names_are_not_registered)
{
    IntentRegistry r;
    r.declareShell("Base Camp", "basecamp.", {}, {}, {}, "", "");
    LOGOS_ASSERT_FALSE(r.isShell("Base Camp"));
    LOGOS_ASSERT_TRUE(r.shellName().empty());
    r.registerLinkRequester("bad/link", {});
    LOGOS_ASSERT_FALSE(r.isLinkRequester("bad/link"));

    r.declareShell("basecamp", "basecamp.", {"Bad"}, {"basecamp.settings.open", "nope"},
                   {"basecamp.apps.launch"}, "", "");
    r.registerLinkRequester("basecamp_link", {"Bad.web"});
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "shell: uses 'Bad'"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "shell: provides 'nope'"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "which the shell doesn't provide"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "link: web intent 'Bad.web'"));
    LOGOS_ASSERT_FALSE(r.isHandoff("basecamp", "basecamp.apps.launch"));
}

// A malformed manifest loses only what is malformed, and says so.
LOGOS_TEST(malformed_uses_and_provides_are_dropped_with_a_diagnostic)
{
    IntentRegistry r;
    json notLists = pkg("a_module", "core");
    notLists["uses"] = "share.text";
    notLists["provides"] = json{{"intent", "share.text"}};
    r.ingest("a_module", notLists, "", "");
    LOGOS_ASSERT_FALSE(r.declaresUse("a_module", "share.text"));
    LOGOS_ASSERT_FALSE(r.declaresProvide("a_module", "share.text"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "a_module: 'uses' is not a list"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "a_module: 'provides' is not a list"));

    json uses = json::array({"share.text", json{{"intent", "share.file"}}, json{{"intent", "share.file"}},
                             json{{"intent", "share.link"}, {"cardinality", "many"}}});
    for (int i = 0; i < 64; ++i) uses.push_back(json{{"intent", "bulk.i" + std::to_string(i)}});
    r.ingest("b_module", pkg("b_module", "core", json::array(), uses), "", "");
    LOGOS_ASSERT_FALSE(r.declaresUse("b_module", "share.text"));
    LOGOS_ASSERT_TRUE(r.declaresUse("b_module", "share.file"));
    LOGOS_ASSERT_TRUE(r.declaresUse("b_module", "share.link"));   // as 'single'
    LOGOS_ASSERT_TRUE(r.declaresUse("b_module", "bulk.i59"));
    LOGOS_ASSERT_FALSE(r.declaresUse("b_module", "bulk.i60"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "'uses' entry must be an object"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "'uses' declares 'share.file' twice"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "unsupported cardinality"));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "more than 64 'uses' entries"));

    json params = json::array();
    for (int i = 0; i < 40; ++i) params.push_back(json{{"name", "p" + std::to_string(i)}, {"type", "string"}});
    r.ingest("c_module", pkg("c_module", "core", json::array({json{{"intent", "share.text"}, {"params", params}}})), "", "");
    LOGOS_ASSERT_EQ(r.paramsSpecFor("c_module", "share.text").size(), std::size_t(32));
    LOGOS_ASSERT_TRUE(hasDiagnosticContaining(r, "more than 32 entries in 'params'"));
}

// Dev hosts lay dependencies out as symlinks (Basecamp's nix/devDeps.nix copies
// with `cp -rs`): a manifest reached through one is read like any other.
LOGOS_TEST(a_symlinked_manifest_is_read)
{
    test::TempPackages dir;
    const std::string real = dir.add("store_copy", pkg("linked_module", "core", intentList({"x.y"})));
    const std::string d = dir.add("linked_module", json::object());
    std::remove((d + "/metadata.json").c_str());
    LOGOS_ASSERT_EQ(symlink((real + "/metadata.json").c_str(), (d + "/metadata.json").c_str()), 0);
    IntentRegistry r;
    r.rebuild(loadPackages({InstalledPackage{"linked_module", d, "", ""}}));
    LOGOS_ASSERT_TRUE(r.declaresProvide("linked_module", "x.y"));
}

LOGOS_TEST(a_metadata_json_that_is_a_directory_is_refused)
{
    test::TempPackages dir;
    const std::string d = dir.add("odd_ui", json::object());
    std::remove((d + "/metadata.json").c_str());
    std::system(("mkdir '" + d + "/metadata.json'").c_str());
    const auto loaded = loadPackages({InstalledPackage{"odd_ui", d, "", ""}});
    LOGOS_ASSERT_EQ(loaded.size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(loaded[0].metadata.is_discarded());
    LOGOS_ASSERT_EQ(loaded[0].error, std::string("metadata.json is not a regular file"));
}
