#pragma once

#include "Module.h"

#include <interfaces/IES1BenchmarkAsync.h>

#include <deque>
#include <functional>
#include <mutex>

namespace WPEFramework {
namespace Plugin {

    class ES1BenchmarkAsyncImplementation : public Exchange::IES1BenchmarkAsync {
    public:
        using Notification = Exchange::IES1BenchmarkAsync::INotification;

        ES1BenchmarkAsyncImplementation();
        ~ES1BenchmarkAsyncImplementation() override;

        ES1BenchmarkAsyncImplementation(const ES1BenchmarkAsyncImplementation&) = delete;
        ES1BenchmarkAsyncImplementation& operator=(const ES1BenchmarkAsyncImplementation&) = delete;

        BEGIN_INTERFACE_MAP(ES1BenchmarkAsyncImplementation)
            INTERFACE_ENTRY(Exchange::IES1BenchmarkAsync)
        END_INTERFACE_MAP

        // The clientid index only matters to Thunder's own JSON-RPC event
        // dispatch (it is what lets a channel register@<clientid> and only
        // ever receive that client's own notifications). The native sink
        // registered here is always the single internal helper the generated
        // J<interface>.h installs on behalf of the plugin shell, listening
        // for every value - so it is accepted but not used to key storage.
        Core::hresult Register(Notification* const notification, const Core::OptionalType<string>& clientid) override;
        Core::hresult Unregister(const Notification* notification, const Core::OptionalType<string>& clientid) override;

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
        using Job = std::function<void()>;

        class AsyncQueue : private Core::WorkerPool::JobType<AsyncQueue&> {
        public:
            explicit AsyncQueue(ES1BenchmarkAsyncImplementation& parent);
            ~AsyncQueue();

            void Submit(const Job& job);
            void Cancel();

        private:
            friend class Core::ThreadPool::JobType<AsyncQueue&>;
            void Dispatch();

            ES1BenchmarkAsyncImplementation& _parent;
            std::mutex _lock;
            std::deque<Job> _jobs;
            bool _scheduled;
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

        void NotifySetString(string clientid, bool success);
        void NotifyGetString(string clientid, const string& value);
        void NotifySetArray(string clientid, bool success);
        void NotifyGetArray(string clientid, const std::vector<uint8_t>& value);
        void NotifySetMixedArray(string clientid, bool success);
        void NotifyGetMixedArray(string clientid, const std::vector<MixedElement>& value);
        void NotifySetNestedObjects(string clientid, bool success);
        void NotifyGetNestedObjects(string clientid, const std::vector<NestedObject>& value);
        void NotifySetUint32(string clientid, bool success);
        void NotifyGetUint32(string clientid, uint32_t value);
        void NotifySetUint64(string clientid, bool success);
        void NotifyGetUint64(string clientid, uint64_t value);
        void NotifySetBool(string clientid, bool success);
        void NotifyGetBool(string clientid, bool value);
        void NotifySetFloat(string clientid, bool success);
        void NotifyGetFloat(string clientid, float value);
        void NotifySetDouble(string clientid, bool success);
        void NotifyGetDouble(string clientid, double value);
        void NotifyMeasureCopyCost(string clientid, uint64_t microseconds);
        void NotifyMeasureStringResizeCost(string clientid, uint64_t microseconds);
        void NotifyMeasureMixedAssignCost(string clientid, uint64_t microseconds);
        void NotifyMeasureNestedAssignCost(string clientid, uint64_t microseconds);

        Core::CriticalSection _adminLock;
        Notification* _notification;
        AsyncQueue _queue;
    };

} // namespace Plugin
} // namespace WPEFramework
