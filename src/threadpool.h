/* threadpool: 고정 개수의 워커 스레드 + 크기 제한이 있는 작업 큐 */
#ifndef THREADPOOL_H
#define THREADPOOL_H
typedef struct ThreadPool ThreadPool;
ThreadPool *tp_create(int nthreads, int queue_cap);
int  tp_submit(ThreadPool *tp, void (*fn)(void *), void *arg);  /* 0 성공, -1 큐가 가득 참/종료 중 */
void tp_destroy(ThreadPool *tp);                                /* 큐에 남은 작업까지 끝내고 종료 */
#endif
