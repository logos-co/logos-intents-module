#pragma once

#include "intent_broker.h"
#include "intent_registry.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <stdlib.h>
#include <unistd.h>

namespace test {

using json = nlohmann::json;

// Predictable ids: tests read them back from effects and work items.
inline intents::IntentBroker::IdFn counterIds()
{
    auto n = std::make_shared<int>(0);
    return [n]() { return "id" + std::to_string(++*n); };
}

inline json pkg(const std::string& name, const std::string& type,
                json provides = json::array(), json uses = json::array())
{
    return json{{"name", name}, {"type", type}, {"provides", provides}, {"uses", uses}};
}

inline json intentList(std::initializer_list<const char*> names)
{
    json out = json::array();
    for (const char* n : names) out.push_back(json{{"intent", n}});
    return out;
}

inline std::vector<intents::Effect> ofKind(const std::vector<intents::Effect>& effects,
                                           intents::Effect::Kind kind)
{
    std::vector<intents::Effect> out;
    for (const auto& e : effects)
        if (e.kind == kind) out.push_back(e);
    return out;
}

inline std::vector<json> workOfKind(const json& items, const std::string& kind)
{
    std::vector<json> out;
    for (const json& i : items)
        if (i.value("kind", "") == kind) out.push_back(i);
    return out;
}

// A directory of installed packages, removed with the object.
class TempPackages {
public:
    TempPackages()
    {
        char tmpl[] = "/tmp/intents-test-XXXXXX";
        const char* made = mkdtemp(tmpl);
        m_root = made ? made : "";
    }
    ~TempPackages()
    {
        if (!m_root.empty()) std::system(("rm -rf '" + m_root + "'").c_str());
    }

    // Writes <root>/<dir>/metadata.json and returns the directory.
    std::string add(const std::string& dir, const json& metadata)
    {
        const std::string path = m_root + "/" + dir;
        std::system(("mkdir -p '" + path + "'").c_str());
        std::ofstream(path + "/metadata.json") << metadata.dump();
        return path;
    }

private:
    std::string m_root;
};

} // namespace test
