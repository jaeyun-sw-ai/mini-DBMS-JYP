#include "bptree.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

typedef struct Node {
    int is_leaf;
    int n;                                   /* 현재 key 개수 */
    int64_t keys[BPT_MAX_KEYS + 1];          /* +1: 넘친 뒤에 분할하기 위한 임시 칸 */
    union {
        struct Node *child[BPT_MAX_KEYS + 2];/* 내부 노드: 자식은 key보다 1개 많다 */
        int64_t vals[BPT_MAX_KEYS + 1];      /* 리프 노드: key와 1:1 */
    };
    struct Node *next;                       /* 리프끼리 잇는 연결 리스트 */
} Node;

struct BPTree { Node *root; size_t size; int height; };

static Node *node_new(int is_leaf) {
    Node *n = xmalloc(sizeof *n);
    n->is_leaf = is_leaf; n->n = 0; n->next = NULL;
    return n;
}

BPTree *bpt_create(void) {
    BPTree *t = xmalloc(sizeof *t);
    t->root = node_new(1); t->size = 0; t->height = 1;
    return t;
}

static void node_free(Node *n) {
    if (!n->is_leaf) for (int i = 0; i <= n->n; i++) node_free(n->child[i]);
    free(n);
}
void bpt_destroy(BPTree *t) { if (t) { node_free(t->root); free(t); } }
size_t bpt_size(const BPTree *t) { return t->size; }
int bpt_height(const BPTree *t) { return t->height; }

/* keys 중 key 이상인 첫 위치 (리프에서 사용) */
static int lower_bound(const int64_t *keys, int n, int64_t key) {
    int lo = 0, hi = n;
    while (lo < hi) { int mid = (lo + hi) / 2; if (keys[mid] < key) lo = mid + 1; else hi = mid; }
    return lo;
}
/* keys 중 key 초과인 첫 위치 = 내려갈 자식 번호 (내부 노드에서 사용) */
static int upper_bound(const int64_t *keys, int n, int64_t key) {
    int lo = 0, hi = n;
    while (lo < hi) { int mid = (lo + hi) / 2; if (keys[mid] <= key) lo = mid + 1; else hi = mid; }
    return lo;
}

static const Node *find_leaf(const BPTree *t, int64_t key) {
    const Node *n = t->root;
    while (!n->is_leaf) n = n->child[upper_bound(n->keys, n->n, key)];
    return n;
}

int bpt_search(const BPTree *t, int64_t key, int64_t *val) {
    const Node *leaf = find_leaf(t, key);
    int i = lower_bound(leaf->keys, leaf->n, key);
    if (i < leaf->n && leaf->keys[i] == key) { if (val) *val = leaf->vals[i]; return 1; }
    return 0;
}

/* 재귀 삽입. 반환: 0 = 끝, 1 = 이 노드가 분할됨(*up_key, *up_node를 부모에 꽂아야 함), -1 = 중복 */
static int insert_rec(Node *n, int64_t key, int64_t val, int64_t *up_key, Node **up_node) {
    if (n->is_leaf) {
        int i = lower_bound(n->keys, n->n, key);
        if (i < n->n && n->keys[i] == key) return -1;
        memmove(&n->keys[i + 1], &n->keys[i], (size_t)(n->n - i) * sizeof(int64_t));
        memmove(&n->vals[i + 1], &n->vals[i], (size_t)(n->n - i) * sizeof(int64_t));
        n->keys[i] = key; n->vals[i] = val; n->n++;
        if (n->n <= BPT_MAX_KEYS) return 0;
        /* 리프 분할: 오른쪽 절반을 새 리프로 옮기고, 새 리프의 첫 key를 "복사"해서 올린다 */
        Node *r = node_new(1);
        int mid = n->n / 2;
        r->n = n->n - mid;
        memcpy(r->keys, &n->keys[mid], (size_t)r->n * sizeof(int64_t));
        memcpy(r->vals, &n->vals[mid], (size_t)r->n * sizeof(int64_t));
        n->n = mid;
        r->next = n->next; n->next = r;
        *up_key = r->keys[0]; *up_node = r;
        return 1;
    }
    int i = upper_bound(n->keys, n->n, key);
    int64_t ck; Node *cn;
    int rc = insert_rec(n->child[i], key, val, &ck, &cn);
    if (rc <= 0) return rc;
    /* 자식이 분할됨 → 올라온 key와 새 자식을 i 자리에 끼운다 */
    memmove(&n->keys[i + 1], &n->keys[i], (size_t)(n->n - i) * sizeof(int64_t));
    memmove(&n->child[i + 2], &n->child[i + 1], (size_t)(n->n - i) * sizeof(Node *));
    n->keys[i] = ck; n->child[i + 1] = cn; n->n++;
    if (n->n <= BPT_MAX_KEYS) return 0;
    /* 내부 노드 분할: 가운데 key는 복사가 아니라 위로 "이동"한다 */
    Node *r = node_new(0);
    int mid = n->n / 2;
    r->n = n->n - mid - 1;
    memcpy(r->keys, &n->keys[mid + 1], (size_t)r->n * sizeof(int64_t));
    memcpy(r->child, &n->child[mid + 1], (size_t)(r->n + 1) * sizeof(Node *));
    *up_key = n->keys[mid]; *up_node = r;
    n->n = mid;
    return 1;
}

int bpt_insert(BPTree *t, int64_t key, int64_t val) {
    int64_t up_key; Node *up_node;
    int rc = insert_rec(t->root, key, val, &up_key, &up_node);
    if (rc < 0) return -1;
    if (rc == 1) {                           /* 루트가 분할됨 → 트리 높이 +1 */
        Node *root = node_new(0);
        root->n = 1; root->keys[0] = up_key;
        root->child[0] = t->root; root->child[1] = up_node;
        t->root = root; t->height++;
    }
    t->size++;
    return 0;
}

void bpt_range(const BPTree *t, int64_t lo, int64_t hi, BptVisit fn, void *ctx) {
    if (lo > hi) return;
    const Node *leaf = find_leaf(t, lo);
    int i = lower_bound(leaf->keys, leaf->n, lo);
    for (; leaf; leaf = leaf->next, i = 0)
        for (; i < leaf->n; i++) {
            if (leaf->keys[i] > hi) return;
            if (!fn(leaf->keys[i], leaf->vals[i], ctx)) return;
        }
}

/* 불변식 검사: 정렬, key 범위, 모든 리프 깊이 동일, 리프 체인 개수 == size */
static int validate_rec(const Node *n, int depth, int height, int has_lo, int64_t lo, int has_hi, int64_t hi) {
    if (n->n > BPT_MAX_KEYS) return 0;
    for (int i = 0; i < n->n; i++) {
        if (i > 0 && n->keys[i - 1] >= n->keys[i]) return 0;
        if (has_lo && n->keys[i] < lo) return 0;
        if (has_hi && n->keys[i] >= hi) return 0;
    }
    if (n->is_leaf) return depth == height;
    if (n->n < 1) return 0;
    for (int i = 0; i <= n->n; i++) {
        int hl = i > 0 ? 1 : has_lo, hh = i < n->n ? 1 : has_hi;
        int64_t l = i > 0 ? n->keys[i - 1] : lo, h = i < n->n ? n->keys[i] : hi;
        if (!validate_rec(n->child[i], depth + 1, height, hl, l, hh, h)) return 0;
    }
    return 1;
}
int bpt_validate(const BPTree *t) {
    if (!validate_rec(t->root, 1, t->height, 0, 0, 0, 0)) return 0;
    const Node *n = t->root;
    while (!n->is_leaf) n = n->child[0];
    size_t count = 0; int first = 1; int64_t prev = 0;
    for (; n; n = n->next)
        for (int i = 0; i < n->n; i++) {
            if (!first && prev >= n->keys[i]) return 0;
            prev = n->keys[i]; first = 0; count++;
        }
    return count == t->size;
}
