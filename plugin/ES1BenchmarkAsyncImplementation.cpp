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

    ES1BenchmarkAsyncImplementation::AsyncQueue::AsyncQueue(ES1BenchmarkAsyncImplementation& parent)
        : Core::WorkerPool::JobType<AsyncQueue&>(*this)
        , _parent(parent)
        , _lock()
        , _jobs()
        , _scheduled(false)
    {
    }

    ES1BenchmarkAsyncImplementation::AsyncQueue::~AsyncQueue()
    {
        Cancel();
    }

    void ES1BenchmarkAsyncImplementation::AsyncQueue::Submit(const Job& job)
    {
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(_lock);
            _jobs.push_back(job);
            if (_scheduled == false) {
                _scheduled = true;
                schedule = true;
            }
        }
        if (schedule) {
            JobType::Reschedule(Core::Time::Now().Add(5000));
        }
    }

    void ES1BenchmarkAsyncImplementation::AsyncQueue::Cancel()
    {
        {
            std::lock_guard<std::mutex> lock(_lock);
            _jobs.clear();
            _scheduled = false;
        }
        JobType::Revoke();
    }

    void ES1BenchmarkAsyncImplementation::AsyncQueue::Dispatch()
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
        if (job) job();
        std::lock_guard<std::mutex> lock(_lock);
        if (_jobs.empty() == false) JobType::Submit();
        else _scheduled = false;
    }

    ES1BenchmarkAsyncImplementation::ES1BenchmarkAsyncImplementation()
        : _adminLock()
        , _notification(nullptr)
        , _queue(*this)
    {
    }

    ES1BenchmarkAsyncImplementation::~ES1BenchmarkAsyncImplementation()
    {
        _queue.Cancel();
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

    Core::hresult ES1BenchmarkAsyncImplementation::SetString(const uint32_t requestId, const string& /* value */)
    { _queue.Submit([this, requestId] { NotifySetString(requestId, true); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsyncImplementation::GetString(const uint32_t requestId, const uint32_t size)
    {
        if (size > StringBuffer().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, requestId, size] { NotifyGetString(requestId, StringBuffer().substr(0, size)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::SetArray(const uint32_t requestId, const std::vector<uint8_t>& /* value */)
    { _queue.Submit([this, requestId] { NotifySetArray(requestId, true); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsyncImplementation::GetArray(const uint32_t requestId, const uint32_t size)
    {
        if (size > ByteBuffer().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, requestId, size] { NotifyGetArray(requestId, std::vector<uint8_t>(ByteBuffer().begin(), ByteBuffer().begin() + size)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::SetMixedArray(const uint32_t requestId, const std::vector<MixedElement>& /* value */)
    { _queue.Submit([this, requestId] { NotifySetMixedArray(requestId, true); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsyncImplementation::GetMixedArray(const uint32_t requestId, const uint32_t count)
    {
        if (count > MixedValues().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, requestId, count] { NotifyGetMixedArray(requestId, std::vector<MixedElement>(MixedValues().begin(), MixedValues().begin() + count)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::SetNestedObjects(const uint32_t requestId, const std::vector<NestedObject>& /* value */)
    { _queue.Submit([this, requestId] { NotifySetNestedObjects(requestId, true); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsyncImplementation::GetNestedObjects(const uint32_t requestId, const uint32_t count)
    {
        if (count > NestedValues().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, requestId, count] { NotifyGetNestedObjects(requestId, std::vector<NestedObject>(NestedValues().begin(), NestedValues().begin() + count)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::SetUint32(const uint32_t requestId, const uint32_t /* value */)
    { _queue.Submit([this, requestId] { NotifySetUint32(requestId, true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetUint32(const uint32_t requestId)
    { _queue.Submit([this, requestId] { NotifyGetUint32(requestId, StaticUint32); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetUint64(const uint32_t requestId, const uint64_t /* value */)
    { _queue.Submit([this, requestId] { NotifySetUint64(requestId, true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetUint64(const uint32_t requestId)
    { _queue.Submit([this, requestId] { NotifyGetUint64(requestId, StaticUint64); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetBool(const uint32_t requestId, const bool /* value */)
    { _queue.Submit([this, requestId] { NotifySetBool(requestId, true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetBool(const uint32_t requestId)
    { _queue.Submit([this, requestId] { NotifyGetBool(requestId, StaticBool); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetFloat(const uint32_t requestId, const float /* value */)
    { _queue.Submit([this, requestId] { NotifySetFloat(requestId, true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetFloat(const uint32_t requestId)
    { _queue.Submit([this, requestId] { NotifyGetFloat(requestId, StaticFloat); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetDouble(const uint32_t requestId, const double /* value */)
    { _queue.Submit([this, requestId] { NotifySetDouble(requestId, true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetDouble(const uint32_t requestId)
    { _queue.Submit([this, requestId] { NotifyGetDouble(requestId, StaticDouble); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureCopyCost(const uint32_t requestId, const uint32_t size)
    {
        if (size > ByteBuffer().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, requestId, size] {
            std::vector<uint8_t> destination(size);
            const auto start = std::chrono::steady_clock::now();
            if (size != 0) std::memcpy(destination.data(), ByteBuffer().data(), size);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureCopyCost(requestId, std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureStringResizeCost(const uint32_t requestId, const uint32_t size)
    {
        if (size > StringBuffer().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, requestId, size] {
            string destination;
            const auto start = std::chrono::steady_clock::now();
            destination.resize(size);
            if (size != 0) std::memcpy(&destination[0], StringBuffer().data(), size);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureStringResizeCost(requestId, std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureMixedAssignCost(const uint32_t requestId, const uint32_t count)
    {
        if (count > MixedValues().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, requestId, count] {
            std::vector<MixedElement> destination;
            const auto start = std::chrono::steady_clock::now();
            destination.assign(MixedValues().begin(), MixedValues().begin() + count);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureMixedAssignCost(requestId, std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureNestedAssignCost(const uint32_t requestId, const uint32_t count)
    {
        if (count > NestedValues().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, requestId, count] {
            std::vector<NestedObject> destination;
            const auto start = std::chrono::steady_clock::now();
            destination.assign(NestedValues().begin(), NestedValues().begin() + count);
            const auto end = std::chrono::steady_clock::now();
            NotifyMeasureNestedAssignCost(requestId, std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
        return Core::ERROR_NONE;
    }

#define NOTIFY_BOOL(name) \
    void ES1BenchmarkAsyncImplementation::Notify##name(uint32_t requestId, const bool value) \
    { \
        NotifySink([requestId, value](Notification* notification) { notification->name##Completed(requestId, value); }); \
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
    void ES1BenchmarkAsyncImplementation::Notify##name(uint32_t requestId, const type value) \
    { \
        NotifySink([requestId, value](Notification* notification) { notification->name##Completed(requestId, value); }); \
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

    void ES1BenchmarkAsyncImplementation::NotifyGetString(uint32_t requestId, const string& value)
    { NotifySink([requestId, &value](Notification* notification) { notification->GetStringCompleted(requestId, value); }); }
    void ES1BenchmarkAsyncImplementation::NotifyGetArray(uint32_t requestId, const std::vector<uint8_t>& value)
    { NotifySink([requestId, &value](Notification* notification) { notification->GetArrayCompleted(requestId, value); }); }
    void ES1BenchmarkAsyncImplementation::NotifyGetMixedArray(uint32_t requestId, const std::vector<MixedElement>& value)
    { NotifySink([requestId, &value](Notification* notification) { notification->GetMixedArrayCompleted(requestId, value); }); }
    void ES1BenchmarkAsyncImplementation::NotifyGetNestedObjects(uint32_t requestId, const std::vector<NestedObject>& value)
    { NotifySink([requestId, &value](Notification* notification) { notification->GetNestedObjectsCompleted(requestId, value); }); }

} // namespace Plugin
} // namespace WPEFramework
