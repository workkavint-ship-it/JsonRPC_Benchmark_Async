#include "ES1BenchmarkAsyncImplementation.h"

#include <chrono>
#include <cstring>

namespace WPEFramework {
namespace Plugin {

    namespace {
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
    }

    void ES1BenchmarkAsyncImplementation::SubmitDelayed(std::function<void()>&& work)
    {
        Core::ProxyType<Core::IDispatch> job(
            Core::ProxyType<CallJob>::Create(*this, std::move(work)));

        _jobsLock.Lock();
        _pendingJobs.push_back(job);
        _jobsLock.Unlock();

        // Submit(), not Schedule() - handed straight to a worker thread
        // now; the 5s delay happens inside CallJob::Dispatch() itself via
        // sleep_for(), not via Thunder's internal timer thread.
        Core::IWorkerPool::Instance().Submit(job);
    }

    void ES1BenchmarkAsyncImplementation::UntrackJob(const Core::IDispatch* job)
    {
        _jobsLock.Lock();
        for (auto it = _pendingJobs.begin(); it != _pendingJobs.end(); ++it) {
            if (it->operator->() == job) {
                _pendingJobs.erase(it);
                break;
            }
        }
        _jobsLock.Unlock();
    }

    ES1BenchmarkAsyncImplementation::ES1BenchmarkAsyncImplementation()
        : _adminLock()
        , _notification(nullptr)
        , _jobsLock()
        , _pendingJobs()
    {
    }

    ES1BenchmarkAsyncImplementation::~ES1BenchmarkAsyncImplementation()
    {
        _jobsLock.Lock();
        std::vector<Core::ProxyType<Core::IDispatch>> pending(std::move(_pendingJobs));
        _pendingJobs.clear();
        _jobsLock.Unlock();

        // Revoke() either cancels a not-yet-started job or, if it is
        // currently mid-Dispatch on a worker thread, blocks until that
        // finishes - either way it is safe to destruct this object once
        // every entry here has been revoked.
        for (const Core::ProxyType<Core::IDispatch>& job : pending) {
            Core::IWorkerPool::Instance().Revoke(job);
        }

        Notification* notification = nullptr;
        _adminLock.Lock();
        notification = _notification;
        _notification = nullptr;
        _adminLock.Unlock();
        if (notification != nullptr) notification->Release();
    }

    SERVICE_REGISTRATION(ES1BenchmarkAsyncImplementation, 1, 0);

    Core::hresult ES1BenchmarkAsyncImplementation::Register(Notification* const notification)
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

    Core::hresult ES1BenchmarkAsyncImplementation::Unregister(const Notification* notification)
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

    Core::hresult ES1BenchmarkAsyncImplementation::SetString(const string& clientid, const string& /* value */)
    { SubmitDelayed([this, clientid] { NotifySetString(clientid, true); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsyncImplementation::GetString(const string& clientid, const uint32_t size)
    {
        if (size > StringBuffer().size()) return Core::ERROR_INVALID_RANGE;
        SubmitDelayed([this, clientid, size] { NotifyGetString(clientid, StringBuffer().substr(0, size)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::SetArray(const string& clientid, const std::vector<uint8_t>& /* value */)
    { SubmitDelayed([this, clientid] { NotifySetArray(clientid, true); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsyncImplementation::GetArray(const string& clientid, const uint32_t size)
    {
        if (size > ByteBuffer().size()) return Core::ERROR_INVALID_RANGE;
        SubmitDelayed([this, clientid, size] { NotifyGetArray(clientid, std::vector<uint8_t>(ByteBuffer().begin(), ByteBuffer().begin() + size)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::SetMixedArray(const string& clientid, const std::vector<MixedElement>& /* value */)
    { SubmitDelayed([this, clientid] { NotifySetMixedArray(clientid, true); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsyncImplementation::GetMixedArray(const string& clientid, const uint32_t count)
    {
        if (count > MixedValues().size()) return Core::ERROR_INVALID_RANGE;
        SubmitDelayed([this, clientid, count] { NotifyGetMixedArray(clientid, std::vector<MixedElement>(MixedValues().begin(), MixedValues().begin() + count)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::SetNestedObjects(const string& clientid, const std::vector<NestedObject>& /* value */)
    { SubmitDelayed([this, clientid] { NotifySetNestedObjects(clientid, true); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsyncImplementation::GetNestedObjects(const string& clientid, const uint32_t count)
    {
        if (count > NestedValues().size()) return Core::ERROR_INVALID_RANGE;
        SubmitDelayed([this, clientid, count] { NotifyGetNestedObjects(clientid, std::vector<NestedObject>(NestedValues().begin(), NestedValues().begin() + count)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::SetUint32(const string& clientid, const uint32_t /* value */)
    { SubmitDelayed([this, clientid] { NotifySetUint32(clientid, true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetUint32(const string& clientid)
    { SubmitDelayed([this, clientid] { NotifyGetUint32(clientid, StaticUint32); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetUint64(const string& clientid, const uint64_t /* value */)
    { SubmitDelayed([this, clientid] { NotifySetUint64(clientid, true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetUint64(const string& clientid)
    { SubmitDelayed([this, clientid] { NotifyGetUint64(clientid, StaticUint64); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetBool(const string& clientid, const bool /* value */)
    { SubmitDelayed([this, clientid] { NotifySetBool(clientid, true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetBool(const string& clientid)
    { SubmitDelayed([this, clientid] { NotifyGetBool(clientid, StaticBool); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetFloat(const string& clientid, const float /* value */)
    { SubmitDelayed([this, clientid] { NotifySetFloat(clientid, true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetFloat(const string& clientid)
    { SubmitDelayed([this, clientid] { NotifyGetFloat(clientid, StaticFloat); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetDouble(const string& clientid, const double /* value */)
    { SubmitDelayed([this, clientid] { NotifySetDouble(clientid, true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetDouble(const string& clientid)
    { SubmitDelayed([this, clientid] { NotifyGetDouble(clientid, StaticDouble); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureCopyCost(const string& clientid, const uint32_t size)
    {
        if (size > ByteBuffer().size()) return Core::ERROR_INVALID_RANGE;
        SubmitDelayed([this, clientid, size] {
            std::vector<uint8_t> destination(size);
            const auto start = std::chrono::steady_clock::now();
            if (size != 0) std::memcpy(destination.data(), ByteBuffer().data(), size);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureCopyCost(clientid, std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureStringResizeCost(const string& clientid, const uint32_t size)
    {
        if (size > StringBuffer().size()) return Core::ERROR_INVALID_RANGE;
        SubmitDelayed([this, clientid, size] {
            string destination;
            const auto start = std::chrono::steady_clock::now();
            destination.resize(size);
            if (size != 0) std::memcpy(&destination[0], StringBuffer().data(), size);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureStringResizeCost(clientid, std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureMixedAssignCost(const string& clientid, const uint32_t count)
    {
        if (count > MixedValues().size()) return Core::ERROR_INVALID_RANGE;
        SubmitDelayed([this, clientid, count] {
            std::vector<MixedElement> destination;
            const auto start = std::chrono::steady_clock::now();
            destination.assign(MixedValues().begin(), MixedValues().begin() + count);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureMixedAssignCost(clientid, std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureNestedAssignCost(const string& clientid, const uint32_t count)
    {
        if (count > NestedValues().size()) return Core::ERROR_INVALID_RANGE;
        SubmitDelayed([this, clientid, count] {
            std::vector<NestedObject> destination;
            const auto start = std::chrono::steady_clock::now();
            destination.assign(NestedValues().begin(), NestedValues().begin() + count);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureNestedAssignCost(clientid, std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

#define NOTIFY_BOOL(name) \
    void ES1BenchmarkAsyncImplementation::Notify##name(string clientid, const bool value) \
    { \
        NotifySink([clientid, value](Notification* notification) { notification->name##Completed(clientid, value); }); \
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
    void ES1BenchmarkAsyncImplementation::Notify##name(string clientid, const type value) \
    { \
        NotifySink([clientid, value](Notification* notification) { notification->name##Completed(clientid, value); }); \
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

    void ES1BenchmarkAsyncImplementation::NotifyGetString(string clientid, const string& value)
    { NotifySink([clientid, &value](Notification* notification) { notification->GetStringCompleted(clientid, value); }); }
    void ES1BenchmarkAsyncImplementation::NotifyGetArray(string clientid, const std::vector<uint8_t>& value)
    { NotifySink([clientid, &value](Notification* notification) { notification->GetArrayCompleted(clientid, value); }); }
    void ES1BenchmarkAsyncImplementation::NotifyGetMixedArray(string clientid, const std::vector<MixedElement>& value)
    { NotifySink([clientid, &value](Notification* notification) { notification->GetMixedArrayCompleted(clientid, value); }); }
    void ES1BenchmarkAsyncImplementation::NotifyGetNestedObjects(string clientid, const std::vector<NestedObject>& value)
    { NotifySink([clientid, &value](Notification* notification) { notification->GetNestedObjectsCompleted(clientid, value); }); }

} // namespace Plugin
} // namespace WPEFramework
