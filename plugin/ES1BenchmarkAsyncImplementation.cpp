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
            JobType::Reschedule(Core::Time::Now().Add(10000));
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

    Core::hresult ES1BenchmarkAsyncImplementation::SetString(const string& /* value */)
    { _queue.Submit([this] { NotifySetString(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetString(const uint32_t size)
    {
        if (size > StringBuffer().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, size] { NotifyGetString(StringBuffer().substr(0, size)); });
        return Core::ERROR_NONE;
    }
    Core::hresult ES1BenchmarkAsyncImplementation::SetArray(const std::vector<uint8_t>& /* value */)
    { _queue.Submit([this] { NotifySetArray(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetArray(const uint32_t size)
    {
        if (size > ByteBuffer().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, size] { NotifyGetArray(std::vector<uint8_t>(ByteBuffer().begin(), ByteBuffer().begin() + size)); });
        return Core::ERROR_NONE;
    }
    Core::hresult ES1BenchmarkAsyncImplementation::SetMixedArray(const std::vector<MixedElement>& /* value */)
    { _queue.Submit([this] { NotifySetMixedArray(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetMixedArray(const uint32_t count)
    {
        if (count > MixedValues().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, count] { NotifyGetMixedArray(std::vector<MixedElement>(MixedValues().begin(), MixedValues().begin() + count)); });
        return Core::ERROR_NONE;
    }
    Core::hresult ES1BenchmarkAsyncImplementation::SetNestedObjects(const std::vector<NestedObject>& /* value */)
    { _queue.Submit([this] { NotifySetNestedObjects(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetNestedObjects(const uint32_t count)
    {
        if (count > NestedValues().size()) return Core::ERROR_INVALID_RANGE;
        _queue.Submit([this, count] { NotifyGetNestedObjects(std::vector<NestedObject>(NestedValues().begin(), NestedValues().begin() + count)); });
        return Core::ERROR_NONE;
    }

    Core::hresult ES1BenchmarkAsyncImplementation::SetUint32(const uint32_t /* value */) { _queue.Submit([this] { NotifySetUint32(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetUint32() { _queue.Submit([this] { NotifyGetUint32(StaticUint32); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetUint64(const uint64_t /* value */) { _queue.Submit([this] { NotifySetUint64(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetUint64() { _queue.Submit([this] { NotifyGetUint64(StaticUint64); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetBool(const bool /* value */) { _queue.Submit([this] { NotifySetBool(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetBool() { _queue.Submit([this] { NotifyGetBool(StaticBool); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetFloat(const float /* value */) { _queue.Submit([this] { NotifySetFloat(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetFloat() { _queue.Submit([this] { NotifyGetFloat(StaticFloat); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::SetDouble(const double /* value */) { _queue.Submit([this] { NotifySetDouble(true); }); return Core::ERROR_NONE; }
    Core::hresult ES1BenchmarkAsyncImplementation::GetDouble() { _queue.Submit([this] { NotifyGetDouble(StaticDouble); }); return Core::ERROR_NONE; }

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureCopyCost(const uint32_t size)
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

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureStringResizeCost(const uint32_t size)
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

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureMixedAssignCost(const uint32_t count)
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

    Core::hresult ES1BenchmarkAsyncImplementation::MeasureNestedAssignCost(const uint32_t count)
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
    void ES1BenchmarkAsyncImplementation::Notify##name(const bool value) \
    { \
        NotifySink([value](Notification* notification) { notification->name##Completed(value); }); \
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
    void ES1BenchmarkAsyncImplementation::Notify##name(const type value) \
    { \
        NotifySink([value](Notification* notification) { notification->name##Completed(value); }); \
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

    void ES1BenchmarkAsyncImplementation::NotifyGetString(const string& value)
    { NotifySink([&value](Notification* notification) { notification->GetStringCompleted(value); }); }
    void ES1BenchmarkAsyncImplementation::NotifyGetArray(const std::vector<uint8_t>& value)
    { NotifySink([&value](Notification* notification) { notification->GetArrayCompleted(value); }); }
    void ES1BenchmarkAsyncImplementation::NotifyGetMixedArray(const std::vector<MixedElement>& value)
    { NotifySink([&value](Notification* notification) { notification->GetMixedArrayCompleted(value); }); }
    void ES1BenchmarkAsyncImplementation::NotifyGetNestedObjects(const std::vector<NestedObject>& value)
    { NotifySink([&value](Notification* notification) { notification->GetNestedObjectsCompleted(value); }); }

} // namespace Plugin
} // namespace WPEFramework
