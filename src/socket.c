/* Part of FileKit, licensed under the GNU Affero GPL. */

#ifdef __linux__
#define _GNU_SOURCE
#else
#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600
#endif

#include "socket.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Set the given socket as nonblocking. */
static int socket_setnonblock(int socket)
{
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    flags |= SOCK_NONBLOCK;
    return fcntl(socket, F_SETFL, flags);
}

/**
 * Bind to a port of the given family.
 * Returns socket fd, or -1 on error.
 */
static int find_and_bind_socket(const char* port_string, int family, bool nonblock)
{
    int rc, s, opt;
    char addr_name[1024];
    struct addrinfo *info, *pinfo;
    struct addrinfo hint = {
        .ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_ADDRCONFIG,
        .ai_protocol = IPPROTO_TCP,
        .ai_socktype = SOCK_STREAM,
    };

    rc = getaddrinfo(NULL, port_string, &hint, &info);
    if (rc) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    for (pinfo = info; pinfo; pinfo = pinfo->ai_next) {
        rc = getnameinfo(pinfo->ai_addr, pinfo->ai_addrlen, addr_name, sizeof(addr_name), NULL, 0, NI_NUMERICHOST);
        if (rc) {
            fprintf(stderr, "getnameinfo: %s\n", gai_strerror(rc));
            freeaddrinfo(info);
            return -1;
        }

        if (pinfo->ai_family != family) {
            continue;
        }

        s = socket(family, pinfo->ai_socktype, pinfo->ai_protocol);
        if (s < 0) {
            perror("socket");
            freeaddrinfo(info);
            return -1;
        }

        opt = 1;
        rc = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (rc < 0) {
            perror("setsockopt");
            close(s);
            freeaddrinfo(info);
            return -1;
        }

        if (nonblock) {
            rc = socket_setnonblock(s);
            if (rc < 0) {
                perror("socket_setnonblock");
                close(s);
                freeaddrinfo(info);
                return -1;
            }
        }

        rc = bind(s, pinfo->ai_addr, pinfo->ai_addrlen);
        if (rc < 0) {
            perror("bind");
            close(s);
            freeaddrinfo(info);
            return -1;
        }

        rc = listen(s, SOMAXCONN);
        if (rc < 0) {
            perror("listen");
            close(s);
            freeaddrinfo(info);
            return -1;
        }

        freeaddrinfo(info);
        return s;
    }
    errno = EAFNOSUPPORT;
    freeaddrinfo(info);
    return -1;
}

int socket_prepare(const char* port_string, bool use_ipv6, bool nonblocking_accepts)
{
    return find_and_bind_socket(port_string, use_ipv6 ? AF_INET6 : AF_INET, nonblocking_accepts);
}

int socket_accept(int sockfd)
{
    struct sockaddr_storage peer;
    socklen_t peersize = sizeof(peer);
    int opt, rc, client;
    char peer_addr[1024], peer_port[10];

#ifdef __linux__
    client = accept4(sockfd, (struct sockaddr*)&peer, &peersize, SOCK_NONBLOCK);
    if (client == -1) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            perror("accept");
        }
        return -1;
    }
#else
    client = accept(sockfd, (struct sockaddr*)&peer, &peersize);
    if (client == -1) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            perror("accept");
        }
        return -1;
    }
    if (socket_setnonblock(sockfd)) {
        close(sockfd);
        perror("fcntl nonblock");
        return -1;
    }
#endif

    // disable Nagle's algorithm, see tcp(7)
    opt = 1;
    if (setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (void*)&opt, sizeof(opt))) {
        perror("setsockopt");
    }

    rc = getnameinfo((struct sockaddr*)&peer, peersize, peer_addr, sizeof peer_addr, peer_port, sizeof peer_port,
                     NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc) {
        fprintf(stderr, "Client accepted (getnameinfo: %s)\n", gai_strerror(rc));
    } else {
        printf("Client accepted from %s:%s\n", peer_addr, peer_port);
    }

    return client;
}

int socket_close(int sockfd)
{
    int save_errno;
    if (shutdown(sockfd, SHUT_RDWR)) {
        save_errno = errno;
        close(sockfd);  // shutdown failed, but we should still try to close if we can (but we don't care if it errors)
        errno = save_errno;
        return -1;
    }
    return close(sockfd);
}
