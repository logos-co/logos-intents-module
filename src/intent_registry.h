#pragma once

// Which installed module can do what, read from each package's own
// metadata.json.
//
// Rebuilt whole, never incrementally: a half-updated index is worse than a
// slightly late one.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace intents {

using json = nlohmann::json;

// What the host knows about an installed package. Everything intent-related is
// read from <installDir>/metadata.json, not taken from the host.
struct InstalledPackage {
    std::string name;
    std::string installDir;
    std::string displayName;   // a label only
    std::string iconSource;
};

// An installed package with its metadata.json read.
struct LoadedPackage {
    std::string name;
    std::string displayName;
    std::string iconSource;
    json metadata;       // discarded when it couldn't be read
    std::string error;   // why not
};

// Reads every package's metadata.json. File I/O and parsing: call it without
// holding a lock, then hand the result to IntentRegistry::rebuild().
std::vector<LoadedPackage> loadPackages(const std::vector<InstalledPackage>& packages);

class IntentRegistry {
public:
    // Per package: entries in `provides` or `uses`, and params per entry.
    static constexpr std::size_t kMaxEntries = 64;
    static constexpr std::size_t kMaxParams = 32;

    enum class Status { Ok, None, Ambiguous };

    struct ProviderEntry {
        std::string moduleName;    // the identity
        std::string displayName;   // a label only: a package can call itself anything
        std::string iconSource;
    };

    struct Resolution {
        Status status = Status::None;
        std::vector<ProviderEntry> found;   // sorted by moduleName
    };

    struct ParamSpec {
        std::string name;
        std::string type;   // empty: any
        bool required = false;
    };

    // The shell: the presenter's own uses and provides, declared in code. Its
    // namespace (e.g. "basecamp.") is then refused from every package, like
    // "logos.", including packages already read. Survives rebuild().
    void declareShell(const std::string& shellName,
                      const std::string& reservedPrefix,
                      const std::vector<std::string>& uses,
                      const std::vector<std::string>& provides,
                      const std::vector<std::string>& handoffIntents,
                      const std::string& displayName,
                      const std::string& iconSource);

    void rebuild(const std::vector<LoadedPackage>& packages);

    // One package's declarations from its whole metadata.json (rebuild() and tests).
    void ingest(const std::string& name, const json& metadata,
                const std::string& displayName, const std::string& iconSource);

    // Only the listed requesters may ask for each intent. The whole set,
    // replacing the last one; survives rebuild().
    void setRestrictions(const std::map<std::string, std::vector<std::string>>& restricted);
    bool requesterAllowed(const std::string& intent, const std::string& requester) const;

    Resolution resolve(const std::string& intent) const;
    // resolve(), narrowed to what `requester` may reach (links: opted-in only).
    Resolution resolveFor(const std::string& requester, const std::string& intent) const;

    // The (provider, intent) pair's declared params. Empty means "not
    // described", never "takes nothing".
    const std::vector<ParamSpec>& paramsSpecFor(const std::string& provider,
                                                const std::string& intent) const;
    // The same for what an answer carries: `result`, in the syntax of `params`.
    const std::vector<ParamSpec>& resultSpecFor(const std::string& provider,
                                                const std::string& intent) const;
    bool isHandoff(const std::string& provider, const std::string& intent) const;

    // "core", "ui_qml" or "shell"; empty for an unknown name.
    std::string typeOf(const std::string& name) const;
    bool isKnownPackage(const std::string& name) const;
    bool isShell(const std::string& name) const;
    const std::string& shellName() const { return m_shellName; }
    const std::string& linkName() const { return m_linkName; }

    bool declaresUse(const std::string& name, const std::string& intent) const;
    bool declaresProvide(const std::string& name, const std::string& intent) const;
    // Changes whenever what was read for the package does, across rebuilds;
    // 0 for an unknown name.
    std::uint64_t revisionOf(const std::string& name) const;

    // ── Links clicked outside the shell ─────────────────────────────────
    // The link requester's `uses` is derived: the shell's web intents plus every
    // provider entry that opted in with "web": true.
    void registerLinkRequester(const std::string& linkName,
                               const std::vector<std::string>& shellWebIntents);
    bool isLinkRequester(const std::string& name) const;
    bool isWebReachable(const std::string& provider, const std::string& intent) const;

    // Everything skipped or refused, one line each, safe to log.
    const std::vector<std::string>& diagnostics() const { return m_diagnostics; }

private:
    struct Provide {
        std::string intent;
        std::vector<ParamSpec> params;
        std::vector<ParamSpec> result;
        bool handoff = false;
        bool web = false;
    };

    bool isReservedNamespace(const std::string& intent) const;
    void resetPackages();
    void installShell();
    void dropPackage(const std::string& name);
    void diagnose(const std::string& line);
    std::vector<std::string> parseUses(const json* raw, const std::string& name);
    std::vector<Provide> parseProvides(const json* raw, const std::string& name);

    std::map<std::string, std::set<std::string>> m_provides;
    std::map<std::string, std::set<std::string>> m_uses;
    std::map<std::string, ProviderEntry> m_entries;
    std::map<std::string, std::string> m_types;
    std::map<std::string, std::vector<ParamSpec>> m_paramsSpec;   // "name/intent"
    std::map<std::string, std::vector<ParamSpec>> m_resultSpec;   // "name/intent"
    std::set<std::string> m_handoff;                               // "name/intent"
    std::set<std::string> m_webReachable;                          // "name/intent"
    std::map<std::string, int> m_webIntents;                       // intent -> web-reachable entries
    std::vector<std::string> m_diagnostics;

    struct Seen {
        std::string fingerprint;   // metadata and labels as last read
        std::uint64_t revision = 0;
    };
    std::map<std::string, Seen> m_seen;   // survives rebuild(); installed packages only
    std::uint64_t m_lastRevision = 0;
    std::uint64_t m_shellRevision = 0;

    // Declared in code; survive rebuild().
    std::string m_shellName;
    std::string m_shellPrefix;
    std::set<std::string> m_shellUses;
    std::set<std::string> m_shellProvides;
    std::set<std::string> m_shellHandoff;
    ProviderEntry m_shellEntry;
    std::map<std::string, std::vector<std::string>> m_restricted;
    std::string m_linkName;
    std::set<std::string> m_shellWebIntents;
};

} // namespace intents
