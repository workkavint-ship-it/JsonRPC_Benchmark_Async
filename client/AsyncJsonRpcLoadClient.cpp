#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<uint64_t> g_clientSequence { 0 };

std::string Trim(const std::string& value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool IsTrue(const std::string& value)
{
    return value == "true" || value == "1" || value == "yes";
}

long ParseSize(std::string value)
{
    for (char& character : value) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    if (value.size() > 2 && value.substr(value.size() - 2) == "KB") return std::stol(value.substr(0, value.size() - 2)) * 1024L;
    if (value.size() > 2 && value.substr(value.size() - 2) == "MB") return std::stol(value.substr(0, value.size() - 2)) * 1024L * 1024L;
    if (!value.empty() && value.back() == 'B') return std::stol(value.substr(0, value.size() - 1));
    return std::stol(value);
}

struct Config {
    std::string mode = "warm";
    std::string transport = "ws";
    std::string scope = "both";
    std::string operation = "both";
    std::string tier = "single";
    std::string size = "5KB";
    int clients = 100;
    int iterations = 10;
    int warmup = 0;
    std::string host = "127.0.0.1";
    int port = 9998;
    int timeoutS = 30;
    bool logRequestResponse = true;
    int logMaxBytes = 16384;
    bool progressOutput = true;
    bool resultFileEnabled = true;
    std::string resultFile = "/opt/AsyncJsonRpcLoadClient-result.jsonl";
};

bool LoadConfig(const std::string& path, Config* config)
{
    std::ifstream input(path);
    if (!input.is_open()) return false;
    std::string line;
    while (std::getline(input, line)) {
        const std::string item = Trim(line);
        if (item.empty() || item[0] == '#') continue;
        const size_t separator = item.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = Trim(item.substr(0, separator));
        const std::string value = Trim(item.substr(separator + 1));
        if (key == "mode") config->mode = value;
        else if (key == "transport") config->transport = value;
        else if (key == "test_scope") config->scope = value;
        else if (key == "operation") config->operation = value;
        else if (key == "tier") config->tier = value;
        else if (key == "size") config->size = value;
        else if (key == "clients") config->clients = std::stoi(value);
        else if (key == "iterations") config->iterations = std::stoi(value);
        else if (key == "warmup") config->warmup = std::stoi(value);
        else if (key == "host") config->host = value;
        else if (key == "port") config->port = std::stoi(value);
        else if (key == "timeout_s") config->timeoutS = std::stoi(value);
        else if (key == "log_request_response") config->logRequestResponse = IsTrue(value);
        else if (key == "log_max_bytes") config->logMaxBytes = std::max(0, std::stoi(value));
        else if (key == "progress_output") config->progressOutput = IsTrue(value);
        else if (key == "result_file_enabled") config->resultFileEnabled = IsTrue(value);
        else if (key == "result_file") config->resultFile = value;
    }
    return config->clients > 0 && config->iterations > 0 && config->warmup >= 0;
}

int ConnectTcp(const Config& config)
{
    const int socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) return -1;
    timeval timeout { config.timeoutS, 0 };
    ::setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int enabled = 1;
    ::setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(config.port));
    if (::inet_pton(AF_INET, config.host.c_str(), &address.sin_addr) != 1
        || ::connect(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socketFd);
        return -1;
    }
    return socketFd;
}

bool SendAll(int socketFd, const char* data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        const ssize_t result = ::send(socketFd, data + sent, length - sent, 0);
        if (result <= 0) return false;
        sent += static_cast<size_t>(result);
    }
    return true;
}

std::string Base64(const uint8_t* data, size_t length)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (size_t index = 0; index < length; index += 3) {
        const size_t remaining = std::min<size_t>(3, length - index);
        uint32_t value = static_cast<uint32_t>(data[index]) << 16;
        if (remaining > 1) value |= static_cast<uint32_t>(data[index + 1]) << 8;
        if (remaining > 2) value |= data[index + 2];
        result += table[(value >> 18) & 0x3F];
        result += table[(value >> 12) & 0x3F];
        result += remaining > 1 ? table[(value >> 6) & 0x3F] : '=';
        result += remaining > 2 ? table[value & 0x3F] : '=';
    }
    return result;
}

class WebSocket {
public:
    ~WebSocket() { Close(); }

    bool Connect(const Config& config)
    {
        _socket = ConnectTcp(config);
        if (_socket < 0) return false;
        std::mt19937 generator(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
        uint8_t keyBytes[16];
        for (uint8_t& byte : keyBytes) byte = static_cast<uint8_t>(generator() & 0xFF);
        std::ostringstream request;
        request << "GET /jsonrpc HTTP/1.1\r\nHost: " << config.host << ':' << config.port
                << "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
                << Base64(keyBytes, sizeof(keyBytes)) << "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        const std::string text = request.str();
        if (!SendAll(_socket, text.data(), text.size())) return false;
        std::string headers;
        char buffer[512];
        while (headers.find("\r\n\r\n") == std::string::npos) {
            const ssize_t received = ::recv(_socket, buffer, sizeof(buffer), 0);
            if (received <= 0 || headers.size() > 16384) return false;
            headers.append(buffer, static_cast<size_t>(received));
        }
        return headers.find("101") != std::string::npos;
    }

    void Close()
    {
        if (_socket >= 0) {
            ::close(_socket);
            _socket = -1;
        }
    }

    bool SendText(const std::string& text) { return SendFrame(0x1, text); }

    bool Receive(std::string* output)
    {
        output->clear();
        for (;;) {
            uint8_t header[2];
            if (!ReceiveExact(reinterpret_cast<char*>(header), sizeof(header))) return false;
            const bool finalFrame = (header[0] & 0x80) != 0;
            const uint8_t opcode = header[0] & 0x0F;
            const bool masked = (header[1] & 0x80) != 0;
            uint64_t length = header[1] & 0x7F;
            if (length == 126) {
                uint8_t extended[2];
                if (!ReceiveExact(reinterpret_cast<char*>(extended), sizeof(extended))) return false;
                length = (static_cast<uint64_t>(extended[0]) << 8) | extended[1];
            } else if (length == 127) {
                uint8_t extended[8];
                if (!ReceiveExact(reinterpret_cast<char*>(extended), sizeof(extended))) return false;
                length = 0;
                for (uint8_t byte : extended) length = (length << 8) | byte;
            }
            uint8_t mask[4] = {};
            if (masked && !ReceiveExact(reinterpret_cast<char*>(mask), sizeof(mask))) return false;
            std::string payload(static_cast<size_t>(length), '\0');
            if (length > 0 && !ReceiveExact(&payload[0], payload.size())) return false;
            if (masked) {
                for (size_t index = 0; index < payload.size(); ++index) {
                    payload[index] = static_cast<char>(static_cast<uint8_t>(payload[index]) ^ mask[index % 4]);
                }
            }
            if (opcode == 0x9) { SendFrame(0xA, payload); continue; }
            if (opcode == 0xA) continue;
            if (opcode == 0x8) return false;
            output->append(payload);
            if (finalFrame) return true;
        }
    }

private:
    bool ReceiveExact(char* output, size_t length)
    {
        size_t received = 0;
        while (received < length) {
            const ssize_t count = ::recv(_socket, output + received, length - received, 0);
            if (count <= 0) return false;
            received += static_cast<size_t>(count);
        }
        return true;
    }

    bool SendFrame(uint8_t opcode, const std::string& payload)
    {
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | opcode));
        const uint64_t length = payload.size();
        if (length < 126) frame.push_back(static_cast<char>(0x80 | length));
        else if (length <= 0xFFFF) {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>((length >> 8) & 0xFF));
            frame.push_back(static_cast<char>(length & 0xFF));
        } else {
            frame.push_back(static_cast<char>(0x80 | 127));
            for (int shift = 56; shift >= 0; shift -= 8) frame.push_back(static_cast<char>((length >> shift) & 0xFF));
        }
        std::mt19937 generator(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
        uint8_t mask[4];
        for (uint8_t& byte : mask) byte = static_cast<uint8_t>(generator() & 0xFF);
        frame.append(reinterpret_cast<const char*>(mask), sizeof(mask));
        const size_t start = frame.size();
        frame.resize(start + payload.size());
        for (size_t index = 0; index < payload.size(); ++index) {
            frame[start + index] = static_cast<char>(static_cast<uint8_t>(payload[index]) ^ mask[index % 4]);
        }
        return SendAll(_socket, frame.data(), frame.size());
    }

    int _socket = -1;
};

struct TestCase {
    std::string method;
    std::string tier;
    std::string params;
    bool isGet;
    long targetBytes;
};

class AsyncClient {
public:
    explicit AsyncClient(const Config& config)
        : _config(config)
        , _clientId("es1-load-" + std::to_string(static_cast<unsigned long long>(::getpid()))
            + "-" + std::to_string(++g_clientSequence))
    {
    }

    bool Connect() { return _socket.Connect(_config); }

    bool Call(const TestCase& test, double* elapsedMs, std::string* requestLog, std::string* responseLog)
    {
        const std::string event = test.method + "completed";
        if (!Register(event)) return false;
        const uint64_t requestId = ++_requestId;
        std::ostringstream request;
        request << "{\"jsonrpc\":\"2.0\",\"id\":" << requestId
                << ",\"method\":\"ES1BenchmarkAsync.1." << test.method
                << "\",\"params\":" << test.params << "}";
        if (requestLog != nullptr) *requestLog = request.str();
        const auto start = std::chrono::steady_clock::now();
        if (!_socket.SendText(request.str())) return false;
        const std::string ack = "\"id\":" + std::to_string(requestId) + ",";
        const std::string completion = "\"method\":\"" + _clientId + "." + event + "@" + _clientId + "\"";
        bool acknowledged = false;
        std::string message;
        while (!acknowledged) {
            if (!_socket.Receive(&message)) return false;
            if (message.find(ack) != std::string::npos) {
                if (message.find("\"error\"") != std::string::npos) return false;
                acknowledged = true;
            }
        }
        while (message.find(completion) == std::string::npos) {
            if (!_socket.Receive(&message)) return false;
        }
        if (responseLog != nullptr) *responseLog = message;
        *elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return Unregister(event);
    }

private:
    bool SendAndWait(uint64_t id, const std::string& request)
    {
        if (!_socket.SendText(request)) return false;
        const std::string marker = "\"id\":" + std::to_string(id) + ",";
        std::string response;
        for (;;) {
            if (!_socket.Receive(&response)) return false;
            if (response.find(marker) != std::string::npos) return response.find("\"error\"") == std::string::npos;
        }
    }

    bool Register(const std::string& event)
    {
        const uint64_t id = ++_requestId;
        std::ostringstream request;
        request << "{\"jsonrpc\":\"2.0\",\"id\":" << id
                << ",\"method\":\"ES1BenchmarkAsync.1.register@" << _clientId
                << "\",\"params\":{\"event\":\"" << event << "\",\"id\":\"" << _clientId << "\"}}";
        return SendAndWait(id, request.str());
    }

    bool Unregister(const std::string& event)
    {
        const uint64_t id = ++_requestId;
        std::ostringstream request;
        request << "{\"jsonrpc\":\"2.0\",\"id\":" << id
                << ",\"method\":\"ES1BenchmarkAsync.1.unregister@" << _clientId
                << "\",\"params\":{\"event\":\"" << event << "\",\"id\":\"" << _clientId << "\"}}";
        return SendAndWait(id, request.str());
    }

    const Config& _config;
    WebSocket _socket;
    std::string _clientId;
    uint64_t _requestId = 0;
};

std::string MakePayload(long size) { return std::string(static_cast<size_t>(size), 'A'); }

std::string BuildArray(long size, long* count)
{
    const long elements = std::max<long>(1, size / 4);
    if (count) *count = elements;
    std::ostringstream output;
    output << '[';
    for (long index = 0; index < elements; ++index) output << (index ? "," : "") << (index % 256);
    output << ']';
    return output.str();
}

std::string BuildMixedArray(long size, long* count)
{
    const long elements = std::min<long>(4228, std::max<long>(1, size / 62));
    if (count) *count = elements;
    std::ostringstream output;
    output << '[';
    for (long index = 0; index < elements; ++index) {
        if (index) output << ',';
        output << "{\"id\":" << index << ",\"name\":\"item" << index
               << "\",\"value\":3.14159,\"flag\":"
               << (index % 2 == 0 ? "true" : "false") << '}';
    }
    output << ']';
    return output.str();
}

std::string BuildNestedArray(long size, long* count)
{
    const long elements = std::min<long>(1736, std::max<long>(1, size / 151));
    if (count) *count = elements;
    std::ostringstream output;
    output << '[';
    for (long index = 0; index < elements; ++index) {
        if (index) output << ',';
        output << "{\"id\":" << index << ",\"flag\":true,\"score\":3.14,"
               << "\"data\":{\"label\":\"item\",\"nested\":{\"count\":"
               << index << ",\"inner\":{\"value\":3,\"name\":\"deep\"}}}}";
    }
    output << ']';
    return output.str();
}

std::vector<TestCase> BuildTests(const Config& config)
{
    const std::vector<std::pair<std::string, long>> tiers = {
        { "500B", 500 }, { "5KB", 5L * 1024 }, { "50KB", 50L * 1024 },
        { "150KB", 150L * 1024 }, { "500KB", 500L * 1024 },
        { "1MB", 1024L * 1024 }, { "4MB", 4L * 1024 * 1024 }
    };
    std::vector<std::pair<std::string, long>> selected;
    if (config.tier == "multiple") selected = tiers;
    else selected.push_back({ config.size, ParseSize(config.size) });
    std::vector<TestCase> tests;
    const bool allowSet = config.operation == "set" || config.operation == "both";
    const bool allowGet = config.operation == "get" || config.operation == "both";
    if (config.scope == "scalar" || config.scope == "both") {
        if (allowSet) tests.push_back({ "setuint64", "scalar", "{\"value\":18446744073709551615}", false, 0 });
        if (allowGet) tests.push_back({ "getuint64", "scalar", "{}", true, 0 });
    }
    if (config.scope == "tier" || config.scope == "both") {
        for (const auto& tier : selected) {
            if (allowSet) tests.push_back({ "setstring", tier.first, "{\"value\":\"" + MakePayload(tier.second) + "\"}", false, tier.second });
            if (allowGet) tests.push_back({ "getstring", tier.first, "{\"size\":" + std::to_string(tier.second) + "}", true, tier.second });
            if (tier.second <= 256 * 1024) {
                long count = 0;
                const std::string array = BuildArray(tier.second, &count);
                if (allowSet) tests.push_back({ "setarray", tier.first, "{\"value\":" + array + "}", false, tier.second });
                if (allowGet) tests.push_back({ "getarray", tier.first, "{\"size\":" + std::to_string(count) + "}", true, tier.second });

                const std::string mixed = BuildMixedArray(tier.second, &count);
                if (allowSet) tests.push_back({ "setmixedarray", tier.first, "{\"value\":" + mixed + "}", false, tier.second });
                if (allowGet) tests.push_back({ "getmixedarray", tier.first, "{\"count\":" + std::to_string(count) + "}", true, tier.second });

                const std::string nested = BuildNestedArray(tier.second, &count);
                if (allowSet) tests.push_back({ "setnestedobjects", tier.first, "{\"value\":" + nested + "}", false, tier.second });
                if (allowGet) tests.push_back({ "getnestedobjects", tier.first, "{\"count\":" + std::to_string(count) + "}", true, tier.second });
            }
        }
    }
    return tests;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string configPath = argc > 1 ? argv[1] : "/opt/AsyncJsonRpcLoadClient.config";
    Config config;
    if (!LoadConfig(configPath, &config) || config.transport != "ws") return 1;
    std::ofstream results;
    if (config.resultFileEnabled) results.open(config.resultFile, std::ios::out | std::ios::trunc);
    const auto tests = BuildTests(config);
    for (const TestCase& test : tests) {
        std::vector<std::vector<double>> samples(static_cast<size_t>(config.clients));
        std::vector<std::thread> workers;
        for (int clientIndex = 0; clientIndex < config.clients; ++clientIndex) {
            workers.emplace_back([&, clientIndex] {
                AsyncClient client(config);
                if (!client.Connect()) return;
                double elapsed = 0;
                for (int warmup = 0; warmup < config.warmup; ++warmup) client.Call(test, &elapsed, nullptr, nullptr);
                for (int iteration = 0; iteration < config.iterations; ++iteration) {
                    if (client.Call(test, &elapsed, nullptr, nullptr)) samples[static_cast<size_t>(clientIndex)].push_back(elapsed);
                }
            });
        }
        for (auto& worker : workers) worker.join();
        std::vector<double> rounds;
        for (int iteration = 0; iteration < config.iterations; ++iteration) {
            double maximum = 0;
            bool complete = true;
            for (const auto& clientSamples : samples) {
                if (static_cast<size_t>(iteration) >= clientSamples.size()) complete = false;
                else maximum = std::max(maximum, clientSamples[static_cast<size_t>(iteration)]);
            }
            if (complete) rounds.push_back(maximum);
        }
        double average = 0;
        for (double value : rounds) average += value;
        if (!rounds.empty()) average /= rounds.size();
        std::ostringstream line;
        line << "{\"method\":\"" << test.method << "\",\"tier\":\"" << test.tier
             << "\",\"clients\":" << config.clients << ",\"samples\":" << rounds.size()
             << ",\"avg_ms\":" << average << "}";
        std::cout << line.str() << std::endl;
        if (results.is_open()) results << line.str() << std::endl;
    }
    return 0;
}
