#pragma once

// One client per provider module, and an orderly end to them at unload.
//
// Destroying a client waits for its running callback and stops further ones
// (lp_client_destroy). close() does that while the service those callbacks
// call into is still alive, outside the lock so a finishing callback can still
// ask for a client (and get none). After close(), get() returns null.

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace intents {

template <class Client>
class ClientPool {
public:
    using Make = std::function<std::shared_ptr<Client>(const std::string& module)>;

    explicit ClientPool(Make make) : m_make(std::move(make)) {}
    ~ClientPool() { close(); }

    std::shared_ptr<Client> get(const std::string& module)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) return nullptr;
        auto& slot = m_clients[module];
        if (!slot) slot = m_make(module);
        return slot;
    }

    void close()
    {
        std::map<std::string, std::shared_ptr<Client>> gone;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
            gone.swap(m_clients);
        }
        gone.clear();
    }

private:
    Make m_make;
    std::mutex m_mutex;
    bool m_closed = false;
    std::map<std::string, std::shared_ptr<Client>> m_clients;
};

} // namespace intents
