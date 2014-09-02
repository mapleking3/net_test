#include "list.h"
#include <fcntl.h>
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
#include <mqueue.h>
#include <signal.h>

#include "interface.h"

#define SERVER_MAX_LISTEN 32

#define CMD_MQ_PATH         "/cmdMQ"
#define MAX_CMD_MSG_NUMS    16
#define MAX_CMD_MSG_LEN     4
#define DEFAULT_MQ_MODE     0600

#define CLIENT_HT_SIZE 40
#define HASH(key)   (key%CLIENT_HT_SIZE)

struct client_node_t {
    struct hlist_node       node;
    unsigned int            UnitNo;
    int                     ctl_fd;
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

struct msg_tunnel {
    int             cmd;
    unsigned int    UnitNo;
    unsigned short  tunnelPort; 
};

static mqd_t cmd_mq;

static struct client_node_t *update_client_list(int UnitNo, 
        const struct sockaddr *addr, int fd)
{
    printf("in update_client_list!\n");
    struct client_node_t *t = NULL;
    struct client_node_t *find = NULL;
    struct hlist_node *temp = NULL;

    struct hlist_head *hlisthead = &client_ht[HASH(UnitNo)].head;

    hlist_for_each_entry_safe(t, temp,  hlisthead, node) {
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
        memcpy(&t->client, addr, sizeof(t->client));
        t->UnitNo = UnitNo;
        t->ctl_fd = fd;
        INIT_HLIST_NODE(&t->node);
        hlist_add_head(&t->node, hlisthead);
    }

    return t;
}

static int create_listener(const char *addr, const char *port, int listen_cnt)
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

    if (listen(sd, listen_cnt) != 0)
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

int tunnel_thread(int fd)
{
    printf("----------start tunnel_thread!\n");
    int ret, len;
    char buf[4096];
    int c1 = accept(fd, NULL, NULL);
    ASSERT_RET(c1 > 0, -1);
    int c2 = accept(fd, NULL, NULL);
    ASSERT_RET(c2 > 0, -1);
    struct epoll_event ev;
    struct epoll_event events[2];

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        fprintf(stderr, "epoll create error.\n");
        goto out;
    }
    ev.events = EPOLLIN;
    ev.data.fd = c1;
    if (0 != epoll_ctl(epoll_fd, EPOLL_CTL_ADD, c1, &ev))
    {
        fprintf(stderr, "epoll ctrol add error.\n");
        goto out;
    }
    ev.events = EPOLLIN;
    ev.data.fd = c2;
    if (0 != epoll_ctl(epoll_fd, EPOLL_CTL_ADD, c2, &ev))
    {
        fprintf(stderr, "epoll ctrol add error.\n");
        goto out;
    }

    for(;;) {
        ret = epoll_wait(epoll_fd, events, 1, -1);
        if (ret != 1) {
            fprintf(stderr, "epoll ctrol add error.\n");
            goto out;
        }
        if (events[0].data.fd == c1) {
            ret = read(c1, buf, sizeof(buf));
            if (ret <= 0) {
                break;
            }
            len = ret;
            ret = write(c2, buf, len);
            if (ret != len) {
                break;
            }
        } else if (events[0].data.fd == c2) {
            ret = read(c2, buf, sizeof(buf));
            if (ret <= 0) {
                break;
            }
            len = ret;
            ret = write(c1, buf, len);
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
    close(c1);
    close(c2);
    close(fd);
    fprintf(stderr, "thread quit.\n");
    return 0;
}

void *recv_cmd_thread(void *arg)
{
    pthread_detach(pthread_self());
    int retval = 0;
    int fd;

    struct msg_tunnel cmd;

    for (;;)
    {
        memset(&cmd, 0, sizeof(struct msg_tunnel));
#if 0
        retval = mq_receive(cmd_mq, (char *)&cmd, sizeof(struct msg_tunnel),
                NULL);
        if (retval != sizeof(struct msg_tunnel)) {
            continue;
        }
#endif
        cmd.UnitNo = 4096;
        cmd.tunnelPort = 22;
        cmd.cmd = CMD_TUNNEL_E;

        struct client_node_t *find = NULL;
        struct client_node_t *t = NULL;
        struct hlist_node *temp = NULL;
        hlist_for_each_entry_safe(t,temp,&client_ht[HASH(cmd.UnitNo)].head, node) {
            if (t->UnitNo == cmd.UnitNo) {
                find = t;
                break;
            }
        }

        if (NULL == find) {
            sleep(1);
            continue;
        }

        if (cmd.cmd != CMD_TUNNEL_E) {
            continue;
        }

        fd = create_listener(NULL, "28100", 2);
        if (fd < 0) {
            printf("create listener failed!\n");
            exit(-1);
        }

        struct sockaddr_in addr;
        socklen_t slen;
        TUNNEL_T tunnel_t;
        if (0 != getsockname(fd, (struct sockaddr *)&addr, &slen)) {
            printf("get sockname error[%m]\n");
            return NULL;
        }

        printf("Create Tunnel Listen Port:%d\n", ntohs(addr.sin_port));

        if (sizeof(cmd.cmd) != write(find->ctl_fd, &cmd.cmd, sizeof(cmd.cmd))) {
            printf("write cmd error!\n");
            return NULL;
        }

        tunnel_t.tunnel_port = cmd.tunnelPort;
        tunnel_t.listen_port = ntohs(addr.sin_port);
        if (sizeof(TUNNEL_T) != write(find->ctl_fd, &tunnel_t, sizeof(TUNNEL_T))) {
            printf("write cmd error!\n");
            return NULL;
        }

        tunnel_thread(fd);
        break;
    }
    return NULL;
}

int main(void) 
{
    int ret, s1, epoll_fd;
    pthread_t pt;
    struct epoll_event ev;
    struct epoll_event events[20];
    struct mq_attr attr;

    attr.mq_maxmsg  = MAX_CMD_MSG_NUMS;
    attr.mq_msgsize= sizeof(struct msg_tunnel);
    mq_unlink(CMD_MQ_PATH);
    cmd_mq = mq_open(CMD_MQ_PATH, O_RDWR|O_CREAT|O_EXCL, DEFAULT_MQ_MODE, &attr); 
    ASSERT_RET(-1 != cmd_mq, -1);
    
    if (0 != pthread_create(&pt, NULL, recv_cmd_thread, NULL)) {
        printf("pthread_create error!\n");
        return -1;
    }

    ASSERT_RET(0 < (s1 = create_listener(NULL, SERVER_LISTEN_PORT,
                    SERVER_MAX_LISTEN)), -1);

    epoll_fd = epoll_create1(0);
    ASSERT_RET(epoll_fd > 0, -1);

    ev.events = EPOLLIN;
    ev.data.fd = s1;
    if (0 != epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s1, &ev)) {
        fprintf(stderr, "epoll ctrol add error.\n");
        return -1;
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
                int cmd = 0;
                int nread = read(events[i].data.fd, &cmd, sizeof(cmd));
                if (nread == 0)
                {
                    continue;
                }

                switch (cmd) {
                    case CMD_HEARTBEAT_E: 
                        { 
                            struct sockaddr addr;
                            socklen_t slen;
                            HEARTBEAT_T heartbeat_t; 
                            if (sizeof(HEARTBEAT_T) != read(events[i].data.fd,
                                        &heartbeat_t, sizeof(HEARTBEAT_T))) {
                                printf("2read error[%m]!\n");
                                continue;
                            }
                            ASSERT_RET(0 == getpeername(events[i].data.fd,
                                        &addr, &slen), -1);
                            update_client_list(heartbeat_t.unit_no, &addr,
                                    events[i].data.fd); 
                        } 
                        break;
                    case CMD_TUNNEL_RSP_E:
                        break; 
                    case CMD_TUNNEL_REQ_E:
                        break;
                    default:
                        break;
                }
            }
        }
    }
    close(epoll_fd);
    return 0;
}
