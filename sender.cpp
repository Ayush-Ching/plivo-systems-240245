#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

std::vector<std::vector<unsigned char>> frame_buffer;

void store_frame(uint32_t seq, const unsigned char* payload) {
    if (seq >= frame_buffer.size()) {
        frame_buffer.resize(seq + 100);
    }
    frame_buffer[seq] = std::vector<unsigned char>(payload, payload + 160);
}

bool get_frame(uint32_t seq, unsigned char* out_payload) {
    if (seq < frame_buffer.size() && !frame_buffer[seq].empty()) {
        std::memcpy(out_payload, frame_buffer[seq].data(), 160);
        return true;
    }
    return false;
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

    unsigned char buf[2048];
    unsigned char pkt[162];
    unsigned char dup_pkt[162];

    while (true) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < 164) continue;

        uint32_t be_seq;
        std::memcpy(&be_seq, buf, 4);
        uint32_t host_seq = ntohl(be_seq);

        const unsigned char* payload = buf + 4;
        store_frame(host_seq, payload);

        // Send current packet
        pkt[0] = (unsigned char)((host_seq >> 8) & 0xFF);
        pkt[1] = (unsigned char)(host_seq & 0xFF);
        std::memcpy(pkt + 2, payload, 160);
        sendto(out_fd, pkt, 162, 0, (struct sockaddr *)&relay, sizeof relay);

        // Send duplicate of previous packet 20ms later (when next frame arrives)
        if (host_seq > 0) {
            uint32_t prev_seq = host_seq - 1;
            if (prev_seq % 20 != 0) { // Skip 1 out of 20 to keep overhead at ~1.97x
                dup_pkt[0] = (unsigned char)((prev_seq >> 8) & 0xFF);
                dup_pkt[1] = (unsigned char)(prev_seq & 0xFF);
                if (get_frame(prev_seq, dup_pkt + 2)) {
                    sendto(out_fd, dup_pkt, 162, 0, (struct sockaddr *)&relay, sizeof relay);
                }
            }
        }
    }

    close(in_fd);
    close(out_fd);
    return 0;
}
