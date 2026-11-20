#pragma once

// Resolve an intent to a provider, get a person's attention when one is needed,
// deliver, and keep the result for the requester alone. Ported from
// logos-basecamp's IntentBroker.
//
// No threads, no I/O and no clock of its own: every call takes `now`, and what
// the broker needs from the world comes back as effects (takeEffects()) and
// presenter work (takeWork()). The caller holds the lock, runs the effects after
// releasing it, and calls tick() for deadlines.

#include "intent_registry.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace intents {

using json = nlohmann::json;

// How the presenter reaches a person.
enum class Attention {
    // No presenter: only a provider the host configured is used, without asking anyone.
    Headless,
    // A terminal presenter the person opened for this: everything is shown at once.
    Immediate,
    // A desktop shell: only a request with a click behind it may pop up; the
    // rest wait in the pending list until the person opens them.
    Interactive,
};

struct BrokerConfig {
    std::int64_t activationMs = 45000;          // loading a provider app
    std::int64_t responseMs = 20000;            // a provider that never took the delivery
    std::int64_t errorFloorMs = 400;            // the earliest a refusal settles
    std::int64_t backstopMs = 10 * 60 * 1000;   // a choice on screen, an accepted delivery
    std::int64_t pendingTtlMs = 30 * 60 * 1000; // the pending list, a choice waiting its turn
    std::int64_t resultTtlMs = 10 * 60 * 1000;  // a settled result nobody pulled
    std::int64_t offerCooldownMs = 60 * 1000;   // between install offers for one intent

    // Per requester.
    int maxOpenPerRequester = 16;
    int maxPendingPerRequester = 4;
    int maxResultsPerRequester = 64;
    std::size_t maxOpenBytesPerRequester = 4 * 1024 * 1024;
    std::size_t maxResultBytesPerRequester = 4 * 1024 * 1024;
    double rateBurst = 20;            // requests at once
    double ratePerSecond = 5;         // sustained

    // Across everyone.
    std::size_t maxOpenTotal = 1024;
    std::size_t maxResultBytesTotal = 64 * 1024 * 1024;
};

struct Effect {
    enum class Kind { Deliver, Settled, WorkAvailable };
    Kind kind = Kind::Settled;
    std::string module;       // Deliver: the provider module
    std::string dispatchId;   // Deliver
    std::string intent;       // Deliver
    json params;              // Deliver
    std::string requester;    // Deliver
    std::string handle;       // Settled
    std::uint64_t counter = 0;   // WorkAvailable
};

class IntentBroker {
public:
    using IdFn = std::function<std::string()>;

    struct Ticket {
        std::string handle;
        std::string receipt;
    };

    // `mintId` makes handles, receipts and dispatch ids, so it must be
    // unguessable in production.
    IntentBroker(IntentRegistry& registry, IdFn mintId, BrokerConfig config = {});

    void setAttention(Attention attention) { m_attention = attention; }
    Attention attention() const { return m_attention; }

    // Headless only: the provider used for `intent`. Empty clears it.
    void configureProvider(const std::string& intent, const std::string& provider);

    // ── App backends, as the presenter registered them ─────────────────
    // Refused when the app isn't an installed ui_qml package, when `module` is
    // an installed package's name, or when it already serves another app.
    bool registerBackend(const std::string& app, const std::string& module, std::int64_t now);
    void unregisterBackend(const std::string& app, std::int64_t now);
    // After the installed set or the shell changes: drops every registration
    // registerBackend() would now refuse.
    void dropStaleBackends(std::int64_t now);
    std::string appForBackend(const std::string& module) const;

    // ── Requesters ──────────────────────────────────────────────────────
    // Spends one of the requester's request tokens; false when it has none left.
    bool admit(const std::string& requester, std::int64_t now);

    // `requester` is the attested name: an app (via its registered backend) or
    // a core module. `fromPresenter` marks the shell's and links' own requests;
    // nothing else can set it, so a module can't pass for them by name.
    Ticket submit(const std::string& requester, const std::string& intent, const json& params,
                  bool clicked, bool fromPresenter, std::int64_t now);
    // { ok:false, error:"unknown_handle" } | { ok, state:"pending" }
    // | { ok, state:"settled", result:{ok,data,error} }
    json result(const std::string& requester, const std::string& handle,
                const std::string& receipt) const;
    // Withdraws an open request, or drops a settled result.
    bool cancel(const std::string& requester, const std::string& handle,
                const std::string& receipt, std::int64_t now);

    // ── Providers ───────────────────────────────────────────────────────
    // Whether `dispatchId` is waiting for an answer from `module`: a cheap check
    // before anything the module sent is parsed.
    bool awaitsResponseFrom(const std::string& module, const std::string& dispatchId) const;
    // Only the module the dispatch went to, and only while it is still the
    // app's registered backend.
    bool respond(const std::string& responderModule, const std::string& dispatchId,
                 bool ok, const json& data, const std::string& error, std::int64_t now);
    // How the Deliver effect went: not reached, reached but not taken, or taken.
    void deliveryOutcome(const std::string& dispatchId, bool reached, bool accepted,
                         std::int64_t now);

    // ── Presenter ───────────────────────────────────────────────────────
    // Drains the work queue.
    json takeWork();
    // Answer a work item by its id. False for an unknown id or a wrong answer;
    // nothing changes then.
    bool answer(const std::string& workId, const json& answer, std::int64_t now);

    void tick(std::int64_t now);
    std::vector<Effect> takeEffects();

    std::size_t openCount() const { return m_open.size(); }
    std::size_t resultBytes() const { return m_resultBytesTotal; }

private:
    enum class Phase { Accepted, Pending, AwaitingChoice, Activating, Dispatched };
    enum class Path { ProviderResponse, Deadline, EndpointGone, Abandoned, Rejected };

    struct Floor {
        std::string code;
        Path path = Path::Rejected;
        std::int64_t at = 0;
    };

    struct Request {
        std::string handle;
        std::string receipt;
        std::string dispatchId;
        std::string requester;
        std::string intent;
        json params;
        std::size_t paramsBytes = 0;
        bool clicked = false;
        bool fromPresenter = false;

        Phase phase = Phase::Accepted;
        std::int64_t acceptedAt = 0;
        std::int64_t phaseSince = 0;
        std::optional<Floor> floor;

        bool choicePresented = false;
        json choiceProviders = json::array();
        std::map<std::string, std::uint64_t> choiceRevisions;   // provider -> revision shown
        std::string choiceWorkId;
        std::string pendingWorkId;
        std::string deliverWorkId;

        std::string provider;          // the registry name
        std::string providerModule;    // where it was delivered; empty for the shell
        std::uint64_t providerGeneration = 0;
        bool providerAccepted = false;
        bool didNavigate = false;
        bool isHandoff = false;
    };

    struct Settled {
        std::string requester;
        std::string receipt;
        json envelope;
        std::int64_t at = 0;
        std::size_t bytes = 0;
    };

    struct Backend {
        std::string module;
        std::uint64_t generation = 0;
    };

    struct Bucket {
        double tokens = 0;
        std::int64_t at = 0;
    };

    enum class WorkKind { Choose, Activate, Pending, ShellDeliver };
    struct WorkTarget {
        WorkKind kind;
        std::string target;   // dispatch id, or the app for Activate
    };

    struct WorkItem {
        std::string id;
        bool notice = false;   // nothing waits on it: safe to drop
        json item;
    };

    struct Activation {
        std::vector<std::string> ids;   // the requests waiting on the app
        std::string workId;             // its `activate`, until the presenter answers
    };

    std::string backendRefusal(const std::string& app, const std::string& module) const;
    void start(const std::string& id, std::int64_t now);
    void park(const std::string& id, std::int64_t now);
    void offerInstall(const std::string& requester, const std::string& intent, std::int64_t now);
    void presentChoice(const std::string& id, std::int64_t now);
    bool showChoice(const std::string& id, std::int64_t now);
    bool stillEligible(const Request& r, const std::string& provider) const;
    void presentNextChoice(std::int64_t now);
    bool aChoiceIsOnScreen() const;
    void selectProvider(const std::string& id, const std::string& provider, std::int64_t now);
    void dispatch(const std::string& id, std::int64_t now);
    std::string specViolation(const std::string& provider, const std::string& intent,
                              const json& params) const;
    void failWithFloor(const std::string& id, const std::string& code, Path path, std::int64_t now);
    void finish(const std::string& id, bool ok, const json& data, const std::string& error,
                Path path, std::int64_t now);
    void keepResult(const Request& r, json envelope, std::int64_t now);
    void eraseSettled(std::map<std::string, Settled>::iterator it);
    void drainActivation(const std::string& app, bool ready, std::int64_t now);
    void endActivation(const std::string& app);
    std::string pushWork(json item, std::optional<WorkTarget> target = std::nullopt);
    void dropWorkTarget(const std::string& workId);
    Request* find(const std::string& id);
    int countOpen(const std::string& requester, std::optional<Phase> phase = std::nullopt) const;
    std::size_t openBytes(const std::string& requester) const;

    IntentRegistry& m_registry;
    IdFn m_mintId;
    BrokerConfig m_config;
    Attention m_attention = Attention::Headless;

    std::map<std::string, Request> m_open;            // dispatch id -> request
    std::map<std::string, std::string> m_byHandle;    // handle -> dispatch id
    std::map<std::string, Settled> m_settled;         // handle -> result
    std::size_t m_resultBytesTotal = 0;
    std::map<std::string, std::string> m_configured;  // intent -> provider (headless)
    std::map<std::string, Backend> m_backends;        // app -> backend
    std::uint64_t m_backendGeneration = 0;
    std::map<std::string, Activation> m_activation;   // app -> its activation
    std::map<std::string, Bucket> m_buckets;          // requester -> request tokens
    std::map<std::string, std::int64_t> m_lastOffer;  // intent -> last install offer

    std::vector<WorkItem> m_work;
    std::map<std::string, WorkTarget> m_workTargets;
    std::uint64_t m_workCounter = 0;
    bool m_workSignalled = false;
    std::vector<Effect> m_effects;
};

} // namespace intents
