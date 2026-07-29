#include <iostream>
#include <vector>
#include <map>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <algorithm>

std::map<uint32_t, std::vector<unsigned char>> frame_buffer;

void store_frame(uint32_t seq, const unsigned char* payload) {
    frame_buffer[seq] = std::vector<unsigned char>(payload, payload + 160);
}

bool has_frame(uint32_t seq) {
    return frame_buffer.find(seq) != frame_buffer.end();
}

const unsigned char* get_frame(uint32_t seq) {
    return frame_buffer[seq].data();
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    setsockopt(in_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) return 1;
    
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    int nack_fd = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(nack_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in nack_addr = {0};
    nack_addr.sin_family = AF_INET;
    nack_addr.sin_port = htons(47004);
    nack_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(nack_fd, (struct sockaddr *)&nack_addr, sizeof(nack_addr)) < 0) return 1;
    
    set_nonblocking(in_fd);
    set_nonblocking(nack_fd);
    
    unsigned char junk[2048];
    while (recvfrom(nack_fd, junk, sizeof(junk), 0, NULL, NULL) > 0);

    unsigned char buf[2048];
    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(in_fd, &readfds);
        FD_SET(nack_fd, &readfds);
        int max_fd = std::max(in_fd, nack_fd);
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000;
        
        select(max_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (FD_ISSET(in_fd, &readfds)) {
            while (true) {
                ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
                if (n < 0) break;
                if (n >= 164) {
                    uint32_t host_seq = ntohl(*(uint32_t*)buf);
                    const unsigned char* payload = buf + 4;
                    store_frame(host_seq, payload);

                    if (host_seq >= 3 && host_seq % 10 != 0) { // 90% of time
                        unsigned char pkt[325];
                        pkt[0] = 2; // Data + FEC
                        uint32_t net_seq = htonl(host_seq);
                        memcpy(pkt + 1, &net_seq, 4);
                        memcpy(pkt + 5, payload, 160);
                        
                        if (has_frame(host_seq - 1) && has_frame(host_seq - 3)) {
                            const unsigned char* p1 = get_frame(host_seq - 1);
                            const unsigned char* p3 = get_frame(host_seq - 3);
                            for (int i = 0; i < 160; i++) {
                                pkt[165 + i] = p1[i] ^ p3[i];
                            }
                            sendto(out_fd, pkt, 325, 0, (struct sockaddr *)&relay, sizeof relay);
                        } else {
                            sendto(out_fd, pkt, 165, 0, (struct sockaddr *)&relay, sizeof relay);
                        }
                    } else {
                        unsigned char pkt[165];
                        pkt[0] = 0; // Data only
                        uint32_t net_seq = htonl(host_seq);
                        memcpy(pkt + 1, &net_seq, 4);
                        memcpy(pkt + 5, payload, 160);
                        sendto(out_fd, pkt, 165, 0, (struct sockaddr *)&relay, sizeof relay);
                    }
                }
            }
        }
        
        if (FD_ISSET(nack_fd, &readfds)) {
            while (true) {
                ssize_t nack_n = recvfrom(nack_fd, buf, sizeof buf, 0, NULL, NULL);
                if (nack_n < 0) break;
                if (nack_n == 5 && buf[0] == 3) {
                    uint32_t nack_seq = ntohl(*(uint32_t*)(buf + 1));
                    if (has_frame(nack_seq)) {
                        unsigned char ret_pkt[165];
                        ret_pkt[0] = 4; // Retransmission
                        uint32_t net_seq = htonl(nack_seq);
                        memcpy(ret_pkt + 1, &net_seq, 4);
                        memcpy(ret_pkt + 5, get_frame(nack_seq), 160);
                        sendto(out_fd, ret_pkt, 165, 0, (struct sockaddr *)&relay, sizeof relay);
                    }
                }
            }
        }
    }
    return 0;
}
