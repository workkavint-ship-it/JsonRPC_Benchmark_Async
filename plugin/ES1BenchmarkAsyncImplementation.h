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

        Core::hresult Register(Notification* const notification) override;
        Core::hresult Unregister(const Notification* notification) override;

        Core::hresult SetString(const string& value) override;
        Core::hresult GetString(const uint32_t size) override;
        Core::hresult SetArray(const std::vector<uint8_t>& value) override;
        Core::hresult GetArray(const uint32_t size) override;
        Core::hresult SetMixedArray(const std::vector<MixedElement>& value) override;
        Core::hresult GetMixedArray(const uint32_t count) override;
        Core::hresult SetNestedObjects(const std::vector<NestedObject>& value) override;
        Core::hresult GetNestedObjects(const uint32_t count) override;
        Core::hresult SetUint32(const uint32_t value) override;
        Core::hresult GetUint32() override;
        Core::hresult SetUint64(const uint64_t value) override;
        Core::hresult GetUint64() override;
        Core::hresult SetBool(const bool value) override;
        Core::hresult GetBool() override;
        Core::hresult SetFloat(const float value) override;
        Core::hresult GetFloat() override;
        Core::hresult SetDouble(const double value) override;
        Core::hresult GetDouble() override;
        Core::hresult MeasureCopyCost(const uint32_t size) override;
        Core::hresult MeasureStringResizeCost(const uint32_t size) override;
        Core::hresult MeasureMixedAssignCost(const uint32_t count) override;
        Core::hresult MeasureNestedAssignCost(const uint32_t count) override;

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

        void NotifySetString(bool success);
        void NotifyGetString(const string& value);
        void NotifySetArray(bool success);
        void NotifyGetArray(const std::vector<uint8_t>& value);
        void NotifySetMixedArray(bool success);
        void NotifyGetMixedArray(const std::vector<MixedElement>& value);
        void NotifySetNestedObjects(bool success);
        void NotifyGetNestedObjects(const std::vector<NestedObject>& value);
        void NotifySetUint32(bool success);
        void NotifyGetUint32(uint32_t value);
        void NotifySetUint64(bool success);
        void NotifyGetUint64(uint64_t value);
        void NotifySetBool(bool success);
        void NotifyGetBool(bool value);
        void NotifySetFloat(bool success);
        void NotifyGetFloat(float value);
        void NotifySetDouble(bool success);
        void NotifyGetDouble(double value);
        void NotifyMeasureCopyCost(uint64_t microseconds);
        void NotifyMeasureStringResizeCost(uint64_t microseconds);
        void NotifyMeasureMixedAssignCost(uint64_t microseconds);
        void NotifyMeasureNestedAssignCost(uint64_t microseconds);

        Core::CriticalSection _adminLock;
        Notification* _notification;
        AsyncQueue _queue;
    };

} // namespace Plugin
} // namespace WPEFramework
