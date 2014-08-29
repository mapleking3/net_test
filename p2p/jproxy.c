#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <inttypes.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "interface.h"

#define SERVER_ADDR "192.168.111.222"

void *tunnel_thread(void *arg)
{
    printf("start tunnel ----------\n");
    pthread_detach(pthread_self());

    int len;
    char buf[4096];
    TUNNEL_T tunnel_t;
    memcpy(&tunnel_t, (TUNNEL_T *)arg, sizeof(TUNNEL_T));
    free(arg);

    struct sockaddr_in server;
    struct sockaddr_in localtunnel;

    int s1 = socket(AF_INET, SOCK_STREAM, 0);
    int s2 = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_RET(s1 > 0 && s2 > 0, NULL);

    memset(&server, 0, sizeof(struct sockaddr_in));
    server.sin_family   = AF_INET;
    server.sin_port     = tunnel_t.listen_port;
    server.sin_addr.s_addr = inet_addr(SERVER_ADDR);

    ASSERT_RET(0 == connect(s1, (struct sockaddr *)&server, sizeof(server)),
            NULL);

    memset(&localtunnel, 0, sizeof(struct sockaddr_in));
    localtunnel.sin_family  = AF_INET;
    localtunnel.sin_port    = tunnel_t.tunnel_port;
    localtunnel.sin_addr.s_addr = inet_addr("127.0.0.1");

    ASSERT_RET(0 == connect(s2, (struct sockaddr *)&localtunnel,
                sizeof(localtunnel)), NULL);

    int epoll_fd = epoll_create(2);
    if (epoll_fd < 0) {
#ifdef DEBUG
        fprintf(stderr, "epoll create error.\n");
#endif
        goto out;
    }

    struct epoll_event ev, events[2];
    ev.events = EPOLLIN;
    ev.data.fd = s1;
    int ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s1, &ev); 
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

int main(int argc, const char **argv) 
{
    int ret, s, epoll_fd;
    struct sockaddr_in server;
    struct epoll_event ev;
    struct epoll_event events[1];

    signal(SIGPIPE, SIG_IGN);

    /* control connections */
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        exit(1);
    }
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(atoi(SERVER_LISTEN_PORT));
    server.sin_addr.s_addr = inet_addr(SERVER_ADDR);

    ret = connect(s, (struct sockaddr *)&server, sizeof(server));
    if (ret) {
        fprintf(stderr, "connet [%s][%s] error.\n", argv[1], argv[2]);
        exit(1);
    }
    printf("connet successfully\n");

    epoll_fd = epoll_create(1);
    if (epoll_fd < 0) {
        fprintf(stderr, "epoll create error.\n");
        exit(1);
    }
    ev.events = EPOLLIN;
    ev.data.fd = s;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s, &ev); 
    if (ret) {
        fprintf(stderr, "epoll ctrol add error.\n");
        exit(1);
    }

    for (;;) {
        ret = epoll_wait(epoll_fd, events, 1, 10*1000);
        if (ret < 0) {
            printf("epoll wait error[%m]\n");
            if (errno == EINTR) {
                continue;
            } else {
                close(epoll_fd);
                return -1;
            }
        }

        if (ret == 0)
        {
            ///< HEARTBEART
            int cmd = CMD_HEARTBEAT_E;
            HEARTBEAT_T heartbeat_t;
            heartbeat_t.unit_no = 4096;
            if (sizeof(cmd) != write(s, &cmd, sizeof(cmd))) {
                printf("write error[%m]\n");
                return -1;
            }
            if (sizeof(HEARTBEAT_T) != write(s, &heartbeat_t,
                        sizeof(HEARTBEAT_T))) {
                printf("write error[%m]\n");
                return -1;
            }
            continue;
        }
        
        if ((events[0].events & EPOLLERR) || (events[0].events & EPOLLHUP)
                || (!events[0].events & EPOLLIN))
        {
            close(events[0].data.fd);
            continue;
        } else if (s == events[0].data.fd) {
            int cmd = 0;
            if (sizeof(cmd) != read(s, &cmd, sizeof(cmd))) {
                printf("1read error[%m]!\n");
                return -1;
            }

            switch (cmd) {
                case CMD_TUNNEL_E:
                    {
                        TUNNEL_T *ptunnel = (TUNNEL_T *)malloc(sizeof(TUNNEL_T));
                        if (sizeof(TUNNEL_T) != read(s, ptunnel,
                                    sizeof(TUNNEL_T))) {
                            printf("2read error![%m]\n");
                            return -1;
                        }
                        printf("TUNNEL CMD: listen port:%d tunnuel port:%d\n",
                            ptunnel->listen_port, ptunnel->tunnel_port);
                        pthread_t pt;
                        if (0 != pthread_create(&pt, NULL, tunnel_thread,
                                    (void *)ptunnel)) {
                            printf("pthread_create error[%m]!\n");
                            return -1;
                        }
                    }
                    break;
                default:
                    printf("Unknow cmd!\n");
                    break;
            }
        }
    }
    return 0;
}
