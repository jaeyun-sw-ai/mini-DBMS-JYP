#include "threadpool.h"
#include "util.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct { void (*fn)(void *); void *arg; } Task;

struct ThreadPool {
    pthread_t *threads; int nthreads;
    Task *queue; int cap, head, count;     /* 원형 큐 */
    pthread_mutex_t mu;
    pthread_cond_t not_empty;              /* 워커가 "일이 생길 때까지" 잠드는 곳 */
    int stopping;
};

static void *worker(void *arg) {
    ThreadPool *tp = arg;
    for (;;) {
        pthread_mutex_lock(&tp->mu);
        while (tp->count == 0 && !tp->stopping) pthread_cond_wait(&tp->not_empty, &tp->mu);
        if (tp->count == 0) { pthread_mutex_unlock(&tp->mu); return NULL; }   /* 종료 중이고 남은 일도 없음 */
        Task task = tp->queue[tp->head];
        tp->head = (tp->head + 1) % tp->cap; tp->count--;
        pthread_mutex_unlock(&tp->mu);
        task.fn(task.arg);                 /* 락을 푼 뒤에 실행해야 다른 워커가 동시에 일할 수 있다 */
    }
}

ThreadPool *tp_create(int nthreads, int queue_cap) {
    if (nthreads < 1 || queue_cap < 1) return NULL;
    ThreadPool *tp = xmalloc(sizeof *tp);
    tp->threads = xmalloc((size_t)nthreads * sizeof(pthread_t));
    tp->queue = xmalloc((size_t)queue_cap * sizeof(Task));
    tp->cap = queue_cap; tp->head = tp->count = 0; tp->stopping = 0; tp->nthreads = 0;
    pthread_mutex_init(&tp->mu, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tp->threads[i], NULL, worker, tp) != 0) break;
        tp->nthreads++;
    }
    if (tp->nthreads == 0) { tp_destroy(tp); return NULL; }
    return tp;
}

int tp_submit(ThreadPool *tp, void (*fn)(void *), void *arg) {
    pthread_mutex_lock(&tp->mu);
    if (tp->stopping || tp->count == tp->cap) { pthread_mutex_unlock(&tp->mu); return -1; }
    tp->queue[(tp->head + tp->count) % tp->cap] = (Task){fn, arg};
    tp->count++;
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
    return 0;
}

void tp_destroy(ThreadPool *tp) {
    if (!tp) return;
    pthread_mutex_lock(&tp->mu);
    tp->stopping = 1;
    pthread_cond_broadcast(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
    for (int i = 0; i < tp->nthreads; i++) pthread_join(tp->threads[i], NULL);
    pthread_mutex_destroy(&tp->mu);
    pthread_cond_destroy(&tp->not_empty);
    free(tp->threads); free(tp->queue); free(tp);
}
