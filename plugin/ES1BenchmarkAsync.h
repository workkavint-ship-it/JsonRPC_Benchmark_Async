#pragma once

#include "Module.h"

#include <interfaces/IES1BenchmarkAsync.h>
#include <interfaces/json/JES1BenchmarkAsync.h>

#include <thread>

namespace WPEFramework {
namespace Plugin {

    class ES1BenchmarkAsync : public PluginHost::IPlugin
                             , public PluginHost::JSONRPC {
    public:
        ES1BenchmarkAsync(const ES1BenchmarkAsync&) = delete;
        ES1BenchmarkAsync& operator=(const ES1BenchmarkAsync&) = delete;

        ES1BenchmarkAsync() = default;
        ~ES1BenchmarkAsync() override = default;

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
            INTERFACE_AGGREGATE(Exchange::IES1BenchmarkAsync, _implementation)
        END_INTERFACE_MAP

        const string Initialize(PluginHost::IShell* service) override;
        void Deinitialize(PluginHost::IShell* service) override;
        string Information() const override;

    private:
        PluginHost::IShell* _service { nullptr };
        uint32_t _connectionId { 0 };
        Exchange::IES1BenchmarkAsync* _implementation { nullptr };
        std::thread _notifyThread;
    };

} // namespace Plugin
} // namespace WPEFramework
