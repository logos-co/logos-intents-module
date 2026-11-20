#include "intent_broker.h"

#include "intent_rules.h"

#include <algorithm>
#include <cstdio>

namespace intents {

namespace {

// Past this, the oldest notice goes. Items someone must answer are never
// dropped; they're bounded by the per-requester limits instead.
constexpr std::size_t kSoftWorkLimit = 1000;

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

void IntentBroker::configureProvider(const std::string& intent, const std::string& provider)
{
    if (provider.empty()) {
        m_configured.erase(intent);
        logLine("headless provider for " + intent + " cleared");
        return;
    }
    m_configured[intent] = provider;
    logLine("headless provider for " + intent + " set to " + provider);
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
    const bool notice = kind == "present" || kind == "offer_install" || kind == "completed";
    if (m_work.size() >= kSoftWorkLimit) {
        const auto oldest = std::find_if(m_work.begin(), m_work.end(),
                                         [](const WorkItem& w) { return w.notice; });
        if (oldest != m_work.end()) m_work.erase(oldest);
    }
    m_work.push_back(WorkItem{id, notice, std::move(item)});
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

// ── Backends ─────────────────────────────────────────────────────────────────

std::string IntentBroker::backendRefusal(const std::string& app, const std::string& module) const
{
    if (m_registry.typeOf(app) != "ui_qml") return "not an installed app";
    // A package installed under this name is that package, not an app's backend.
    if (m_registry.isKnownPackage(module) || m_registry.isShell(module)
        || m_registry.isLinkRequester(module))
        return "the name belongs to a package";
    return {};
}

bool IntentBroker::registerBackend(const std::string& app, const std::string& module,
                                   std::int64_t now)
{
    if (app.empty() || !isValidPackageName(module)) return false;
    if (const std::string why = backendRefusal(app, module); !why.empty()) {
        logLine("refused backend " + module + " for " + app + ": " + why);
        return false;
    }
    for (const auto& [otherApp, b] : m_backends) {
        if (b.module == module && otherApp != app) {
            logLine("refused backend " + module + " for " + app + ": it serves " + otherApp);
            return false;
        }
    }
    // A new instance is a new backend: what was dispatched to the old one fails.
    if (m_backends.count(app)) unregisterBackend(app, now);
    m_backends[app] = Backend{module, ++m_backendGeneration};
    drainActivation(app, /*ready=*/true, now);
    return true;
}

void IntentBroker::unregisterBackend(const std::string& app, std::int64_t now)
{
    const auto b = m_backends.find(app);
    if (b == m_backends.end()) return;
    const Backend gone = b->second;
    m_backends.erase(b);

    std::vector<std::string> ids;
    for (const auto& [id, r] : m_open) ids.push_back(id);
    for (const std::string& id : ids) {
        Request* r = find(id);
        if (!r) continue;
        if (r->requester == app && !r->fromPresenter) {
            // Nobody left to pull it; a reloaded instance has no receipt for it.
            finish(id, false, json(), err::cancelled, Path::EndpointGone, now);
            continue;
        }
        if (r->provider == app && r->providerModule == gone.module
            && r->providerGeneration == gone.generation && !r->floor)
            failWithFloor(id, err::unavailable, Path::EndpointGone, now);
    }
    for (auto it = m_settled.begin(); it != m_settled.end();) {
        auto next = std::next(it);
        if (it->second.requester == app) eraseSettled(it);
        it = next;
    }
}

void IntentBroker::dropStaleBackends(std::int64_t now)
{
    std::vector<std::string> stale;
    for (const auto& [app, b] : m_backends) {
        if (const std::string why = backendRefusal(app, b.module); !why.empty()) {
            logLine("dropped backend " + b.module + " for " + app + ": " + why);
            stale.push_back(app);
        }
    }
    for (const std::string& app : stale) unregisterBackend(app, now);
}

std::string IntentBroker::appForBackend(const std::string& module) const
{
    for (const auto& [app, b] : m_backends)
        if (b.module == module) return app;
    return {};
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
                                          const json& params, bool clicked, bool fromPresenter,
                                          std::int64_t now)
{
    Request r;
    r.handle = m_mintId();
    r.receipt = m_mintId();
    r.dispatchId = m_mintId();
    r.requester = requester;
    r.intent = intent;
    r.clicked = clicked || fromPresenter;
    r.fromPresenter = fromPresenter;
    r.acceptedAt = now;
    r.phaseSince = now;

    const bool payloadOk = isCanonicalParams(params);
    const std::size_t bytes = payloadOk ? approxBytes(params) : 0;
    const bool withinLimit = countOpen(requester) < m_config.maxOpenPerRequester
        && m_open.size() < m_config.maxOpenTotal
        && openBytes(requester) + bytes <= m_config.maxOpenBytesPerRequester;
    if (payloadOk && withinLimit) {
        r.params = params;
        r.paramsBytes = bytes;
    }

    const std::string id = r.dispatchId;
    const Ticket ticket{r.handle, r.receipt};
    m_byHandle[r.handle] = id;
    m_open.emplace(id, std::move(r));

    // Floored: a provider may mint bad_request too, so an instant answer would
    // say no provider was consulted.
    if (!payloadOk) {
        logLine("refused " + forLog(intent, 64) + " from " + requester + ": params are not plain data");
        failWithFloor(id, err::badRequest, Path::Rejected, now);
    } else if (!withinLimit) {
        logLine("refused " + forLog(intent, 64) + " from " + requester + ": over its open-request limits");
        failWithFloor(id, err::failed, Path::Rejected, now);
    } else {
        start(id, now);
    }
    return ticket;
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

    if (m_attention == Attention::Headless) {
        const IntentRegistry::Resolution resolution = m_registry.resolveFor(requester, intent);
        const auto configured = m_configured.find(intent);
        const bool usable = configured != m_configured.end()
            && std::any_of(resolution.found.begin(), resolution.found.end(),
                           [&](const IntentRegistry::ProviderEntry& e) {
                               return e.moduleName == configured->second;
                           });
        if (!usable) {
            failWithFloor(id, err::unavailable, Path::Rejected, now);
            return;
        }
        selectProvider(id, configured->second, now);
        return;
    }

    // Asking for its own capability is internal navigation: no boundary, no person.
    if (m_registry.declaresProvide(requester, intent)) {
        selectProvider(id, requester, now);
        return;
    }

    // No click behind it: into the pending list before anything is resolved, so
    // whether a provider exists is only decided once the person opens it.
    if (m_attention == Attention::Interactive && !r->clicked) {
        park(id, now);
        return;
    }

    const IntentRegistry::Resolution resolution = m_registry.resolveFor(requester, intent);
    if (resolution.status == IntentRegistry::Status::None) {
        offerInstall(requester, intent, now);
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

void IntentBroker::offerInstall(const std::string& requester, const std::string& intent,
                                std::int64_t now)
{
    // The presenter may offer an install; the requester learns nothing either way.
    const std::vector<std::string> installable = m_registry.installableFor(requester, intent);
    if (installable.empty()) return;
    // One offer per intent a minute: requests can't become a stream of prompts.
    const auto last = m_lastOffer.find(intent);
    if (last != m_lastOffer.end() && now - last->second < m_config.offerCooldownMs) return;
    m_lastOffer[intent] = now;
    pushWork(json{{"kind", "offer_install"}, {"intent", intent}, {"candidates", installable}});
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

// What made the provider one to show: still allowed, still reachable by this
// requester, and still the package that was shown.
bool IntentBroker::stillEligible(const Request& r, const std::string& provider) const
{
    const auto shown = r.choiceRevisions.find(provider);
    if (shown == r.choiceRevisions.end() || shown->second != m_registry.revisionOf(provider)) return false;
    if (!m_registry.requesterAllowed(r.intent, r.requester)) return false;
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
        r->providerGeneration = 0;
        dispatch(id, now);
    } else if (type == "ui_qml") {
        const auto b = m_backends.find(provider);
        if (b != m_backends.end()) {
            r->providerModule = b->second.module;
            r->providerGeneration = b->second.generation;
            dispatch(id, now);
        } else if (m_attention == Attention::Headless) {
            failWithFloor(id, err::unavailable, Path::Rejected, now);
        } else {
            r->phase = Phase::Activating;
            r->phaseSince = now;
            Activation& activation = m_activation[provider];
            const bool alreadyAsked = !activation.ids.empty();
            activation.ids.push_back(id);
            if (!alreadyAsked)
                activation.workId = pushWork(json{{"kind", "activate"}, {"app", provider}},
                                             WorkTarget{WorkKind::Activate, provider});
        }
    } else {
        failWithFloor(id, err::unavailable, Path::Rejected, now);
    }

    // The chooser is free once a choice is made, not when the request ends.
    presentNextChoice(now);
}

void IntentBroker::drainActivation(const std::string& app, bool ready, std::int64_t now)
{
    const auto q = m_activation.find(app);
    if (q == m_activation.end()) return;
    const std::vector<std::string> ids = q->second.ids;
    endActivation(app);
    for (const std::string& id : ids) {
        Request* r = find(id);
        if (!r || r->phase != Phase::Activating || r->floor) continue;
        const auto b = m_backends.find(app);
        if (ready && b != m_backends.end()) {
            r->providerModule = b->second.module;
            r->providerGeneration = b->second.generation;
            dispatch(id, now);
        } else {
            failWithFloor(id, err::unavailable, Path::EndpointGone, now);
        }
    }
}

void IntentBroker::endActivation(const std::string& app)
{
    const auto a = m_activation.find(app);
    if (a == m_activation.end()) return;
    const std::string workId = a->second.workId;
    m_activation.erase(a);
    // Not pulled: taken back. Pulled and unanswered: the presenter takes it down.
    dropWorkTarget(workId);
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
    // Snapshot: a rebuild mustn't change the navigation of a request in flight.
    r->isHandoff = m_registry.isHandoff(r->provider, r->intent);

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

    // Raise the provider's window. The presenter decides how; a terminal ignores it.
    if (m_registry.typeOf(r->provider) == "ui_qml" && r->provider != r->requester
        && m_attention != Attention::Headless) {
        pushWork(json{{"kind", "present"}, {"app", r->provider}, {"dispatch", id}});
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
    else if (m_registry.typeOf(r->provider) == "ui_qml") {
        const auto b = m_backends.find(r->provider);
        if (b == m_backends.end() || b->second.generation != r->providerGeneration)
            reject = "backend changed since delivery";
    }
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
    case WorkKind::Activate: {
        if (action != "activated" && action != "failed") return false;
        // Ready only once its backend is registered; until then it waits.
        const bool ready = action == "activated";
        if (ready && !m_backends.count(target.target)) return true;
        // Answered, so there is nothing left on screen to take down.
        m_workTargets.erase(workId);
        const auto a = m_activation.find(target.target);
        if (a != m_activation.end() && a->second.workId == workId) a->second.workId.clear();
        drainActivation(target.target, ready, now);
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
        // Opening it is the click.
        m_workTargets.erase(workId);
        r->pendingWorkId.clear();
        r->clicked = true;
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

void IntentBroker::keepResult(const Request& r, json envelope, std::int64_t now)
{
    const std::size_t bytes = approxBytes(envelope);
    // Make room: the requester's own oldest results first, then anyone's.
    while (true) {
        int count = 0;
        std::size_t held = 0;
        auto oldest = m_settled.end();
        for (auto s = m_settled.begin(); s != m_settled.end(); ++s) {
            if (s->second.requester != r.requester) continue;
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
    m_settled[r.handle] = Settled{r.requester, r.receipt, std::move(envelope), now, bytes};
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
    if (!r.provider.empty()) {
        const auto a = m_activation.find(r.provider);
        if (a != m_activation.end()) {
            std::vector<std::string>& ids = a->second.ids;
            ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
            // The last one waiting: nothing may raise the app for it now.
            if (ids.empty()) endActivation(r.provider);
        }
    }

    json envelope = makeEnvelope(ok, data, error);
    if (r.fromPresenter) {
        m_byHandle.erase(r.handle);
        pushWork(json{{"kind", "result"}, {"handle", r.handle}, {"result", std::move(envelope)}});
    } else {
        keepResult(r, std::move(envelope), now);
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
            if (inPhase > m_config.activationMs)
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
        case Phase::Activating:
            if (inPhase > m_config.activationMs)
                failWithFloor(id, err::unavailable, Path::Deadline, now);
            break;
        case Phase::Dispatched:
            if (inPhase > (r->providerAccepted ? m_config.backstopMs : m_config.responseMs))
                finish(id, false, json(), err::timeout, Path::Deadline, now);
            break;
        }
    }

    for (auto it = m_settled.begin(); it != m_settled.end();) {
        auto next = std::next(it);
        if (now - it->second.at > m_config.resultTtlMs) eraseSettled(it);
        it = next;
    }
}

} // namespace intents
