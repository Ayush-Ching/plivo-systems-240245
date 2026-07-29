#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <map>
#include <set>
#include <chrono>
#include <string>

bool sent_to_player[100000] = {false};

struct DataInfo {
    bool present = false;
    unsigned char payload[160];
};

struct FecInfo {
    bool present = false;
    unsigned char payload[160];
};

std::map<uint32_t, DataInfo> data_buffer;
std::map<uint32_t, FecInfo> fec_buffer;
std::map<uint32_t, double> last_nack_time;

double get_now_s() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

void drain_socket(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    unsigned char junk[2048];
    while (recvfrom(fd, junk, sizeof(junk), 0, NULL, NULL) > 0) {}
    fcntl(fd, F_SETFL, flags);
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    setsockopt(in_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int rcvbuf = 1024 * 1024;
    setsockopt(in_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr);
    
    drain_socket(in_fd);
    set_nonblocking(in_fd);

    int player_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    int nack_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay_nack = {0};
    relay_nack.sin_family = AF_INET;
    relay_nack.sin_port = htons(47003); // Send NACKs to relay 47003
    relay_nack.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char buf[2048];
    unsigned char send_buf[164];
    
    double t0 = 0.0;
    const char* env_t0 = std::getenv("T0");
    if (env_t0) t0 = std::stod(env_t0);
    
    double srtt = 0.05;
    double rttvar = 0.02;

    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(in_fd, &readfds);
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000; // 1ms
        
        select(in_fd + 1, &readfds, NULL, NULL, &tv);
        
        double now_s = get_now_s();
        
        if (FD_ISSET(in_fd, &readfds)) {
            while (true) {
                ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
                if (n < 0) break;
                
                if (n >= 165) {
                    uint32_t seq = ntohl(*(uint32_t*)(buf + 1));
                    int type = buf[0];
                    
                    if (type == 0 || type == 2 || type == 4) {
                        if (!data_buffer[seq].present) {
                            data_buffer[seq].present = true;
                            memcpy(data_buffer[seq].payload, buf + 5, 160);
                            
                            if (seq < 100000 && !sent_to_player[seq]) {
                                sent_to_player[seq] = true;
                                uint32_t be_seq = htonl(seq);
                                memcpy(send_buf, &be_seq, 4);
                                memcpy(send_buf + 4, buf + 5, 160);
                                sendto(player_fd, send_buf, 164, 0, (struct sockaddr *)&player, sizeof(player));
                            }
                            
                            if (type == 0 || type == 2) {
                                double delay = now_s - (t0 + seq * 0.02);
                                if (delay > 0 && delay < 0.5) {
                                    double err = delay - srtt;
                                    srtt = srtt + 0.125 * err;
                                    double abs_err = err > 0 ? err : -err;
                                    rttvar = rttvar + 0.25 * (abs_err - rttvar);
                                }
                            }
                        }
                    }
                    
                    if (type == 2 && n >= 325) {
                        if (!fec_buffer[seq].present) {
                            fec_buffer[seq].present = true;
                            memcpy(fec_buffer[seq].payload, buf + 165, 160);
                        }
                    }
                }
            }
        }
        
        // FEC decoding
        bool changed = true;
        uint32_t max_expected = 0;
        if (t0 > 0) {
            max_expected = (now_s - t0) / 0.02;
            if (max_expected > 100000) max_expected = 0;
        }
        
        while (changed) {
            changed = false;
            uint32_t start_seq = (max_expected > 100) ? (max_expected - 100) : 0;
            for (uint32_t n_seq = start_seq; n_seq <= max_expected + 10; n_seq++) {
                if (fec_buffer.find(n_seq) == fec_buffer.end() || !fec_buffer[n_seq].present) continue;
                
                int missing_count = 0;
                uint32_t missing_seq = 0;
                for (uint32_t i = 1; i <= 3; i++) {
                    if (n_seq >= i) {
                        uint32_t target = n_seq - i;
                        if (!data_buffer[target].present) {
                            missing_count++;
                            missing_seq = target;
                        }
                    } else {
                        missing_count++; 
                    }
                }
                
                if (missing_count == 1 && n_seq >= 3 && (missing_seq == n_seq - 1 || missing_seq == n_seq - 2 || missing_seq == n_seq - 3)) {
                    unsigned char recovered[160];
                    memcpy(recovered, fec_buffer[n_seq].payload, 160);
                    for (uint32_t i = 1; i <= 3; i++) {
                        uint32_t target = n_seq - i;
                        if (target != missing_seq) {
                            for (int j = 0; j < 160; j++) {
                                recovered[j] ^= data_buffer[target].payload[j];
                            }
                        }
                    }
                    data_buffer[missing_seq].present = true;
                    memcpy(data_buffer[missing_seq].payload, recovered, 160);
                    
                    if (missing_seq < 100000 && !sent_to_player[missing_seq]) {
                        sent_to_player[missing_seq] = true;
                        uint32_t be_seq = htonl(missing_seq);
                        memcpy(send_buf, &be_seq, 4);
                        memcpy(send_buf + 4, recovered, 160);
                        sendto(player_fd, send_buf, 164, 0, (struct sockaddr *)&player, sizeof(player));
                    }
                    changed = true;
                }
            }
        }
        
        // Dynamic NACK
        if (t0 > 0) {
            double expected_delay = srtt + 2.0 * rttvar;
            if (expected_delay < 0.02) expected_delay = 0.02;
            if (expected_delay > 0.08) expected_delay = 0.08;
            
            uint32_t start_seq = (max_expected > 100) ? (max_expected - 100) : 0;
            for (uint32_t s = start_seq; s <= max_expected; s++) {
                if (!data_buffer[s].present) {
                    double expected_arrival = t0 + s * 0.02 + expected_delay;
                    if (now_s > expected_arrival) {
                        if (now_s - last_nack_time[s] > 0.04) {
                            unsigned char nack_pkt[5];
                            nack_pkt[0] = 3;
                            uint32_t net_s = htonl(s);
                            memcpy(nack_pkt + 1, &net_s, 4);
                            sendto(nack_fd, nack_pkt, 5, 0, (struct sockaddr*)&relay_nack, sizeof(relay_nack));
                            last_nack_time[s] = now_s;
                        }
                    }
                }
            }
        }
    }
    
    return 0;
}
