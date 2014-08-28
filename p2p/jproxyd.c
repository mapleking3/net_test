#include "list.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>

#define CMD_HEARTBEAT   (1001)
#define CMD_TUNNEL_RESP (1002)

#define SERVER_LISTEN_PORT ("28099")

#define SAFE_METHOD_FREE(method, p) do { if (NULL != p) { method(p); p = NULL;}} while (0)

#define ASSERT_GOTO(cond, jump) {\
    if (!(cond)) {\
        printf("assert: %s failed[%m]!\n", #cond);\
        goto jump;\
    }\
}
#define ASSERT_RET(cond, ret) {\
    if (!(cond)) {\
        printf("assert: %s failed[%m]!\n", #cond);\
        return (ret);\
    }\
}

#define CLIENT_HT_SIZE 40
#define HASH(key)   (key%CLIENT_HT_SIZE)

struct client_node_t {
    struct hlist_node       node;
    unsigned int            UnitNo;
    unsigned char           UnitCode[16];
    struct sockaddr_in      client;
};

struct client_hlist_t {
    struct hlist_head       head;
    pthread_rwlock_t        rwlock;
};

static struct client_hlist_t client_ht[CLIENT_HT_SIZE] = {
    [0 ... CLIENT_HT_SIZE-1] = {
        .head   = HLIST_HEAD_INIT,
        .rwlock = PTHREAD_RWLOCK_INITIALIZER,
    },
};

struct socket_pair {
    int s1;
    int s2;
};

static struct client_node_t *update_client_list(int UnitNo, 
        const struct sockaddr *addr)
{
    char dst[INET_ADDRSTRLEN];

    struct client_node_t *t = NULL;
    struct client_node_t *find = NULL;

    struct hlist_head *hlisthead = &client_ht[HASH(UnitNo)].head;

    hlist_for_each_entry(t, hlisthead, node) {
        if (t->UnitNo == UnitNo) {
            find = t;
            break;
        }
    }

    if (NULL != find) {
        t = find;
        if (memcmp(&t->client, addr, sizeof(struct sockaddr_in)) != 0) {
            memcpy(&t->client, addr, sizeof(struct sockaddr_in));
        }
    } else {
        t = (struct client_node_t *)calloc(sizeof(struct client_node_t), 1);
        if (NULL == t) {
            return NULL;
        }
        t->client = *addr;
        t->UnitNo = UnitNo;
        INIT_HLIST_NODE(&t->node);
        hlist_add_head(&t->node, hlisthead);
    }

    return t;
}

static int create_listener(const char *addr, const char *port)
{
    int sd;
    struct addrinfo hints;
    struct addrinfo *result, *rp;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family     = AF_UNSPEC;
    hints.ai_socktype   = SOCK_STREAM;
    hints.ai_flags      = AI_PASSIVE;

    int ret = getaddrinfo(addr, port, &hints, &result);
    if (0 != ret)
    {
        printf("getaddrinfo error!\n");
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        sd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        int len = 1;
        setsockopt(sd, SOL_SOCKET, SO_REUSEADDR,
                (const void *)&len, sizeof(int));
        if (sd < 0)
        {
            continue;
        }

        if (0 == bind(sd, rp->ai_addr, rp->ai_addrlen))
        {
            break;
        }

        close(sd);
    }

    if (rp == NULL)
    {
        printf("could not bind!\n");
        freeaddrinfo(result);
        return -1;
    }
    freeaddrinfo(result);

#define SERVER_MAX_LISTEN  32
    if (listen(sd, SERVER_MAX_LISTEN) != 0)
    {
        printf("listen error[%m]\n");
        return -1;
    }

    return sd;
}

void *data_forward(void *arg) {
    int ret, s1, s2, len, epoll_fd;
    char buf[4096];
    struct socket_pair *sp;
    struct epoll_event ev;
    struct epoll_event events[2];

    pthread_detach(pthread_self());

    sp = (struct socket_pair *)arg;
    s1 = sp->s1;
    s2 = sp->s2;
    free(sp);
    epoll_fd = epoll_create(2);
    if (epoll_fd < 0) {
        fprintf(stderr, "epoll create error.\n");
        goto out;
    }
    ev.events = EPOLLIN;
    ev.data.fd = s1;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s1, &ev); 
    if (ret) {
        fprintf(stderr, "epoll ctrol add error.\n");
        goto out;
    }
    ev.events = EPOLLIN;
    ev.data.fd = s2;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s2, &ev); 
    if (ret) {
        fprintf(stderr, "epoll ctrol add error.\n");
        goto out;
    }

    for(;;) {
        ret = epoll_wait(epoll_fd, events, 1, -1);
        if (ret != 1) {
            fprintf(stderr, "epoll ctrol add error.\n");
            goto out;
        }
        if (events[0].data.fd == s1) {
            ret = read(s1, buf, sizeof(buf));
            if (ret <= 0) {
                break;
            }
            len = ret;
            ret = write(s2, buf, len);
            if (ret != len) {
                break;
            }
        } else if (events[0].data.fd == s2) {
            ret = read(s2, buf, sizeof(buf));
            if (ret <= 0) {
                break;
            }
            len = ret;
            ret = write(s1, buf, len);
            if (ret != len) {
                break;
            }
        } else {
            break;
        }
    }

out:
    if (epoll_fd > 0) {
        close(epoll_fd);
    }
    close(s1);
    close(s2);
    fprintf(stderr, "thread quit.\n");
    return NULL;
}

static void daemon_init() {
    int ret;
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "WARNING : fork() error, can not be daemon!\n");
        exit(1);
    } else if (pid != 0)
        exit(0);

    ret = setsid();
    if (ret == ((pid_t) -1)) {
        exit(1);
    }
    ret = chdir("/");
    if (ret) {
        exit(1);
    }
    ret = umask(0700);
#if 0
    int i;
    for (i = 0; i < 256; i++)
        close(i);
#endif
    return;
}

int main(int argc, const char **argv) 
{
    int ret, len, s1, s2, c, c1, c2, epoll_fd;
    struct sockaddr_in server;
    struct socket_pair *sp;
    pthread_t pt;
    struct epoll_event ev;
    struct epoll_event events[2];
    uint32_t cmd;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s listen_port proxy_port\n", argv[0]);
        exit(1);
    }

    //daemon_init();

    ASSERT_RET(0 < (s1 = create_listener(NULL, SERVER_LISTEN_PORT)), -1);

    epoll_fd = epoll_create1(0);
    ASSERT_RET(epoll_fd > 0, -1);

    ev.events = EPOLLIN;
    ev.data.fd = s1;
    if (0 != epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s1, &ev)) {
        fprintf(stderr, "epoll ctrol add error.\n");
        goto out;
    }
    for (;;) {
        ret = epoll_wait(epoll_fd, events, SERVER_MAX_LISTEN, -1);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            } else {
                close(epoll_fd);
                return -1;
            }
        }
        
        int i;
        for (i = 0; i < ret; ++i) {
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)
                    || (!events[i].events & EPOLLIN))
            {
                close(events[i].data.fd);
                continue;
            } else if (s1 == events[i].data.fd) {
                int connfd = -1;
                if (0 > (connfd = accept(s1, NULL, NULL))) {
                    printf("accpet failed[%m]!\n");
                    close(s1);
                    return -1;
                }
                memset(&ev, 0, sizeof(struct epoll_event));
                ev.data.fd = connfd;
                ev.events = EPOLLIN;
                if (0 != epoll_ctl(epoll_fd, EPOLL_CTL_ADD, connfd, &ev)) {
                    printf("epoll add connfd error[%m]!\n");
                    continue;
                }
            } else if (events[i].events & EPOLLIN) {
                ///< Client->Server HeartBeat: CMD_HEARTBEAT + UNITNO
                int value = 0;
                ret = read(events[i].data.fd, &value, sizeof(value));
                if (ret != sizeof(value)) {
                    printf("read error!\n");
                    return -1;
                }

                if (CMD_HEARTBEAT == value) {
                    int UnitNo = 0;
                    if (sizeof(UnitNo) != read(events[i].data.fd, &UnitNo,
                                sizeof(UnitNo))) {
                        printf("read error!\n");
                        return -1;
                    }
                    struct sockaddr addr;
                    socklen_t s_len;
                    ASSERT_RET(0 == getpeername(events[i].data.fd, &addr, &s_len), -1);
                    update_client_list(UnitNo, &addr);
                }
                else if (CMD_TUNNEL_RESP == value) {
                }
            }
            
        }
    }

loop:
    ret = epoll_wait(epoll_fd, events, 1, -1);
    if (ret != 1) {
        /* error */
        exit(1);
    }

    if (events[0].data.fd == c) {
        /* heartbreak */
        ret = read(c, &cmd, sizeof(cmd));
        if (ret != sizeof(cmd)) {
            exit(1);
        }
        cmd = ntohl(cmd);
        switch(cmd) {
        case 0:
            cmd = htonl(0);
            ret = write(c, &cmd, sizeof(cmd));
            if (ret != sizeof(cmd)) {
                exit(1);
            }
            break;
        default:
            exit(1);
            break;
        }
    } else if (events[0].data.fd != s2) {
        exit(1);
    }

    /* new connections */
    c2 = accept(s2, NULL, NULL);
    if (c2 < 0) {
        fprintf(stderr, "accept port[%s] error.\n", argv[2]);
        exit(1);
    }

    cmd = htonl(1);
    ret = write(c, &cmd, sizeof(cmd));
    if (ret != sizeof(cmd)) {
        exit(1);
    }

    c1 = accept(s1, NULL, NULL);
    if (c1 < 0) {
        fprintf(stderr, "accept port[%s] error.\n", argv[1]);
        exit(1);
    }


    sp = (struct socket_pair *)malloc(sizeof(struct socket_pair));
    if (sp == NULL) {
        exit(1);
    }
    sp->s1 = c1;
    sp->s2 = c2;

    ret = pthread_create(&pt, NULL, data_forward, sp);
    if (ret) {
        exit(1);
    }

    goto loop;

out:
    close(epoll_fd);
    return 0;
}
