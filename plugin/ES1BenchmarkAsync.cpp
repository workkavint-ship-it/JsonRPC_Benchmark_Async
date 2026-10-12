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

    const string ES1BenchmarkAsync::Initialize(PluginHost::IShell* service)
    {
        _service = service;
        _service->AddRef();

        _implementation = _service->Root<Exchange::IES1BenchmarkAsync>(
            _connectionId, 2000, _T("ES1BenchmarkAsyncImplementation"));
        if (_implementation == nullptr) {
            _service->Release();
            _service = nullptr;
            return _T("ES1BenchmarkAsync implementation could not be instantiated");
        }

        Exchange::JES1BenchmarkAsync::Register(*this, _implementation);

        Config config;
        config.FromString(service->ConfigLine());
        const string notifyHost = config.NotifyHost.Value();
        const uint16_t notifyPort = config.NotifyPort.Value();
        const string thunderHost = config.ThunderHost.Value();
        const uint16_t thunderPort = config.ThunderPort.Value();

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

        return string();
    }

    void ES1BenchmarkAsync::Deinitialize(PluginHost::IShell* /* service */)
    {
        Exchange::JES1BenchmarkAsync::Unregister(*this);
        if (_notifyThread.joinable()) {
            _notifyThread.join();
        }
        if (_implementation != nullptr) {
            _implementation->Release();
            _implementation = nullptr;
        }
        if (_service != nullptr) {
            _service->Release();
            _service = nullptr;
        }
        _connectionId = 0;
    }

    string ES1BenchmarkAsync::Information() const
    {
        return string("ES1 asynchronous JSON-RPC benchmark plugin facade");
    }

} // namespace Plugin
} // namespace WPEFramework
