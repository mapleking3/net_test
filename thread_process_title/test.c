#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <sys/prctl.h>
#include "proctitle.h"

void *thread_test(void *arg)
{
    char thread_name[16] = {0};
    snprintf(thread_name, sizeof(thread_name), "%s", __func__);
    pthread_setname_np(pthread_self(), thread_name);
    arg = arg;
    while (1)
    {
        sleep(9);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    proctitle_init(argc, argv);
    char thread_name[16] = {0};
    pthread_t tid;
    pthread_create(&tid, NULL, thread_test, NULL);

    snprintf(thread_name, sizeof(thread_name), "%s", __func__);
    pthread_setname_np(pthread_self(), thread_name);

    int i = 0;
    while (1)
    {
        proctitle_set("vary_p_name_%d", ++i);
        sleep(2);
    }
    return 0;
}
