#include "threadpool.h"
#include "test.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

static atomic_int counter;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static void inc(void *arg) { (void)arg; atomic_fetch_add(&counter, 1); }
static void blocked(void *arg) { (void)arg; pthread_mutex_lock(&gate); pthread_mutex_unlock(&gate); atomic_fetch_add(&counter, 1); }

int main(void) {
    CHECK(tp_create(0, 10) == NULL && tp_create(2, 0) == NULL);

    /* 모든 작업이 정확히 한 번씩 실행된다 (destroy는 남은 작업을 다 끝낸다) */
    ThreadPool *tp = tp_create(8, 4096);
    for (int i = 0; i < 4000; i++) CHECK(tp_submit(tp, inc, NULL) == 0);
    tp_destroy(tp);
    CHECK(atomic_load(&counter) == 4000);

    /* 큐가 가득 차면 거절한다: 워커 1개를 막아 두고 큐(2칸)를 채운다 */
    atomic_store(&counter, 0);
    pthread_mutex_lock(&gate);
    tp = tp_create(1, 2);
    CHECK(tp_submit(tp, blocked, NULL) == 0);
    usleep(100 * 1000);                    /* 워커가 첫 작업을 집어 가서 막힐 때까지 */
    CHECK(tp_submit(tp, blocked, NULL) == 0);
    CHECK(tp_submit(tp, blocked, NULL) == 0);
    CHECK(tp_submit(tp, blocked, NULL) == -1);
    pthread_mutex_unlock(&gate);
    tp_destroy(tp);
    CHECK(atomic_load(&counter) == 3);
    TEST_END();
}
