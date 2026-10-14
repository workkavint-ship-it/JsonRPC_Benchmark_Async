#pragma once

#include "Module.h"

#include <interfaces/IES1BenchmarkAsync.h>
#include <interfaces/json/JES1BenchmarkAsync.h>

#include <thread>

namespace WPEFramework {
namespace Plugin {

    class ES1BenchmarkAsync : public PluginHost::IPlugin
                             , public PluginHost::JSONRPC
                             , public Exchange::IES1BenchmarkAsync {
    public:
        using Notification = Exchange::IES1BenchmarkAsync::INotification;

        ES1BenchmarkAsync();
        ~ES1BenchmarkAsync() override = default;

        ES1BenchmarkAsync(const ES1BenchmarkAsync&) = delete;
        ES1BenchmarkAsync& operator=(const ES1BenchmarkAsync&) = delete;

        class Config : public Core::JSON::Container {
        public:
            Config()
                : Core::JSON::Container()
                , NotifyHost("127.0.0.1")
                , NotifyPort(8080)
                , ThunderHost("127.0.0.1")
                , ThunderPort(55555)
            {
                Add(_T("notifyhost"), &NotifyHost);
                Add(_T("notifyport"), &NotifyPort);
                Add(_T("thunderhost"), &ThunderHost);
                Add(_T("thunderport"), &ThunderPort);
            }

            Core::JSON::String NotifyHost;
            Core::JSON::DecUInt16 NotifyPort;
            Core::JSON::String ThunderHost;
            Core::JSON::DecUInt16 ThunderPort;
        };

        BEGIN_INTERFACE_MAP(ES1BenchmarkAsync)
            INTERFACE_ENTRY(PluginHost::IPlugin)
            INTERFACE_ENTRY(PluginHost::IDispatcher)
            INTERFACE_ENTRY(Exchange::IES1BenchmarkAsync)
        END_INTERFACE_MAP

        // IPlugin
        const string Initialize(PluginHost::IShell* service) override;
        void Deinitialize(PluginHost::IShell* service) override;
        string Information() const override;

        // Exchange::IES1BenchmarkAsync - delegates to the out-of-process
        // _implementation. Not an INTERFACE_AGGREGATE on purpose: being the
        // concrete implementer lets this class own the NotificationBridge
        // below and explicitly drive notification forwarding and
        // out-of-process crash detection, instead of relying on whatever
        // the generated Register(parent, implementation) helper does
        // internally (which could never be verified/observed directly).
        Core::hresult Register(Notification* const notification) override;
        Core::hresult Unregister(const Notification* notification) override;

        Core::hresult SetString(const string& clientid, const string& value) override;
        Core::hresult GetString(const string& clientid, const uint32_t size) override;
        Core::hresult SetArray(const string& clientid, const std::vector<uint8_t>& value) override;
        Core::hresult GetArray(const string& clientid, const uint32_t size) override;
        Core::hresult SetMixedArray(const string& clientid, const std::vector<MixedElement>& value) override;
        Core::hresult GetMixedArray(const string& clientid, const uint32_t count) override;
        Core::hresult SetNestedObjects(const string& clientid, const std::vector<NestedObject>& value) override;
        Core::hresult GetNestedObjects(const string& clientid, const uint32_t count) override;
        Core::hresult SetUint32(const string& clientid, const uint32_t value) override;
        Core::hresult GetUint32(const string& clientid) override;
        Core::hresult SetUint64(const string& clientid, const uint64_t value) override;
        Core::hresult GetUint64(const string& clientid) override;
        Core::hresult SetBool(const string& clientid, const bool value) override;
        Core::hresult GetBool(const string& clientid) override;
        Core::hresult SetFloat(const string& clientid, const float value) override;
        Core::hresult GetFloat(const string& clientid) override;
        Core::hresult SetDouble(const string& clientid, const double value) override;
        Core::hresult GetDouble(const string& clientid) override;
        Core::hresult MeasureCopyCost(const string& clientid, const uint32_t size) override;
        Core::hresult MeasureStringResizeCost(const string& clientid, const uint32_t size) override;
        Core::hresult MeasureMixedAssignCost(const string& clientid, const uint32_t count) override;
        Core::hresult MeasureNestedAssignCost(const string& clientid, const uint32_t count) override;

    private:
        // Sits between the out-of-process Implementation and this shell:
        // registered directly on _implementation (native COM-RPC sink) so
        // every completion is observed here explicitly rather than assumed,
        // and separately registered on the IShell as an
        // RPC::IRemoteConnection::INotification so a crash/exit of the
        // out-of-process Implementation is actually observed instead of
        // silently leaving the plugin in a dead state.
        class NotificationBridge : public Exchange::IES1BenchmarkAsync::INotification
                                  , public RPC::IRemoteConnection::INotification {
        public:
            explicit NotificationBridge(ES1BenchmarkAsync& parent) : _parent(parent) {}
            ~NotificationBridge() override = default;

            NotificationBridge(const NotificationBridge&) = delete;
            NotificationBridge& operator=(const NotificationBridge&) = delete;

            BEGIN_INTERFACE_MAP(NotificationBridge)
                INTERFACE_ENTRY(Exchange::IES1BenchmarkAsync::INotification)
                INTERFACE_ENTRY(RPC::IRemoteConnection::INotification)
            END_INTERFACE_MAP

            void SetStringCompleted(const string& clientid, const bool success) override;
            void GetStringCompleted(const string& clientid, const string& value) override;
            void SetArrayCompleted(const string& clientid, const bool success) override;
            void GetArrayCompleted(const string& clientid, const std::vector<uint8_t>& value) override;
            void SetMixedArrayCompleted(const string& clientid, const bool success) override;
            void GetMixedArrayCompleted(const string& clientid, const std::vector<MixedElement>& value) override;
            void SetNestedObjectsCompleted(const string& clientid, const bool success) override;
            void GetNestedObjectsCompleted(const string& clientid, const std::vector<NestedObject>& value) override;
            void SetUint32Completed(const string& clientid, const bool success) override;
            void GetUint32Completed(const string& clientid, const uint32_t value) override;
            void SetUint64Completed(const string& clientid, const bool success) override;
            void GetUint64Completed(const string& clientid, const uint64_t value) override;
            void SetBoolCompleted(const string& clientid, const bool success) override;
            void GetBoolCompleted(const string& clientid, const bool value) override;
            void SetFloatCompleted(const string& clientid, const bool success) override;
            void GetFloatCompleted(const string& clientid, const float value) override;
            void SetDoubleCompleted(const string& clientid, const bool success) override;
            void GetDoubleCompleted(const string& clientid, const double value) override;
            void MeasureCopyCostCompleted(const string& clientid, const uint64_t microseconds) override;
            void MeasureStringResizeCostCompleted(const string& clientid, const uint64_t microseconds) override;
            void MeasureMixedAssignCostCompleted(const string& clientid, const uint64_t microseconds) override;
            void MeasureNestedAssignCostCompleted(const string& clientid, const uint64_t microseconds) override;

            void Activated(RPC::IRemoteConnection* connection) override;
            void Deactivated(RPC::IRemoteConnection* connection) override;
            void Terminated(RPC::IRemoteConnection* connection) override;

        private:
            ES1BenchmarkAsync& _parent;
        };

        template <typename ACTION>
        void NotifySink(const ACTION& action)
        {
            Notification* notification = nullptr;
            _adminLock.Lock();
            if (_notification != nullptr) {
                notification = _notification;
                notification->AddRef();
            }
            _adminLock.Unlock();

            if (notification != nullptr) {
                action(notification);
                notification->Release();
            }
        }

        void NotifySetString(const string& clientid, bool success);
        void NotifyGetString(const string& clientid, const string& value);
        void NotifySetArray(const string& clientid, bool success);
        void NotifyGetArray(const string& clientid, const std::vector<uint8_t>& value);
        void NotifySetMixedArray(const string& clientid, bool success);
        void NotifyGetMixedArray(const string& clientid, const std::vector<MixedElement>& value);
        void NotifySetNestedObjects(const string& clientid, bool success);
        void NotifyGetNestedObjects(const string& clientid, const std::vector<NestedObject>& value);
        void NotifySetUint32(const string& clientid, bool success);
        void NotifyGetUint32(const string& clientid, uint32_t value);
        void NotifySetUint64(const string& clientid, bool success);
        void NotifyGetUint64(const string& clientid, uint64_t value);
        void NotifySetBool(const string& clientid, bool success);
        void NotifyGetBool(const string& clientid, bool value);
        void NotifySetFloat(const string& clientid, bool success);
        void NotifyGetFloat(const string& clientid, float value);
        void NotifySetDouble(const string& clientid, bool success);
        void NotifyGetDouble(const string& clientid, double value);
        void NotifyMeasureCopyCost(const string& clientid, uint64_t microseconds);
        void NotifyMeasureStringResizeCost(const string& clientid, uint64_t microseconds);
        void NotifyMeasureMixedAssignCost(const string& clientid, uint64_t microseconds);
        void NotifyMeasureNestedAssignCost(const string& clientid, uint64_t microseconds);

        Core::CriticalSection _adminLock;
        Notification* _notification;
        PluginHost::IShell* _service;
        Exchange::IES1BenchmarkAsync* _implementation;
        uint32_t _connectionId;
        // Core::SinkType<> adds safe AddRef/Release for a sink that is a
        // plain member of a larger object (never deletes itself - see
        // Thunder/Source/core/Services.h). BEGIN_INTERFACE_MAP alone only
        // generates QueryInterface, not AddRef/Release, so NotificationBridge
        // on its own would not satisfy Core::IUnknown.
        Core::SinkType<NotificationBridge> _bridge;
        bool _jsonRegistered;
        std::thread _notifyThread;
    };

} // namespace Plugin
} // namespace WPEFramework
