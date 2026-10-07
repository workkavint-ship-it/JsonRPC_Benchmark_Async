#include "ES1BenchmarkAsync.h"

#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace WPEFramework {
namespace Plugin {

    namespace {
        static Metadata<ES1BenchmarkAsync> metadata(
            // Version (Major, Minor, Patch)
            1, 0, 0,
            // Preconditions
            {},
            // Terminations
            {},
            // Controls
            {}
        );

        // Returns Unix epoch time in microseconds - same epoch as Python time.time(),
        // matching ES1Benchmark's coldstart notifier.
        inline uint64_t GetUnixMicroseconds()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        template <typename T, std::size_t... Is>
        constexpr std::array<T, sizeof...(Is)> FillArray(T value, std::index_sequence<Is...>)
        {
            return {{ (static_cast<void>(Is), value)... }};
        }

        template <typename T, std::size_t N>
        constexpr std::array<T, N> FillArray(T value)
        {
            return FillArray<T>(value, std::make_index_sequence<N>{});
        }

        static constexpr auto s_staticCharBuffer = FillArray<char, 4 * 1024 * 1024>('A');
        static constexpr auto s_staticByteBuffer = FillArray<uint8_t, 256 * 1024>(uint8_t { 0xAA });
        static uint32_t s_staticUint32 = 0xFFFFFFFFu;
        static uint64_t s_staticUint64 = 0xFFFFFFFFFFFFFFFFull;
        static bool s_staticBool = true;
        static float s_staticFloat = 3.4028235e+38f;
        static double s_staticDouble = 1.7976931348623157e+308;

        static const std::vector<Exchange::IES1BenchmarkAsync::MixedElement> s_staticMixedVec = [] {
            std::vector<Exchange::IES1BenchmarkAsync::MixedElement> values;
            values.reserve(4228);
            for (uint32_t index = 0; index < 4228; ++index) {
                values.push_back({ index, "item" + std::to_string(index), index * 3.14159, (index % 2 == 0) });
            }
            return values;
        }();

        static const std::vector<Exchange::IES1BenchmarkAsync::NestedObject> s_staticNestedVec = [] {
            std::vector<Exchange::IES1BenchmarkAsync::NestedObject> values;
            values.reserve(1736);
            for (uint32_t index = 0; index < 1736; ++index) {
                Exchange::IES1BenchmarkAsync::NestedObject object;
                object.id = index;
                object.flag = (index % 2 == 0);
                object.score = index * 3.14159;
                object.data.label = "item" + std::to_string(index);
                object.data.nested.count = index;
                object.data.nested.inner.value = index * 3;
                object.data.nested.inner.name = "deep" + std::to_string(index);
                values.push_back(object);
            }
            return values;
        }();
    }

    const string ES1BenchmarkAsync::Initialize(PluginHost::IShell* service)
    {
        Exchange::JES1BenchmarkAsync::Register(*this, this);

        // Same fire-and-forget coldstart notify as ES1Benchmark, so this
        // plugin can be driven by the same cold-boot-triggered benchmark
        // workflow instead of only being reachable via manual/on-demand runs.
        Config config;
        config.FromString(service->ConfigLine());

        const string notifyHost   = config.NotifyHost.Value();
        const uint16_t notifyPort = config.NotifyPort.Value();
        const string thunderHost  = config.ThunderHost.Value();
        const uint16_t thunderPort = config.ThunderPort.Value();

        _notifyThread = std::thread([notifyHost, notifyPort, thunderHost, thunderPort] {
            const uint64_t bootUs = GetUnixMicroseconds();
            char request[512];
            ::snprintf(request, sizeof(request),
                "GET /trigger?host=%s&port=%u&boot_time_us=%llu HTTP/1.0\r\n"
                "Host: %s\r\n\r\n",
                thunderHost.c_str(),
                static_cast<unsigned>(thunderPort),
                static_cast<unsigned long long>(bootUs),
                notifyHost.c_str());

            const int socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (socketFd < 0) return;

            timeval timeout { 2, 0 };
            ::setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            ::setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

            sockaddr_in address {};
            address.sin_family = AF_INET;
            address.sin_port = ::htons(notifyPort);
            address.sin_addr.s_addr = ::inet_addr(notifyHost.c_str());
            if (::connect(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
                ::send(socketFd, request, ::strlen(request), 0);
            }
            ::close(socketFd);
        });

        return string();
    }

    void ES1BenchmarkAsync::Deinitialize(PluginHost::IShell* /* service */)
    {
        Exchange::JES1BenchmarkAsync::Unregister(*this);

        if (_notifyThread.joinable()) {
            _notifyThread.join();
        }

        while (_pendingJobs.load() != 0 || JobsIdle() == false) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    string ES1BenchmarkAsync::Information() const
    {
        return string("ES1 asynchronous JSON-RPC round-trip benchmark echo plugin");
    }

    Core::hresult ES1BenchmarkAsync::SetString(const string& /* value */, ISetStringCallback* const callback)
    {
        return Queue(callback, [](ISetStringCallback* result) { result->Complete(); });
    }

    Core::hresult ES1BenchmarkAsync::GetString(const uint32_t size, IGetStringCallback* const callback)
    {
        if (size > s_staticCharBuffer.size()) {
            return Core::ERROR_INVALID_RANGE;
        }
        return Queue(callback, [size](IGetStringCallback* result) {
            string value;
            value.resize(size);
            if (size != 0) {
                std::memcpy(&value[0], s_staticCharBuffer.data(), size);
            }
            result->Complete(value);
        });
    }

    Core::hresult ES1BenchmarkAsync::SetArray(const std::vector<uint8_t>& /* value */, ISetArrayCallback* const callback)
    {
        return Queue(callback, [](ISetArrayCallback* result) { result->Complete(); });
    }

    Core::hresult ES1BenchmarkAsync::GetArray(const uint32_t size, IGetArrayCallback* const callback)
    {
        if (size > s_staticByteBuffer.size()) {
            return Core::ERROR_INVALID_RANGE;
        }
        return Queue(callback, [size](IGetArrayCallback* result) {
            std::vector<uint8_t> value(size);
            if (size != 0) {
                std::memcpy(value.data(), s_staticByteBuffer.data(), size);
            }
            result->Complete(value);
        });
    }

    Core::hresult ES1BenchmarkAsync::SetMixedArray(const std::vector<MixedElement>& /* value */, ISetMixedCallback* const callback)
    {
        return Queue(callback, [](ISetMixedCallback* result) { result->Complete(); });
    }

    Core::hresult ES1BenchmarkAsync::GetMixedArray(const uint32_t count, IGetMixedCallback* const callback)
    {
        if (count > s_staticMixedVec.size()) {
            return Core::ERROR_INVALID_RANGE;
        }
        return Queue(callback, [count](IGetMixedCallback* result) {
            std::vector<MixedElement> value(s_staticMixedVec.begin(), s_staticMixedVec.begin() + count);
            result->Complete(value);
        });
    }

    Core::hresult ES1BenchmarkAsync::SetNestedObjects(const std::vector<NestedObject>& /* value */, ISetNestedCallback* const callback)
    {
        return Queue(callback, [](ISetNestedCallback* result) { result->Complete(); });
    }

    Core::hresult ES1BenchmarkAsync::GetNestedObjects(const uint32_t count, IGetNestedCallback* const callback)
    {
        if (count > s_staticNestedVec.size()) {
            return Core::ERROR_INVALID_RANGE;
        }
        return Queue(callback, [count](IGetNestedCallback* result) {
            std::vector<NestedObject> value(s_staticNestedVec.begin(), s_staticNestedVec.begin() + count);
            result->Complete(value);
        });
    }

    Core::hresult ES1BenchmarkAsync::SetUint32(const uint32_t /* value */, ISetUint32Callback* const callback)
    {
        return Queue(callback, [](ISetUint32Callback* result) { result->Complete(); });
    }

    Core::hresult ES1BenchmarkAsync::GetUint32(IGetUint32Callback* const callback)
    {
        return Queue(callback, [](IGetUint32Callback* result) { result->Complete(s_staticUint32); });
    }

    Core::hresult ES1BenchmarkAsync::SetUint64(const uint64_t /* value */, ISetUint64Callback* const callback)
    {
        return Queue(callback, [](ISetUint64Callback* result) { result->Complete(); });
    }

    Core::hresult ES1BenchmarkAsync::GetUint64(IGetUint64Callback* const callback)
    {
        return Queue(callback, [](IGetUint64Callback* result) { result->Complete(s_staticUint64); });
    }

    Core::hresult ES1BenchmarkAsync::SetBool(const bool /* value */, ISetBoolCallback* const callback)
    {
        return Queue(callback, [](ISetBoolCallback* result) { result->Complete(); });
    }

    Core::hresult ES1BenchmarkAsync::GetBool(IGetBoolCallback* const callback)
    {
        return Queue(callback, [](IGetBoolCallback* result) { result->Complete(s_staticBool); });
    }

    Core::hresult ES1BenchmarkAsync::SetFloat(const float /* value */, ISetFloatCallback* const callback)
    {
        return Queue(callback, [](ISetFloatCallback* result) { result->Complete(); });
    }

    Core::hresult ES1BenchmarkAsync::GetFloat(IGetFloatCallback* const callback)
    {
        return Queue(callback, [](IGetFloatCallback* result) { result->Complete(s_staticFloat); });
    }

    Core::hresult ES1BenchmarkAsync::SetDouble(const double /* value */, ISetDoubleCallback* const callback)
    {
        return Queue(callback, [](ISetDoubleCallback* result) { result->Complete(); });
    }

    Core::hresult ES1BenchmarkAsync::GetDouble(IGetDoubleCallback* const callback)
    {
        return Queue(callback, [](IGetDoubleCallback* result) { result->Complete(s_staticDouble); });
    }

    Core::hresult ES1BenchmarkAsync::MeasureCopyCost(const uint32_t size, IMeasureCopyCallback* const callback)
    {
        if (size > s_staticByteBuffer.size()) {
            return Core::ERROR_INVALID_RANGE;
        }
        return Queue(callback, [size](IMeasureCopyCallback* result) {
            std::vector<uint8_t> destination;
            const auto start = std::chrono::steady_clock::now();
            destination.resize(size);
            if (size != 0) {
                std::memcpy(destination.data(), s_staticByteBuffer.data(), size);
            }
            const auto end = std::chrono::steady_clock::now();
            result->Complete(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
    }

    Core::hresult ES1BenchmarkAsync::MeasureStringResizeCost(const uint32_t size, IMeasureStringCallback* const callback)
    {
        if (size > s_staticCharBuffer.size()) {
            return Core::ERROR_INVALID_RANGE;
        }
        return Queue(callback, [size](IMeasureStringCallback* result) {
            string destination;
            const auto start = std::chrono::steady_clock::now();
            destination.resize(size);
            if (size != 0) {
                std::memcpy(&destination[0], s_staticCharBuffer.data(), size);
            }
            const auto end = std::chrono::steady_clock::now();
            result->Complete(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
    }

    Core::hresult ES1BenchmarkAsync::MeasureMixedAssignCost(const uint32_t count, IMeasureMixedCallback* const callback)
    {
        if (count > s_staticMixedVec.size()) {
            return Core::ERROR_INVALID_RANGE;
        }
        return Queue(callback, [count](IMeasureMixedCallback* result) {
            std::vector<MixedElement> destination;
            const auto start = std::chrono::steady_clock::now();
            destination.assign(s_staticMixedVec.begin(), s_staticMixedVec.begin() + count);
            const auto end = std::chrono::steady_clock::now();
            result->Complete(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
    }

    Core::hresult ES1BenchmarkAsync::MeasureNestedAssignCost(const uint32_t count, IMeasureNestedCallback* const callback)
    {
        if (count > s_staticNestedVec.size()) {
            return Core::ERROR_INVALID_RANGE;
        }
        return Queue(callback, [count](IMeasureNestedCallback* result) {
            std::vector<NestedObject> destination;
            const auto start = std::chrono::steady_clock::now();
            destination.assign(s_staticNestedVec.begin(), s_staticNestedVec.begin() + count);
            const auto end = std::chrono::steady_clock::now();
            result->Complete(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        });
    }

} // namespace Plugin
} // namespace WPEFramework
