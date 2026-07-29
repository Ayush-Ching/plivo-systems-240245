#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

uint32_t highest_seq_seen = 0;
bool has_received_any = false;
bool sent_to_player[100000] = {false};

uint32_t reconstruct_seq(uint8_t low_byte) {
    if (!has_received_any) {
        highest_seq_seen = low_byte;
        has_received_any = true;
        return low_byte;
    }
    int diff = (int)low_byte - (int)(highest_seq_seen % 256);
    if (diff < -128) diff += 256;
    if (diff > 128) diff -= 256;
    
    int candidate = (int)highest_seq_seen + diff;
    if (candidate < 0) return 0;
    
    uint32_t u_candidate = (uint32_t)candidate;
    if (u_candidate > highest_seq_seen) {
        highest_seq_seen = u_candidate;
    }
    return u_candidate;
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (in_fd < 0) {
        perror("input socket");
        return 1;
    }

    int reuse = 1;
    setsockopt(in_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Increase socket receive buffer to 1MB
    int rcvbuf = 1024 * 1024;
    setsockopt(in_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

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

    unsigned char buf[2048];
    unsigned char send_buf[164];

    while (true) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < 161) continue;

        uint8_t low_byte = buf[0];
        uint32_t seq = reconstruct_seq(low_byte);

        if (seq < 100000 && !sent_to_player[seq]) {
            sent_to_player[seq] = true;

            uint32_t be_seq = htonl(seq);
            std::memcpy(send_buf, &be_seq, 4);
            std::memcpy(send_buf + 4, buf + 1, 160);

            sendto(player_fd, send_buf, 164, 0, (struct sockaddr *)&player, sizeof(player));
        }
    }

    close(in_fd);
    close(player_fd);
    return 0;
}
