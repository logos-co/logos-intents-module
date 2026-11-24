#pragma once

// Resolve an intent to a provider, get a person's attention when one is needed,
// deliver, and keep the result for the requester alone. It knows modules and
// intents only: the presenter decides how and when to reach the person.
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

struct BrokerConfig {
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
    std::size_t maxOpenBytesTotal = 64 * 1024 * 1024;
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

    // Until a presenter has claimed, nobody can be asked: every request is unavailable.
    void setClaimed() { m_claimed = true; }

    // ── Requesters ──────────────────────────────────────────────────────
    // Spends one of the requester's request tokens; false when it has none left.
    bool admit(const std::string& requester, std::int64_t now);

    // `requester` is the module's attested name. Its request waits as `pending`
    // until the presenter opens it. `fromPresenter` marks the shell's and links'
    // own requests, which are open already; nothing else can set it, so a module
    // can't pass for them by name.
    Ticket submit(const std::string& requester, const std::string& intent, const json& params,
                  bool fromPresenter, std::int64_t now);
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
    // Only the module the dispatch went to.
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
    // After the registry changed: a request its requester no longer declares,
    // or may no longer make, ends.
    void registryChanged(std::int64_t now);
    std::vector<Effect> takeEffects();

    std::size_t openCount() const { return m_open.size(); }
    std::size_t resultBytes() const { return m_resultBytesTotal; }

private:
    enum class Phase { Accepted, Pending, AwaitingChoice, Dispatched };
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
        bool opened = false;   // by the presenter; the shell's own requests start open
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
        std::vector<IntentRegistry::ParamSpec> resultSpec;   // as it was when delivered
        std::string providerModule;    // where it was delivered; empty for the shell
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

    struct Bucket {
        double tokens = 0;
        std::int64_t at = 0;
    };

    enum class WorkKind { Choose, Pending, ShellDeliver };
    struct WorkTarget {
        WorkKind kind;
        std::string target;   // dispatch id
    };

    struct WorkItem {
        // What a full queue may drop: a notice first, then an outcome the
        // presenter would only read. An item someone must answer, never.
        enum class Keep { Notice, Outcome, Answer };
        std::string id;
        Keep keep = Keep::Answer;
        json item;
    };

    // Over its limits: answered only after the floor, from outside the open requests.
    struct Refusal {
        std::string requester;
        std::string receipt;
        std::string code;
        std::int64_t due = 0;
        bool fromPresenter = false;
    };

    void start(const std::string& id, std::int64_t now);
    void park(const std::string& id, std::int64_t now);
    void offerInstall(const std::string& intent, std::int64_t now);
    void presentChoice(const std::string& id, std::int64_t now);
    bool showChoice(const std::string& id, std::int64_t now);
    bool stillEligible(const Request& r, const std::string& provider) const;
    bool stillProvides(const Request& r, const std::string& provider, std::uint64_t revision) const;
    void presentNextChoice(std::int64_t now);
    bool aChoiceIsOnScreen() const;
    void selectProvider(const std::string& id, const std::string& provider, std::int64_t now);
    void dispatch(const std::string& id, std::int64_t now);
    std::string specViolation(const std::string& provider, const std::string& intent,
                              const json& params) const;
    static std::string resultViolation(const std::vector<IntentRegistry::ParamSpec>& spec,
                                       const json& data);
    void failWithFloor(const std::string& id, const std::string& code, Path path, std::int64_t now);
    void finish(const std::string& id, bool ok, const json& data, const std::string& error,
                Path path, std::int64_t now);
    void keepResult(const std::string& requester, const std::string& handle, const std::string& receipt,
                    json envelope, std::int64_t now);
    void holdRefusal(const Request& r, const std::string& code, std::int64_t now);
    void settleRefusal(const std::string& handle, const Refusal& refusal, std::int64_t now);
    void eraseSettled(std::map<std::string, Settled>::iterator it);
    std::string pushWork(json item, std::optional<WorkTarget> target = std::nullopt);
    void dropWorkTarget(const std::string& workId);
    Request* find(const std::string& id);
    int countOpen(const std::string& requester, std::optional<Phase> phase = std::nullopt) const;
    std::size_t openBytes(const std::string& requester) const;
    std::size_t openBytesTotal() const;
    // False, ending the request, when its requester may no longer make it.
    bool requesterStillMay(const std::string& id, std::int64_t now);

    IntentRegistry& m_registry;
    IdFn m_mintId;
    BrokerConfig m_config;
    bool m_claimed = false;

    std::map<std::string, Request> m_open;            // dispatch id -> request
    std::map<std::string, std::string> m_byHandle;    // handle -> dispatch id
    std::map<std::string, Settled> m_settled;         // handle -> result
    std::map<std::string, Refusal> m_refused;         // handle -> refusal held to its floor
    std::size_t m_resultBytesTotal = 0;
    std::map<std::string, Bucket> m_buckets;          // requester -> request tokens
    std::map<std::string, std::int64_t> m_lastOffer;  // intent -> last install offer

    std::vector<WorkItem> m_work;
    std::map<std::string, WorkTarget> m_workTargets;
    std::uint64_t m_workCounter = 0;
    bool m_workSignalled = false;
    std::vector<Effect> m_effects;
};

} // namespace intents
