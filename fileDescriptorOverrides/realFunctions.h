#ifndef REALFUNCTIONS_H
#define REALFUNCTIONS_H

#include <sys/socket.h>
#include <poll.h>

namespace real {
    ssize_t write(int fd, const void *data, size_t size);

    ssize_t read(int fd, void *data, size_t size);

    ssize_t send(int fd, const void *buffer, size_t length, int flags);

    ssize_t recv(int fd, void *buffer, size_t length, int flags);

    ssize_t sendmsg(int fd, const struct msghdr *message, int flags);

    ssize_t recvmsg(int fd, struct msghdr *message, int flags);

    ssize_t sendto(int fd, const void *buffer, size_t length, int flags, const struct sockaddr *dest_addr,
                   socklen_t dest_len);

    ssize_t
    recvfrom(int fd, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_len);

    int accept(int fd, sockaddr *address, socklen_t *length);

    int connect(int fd, const sockaddr *address, socklen_t length);

    int close(int fd);

    int getsockopt(int fd, int level, int option_name, void *option_value, socklen_t *option_len);

    int setsockopt(int fd, int level, int option_name, const void *option_value, socklen_t option_len);

    int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);

    int fcntl(int fd, int command, ...);

    int poll(struct pollfd fds[], nfds_t nfds, int timeout);

    pid_t fork();
}

#endif //REALFUNCTIONS_H
