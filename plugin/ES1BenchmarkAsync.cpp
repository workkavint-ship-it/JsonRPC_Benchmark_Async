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

        uint64_t UnixMicroseconds()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
    }

    ES1BenchmarkAsync::ES1BenchmarkAsync()
        : _adminLock()
        , _notification(nullptr)
        , _service(nullptr)
        , _implementation(nullptr)
        , _connectionId(0)
        , _bridge(*this)
        , _jsonRegistered(false)
        , _notifyThread()
    {
    }

    const string ES1BenchmarkAsync::Initialize(PluginHost::IShell* service)
    {
        _service = service;
        _service->AddRef();
        _service->Register(static_cast<RPC::IRemoteConnection::INotification*>(&_bridge));

        _implementation = _service->Root<Exchange::IES1BenchmarkAsync>(
            _connectionId, 2000, _T("ES1BenchmarkAsyncImplementation"));
        if (_implementation == nullptr) {
            _service->Unregister(static_cast<RPC::IRemoteConnection::INotification*>(&_bridge));
            _service->Release();
            _service = nullptr;
            return _T("ES1BenchmarkAsync implementation could not be instantiated");
        }

        // Explicitly register the bridge as a native COM-RPC sink on the
        // out-of-process implementation, rather than only relying on
        // whatever Exchange::JES1BenchmarkAsync::Register(*this,
        // _implementation) does internally for notification forwarding -
        // the return value is logged so this is actually verifiable instead
        // of assumed.
        const Core::hresult bridgeResult = _implementation->Register(&_bridge);
        TRACE_L1("ES1BenchmarkAsync: bridge Register on implementation returned 0x%08X", bridgeResult);

        Exchange::JES1BenchmarkAsync::Register(*this, _implementation);
        _jsonRegistered = true;

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
                if (socketFd < 0) return;

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
                    const ssize_t sent = ::send(socketFd, request, ::strlen(request), 0);
                    if (sent < 0) {
                        ::close(socketFd);
                        return;
                    }
                }
                ::close(socketFd);
            });
        }

        return string();
    }

    void ES1BenchmarkAsync::Deinitialize(PluginHost::IShell* /* service */)
    {
        if (_jsonRegistered == true) {
            Exchange::JES1BenchmarkAsync::Unregister(*this);
            _jsonRegistered = false;
        }
        if (_service != nullptr) {
            _service->Unregister(static_cast<RPC::IRemoteConnection::INotification*>(&_bridge));
        }

        if (_implementation != nullptr) {
            _implementation->Unregister(&_bridge);
            RPC::IRemoteConnection* connection = (_service != nullptr) ? _service->RemoteConnection(_connectionId) : nullptr;
            const uint32_t result = _implementation->Release();
            _implementation = nullptr;
            if (connection != nullptr) {
                if (result == Core::ERROR_DESTRUCTION_SUCCEEDED) {
                    connection->Terminate();
                }
                connection->Release();
            }
        }

        if (_notifyThread.joinable()) {
            _notifyThread.join();
        }
        if (_notification != nullptr) {
            _notification->Release();
            _notification = nullptr;
        }
        if (_service != nullptr) {
            _service->Release();
            _service = nullptr;
        }
        _connectionId = 0;
    }

    string ES1BenchmarkAsync::Information() const
    {
        return string("ES1 asynchronous JSON-RPC benchmark plugin");
    }

    Core::hresult ES1BenchmarkAsync::Register(Notification* const notification)
    {
        if (notification == nullptr) return Core::ERROR_BAD_REQUEST;
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
        if (registered == nullptr) return Core::ERROR_UNKNOWN_KEY;
        registered->Release();
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsync::SetString(const string& clientid, const string& value)
    { return (_implementation != nullptr ? _implementation->SetString(clientid, value) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::GetString(const string& clientid, const uint32_t size)
    { return (_implementation != nullptr ? _implementation->GetString(clientid, size) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::SetArray(const string& clientid, const std::vector<uint8_t>& value)
    { return (_implementation != nullptr ? _implementation->SetArray(clientid, value) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::GetArray(const string& clientid, const uint32_t size)
    { return (_implementation != nullptr ? _implementation->GetArray(clientid, size) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::SetMixedArray(const string& clientid, const std::vector<MixedElement>& value)
    { return (_implementation != nullptr ? _implementation->SetMixedArray(clientid, value) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::GetMixedArray(const string& clientid, const uint32_t count)
    { return (_implementation != nullptr ? _implementation->GetMixedArray(clientid, count) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::SetNestedObjects(const string& clientid, const std::vector<NestedObject>& value)
    { return (_implementation != nullptr ? _implementation->SetNestedObjects(clientid, value) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::GetNestedObjects(const string& clientid, const uint32_t count)
    { return (_implementation != nullptr ? _implementation->GetNestedObjects(clientid, count) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::SetUint32(const string& clientid, const uint32_t value)
    { return (_implementation != nullptr ? _implementation->SetUint32(clientid, value) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::GetUint32(const string& clientid)
    { return (_implementation != nullptr ? _implementation->GetUint32(clientid) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::SetUint64(const string& clientid, const uint64_t value)
    { return (_implementation != nullptr ? _implementation->SetUint64(clientid, value) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::GetUint64(const string& clientid)
    { return (_implementation != nullptr ? _implementation->GetUint64(clientid) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::SetBool(const string& clientid, const bool value)
    { return (_implementation != nullptr ? _implementation->SetBool(clientid, value) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::GetBool(const string& clientid)
    { return (_implementation != nullptr ? _implementation->GetBool(clientid) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::SetFloat(const string& clientid, const float value)
    { return (_implementation != nullptr ? _implementation->SetFloat(clientid, value) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::GetFloat(const string& clientid)
    { return (_implementation != nullptr ? _implementation->GetFloat(clientid) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::SetDouble(const string& clientid, const double value)
    { return (_implementation != nullptr ? _implementation->SetDouble(clientid, value) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::GetDouble(const string& clientid)
    { return (_implementation != nullptr ? _implementation->GetDouble(clientid) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::MeasureCopyCost(const string& clientid, const uint32_t size)
    { return (_implementation != nullptr ? _implementation->MeasureCopyCost(clientid, size) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::MeasureStringResizeCost(const string& clientid, const uint32_t size)
    { return (_implementation != nullptr ? _implementation->MeasureStringResizeCost(clientid, size) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::MeasureMixedAssignCost(const string& clientid, const uint32_t count)
    { return (_implementation != nullptr ? _implementation->MeasureMixedAssignCost(clientid, count) : Core::ERROR_ILLEGAL_STATE); }
    Core::hresult ES1BenchmarkAsync::MeasureNestedAssignCost(const string& clientid, const uint32_t count)
    { return (_implementation != nullptr ? _implementation->MeasureNestedAssignCost(clientid, count) : Core::ERROR_ILLEGAL_STATE); }

#define NOTIFY_BOOL(name) \
    void ES1BenchmarkAsync::Notify##name(const string& clientid, const bool value) \
    { \
        NotifySink([&](Notification* notification) { notification->name##Completed(clientid, value); }); \
        Exchange::JES1BenchmarkAsync::Event::name##Completed(*this, clientid, value); \
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
    void ES1BenchmarkAsync::Notify##name(const string& clientid, type value) \
    { \
        NotifySink([&](Notification* notification) { notification->name##Completed(clientid, value); }); \
        Exchange::JES1BenchmarkAsync::Event::name##Completed(*this, clientid, value); \
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

    void ES1BenchmarkAsync::NotifyGetString(const string& clientid, const string& value)
    {
        NotifySink([&](Notification* notification) { notification->GetStringCompleted(clientid, value); });
        Exchange::JES1BenchmarkAsync::Event::GetStringCompleted(*this, clientid, value);
    }
    void ES1BenchmarkAsync::NotifyGetArray(const string& clientid, const std::vector<uint8_t>& value)
    {
        NotifySink([&](Notification* notification) { notification->GetArrayCompleted(clientid, value); });
        Exchange::JES1BenchmarkAsync::Event::GetArrayCompleted(*this, clientid, value);
    }
    void ES1BenchmarkAsync::NotifyGetMixedArray(const string& clientid, const std::vector<MixedElement>& value)
    {
        NotifySink([&](Notification* notification) { notification->GetMixedArrayCompleted(clientid, value); });
        Exchange::JES1BenchmarkAsync::Event::GetMixedArrayCompleted(*this, clientid, value);
    }
    void ES1BenchmarkAsync::NotifyGetNestedObjects(const string& clientid, const std::vector<NestedObject>& value)
    {
        NotifySink([&](Notification* notification) { notification->GetNestedObjectsCompleted(clientid, value); });
        Exchange::JES1BenchmarkAsync::Event::GetNestedObjectsCompleted(*this, clientid, value);
    }

    void ES1BenchmarkAsync::NotificationBridge::SetStringCompleted(const string& clientid, const bool value) { _parent.NotifySetString(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::GetStringCompleted(const string& clientid, const string& value) { _parent.NotifyGetString(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::SetArrayCompleted(const string& clientid, const bool value) { _parent.NotifySetArray(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::GetArrayCompleted(const string& clientid, const std::vector<uint8_t>& value) { _parent.NotifyGetArray(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::SetMixedArrayCompleted(const string& clientid, const bool value) { _parent.NotifySetMixedArray(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::GetMixedArrayCompleted(const string& clientid, const std::vector<MixedElement>& value) { _parent.NotifyGetMixedArray(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::SetNestedObjectsCompleted(const string& clientid, const bool value) { _parent.NotifySetNestedObjects(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::GetNestedObjectsCompleted(const string& clientid, const std::vector<NestedObject>& value) { _parent.NotifyGetNestedObjects(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::SetUint32Completed(const string& clientid, const bool value) { _parent.NotifySetUint32(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::GetUint32Completed(const string& clientid, const uint32_t value) { _parent.NotifyGetUint32(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::SetUint64Completed(const string& clientid, const bool value) { _parent.NotifySetUint64(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::GetUint64Completed(const string& clientid, const uint64_t value) { _parent.NotifyGetUint64(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::SetBoolCompleted(const string& clientid, const bool value) { _parent.NotifySetBool(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::GetBoolCompleted(const string& clientid, const bool value) { _parent.NotifyGetBool(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::SetFloatCompleted(const string& clientid, const bool value) { _parent.NotifySetFloat(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::GetFloatCompleted(const string& clientid, const float value) { _parent.NotifyGetFloat(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::SetDoubleCompleted(const string& clientid, const bool value) { _parent.NotifySetDouble(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::GetDoubleCompleted(const string& clientid, const double value) { _parent.NotifyGetDouble(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::MeasureCopyCostCompleted(const string& clientid, const uint64_t value) { _parent.NotifyMeasureCopyCost(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::MeasureStringResizeCostCompleted(const string& clientid, const uint64_t value) { _parent.NotifyMeasureStringResizeCost(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::MeasureMixedAssignCostCompleted(const string& clientid, const uint64_t value) { _parent.NotifyMeasureMixedAssignCost(clientid, value); }
    void ES1BenchmarkAsync::NotificationBridge::MeasureNestedAssignCostCompleted(const string& clientid, const uint64_t value) { _parent.NotifyMeasureNestedAssignCost(clientid, value); }

    void ES1BenchmarkAsync::NotificationBridge::Activated(RPC::IRemoteConnection* connection)
    {
        TRACE_L1("ES1BenchmarkAsync: remote connection activated id=%u", connection != nullptr ? connection->Id() : 0);
    }

    void ES1BenchmarkAsync::NotificationBridge::Deactivated(RPC::IRemoteConnection* connection)
    {
        if (connection != nullptr) {
            TRACE(Trace::Error, (_T("ES1BenchmarkAsync: child process deactivated id=%u pid=%u"), connection->Id(), connection->RemoteId()));
        }
    }

    void ES1BenchmarkAsync::NotificationBridge::Terminated(RPC::IRemoteConnection* connection)
    {
        if (connection != nullptr) {
            TRACE_L1("ES1BenchmarkAsync: remote connection terminated id=%u", connection->Id());
        }
    }

} // namespace Plugin
} // namespace WPEFramework
