#pragma once

namespace socket_utils {
    // Send exactly one file descriptor over a unix stream socket.
    // Returns 0 on success, -1 on failure.
    int send_fd(int sockfd, int fd);

    // Receive exactly one file descriptor. The returned fd has O_CLOEXEC set.
    // Returns -1 on timeout, protocol error, or EOF.
    int recv_fd(int sockfd);
}
