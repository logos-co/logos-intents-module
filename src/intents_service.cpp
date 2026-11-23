#include "intents_service.h"

#include "intent_rules.h"

#include <logos_caller.h>

#include <cstdio>

namespace intents {

namespace {

constexpr std::int64_t kRefusalLogMs = 60 * 1000;

void logLine(const std::string& line)
{
    std::fprintf(stderr, "[intents_module] %s\n", forLog(line, 400).c_str());
}

std::string reply(bool ok)
{
    return json{{"ok", ok}}.dump();
}

std::string refusal(const std::string& error)
{
    return json{{"ok", false}, {"error", error}}.dump();
}

bool sameSecret(const std::string& a, const std::string& b)
{
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::vector<std::string> stringList(const json* v)
{
    std::vector<std::string> out;
    if (!v || !v->is_array()) return out;
    for (const json& s : *v)
        if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

} // namespace

IntentsService::IntentsService(Outbound outbound, Clock clock, IdFn mintId, BrokerConfig config,
                               Loader load)
    : m_out(std::move(outbound))
    , m_clock(std::move(clock))
    , m_mintId(mintId)
    , m_load(std::move(load))
    , m_broker(m_registry, std::move(mintId), config)
{
}

void IntentsService::logRefusal(const std::string& caller, const std::string& line, std::int64_t now)
{
    const auto it = m_lastRefusalLog.find(caller);
    if (it != m_lastRefusalLog.end() && now - it->second < kRefusalLogMs) return;
    m_lastRefusalLog[caller] = now;
    logLine(line);
}

std::string IntentsService::requesterForCaller()
{
    const logos::LogosCaller caller = logos::currentCaller();
    // Host is refused here on purpose: under the daemon every `logosctl call`
    // is Host. The shell asks through shellRequest(), with the secret.
    if (!caller.isModule() || caller.name.empty()) return {};
    // These names are identities a module can't have: the shell and links ask
    // only through the presenter, and an app only through its backend module.
    if (m_registry.isShell(caller.name) || m_registry.isLinkRequester(caller.name)
        || m_registry.typeOf(caller.name) == "ui_qml") {
        logRefusal(caller.name, "refused module " + caller.name
                                    + ": the name belongs to the shell, the link requester or an app",
                   m_clock());
        return {};
    }
    return caller.name;
}

bool IntentsService::presenterCall(const std::string& secret, const char* method)
{
    if (!logos::currentCaller().isHost()) {
        logRefusal("presenter", std::string("refused ") + method + ": the presenter calls as the host",
                   m_clock());
        return false;
    }
    if (!m_claimed || !sameSecret(secret, m_secret)) {
        logRefusal("presenter", std::string("refused ") + method + ": wrong presenter secret", m_clock());
        return false;
    }
    return true;
}

void IntentsService::run(std::vector<Effect> effects)
{
    for (Effect& e : effects) {
        switch (e.kind) {
        case Effect::Kind::Deliver: {
            if (!m_out.deliver) break;
            const std::string dispatchId = e.dispatchId;
            const json args = json::array({e.dispatchId, e.intent, e.params.dump(), e.requester});
            std::weak_ptr<int> alive = m_alive;
            m_out.deliver(e.module, args, [this, alive, dispatchId](bool reached, const json& replyJson) {
                if (alive.expired()) return;
                // The provider answers `deliver` with {"ok": true} when it took it.
                // Read in place, never copied: the reply is the provider's to shape.
                bool accepted = false;
                if (reached && replyJson.is_string()) {
                    const json r = parseBounded(replyJson.get_ref<const std::string&>(), kReplyLimits);
                    accepted = boolAt(r, "ok").value_or(false);
                } else if (reached) {
                    accepted = boolAt(replyJson, "ok").value_or(false);
                }
                std::vector<Effect> more;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_broker.deliveryOutcome(dispatchId, reached, accepted, m_clock());
                    more = m_broker.takeEffects();
                }
                run(std::move(more));
            });
            break;
        }
        case Effect::Kind::Settled:
            if (m_out.settled) m_out.settled(e.handle);
            break;
        case Effect::Kind::WorkAvailable:
            if (m_out.workAvailable) m_out.workAvailable(e.counter);
            break;
        }
    }
}

// ── Requesters ───────────────────────────────────────────────────────────────

std::string IntentsService::request(const std::string& intent, const std::string& paramsJson)
{
    std::vector<Effect> effects;
    json out;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string who = requesterForCaller();
        // Answered here, with no handle and no event: a fact about the caller's
        // own manifest, and an event per call would let one module flood every other.
        if (who.empty() || !isValidName(intent) || !m_registry.declaresUse(who, intent))
            return refusal(err::notDeclared);
        const std::int64_t now = m_clock();
        if (!m_broker.admit(who, now)) return refusal(err::failed);

        // Only now, for a declared request within its rate, is anything it sent read.
        const json params = paramsJson.empty() ? json::object() : parseBounded(paramsJson, kPayloadLimits);
        const IntentBroker::Ticket t = m_broker.submit(who, intent, params, /*fromPresenter=*/false, now);
        // A `settled` for this request waits for the next tick, so it can't reach
        // the requester before the handle it names.
        for (Effect& e : m_broker.takeEffects()) {
            if (e.kind == Effect::Kind::Settled) m_deferredSettled.push_back(std::move(e));
            else effects.push_back(std::move(e));
        }
        out = json{{"ok", true}, {"handle", t.handle}, {"receipt", t.receipt}};
    }
    run(std::move(effects));
    return out.dump();
}

std::string IntentsService::result(const std::string& handle, const std::string& receipt)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string who = requesterForCaller();
    if (who.empty()) return refusal("unknown_handle");
    return m_broker.result(who, handle, receipt).dump();
}

std::string IntentsService::cancel(const std::string& handle, const std::string& receipt)
{
    std::vector<Effect> effects;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string who = requesterForCaller();
        if (who.empty()) return reply(false);
        ok = m_broker.cancel(who, handle, receipt, m_clock());
        effects = m_broker.takeEffects();
    }
    run(std::move(effects));
    return reply(ok);
}

// ── Providers ────────────────────────────────────────────────────────────────

std::string IntentsService::respond(const std::string& dispatchId, bool ok,
                                    const std::string& dataJson, const std::string& error)
{
    const logos::LogosCaller caller = logos::currentCaller();
    if (!caller.isModule() || caller.name.empty()) return reply(false);

    std::vector<Effect> effects;
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Nothing the caller sent is read unless this dispatch waits on it.
        if (!m_broker.awaitsResponseFrom(caller.name, dispatchId)) {
            logRefusal(caller.name, "dropped a response from " + caller.name
                                    + ": not waiting for it, payload unread",
                       m_clock());
            return reply(false);
        }
        const json data = dataJson.empty() ? json() : parseBounded(dataJson, kPayloadLimits);
        accepted = m_broker.respond(caller.name, dispatchId, ok, data, error, m_clock());
        effects = m_broker.takeEffects();
    }
    run(std::move(effects));
    return reply(accepted);
}

// ── Presenter ────────────────────────────────────────────────────────────────

std::string IntentsService::claim()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Once, by the host, before any app and before the daemon takes logosctl
    // calls. No handover: a second claim is always refused.
    if (!logos::currentCaller().isHost()) {
        logLine("refused claim: only the host may claim the presenter role");
        return refusal("refused");
    }
    if (m_claimed) {
        logLine("refused claim: the presenter role is already taken");
        return refusal("refused");
    }
    m_secret = m_mintId() + m_mintId();
    m_claimed = true;
    m_broker.setClaimed();
    logLine("presenter claimed");
    return json{{"ok", true}, {"secret", m_secret}}.dump();
}

std::string IntentsService::nextWork(const std::string& secret)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!presenterCall(secret, "next_work")) return refusal("refused");
    return json{{"ok", true}, {"items", m_broker.takeWork()}}.dump();
}

std::string IntentsService::answer(const std::string& secret, const std::string& workId,
                                   const std::string& answerJson)
{
    std::vector<Effect> effects;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!presenterCall(secret, "answer")) return refusal("refused");
        ok = m_broker.answer(workId, parseBounded(answerJson, kPresenterLimits), m_clock());
        effects = m_broker.takeEffects();
    }
    run(std::move(effects));
    return reply(ok);
}

std::string IntentsService::setInstalled(const std::string& secret, const std::string& packagesJson)
{
    std::lock_guard<std::mutex> installing(m_installMutex);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!presenterCall(secret, "set_installed")) return refusal("refused");
    }
    // Read without the lock so timers and delivery callbacks aren't held up;
    // readManifest() never blocks on what a package put in its directory.
    const json packages = parseBounded(packagesJson, kPresenterLimits);
    if (!packages.is_array()) return refusal("bad_request");
    std::vector<InstalledPackage> list;
    for (const json& p : packages)
        list.push_back(InstalledPackage{stringAt(p, "name"), stringAt(p, "installDir"),
                                        stringAt(p, "displayName"), stringAt(p, "iconSource")});
    const std::vector<LoadedPackage> loaded = m_load(list);

    std::vector<Effect> effects;
    json out;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_registry.rebuild(loaded);
        effects = m_broker.takeEffects();
        for (const std::string& d : m_registry.diagnostics()) logLine(d);
        out = json{{"ok", true}, {"diagnostics", m_registry.diagnostics()}};
    }
    run(std::move(effects));
    return out.dump();
}

std::string IntentsService::declareShell(const std::string& secret, const std::string& shellJson)
{
    std::vector<Effect> effects;
    json out;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!presenterCall(secret, "declare_shell")) return refusal("refused");
        out = declareShellLocked(shellJson);
        effects = m_broker.takeEffects();
    }
    run(std::move(effects));
    return out.dump();
}

json IntentsService::declareShellLocked(const std::string& shellJson)
{
    const auto bad = []() { return json{{"ok", false}, {"error", "bad_request"}}; };
    const json shell = parseBounded(shellJson, kPresenterLimits);
    if (!shell.is_object()) return bad();
    const std::string name = stringAt(shell, "name");
    const std::string prefix = stringAt(shell, "reservedPrefix");
    // A shell may reserve its own namespace, never the platform's or a bare one.
    if (!isValidPackageName(name)
        || (!prefix.empty() && (prefix.back() != '.' || prefix.size() < 2 || isPlatformName(prefix))))
        return bad();
    const json* link = findKey(shell, "link");
    const std::string linkName = link ? stringAt(*link, "name") : std::string();
    if (link && (!isValidPackageName(linkName) || linkName == name)) return bad();
    // Replaced whole, so read whole first: one that can't be leaves the last standing.
    std::map<std::string, std::vector<std::string>> restricted;
    if (const json* given = findKey(shell, "restricted")) {
        if (!given->is_object()) return bad();
        for (auto it = given->begin(); it != given->end(); ++it) {
            std::vector<std::string> requesters = stringList(&it.value());
            if (!isValidName(it.key()) || requesters.empty()) return bad();
            restricted[it.key()] = std::move(requesters);
        }
    }

    m_registry.declareShell(name, prefix,
                            stringList(findKey(shell, "uses")),
                            stringList(findKey(shell, "provides")),
                            stringList(findKey(shell, "handoff")),
                            stringAt(shell, "displayName"),
                            stringAt(shell, "iconSource"));
    m_registry.setRestrictions(restricted);
    if (link) m_registry.registerLinkRequester(linkName, stringList(findKey(*link, "webIntents")));
    return json{{"ok", true}, {"diagnostics", m_registry.diagnostics()}};
}

std::string IntentsService::shellRequest(const std::string& secret, const std::string& intent,
                                         const std::string& paramsJson, bool asLink)
{
    std::vector<Effect> effects;
    json out;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!presenterCall(secret, "shell_request")) return refusal("refused");
        const std::string requester = asLink ? m_registry.linkName() : m_registry.shellName();
        if (requester.empty()) return refusal(err::notDeclared);
        // Each one ends as a work item, so the presenter's own requests have a rate too.
        if (!m_broker.admit(requester, m_clock())) return refusal(err::failed);
        const json params = paramsJson.empty() ? json::object() : parseBounded(paramsJson, kPayloadLimits);
        const IntentBroker::Ticket t = m_broker.submit(requester, intent, params,
                                                       /*fromPresenter=*/true, m_clock());
        effects = m_broker.takeEffects();
        // Its result comes back as a work item, so no receipt.
        out = json{{"ok", true}, {"handle", t.handle}};
    }
    run(std::move(effects));
    return out.dump();
}

void IntentsService::tick()
{
    std::vector<Effect> effects;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::int64_t now = m_clock();
        effects = std::move(m_deferredSettled);
        m_deferredSettled.clear();
        m_broker.tick(now);
        for (Effect& e : m_broker.takeEffects()) effects.push_back(std::move(e));
    }
    run(std::move(effects));
}

} // namespace intents
