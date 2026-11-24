#include "socket_utils.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr int kOneFd = 1;
}

namespace socket_utils {

int send_fd(int sockfd, int fd) {
    if (sockfd < 0 || fd < 0) {
        errno = EINVAL;
        return -1;
    }

    int count = kOneFd;
    struct iovec iov {};
    iov.iov_base = &count;
    iov.iov_len = sizeof(count);

    char ctrl[CMSG_SPACE(sizeof(int))];
    memset(ctrl, 0, sizeof(ctrl));

    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == nullptr) {
        errno = EINVAL;
        return -1;
    }
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

    ssize_t n;
    do {
        n = sendmsg(sockfd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    return n == static_cast<ssize_t>(sizeof(count)) ? 0 : -1;
}

int recv_fd(int sockfd) {
    if (sockfd < 0) {
        errno = EINVAL;
        return -1;
    }

    // Peek the 1-fd count so a malformed sender cannot make us allocate based
    // on an unchecked size. This protocol only ever sends exactly one fd.
    int count = 0;
    ssize_t n;
    do {
        n = recv(sockfd, &count, sizeof(count), MSG_PEEK);
    } while (n < 0 && errno == EINTR);

    if (n != static_cast<ssize_t>(sizeof(count))) {
        return -1;
    }
    if (count != kOneFd) {
        // Consume the unexpected count so the next request starts clean.
        char drain[sizeof(count)];
        (void) recv(sockfd, drain, sizeof(drain), MSG_WAITALL);
        errno = EPROTO;
        return -1;
    }

    char ctrl[CMSG_SPACE(sizeof(int))];
    memset(ctrl, 0, sizeof(ctrl));

    struct iovec iov {};
    iov.iov_base = &count;
    iov.iov_len = sizeof(count);

    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);

    do {
        n = recvmsg(sockfd, &msg, MSG_WAITALL | MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);

    if (n != static_cast<ssize_t>(sizeof(count))) {
        return -1;
    }

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == nullptr ||
        cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
        errno = EPROTO;
        return -1;
    }

    int fd = -1;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
}

}  // namespace socket_utils
