#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

bool sent_to_player[100000] = {false};

void drain_socket(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    unsigned char junk[2048];
    while (recvfrom(fd, junk, sizeof(junk), 0, NULL, NULL) > 0) {
        // discard leftover packets from previous runs
    }
    fcntl(fd, F_SETFL, flags);
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

    // Drain any leftover UDP packets from previous runs
    drain_socket(in_fd);

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
        if (n < 162) continue;

        uint32_t seq = (buf[0] << 8) | buf[1];

        if (seq < 100000 && !sent_to_player[seq]) {
            sent_to_player[seq] = true;

            uint32_t be_seq = htonl(seq);
            std::memcpy(send_buf, &be_seq, 4);
            std::memcpy(send_buf + 4, buf + 2, 160);

            sendto(player_fd, send_buf, 164, 0, (struct sockaddr *)&player, sizeof(player));
        }
    }

    close(in_fd);
    close(player_fd);
    return 0;
}
