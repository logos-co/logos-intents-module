#include "intents_module_impl.h"

#include "client_pool.h"
#include "intent_rules.h"
#include "intents_service.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <logos_lp_client.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {

constexpr int kTickMs = 100;
constexpr int kDeliverTimeoutMs = 5000;

// Handles, receipts and the presenter secret: the platform CSPRNG, as the
// host uses for module tokens (std::random_device may be deterministic).
std::string mintId()
{
    static std::mutex mutex;
    static boost::uuids::random_generator gen;
    std::lock_guard<std::mutex> lock(mutex);
    std::string s = boost::uuids::to_string(gen());
    s.erase(std::remove(s.begin(), s.end(), '-'), s.end());
    return s;
}

std::int64_t steadyMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Nothing in the service is meant to throw; if something does, the caller gets
// a refusal instead of the module going down with every request in flight.
template <class F>
std::string guarded(const char* method, F&& call)
{
    try {
        return call();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[intents_module] %s threw: %s\n", method,
                     intents::forLog(e.what()).c_str());
    } catch (...) {
        std::fprintf(stderr, "[intents_module] %s threw\n", method);
    }
    return R"({"ok":false,"error":"failed"})";
}

} // namespace

struct IntentsModuleState {
    explicit IntentsModuleState(intents::ClientPool<logos::LpClient>::Make make)
        : clients(std::move(make)) {}

    std::unique_ptr<intents::IntentsService> service;
    intents::ClientPool<logos::LpClient> clients;

    std::mutex timerMutex;
    std::condition_variable timerCv;
    bool stopping = false;
    std::thread timer;

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(timerMutex);
            stopping = true;
        }
        timerCv.notify_all();
        if (timer.joinable()) timer.join();
        // Delivery callbacks call into the service: end them while it is alive.
        clients.close();
    }
};

IntentsModuleImpl::IntentsModuleImpl()
    : m_state(std::make_unique<IntentsModuleState>([this](const std::string& module) {
          return std::make_shared<logos::LpClient>(module, moduleName());
      }))
{
    IntentsModuleState* state = m_state.get();

    intents::IntentsService::Outbound out;
    out.deliver = [state](const std::string& module, const nlohmann::json& args,
                          std::function<void(bool, const nlohmann::json&)> done) {
        const auto client = state->clients.get(module);
        if (!client) {
            done(false, nlohmann::json());   // unloading
            return;
        }
        client->invokeAsyncResult(
            "deliver", args,
            [done](nlohmann::json reply, const logos::CallError& error) {
                done(error.ok(), reply);
            },
            kDeliverTimeoutMs);
    };
    out.settled = [this](const std::string& handle) { settled(handle); };
    out.workAvailable = [this](std::uint64_t counter) { work_available(counter); };

    m_state->service = std::make_unique<intents::IntentsService>(std::move(out), &steadyMs, &mintId);

    // Deadlines and the refusal floor. Events go out with no lock held.
    m_state->timer = std::thread([state]() {
        std::unique_lock<std::mutex> lock(state->timerMutex);
        while (!state->stopping) {
            state->timerCv.wait_for(lock, std::chrono::milliseconds(kTickMs));
            if (state->stopping) break;
            lock.unlock();
            guarded("tick", [&] { state->service->tick(); return std::string(); });
            lock.lock();
        }
    });
}

IntentsModuleImpl::~IntentsModuleImpl()
{
    m_state->stop();
}

LogosShutdown IntentsModuleImpl::aboutToUnload()
{
    m_state->stop();
    return LogosShutdown::Synchronous;
}

std::string IntentsModuleImpl::request(const std::string& intent, const std::string& params_json,
                                       const std::string& click_token)
{
    return guarded("request", [&] { return m_state->service->request(intent, params_json, click_token); });
}

std::string IntentsModuleImpl::result(const std::string& handle, const std::string& receipt)
{
    return guarded("result", [&] { return m_state->service->result(handle, receipt); });
}

std::string IntentsModuleImpl::cancel(const std::string& handle, const std::string& receipt)
{
    return guarded("cancel", [&] { return m_state->service->cancel(handle, receipt); });
}

std::string IntentsModuleImpl::respond(const std::string& dispatch, bool ok,
                                       const std::string& data_json, const std::string& error)
{
    return guarded("respond", [&] { return m_state->service->respond(dispatch, ok, data_json, error); });
}

std::string IntentsModuleImpl::claim(const std::string& options_json)
{
    return guarded("claim", [&] { return m_state->service->claim(options_json); });
}

std::string IntentsModuleImpl::next_work(const std::string& secret)
{
    return guarded("nextWork", [&] { return m_state->service->nextWork(secret); });
}

std::string IntentsModuleImpl::answer(const std::string& secret, const std::string& work_id,
                                      const std::string& answer_json)
{
    return guarded("answer", [&] { return m_state->service->answer(secret, work_id, answer_json); });
}

std::string IntentsModuleImpl::register_click_token(const std::string& secret, const std::string& token,
                                                    const std::string& app, const std::string& intent)
{
    return guarded("registerClickToken", [&] { return m_state->service->registerClickToken(secret, token, app, intent); });
}

std::string IntentsModuleImpl::backend_registered(const std::string& secret, const std::string& app,
                                                  const std::string& module)
{
    return guarded("backendRegistered", [&] { return m_state->service->backendRegistered(secret, app, module); });
}

std::string IntentsModuleImpl::backend_unregistered(const std::string& secret, const std::string& app)
{
    return guarded("backendUnregistered", [&] { return m_state->service->backendUnregistered(secret, app); });
}

std::string IntentsModuleImpl::set_installed(const std::string& secret, const std::string& packages_json)
{
    return guarded("setInstalled", [&] { return m_state->service->setInstalled(secret, packages_json); });
}

std::string IntentsModuleImpl::set_installable(const std::string& secret, const std::string& catalog_json)
{
    return guarded("setInstallable", [&] { return m_state->service->setInstallable(secret, catalog_json); });
}

std::string IntentsModuleImpl::declare_shell(const std::string& secret, const std::string& shell_json)
{
    return guarded("declareShell", [&] { return m_state->service->declareShell(secret, shell_json); });
}

std::string IntentsModuleImpl::configure_provider(const std::string& secret, const std::string& intent,
                                                  const std::string& provider)
{
    return guarded("configureProvider", [&] { return m_state->service->configureProvider(secret, intent, provider); });
}

std::string IntentsModuleImpl::shell_request(const std::string& secret, const std::string& intent,
                                             const std::string& params_json, bool as_link)
{
    return guarded("shellRequest", [&] { return m_state->service->shellRequest(secret, intent, params_json, as_link); });
}
