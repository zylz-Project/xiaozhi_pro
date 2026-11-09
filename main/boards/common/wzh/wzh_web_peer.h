#ifndef WZH_WEB_PEER_H_
#define WZH_WEB_PEER_H_

#include <netinet/in.h>
#include <sys/socket.h>
#include <cstdint>

namespace wzh {
// ESP-IDF's HTTP server uses an IPv6 listening socket when IPv6 is enabled.
// IPv4 clients then arrive as IPv4-mapped IPv6 addresses on that socket.
inline bool WebPeerIpv4(int socket, uint32_t& ip) {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (getpeername(socket, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
        return false;
    const auto* address = reinterpret_cast<const sockaddr*>(&storage);
    if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&storage);
        ip = ntohl(ipv4->sin_addr.s_addr);
        return true;
    }
#if CONFIG_LWIP_IPV6
    if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&storage);
        const auto* bytes = ipv6->sin6_addr.s6_addr;
        for (int i = 0; i < 10; ++i)
            if (bytes[i] != 0)
                return false;
        if (bytes[10] != 0xff || bytes[11] != 0xff)
            return false;
        ip = (uint32_t(bytes[12]) << 24) | (uint32_t(bytes[13]) << 16) |
             (uint32_t(bytes[14]) << 8) | uint32_t(bytes[15]);
        return true;
    }
#endif
    return false;
}
}  // namespace wzh
#endif  // WZH_WEB_PEER_H_
