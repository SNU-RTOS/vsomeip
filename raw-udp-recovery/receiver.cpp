// Raw-socket "fast recovery" receiver - no vsomeip, no SOME/IP, plain POSIX UDP sockets.
// Pairs with sender.cpp - see that file's header comment for why this exists.
//
// Loss detection is the same idea as request-udp-recovery.cpp: an 8-byte counter in each
// UDP datagram, a non-contiguous value is a deterministic, guaranteed-real loss (no
// heuristic, no self-calibrating baseline, no false positives to chase). On a gap, this
// sends one byte to the sender's recovery port and blocks on the reply, timing the round
// trip - as minimal as a "did you get that, give me something fresh" exchange can be.
//
// Usage: ./receiver <sender-ip> [min_losses=10] [event_port=30513] [recovery_port=30514]
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

static void die(const char *what) {
    std::perror(what);
    std::exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0]
                  << " <sender-ip> [min_losses=10] [event_port=30513] [recovery_port=30514]"
                  << std::endl;
        return 1;
    }
    const char *peer_ip = argv[1];
    uint64_t min_losses = argc > 2 ? static_cast<uint64_t>(std::atoll(argv[2])) : 10;
    uint16_t event_port = argc > 3 ? static_cast<uint16_t>(std::atoi(argv[3])) : 30513;
    uint16_t recovery_port = argc > 4 ? static_cast<uint16_t>(std::atoi(argv[4])) : 30514;

    int event_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (event_fd < 0)
        die("socket(event)");
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(event_port);
    if (bind(event_fd, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0)
        die("bind(event)");

    int recovery_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (recovery_fd < 0)
        die("socket(recovery)");
    sockaddr_in peer_addr{};
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(recovery_port);
    if (inet_pton(AF_INET, peer_ip, &peer_addr.sin_addr) != 1) {
        std::cerr << "bad IP: " << peer_ip << std::endl;
        return 1;
    }
    // connect() on a UDP socket just fixes the peer for send()/recv() - no handshake,
    // still a datagram socket - but it means recv() here only ever returns packets from
    // the sender's recovery port, not any stray traffic.
    if (connect(recovery_fd, reinterpret_cast<sockaddr *>(&peer_addr), sizeof(peer_addr)) < 0)
        die("connect(recovery)");
    timeval tv{1, 0}; // 1s safety timeout - should never actually fire in a working run
    setsockopt(recovery_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::cout << "receiver: listening on :" << event_port << ", recovery RPC to " << peer_ip
              << ":" << recovery_port << (min_losses > 0 ? ", stopping after " : "")
              << (min_losses > 0 ? std::to_string(min_losses) + " recoveries" : "")
              << std::endl;

    uint64_t received = 0;
    bool have_last = false;
    uint64_t last_seq = 0;
    std::vector<int64_t> recovery_us;

    while (true) {
        uint64_t seq;
        ssize_t n = recvfrom(event_fd, &seq, sizeof(seq), 0, nullptr, nullptr);
        if (n != static_cast<ssize_t>(sizeof(seq))) {
            if (n < 0)
                std::perror("recvfrom(event)");
            continue;
        }
        ++received;

        if (have_last) {
            if (seq != last_seq + 1) {
                std::cout << "[gap] seq " << last_seq << " -> " << seq << " ("
                          << (seq - last_seq - 1) << " missing) - requesting recovery"
                          << std::endl;
                auto t0 = std::chrono::high_resolution_clock::now();
                uint8_t probe = 1;
                if (send(recovery_fd, &probe, sizeof(probe), 0) < 0) {
                    std::perror("send(recovery)");
                } else {
                    uint8_t ack;
                    ssize_t rn = recv(recovery_fd, &ack, sizeof(ack), 0);
                    auto t1 = std::chrono::high_resolution_clock::now();
                    if (rn < 0) {
                        std::cout << "[recovery timed out]" << std::endl;
                    } else {
                        int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                                t1 - t0).count();
                        recovery_us.push_back(us);
                        std::cout << "[recovered] 복구 시간 " << us << "us" << std::endl;
                        if (min_losses > 0 && recovery_us.size() >= min_losses)
                            goto done;
                    }
                }
            }
        } else {
            have_last = true;
            std::cout << "첫 이벤트 수신, seq=" << seq << std::endl;
        }
        last_seq = seq;
    }

done:
    std::cout << "총 수신 " << received << "건 중 " << recovery_us.size() << "건 유실";
    if (!recovery_us.empty()) {
        std::vector<int64_t> sorted = recovery_us;
        std::sort(sorted.begin(), sorted.end());
        int64_t sum = 0;
        for (int64_t v : sorted) sum += v;
        size_t n = sorted.size();
        std::cout << ", 평균 복구 시간 " << (sum / static_cast<int64_t>(n)) << "us"
                  << " (p50=" << sorted[n / 2] << "us"
                  << " p90=" << sorted[std::min(n - 1, static_cast<size_t>(n * 0.9))] << "us"
                  << " max=" << sorted[n - 1] << "us"
                  << " min=" << sorted[0] << "us)";
    }
    std::cout << std::endl;
    return 0;
}
