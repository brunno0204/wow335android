#include "Engine/Source/Os/OsTcp.h"
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
// Preserve original IPv4 NETADDR layout/network-order address and host-order port.
// memcpy avoids imposing sockaddr alignment on the original byte-aligned union.
static_assert(sizeof(sockaddr_in) == sizeof(NETADDR));
void OsNetAddrMake(uint32_t address, uint16_t port, NETADDR *addr) {
    if (!addr)
        throw std::invalid_argument("Missing NETADDR output");
    sockaddr_in native{};
    native.sin_family = AF_INET;
    native.sin_port = htons(port);
    native.sin_addr.s_addr = address;
    std::memcpy(addr, &native, sizeof(native));
}
static sockaddr_in nativeAddress(const NETADDR *addr) {
    if (!addr)
        throw std::invalid_argument("Missing NETADDR input");
    sockaddr_in native;
    std::memcpy(&native, addr, sizeof(native));
    if (native.sin_family != AF_INET)
        throw std::invalid_argument("NETADDR is not IPv4");
    return native;
}
uint32_t OsNetAddrGetAddress(const NETADDR *addr, uint16_t *port) {
    const auto native = nativeAddress(addr);
    if (port)
        *port = ntohs(native.sin_port);
    return native.sin_addr.s_addr;
}
const char *OsNetAddrToStr(const NETADDR *addr, char *buffer, uint32_t size) {
    const auto native = nativeAddress(addr);
    if (!buffer || !size)
        throw std::invalid_argument("Missing NETADDR text output");
    char address[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &native.sin_addr, address, sizeof(address)))
        throw std::runtime_error("Cannot format NETADDR");
    std::snprintf(buffer, size, "%s:%u", address, unsigned(ntohs(native.sin_port)));
    return buffer;
}
