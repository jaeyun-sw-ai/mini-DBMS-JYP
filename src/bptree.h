/* bptree: 메모리 기반 B+ 트리 (key = 레코드 id, value = 데이터 파일 안의 행 오프셋)
 *  - 내부 노드: 길 안내용 key + 자식 포인터
 *  - 리프 노드: 실제 (key, value) + 옆 리프로 가는 next 포인터 → 범위 검색 가능 */
#ifndef BPTREE_H
#define BPTREE_H
#include <stddef.h>
#include <stdint.h>

#ifndef BPT_MAX_KEYS
#define BPT_MAX_KEYS 64          /* 노드 하나가 담는 최대 key 수 (테스트에서는 4로 줄여 분할을 자주 일으킨다) */
#endif

typedef struct BPTree BPTree;
typedef int (*BptVisit)(int64_t key, int64_t val, void *ctx);   /* 0을 반환하면 순회 중단 */

BPTree *bpt_create(void);
void    bpt_destroy(BPTree *t);
int     bpt_insert(BPTree *t, int64_t key, int64_t val);        /* 0 성공, -1 중복 key */
int     bpt_search(const BPTree *t, int64_t key, int64_t *val); /* 1 찾음, 0 없음 */
void    bpt_range(const BPTree *t, int64_t lo, int64_t hi, BptVisit fn, void *ctx); /* lo<=key<=hi 오름차순 */
size_t  bpt_size(const BPTree *t);
int     bpt_height(const BPTree *t);
int     bpt_validate(const BPTree *t);                          /* 1이면 B+ 트리 불변식 모두 만족 */
#endif
