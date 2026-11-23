#include "intent_broker.h"

#include "intent_rules.h"

#include <algorithm>
#include <cstdio>

namespace intents {

namespace {

// Past this, the oldest item nothing waits on goes. Items someone must answer
// stay: there are never more of them than open requests and apps being raised.
constexpr std::size_t kWorkLimit = 1000;

void logLine(const std::string& line)
{
    // Names and codes only, never payloads, and one line whatever they contain.
    std::fprintf(stderr, "[intents_module] %s\n", forLog(line, 400).c_str());
}

const char* pathName(int path)
{
    switch (path) {
    case 0: return "provider";
    case 1: return "deadline";
    case 2: return "gone";
    case 3: return "abandoned";
    default: return "rejected";
    }
}

// Lenient about number width: a caller has no reason to tell 1 from 1.0.
bool matchesDeclaredType(const json& v, const std::string& declared)
{
    if (declared == "string") return v.is_string();
    if (declared == "number") return v.is_number();
    if (declared == "bool") return v.is_boolean();
    if (declared == "object") return v.is_object();
    if (declared == "array") return v.is_array();
    // A type this module doesn't know: the provider may be ahead of us.
    return true;
}

bool sameSecret(const std::string& a, const std::string& b)
{
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace

IntentBroker::IntentBroker(IntentRegistry& registry, IdFn mintId, BrokerConfig config)
    : m_registry(registry)
    , m_mintId(std::move(mintId))
    , m_config(config)
{
}

IntentBroker::Request* IntentBroker::find(const std::string& id)
{
    const auto it = m_open.find(id);
    return it == m_open.end() ? nullptr : &it->second;
}

int IntentBroker::countOpen(const std::string& requester, std::optional<Phase> phase) const
{
    int n = 0;
    for (const auto& [id, r] : m_open)
        if (r.requester == requester && (!phase || r.phase == *phase)) ++n;
    return n;
}

std::size_t IntentBroker::openBytes(const std::string& requester) const
{
    std::size_t n = 0;
    for (const auto& [id, r] : m_open)
        if (r.requester == requester) n += r.paramsBytes;
    return n;
}

std::string IntentBroker::pushWork(json item, std::optional<WorkTarget> target)
{
    const std::string id = m_mintId();
    item["id"] = id;
    const std::string kind = stringAt(item, "kind");
    using Keep = WorkItem::Keep;
    const Keep keep = kind == "present" || kind == "offer_install" || kind == "completed" ? Keep::Notice
                    : kind == "result" || kind == "dismiss" ? Keep::Outcome
                    : Keep::Answer;
    if (m_work.size() >= kWorkLimit) {
        auto oldest = std::find_if(m_work.begin(), m_work.end(),
                                   [](const WorkItem& w) { return w.keep == Keep::Notice; });
        if (oldest == m_work.end())
            oldest = std::find_if(m_work.begin(), m_work.end(),
                                  [](const WorkItem& w) { return w.keep == Keep::Outcome; });
        if (oldest != m_work.end()) m_work.erase(oldest);
    }
    m_work.push_back(WorkItem{id, keep, std::move(item)});
    if (target) m_workTargets[id] = *target;
    ++m_workCounter;
    m_workSignalled = true;
    return id;
}

void IntentBroker::dropWorkTarget(const std::string& workId)
{
    if (workId.empty()) return;
    m_workTargets.erase(workId);
    // Not pulled yet: take it back. Pulled: tell the presenter to take it down.
    const auto queued = std::find_if(m_work.begin(), m_work.end(),
                                     [&](const WorkItem& w) { return w.id == workId; });
    if (queued != m_work.end()) {
        m_work.erase(queued);
        return;
    }
    pushWork(json{{"kind", "dismiss"}, {"work", workId}});
}

json IntentBroker::takeWork()
{
    json out = json::array();
    for (WorkItem& w : m_work) out.push_back(std::move(w.item));
    m_work.clear();
    return out;
}

std::vector<Effect> IntentBroker::takeEffects()
{
    std::vector<Effect> out = std::move(m_effects);
    m_effects.clear();
    if (m_workSignalled) {
        Effect e;
        e.kind = Effect::Kind::WorkAvailable;
        e.counter = m_workCounter;
        out.push_back(e);
        m_workSignalled = false;
    }
    return out;
}

// ── Requests ─────────────────────────────────────────────────────────────────

bool IntentBroker::admit(const std::string& requester, std::int64_t now)
{
    auto [it, fresh] = m_buckets.try_emplace(requester, Bucket{m_config.rateBurst, now});
    Bucket& b = it->second;
    if (!fresh && now > b.at) {
        b.tokens = std::min(m_config.rateBurst,
                            b.tokens + static_cast<double>(now - b.at) * m_config.ratePerSecond / 1000.0);
        b.at = now;
    }
    if (b.tokens < 1) return false;
    b.tokens -= 1;
    return true;
}

IntentBroker::Ticket IntentBroker::submit(const std::string& requester, const std::string& intent,
                                          const json& params, bool fromPresenter, std::int64_t now)
{
    Request r;
    r.handle = m_mintId();
    r.receipt = m_mintId();
    r.dispatchId = m_mintId();
    r.requester = requester;
    r.intent = intent;
    r.opened = fromPresenter;
    r.fromPresenter = fromPresenter;
    r.acceptedAt = now;
    r.phaseSince = now;

    const bool payloadOk = isCanonicalParams(params);
    const std::size_t bytes = payloadOk ? approxBytes(params) : 0;
    const bool withinLimit = countOpen(requester) < m_config.maxOpenPerRequester
        && m_open.size() < m_config.maxOpenTotal
        && openBytes(requester) + bytes <= m_config.maxOpenBytesPerRequester;
    const Ticket ticket{r.handle, r.receipt};
    if (!withinLimit) {
        logLine("refused " + forLog(intent, 64) + " from " + requester + ": over its open-request limits");
        holdRefusal(r, err::failed, now);
        return ticket;
    }
    if (payloadOk) {
        r.params = params;
        r.paramsBytes = bytes;
    }

    const std::string id = r.dispatchId;
    m_byHandle[r.handle] = id;
    m_open.emplace(id, std::move(r));

    // Floored: a provider may mint bad_request too, so an instant answer would
    // say no provider was consulted.
    if (!payloadOk) {
        logLine("refused " + forLog(intent, 64) + " from " + requester + ": params are not plain data");
        failWithFloor(id, err::badRequest, Path::Rejected, now);
    } else {
        start(id, now);
    }
    return ticket;
}

// Kept apart from the open requests, so their limit holds while the refusal
// waits out its floor. Past as many again, it settles at once.
void IntentBroker::holdRefusal(const Request& r, const std::string& code, std::int64_t now)
{
    const Refusal refusal{r.requester, r.receipt, code, r.acceptedAt + m_config.errorFloorMs, r.fromPresenter};
    if (now >= refusal.due || m_refused.size() >= m_config.maxOpenTotal) {
        settleRefusal(r.handle, refusal, now);
        return;
    }
    m_refused[r.handle] = refusal;
}

void IntentBroker::settleRefusal(const std::string& handle, const Refusal& refusal, std::int64_t now)
{
    json envelope = makeEnvelope(false, json(), refusal.code);
    if (refusal.fromPresenter) {
        pushWork(json{{"kind", "result"}, {"handle", handle}, {"result", std::move(envelope)}});
        return;
    }
    keepResult(refusal.requester, handle, refusal.receipt, std::move(envelope), now);
    Effect e;
    e.kind = Effect::Kind::Settled;
    e.handle = handle;
    m_effects.push_back(std::move(e));
}

void IntentBroker::start(const std::string& id, std::int64_t now)
{
    Request* r = find(id);
    if (!r) return;
    const std::string intent = r->intent;
    const std::string requester = r->requester;

    // Not floored: a fact about the caller's own manifest, not about what's installed.
    if (!isValidName(intent) || !m_registry.declaresUse(requester, intent)) {
        finish(id, false, json(), err::notDeclared, Path::Rejected, now);
        return;
    }

    // Restrictions are the shell's published policy, not install state.
    if (!m_registry.requesterAllowed(intent, requester)) {
        logLine("refused " + intent + " from " + requester + ": not a permitted requester");
        failWithFloor(id, err::unavailable, Path::Rejected, now);
        return;
    }

    if (!m_claimed) {
        failWithFloor(id, err::unavailable, Path::Rejected, now);
        return;
    }

    // Asking for its own capability is internal navigation: no boundary, no person.
    if (m_registry.declaresProvide(requester, intent)) {
        selectProvider(id, requester, now);
        return;
    }

    // Into the pending list before anything is resolved: whether a provider
    // exists is only decided once the presenter opens it.
    if (!r->opened) {
        park(id, now);
        return;
    }

    const IntentRegistry::Resolution resolution = m_registry.resolveFor(requester, intent);
    if (resolution.status == IntentRegistry::Status::None) {
        offerInstall(intent, now);
        failWithFloor(id, err::unavailable, Path::Rejected, now);
        return;
    }

    // One provider isn't a reason to skip the person, even for the shell's own
    // requests. Only the shell as provider is exempt: a dialog answering itself.
    if (resolution.status == IntentRegistry::Status::Ok) {
        const std::string only = resolution.found.front().moduleName;
        if (m_registry.isShell(only)) {
            selectProvider(id, only, now);
            return;
        }
    }

    presentChoice(id, now);
}

void IntentBroker::park(const std::string& id, std::int64_t now)
{
    Request* r = find(id);
    if (!r) return;
    if (countOpen(r->requester, Phase::Pending) >= m_config.maxPendingPerRequester) {
        logLine("refused " + r->intent + " from " + r->requester + ": pending list full");
        failWithFloor(id, err::unavailable, Path::Rejected, now);
        return;
    }
    r->phase = Phase::Pending;
    r->phaseSince = now;
    r->pendingWorkId = pushWork(json{{"kind", "pending"},
                                     {"dispatch", id},
                                     {"intent", r->intent},
                                     {"requester", displaySafe(r->requester)}},
                                WorkTarget{WorkKind::Pending, id});
}

void IntentBroker::offerInstall(const std::string& intent, std::int64_t now)
{
    // The presenter, which has the catalogue, may offer an install; the
    // requester learns nothing either way. One offer per intent a minute:
    // requests can't become a stream of prompts.
    const auto last = m_lastOffer.find(intent);
    if (last != m_lastOffer.end() && now - last->second < m_config.offerCooldownMs) return;
    m_lastOffer[intent] = now;
    pushWork(json{{"kind", "offer_install"}, {"intent", intent}});
}

bool IntentBroker::aChoiceIsOnScreen() const
{
    for (const auto& [id, r] : m_open)
        if (r.choicePresented && r.phase == Phase::AwaitingChoice) return true;
    return false;
}

void IntentBroker::presentChoice(const std::string& id, std::int64_t now)
{
    Request* r = find(id);
    if (!r) return;
    r->phase = Phase::AwaitingChoice;
    r->phaseSince = now;

    // Queue, never re-point a chooser that is showing: a click meant for the
    // request the person read would answer the one that just arrived.
    if (aChoiceIsOnScreen()) return;
    showChoice(id, now);
}

// Resolved when shown, not when queued: the person sees what is there now.
// False when nothing is, and the request fails instead.
bool IntentBroker::showChoice(const std::string& id, std::int64_t now)
{
    Request* r = find(id);
    if (!r) return false;
    const IntentRegistry::Resolution current = m_registry.resolveFor(r->requester, r->intent);
    if (current.found.empty() || !m_registry.requesterAllowed(r->intent, r->requester)) {
        failWithFloor(id, err::unavailable, Path::Rejected, now);
        return false;
    }
    json providers = json::array();
    r->choiceRevisions.clear();
    for (const IntentRegistry::ProviderEntry& e : current.found) {
        providers.push_back(json{{"module", e.moduleName},
                                 {"displayName", displaySafe(e.displayName)},
                                 {"iconSource", e.iconSource}});
        r->choiceRevisions[e.moduleName] = m_registry.revisionOf(e.moduleName);
    }
    r->choiceProviders = providers;
    r->choicePresented = true;
    r->phaseSince = now;   // the person's time starts when it's shown
    r->choiceWorkId = pushWork(json{{"kind", "choose"},
                                    {"dispatch", id},
                                    {"intent", r->intent},
                                    {"requester", displaySafe(r->requester)},
                                    {"providers", providers}},
                               WorkTarget{WorkKind::Choose, id});
    return true;
}

void IntentBroker::presentNextChoice(std::int64_t now)
{
    while (!aChoiceIsOnScreen()) {
        // Oldest first, so which request the person answers doesn't depend on map order.
        const Request* oldest = nullptr;
        for (const auto& [id, r] : m_open) {
            if (r.phase != Phase::AwaitingChoice || r.choicePresented || r.floor) continue;
            if (!oldest || r.acceptedAt < oldest->acceptedAt) oldest = &r;
        }
        if (!oldest) return;
        const std::string id = oldest->dispatchId;
        if (showChoice(id, now)) return;
    }
}

// The provider as the person was shown it.
bool IntentBroker::stillEligible(const Request& r, const std::string& provider) const
{
    const auto shown = r.choiceRevisions.find(provider);
    return shown != r.choiceRevisions.end() && stillProvides(r, provider, shown->second);
}

// Still the package at `revision`, the requester still allowed, and the
// provider still reachable by it.
bool IntentBroker::stillProvides(const Request& r, const std::string& provider,
                                 std::uint64_t revision) const
{
    if (m_registry.revisionOf(provider) != revision) return false;
    if (!m_registry.requesterAllowed(r.intent, r.requester)) return false;
    if (provider == r.requester) return m_registry.declaresProvide(provider, r.intent);
    const IntentRegistry::Resolution current = m_registry.resolveFor(r.requester, r.intent);
    return std::any_of(current.found.begin(), current.found.end(),
                       [&](const IntentRegistry::ProviderEntry& e) { return e.moduleName == provider; });
}

void IntentBroker::selectProvider(const std::string& id, const std::string& provider,
                                  std::int64_t now)
{
    Request* r = find(id);
    if (!r) return;
    r->provider = provider;
    if (r->choicePresented) {
        r->choicePresented = false;
        dropWorkTarget(r->choiceWorkId);
        r->choiceWorkId.clear();
    }

    const std::string type = m_registry.typeOf(provider);
    if (type == "shell") {
        r->providerModule.clear();
        dispatch(id, now);
    } else if (type == "core") {
        r->providerModule = provider;
        dispatch(id, now);
    } else {
        failWithFloor(id, err::unavailable, Path::Rejected, now);
    }

    // The chooser is free once a choice is made, not when the request ends.
    presentNextChoice(now);
}

std::string IntentBroker::specViolation(const std::string& provider, const std::string& intent,
                                        const json& params) const
{
    for (const IntentRegistry::ParamSpec& p : m_registry.paramsSpecFor(provider, intent)) {
        const auto value = params.find(p.name);
        if (value == params.end()) {
            if (p.required) return "missing required parameter '" + forLog(p.name, 64) + "'";
            continue;
        }
        if (!p.type.empty() && !matchesDeclaredType(*value, p.type))
            return "parameter '" + forLog(p.name, 64) + "' should be " + forLog(p.type, 32);
    }
    // Undescribed params pass: a newer caller mustn't break on an older description.
    return {};
}

// The answer against the provider's declared `result`, as params are checked
// against `params`; nothing declared, nothing checked.
std::string IntentBroker::resultViolation(const std::vector<IntentRegistry::ParamSpec>& spec,
                                          const json& data)
{
    if (spec.empty()) return {};
    if (!data.is_object()) return "the result is not an object";
    for (const IntentRegistry::ParamSpec& field : spec) {
        const auto value = data.find(field.name);
        if (value == data.end()) {
            if (field.required) return "missing required field '" + forLog(field.name, 64) + "'";
            continue;
        }
        if (!field.type.empty() && !matchesDeclaredType(*value, field.type))
            return "field '" + forLog(field.name, 64) + "' should be " + forLog(field.type, 32);
    }
    return {};
}

void IntentBroker::dispatch(const std::string& id, std::int64_t now)
{
    Request* r = find(id);
    if (!r) return;

    const std::string violation = specViolation(r->provider, r->intent, r->params);
    if (!violation.empty()) {
        logLine("refused " + r->intent + " from " + r->requester + ": " + violation
                + " (as declared by " + r->provider + ")");
        failWithFloor(id, err::badRequest, Path::Rejected, now);
        return;
    }

    r->phase = Phase::Dispatched;
    r->phaseSince = now;
    // Snapshot: a rebuild mustn't change the navigation of a request in flight,
    // nor what its answer is checked against.
    r->isHandoff = m_registry.isHandoff(r->provider, r->intent);
    r->resultSpec = m_registry.resultSpecFor(r->provider, r->intent);

    if (m_registry.isShell(r->provider)) {
        r->providerAccepted = true;
        r->deliverWorkId = pushWork(json{{"kind", "deliver"},
                                         {"dispatch", id},
                                         {"intent", r->intent},
                                         {"params", r->params},
                                         {"requester", displaySafe(r->requester)}},
                                    WorkTarget{WorkKind::ShellDeliver, id});
        return;
    }

    // The presenter may raise the provider, if it has a screen; a terminal ignores it.
    if (r->provider != r->requester) {
        pushWork(json{{"kind", "present"}, {"provider", r->provider}, {"dispatch", id}});
        r->didNavigate = true;
    }

    Effect e;
    e.kind = Effect::Kind::Deliver;
    e.module = r->providerModule;
    e.dispatchId = id;
    e.intent = r->intent;
    e.params = r->params;
    e.requester = r->requester;
    m_effects.push_back(std::move(e));
}

void IntentBroker::deliveryOutcome(const std::string& dispatchId, bool reached, bool accepted,
                                   std::int64_t now)
{
    Request* r = find(dispatchId);
    if (!r || r->phase != Phase::Dispatched || r->floor) return;
    if (!reached) {
        // "unavailable", not "failed": "reached and died" would leak install state.
        failWithFloor(dispatchId, err::unavailable, Path::EndpointGone, now);
        return;
    }
    if (accepted) r->providerAccepted = true;
    else logLine("provider " + r->provider + " did not take " + r->intent + " — it will time out");
}

bool IntentBroker::awaitsResponseFrom(const std::string& module, const std::string& dispatchId) const
{
    const auto it = m_open.find(dispatchId);
    return it != m_open.end() && it->second.phase == Phase::Dispatched && !it->second.floor
        && !it->second.providerModule.empty() && it->second.providerModule == module;
}

bool IntentBroker::respond(const std::string& responderModule, const std::string& dispatchId,
                           bool ok, const json& data, const std::string& error, std::int64_t now)
{
    Request* r = find(dispatchId);
    const char* reject = nullptr;
    if (!awaitsResponseFrom(responderModule, dispatchId)) reject = "not waiting for it";
    if (reject) {
        logLine("dropped a response from " + responderModule + ": " + reject);
        return false;
    }

    // `failed`, not bad_request: the requester did nothing wrong.
    if (!isCanonicalValue(data)) {
        logLine("provider " + r->provider + " answered " + r->intent + " with a non-plain or oversized payload");
        finish(dispatchId, false, json(), err::failed, Path::ProviderResponse, now);
        return false;
    }
    if (ok) {
        if (const std::string violation = resultViolation(r->resultSpec, data); !violation.empty()) {
            logLine("provider " + r->provider + " answered " + r->intent + " outside its declared result: "
                    + violation);
            finish(dispatchId, false, json(), err::failed, Path::ProviderResponse, now);
            return false;
        }
    }
    finish(dispatchId, ok, data, normalizeError(ok, error), Path::ProviderResponse, now);
    return true;
}

json IntentBroker::result(const std::string& requester, const std::string& handle,
                          const std::string& receipt) const
{
    const json unknown = json{{"ok", false}, {"error", "unknown_handle"}};
    const auto s = m_settled.find(handle);
    if (s != m_settled.end()) {
        if (s->second.requester != requester || !sameSecret(s->second.receipt, receipt))
            return unknown;
        return json{{"ok", true}, {"state", "settled"}, {"result", s->second.envelope}};
    }
    if (const auto f = m_refused.find(handle); f != m_refused.end()) {
        if (f->second.fromPresenter || f->second.requester != requester
            || !sameSecret(f->second.receipt, receipt))
            return unknown;
        return json{{"ok", true}, {"state", "pending"}};
    }
    const auto h = m_byHandle.find(handle);
    if (h == m_byHandle.end()) return unknown;
    const auto o = m_open.find(h->second);
    if (o == m_open.end() || o->second.fromPresenter || o->second.requester != requester
        || !sameSecret(o->second.receipt, receipt))
        return unknown;
    return json{{"ok", true}, {"state", "pending"}};
}

bool IntentBroker::cancel(const std::string& requester, const std::string& handle,
                          const std::string& receipt, std::int64_t now)
{
    const auto s = m_settled.find(handle);
    if (s != m_settled.end()) {
        if (s->second.requester != requester || !sameSecret(s->second.receipt, receipt))
            return false;
        eraseSettled(s);
        return true;
    }
    if (const auto f = m_refused.find(handle); f != m_refused.end()) {
        if (f->second.fromPresenter || f->second.requester != requester
            || !sameSecret(f->second.receipt, receipt))
            return false;
        Refusal withdrawn = f->second;
        m_refused.erase(f);
        withdrawn.code = err::cancelled;
        settleRefusal(handle, withdrawn, now);
        return true;
    }
    const auto h = m_byHandle.find(handle);
    if (h == m_byHandle.end()) return false;
    Request* r = find(h->second);
    if (!r || r->fromPresenter || r->requester != requester || !sameSecret(r->receipt, receipt))
        return false;
    finish(h->second, false, json(), err::cancelled, Path::Abandoned, now);
    return true;
}

// ── Presenter answers ────────────────────────────────────────────────────────

bool IntentBroker::answer(const std::string& workId, const json& answer, std::int64_t now)
{
    // Everything is read and checked before anything changes, so a malformed
    // answer leaves the item answerable.
    const auto t = m_workTargets.find(workId);
    if (t == m_workTargets.end() || !answer.is_object()) return false;
    const WorkTarget target = t->second;
    const std::string action = stringAt(answer, "action");

    switch (target.kind) {
    case WorkKind::Choose: {
        Request* r = find(target.target);
        if (!r || r->phase != Phase::AwaitingChoice || r->floor) return false;
        if (action == "cancel") {
            m_workTargets.erase(workId);
            r->choicePresented = false;
            finish(target.target, false, json(), err::cancelled, Path::Rejected, now);
            return true;
        }
        if (action != "choose") return false;
        const std::string provider = stringAt(answer, "provider");
        // Only one of the providers the person was shown.
        const bool offered = std::any_of(r->choiceProviders.begin(), r->choiceProviders.end(),
                                         [&](const json& p) { return stringAt(p, "module") == provider; });
        if (provider.empty() || !offered) return false;
        m_workTargets.erase(workId);
        r->choicePresented = false;
        r->choiceWorkId.clear();
        // The registry may have changed under the chooser: ask again rather than
        // deliver to what the person didn't see.
        if (!stillEligible(*r, provider)) {
            logLine(provider + " changed while the chooser for " + r->intent + " was open: asking again");
            if (!showChoice(target.target, now)) presentNextChoice(now);
            return true;
        }
        selectProvider(target.target, provider, now);
        return true;
    }
    case WorkKind::Pending: {
        Request* r = find(target.target);
        if (!r || r->phase != Phase::Pending || r->floor) return false;
        if (action == "dismiss") {
            m_workTargets.erase(workId);
            r->pendingWorkId.clear();
            finish(target.target, false, json(), err::cancelled, Path::Rejected, now);
            return true;
        }
        if (action != "open") return false;
        m_workTargets.erase(workId);
        r->pendingWorkId.clear();
        r->opened = true;
        r->phase = Phase::Accepted;
        r->phaseSince = now;
        start(target.target, now);
        return true;
    }
    case WorkKind::ShellDeliver: {
        if (action != "respond") return false;
        Request* r = find(target.target);
        if (!r || r->phase != Phase::Dispatched || r->floor || !m_registry.isShell(r->provider))
            return false;
        const bool ok = boolAt(answer, "ok").value_or(false);
        const json* data = findKey(answer, "data");
        const std::string error = stringAt(answer, "error");
        m_workTargets.erase(workId);
        r->deliverWorkId.clear();
        if (data && !isCanonicalValue(*data)) {
            finish(target.target, false, json(), err::failed, Path::ProviderResponse, now);
            return true;
        }
        finish(target.target, ok, data ? *data : json(), normalizeError(ok, error),
               Path::ProviderResponse, now);
        return true;
    }
    }
    return false;
}

// ── Completion ───────────────────────────────────────────────────────────────

void IntentBroker::failWithFloor(const std::string& id, const std::string& code, Path path,
                                 std::int64_t now)
{
    Request* r = find(id);
    if (!r || r->floor) return;
    const std::int64_t due = r->acceptedAt + m_config.errorFloorMs;
    if (now >= due) {
        finish(id, false, json(), code, path, now);
        return;
    }
    // Held: an instant "no" says nothing is installed without anyone asking what is.
    r->floor = Floor{code, path, due};
}

void IntentBroker::eraseSettled(std::map<std::string, Settled>::iterator it)
{
    m_resultBytesTotal -= std::min(m_resultBytesTotal, it->second.bytes);
    m_byHandle.erase(it->first);
    m_settled.erase(it);
}

void IntentBroker::keepResult(const std::string& requester, const std::string& handle,
                              const std::string& receipt, json envelope, std::int64_t now)
{
    const std::size_t bytes = approxBytes(envelope);
    // Make room: the requester's own oldest results first, then anyone's.
    while (true) {
        int count = 0;
        std::size_t held = 0;
        auto oldest = m_settled.end();
        for (auto s = m_settled.begin(); s != m_settled.end(); ++s) {
            if (s->second.requester != requester) continue;
            ++count;
            held += s->second.bytes;
            if (oldest == m_settled.end() || s->second.at < oldest->second.at) oldest = s;
        }
        if (oldest == m_settled.end()) break;
        if (count < m_config.maxResultsPerRequester && held + bytes <= m_config.maxResultBytesPerRequester)
            break;
        eraseSettled(oldest);
    }
    while (!m_settled.empty() && m_resultBytesTotal + bytes > m_config.maxResultBytesTotal) {
        auto oldest = m_settled.begin();
        for (auto s = m_settled.begin(); s != m_settled.end(); ++s)
            if (s->second.at < oldest->second.at) oldest = s;
        eraseSettled(oldest);
    }
    m_settled[handle] = Settled{requester, receipt, std::move(envelope), now, bytes};
    m_resultBytesTotal += bytes;
}

void IntentBroker::finish(const std::string& id, bool ok, const json& data,
                          const std::string& error, Path path, std::int64_t now)
{
    const auto it = m_open.find(id);
    if (it == m_open.end()) return;
    const Request r = std::move(it->second);
    m_open.erase(it);   // before anything else: fire once by construction

    if (r.choicePresented) dropWorkTarget(r.choiceWorkId);
    if (!r.pendingWorkId.empty()) dropWorkTarget(r.pendingWorkId);
    if (!r.deliverWorkId.empty()) dropWorkTarget(r.deliverWorkId);

    json envelope = makeEnvelope(ok, data, error);
    if (r.fromPresenter) {
        m_byHandle.erase(r.handle);
        pushWork(json{{"kind", "result"}, {"handle", r.handle}, {"result", std::move(envelope)}});
    } else {
        keepResult(r.requester, r.handle, r.receipt, std::move(envelope), now);
        Effect e;
        e.kind = Effect::Kind::Settled;
        e.handle = r.handle;
        m_effects.push_back(std::move(e));
    }

    // The frontend decides whether to take the person back; it gets the facts.
    if (r.didNavigate) {
        pushWork(json{{"kind", "completed"},
                      {"dispatch", id},
                      {"requester", displaySafe(r.requester)},
                      {"provider", r.provider},
                      {"handoff", r.isHandoff},
                      {"by", pathName(static_cast<int>(path))}});
    }

    presentNextChoice(now);
}

void IntentBroker::tick(std::int64_t now)
{
    std::vector<std::string> ids;
    for (const auto& [id, r] : m_open) ids.push_back(id);

    for (const std::string& id : ids) {
        Request* r = find(id);
        if (!r) continue;
        if (r->floor) {
            if (now >= r->floor->at) {
                const Floor f = *r->floor;
                finish(id, false, json(), f.code, f.path, now);
            }
            continue;
        }
        const std::int64_t inPhase = now - r->phaseSince;
        switch (r->phase) {
        case Phase::Accepted:
            // Nothing stays here; if it does, something was lost.
            if (inPhase > m_config.responseMs)
                failWithFloor(id, err::unavailable, Path::Deadline, now);
            break;
        case Phase::Pending:
            if (inPhase > m_config.pendingTtlMs)
                finish(id, false, json(), err::cancelled, Path::Deadline, now);
            break;
        case Phase::AwaitingChoice:
            // On screen: a person is deciding, so this only catches a chooser that
            // went away. Waiting its turn: as long as the pending list.
            if (inPhase > (r->choicePresented ? m_config.backstopMs : m_config.pendingTtlMs))
                failWithFloor(id, err::cancelled, Path::Deadline, now);
            break;
        case Phase::Dispatched:
            if (inPhase > (r->providerAccepted ? m_config.backstopMs : m_config.responseMs))
                finish(id, false, json(), err::timeout, Path::Deadline, now);
            break;
        }
    }

    for (auto it = m_refused.begin(); it != m_refused.end();) {
        if (now < it->second.due) {
            ++it;
            continue;
        }
        const std::string handle = it->first;
        const Refusal refusal = it->second;
        it = m_refused.erase(it);
        settleRefusal(handle, refusal, now);
    }

    for (auto it = m_settled.begin(); it != m_settled.end();) {
        auto next = std::next(it);
        if (now - it->second.at > m_config.resultTtlMs) eraseSettled(it);
        it = next;
    }
}

} // namespace intents
