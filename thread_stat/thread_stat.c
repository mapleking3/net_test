#include <sys/types.h>
#include <pthread.h>
#include <sys/syscall.h>

#include "list.h"

#define THREAD_INFO_TEST
#ifdef THREAD_INFO_TEST
struct thread_node {
    struct list_head        node;
    pid_t                   t_id;
    char                    t_entry_fname[256];
};

static struct thread_list {
    struct list_head        head;
    pthread_mutex_t         mutex;
} tlist = {
    .head = {
        .prev = &tlist.head,
        .next = &tlist.head,
    },
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};
#endif

void pthread_push(const char *fname)
{
#ifdef THREAD_INFO_TEST
    pid_t thread_id = syscall(SYS_gettid);
    struct thread_node *node = (struct thread_node *)calloc(1,
            sizeof(struct thread_node));
    if (NULL == node) {
        printf("calloc thread node error:%m\n");
        return;
    }

    printf("thread push(id:%d, entry:%s)\n", thread_id, fname);

    node->t_id = thread_id;
    snprintf(node->t_entry_fname, sizeof(node->t_entry_fname), "%s", fname);

    pthread_mutex_lock(&tlist.mutex);
    list_add_tail(&node->node, &tlist.head);
    pthread_mutex_unlock(&tlist.mutex);
    return;
#endif
}

void pthread_pop(void)
{
#ifdef THREAD_INFO_TEST
    pid_t thread_id = syscall(SYS_gettid);
    struct thread_node *pos = NULL;
    struct thread_node *tmp = NULL;
    pthread_mutex_lock(&tlist.mutex);
    list_for_each_entry_safe(pos, tmp, &tlist.head, node)
    {
        if (pos->t_id == thread_id)
        {
            list_del(&pos->node);
            free(pos);
            break;
        }
    }
    pthread_mutex_unlock(&tlist.mutex);

    return;
#endif
}

