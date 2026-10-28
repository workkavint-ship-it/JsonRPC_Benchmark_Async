#pragma once

#include "Module.h"

#include <interfaces/IES1BenchmarkAsync.h>

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

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

        // @index lives only on the notification's clientid parameter (not
        // on Register/Unregister - that placement is optional per Thunder's
        // own docs, and the plain single-sink form here is the one already
        // proven to work). The clientid-based filtering that makes each
        // channel only ever receive its own notifications happens entirely
        // in Thunder's generated JSON-RPC event dispatch, driven by each
        // client's register@<clientid> call - not by this native Register.
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
        // Each call gets its own independent, disposable job submitted
        // directly to Thunder's shared worker pool, rather than being
        // funneled through a single reusable Core::WorkerPool::JobType<>
        // slot. A single JobType<> instance holds exactly one state
        // (IDLE/SUBMITTED/EXECUTING/...) at a time, so it can only ever
        // represent one pending/in-flight job - that serialized every
        // caller through one shared delay, meaning N concurrent callers
        // never actually got N independent delays, just one delay for the
        // whole batch. Submitting each call's own CallJob independently
        // lets up to N calls each genuinely wait out their own delay
        // concurrently, bounded only by the worker pool's own thread count.
        //
        // The 3s heavy-work delay is simulated with a blocking sleep inside
        // Dispatch() itself, so it occupies a worker-pool thread for the
        // full 3s - jobs are handed to the pool immediately via Submit(),
        // not parked on Thunder's internal timer. With root.threads=32,
        // up to 32 of these can run their 3s sleep concurrently.
        class CallJob : public Core::IDispatch {
        public:
            CallJob(ES1BenchmarkAsyncImplementation& parent, std::function<void()>&& work)
                : _parent(parent)
                , _work(std::move(work))
            {
            }
            CallJob(const CallJob&) = delete;
            CallJob& operator=(const CallJob&) = delete;
            ~CallJob() override = default;

            void Dispatch() override
            {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                if (_work) {
                    _work();
                }
                _parent.UntrackJob(this);
            }

        private:
            ES1BenchmarkAsyncImplementation& _parent;
            std::function<void()> _work;
        };

        // Submits work to run immediately on the worker pool, as its own
        // independent job (see CallJob above) - the heavy-work delay lives
        // inside CallJob::Dispatch() itself, not in how it's scheduled.
        void SubmitDelayed(std::function<void()>&& work);
        // Called by a CallJob once it has run, to drop it from
        // _pendingJobs - keeps that list bounded to jobs that are still
        // actually outstanding, not ones that already completed.
        void UntrackJob(const Core::IDispatch* job);

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
        Core::CriticalSection _jobsLock;
        std::vector<Core::ProxyType<Core::IDispatch>> _pendingJobs;
    };

} // namespace Plugin
} // namespace WPEFramework
