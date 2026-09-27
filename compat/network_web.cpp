// SPDX-License-Identifier: GPL-2.0-or-later
// network_web.cpp - the road to the UNCHANGED game server: a
// replacement `connect()` that routes every TCP connection of the client
// through the WebSocket bridge (bridge/), without touching game code.

// Design:
// A browser tab cannot open a TCP socket. Emscripten fakes POSIX
// `connect()` by dialling `ws://<host>:<port>/` - talking WebSocket to an
// address where a plain-TCP game server listens, which ends before it
// begins ("WebSocket connection to 'ws://<server>:13000/' failed").
// The other half is on the server side: the bridge (bridge/) listens on ONE
// port, accepts WebSocket and opens TCP to the destination named in the
// WebSocket path - ws://bridge:11496/to/<host>:<port> - if that destination
// is on its allowlist. Composing that address is the client's job.
//
// How, without touching game code: `connect()` already receives a
// `sockaddr_in` - the host name is long gone (`CNetworkStream::Connect`
// gets a ready address from `CNetworkAddress`). But emscripten keeps its
// own name table and REVERSES it on every connection:
//     getSocketAddress:  info.addr = DNS.lookup_addr(info.addr) || info.addr
//     SOCKFS:            parts = addr.split("/")
//                        url = "ws://" + parts[0] + ":" + port + "/" + parts.slice(1).join("/")
// That is the whole trick: a name
// WITH SLASHES, "127.0.0.1/to/<server>:13000", is registered in the table
// (`DNS.lookup_name` hands out a fake 172.29.x.x address), the connection
// goes to the fake address on the BRIDGE port, and emscripten, composing
// the URL, reverses the name and splits it on the slashes - exactly what
// the bridge wants. Not a line in game code, not a patch in stage_port.py.
//
// Why `connect` and nothing higher: above it `CNetworkAddress` builds the
// `sockaddr_in` from a number, so there is nothing to intercept; below it
// is `__syscall_connect`, called here exactly as musl would, with its
// error contract (`-errno`, not `-1`). The replacement covers EVERY
// connection - authentication, game channels, channel switches; the bridge
// binds one port, so switching hosts works too. Where the bridge is and
// how SOCKFS learns the scheme is decided in m2w.bridgeStart (runtime.js).

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <emscripten/emscripten.h>

extern "C" {

/// The system call musl normally makes. Returns 0 or `-errno`.
int __syscall_connect(int fd, const void* c_pAddress, int iLength);

/// Decides the bridge address once (m2w.bridgeStart).
EM_JS(void, m2w_bridge_start, (void), { m2w.bridgeStart(); });

/// The bridge port (11496 when undecided).
EM_JS(int, m2w_bridge_port, (void), {
    var k = m2w.bridge;
    return (k && k.port > 0 && k.port < 65536) ? k.port : 11496;
});

/// Fake address for `<bridge host>/to/<target>` (m2w.addressViaBridge).
EM_JS(int, m2w_address_via_bridge, (const char* c_szTarget), {
    return m2w.addressViaBridge(UTF8ToString(c_szTarget));
});

/// POSIX `connect` for the whole client: IPv4 targets go to the bridge
/// under the `/to/<host>:<port>` name; everything else takes the ordinary
/// road, so that it fails THE WAY IT ALWAYS DID rather than differently
/// through this layer.
int connect(int fd, const struct sockaddr* c_pAddress, socklen_t uLength)
{
    long lResult;

    if (c_pAddress && c_pAddress->sa_family == AF_INET && uLength >= sizeof(sockaddr_in))
    {
        m2w_bridge_start();

        const sockaddr_in* c_pIn = reinterpret_cast<const sockaddr_in*>(c_pAddress);

        char szTarget[80];
        std::snprintf(szTarget, sizeof(szTarget), "%s:%u",
                      inet_ntoa(c_pIn->sin_addr),
                      static_cast<unsigned>(ntohs(c_pIn->sin_port)));

        const int iFake = m2w_address_via_bridge(szTarget);
        const int iPort = m2w_bridge_port();

        if (iFake != 0 && iPort != 0)
        {
            // One line per connection. The client connects a few times
            // (authentication, then a channel), so this does not flood the
            // log - and with a wrong address it shows at once WHERE it
            // really went.
            std::printf("m2w network: %s -> bridge :%d\n", szTarget, iPort);

            sockaddr_in kVia = *c_pIn;
            kVia.sin_addr.s_addr = static_cast<in_addr_t>(iFake);
            kVia.sin_port = htons(static_cast<uint16_t>(iPort));

            lResult = __syscall_connect(fd, &kVia, sizeof(kVia));
            if (lResult < 0) { errno = static_cast<int>(-lResult); return -1; }
            return static_cast<int>(lResult);
        }
    }

    lResult = __syscall_connect(fd, c_pAddress, static_cast<int>(uLength));
    if (lResult < 0) { errno = static_cast<int>(-lResult); return -1; }
    return static_cast<int>(lResult);
}

}  // extern "C"
