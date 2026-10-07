#pragma once

#include "Module.h"

#include <interfaces/IES1BenchmarkAsync.h>
#include <interfaces/json/JES1BenchmarkAsync.h>

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace WPEFramework {
namespace Plugin {

    class ES1BenchmarkAsync : public PluginHost::IPlugin
                             , public PluginHost::JSONRPC
                             , public Exchange::IES1BenchmarkAsync {
    public:
        ES1BenchmarkAsync(const ES1BenchmarkAsync&) = delete;
        ES1BenchmarkAsync& operator=(const ES1BenchmarkAsync&) = delete;

        ES1BenchmarkAsync() = default;
        ~ES1BenchmarkAsync() override = default;

        // Plugin configuration - read from ES1BenchmarkAsync.json on device.
        // Mirrors ES1Benchmark's coldstart notify config, so the async plugin
        // can participate in the same cold-boot-triggered benchmark workflow.
        class Config : public Core::JSON::Container {
        public:
            Config()
                : Core::JSON::Container()
                , NotifyHost("127.0.0.1")
                , NotifyPort(8080)
                , ThunderHost("127.0.0.1")
                , ThunderPort(55555)
            {
                Add(_T("notifyhost"),  &NotifyHost);
                Add(_T("notifyport"),  &NotifyPort);
                Add(_T("thunderhost"), &ThunderHost);
                Add(_T("thunderport"), &ThunderPort);
            }
            Core::JSON::String    NotifyHost;
            Core::JSON::DecUInt16 NotifyPort;
            Core::JSON::String    ThunderHost;
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

        // IES1BenchmarkAsync
        Core::hresult SetString(const string& value, ISetStringCallback* const callback) override;
        Core::hresult GetString(const uint32_t size, IGetStringCallback* const callback) override;
        Core::hresult SetArray(const std::vector<uint8_t>& value, ISetArrayCallback* const callback) override;
        Core::hresult GetArray(const uint32_t size, IGetArrayCallback* const callback) override;
        Core::hresult SetMixedArray(const std::vector<MixedElement>& value, ISetMixedCallback* const callback) override;
        Core::hresult GetMixedArray(const uint32_t count, IGetMixedCallback* const callback) override;
        Core::hresult SetNestedObjects(const std::vector<NestedObject>& value, ISetNestedCallback* const callback) override;
        Core::hresult GetNestedObjects(const uint32_t count, IGetNestedCallback* const callback) override;
        Core::hresult SetUint32(const uint32_t value, ISetUint32Callback* const callback) override;
        Core::hresult GetUint32(IGetUint32Callback* const callback) override;
        Core::hresult SetUint64(const uint64_t value, ISetUint64Callback* const callback) override;
        Core::hresult GetUint64(IGetUint64Callback* const callback) override;
        Core::hresult SetBool(const bool value, ISetBoolCallback* const callback) override;
        Core::hresult GetBool(IGetBoolCallback* const callback) override;
        Core::hresult SetFloat(const float value, ISetFloatCallback* const callback) override;
        Core::hresult GetFloat(IGetFloatCallback* const callback) override;
        Core::hresult SetDouble(const double value, ISetDoubleCallback* const callback) override;
        Core::hresult GetDouble(IGetDoubleCallback* const callback) override;
        Core::hresult MeasureCopyCost(const uint32_t size, IMeasureCopyCallback* const callback) override;
        Core::hresult MeasureStringResizeCost(const uint32_t size, IMeasureStringCallback* const callback) override;
        Core::hresult MeasureMixedAssignCost(const uint32_t count, IMeasureMixedCallback* const callback) override;
        Core::hresult MeasureNestedAssignCost(const uint32_t count, IMeasureNestedCallback* const callback) override;

    private:
        class CallJob {
        public:
            CallJob()
                : _job(*this)
                , _pending(nullptr)
                , _task()
            {
            }
            CallJob(const CallJob&) = delete;
            CallJob& operator=(const CallJob&) = delete;

            bool IsIdle() const
            {
                return _job.IsIdle();
            }

            bool Submit(std::atomic<uint32_t>& pending, std::function<void()>&& task)
            {
                if (IsIdle() == false) {
                    return false;
                }

                std::lock_guard<std::mutex> lock(_lock);
                _pending = &pending;
                _task = std::move(task);
                _job.Submit();
                return true;
            }

        private:
            friend Core::ThreadPool::JobType<CallJob&>;
            void Dispatch()
            {
                std::function<void()> task;
                std::atomic<uint32_t>* pending = nullptr;
                {
                    std::lock_guard<std::mutex> lock(_lock);
                    task = std::move(_task);
                    pending = _pending;
                    _pending = nullptr;
                }

                task();
                --(*pending);
            }

            Core::WorkerPool::JobType<CallJob&> _job;
            std::mutex _lock;
            std::atomic<uint32_t>* _pending;
            std::function<void()> _task;
        };

        static constexpr uint32_t JobSlots = 64;

        template <typename CALLBACK, typename FUNCTION>
        Core::hresult Queue(CALLBACK* const callback, FUNCTION&& function)
        {
            if (callback == nullptr) {
                return Core::ERROR_BAD_REQUEST;
            }
            if (Core::IWorkerPool::IsAvailable() == false) {
                return Core::ERROR_UNAVAILABLE;
            }

            callback->AddRef();
            ++_pendingJobs;

            std::function<void()> task = [callback, function = std::forward<FUNCTION>(function)]() mutable {
                function(callback);
                callback->Release();
            };

            std::lock_guard<std::mutex> lock(_jobsLock);
            for (CallJob& job : _jobs) {
                if (job.Submit(_pendingJobs, std::move(task))) {
                    return Core::ERROR_NONE;
                }
            }

            --_pendingJobs;
            callback->Release();
            return Core::ERROR_INPROGRESS;
        }

        bool JobsIdle() const
        {
            std::lock_guard<std::mutex> lock(_jobsLock);
            for (const CallJob& job : _jobs) {
                if (job.IsIdle() == false) {
                    return false;
                }
            }
            return true;
        }

        std::array<CallJob, JobSlots> _jobs;
        mutable std::mutex _jobsLock;
        std::atomic<uint32_t> _pendingJobs { 0 };
        std::thread _notifyThread;
    };

} // namespace Plugin
} // namespace WPEFramework
