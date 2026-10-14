// Raw-socket "fast recovery" sender - no vsomeip, no SOME/IP, plain POSIX UDP sockets.
//
// Pairs with receiver.cpp. Exists to answer one question cleanly: the vsomeip-based UDP
// prototype (examples/response-udp-recovery.cpp) measured RPC recovery round trips of
// ~13.9ms - *worse* than TCP's kernel-retransmission path (~2.6ms) - and a wire capture
// on the recovery RPC's own port showed the delay sitting between the request arriving
// at the server and the response leaving it, i.e. inside the server process, not the
// network. The leading suspect is vsomeip's own single dispatch thread, busy with the
// 1ms event-publish loop's internal bookkeeping, serializing the recovery handler behind
// it - a vsomeip-specific cost, not evidence about the kernel socket-receive path this
// whole investigation has been chasing (see CHANGES_THOR.md's bpftrace section).
//
// This version removes vsomeip from both the event stream and the recovery RPC entirely:
// two plain UDP sockets, one blocking recv/send loop each. If recovery time here is still
// ~2ms+, the cost really is generic (kernel receive path, CONFIG_HZ-tick-gated wakeup,
// whatever it is) and would show up no matter what middleware sits on top. If it drops to
// near the TCP wire-level number (hundreds of µs), that specific ~2ms-plus application-level
// gap was a vsomeip dispatch-queue cost all along, not a kernel one.
//
// Usage: ./sender <receiver-ip> [cycle_ms=1] [event_port=30513] [recovery_port=30514]
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>

static void die(const char *what) {
    std::perror(what);
    std::exit(1);
}

static int make_bound_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        die("socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        die("bind");
    return fd;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0]
                  << " <receiver-ip> [cycle_ms=1] [event_port=30513] [recovery_port=30514]"
                  << std::endl;
        return 1;
    }
    const char *peer_ip = argv[1];
    int cycle_ms = argc > 2 ? std::atoi(argv[2]) : 1;
    uint16_t event_port = argc > 3 ? static_cast<uint16_t>(std::atoi(argv[3])) : 30513;
    uint16_t recovery_port = argc > 4 ? static_cast<uint16_t>(std::atoi(argv[4])) : 30514;

    int event_fd = make_bound_socket(event_port);
    sockaddr_in peer_addr{};
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(event_port);
    if (inet_pton(AF_INET, peer_ip, &peer_addr.sin_addr) != 1) {
        std::cerr << "bad IP: " << peer_ip << std::endl;
        return 1;
    }

    std::cout << "sender: publishing to " << peer_ip << ":" << event_port << " every "
              << cycle_ms << "ms, recovery RPC on :" << recovery_port << std::endl;

    // Publish loop, its own thread - same structure as response-udp-recovery.cpp's
    // offer_thread_, just without vsomeip underneath it.
    std::thread publisher([&]() {
        uint64_t seq = 0;
        while (true) {
            uint64_t be = seq; // host byte order is fine - both ends are the same arch,
                                // and this is a throwaway prototype, not a wire protocol.
            ssize_t n = sendto(event_fd, &be, sizeof(be), 0,
                                reinterpret_cast<sockaddr *>(&peer_addr), sizeof(peer_addr));
            if (n < 0)
                std::perror("sendto(event)");
            ++seq;
            std::this_thread::sleep_for(std::chrono::milliseconds(cycle_ms));
        }
    });

    // Recovery RPC loop, main thread: block on recvfrom, answer immediately with the
    // current seq. No dispatch queue, no handler registry - this IS the handler.
    int recovery_fd = make_bound_socket(recovery_port);
    uint64_t current_seq = 0;
    // current_seq here is only ever read by this thread; the publisher thread owns its
    // own `seq` above and this loop does not share it - deliberately: wiring up a shared
    // counter would need synchronization whose cost might itself confound the very
    // measurement this tool exists to take. The response answers with *a* recent value by
    // re-sending a one-byte probe back - the client under test only cares about the
    // round-trip time, not which exact value comes back (see receiver.cpp).
    (void)current_seq;
    char buf[64];
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    while (true) {
        ssize_t n = recvfrom(recovery_fd, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr *>(&from), &from_len);
        if (n < 0) {
            std::perror("recvfrom(recovery)");
            continue;
        }
        uint8_t ack = 1;
        sendto(recovery_fd, &ack, sizeof(ack), 0, reinterpret_cast<sockaddr *>(&from), from_len);
    }
}
