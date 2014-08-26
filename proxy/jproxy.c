/***********************************************************
  jproxy.c
  zxsoft @ 2012
  author : Chen Huitao
***********************************************************/
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

struct socket_pair {
    int s1;
    int s2;
};

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
#ifdef DEBUG
        fprintf(stderr, "epoll create error.\n");
#endif
        goto out;
    }
    ev.events = EPOLLIN;
    ev.data.fd = s1;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s1, &ev); 
    if (ret) {
#ifdef DEBUG
        fprintf(stderr, "epoll ctrol add error.\n");
#endif
        goto out;
    }
    ev.events = EPOLLIN;
    ev.data.fd = s2;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s2, &ev); 
    if (ret) {
#ifdef DEBUG
        fprintf(stderr, "epoll ctrol add error.\n");
#endif
        goto out;
    }

    for(;;) {
        ret = epoll_wait(epoll_fd, events, 1, -1);
        if (ret != 1) {
#ifdef DEBUG
            fprintf(stderr, "epoll ctrol add error.\n");
#endif
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
#ifdef DEBUG
    fprintf(stderr, "thread quit.\n");
#endif
    return NULL;
}

static void daemon_init() {
#ifndef DEBUG
    int ret;
    pid_t pid;

    pid = fork();
    if (pid < 0) {
#ifdef DEBUG
        fprintf(stderr, "WARNING : fork() error, can not be daemon!\n");
#endif
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
#endif

    return;
}

int main(int argc, const char **argv) {
    int ret, s, s1, s2, epoll_fd;
    struct sockaddr_in server;
    struct socket_pair *sp;
    pthread_t pt;
    struct epoll_event ev;
    struct epoll_event events[1];
    uint32_t cmd;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s server_ip server_port local_port\n", argv[0]);
        exit(1);
    }

    daemon_init();

    signal(SIGPIPE, SIG_IGN);

    /* control connections */
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        exit(1);
    }
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(atoi(argv[2]));
    server.sin_addr.s_addr = inet_addr(argv[1]);

    ret = connect(s, (struct sockaddr *)&server, sizeof(server));
    if (ret) {
#ifdef DEBUG
        fprintf(stderr, "connet [%s][%s] error.\n", argv[1], argv[2]);
#endif
        exit(1);
    }
#ifdef DEBUG
    printf("connet [%s][%s] successfully\n", argv[1], argv[2]);
#endif

    epoll_fd = epoll_create(1);
    if (epoll_fd < 0) {
#ifdef DEBUG
        fprintf(stderr, "epoll create error.\n");
#endif
        exit(1);
    }
    ev.events = EPOLLIN;
    ev.data.fd = s;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s, &ev); 
    if (ret) {
#ifdef DEBUG
        fprintf(stderr, "epoll ctrol add error.\n");
#endif
        exit(1);
    }

loop:
    ret = epoll_wait(epoll_fd, events, 1, 60*1000);
    if (ret < 0) {
        /* error */
        exit(1);
    } else if (ret == 0) {
        /* time out */
        cmd = htonl(0);
        /* send heartbreak */
        ret = write(s, &cmd, sizeof(cmd));
        if (ret != sizeof(cmd)) {
            exit(1);
        }
        goto loop;
    } else if (ret == 1) {
        ret = read(s, &cmd, sizeof(cmd));
        if (ret != sizeof(cmd)) {
            exit(1);
        }
        cmd = ntohl(cmd);
    } else {
        /* error */
        exit(1);
    }

    if (cmd == 0) {
        /* recv heartbreak resp */
        goto loop;
    }
    if (cmd != 1) {
        /* unknown command */
       goto out;
    }

    /* cmd == 1, create new connection */
    s1 = socket(AF_INET, SOCK_STREAM, 0);
    if (s1 < 0) {
        exit(1);
    }
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(atoi(argv[2]));
    server.sin_addr.s_addr = inet_addr(argv[1]);

    ret = connect(s1, (struct sockaddr *)&server, sizeof(server));
    if (ret) {
#ifdef DEBUG
        fprintf(stderr, "connet [%s][%s] error.\n", argv[1], argv[2]);
#endif
        exit(1);
    }
#ifdef DEBUG
    printf("connet [%s][%s] successfully\n", argv[1], argv[2]);
#endif

    s2 = socket(AF_INET, SOCK_STREAM, 0);
    if (s2 < 0) {
        exit(1);
    }
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(atoi(argv[3]));
    server.sin_addr.s_addr = inet_addr("127.0.0.1");

    ret = connect(s2, (struct sockaddr *)&server, sizeof(server));
    if (ret) {
#ifdef DEBUG
        fprintf(stderr, "connet [127.0.0.1][%s] error.\n", argv[3]);
#endif
        exit(1);
    }
#ifdef DEBUG
    printf("connet [127.0.0.1][%s] successfully\n", argv[3]);
#endif

    sp = malloc(sizeof(struct socket_pair));
    if (sp == NULL) {
        exit(1);
    }
    sp->s1 = s1;
    sp->s2 = s2;
    ret = pthread_create(&pt, NULL, data_forward, sp);
    if (ret) {
        exit(1);
    }
#ifdef DEBUG
    printf("created proxy threads\n");
#endif

    goto loop;

out:
    close(epoll_fd);

    return 0;
}
