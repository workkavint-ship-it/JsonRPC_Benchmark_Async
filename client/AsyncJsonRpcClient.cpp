// Standalone client for the Thunder ES1 benchmark event interface.

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
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
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
    if (first == std::string::npos) {
        return std::string();
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string JsonEscape(const std::string& value)
{
    std::string result;
    result.reserve(value.size() + 2);
    for (const char character : value) {
        if (character == '\\' || character == '"') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

struct Config {
    std::string mode = "warm";
    std::string transport = "ws";
    std::string host = "127.0.0.1";
    int port = 9998;
    int iterations = 20;
    int warmup = 3;
    std::string size = "5KB";
    std::string tier = "multiple";
    std::vector<int> clients = { 1 };
    bool skipScalars = false;
    int timeoutS = 30;
    int coldstartPollMs = 100;
    int coldstartTimeoutS = 120;
    bool resultFileEnabled = false;
    std::string resultFile = "/opt/es1-async-results.jsonl";
};

bool LoadConfig(const std::string& path, Config* output)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }

    Config config;
    std::string line;
    while (std::getline(input, line)) {
        const std::string entry = Trim(line);
        if (entry.empty() || entry[0] == '#') {
            continue;
        }
        const size_t separator = entry.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = Trim(entry.substr(0, separator));
        const std::string value = Trim(entry.substr(separator + 1));
        if (key == "mode") config.mode = value;
        else if (key == "transport") config.transport = value;
        else if (key == "host") config.host = value;
        else if (key == "port") config.port = std::stoi(value);
        else if (key == "iterations") config.iterations = std::stoi(value);
        else if (key == "warmup") config.warmup = std::stoi(value);
        else if (key == "size") config.size = value;
        else if (key == "tier") config.tier = value;
        else if (key == "skip_scalars") config.skipScalars = (value == "true" || value == "1");
        else if (key == "clients") {
            config.clients.clear();
            std::stringstream clients(value);
            std::string token;
            while (std::getline(clients, token, ',')) {
                token = Trim(token);
                if (!token.empty()) config.clients.push_back(std::stoi(token));
            }
            if (config.clients.empty()) config.clients.push_back(1);
        }
        else if (key == "timeout_s") config.timeoutS = std::stoi(value);
        else if (key == "coldstart_poll_ms") config.coldstartPollMs = std::stoi(value);
        else if (key == "coldstart_timeout_s") config.coldstartTimeoutS = std::stoi(value);
        else if (key == "result_file_enabled") config.resultFileEnabled = (value == "true" || value == "1");
        else if (key == "result_file") config.resultFile = value;
    }
    *output = config;
    return true;
}

long ParseSize(const std::string& text)
{
    std::string value = text;
    for (char& character : value) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    if (value.size() > 2 && value.substr(value.size() - 2) == "KB") {
        return std::stol(value.substr(0, value.size() - 2)) * 1024L;
    }
    if (value.size() > 2 && value.substr(value.size() - 2) == "MB") {
        return std::stol(value.substr(0, value.size() - 2)) * 1024L * 1024L;
    }
    if (!value.empty() && value.back() == 'B') {
        return std::stol(value.substr(0, value.size() - 1));
    }
    return std::stol(value);
}

int ConnectTcp(const std::string& host, int port, int timeoutS)
{
    const int socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) return -1;

    timeval timeout { timeoutS, 0 };
    ::setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int enabled = 1;
    ::setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1
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
        result.push_back(table[(value >> 18) & 0x3F]);
        result.push_back(table[(value >> 12) & 0x3F]);
        result.push_back(remaining > 1 ? table[(value >> 6) & 0x3F] : '=');
        result.push_back(remaining > 2 ? table[value & 0x3F] : '=');
    }
    return result;
}

class WebSocket {
public:
    ~WebSocket() { Close(); }

    bool Connect(const std::string& host, int port, int timeoutS)
    {
        _socket = ConnectTcp(host, port, timeoutS);
        if (_socket < 0) return false;

        std::mt19937 generator(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
        uint8_t keyBytes[16];
        for (uint8_t& byte : keyBytes) byte = static_cast<uint8_t>(generator() & 0xFF);

        std::ostringstream request;
        request << "GET /jsonrpc HTTP/1.1\r\n"
                << "Host: " << host << ":" << port << "\r\n"
                << "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                << "Sec-WebSocket-Key: " << Base64(keyBytes, sizeof(keyBytes)) << "\r\n"
                << "Sec-WebSocket-Version: 13\r\n\r\n";
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
            if (opcode == 0x9) {
                SendFrame(0xA, payload);
                continue;
            }
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
        if (length < 126) {
            frame.push_back(static_cast<char>(0x80 | length));
        } else if (length <= 0xFFFF) {
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

// Splices a "requestId" field into a pre-built JSON params object, e.g.
// "{\"value\":1}" -> "{\"requestId\":7,\"value\":1}", or "{}" -> "{\"requestId\":7}".
// Assumes params is always a plain top-level JSON object (true for every
// test case this client builds), so no general JSON parsing is needed.
std::string InjectRequestId(const std::string& parameters, uint64_t requestId)
{
    const std::string field = "\"requestId\":" + std::to_string(requestId);
    const std::string trimmed = Trim(parameters);
    if (trimmed.size() < 2 || trimmed.front() != '{' || trimmed.back() != '}') {
        return parameters;
    }
    const std::string body = Trim(trimmed.substr(1, trimmed.size() - 2));
    if (body.empty()) {
        return "{" + field + "}";
    }
    return "{" + field + "," + body + "}";
}

class AsyncClient {
public:
    AsyncClient()
        : _clientId("es1-client-" + std::to_string(static_cast<unsigned long long>(::getpid()))
            + "-" + std::to_string(++g_clientSequence))
    {
    }

    bool Connect(const Config& config) { return _socket.Connect(config.host, config.port, config.timeoutS); }

    bool Call(const std::string& method, const std::string& parameters, uint64_t* elapsedUs)
    {
    const std::string event = method + "completed";
    const std::string& clientId = _clientId;

    if (!Register(event, clientId)) return false;

        const uint64_t requestId = ++_requestId;

        std::ostringstream request;
        request << "{\"jsonrpc\":\"2.0\",\"id\":" << requestId
                << ",\"method\":\"ES1BenchmarkAsync.1." << method
        << "\",\"params\":" << InjectRequestId(parameters, requestId) << "}";

        const uint64_t start = NowUs();
        if (!_socket.SendText(request.str())) return false;

        const std::string ackMarker = "\"id\":" + std::to_string(requestId) + ",";
        // The event name narrows a received notification down to "this
        // channel, this event" - but every client registered for the same
        // event still receives every completion of that event (Thunder
        // personalizes only the id-prefix per recipient, not the payload).
        // The requestId marker is what actually confirms this specific
        // completion belongs to this specific call, not some other client's.
        const std::string completionMarker = "\"method\":\"" + clientId + "." + event + "\"";
        const std::string requestIdMarker = "\"requestId\":" + std::to_string(requestId);
        bool acknowledgement = false;
        bool completion = false;
        std::string message;
        while (!acknowledgement || !completion) {
            if (!_socket.Receive(&message)) return false;
            if (!acknowledgement && message.find(ackMarker) != std::string::npos) {
                if (message.find("\"error\"") != std::string::npos) return false;
                acknowledgement = true;
                continue;
            }
            if (message.find(completionMarker) != std::string::npos
                && message.find(requestIdMarker) != std::string::npos) {
                completion = true;
            }
        }
        *elapsedUs = NowUs() - start;

        if (!Unregister(event, clientId)) {
            std::cerr << "warning: unregister failed for " << event << " (" << clientId << ")" << std::endl;
        }
        return true;
    }

    static uint64_t NowUs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

private:
    // Sends a request and waits only for the response whose "id" matches
    // requestId (anchored on the trailing comma so e.g. id 1 cannot match
    // inside id 10), ignoring any other traffic interleaved on the channel.
    bool SendAndWaitAck(uint64_t requestId, const std::string& request)
    {
        if (!_socket.SendText(request)) return false;
        const std::string marker = "\"id\":" + std::to_string(requestId) + ",";
        std::string message;
        for (;;) {
            if (!_socket.Receive(&message)) return false;
            if (message.find(marker) == std::string::npos) continue;
            return message.find("\"error\"") == std::string::npos;
        }
    }

    bool Register(const std::string& method, const std::string& asyncId)
    {
        const uint64_t requestId = ++_requestId;
        std::ostringstream request;
        request << "{\"jsonrpc\":\"2.0\",\"id\":" << requestId
                << ",\"method\":\"ES1BenchmarkAsync.1.register@" << asyncId
                << "\",\"params\":{\"event\":\"" << method << "\",\"id\":\"" << asyncId << "\"}}";
        return SendAndWaitAck(requestId, request.str());
    }

    bool Unregister(const std::string& method, const std::string& asyncId)
    {
        const uint64_t requestId = ++_requestId;
        std::ostringstream request;
        request << "{\"jsonrpc\":\"2.0\",\"id\":" << requestId
                << ",\"method\":\"ES1BenchmarkAsync.1.unregister@" << asyncId
                << "\",\"params\":{\"event\":\"" << method << "\",\"id\":\"" << asyncId << "\"}}";
        return SendAndWaitAck(requestId, request.str());
    }

    WebSocket _socket;
    uint64_t _requestId = 0;
    std::string _clientId;
};

std::string ArrayPayload(long count)
{
    std::ostringstream output;
    output << '[';
    for (long index = 0; index < count; ++index) {
        if (index != 0) output << ',';
        output << (index % 256);
    }
    output << ']';
    return output.str();
}

std::string MixedPayload(long count)
{
    std::ostringstream output;
    output << '[';
    for (long index = 0; index < count; ++index) {
        if (index != 0) output << ',';
        output << "{\"id\":" << index << ",\"name\":\"item" << index
               << "\",\"value\":" << index * 3.14159
               << ",\"flag\":" << ((index % 2 == 0) ? "true" : "false") << '}';
    }
    output << ']';
    return output.str();
}

std::string NestedPayload(long count)
{
    std::ostringstream output;
    output << '[';
    for (long index = 0; index < count; ++index) {
        if (index != 0) output << ',';
        output << "{\"id\":" << index
               << ",\"flag\":" << ((index % 2 == 0) ? "true" : "false")
               << ",\"score\":" << index * 3.14159
               << ",\"data\":{\"label\":\"item" << index
               << "\",\"nested\":{\"count\":" << index
               << ",\"inner\":{\"value\":" << index * 3
               << ",\"name\":\"deep" << index << "\"}}}}";
    }
    output << ']';
    return output.str();
}

void Emit(const std::string& line, std::ofstream* resultFile)
{
    std::cout << line << std::endl;
    if (resultFile != nullptr && resultFile->is_open()) {
        *resultFile << line << std::endl;
        resultFile->flush();
    }
}

bool Run(AsyncClient* client, const std::string& method, const std::string& params, int iteration, std::ofstream* resultFile)
{
    uint64_t elapsedUs = 0;
    if (!client->Call(method, params, &elapsedUs)) {
        std::cerr << "async JSON-RPC call failed: " << method << std::endl;
        return false;
    }
    std::ostringstream result;
    result << "{\"method\":\"" << method << "\",\"iteration\":" << iteration
           << ",\"async_roundtrip_us\":" << elapsedUs << '}';
    Emit(result.str(), resultFile);
    return true;
}

struct TestCase {
    std::string method;
    std::string params;
    std::string tier;
    long payloadSize;
};

std::string ExactArray(long targetBytes, const std::string& kind, long limit, long* countOut)
{
    const double average = kind == "array" ? 4.57 : kind == "mixed" ? 62.0 : 151.0;
    long count = std::min<long>(limit, std::max<long>(1, static_cast<long>(targetBytes / average)));
    std::vector<std::string> elements;
    elements.reserve(static_cast<size_t>(count));

    const auto makeElement = [&kind](const long index) {
        std::ostringstream element;
        if (kind == "array") {
            element << (index % 256);
        } else if (kind == "mixed") {
            element << "{\"id\":" << index << ",\"name\":\"item" << index
                    << "\",\"value\":" << index * 3.14159
                    << ",\"flag\":" << ((index % 2 == 0) ? "true" : "false") << '}';
        } else {
            element << "{\"id\":" << index
                    << ",\"flag\":" << ((index % 2 == 0) ? "true" : "false")
                    << ",\"score\":" << index * 3.14159
                    << ",\"data\":{\"label\":\"item" << index
                    << "\",\"nested\":{\"count\":" << index
                    << ",\"inner\":{\"value\":" << index * 3
                    << ",\"name\":\"deep" << index << "\"}}}}";
        }
        return element.str();
    };

    for (long index = 0; index < count; ++index) {
        elements.push_back(makeElement(index));
    }

    auto length = [&elements]() {
        long result = 2;
        for (size_t index = 0; index < elements.size(); ++index) {
            result += static_cast<long>(elements[index].size()) + (index == 0 ? 0 : 1);
        }
        return result;
    };

    long current = length();
    for (long index = count; current < targetBytes && index < limit; ++index) {
        const std::string element = makeElement(index);
        current += static_cast<long>(element.size()) + 1;
        elements.push_back(std::move(element));
    }
    while (current > targetBytes && elements.size() > 1) {
        current -= static_cast<long>(elements.back().size()) + 1;
        elements.pop_back();
    }

    if (countOut != nullptr) *countOut = static_cast<long>(elements.size());
    std::ostringstream output;
    output << '[';
    for (size_t index = 0; index < elements.size(); ++index) {
        if (index != 0) output << ',';
        output << elements[index];
    }
    output << ']';
    return output.str();
}

std::vector<TestCase> BuildTierTests(const std::string& tier, long size)
{
    std::vector<TestCase> tests;
    tests.push_back({ "setstring", "{\"value\":\"" + std::string(static_cast<size_t>(size), 'A') + "\"}", tier, size });
    tests.push_back({ "getstring", "{\"size\":" + std::to_string(size) + "}", tier, size });

    long arrayCount = 0;
    long mixedCount = 0;
    long nestedCount = 0;
    const std::string array = ExactArray(size, "array", 256 * 1024, &arrayCount);
    const std::string mixed = ExactArray(size, "mixed", 4228, &mixedCount);
    const std::string nested = ExactArray(size, "nested", 1736, &nestedCount);
    tests.push_back({ "setarray", "{\"value\":" + array + "}", tier, size });
    tests.push_back({ "getarray", "{\"size\":" + std::to_string(arrayCount) + "}", tier, size });
    tests.push_back({ "setmixedarray", "{\"value\":" + mixed + "}", tier, size });
    tests.push_back({ "getmixedarray", "{\"count\":" + std::to_string(mixedCount) + "}", tier, size });
    tests.push_back({ "setnestedobjects", "{\"value\":" + nested + "}", tier, size });
    tests.push_back({ "getnestedobjects", "{\"count\":" + std::to_string(nestedCount) + "}", tier, size });
    return tests;
}

std::vector<TestCase> BuildScalarTests()
{
    return {
        { "setuint32", "{\"value\":4294967295}", "scalar", 0 },
        { "getuint32", "{}", "scalar", 0 },
        { "setuint64", "{\"value\":18446744073709551615}", "scalar", 0 },
        { "getuint64", "{}", "scalar", 0 },
        { "setbool", "{\"value\":true}", "scalar", 0 },
        { "getbool", "{}", "scalar", 0 },
        { "setfloat", "{\"value\":3.4028235e+38}", "scalar", 0 },
        { "getfloat", "{}", "scalar", 0 },
        { "setdouble", "{\"value\":1.7976931348623157e+308}", "scalar", 0 },
        { "getdouble", "{}", "scalar", 0 }
    };
}

class StartGate {
public:
    explicit StartGate(const int expected) : _expected(expected) {}

    void Arrive()
    {
        std::unique_lock<std::mutex> lock(_lock);
        ++_arrived;
        _condition.notify_all();
        _condition.wait(lock, [this] { return _released; });
    }

    void Release()
    {
        std::lock_guard<std::mutex> lock(_lock);
        _released = true;
        _condition.notify_all();
    }

    void WaitForAll()
    {
        std::unique_lock<std::mutex> lock(_lock);
        _condition.wait(lock, [this] { return _arrived == _expected; });
    }

private:
    std::mutex _lock;
    std::condition_variable _condition;
    int _expected;
    int _arrived = 0;
    bool _released = false;
};

struct Stats {
    double minMs = 0;
    double maxMs = 0;
    double averageMs = 0;
    double standardDeviationMs = 0;
    int samples = 0;
    int failed = 0;
};

Stats CalculateStats(const std::vector<double>& samples, const int failed)
{
    Stats stats;
    stats.failed = failed;
    if (samples.empty()) return stats;

    stats.samples = static_cast<int>(samples.size());
    stats.minMs = *std::min_element(samples.begin(), samples.end());
    stats.maxMs = *std::max_element(samples.begin(), samples.end());
    for (double sample : samples) stats.averageMs += sample;
    stats.averageMs /= samples.size();
    for (double sample : samples) {
        const double delta = sample - stats.averageMs;
        stats.standardDeviationMs += delta * delta;
    }
    stats.standardDeviationMs = std::sqrt(stats.standardDeviationMs / samples.size());
    return stats;
}

bool WaitForReady(const Config& config)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(config.coldstartTimeoutS);
    while (std::chrono::steady_clock::now() < deadline) {
        AsyncClient client;
        if (client.Connect(config)) {
            uint64_t elapsed = 0;
            if (client.Call("getuint64", "{}", &elapsed)) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(config.coldstartPollMs));
    }
    return false;
}

void RunTest(const Config& config, const TestCase& test, const int clientCount, std::ofstream* resultFile)
{
    StartGate gate(clientCount);
    std::vector<std::vector<double>> clientSamples(static_cast<size_t>(clientCount));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(clientCount));

    for (int clientIndex = 0; clientIndex < clientCount; ++clientIndex) {
        workers.emplace_back([&, clientIndex] {
            AsyncClient client;
            if (!client.Connect(config)) {
                gate.Arrive();
                return;
            }

            uint64_t elapsed = 0;
            for (int warmup = 0; warmup < config.warmup; ++warmup) {
                client.Call(test.method, test.params, &elapsed);
            }

            gate.Arrive();
            for (int iteration = 0; iteration < config.iterations; ++iteration) {
                if (client.Call(test.method, test.params, &elapsed)) {
                    clientSamples[static_cast<size_t>(clientIndex)].push_back(elapsed / 1000.0);
                }
            }
        });
    }

    gate.WaitForAll();
    gate.Release();
    for (std::thread& worker : workers) worker.join();

    std::vector<double> roundSamples;
    int failed = 0;
    for (int iteration = 0; iteration < config.iterations; ++iteration) {
        double roundMaximum = 0;
        bool complete = true;
        for (int clientIndex = 0; clientIndex < clientCount; ++clientIndex) {
            const auto& samples = clientSamples[static_cast<size_t>(clientIndex)];
            if (static_cast<size_t>(iteration) >= samples.size()) {
                complete = false;
                break;
            }
            roundMaximum = std::max(roundMaximum, samples[static_cast<size_t>(iteration)]);
        }
        if (complete) roundSamples.push_back(roundMaximum);
        else ++failed;
    }

    const Stats stats = CalculateStats(roundSamples, failed);
    std::ostringstream output;
    output << "{\"method\":\"" << test.method
           << "\",\"tier\":\"" << test.tier
           << "\",\"payload_bytes\":" << test.payloadSize
           << ",\"clients\":" << clientCount
           << ",\"iterations\":" << config.iterations
           << ",\"warmup\":" << config.warmup
           << ",\"roundtrip_ms\":{\"min\":" << stats.minMs
           << ",\"max\":" << stats.maxMs
           << ",\"avg\":" << stats.averageMs
           << ",\"stddev\":" << stats.standardDeviationMs
           << "},\"samples\":" << stats.samples
           << ",\"failed\":" << stats.failed << '}';
    Emit(output.str(), resultFile);
}

int RunMatrix(const Config& config, std::ofstream* resultFile)
{
    if (config.transport != "ws") {
        std::cerr << "async client supports only transport=ws because completions are WebSocket events\n";
        return 1;
    }
    if (config.mode == "coldstart" && !WaitForReady(config)) {
        std::cerr << "ES1BenchmarkAsync did not become ready before timeout\n";
        return 1;
    }

    std::vector<TestCase> tests;
    if (!config.skipScalars) {
        const auto scalarTests = BuildScalarTests();
        tests.insert(tests.end(), scalarTests.begin(), scalarTests.end());
    }
    const std::vector<std::pair<std::string, long>> tiers = {
        { "500B", 500 }, { "5KB", 5L * 1024 }, { "50KB", 50L * 1024 },
        { "150KB", 150L * 1024 }, { "500KB", 500L * 1024 },
        { "1MB", 1L * 1024 * 1024 }, { "4MB", 4L * 1024 * 1024 }
    };
    if (config.tier == "multiple") {
        for (const auto& tier : tiers) {
            const auto tierTests = BuildTierTests(tier.first, tier.second);
            tests.insert(tests.end(), tierTests.begin(), tierTests.end());
        }
    } else {
        const long size = ParseSize(config.size);
        const auto tierTests = BuildTierTests(config.size, size);
        tests.insert(tests.end(), tierTests.begin(), tierTests.end());
    }

    for (int clientCount : config.clients) {
        for (const TestCase& test : tests) {
            RunTest(config, test, std::max(1, clientCount), resultFile);
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string configPath = argc > 1 ? argv[1] : "/opt/es1-async.config";
    Config config;
    if (!LoadConfig(configPath, &config)) {
        return 0;
    }

    std::ofstream resultFile;
    if (config.resultFileEnabled) {
        resultFile.open(config.resultFile, std::ios::out | std::ios::trunc);
    }

    return RunMatrix(config, &resultFile);
}