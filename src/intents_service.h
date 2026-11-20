#pragma once

// Everything intents_module does, behind its public methods: who is calling,
// the presenter's secret, click tokens, and the lock around the broker.
//
// Kept apart from the module class so tests can drive it with a fake clock and
// a fake transport, and so the module's header — which IS its API — stays the
// list of methods and nothing else.

#include "intent_broker.h"
#include "intent_registry.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace intents {

using json = nlohmann::json;

class IntentsService {
public:
    // How the service reaches out. Called without the lock held.
    struct Outbound {
        // Call `deliver` on `module`. `done(reached, reply)` must be called
        // exactly once, from any thread.
        std::function<void(const std::string& module, const json& args,
                           std::function<void(bool reached, const json& reply)> done)> deliver;
        std::function<void(const std::string& handle)> settled;
        std::function<void(std::uint64_t counter)> workAvailable;
    };
    using Clock = std::function<std::int64_t()>;
    using IdFn = std::function<std::string()>;

    IntentsService(Outbound outbound, Clock clock, IdFn mintId, BrokerConfig config = {});

    // ── Requesters: app backends and core modules ───────────────────────
    std::string request(const std::string& intent, const std::string& paramsJson,
                        const std::string& clickToken);
    std::string result(const std::string& handle, const std::string& receipt);
    std::string cancel(const std::string& handle, const std::string& receipt);

    // ── Providers: the module a request was delivered to ───────────────
    std::string respond(const std::string& dispatchId, bool ok, const std::string& dataJson,
                        const std::string& error);

    // ── Presenter: the host, once, then with the secret ────────────────
    std::string claim(const std::string& optionsJson);
    std::string nextWork(const std::string& secret);
    std::string answer(const std::string& secret, const std::string& workId,
                       const std::string& answerJson);
    std::string registerClickToken(const std::string& secret, const std::string& token,
                                   const std::string& app, const std::string& intent);
    std::string backendRegistered(const std::string& secret, const std::string& app,
                                  const std::string& module);
    std::string backendUnregistered(const std::string& secret, const std::string& app);
    std::string setInstalled(const std::string& secret, const std::string& packagesJson);
    std::string setInstallable(const std::string& secret, const std::string& catalogJson);
    std::string declareShell(const std::string& secret, const std::string& shellJson);
    std::string configureProvider(const std::string& secret, const std::string& intent,
                                  const std::string& provider);
    std::string shellRequest(const std::string& secret, const std::string& intent,
                             const std::string& paramsJson, bool asLink);

    // Deadlines, and `settled` events held back until their request has returned.
    void tick();

    static constexpr std::int64_t kClickTokenMs = 2000;

private:
    struct ClickToken {
        std::string token;
        std::string intent;   // empty: any intent
        std::int64_t expires = 0;
    };

    // Who is calling, as a requester: an app through its registered backend,
    // or a module under its own name. Empty for anything else, including a
    // module that has the name of the shell, the link requester or an app.
    struct Requester {
        std::string name;
        bool viaBackend = false;
    };
    Requester requesterForCaller();

    // Host caller holding the secret. Logs and returns false otherwise.
    bool presenterCall(const std::string& secret, const char* method);
    json declareShellLocked(const std::string& shellJson);
    bool consumeClickToken(const Requester& who, const std::string& token,
                           const std::string& intent, std::int64_t now);
    // A refusal worth logging, at most once a minute per caller.
    void logRefusal(const std::string& caller, const std::string& line, std::int64_t now);

    // Hand the broker's effects to the world. Called WITHOUT the lock.
    void run(std::vector<Effect> effects);

    Outbound m_out;
    Clock m_clock;
    IdFn m_mintId;
    // Delivery replies can arrive after this object is gone; they check this first.
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);

    std::mutex m_mutex;
    IntentRegistry m_registry;
    IntentBroker m_broker;
    std::string m_secret;
    bool m_claimed = false;
    std::map<std::string, ClickToken> m_clickTokens;   // app -> its one live token
    std::vector<Effect> m_deferredSettled;
    std::map<std::string, std::int64_t> m_lastRefusalLog;
};

} // namespace intents
