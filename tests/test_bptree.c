#include "bptree.h"
#include "test.h"
#include <stdlib.h>

static int collect(int64_t k, int64_t v, void *ctx) { int64_t **p = ctx; *(*p)++ = k; (void)v; return 1; }
static int stop_after_3(int64_t k, int64_t v, void *ctx) { (void)k; (void)v; return ++*(int *)ctx < 3; }

int main(void) {
    /* 빈 트리 */
    BPTree *t = bpt_create();
    int64_t v = -1;
    CHECK(bpt_size(t) == 0 && bpt_height(t) == 1 && bpt_validate(t));
    CHECK(!bpt_search(t, 1, &v));
    int cnt = 0; bpt_range(t, INT64_MIN, INT64_MAX, stop_after_3, &cnt); CHECK(cnt == 0);

    /* 오름차순 삽입 (자동 증가 id와 같은 패턴) */
    for (int64_t i = 1; i <= 10000; i++) CHECK(bpt_insert(t, i, i * 10) == 0);
    CHECK(bpt_size(t) == 10000 && bpt_height(t) > 3 && bpt_validate(t));
    for (int64_t i = 1; i <= 10000; i++) { CHECK(bpt_search(t, i, &v) && v == i * 10); }
    CHECK(!bpt_search(t, 0, &v) && !bpt_search(t, 10001, &v) && !bpt_search(t, -5, &v));

    /* 중복 key 거부, 기존 값 유지 */
    CHECK(bpt_insert(t, 500, 1) == -1);
    CHECK(bpt_search(t, 500, &v) && v == 5000 && bpt_size(t) == 10000);

    /* 범위 검색: 리프 연결 리스트를 따라 오름차순 */
    int64_t out[64], *p = out;
    bpt_range(t, 98, 103, collect, &p);
    CHECK(p - out == 6 && out[0] == 98 && out[5] == 103);
    p = out; bpt_range(t, 9999, INT64_MAX, collect, &p); CHECK(p - out == 2);
    p = out; bpt_range(t, INT64_MIN, 2, collect, &p);    CHECK(p - out == 2 && out[0] == 1);
    p = out; bpt_range(t, 10, 5, collect, &p);           CHECK(p - out == 0);   /* lo > hi */
    p = out; bpt_range(t, 20000, 30000, collect, &p);    CHECK(p - out == 0);
    cnt = 0; bpt_range(t, 1, 10000, stop_after_3, &cnt); CHECK(cnt == 3);       /* 조기 중단 */
    bpt_destroy(t);

    /* 내림차순 삽입 */
    t = bpt_create();
    for (int64_t i = 5000; i >= 1; i--) CHECK(bpt_insert(t, i, -i) == 0);
    CHECK(bpt_validate(t) && bpt_size(t) == 5000);
    for (int64_t i = 1; i <= 5000; i++) CHECK(bpt_search(t, i, &v) && v == -i);
    bpt_destroy(t);

    /* 무작위 삽입 + 음수/극단값 */
    t = bpt_create();
    srand(7);
    size_t inserted = 0;
    for (int i = 0; i < 20000; i++) {
        int64_t k = (int64_t)(rand() % 50000) - 25000;
        int had = bpt_search(t, k, NULL);
        int rc = bpt_insert(t, k, k * 2);
        CHECK(rc == (had ? -1 : 0));
        if (rc == 0) inserted++;
    }
    CHECK(bpt_insert(t, INT64_MAX, 1) == 0 && bpt_insert(t, INT64_MIN, 2) == 0);
    CHECK(bpt_size(t) == inserted + 2 && bpt_validate(t));
    CHECK(bpt_search(t, INT64_MAX, &v) && v == 1);
    CHECK(bpt_search(t, INT64_MIN, &v) && v == 2);
    bpt_destroy(t);
    bpt_destroy(NULL);
    TEST_END();
}
