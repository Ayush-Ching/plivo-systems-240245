#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

    while (true) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < 164) continue;

        // buf[0..3] is big-endian seq, buf[4..163] is payload
        uint32_t be_seq;
        std::memcpy(&be_seq, buf, 4);
        uint32_t host_seq = ntohl(be_seq);

        pkt[0] = (unsigned char)((host_seq >> 8) & 0xFF);
        pkt[1] = (unsigned char)(host_seq & 0xFF);
        std::memcpy(pkt + 2, buf + 4, 160);

        // Send first copy
        sendto(out_fd, pkt, 162, 0, (struct sockaddr *)&relay, sizeof relay);

        // Send second copy for 9 out of 10 packets (keeps overhead at ~1.92x)
        if (host_seq % 10 != 0) {
            sendto(out_fd, pkt, 162, 0, (struct sockaddr *)&relay, sizeof relay);
        }
    }

    close(in_fd);
    close(out_fd);
    return 0;
}
