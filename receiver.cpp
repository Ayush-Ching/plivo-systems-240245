#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <chrono>
#include <algorithm>

#pragma pack(push, 1)
struct RelayPacket {
    uint8_t type;         // 0 = data, 1 = parity
    uint32_t seq;         // big-endian
    unsigned char payload[160];
};

struct NackPacket {
    uint8_t type;         // 2
    uint32_t seq;         // big-endian
};
#pragma pack(pop)

struct FrameState {
    bool received = false;
    bool sent_to_player = false;
    unsigned char payload[160];
};

struct ParityState {
    bool received = false;
    unsigned char payload[160];
};

std::recursive_mutex db_mutex;
std::unordered_map<uint32_t, FrameState> frames_db;
std::unordered_map<uint32_t, ParityState> parities_db; // keyed by odd seq (2i+1)
uint32_t highest_seq_seen = 0;

// Clock-independent steady time synchronization
std::chrono::steady_clock::time_point local_t0;
bool has_local_t0 = false;
std::mutex time_mutex;

void update_local_t0(uint32_t seq) {
    std::lock_guard<std::mutex> lock(time_mutex);
    if (!has_local_t0) {
        local_t0 = std::chrono::steady_clock::now() - std::chrono::milliseconds(seq * 20 + 25);
        has_local_t0 = true;
    }
}

void send_to_player(int player_fd, struct sockaddr_in player_addr, uint32_t seq, const unsigned char* payload) {
    unsigned char send_buf[164];
    uint32_t be_seq = htonl(seq);
    std::memcpy(send_buf, &be_seq, 4);
    std::memcpy(send_buf + 4, payload, 160);
    sendto(player_fd, send_buf, sizeof(send_buf), 0, (struct sockaddr *)&player_addr, sizeof(player_addr));
}

void try_fec(int player_fd, struct sockaddr_in player_addr, uint32_t odd_seq) {
    uint32_t even_seq = odd_seq - 1;

    auto& f_even = frames_db[even_seq];
    auto& f_odd = frames_db[odd_seq];
    auto& parity = parities_db[odd_seq];

    if (!parity.received) return;

    if (f_even.received && !f_odd.received) {
        f_odd.received = true;
        for (int i = 0; i < 160; ++i) {
            f_odd.payload[i] = f_even.payload[i] ^ parity.payload[i];
        }
        if (!f_odd.sent_to_player) {
            f_odd.sent_to_player = true;
            send_to_player(player_fd, player_addr, odd_seq, f_odd.payload);
        }
    } else if (!f_even.received && f_odd.received) {
        f_even.received = true;
        for (int i = 0; i < 160; ++i) {
            f_even.payload[i] = f_odd.payload[i] ^ parity.payload[i];
        }
        if (!f_even.sent_to_player) {
            f_even.sent_to_player = true;
            send_to_player(player_fd, player_addr, even_seq, f_even.payload);
        }
    }
}

void handle_data_packet(int player_fd, struct sockaddr_in player_addr, uint32_t seq, const unsigned char* payload) {
    std::lock_guard<std::recursive_mutex> lock(db_mutex);
    update_local_t0(seq);

    if (seq > highest_seq_seen) {
        highest_seq_seen = seq;
    }
    auto& f = frames_db[seq];
    if (!f.received) {
        f.received = true;
        std::memcpy(f.payload, payload, 160);
        if (!f.sent_to_player) {
            f.sent_to_player = true;
            send_to_player(player_fd, player_addr, seq, f.payload);
        }
        if (seq % 2 == 0) {
            try_fec(player_fd, player_addr, seq + 1);
        } else {
            try_fec(player_fd, player_addr, seq);
        }
    }
}

void handle_parity_packet(int player_fd, struct sockaddr_in player_addr, uint32_t odd_seq, const unsigned char* payload) {
    std::lock_guard<std::recursive_mutex> lock(db_mutex);
    update_local_t0(odd_seq);

    if (odd_seq > highest_seq_seen) {
        highest_seq_seen = odd_seq;
    }
    auto& p = parities_db[odd_seq];
    if (!p.received) {
        p.received = true;
        std::memcpy(p.payload, payload, 160);
        try_fec(player_fd, player_addr, odd_seq);
    }
}

void nack_thread_func(int feedback_fd, struct sockaddr_in relay_feedback_addr, double delay_ms) {
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> last_nack_sent;
    double nack_interval_ms = std::max(40.0, delay_ms * 0.5);

    // Disable NACKs for very low delays where retransmissions are mathematically useless
    if (delay_ms < 45.0) {
        return;
    }

    double mult = 0.55;
    if (delay_ms >= 80.0) {
        if (delay_ms <= 85.0) {
            mult = 0.70; // Prevent NACK flooding under tight delay budget
        } else {
            mult = 0.65;
        }
    }

    double nack_delay_sec = (delay_ms / 1000.0) * mult;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        std::chrono::steady_clock::time_point t0_local;
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(time_mutex);
            if (has_local_t0) {
                t0_local = local_t0;
                active = true;
            }
        }
        if (!active) continue;

        auto now = std::chrono::steady_clock::now();
        double elapsed_sec = std::chrono::duration<double>(now - t0_local).count();
        int max_expected_seq = elapsed_sec / 0.020;

        if (max_expected_seq < 0) continue;

        int start_seq = std::max(0, max_expected_seq - 100);
        int end_seq = max_expected_seq;

        std::lock_guard<std::recursive_mutex> lock(db_mutex);
        for (int j = start_seq; j <= end_seq; ++j) {
            auto& f = frames_db[j];
            if (!f.received) {
                auto t_send = t0_local + std::chrono::milliseconds(j * 20);
                auto deadline = t_send + std::chrono::milliseconds((int)delay_ms);
                auto nack_trigger = t_send + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(nack_delay_sec)
                );

                if (now > nack_trigger && now < deadline) {
                    auto it = last_nack_sent.find(j);
                    bool send_now = false;
                    if (it == last_nack_sent.end()) {
                        send_now = true;
                    } else {
                        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
                        if (elapsed_ms >= nack_interval_ms) {
                            send_now = true;
                        }
                    }

                    if (send_now) {
                        NackPacket nack;
                        nack.type = 2;
                        nack.seq = htonl(j);
                        sendto(feedback_fd, &nack, sizeof(nack), 0,
                               (struct sockaddr *)&relay_feedback_addr, sizeof(relay_feedback_addr));
                        last_nack_sent[j] = now;
                    }
                }
            }
        }
    }
}

int main(void) {
    const char* delay_str = std::getenv("DELAY_MS");
    double delay_ms = delay_str ? std::strtod(delay_str, nullptr) : 60.0;

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (in_fd < 0) {
        perror("input socket");
        return 1;
    }

    int reuse = 1;
    setsockopt(in_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002");
        return 1;
    }

    int player_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (player_fd < 0) {
        perror("player socket");
        return 1;
    }

    struct sockaddr_in player = {0};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    int feedback_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (feedback_fd < 0) {
        perror("feedback socket");
        return 1;
    }

    struct sockaddr_in relay_feedback = {0};
    relay_feedback.sin_family = AF_INET;
    relay_feedback.sin_port = htons(47003);
    relay_feedback.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Launch NACK worker thread
    std::thread nack_thread(nack_thread_func, feedback_fd, relay_feedback, delay_ms);
    nack_thread.detach();

    unsigned char buf[2048];
    while (true) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < 5) continue; // At least type (1) + seq (4)

        RelayPacket* pkt = (RelayPacket*)buf;
        uint32_t host_seq = ntohl(pkt->seq);

        if (pkt->type == 0) {
            if (n < 165) continue;
            handle_data_packet(player_fd, player, host_seq, pkt->payload);
        } else if (pkt->type == 1) {
            if (n < 165) continue;
            handle_parity_packet(player_fd, player, host_seq, pkt->payload);
        }
    }

    close(in_fd);
    close(player_fd);
    close(feedback_fd);
    return 0;
}
