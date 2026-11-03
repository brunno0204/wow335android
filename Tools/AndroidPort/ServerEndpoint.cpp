#include "ServerEndpoint.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <memory>
#include <netdb.h>
#include <stdexcept>
namespace wow::android {
namespace {
std::string numeric(const std::string &host) {
    if (host.empty() || host.size() > 45 || host.find('\0') != std::string::npos)
        return {};
    in_addr v4{};
    in6_addr v6{};
    char output[INET6_ADDRSTRLEN];
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        if (inet_ntop(AF_INET, &v4, output, sizeof(output)))
            return output;
    } else if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        if (inet_ntop(AF_INET6, &v6, output, sizeof(output)))
            return output;
    }
    return {};
}
std::string checkedHost(const std::string &text) {
    if (auto ip = numeric(text); !ip.empty())
        return ip;
    if (text.empty() || text.size() > 253)
        throw std::invalid_argument("Invalid server hostname");
    std::string host = text;
    const bool absolute = host.back() == '.';
    if (absolute)
        host.pop_back();
    if (host.empty())
        throw std::invalid_argument("Empty DNS hostname");
    size_t count = 0;
    bool priorHyphen = false;
    for (char &c : host) {
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        if (c == '.') {
            if (!count || priorHyphen)
                throw std::invalid_argument("Invalid DNS label");
            count = 0;
            priorHyphen = false;
            continue;
        }
        const bool alpha = (c >= 'a' && c <= 'z'), digit = (c >= '0' && c <= '9');
        if ((!alpha && !digit && c != '-') || (!count && c == '-') || ++count > 63)
            throw std::invalid_argument("Invalid DNS label");
        priorHyphen = c == '-';
    }
    if (!count || priorHyphen)
        throw std::invalid_argument("Invalid DNS label");
    // Invalid dotted numeric addresses must not be sent to DNS as hostnames.
    if (host.find_first_not_of("0123456789.") == std::string::npos)
        throw std::invalid_argument("Invalid numeric IPv4 address");
    return absolute ? host + "." : host;
}
uint16_t checkedPort(const std::string &text) {
    if (text.empty() || text.size() > 5)
        throw std::invalid_argument("Invalid server port");
    uint32_t port = 0;
    for (char c : text) {
        if (c < '0' || c > '9')
            throw std::invalid_argument("Invalid server port");
        port = port * 10 + unsigned(c - '0');
    }
    if (!port || port > 65535)
        throw std::invalid_argument("Invalid server port");
    return uint16_t(port);
}
} // namespace
ServerEndpoint parseServerEndpoint(const std::string &text) {
    if (text.empty() || text.size() > 261 || text.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid server endpoint");
    ServerEndpoint endpoint;
    if (text.front() == '[') {
        const auto end = text.find(']');
        if (end == std::string::npos)
            throw std::invalid_argument("Unclosed IPv6 endpoint");
        endpoint.host = numeric(text.substr(1, end - 1));
        if (endpoint.host.empty() || endpoint.host.find(':') == std::string::npos)
            throw std::invalid_argument("Brackets require numeric IPv6");
        if (end + 1 < text.size()) {
            if (text[end + 1] != ':')
                throw std::invalid_argument("Invalid IPv6 port delimiter");
            endpoint.port = checkedPort(text.substr(end + 2));
        }
    } else if (std::count(text.begin(), text.end(), ':') > 1) {
        endpoint.host = numeric(text);
        if (endpoint.host.empty())
            throw std::invalid_argument("Invalid bare IPv6 endpoint");
    } else {
        const auto colon = text.find(':');
        endpoint.host = checkedHost(text.substr(0, colon));
        if (colon != std::string::npos)
            endpoint.port = checkedPort(text.substr(colon + 1));
    }
    return endpoint;
}
ResolvedServerEndpoint prepareResolvedServerEndpoint(const std::vector<std::string> &addresses,
                                                     uint16_t port) {
    if (!port || addresses.empty() || addresses.size() > 16)
        throw std::invalid_argument("Invalid resolved endpoint budget/port");
    ResolvedServerEndpoint result;
    result.port = port;
    for (const auto &address : addresses) {
        auto ip = numeric(address);
        if (ip.empty())
            throw std::invalid_argument("Resolved endpoint contains a nonnumeric address");
        if (std::find(result.addresses.begin(), result.addresses.end(), ip) ==
            result.addresses.end())
            result.addresses.push_back(std::move(ip));
    }
    return result;
}
ResolvedServerEndpoint resolveServerEndpoint(const ServerEndpoint &input) {
    if (!input.port)
        throw std::invalid_argument("Invalid resolver port");
    const std::string host = checkedHost(input.host);
    if (auto ip = numeric(host); !ip.empty())
        return prepareResolvedServerEndpoint({ip}, input.port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo *raw = nullptr;
    const auto service = std::to_string(input.port);
    const int error = getaddrinfo(host.c_str(), service.c_str(), &hints, &raw);
    if (error)
        throw std::runtime_error("System hostname resolution failed");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> records(raw, freeaddrinfo);
    std::vector<std::string> addresses;
    size_t inspected = 0;
    for (auto *entry = raw; entry && inspected < 256 && addresses.size() < 16;
         entry = entry->ai_next, ++inspected) {
        if (!entry->ai_addr)
            continue;
        char address[INET6_ADDRSTRLEN];
        const void *bytes = nullptr;
        if (entry->ai_family == AF_INET && entry->ai_addrlen >= sizeof(sockaddr_in))
            bytes = &reinterpret_cast<const sockaddr_in *>(entry->ai_addr)->sin_addr;
        else if (entry->ai_family == AF_INET6 && entry->ai_addrlen >= sizeof(sockaddr_in6)) {
            const auto *a = reinterpret_cast<const sockaddr_in6 *>(entry->ai_addr);
            if (a->sin6_scope_id)
                continue;
            bytes = &a->sin6_addr;
        }
        if (bytes && inet_ntop(entry->ai_family, bytes, address, sizeof(address)) &&
            std::find(addresses.begin(), addresses.end(), address) == addresses.end())
            addresses.emplace_back(address);
    }
    return prepareResolvedServerEndpoint(addresses, input.port);
}
NativeTcp::State TcpEndpointConnector::state() const noexcept {
    return expired_ ? NativeTcp::State::Failed : stream_.state();
}
int TcpEndpointConnector::error() const noexcept { return expired_ ? ETIMEDOUT : stream_.error(); }
void TcpEndpointConnector::expire() noexcept {
    stream_.close();
    expired_ = true;
}
void TcpEndpointConnector::close() noexcept {
    stream_.close();
    endpoint_.addresses.clear();
    next_ = 0;
    established_ = expired_ = false;
}
void TcpEndpointConnector::connect(const ResolvedServerEndpoint &endpoint, uint32_t timeoutMs) {
    if (!timeoutMs || timeoutMs > 60000)
        throw std::invalid_argument("Invalid endpoint connect budget");
    auto prepared = prepareResolvedServerEndpoint(endpoint.addresses, endpoint.port);
    close();
    endpoint_ = std::move(prepared);
    deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    startNext();
}
void TcpEndpointConnector::startNext() {
    while (next_ < endpoint_.addresses.size()) {
        const auto remaining = deadline_ - std::chrono::steady_clock::now();
        if (remaining <= decltype(remaining)::zero()) {
            expire();
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        // Share remaining time among remaining candidates so a silent first IP
        // cannot consume the entire total deadline before fallback is attempted.
        const auto count = int64_t(endpoint_.addresses.size() - next_);
        const auto slice = std::max<int64_t>(1, (ms + 1) / count);
        stream_.connect(endpoint_.addresses[next_++], endpoint_.port,
                        uint32_t(std::min<int64_t>(60000, slice)));
        if (stream_.state() == NativeTcp::State::Connected) {
            established_ = true;
            return;
        }
        if (stream_.state() == NativeTcp::State::Connecting)
            return;
    }
}
void TcpEndpointConnector::pump(uint32_t waitMs) {
    if (waitMs > 100)
        throw std::invalid_argument("Endpoint pump wait exceeds100ms");
    if (expired_ || state() == NativeTcp::State::Closed ||
        (state() == NativeTcp::State::Failed && next_ >= endpoint_.addresses.size()))
        return;
    if (!established_) {
        const auto remaining = deadline_ - std::chrono::steady_clock::now();
        if (remaining <= decltype(remaining)::zero()) {
            expire();
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        waitMs = std::min(waitMs, uint32_t(ms + 1));
    }
    stream_.pump(waitMs);
    if (!established_) {
        if (stream_.state() == NativeTcp::State::Connected)
            established_ = true;
        else if (stream_.state() == NativeTcp::State::Failed)
            startNext();
    }
}
} // namespace wow::android
