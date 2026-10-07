// Standalone client for the Thunder @async ES1 benchmark interface.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

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
    std::string host = "127.0.0.1";
    int port = 9998;
    int iterations = 20;
    std::string size = "5KB";
    int timeoutS = 30;
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
        else if (key == "host") config.host = value;
        else if (key == "port") config.port = std::stoi(value);
        else if (key == "iterations") config.iterations = std::stoi(value);
        else if (key == "size") config.size = value;
        else if (key == "timeout_s") config.timeoutS = std::stoi(value);
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

class AsyncClient {
public:
    bool Connect(const Config& config) { return _socket.Connect(config.host, config.port, config.timeoutS); }

    // The completion notification is only ever delivered to a channel that
    // explicitly registered for it first (register@<asyncId> with
    // event=<method>, id=<asyncId>) - without that step the async method
    // call still acks with "result": null but the completion never arrives.
    bool Call(const std::string& method, const std::string& parameters, uint64_t* elapsedUs)
    {
        const std::string asyncId = "es1-async-" + std::to_string(++_asyncSequence);

        if (!Register(method, asyncId)) return false;

        const uint64_t requestId = ++_requestId;
        std::string params = parameters;
        params.insert(params.size() - 1, (params.size() == 2 ? "\"id\":\"" : ",\"id\":\"") + asyncId + "\"");

        std::ostringstream request;
        request << "{\"jsonrpc\":\"2.0\",\"id\":" << requestId
                << ",\"method\":\"ES1BenchmarkAsync.1." << method
                << "\",\"params\":" << params << "}";

        const uint64_t start = NowUs();
        if (!_socket.SendText(request.str())) return false;

        const std::string ackMarker = "\"id\":" + std::to_string(requestId) + ",";
        const std::string completionMarker = "\"method\":\"" + asyncId + "." + method + "@" + asyncId + "\"";
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
            if (message.find(completionMarker) != std::string::npos) completion = true;
        }
        *elapsedUs = NowUs() - start;

        if (!Unregister(method, asyncId)) {
            std::cerr << "warning: unregister failed for " << method << " (" << asyncId << ")" << std::endl;
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
    uint64_t _asyncSequence = 0;
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

    AsyncClient client;
    if (!client.Connect(config)) {
        std::cerr << "could not connect to ES1BenchmarkAsync at " << config.host << ':' << config.port << std::endl;
        return 1;
    }

    const long size = ParseSize(config.size);
    const long arrayCount = std::min<long>(256 * 1024, std::max<long>(1, size / 5));
    const long mixedCount = std::min<long>(4228, std::max<long>(1, size / 62));
    const long nestedCount = std::min<long>(1736, std::max<long>(1, size / 151));

    for (int iteration = 0; iteration < config.iterations; ++iteration) {
        if (!Run(&client, "setstring", "{\"value\":\"" + std::string(static_cast<size_t>(size), 'A') + "\"}", iteration, &resultFile)
            || !Run(&client, "getstring", "{\"size\":" + std::to_string(size) + "}", iteration, &resultFile)
            || !Run(&client, "setarray", "{\"value\":" + ArrayPayload(arrayCount) + "}", iteration, &resultFile)
            || !Run(&client, "getarray", "{\"size\":" + std::to_string(arrayCount) + "}", iteration, &resultFile)
            || !Run(&client, "setmixedarray", "{\"value\":" + MixedPayload(mixedCount) + "}", iteration, &resultFile)
            || !Run(&client, "getmixedarray", "{\"count\":" + std::to_string(mixedCount) + "}", iteration, &resultFile)
            || !Run(&client, "setnestedobjects", "{\"value\":" + NestedPayload(nestedCount) + "}", iteration, &resultFile)
            || !Run(&client, "getnestedobjects", "{\"count\":" + std::to_string(nestedCount) + "}", iteration, &resultFile)
            || !Run(&client, "setuint32", "{\"value\":4294967295}", iteration, &resultFile)
            || !Run(&client, "getuint32", "{}", iteration, &resultFile)
            || !Run(&client, "setuint64", "{\"value\":18446744073709551615}", iteration, &resultFile)
            || !Run(&client, "getuint64", "{}", iteration, &resultFile)
            || !Run(&client, "setbool", "{\"value\":true}", iteration, &resultFile)
            || !Run(&client, "getbool", "{}", iteration, &resultFile)
            || !Run(&client, "setfloat", "{\"value\":3.4028235e+38}", iteration, &resultFile)
            || !Run(&client, "getfloat", "{}", iteration, &resultFile)
            || !Run(&client, "setdouble", "{\"value\":1.7976931348623157e+308}", iteration, &resultFile)
            || !Run(&client, "getdouble", "{}", iteration, &resultFile)
            || !Run(&client, "measurecopycost", "{\"size\":" + std::to_string(arrayCount) + "}", iteration, &resultFile)
            || !Run(&client, "measurestringresizecost", "{\"size\":" + std::to_string(size) + "}", iteration, &resultFile)
            || !Run(&client, "measuremixedassigncost", "{\"count\":" + std::to_string(mixedCount) + "}", iteration, &resultFile)
            || !Run(&client, "measurenestedassigncost", "{\"count\":" + std::to_string(nestedCount) + "}", iteration, &resultFile)) {
            return 1;
        }
    }

    return 0;
}