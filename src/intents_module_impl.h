#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// intents_module — the intent broker as a core module, hosted by the shell.
//
// Basecamp starts it in Basecamp, the logoscore daemon for logosctl and CLI
// apps. Apps never load or list it; they declare its contract under
// interface_dependencies and bind it by name.
//
// The public methods below ARE the module's API (the builder derives the .lidl
// from this header). Every answer is a JSON object as a string. See
// docs/intents-module.md for the contract.
//
// NO trailing `// comments` on declaration lines (the parser needs a `;`).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <memory>
#include <string>

#include <logos_module_context.h>

namespace intents { class IntentsService; }
struct IntentsModuleState;

class IntentsModuleImpl : public LogosModuleContext {
public:
    IntentsModuleImpl();
    ~IntentsModuleImpl();

    // ── Requesters: app backends and core modules ───────────────────────
    // Ask for `intent`. Answers at once with { ok, handle, receipt }; the
    // outcome comes later: wait for `settled(handle)`, then call result().
    // `click_token` is what the app's view got from logos.clickToken(), or "".
    std::string request(const std::string& intent, const std::string& params_json, const std::string& click_token);

    // { ok, state: "pending" } or { ok, state: "settled", result: { ok, data, error } }.
    std::string result(const std::string& handle, const std::string& receipt);

    // Withdraw an open request, or drop a settled result.
    std::string cancel(const std::string& handle, const std::string& receipt);

    // ── Providers ───────────────────────────────────────────────────────
    // Answer a request delivered to this module's `deliver` method.
    std::string respond(const std::string& dispatch, bool ok, const std::string& data_json, const std::string& error);

    // ── Presenter: the frontend that started this module ────────────────
    // Once, by the host, before any app loads. Answers { ok, secret }.
    // options: { "attention": "interactive" | "immediate" | "headless" }.
    std::string claim(const std::string& options_json);

    std::string next_work(const std::string& secret);

    std::string answer(const std::string& secret, const std::string& work_id, const std::string& answer_json);

    std::string register_click_token(const std::string& secret, const std::string& token, const std::string& app, const std::string& intent);

    std::string backend_registered(const std::string& secret, const std::string& app, const std::string& module);

    std::string backend_unregistered(const std::string& secret, const std::string& app);

    // [{ name, installDir, displayName?, iconSource? }]
    std::string set_installed(const std::string& secret, const std::string& packages_json);

    // { byName: { pkg: [intent] }, webByName: { pkg: [intent] } }
    std::string set_installable(const std::string& secret, const std::string& catalog_json);

    // { name, reservedPrefix, uses, provides, handoff, displayName, iconSource,
    //   restricted: { intent: [requester] }, link: { name, webIntents } }
    std::string declare_shell(const std::string& secret, const std::string& shell_json);

    std::string configure_provider(const std::string& secret, const std::string& intent, const std::string& provider);

    std::string shell_request(const std::string& secret, const std::string& intent, const std::string& params_json, bool as_link);

logos_events:
    // A request settled. The handle only: the result is pulled with the receipt.
    void settled(const std::string& handle);

    // The presenter has new work. A counter only.
    void work_available(uint64_t counter);

protected:
    LogosShutdown aboutToUnload() override;

private:
    std::unique_ptr<IntentsModuleState> m_state;
};
