/*
 * This file is part of the Soletta Project
 *
 * Copyright (C) 2015 Intel Corporation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "sol-log.h"
#include "sol-mainloop.h"
#include "sol-network.h"
#include "sol-network-util.h"
#include "sol-util-internal.h"

#include "sol-socket.h"
#include "sol-socket-impl.h"

struct sol_socket_linux {
    struct sol_socket base;

    int fd;
    struct {
        bool (*cb)(void *data, struct sol_socket *s);
        const void *data;
        struct sol_fd *watch;
    } read, write;
};

static bool
on_socket_read(void *data, int fd, unsigned int flags)
{
    struct sol_socket_linux *s = data;
    bool ret;

    ret = s->read.cb((void *)s->read.data, &s->base);
    if (!ret) {
        s->read.cb = NULL;
        s->read.data = NULL;
        s->read.watch = NULL;
    }
    return ret;
}

static bool
on_socket_write(void *data, int fd, unsigned int flags)
{
    struct sol_socket_linux *s = data;
    bool ret;

    ret = s->write.cb((void *)s->write.data, &s->base);
    if (!ret) {
        s->write.cb = NULL;
        s->write.data = NULL;
        s->write.watch = NULL;
    }
    return ret;
}

static int
from_sockaddr(const struct sockaddr *sockaddr, socklen_t socklen,
    struct sol_network_link_addr *addr)
{
    SOL_NULL_CHECK(sockaddr, -EINVAL);
    SOL_NULL_CHECK(addr, -EINVAL);

    if (sockaddr->sa_family != AF_INET && sockaddr->sa_family != AF_INET6)
        return -EINVAL;

    addr->family = sol_network_af_to_sol(sockaddr->sa_family);

    if (sockaddr->sa_family == AF_INET) {
        struct sockaddr_in *sock4 = (struct sockaddr_in *)sockaddr;
        if (socklen < sizeof(struct sockaddr_in))
            return -EINVAL;

        addr->port = ntohs(sock4->sin_port);
        memcpy(&addr->addr.in, &sock4->sin_addr, sizeof(sock4->sin_addr));
    } else {
        struct sockaddr_in6 *sock6 = (struct sockaddr_in6 *)sockaddr;
        if (socklen < sizeof(struct sockaddr_in6))
            return -EINVAL;

        addr->port = ntohs(sock6->sin6_port);
        memcpy(&addr->addr.in6, &sock6->sin6_addr, sizeof(sock6->sin6_addr));
    }

    return 0;
}

static int
to_sockaddr(const struct sol_network_link_addr *addr, struct sockaddr *sockaddr, socklen_t *socklen)
{
    SOL_NULL_CHECK(addr, -EINVAL);
    SOL_NULL_CHECK(sockaddr, -EINVAL);
    SOL_NULL_CHECK(socklen, -EINVAL);

    if (addr->family == SOL_NETWORK_FAMILY_INET) {
        struct sockaddr_in *sock4 = (struct sockaddr_in *)sockaddr;
        if (*socklen < sizeof(struct sockaddr_in))
            return -EINVAL;

        memcpy(&sock4->sin_addr, addr->addr.in, sizeof(addr->addr.in));
        sock4->sin_port = htons(addr->port);
        sock4->sin_family = AF_INET;
        *socklen = sizeof(*sock4);
    } else if (addr->family == SOL_NETWORK_FAMILY_INET6) {
        struct sockaddr_in6 *sock6 = (struct sockaddr_in6 *)sockaddr;
        if (*socklen < sizeof(struct sockaddr_in6))
            return -EINVAL;

        memcpy(&sock6->sin6_addr, addr->addr.in6, sizeof(addr->addr.in6));
        sock6->sin6_port = htons(addr->port);
        sock6->sin6_family = AF_INET6;
        *socklen = sizeof(*sock6);
    } else {
        return -EINVAL;
    }

    return *socklen;
}

static struct sol_socket *
sol_socket_linux_new(int domain, enum sol_socket_type type, int protocol)
{
    struct sol_socket_linux *s;
    int fd, socktype = SOCK_CLOEXEC | SOCK_NONBLOCK;

    switch (type) {
    case SOL_SOCKET_UDP:
        socktype |= SOCK_DGRAM;
        break;
    default:
        SOL_WRN("Unsupported socket type: %d", type);
        errno = EPROTOTYPE;
        return NULL;
    }

    fd = socket(sol_network_sol_to_af(domain), socktype, protocol);
    SOL_INT_CHECK(fd, < 0, NULL);

    s = calloc(1, sizeof(*s));
    SOL_NULL_CHECK_GOTO(s, calloc_error);

    s->fd = fd;

    return &s->base;
calloc_error:
    close(fd);
    return NULL;
}

static void
sol_socket_linux_del(struct sol_socket *socket)
{
    struct sol_socket_linux *s = (struct sol_socket_linux *)socket;

    if (s->read.watch)
        sol_fd_del(s->read.watch);
    if (s->write.watch)
        sol_fd_del(s->write.watch);
    close(s->fd);
    free(s);
}

static int
sol_socket_linux_set_on_read(struct sol_socket *socket, bool (*cb)(void *data, struct sol_socket *s), const void *data)
{
    struct sol_socket_linux *s = (struct sol_socket_linux *)socket;

    if (cb && !s->read.watch) {
        s->read.watch = sol_fd_add(s->fd, SOL_FD_FLAGS_IN, on_socket_read, s);
        SOL_NULL_CHECK(s->read.watch, -ENOMEM);
    } else if (!cb && s->read.watch) {
        sol_fd_del(s->read.watch);
        s->read.watch = NULL;
    }

    s->read.cb = cb;
    s->read.data = data;

    return 0;
}

static int
sol_socket_linux_set_on_write(struct sol_socket *socket, bool (*cb)(void *data, struct sol_socket *s), const void *data)
{
    struct sol_socket_linux *s = (struct sol_socket_linux *)socket;

    if (cb && !s->write.watch) {
        s->write.watch = sol_fd_add(s->fd, SOL_FD_FLAGS_OUT, on_socket_write, s);
        SOL_NULL_CHECK(s->write.watch, -ENOMEM);
    } else if (!cb && s->write.watch) {
        sol_fd_del(s->write.watch);
        s->write.watch = NULL;
    }

    s->write.cb = cb;
    s->write.data = data;

    return 0;
}

static ssize_t
sol_socket_linux_recvmsg(struct sol_socket *socket, void *buf, size_t len, struct sol_network_link_addr *cliaddr)
{
    struct sol_socket_linux *s = (struct sol_socket_linux *)socket;
    uint8_t sockaddr[sizeof(struct sockaddr_in6)] = { };
    struct iovec iov = { .iov_base = buf,
                         .iov_len = len };
    struct msghdr msg = {
        .msg_name = &sockaddr,
        .msg_namelen = sizeof(sockaddr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    ssize_t r;

    /* When more types of sockets (not just datagram) are accepted,
     * remember to err if !buf && type != DGRAM */
    if (!buf) {
        msg.msg_iov->iov_len = 0;
        return recvmsg(s->fd, &msg, MSG_TRUNC | MSG_PEEK);
    } else
        r = recvmsg(s->fd, &msg, 0);

    if (r < 0)
        return -errno;

    if (from_sockaddr((struct sockaddr *)sockaddr, sizeof(sockaddr), cliaddr) < 0)
        return -EINVAL;

    return r;
}

static bool
sendmsg_multicast_addrs(int fd, struct sol_network_link *net_link,
    struct msghdr *msg)
{
    struct ip_mreqn ip4_mreq = { .imr_ifindex = net_link->index };
    struct ipv6_mreq ip6_mreq = { .ipv6mr_interface = net_link->index };
    struct ip_mreqn orig_ip4_mreq;
    struct ipv6_mreq orig_ip6_mreq;
    struct sol_network_link_addr *addr;
    uint16_t idx;
    bool success = false;

    SOL_VECTOR_FOREACH_IDX (&net_link->addrs, addr, idx) {
        void *p_orig, *p_new;
        int level, option;
        socklen_t l, l_orig;

        if (addr->family == SOL_NETWORK_FAMILY_INET) {
            level = IPPROTO_IP;
            option = IP_MULTICAST_IF;
            p_orig = &orig_ip4_mreq;
            p_new = &ip4_mreq;
            l = sizeof(orig_ip4_mreq);
        } else if (addr->family == SOL_NETWORK_FAMILY_INET6) {
            level = IPPROTO_IPV6;
            option = IPV6_MULTICAST_IF;
            p_orig = &orig_ip6_mreq;
            p_new = &ip6_mreq;
            l = sizeof(orig_ip6_mreq);
        } else {
            SOL_WRN("Unknown address family: %d", addr->family);
            continue;
        }

        l_orig = l;
        if (getsockopt(fd, level, option, p_orig, &l_orig) < 0) {
            SOL_DBG("Error while getting socket interface: %s",
                sol_util_strerrora(errno));
            continue;
        }

        if (setsockopt(fd, level, option, p_new, l) < 0) {
            SOL_DBG("Error while setting socket interface: %s",
                sol_util_strerrora(errno));
            continue;
        }

        if (sendmsg(fd, msg, 0) < 0) {
            SOL_DBG("Error while sending multicast message: %s",
                sol_util_strerrora(errno));
            continue;
        }

        if (setsockopt(fd, level, option, p_orig, l_orig) < 0) {
            SOL_DBG("Error while restoring socket interface: %s",
                sol_util_strerrora(errno));
            continue;
        }

        success = true;
    }

    return success;
}

static int
sendmsg_multicast(int fd, struct msghdr *msg)
{
    const unsigned int running_multicast = SOL_NETWORK_LINK_RUNNING | SOL_NETWORK_LINK_MULTICAST;
    const struct sol_vector *net_links = sol_network_get_available_links();
    struct sol_network_link *net_link;
    uint16_t idx;
    bool had_success = false;

    if (!net_links || !net_links->len)
        return -ENOTCONN;

    SOL_VECTOR_FOREACH_IDX (net_links, net_link, idx) {
        if ((net_link->flags & running_multicast) == running_multicast) {
            if (sendmsg_multicast_addrs(fd, net_link, msg))
                had_success = true;
        }
    }

    return had_success ? 0 : -EIO;
}

static bool
is_multicast(enum sol_network_family family, const struct sockaddr *sockaddr)
{
    if (family == SOL_NETWORK_FAMILY_INET6) {
        const struct sockaddr_in6 *addr6 = (const struct sockaddr_in6 *)sockaddr;

        return IN6_IS_ADDR_MULTICAST(&addr6->sin6_addr);
    }

    if (family == SOL_NETWORK_FAMILY_INET) {
        const struct sockaddr_in *addr4 = (const struct sockaddr_in *)sockaddr;

        return IN_MULTICAST(htonl(addr4->sin_addr.s_addr));
    }

    SOL_WRN("Unknown address family (%d)", family);
    return false;
}

static int
sol_socket_linux_sendmsg(struct sol_socket *socket, const void *buf, size_t len,
    const struct sol_network_link_addr *cliaddr)
{
    struct sol_socket_linux *s = (struct sol_socket_linux *)socket;
    uint8_t sockaddr[sizeof(struct sockaddr_in6)] = { };
    struct iovec iov = { .iov_base = (void *)buf,
                         .iov_len = len };
    struct msghdr msg = { .msg_iov = &iov,
                          .msg_iovlen = 1 };
    socklen_t l;

    l = sizeof(struct sockaddr_in6);
    if (to_sockaddr(cliaddr, (struct sockaddr *)sockaddr, &l) < 0)
        return -EINVAL;

    msg.msg_name = &sockaddr;
    msg.msg_namelen = l;

    if (is_multicast(cliaddr->family, (struct sockaddr *)sockaddr))
        return sendmsg_multicast(s->fd, &msg);

    if (sendmsg(s->fd, &msg, 0) < 0)
        return -errno;

    return 0;
}

static int
sol_socket_linux_join_group(struct sol_socket *socket, int ifindex, const struct sol_network_link_addr *group)
{
    struct sol_socket_linux *s = (struct sol_socket_linux *)socket;
    struct ip_mreqn ip_join = { };
    struct ipv6_mreq ip6_join = { };
    const void *p;
    int level, option;
    socklen_t l;

    if (group->family != SOL_NETWORK_FAMILY_INET && group->family != SOL_NETWORK_FAMILY_INET6)
        return -EINVAL;

    if (group->family == SOL_NETWORK_FAMILY_INET) {
        memcpy(&ip_join.imr_multiaddr, group->addr.in, sizeof(group->addr.in));
        ip_join.imr_ifindex = ifindex;

        p = &ip_join;
        l = sizeof(ip_join);
        level = IPPROTO_IP;
        option = IP_ADD_MEMBERSHIP;
    } else {
        memcpy(&ip6_join.ipv6mr_multiaddr, group->addr.in6, sizeof(group->addr.in6));
        ip6_join.ipv6mr_interface = ifindex;

        p = &ip6_join;
        l = sizeof(ip6_join);
        level = IPPROTO_IPV6;
        option = IPV6_JOIN_GROUP;
    }

    if (setsockopt(s->fd, level, option, p, l) < 0)
        return -errno;

    return 0;
}

static int
sol_socket_linux_bind(struct sol_socket *socket, const struct sol_network_link_addr *addr)
{
    struct sol_socket_linux *s = (struct sol_socket_linux *)socket;
    uint8_t sockaddr[sizeof(struct sockaddr_in6)] = { };
    socklen_t l;

    l = sizeof(struct sockaddr_in6);
    if (to_sockaddr(addr, (void *)sockaddr, &l) < 0)
        return -EINVAL;

    if (bind(s->fd, (struct sockaddr *)sockaddr, l) < 0) {
        return -errno;
    }

    return 0;
}

static int
sol_socket_option_to_linux(enum sol_socket_option option)
{
    switch (option) {
    case SOL_SOCKET_OPTION_REUSEADDR:
        return SO_REUSEADDR;
    case SOL_SOCKET_OPTION_REUSEPORT:
        return SO_REUSEPORT;
    default:
        SOL_WRN("Invalid option %d", option);
        break;
    }

    return -1;
}

static int
sol_socket_level_to_linux(enum sol_socket_level level)
{
    switch (level) {
    case SOL_SOCKET_LEVEL_SOCKET:
        return SOL_SOCKET;
    case SOL_SOCKET_LEVEL_IP:
        return IPPROTO_IP;
    case SOL_SOCKET_LEVEL_IPV6:
        return IPPROTO_IPV6;
    default:
        SOL_WRN("Invalid level %d", level);
        break;
    }

    return -1;
}

static int
sol_socket_linux_setsockopt(struct sol_socket *socket, enum sol_socket_level level,
    enum sol_socket_option optname, const void *optval, size_t optlen)
{
    int l, option;
    struct sol_socket_linux *s = (struct sol_socket_linux *)socket;

    l = sol_socket_level_to_linux(level);
    option = sol_socket_option_to_linux(optname);

    return setsockopt(s->fd, l, option, optval, optlen);
}

static int
sol_socket_linux_getsockopt(struct sol_socket *socket, enum sol_socket_level level,
    enum sol_socket_option optname, void *optval, size_t *optlen)
{
    int ret, l, option;
    socklen_t len = 0;
    struct sol_socket_linux *s = (struct sol_socket_linux *)socket;

    option = sol_socket_option_to_linux(optname);
    l = sol_socket_level_to_linux(level);

    ret = getsockopt(s->fd, l, option, optval, &len);
    SOL_INT_CHECK(ret, < 0, ret);

    *optlen = len;
    return 0;
}

const struct sol_socket_impl *
sol_socket_linux_get_impl(void)
{
    static const struct sol_socket_impl impl = {
        .bind = sol_socket_linux_bind,
        .join_group = sol_socket_linux_join_group,
        .sendmsg = sol_socket_linux_sendmsg,
        .recvmsg = sol_socket_linux_recvmsg,
        .set_on_write = sol_socket_linux_set_on_write,
        .set_on_read = sol_socket_linux_set_on_read,
        .del = sol_socket_linux_del,
        .new = sol_socket_linux_new,
        .setsockopt = sol_socket_linux_setsockopt,
        .getsockopt = sol_socket_linux_getsockopt
    };

    return &impl;
}
