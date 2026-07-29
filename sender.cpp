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

struct __attribute__((packed)) RelayPacket {
    uint8_t type;         // 0 = data, 1 = parity
    uint32_t seq;         // big-endian
    unsigned char payload[160];
};

struct __attribute__((packed)) NackPacket {
    uint8_t type;         // 2
    uint32_t seq;         // big-endian
};

// Thread-safe frame buffer
std::mutex buffer_mutex;
std::vector<std::vector<unsigned char>> frame_buffer;

void store_frame(uint32_t seq, const unsigned char* payload) {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    if (seq >= frame_buffer.size()) {
        frame_buffer.resize(seq + 1000);
    }
    frame_buffer[seq] = std::vector<unsigned char>(payload, payload + 160);
}

bool get_frame(uint32_t seq, unsigned char* out_payload) {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    if (seq < frame_buffer.size() && !frame_buffer[seq].empty()) {
        std::memcpy(out_payload, frame_buffer[seq].data(), 160);
        return true;
    }
    return false;
}

void feedback_thread_func(int out_fd, struct sockaddr_in relay_addr) {
    int feedback_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (feedback_fd < 0) {
        perror("feedback socket");
        return;
    }

    int reuse = 1;
    setsockopt(feedback_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in feedback_addr = {0};
    feedback_addr.sin_family = AF_INET;
    feedback_addr.sin_port = htons(47004);
    feedback_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(feedback_fd, (struct sockaddr *)&feedback_addr, sizeof(feedback_addr)) < 0) {
        perror("bind 47004");
        close(feedback_fd);
        return;
    }

    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> last_retransmit;

    NackPacket nack;
    while (true) {
        ssize_t n = recvfrom(feedback_fd, &nack, sizeof(nack), 0, NULL, NULL);
        if (n < (ssize_t)sizeof(nack)) continue;
        if (nack.type != 2) continue;

        uint32_t seq = ntohl(nack.seq);

        auto now = std::chrono::steady_clock::now();
        auto it = last_retransmit.find(seq);
        if (it != last_retransmit.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (elapsed < 30) {
                continue; // Rate limit duplicate NACKs
            }
        }
        last_retransmit[seq] = now;

        RelayPacket resend_pkt;
        resend_pkt.type = 0;
        resend_pkt.seq = htonl(seq);
        if (get_frame(seq, resend_pkt.payload)) {
            sendto(out_fd, &resend_pkt, sizeof(resend_pkt), 0,
                   (struct sockaddr *)&relay_addr, sizeof(relay_addr));
        }
    }
    close(feedback_fd);
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (in_fd < 0) {
        perror("input socket");
        return 1;
    }

    int reuse = 1;
    setsockopt(in_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47010");
        return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (out_fd < 0) {
        perror("output socket");
        return 1;
    }

    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Launch feedback thread
    std::thread feedback_thread(feedback_thread_func, out_fd, relay);
    feedback_thread.detach();

    unsigned char buf[2048];
    while (true) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < 164) continue;

        uint32_t seq;
        std::memcpy(&seq, buf, 4);
        uint32_t host_seq = ntohl(seq);

        const unsigned char* payload = buf + 4;
        store_frame(host_seq, payload);

        // Forward to receiver via relay
        RelayPacket pkt;
        pkt.type = 0;
        pkt.seq = seq;
        std::memcpy(pkt.payload, payload, 160);

        sendto(out_fd, &pkt, sizeof pkt, 0, (struct sockaddr *)&relay, sizeof relay);

        // XOR FEC with group size K = 2
        if (host_seq % 2 == 1) {
            uint32_t prev_seq = host_seq - 1;
            unsigned char prev_payload[160];
            if (get_frame(prev_seq, prev_payload)) {
                RelayPacket parity_pkt;
                parity_pkt.type = 1;
                parity_pkt.seq = seq; // 2i + 1
                for (int i = 0; i < 160; ++i) {
                    parity_pkt.payload[i] = payload[i] ^ prev_payload[i];
                }
                sendto(out_fd, &parity_pkt, sizeof parity_pkt, 0, (struct sockaddr *)&relay, sizeof relay);
            }
        }
    }

    close(in_fd);
    close(out_fd);
    return 0;
}
