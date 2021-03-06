#include <iostream>
#include <map>
#include <arpa/inet.h>
#include <cstdarg>
#include <fcntl.h>
#include <set>

#include "rdma_tests/RDMAMessageBuffer.h"
#include "realFunctions.h"
#include "overrides.h"

namespace {
// unordered_map does not like to be 0 initialized, so we can't use it here
    std::map<int, std::unique_ptr<RDMAMessageBuffer>> bridge;
    std::set<int> rdmableSockets;
    bool dontCloseRDMA = true; // as long as we can't get rid of the RDMA deallocation errors, don't ever close RDMA connections
    size_t forkGeneration = 0;

    const size_t BUFFER_SIZE = 128 * 1024;

    auto getRdmaEnv() {
        static const auto rdmaReachable = getenv("USE_RDMA");
        return rdmaReachable;
    }

    auto getForkGenIntercept() {
        static const auto forkGenChars = getenv("RDMA_FORKGEN");
        static const auto forkGen = forkGenChars ? std::stoul(std::string(forkGenChars)) : 0;
        return forkGen;
    }

    bool isTcpSocket(int socket, bool isServer) {
        int socketType;
        {
            socklen_t option;
            socklen_t option_length = sizeof(option);
            if (real::getsockopt(socket, SOL_SOCKET, SO_TYPE, &option, &option_length) < 0) {
                return false;
            }
            socketType = option;
        }

        int addressLocation;
        {
            struct sockaddr_storage options;
            socklen_t size = sizeof(options);
            if (isServer) {
                if (getsockname(socket, reinterpret_cast<struct sockaddr *>(&options), &size) < 0) {
                    return false;
                }
            } else if (getpeername(socket, reinterpret_cast<struct sockaddr *>(&options), &size) < 0) {
                return false;
            }
            addressLocation = options.ss_family;
        }

        return socketType == SOCK_STREAM && addressLocation == AF_INET;
    }

    sockaddr_in getRDMAReachable() {
        sockaddr_in possibleAddr{};
        if (getRdmaEnv() == nullptr) {
            std::cerr << "USE_RDMA not set, disabling RDMA socket interception" << std::endl;
            return possibleAddr;
        }

        inet_pton(AF_INET, getRdmaEnv(), &possibleAddr.sin_addr);
        return possibleAddr;
    }

    bool shouldServerIntercept(int serverSocket, int clientSocket) {
        if (not isTcpSocket(serverSocket, true)) {
            return false;
        }

        const auto possibleAddr = getRDMAReachable();

        sockaddr_in connectedAddr;
        {
            socklen_t size = sizeof(connectedAddr);
            getpeername(clientSocket, reinterpret_cast<struct sockaddr *>(&connectedAddr), &size);
        }

        return connectedAddr.sin_addr.s_addr == possibleAddr.sin_addr.s_addr;
    }

    bool shouldClientIntercept(int socket) {
        if (not isTcpSocket(socket, false)) {
            return false;
        }

        const auto possibleAddr = getRDMAReachable();

        sockaddr_in connectedAddr;
        {
            socklen_t size = sizeof(connectedAddr);
            getpeername(socket, reinterpret_cast<struct sockaddr *>(&connectedAddr), &size);
        }

        return connectedAddr.sin_addr.s_addr == possibleAddr.sin_addr.s_addr;
    }
}

int accept(int server_socket, sockaddr *address, socklen_t *length) {
    const int client_socket = real::accept(server_socket, address, length);
    if (client_socket < 0) {
        return ERROR;
    }

    if (not shouldServerIntercept(server_socket, client_socket)) {
        return SUCCESS;
    }

    rdmableSockets.insert(client_socket);
    return client_socket;
}

int connect(int fd, const sockaddr *address, socklen_t length) {
    if (real::connect(fd, address, length) == ERROR) {
        if (errno != EINPROGRESS) {
            return ERROR;
        }
        // In case of a non blocking socket, we just poll until it is ready...
        pollfd pfd{fd, POLLOUT, 0};
        real::poll(&pfd, 1, -1);
        if ((pfd.revents & POLLERR) != 0) {
            return ERROR;
        }
    }

    if (not shouldClientIntercept(fd)) {
        return SUCCESS;
    }

    rdmableSockets.insert(fd);
    return SUCCESS;
}

ssize_t write(int fd, const void *source, size_t requested_bytes) {
    if (bridge.find(fd) != bridge.end()) {
        bridge[fd]->send(reinterpret_cast<const uint8_t *>(source), requested_bytes);
        return requested_bytes;
    }
    if (forkGeneration == getForkGenIntercept() &&
        // When dealing with the accept then fork pattern, delay the actual RDMA connection to the child process
        rdmableSockets.find(fd) != rdmableSockets.end()) {
        rdmableSockets.erase(rdmableSockets.find(fd));
        bridge[fd] = std::make_unique<RDMAMessageBuffer>(BUFFER_SIZE, fd);
        return write(fd, source, requested_bytes);
    }
    return real::write(fd, source, requested_bytes);
}

ssize_t read(int fd, void *destination, size_t requested_bytes) {
    if (bridge.find(fd) != bridge.end()) {
        return bridge[fd]->receive(destination, requested_bytes);
    }

    if (forkGeneration == getForkGenIntercept() &&
        // When dealing with the accept then fork pattern, delay the actual RDMA connection to the child process
        rdmableSockets.find(fd) != rdmableSockets.end()) {
        rdmableSockets.erase(rdmableSockets.find(fd));
        bridge[fd] = std::make_unique<RDMAMessageBuffer>(BUFFER_SIZE, fd);
        return read(fd, destination, requested_bytes);
    }
    return real::read(fd, destination, requested_bytes);
}

int close(int fd) {
    if (not dontCloseRDMA) {
        bridge.erase(fd);
    }

    return real::close(fd);
}

ssize_t send(int fd, const void *buffer, size_t length, int flags) {
// For now: We forward the call to write for a certain set of
// flags, which we chose to ignore. By putting them here explicitly,
// we make sure that we only ignore flags, which are not important.
// For production, we might wanna handle these flags
#ifdef __APPLE__
    if (flags == 0) {
#else
    if (flags == 0 || flags == MSG_NOSIGNAL) {
#endif
        return write(fd, buffer, length);
    } else {
        std::cerr << "Routing send to socket (unsupported flags)" << std::endl;
        return real::send(fd, buffer, length, flags);
    }
}

ssize_t recv(int fd, void *buffer, size_t length, int flags) {
#ifdef __APPLE__
    if (flags == 0) {
#else
    if (flags == 0 || flags == MSG_NOSIGNAL) {
#endif
        return read(fd, buffer, length);
    } else {
        std::cerr << "Routing recv to socket (unsupported flags)" << std::endl;
        return real::recv(fd, buffer, length, flags);
    }
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
    // This one is hard to implement because the `msghdr` struct contains
    // an iovec pointer, which points to an array of iovec structs. Each such
    // struct is then a vector with a starting address and length. The sendmsg
    // call then fills these vectors one by one until the stream is empty or
    // all the vectors have been filled. I don't know how many people use this
    // function, but right now we just support a single buffer and else route
    // the call to the socket itself.
    if (msg->msg_iovlen == 1) {
        return sendto(fd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags,
                      reinterpret_cast<struct sockaddr *>(msg->msg_name),
                      msg->msg_namelen);
    } else {
        std::cerr << "Routing sendmsg to socket (too many buffers)" << std::endl;
        return real::sendmsg(fd, msg, flags);
    }
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
    if (msg->msg_iovlen == 1) {
        return recvfrom(fd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags,
                        reinterpret_cast<struct sockaddr *>(msg->msg_name),
                        &msg->msg_namelen);
    } else {
        std::cerr << "Routing recvmsg to socket (too many buffers)" << std::endl;
        return real::recvmsg(fd, msg, flags);
    }
}

ssize_t
sendto(int fd, const void *buffer, size_t length, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
    // When the destination address is null, then this should be a stream socket
    if (dest_addr == NULL) {
        return send(fd, buffer, length, flags);
    } else {
        // Connection-less sockets (UDP) sockets never use RDMA anyway
        return real::sendto(fd, buffer, length, flags, dest_addr, addrlen);
    }
}

ssize_t recvfrom(int fd, void *buffer, size_t length, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    // When the destination address is null, then this should be a stream socket
    if (src_addr == NULL) {
        return recv(fd, buffer, length, flags);
    } else {
        // Connection-Less sockets (UDP) sockets never use RDMA anyway
        return real::recvfrom(fd, buffer, length, flags, src_addr, addrlen);
    }
}

pid_t fork(void) {
    dontCloseRDMA = true;
    auto res = real::fork();
    if (res == 0) {
        ++forkGeneration;
    }
    return res;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    const auto start = std::chrono::steady_clock::now();
    if (nfds == 0) return 0;

    int event_count = 0;
    std::vector<size_t> rdma_fds, normal_fds;
    for (nfds_t index = 0; index < nfds; ++index) {
        if (bridge.find(fds[index].fd) != bridge.end()) {
            rdma_fds.push_back(index);
        } else {
            normal_fds.push_back(index);
        }
    }

    if (rdma_fds.size() == 0) {
        event_count = real::poll(fds, nfds, timeout);
    } else if (normal_fds.size() == 0) {
        do {
            // Do a full loop over all FDs
            for (auto &i : rdma_fds) {
                auto &msgBuf = bridge[fds[i].fd];
                if (msgBuf->hasData()) {
                    auto inFlag = fds[i].events & POLLIN;
                    if (inFlag != 0) ++event_count;
                    fds[i].revents |= inFlag;
                }
                auto outFlag = fds[i].events & POLLOUT;
                if (outFlag != 0) ++event_count;
                fds[i].revents |= outFlag;
            }
            if (event_count > 0) break;
        } while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >
                 timeout);
    } else {
        std::cerr << "can't do mixed RDMA / TCP yet" << std::endl;
        return ERROR;
    }

    // This is necessary for repeated calls with the same poll structures
    // (the kernel probably does this internally first too)
    for (size_t i = 0; i < nfds; ++i) {
        fds[i].revents = 0;
    }

    return event_count;
}

static int fcntl_set(int fd, int command, int flags) {
    if (bridge.find(fd) != bridge.end()) {
        // TODO: actually do something to set our implementation nonblocking here
        return SUCCESS;
    }

    return real::fcntl_set_flags(fd, command, flags);
}

static int fcntl_get(int fd, int command) {
    int flags = real::fcntl_get_flags(fd, command);

    if (bridge.find(fd) != bridge.end()) {
        // First unset the flag, then check if we have it set
        flags &= ~O_NONBLOCK; // TODO: if we fcntl_set set this, we also need to query this
    }

    return flags;
}


int fcntl(int fd, int command, ...) {
    if (bridge.find(fd) != bridge.end()) {
        std::cerr << "RDMA fcntl isn't supported!" << std::endl;
        // we can probably support O_NONBLOCK, but just ignore it for now
        return SUCCESS;
    }

    va_list argument;

    // Takes the argument pointer and the last positional argument
    // Makes the argument pointer point to the first optional argument
    va_start(argument, command);

    if (command == F_SETFL || command == F_SETFD) {
        return fcntl_set(fd, command, va_arg(argument, int));
    } else if (command == F_GETFL || command == F_GETFD) {
        return fcntl_get(fd, command);
    } else {
        // Sorry, don't know what to do for other commands :(
        // If necessary: handle all cases of arguments ...
        return ERROR;
    }
}

int getsockopt(int fd, int level, int option_name, void *option_value, socklen_t *option_len) __THROW {
    return real::getsockopt(fd, level, option_name, option_value, option_len);
}

int setsockopt(int fd, int level, int option_name, const void *option_value, socklen_t option_len) __THROW {
    if (bridge.find(fd) != bridge.end()) {
        std::cerr << "RDMA setsockopt isn't supported!" << std::endl;
        // we can probably support O_NONBLOCK
        return SUCCESS;
    }
    return real::setsockopt(fd, level, option_name, option_value, option_len);
}

// Snip.
// Select forwarding to poll here. Skip all the way to the bottom

typedef struct DescriptorSets {
    fd_set *readfds;
    fd_set *writefds;
    fd_set *errorfds;
} DescriptorSets;

static bool fd_is_set(int fd, const fd_set *set) {
    return set != NULL && FD_ISSET(fd, set);
}

static bool is_in_any_set(int fd, const DescriptorSets *sets) {
    return fd_is_set(fd, sets->readfds) ||
           fd_is_set(fd, sets->writefds) ||
           fd_is_set(fd, sets->errorfds);
}

static void count_rdma_sockets(size_t highest_fd, const DescriptorSets *sets, size_t *rdma_count) {
    *rdma_count = 0;
    for (size_t fd = 0; fd < highest_fd; ++fd) {
        if (is_in_any_set(fd, sets)) {
            if (bridge.find(fd) != bridge.end()) {
                ++(*rdma_count);
            }
        }
    }
}

static int timeval_to_milliseconds(const struct timeval *time) {
    auto milliseconds = time->tv_sec * 1000;
    milliseconds += time->tv_usec / 1000;

    return static_cast<int>(milliseconds);
}

static std::unique_ptr<struct pollfd[]>
select_to_poll(int *nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds) {
    auto fds = std::make_unique<struct pollfd[]>(*nfds);

    int i = 0;
    for (int fd = 0; fd < *nfds; fd++) {
        if (readfds && FD_ISSET(fd, readfds)) {
            fds[i].fd = fd;
            fds[i].events = POLLIN;
        }

        if (writefds && FD_ISSET(fd, writefds)) {
            fds[i].fd = fd;
            fds[i].events |= POLLOUT;
        }

        if (exceptfds && FD_ISSET(fd, exceptfds))
            fds[i].fd = fd;

        if (fds[i].fd)
            i++;
    }

    *nfds = i;
    return fds;
}

static int poll_to_select(int nfds, struct pollfd *fds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds) {
    int cnt = 0;
    for (int i = 0; i < nfds; i++) {
        if (readfds && (fds[i].revents & (POLLIN | POLLHUP))) {
            FD_SET(fds[i].fd, readfds);
            cnt++;
        }

        if (writefds && (fds[i].revents & POLLOUT)) {
            FD_SET(fds[i].fd, writefds);
            cnt++;
        }

        if (exceptfds && (fds[i].revents & ~(POLLIN | POLLOUT))) {
            FD_SET(fds[i].fd, exceptfds);
            cnt++;
        }
    }
    return cnt;
}

static int forward_to_poll(int nfds, DescriptorSets *sets, struct timeval *timeout) {
    auto pollfds = select_to_poll(&nfds, sets->readfds, sets->writefds, sets->errorfds);

    auto milliseconds = timeout ? timeval_to_milliseconds(timeout) : -1;

    // The actual forwarding call
    auto number_of_events = poll(pollfds.get(), nfds, milliseconds);

    if (sets->readfds)
        FD_ZERO(sets->readfds);
    if (sets->writefds)
        FD_ZERO(sets->writefds);
    if (sets->errorfds)
        FD_ZERO(sets->errorfds);

    if (number_of_events > 0) {
        number_of_events = poll_to_select(nfds, pollfds.get(), sets->readfds, sets->writefds, sets->errorfds);
    }
    return number_of_events;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *errorfds, struct timeval *timeout) {
    DescriptorSets sets = {readfds, writefds, errorfds};
    size_t rdma_count;
    count_rdma_sockets(nfds, &sets, &rdma_count);

    if (rdma_count == 0) {
        return real::select(nfds, readfds, writefds, errorfds, timeout);
    }

    return forward_to_poll(nfds, &sets, timeout);
}
