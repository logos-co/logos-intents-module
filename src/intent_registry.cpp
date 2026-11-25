#include "intent_registry.h"

#include "intent_rules.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace intents {

namespace {

constexpr std::size_t kMaxDiagnostics = 1000;

std::string key(const std::string& name, const std::string& intent)
{
    return name + "/" + intent;
}

enum class FileRead { Ok, Missing, Unreadable, NotRegular, TooLarge };

// The whole of `path` into *text, or why not. Only a regular file is read, and
// opening it never waits, so a FIFO, a pipe or a device can't stall the module.
FileRead readRegularFile(const std::string& path, std::size_t maxBytes, std::string* text)
{
#ifdef _WIN32
    const int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                         static_cast<int>(path.size()), nullptr, 0);
    if (wide <= 0) return FileRead::Unreadable;
    std::wstring wpath(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
                        wpath.data(), wide);
    // Opening a pipe doesn't wait for its other end, and a directory doesn't open.
    const HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? FileRead::Missing
                                                                             : FileRead::Unreadable;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info)
        || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(h);
        return FileRead::NotRegular;
    }
    char buf[4096];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof buf, &n, nullptr)) {
            CloseHandle(h);
            return FileRead::Unreadable;
        }
        if (n == 0) break;
        text->append(buf, n);
        if (text->size() > maxBytes) {
            CloseHandle(h);
            return FileRead::TooLarge;
        }
    }
    CloseHandle(h);
    return FileRead::Ok;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? FileRead::Missing : FileRead::Unreadable;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return FileRead::NotRegular;
    }
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            ::close(fd);
            return FileRead::Unreadable;
        }
        if (n == 0) break;
        text->append(buf, static_cast<std::size_t>(n));
        if (text->size() > maxBytes) {
            ::close(fd);
            return FileRead::TooLarge;
        }
    }
    ::close(fd);
    return FileRead::Ok;
#endif
}

// <installDir>/metadata.json, or a discarded value and a reason. Bounded before
// anything is built: a package controls this file. A package whose directory
// holds none declares nothing, which is no reason; a directory that isn't
// there is.
json readManifest(const std::string& installDir, std::string* error)
{
    const std::string path = installDir + "/metadata.json";
    std::string text;
    switch (readRegularFile(path, kManifestLimits.maxBytes, &text)) {
    case FileRead::Ok:
        break;
    case FileRead::Missing: {
        std::error_code ec;
        if (!std::filesystem::is_directory(installDir, ec)) *error = "cannot read " + forLog(path);
        return json::value_t::discarded;
    }
    case FileRead::Unreadable:
        *error = "cannot read " + forLog(path);
        return json::value_t::discarded;
    case FileRead::NotRegular:
        *error = "metadata.json is not a regular file";
        return json::value_t::discarded;
    case FileRead::TooLarge:
        *error = "metadata.json is larger than 1 MiB";
        return json::value_t::discarded;
    }
    json doc = parseBounded(text, kManifestLimits);
    if (!doc.is_object()) {
        *error = "metadata.json is malformed, too deeply nested or too large";
        return json::value_t::discarded;
    }
    return doc;
}

} // namespace

std::vector<LoadedPackage> loadPackages(const std::vector<InstalledPackage>& packages)
{
    std::vector<LoadedPackage> out;
    out.reserve(packages.size());
    for (const InstalledPackage& p : packages) {
        LoadedPackage l{p.name, p.displayName, p.iconSource, json::value_t::discarded, {}};
        if (!isValidPackageName(p.name)) l.error = "not a valid package name";
        else if (p.installDir.empty()) l.error = "no install directory";
        else l.metadata = readManifest(p.installDir, &l.error);
        out.push_back(std::move(l));
    }
    return out;
}

void IntentRegistry::diagnose(const std::string& line)
{
    if (m_diagnostics.size() < kMaxDiagnostics) m_diagnostics.push_back(line);
}

bool IntentRegistry::isReservedNamespace(const std::string& intent) const
{
    return isPlatformName(intent)
        || (!m_shellPrefix.empty() && intent.rfind(m_shellPrefix, 0) == 0);
}

void IntentRegistry::installShell()
{
    if (m_shellName.empty()) return;
    if (!m_shellProvides.empty()) m_provides[m_shellName] = m_shellProvides;
    if (!m_shellUses.empty()) m_uses[m_shellName] = m_shellUses;
    m_entries[m_shellName] = m_shellEntry;
    m_types[m_shellName] = "shell";
    for (const std::string& intent : m_shellHandoff) m_handoff.insert(key(m_shellName, intent));
}

void IntentRegistry::dropPackage(const std::string& name)
{
    const auto p = m_provides.find(name);
    if (p != m_provides.end()) {
        for (const std::string& intent : p->second) {
            const std::string k = key(name, intent);
            m_paramsSpec.erase(k);
            m_resultSpec.erase(k);
            m_handoff.erase(k);
            if (m_webReachable.erase(k) && --m_webIntents[intent] <= 0) m_webIntents.erase(intent);
        }
        m_provides.erase(p);
    }
    m_uses.erase(name);
    m_entries.erase(name);
    m_types.erase(name);
}

void IntentRegistry::declareShell(const std::string& shellName,
                                  const std::string& reservedPrefix,
                                  const std::vector<std::string>& uses,
                                  const std::vector<std::string>& provides,
                                  const std::vector<std::string>& handoffIntents,
                                  const std::string& displayName,
                                  const std::string& iconSource)
{
    if (!isValidPackageName(shellName)) {
        diagnose("shell: '" + forLog(shellName) + "' is not a valid name — not declared");
        return;
    }
    if (!m_shellName.empty()) dropPackage(m_shellName);

    // A package that took the name first loses it.
    if (m_types.count(shellName)) {
        diagnose(shellName + ": the shell's name — the package is dropped");
        dropPackage(shellName);
    }

    m_shellName = shellName;
    m_shellPrefix = reservedPrefix;
    m_shellUses.clear();
    m_shellProvides.clear();
    m_shellHandoff.clear();
    for (const std::string& intent : uses) {
        if (isValidName(intent)) m_shellUses.insert(intent);
        else diagnose("shell: uses '" + forLog(intent) + "' fails the name grammar — ignored");
    }
    for (const std::string& intent : provides) {
        if (isValidName(intent)) m_shellProvides.insert(intent);
        else diagnose("shell: provides '" + forLog(intent) + "' fails the name grammar — ignored");
    }
    for (const std::string& intent : handoffIntents) {
        if (m_shellProvides.count(intent)) m_shellHandoff.insert(intent);
        else diagnose("shell: handoff for '" + forLog(intent) + "', which the shell doesn't provide — ignored");
    }
    m_shellEntry = {shellName, displayName.empty() ? shellName : displayName, iconSource};
    ++m_shellRevision;
    installShell();

    // The namespace is reserved for packages already read too.
    for (auto& [name, intentSet] : m_provides) {
        if (name == m_shellName) continue;
        for (auto it = intentSet.begin(); it != intentSet.end();) {
            if (!isReservedNamespace(*it)) {
                ++it;
                continue;
            }
            diagnose(name + ": refused to provide reserved intent '" + *it + "'");
            const std::string k = key(name, *it);
            m_paramsSpec.erase(k);
            m_resultSpec.erase(k);
            m_handoff.erase(k);
            if (m_webReachable.erase(k) && --m_webIntents[*it] <= 0) m_webIntents.erase(*it);
            it = intentSet.erase(it);
        }
    }
}

void IntentRegistry::resetPackages()
{
    m_provides.clear();
    m_uses.clear();
    m_entries.clear();
    m_types.clear();
    m_paramsSpec.clear();
    m_resultSpec.clear();
    m_handoff.clear();
    m_webReachable.clear();
    m_webIntents.clear();
    m_diagnostics.clear();
    installShell();
}

void IntentRegistry::rebuild(const std::vector<LoadedPackage>& packages)
{
    resetPackages();
    for (const LoadedPackage& p : packages) {
        if (!p.error.empty()) {
            diagnose(forLog(p.name) + ": " + p.error);
            continue;
        }
        if (p.metadata.is_discarded()) continue;   // no metadata.json: it declares nothing
        // The host names the package; its own file must agree.
        if (stringAt(p.metadata, "name") != p.name) {
            diagnose(forLog(p.name) + ": metadata.json names another package — skipped");
            continue;
        }
        ingest(p.name, p.metadata, p.displayName, p.iconSource);
    }
    // Only what is installed: a package back after an uninstall is a new one.
    for (auto it = m_seen.begin(); it != m_seen.end();)
        it = m_types.count(it->first) ? std::next(it) : m_seen.erase(it);
}

std::vector<std::string> IntentRegistry::parseUses(const json* raw, const std::string& name)
{
    std::vector<std::string> out;
    if (!raw) return out;
    if (!raw->is_array()) {
        diagnose(name + ": 'uses' is not a list — ignored");
        return out;
    }
    std::set<std::string> seen;
    std::size_t n = 0;
    for (const json& entry : *raw) {
        if (++n > kMaxEntries) {
            diagnose(name + ": more than 64 'uses' entries — the rest are ignored");
            break;
        }
        // Objects only: tolerating two shapes is how a surface stops being frozen.
        if (!entry.is_object()) {
            diagnose(name + ": 'uses' entry must be an object like {\"intent\": \"a.b\"} — ignored");
            continue;
        }
        const std::string intent = stringAt(entry, "intent");
        if (!isValidName(intent)) {
            diagnose(name + ": 'uses' intent '" + forLog(intent) + "' fails the name grammar — ignored");
            continue;
        }
        if (!seen.insert(intent).second) {
            diagnose(name + ": 'uses' declares '" + intent + "' twice — ignored");
            continue;
        }
        const json* cardinality = findKey(entry, "cardinality");
        if (cardinality && !(cardinality->is_string() && cardinality->get<std::string>() == "single"))
            diagnose(name + ": 'uses' intent '" + intent
                     + "' requests an unsupported cardinality — treated as 'single'");
        out.push_back(intent);
    }
    return out;
}

std::vector<IntentRegistry::Provide> IntentRegistry::parseProvides(const json* raw,
                                                                   const std::string& name)
{
    std::vector<Provide> out;
    if (!raw) return out;
    if (!raw->is_array()) {
        diagnose(name + ": 'provides' is not a list — ignored");
        return out;
    }
    std::set<std::string> seen;
    std::size_t n = 0;
    for (const json& entry : *raw) {
        if (++n > kMaxEntries) {
            diagnose(name + ": more than 64 'provides' entries — the rest are ignored");
            break;
        }
        if (!entry.is_object()) {
            diagnose(name + ": 'provides' entry must be an object like {\"intent\": \"a.b\"} — ignored");
            continue;
        }
        Provide p;
        p.intent = stringAt(entry, "intent");
        if (!isValidName(p.intent)) {
            diagnose(name + ": 'provides' intent '" + forLog(p.intent) + "' fails the name grammar — ignored");
            continue;
        }
        if (!seen.insert(p.intent).second) {
            diagnose(name + ": 'provides' declares '" + p.intent + "' twice — ignored");
            continue;
        }
        // `params` describes the request, `result` the answer; the same syntax.
        const auto describe = [&](const char* field, std::vector<ParamSpec>& out) {
            const json* specs = findKey(entry, field);
            if (!specs || !specs->is_array()) return;
            std::size_t k = 0;
            for (const json& spec : *specs) {
                if (++k > kMaxParams) {
                    diagnose(name + ": '" + p.intent + "' has more than 32 entries in '" + field
                             + "' — the rest are ignored");
                    break;
                }
                const std::string fieldName = stringAt(spec, "name");
                if (fieldName.empty()) continue;
                const std::string type = stringAt(spec, "type");
                if (!type.empty() && !isParamType(type))
                    diagnose(name + ": '" + p.intent + "' declares '" + forLog(fieldName, 64) + "' in '" + field
                             + "' as '" + forLog(type, 32)
                             + "', not one of string, number, bool, object, array — its type isn't checked");
                out.push_back({fieldName, type, boolAt(spec, "required").value_or(false)});
            }
        };
        describe("params", p.params);
        describe("result", p.result);
        // Strict booleans: "true" as a string would otherwise flip the meaning.
        if (const json* h = findKey(entry, "handoff")) {
            if (h->is_boolean()) p.handoff = h->get<bool>();
            else diagnose(name + ": '" + p.intent + "' declares handoff as a non-boolean — treated as false");
        }
        if (const json* w = findKey(entry, "web")) {
            if (w->is_boolean()) p.web = w->get<bool>();
            else diagnose(name + ": '" + p.intent + "' declares web as a non-boolean — treated as false");
        }
        out.push_back(std::move(p));
    }
    return out;
}

void IntentRegistry::ingest(const std::string& name, const json& metadata,
                            const std::string& displayName, const std::string& iconSource)
{
    if (!isValidPackageName(name)) {
        diagnose(forLog(name) + ": not a valid package name — skipped");
        return;
    }
    if (!metadata.is_object()) return;

    // A package must never take the shell's name, or it could answer for the shell.
    if (isShell(name)) {
        diagnose(name + ": a package may not use the shell's name — skipped");
        return;
    }
    // Nor the link requester's: its `uses` is what a web page may reach.
    if (isLinkRequester(name)) {
        diagnose(name + ": a package may not use the link requester's name — skipped");
        return;
    }
    if (m_types.count(name)) {
        diagnose(name + ": duplicate package name — first record wins");
        return;
    }

    const std::string type = stringAt(metadata, "type");
    if (type != "core" && type != "ui_qml") return;
    // An app's screen is only a name here: its backend asks and answers for it,
    // with the intents in the backend's own metadata.json.
    if (type == "ui_qml") {
        m_types[name] = type;
        return;
    }

    std::vector<Provide> provides = parseProvides(findKey(metadata, "provides"), name);
    const std::vector<std::string> uses = parseUses(findKey(metadata, "uses"), name);

    const std::string fingerprint = metadata.dump(-1, ' ', false, json::error_handler_t::replace)
        + '\x1f' + displayName + '\x1f' + iconSource;
    Seen& seen = m_seen[name];
    if (seen.revision == 0 || seen.fingerprint != fingerprint) seen = Seen{fingerprint, ++m_lastRevision};

    m_types[name] = type;
    m_entries[name] = {name, displayName.empty() ? name : displayName, iconSource};
    for (Provide& p : provides) {
        if (isReservedNamespace(p.intent)) {
            diagnose(name + ": refused to provide reserved intent '" + p.intent + "'");
            continue;
        }
        m_provides[name].insert(p.intent);
        const std::string k = key(name, p.intent);
        if (!p.params.empty()) m_paramsSpec[k] = std::move(p.params);
        if (!p.result.empty()) m_resultSpec[k] = std::move(p.result);
        if (p.handoff) m_handoff.insert(k);
        if (p.web && m_webReachable.insert(k).second) ++m_webIntents[p.intent];
    }
    if (!uses.empty()) m_uses[name] = std::set<std::string>(uses.begin(), uses.end());
}

void IntentRegistry::setRestrictions(const std::map<std::string, std::vector<std::string>>& restricted)
{
    m_restricted.clear();
    for (const auto& [intent, requesters] : restricted) {
        if (intent.empty()) continue;
        // An empty list would store as "unrestricted".
        if (requesters.empty()) {
            diagnose("shell: refusing an empty requester list for '" + forLog(intent) + "'");
            continue;
        }
        m_restricted[intent] = requesters;
    }
}

bool IntentRegistry::requesterAllowed(const std::string& intent, const std::string& requester) const
{
    const auto it = m_restricted.find(intent);
    return it == m_restricted.end()
        || std::find(it->second.begin(), it->second.end(), requester) != it->second.end();
}

IntentRegistry::Resolution IntentRegistry::resolve(const std::string& intent) const
{
    Resolution r;
    if (intent.empty()) return r;
    for (const auto& [name, intentSet] : m_provides) {
        if (!intentSet.count(intent)) continue;
        const auto e = m_entries.find(name);
        r.found.push_back(e != m_entries.end() ? e->second : ProviderEntry{name, name, {}});
    }
    // m_provides is ordered by name, so `found` is too.
    r.status = r.found.empty() ? Status::None
             : r.found.size() == 1 ? Status::Ok : Status::Ambiguous;
    return r;
}

IntentRegistry::Resolution IntentRegistry::resolveFor(const std::string& requester,
                                                      const std::string& intent) const
{
    Resolution r = resolve(intent);
    if (!isLinkRequester(requester)) return r;
    r.found.erase(std::remove_if(r.found.begin(), r.found.end(),
                                 [&](const ProviderEntry& e) { return !isWebReachable(e.moduleName, intent); }),
                  r.found.end());
    r.status = r.found.empty() ? Status::None
             : r.found.size() == 1 ? Status::Ok : Status::Ambiguous;
    return r;
}

const std::vector<IntentRegistry::ParamSpec>& IntentRegistry::paramsSpecFor(
    const std::string& provider, const std::string& intent) const
{
    static const std::vector<ParamSpec> none;
    const auto it = m_paramsSpec.find(key(provider, intent));
    return it == m_paramsSpec.end() ? none : it->second;
}

const std::vector<IntentRegistry::ParamSpec>& IntentRegistry::resultSpecFor(
    const std::string& provider, const std::string& intent) const
{
    static const std::vector<ParamSpec> none;
    const auto it = m_resultSpec.find(key(provider, intent));
    return it == m_resultSpec.end() ? none : it->second;
}

bool IntentRegistry::isHandoff(const std::string& provider, const std::string& intent) const
{
    return m_handoff.count(key(provider, intent)) > 0;
}

std::string IntentRegistry::typeOf(const std::string& name) const
{
    const auto it = m_types.find(name);
    return it == m_types.end() ? std::string() : it->second;
}

bool IntentRegistry::isKnownPackage(const std::string& name) const
{
    const std::string t = typeOf(name);
    return t == "core" || t == "ui_qml";
}

bool IntentRegistry::isShell(const std::string& name) const
{
    return !m_shellName.empty() && name == m_shellName;
}

bool IntentRegistry::declaresUse(const std::string& name, const std::string& intent) const
{
    // Derived on demand: the shell's web intents, and every installed opt-in.
    if (isLinkRequester(name))
        return m_shellWebIntents.count(intent) || m_webIntents.count(intent);
    const auto it = m_uses.find(name);
    return it != m_uses.end() && it->second.count(intent);
}

std::uint64_t IntentRegistry::revisionOf(const std::string& name) const
{
    if (isShell(name)) return m_shellRevision;
    const auto it = m_seen.find(name);
    return it == m_seen.end() || !m_types.count(name) ? 0 : it->second.revision;
}

bool IntentRegistry::declaresProvide(const std::string& name, const std::string& intent) const
{
    const auto it = m_provides.find(name);
    return it != m_provides.end() && it->second.count(intent);
}

void IntentRegistry::registerLinkRequester(const std::string& linkName,
                                           const std::vector<std::string>& shellWebIntents)
{
    if (!isValidPackageName(linkName)) {
        diagnose("link: '" + forLog(linkName) + "' is not a valid name — not registered");
        return;
    }
    if (m_types.count(linkName) && !isShell(linkName)) {
        diagnose(linkName + ": the link requester's name — the package is dropped");
        dropPackage(linkName);
    }
    m_linkName = linkName;
    m_shellWebIntents.clear();
    for (const std::string& intent : shellWebIntents) {
        if (isValidName(intent)) m_shellWebIntents.insert(intent);
        else diagnose("link: web intent '" + forLog(intent) + "' fails the name grammar — ignored");
    }
}

bool IntentRegistry::isLinkRequester(const std::string& name) const
{
    return !m_linkName.empty() && name == m_linkName;
}

bool IntentRegistry::isWebReachable(const std::string& provider, const std::string& intent) const
{
    // Per provider: one app's opt-in must not publish another's implementation.
    if (isShell(provider)) return m_shellWebIntents.count(intent) > 0;
    return m_webReachable.count(key(provider, intent)) > 0;
}

} // namespace intents
