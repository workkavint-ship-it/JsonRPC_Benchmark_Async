#include "ES1BenchmarkAsync.h"

#include <cerrno>
#include <fcntl.h>
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace WPEFramework {
namespace Plugin {

    namespace {
        static Metadata<ES1BenchmarkAsync> metadata(1, 0, 0, {}, {}, {});

        const string& StringBuffer()
        {
            static const string value(4 * 1024 * 1024, 'A');
            return value;
        }

        const std::vector<uint8_t>& ByteBuffer()
        {
            static const std::vector<uint8_t> value(256 * 1024, 0xAA);
            return value;
        }

        const std::vector<Exchange::IES1BenchmarkAsync::MixedElement>& MixedValues()
        {
            static const std::vector<Exchange::IES1BenchmarkAsync::MixedElement> value = [] {
                std::vector<Exchange::IES1BenchmarkAsync::MixedElement> result;
                result.reserve(4228);
                for (uint32_t index = 0; index < 4228; ++index) {
                    result.push_back({ index, "item" + std::to_string(index), index * 3.14159, (index % 2) == 0 });
                }
                return result;
            }();
            return value;
        }

        const std::vector<Exchange::IES1BenchmarkAsync::NestedObject>& NestedValues()
        {
            static const std::vector<Exchange::IES1BenchmarkAsync::NestedObject> value = [] {
                std::vector<Exchange::IES1BenchmarkAsync::NestedObject> result;
                result.reserve(1736);
                for (uint32_t index = 0; index < 1736; ++index) {
                    Exchange::IES1BenchmarkAsync::NestedObject object;
                    object.id = index;
                    object.flag = (index % 2) == 0;
                    object.score = index * 3.14159;
                    object.data.label = "item" + std::to_string(index);
                    object.data.nested.count = index;
                    object.data.nested.inner.value = index * 3;
                    object.data.nested.inner.name = "deep" + std::to_string(index);
                    result.push_back(object);
                }
                return result;
            }();
            return value;
        }

        const uint32_t StaticUint32 = 0xFFFFFFFFu;
        const uint64_t StaticUint64 = 0xFFFFFFFFFFFFFFFFull;
        const bool StaticBool = true;
        const float StaticFloat = 3.4028235e+38f;
        const double StaticDouble = 1.7976931348623157e+308;

        uint64_t UnixMicroseconds()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
    }

    ES1BenchmarkAsync::AsyncQueue::AsyncQueue(ES1BenchmarkAsync& parent)
        : Core::WorkerPool::JobType<AsyncQueue&>(*this)
        , _parent(parent)
        , _lock()
        , _jobs()
        , _scheduled(false)
    {
    }

    ES1BenchmarkAsync::AsyncQueue::~AsyncQueue()
    {
        Cancel();
    }

    void ES1BenchmarkAsync::AsyncQueue::Submit(const Job& job)
    {
        std::lock_guard<std::mutex> lock(_lock);
        _jobs.push_back(job);
        if (_scheduled == false) {
            _scheduled = true;
            JobType::Submit();
        }
    }

    void ES1BenchmarkAsync::AsyncQueue::Cancel()
    {
        {
            std::lock_guard<std::mutex> lock(_lock);
            _jobs.clear();
            _scheduled = false;
        }
        JobType::Revoke();
    }

    void ES1BenchmarkAsync::AsyncQueue::Dispatch()
    {
        Job job;
        {
            std::lock_guard<std::mutex> lock(_lock);
            if (_jobs.empty() == false) {
                job = std::move(_jobs.front());
                _jobs.pop_front();
            } else {
                _scheduled = false;
            }
        }

        if (job) {
            job();
        }

        std::lock_guard<std::mutex> lock(_lock);
        if (_jobs.empty() == false) {
            JobType::Submit();
        } else {
            _scheduled = false;
        }
    }

    ES1BenchmarkAsync::ES1BenchmarkAsync()
        : _adminLock()
        , _notification(nullptr)
        , _active(false)
        , _queue(*this)
    {
    }

    const string ES1BenchmarkAsync::Initialize(PluginHost::IShell* service)
    {
        Exchange::JES1BenchmarkAsync::Register(*this, this);

        _adminLock.Lock();
        _active = true;
        _adminLock.Unlock();

        Config config;
        config.FromString(service->ConfigLine());
        const string notifyHost = config.NotifyHost.Value();
        const uint16_t notifyPort = config.NotifyPort.Value();
        const string thunderHost = config.ThunderHost.Value();
        const uint16_t thunderPort = config.ThunderPort.Value();

        if ((notifyPort != 0) && (thunderPort != 0)) {
            _notifyThread = std::thread([notifyHost, notifyPort, thunderHost, thunderPort] {
                char request[512];
                ::snprintf(request, sizeof(request),
                    "GET /trigger?host=%s&port=%u&boot_time_us=%llu HTTP/1.0\r\n"
                    "Host: %s\r\n\r\n",
                    thunderHost.c_str(),
                    static_cast<unsigned>(thunderPort),
                    static_cast<unsigned long long>(UnixMicroseconds()),
                    notifyHost.c_str());

                const int socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (socketFd < 0) {
                    return;
                }

                sockaddr_in address {};
                address.sin_family = AF_INET;
                address.sin_port = ::htons(notifyPort);
                address.sin_addr.s_addr = ::inet_addr(notifyHost.c_str());

                const int flags = ::fcntl(socketFd, F_GETFL, 0);
                ::fcntl(socketFd, F_SETFL, flags | O_NONBLOCK);
                int connected = (::connect(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
                if (!connected && errno == EINPROGRESS) {
                    pollfd descriptor { socketFd, POLLOUT, 0 };
                    if (::poll(&descriptor, 1, 2000) > 0) {
                        int error = 0;
                        socklen_t length = sizeof(error);
                        connected = (::getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &error, &length) == 0) && (error == 0);
                    }
                }
                if (connected) {
                    ::send(socketFd, request, ::strlen(request), 0);
                }
                ::close(socketFd);
            });
        }

        return string();
    }

    void ES1BenchmarkAsync::Deinitialize(PluginHost::IShell* /* service */)
    {
        _adminLock.Lock();
        _active = false;
        Notification* notification = _notification;
        _notification = nullptr;
        _adminLock.Unlock();

        _queue.Cancel();
        Exchange::JES1BenchmarkAsync::Unregister(*this);

        if (_notifyThread.joinable()) {
            _notifyThread.join();
        }
        if (notification != nullptr) {
            notification->Release();
        }
    }

    string ES1BenchmarkAsync::Information() const
    {
        return string("ES1 asynchronous JSON-RPC benchmark plugin");
    }

    Core::hresult ES1BenchmarkAsync::Register(Notification* const notification)
    {
        if (notification == nullptr) {
            return Core::ERROR_BAD_REQUEST;
        }

        _adminLock.Lock();
        if (_notification != nullptr) {
            const bool same = (_notification == notification);
            _adminLock.Unlock();
            return same ? Core::ERROR_NONE : Core::ERROR_ALREADY_CONNECTED;
        }
        notification->AddRef();
        _notification = notification;
        _adminLock.Unlock();
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::Unregister(const Notification* notification)
    {
        Notification* registered = nullptr;
        _adminLock.Lock();
        if (_notification == notification) {
            registered = _notification;
            _notification = nullptr;
        }
        _adminLock.Unlock();

        if (registered == nullptr) {
            return Core::ERROR_UNKNOWN_KEY;
        }
        registered->Release();
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::SetString(const string& /* value */)
    {
        _queue.Submit([this] { NotifySetString(true); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::GetString(const uint32_t size)
    {
        if (size > StringBuffer().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, size] { NotifyGetString(StringBuffer().substr(0, size)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::SetArray(const std::vector<uint8_t>& /* value */)
    {
        _queue.Submit([this] { NotifySetArray(true); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::GetArray(const uint32_t size)
    {
        if (size > ByteBuffer().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, size] { NotifyGetArray(std::vector<uint8_t>(ByteBuffer().begin(), ByteBuffer().begin() + size)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::SetMixedArray(const std::vector<MixedElement>& /* value */)
    {
        _queue.Submit([this] { NotifySetMixedArray(true); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::GetMixedArray(const uint32_t count)
    {
        if (count > MixedValues().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, count] { NotifyGetMixedArray(std::vector<MixedElement>(MixedValues().begin(), MixedValues().begin() + count)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::SetNestedObjects(const std::vector<NestedObject>& /* value */)
    {
        _queue.Submit([this] { NotifySetNestedObjects(true); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::GetNestedObjects(const uint32_t count)
    {
        if (count > NestedValues().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, count] { NotifyGetNestedObjects(std::vector<NestedObject>(NestedValues().begin(), NestedValues().begin() + count)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::SetUint32(const uint32_t /* value */) { _queue.Submit([this] { NotifySetUint32(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsync::GetUint32() { _queue.Submit([this] { NotifyGetUint32(StaticUint32); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsync::SetUint64(const uint64_t /* value */) { _queue.Submit([this] { NotifySetUint64(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsync::GetUint64() { _queue.Submit([this] { NotifyGetUint64(StaticUint64); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsync::SetBool(const bool /* value */) { _queue.Submit([this] { NotifySetBool(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsync::GetBool() { _queue.Submit([this] { NotifyGetBool(StaticBool); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsync::SetFloat(const float /* value */) { _queue.Submit([this] { NotifySetFloat(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsync::GetFloat() { _queue.Submit([this] { NotifyGetFloat(StaticFloat); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsync::SetDouble(const double /* value */) { _queue.Submit([this] { NotifySetDouble(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsync::GetDouble() { _queue.Submit([this] { NotifyGetDouble(StaticDouble); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsync::MeasureCopyCost(const uint32_t size)
    {
        if (size > ByteBuffer().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, size] {
            std::vector<uint8_t> destination(size);
            const auto start = std::chrono::steady_clock::now();
            if (size != 0) std::memcpy(destination.data(), ByteBuffer().data(), size);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureCopyCost(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::MeasureStringResizeCost(const uint32_t size)
    {
        if (size > StringBuffer().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, size] {
            string destination;
            const auto start = std::chrono::steady_clock::now();
            destination.resize(size);
            if (size != 0) std::memcpy(&destination[0], StringBuffer().data(), size);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureStringResizeCost(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::MeasureMixedAssignCost(const uint32_t count)
    {
        if (count > MixedValues().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, count] {
            std::vector<MixedElement> destination;
            const auto start = std::chrono::steady_clock::now();
            destination.assign(MixedValues().begin(), MixedValues().begin() + count);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureMixedAssignCost(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::MeasureNestedAssignCost(const uint32_t count)
    {
        if (count > NestedValues().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, count] {
            std::vector<NestedObject> destination;
            const auto start = std::chrono::steady_clock::now();
            destination.assign(NestedValues().begin(), NestedValues().begin() + count);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureNestedAssignCost(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

    #define NOTIFY_BOOL(name) \
        void ES1BenchmarkAsync::Notify##name(const bool value) \
        { \
            NotifySink([value](Notification* notification) { notification->name##Completed(value); }); \
            Exchange::JES1BenchmarkAsync::Event::name##Completed(*this, value); \
        }

    NOTIFY_BOOL(SetString)
    NOTIFY_BOOL(SetArray)
    NOTIFY_BOOL(SetMixedArray)
    NOTIFY_BOOL(SetNestedObjects)
    NOTIFY_BOOL(SetUint32)
    NOTIFY_BOOL(SetUint64)
    NOTIFY_BOOL(SetBool)
    NOTIFY_BOOL(SetFloat)
    NOTIFY_BOOL(SetDouble)

    #undef NOTIFY_BOOL

    #define NOTIFY_VALUE(name, type) \
        void ES1BenchmarkAsync::Notify##name(const type value) \
        { \
            NotifySink([value](Notification* notification) { notification->name##Completed(value); }); \
            Exchange::JES1BenchmarkAsync::Event::name##Completed(*this, value); \
        }

    NOTIFY_VALUE(GetUint32, uint32_t)
    NOTIFY_VALUE(GetUint64, uint64_t)
    NOTIFY_VALUE(GetBool, bool)
    NOTIFY_VALUE(GetFloat, float)
    NOTIFY_VALUE(GetDouble, double)
    NOTIFY_VALUE(MeasureCopyCost, uint64_t)
    NOTIFY_VALUE(MeasureStringResizeCost, uint64_t)
    NOTIFY_VALUE(MeasureMixedAssignCost, uint64_t)
    NOTIFY_VALUE(MeasureNestedAssignCost, uint64_t)

    #undef NOTIFY_VALUE

    void ES1BenchmarkAsync::NotifyGetString(const string& value)
    {
        NotifySink([&value](Notification* notification) { notification->GetStringCompleted(value); });
        Exchange::JES1BenchmarkAsync::Event::GetStringCompleted(*this, value);
    }

    void ES1BenchmarkAsync::NotifyGetArray(const std::vector<uint8_t>& value)
    {
        NotifySink([&value](Notification* notification) { notification->GetArrayCompleted(value); });
        Exchange::JES1BenchmarkAsync::Event::GetArrayCompleted(*this, value);
    }

    void ES1BenchmarkAsync::NotifyGetMixedArray(const std::vector<MixedElement>& value)
    {
        NotifySink([&value](Notification* notification) { notification->GetMixedArrayCompleted(value); });
        Exchange::JES1BenchmarkAsync::Event::GetMixedArrayCompleted(*this, value);
    }

    void ES1BenchmarkAsync::NotifyGetNestedObjects(const std::vector<NestedObject>& value)
    {
        NotifySink([&value](Notification* notification) { notification->GetNestedObjectsCompleted(value); });
        Exchange::JES1BenchmarkAsync::Event::GetNestedObjectsCompleted(*this, value);
    }

} // namespace Plugin
} // namespace WPEFramework