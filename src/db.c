#include "db.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* ═════════════ 1. 행 인코딩 (메모리 값 ↔ 파일의 한 줄) ═════════════ */

static void encode_field(StrBuf *sb, const char *s) {
    for (; *s; s++) {
        if (*s == '\\') sb_puts(sb, "\\\\");
        else if (*s == ',') sb_puts(sb, "\\,");
        else if (*s == '\n') sb_puts(sb, "\\n");
        else if (*s == '\r') sb_puts(sb, "\\r");
        else sb_putc(sb, *s);
    }
}

/* 한 줄을 제자리에서 필드로 쪼갠다(이스케이프 해제 포함). 반환: 필드 수, 손상 시 -1 */
static int split_row(char *line, char **fields, int max) {
    int n = 0; char *r = line, *w = line;
    fields[n++] = w;
    for (;;) {
        char c = *r++;
        if (!c) { *w = 0; break; }
        if (c == '\\') {
            char e = *r++;
            if (!e) return -1;
            *w++ = e == 'n' ? '\n' : e == 'r' ? '\r' : e;
        } else if (c == ',') {
            *w++ = 0;
            if (n == max) return -1;
            fields[n++] = w;
        } else *w++ = c;
    }
    return n;
}

/* ═════════════ 2. 파일 읽기 도우미 ═════════════ */

/* 파일을 큰 덩어리로 읽으면서 한 줄씩 꺼내 주는 리더 (선형 탐색 / 인덱스 재구축에 사용) */
typedef struct {
    int fd; int64_t pos, end;      /* 다음에 읽을 파일 위치, 읽을 끝 */
    char *buf; size_t cap, start, len;
    int64_t base;                  /* buf[0]의 파일 오프셋 */
    int io_error;
} LineReader;

static void lr_init(LineReader *lr, int fd, int64_t end) {
    memset(lr, 0, sizeof *lr);
    lr->fd = fd; lr->end = end; lr->cap = 1 << 20; lr->buf = xmalloc(lr->cap);
}
static char *lr_next(LineReader *lr, int64_t *off) {
    for (;;) {
        char *nl = memchr(lr->buf + lr->start, '\n', lr->len - lr->start);
        if (nl) {
            char *line = lr->buf + lr->start;
            *nl = 0; *off = lr->base + (int64_t)lr->start;
            lr->start = (size_t)(nl - lr->buf) + 1;
            return line;
        }
        if (lr->pos >= lr->end) return NULL;           /* 남은 것은 줄바꿈 없는 조각(있다면) */
        if (lr->start > 0) {                           /* 처리한 앞부분을 버리고 당긴다 */
            memmove(lr->buf, lr->buf + lr->start, lr->len - lr->start);
            lr->base += (int64_t)lr->start; lr->len -= lr->start; lr->start = 0;
        }
        if (lr->len == lr->cap) { lr->cap *= 2; lr->buf = xrealloc(lr->buf, lr->cap); }
        size_t want = lr->cap - lr->len;
        if ((int64_t)want > lr->end - lr->pos) want = (size_t)(lr->end - lr->pos);
        ssize_t n = pread(lr->fd, lr->buf + lr->len, want, lr->pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { lr->io_error = 1; return NULL; }
        lr->pos += n; lr->len += (size_t)n;
    }
}

/* 오프셋 off에서 시작하는 한 줄만 읽는다 (인덱스 탐색 후 "그 행만" 가져올 때 사용) */
static char *read_line_at(int fd, int64_t off, int64_t end) {
    size_t cap = 256, len = 0; char *buf = xmalloc(cap + 1);
    for (;;) {
        size_t want = cap - len;
        if ((int64_t)want > end - off - (int64_t)len) want = (size_t)(end - off - (int64_t)len);
        if (want == 0) { free(buf); return NULL; }
        ssize_t n = pread(fd, buf + len, want, off + (int64_t)len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(buf); return NULL; }
        char *nl = memchr(buf + len, '\n', (size_t)n);
        if (nl) { *nl = 0; return buf; }
        len += (size_t)n;
        if (len == cap) { cap *= 2; buf = xrealloc(buf, cap + 1); }
    }
}

/* ═════════════ 3. 테이블 열기 (스키마 읽기 + 파일 훑어서 B+ 트리 재구축) ═════════════ */

static int valid_ident(const char *s) {
    if (!*s || !(isalpha((unsigned char)*s) || *s == '_')) return 0;
    size_t n = 0;
    for (; *s; s++, n++) if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
    return n <= MAX_IDENT;
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int load_schema(Table *t, const char *path, char *err, size_t errsz) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errsz, "table '%s' does not exist", t->name); return -1; }
    char line[4096];
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    char *save = NULL;
    for (char *tok = strtok_r(line, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        if (!colon) { snprintf(err, errsz, "schema of '%s': expected name:type", t->name); return -1; }
        *colon = 0;
        char *name = trim(tok), *type = trim(colon + 1);
        if (t->ncols == MAX_COLS) { snprintf(err, errsz, "schema of '%s': too many columns (max %d)", t->name, MAX_COLS); return -1; }
        if (!valid_ident(name)) { snprintf(err, errsz, "schema of '%s': invalid column name", t->name); return -1; }
        for (int i = 0; i < t->ncols; i++)
            if (!strcasecmp(t->cols[i].name, name)) { snprintf(err, errsz, "schema of '%s': duplicate column '%s'", t->name, name); return -1; }
        Column *c = &t->cols[t->ncols++];
        strcpy(c->name, name);
        if (!strcasecmp(type, "int")) c->type = COL_INT;
        else if (!strcasecmp(type, "string") || !strcasecmp(type, "text")) c->type = COL_STR;
        else { snprintf(err, errsz, "schema of '%s': unknown type '%.20s' (use int or string)", t->name, type); return -1; }
    }
    if (t->ncols == 0 || strcasecmp(t->cols[0].name, "id") || t->cols[0].type != COL_INT) {
        snprintf(err, errsz, "schema of '%s': first column must be id:int", t->name); return -1;
    }
    return 0;
}

static int parse_i64(const char *s, int64_t *out) {
    char *end; errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || end == s || *end) return 0;
    *out = v; return 1;
}

static Table *table_open(Database *db, const char *name, char *err, size_t errsz) {
    Table *t = xmalloc(sizeof *t);
    memset(t, 0, sizeof *t);
    t->fd = -1; t->next_id = 1;
    snprintf(t->name, sizeof t->name, "%s", name);
    char path[1200];
    snprintf(path, sizeof path, "%s/%s.schema", db->dir, name);
    if (load_schema(t, path, err, errsz) < 0) goto fail;

    snprintf(path, sizeof path, "%s/%s.csv", db->dir, name);
    t->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (t->fd < 0) { snprintf(err, errsz, "cannot open data file of '%s': %s", name, strerror(errno)); goto fail; }
    /* 다른 프로세스(예: 서버 실행 중에 CLI)가 같은 파일을 동시에 쓰면 인덱스가 어긋나므로 막는다 */
    if (flock(t->fd, LOCK_EX | LOCK_NB) < 0) { snprintf(err, errsz, "table '%s' is in use by another process", name); goto fail; }
    struct stat sb;
    if (fstat(t->fd, &sb) < 0) { snprintf(err, errsz, "cannot stat data file of '%s'", name); goto fail; }

    t->index = bpt_create();
    LineReader lr; lr_init(&lr, t->fd, sb.st_size);
    char *fields[MAX_COLS], *line; int64_t off; int bad = 0;
    while ((line = lr_next(&lr, &off))) {
        int64_t id;
        if (split_row(line, fields, MAX_COLS) != t->ncols || !parse_i64(fields[0], &id) || id < 1) {
            snprintf(err, errsz, "data file of '%s' is corrupted at byte %lld", name, (long long)off); bad = 1; break;
        }
        if (bpt_insert(t->index, id, off) < 0) { snprintf(err, errsz, "data file of '%s' has duplicate id %lld", name, (long long)id); bad = 1; break; }
        if (id >= t->next_id) t->next_id = id + 1;
    }
    int64_t valid_end = lr.base + (int64_t)lr.start;   /* 마지막으로 온전히 읽은 줄의 끝 */
    int io_error = lr.io_error;
    free(lr.buf);
    if (bad) goto fail;
    if (io_error) { snprintf(err, errsz, "I/O error while reading '%s'", name); goto fail; }
    if (valid_end < sb.st_size) {                      /* 쓰다가 죽어서 줄바꿈 없이 끝난 조각 → 잘라낸다 */
        fprintf(stderr, "warning: table '%s': dropping %lld bytes of an incomplete last row\n", name, (long long)(sb.st_size - valid_end));
        if (ftruncate(t->fd, valid_end) < 0) { snprintf(err, errsz, "cannot repair data file of '%s'", name); goto fail; }
    }
    t->file_size = valid_end;
    pthread_rwlock_init(&t->lock, NULL);
    return t;
fail:
    if (t->fd >= 0) close(t->fd);
    bpt_destroy(t->index);
    free(t);
    return NULL;
}

static Table *get_table(Database *db, const char *name, char *err, size_t errsz) {
    pthread_mutex_lock(&db->mu);
    Table *t = db->tables;
    while (t && strcmp(t->name, name)) t = t->next;
    if (!t && (t = table_open(db, name, err, errsz))) { t->next = db->tables; db->tables = t; }
    pthread_mutex_unlock(&db->mu);
    return t;
}

Database *db_open(const char *dir) {
    struct stat sb;
    if (stat(dir, &sb) < 0 || !S_ISDIR(sb.st_mode)) return NULL;
    Database *db = xmalloc(sizeof *db);
    snprintf(db->dir, sizeof db->dir, "%s", dir);
    db->tables = NULL;
    pthread_mutex_init(&db->mu, NULL);
    return db;
}
void db_close(Database *db) {
    if (!db) return;
    for (Table *t = db->tables, *n; t; t = n) {
        n = t->next;
        close(t->fd); bpt_destroy(t->index); pthread_rwlock_destroy(&t->lock); free(t);
    }
    pthread_mutex_destroy(&db->mu);
    free(db);
}

/* ═════════════ 4. 실행기 ═════════════ */

static int find_col(const Table *t, const char *name) {
    for (int i = 0; i < t->ncols; i++) if (!strcasecmp(t->cols[i].name, name)) return i;
    return -1;
}
#define RFAIL(r, ...) do { (r)->ok = 0; snprintf((r)->err, sizeof (r)->err, __VA_ARGS__); return; } while (0)

/* ── INSERT: 검증 → (쓰기 락) id 부여 → 파일 끝에 추가 → B+ 트리에 등록 ── */
static void exec_insert(Table *t, const Statement *st, Result *r) {
    int nuser = t->ncols - 1;                         /* id를 뺀, 사용자가 채워야 하는 컬럼 수 */
    int map[MAX_COLS];                                /* map[테이블 컬럼 번호] = 값 번호 */
    if (st->ncols == 0) {
        if (st->nvals != nuser) RFAIL(r, "table '%s' needs %d values (id is assigned automatically) but %d given", t->name, nuser, st->nvals);
        for (int c = 1; c < t->ncols; c++) map[c] = c - 1;
    } else {
        for (int c = 0; c < t->ncols; c++) map[c] = -1;
        for (int i = 0; i < st->ncols; i++) {
            int c = find_col(t, st->cols[i]);
            if (c < 0) RFAIL(r, "unknown column '%s' in table '%s'", st->cols[i], t->name);
            if (c == 0) RFAIL(r, "column 'id' is assigned automatically and cannot be inserted");
            if (map[c] >= 0) RFAIL(r, "column '%s' listed twice", st->cols[i]);
            map[c] = i;
        }
        for (int c = 1; c < t->ncols; c++)
            if (map[c] < 0) RFAIL(r, "missing value for column '%s' (NULL is not supported)", t->cols[c].name);
    }
    for (int row = 0; row < st->nrows; row++)         /* 타입 검사는 락 밖에서 미리 */
        for (int c = 1; c < t->ncols; c++) {
            const Value *v = &st->vals[(size_t)row * (size_t)st->nvals + (size_t)map[c]];
            if (t->cols[c].type == COL_INT && v->type != VAL_INT) RFAIL(r, "column '%s' expects an integer (row %d)", t->cols[c].name, row + 1);
            if (t->cols[c].type == COL_STR && v->type != VAL_STR) RFAIL(r, "column '%s' expects a 'string' (row %d)", t->cols[c].name, row + 1);
        }

    StrBuf buf; sb_init(&buf);
    int64_t *offs = xmalloc((size_t)st->nrows * sizeof(int64_t));
    pthread_rwlock_wrlock(&t->lock);
    if (t->next_id > INT64_MAX - st->nrows) {
        pthread_rwlock_unlock(&t->lock); sb_free(&buf); free(offs);
        RFAIL(r, "id space exhausted");
    }
    int64_t first = t->next_id;
    for (int row = 0; row < st->nrows; row++) {
        offs[row] = t->file_size + (int64_t)buf.len;
        sb_printf(&buf, "%lld", (long long)(first + row));
        for (int c = 1; c < t->ncols; c++) {
            const Value *v = &st->vals[(size_t)row * (size_t)st->nvals + (size_t)map[c]];
            sb_putc(&buf, ',');
            if (v->type == VAL_INT) sb_printf(&buf, "%lld", (long long)v->i);
            else encode_field(&buf, v->s);
        }
        sb_putc(&buf, '\n');
    }
    size_t done = 0; int werr = 0;
    while (done < buf.len) {
        ssize_t n = pwrite(t->fd, buf.data + done, buf.len - done, t->file_size + (int64_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { werr = errno ? errno : EIO; break; }
        done += (size_t)n;
    }
    if (werr) {                                       /* 반쯤 쓰인 것을 되돌린다 → 문장 단위로 전부 아니면 전무 */
        if (ftruncate(t->fd, t->file_size) < 0) { /* 다음에 열 때 조각 복구 로직이 처리 */ }
        pthread_rwlock_unlock(&t->lock); sb_free(&buf); free(offs);
        RFAIL(r, "write failed: %s", strerror(werr));
    }
    for (int row = 0; row < st->nrows; row++) bpt_insert(t->index, first + row, offs[row]);
    t->file_size += (int64_t)buf.len;
    t->next_id += st->nrows;
    pthread_rwlock_unlock(&t->lock);
    sb_free(&buf); free(offs);
    r->affected = st->nrows; r->first_id = first; r->last_id = first + st->nrows - 1;
}

/* ── SELECT ── */
typedef struct {
    Table *t; const Statement *st; Result *r;
    int wcol;                 /* WHERE 컬럼 번호, 없으면 -1 */
    int proj[MAX_COLS]; int nproj;
    int io_error;
} SelCtx;

static int row_matches(const SelCtx *c, char **fields) {
    if (c->wcol < 0) return 1;
    const Value *v = &c->st->where_val;
    int cmp;
    if (c->t->cols[c->wcol].type == COL_INT) {
        int64_t x = strtoll(fields[c->wcol], NULL, 10);
        cmp = x < v->i ? -1 : x > v->i ? 1 : 0;
    } else cmp = strcmp(fields[c->wcol], v->s);
    switch (c->st->where_op) {
    case OP_EQ: return cmp == 0;  case OP_NE: return cmp != 0;
    case OP_LT: return cmp < 0;   case OP_LE: return cmp <= 0;
    case OP_GT: return cmp > 0;   default:    return cmp >= 0;
    }
}
static int limit_reached(const SelCtx *c) { return c->st->limit >= 0 && (int64_t)c->r->nrows >= c->st->limit; }

static void emit_row(SelCtx *c, char **fields) {
    Result *r = c->r;
    if (r->nrows == r->cap) { r->cap = r->cap ? r->cap * 2 : 16; r->rows = xrealloc(r->rows, r->cap * sizeof(char **)); }
    char **row = xmalloc((size_t)c->nproj * sizeof(char *));
    for (int i = 0; i < c->nproj; i++) row[i] = xstrdup(fields[c->proj[i]]);
    r->rows[r->nrows++] = row;
}

/* B+ 트리가 찾아 준 오프셋으로 가서 그 행만 읽는다 */
static int index_visit(int64_t key, int64_t off, void *ud) {
    (void)key;
    SelCtx *c = ud;
    if (limit_reached(c)) return 0;
    char *line = read_line_at(c->t->fd, off, c->t->file_size);
    char *fields[MAX_COLS];
    if (!line || split_row(line, fields, MAX_COLS) != c->t->ncols) { free(line); c->io_error = 1; return 0; }
    if (row_matches(c, fields)) emit_row(c, fields);
    free(line);
    return 1;
}

static void exec_select(Table *t, const Statement *st, Result *r) {
    SelCtx c; memset(&c, 0, sizeof c);
    c.t = t; c.st = st; c.r = r; c.wcol = -1;
    if (st->star) { c.nproj = t->ncols; for (int i = 0; i < t->ncols; i++) c.proj[i] = i; }
    else {
        if (st->ncols > MAX_COLS) RFAIL(r, "too many columns in SELECT list");
        for (int i = 0; i < st->ncols; i++) {
            int col = find_col(t, st->cols[i]);
            if (col < 0) RFAIL(r, "unknown column '%s' in table '%s'", st->cols[i], t->name);
            c.proj[c.nproj++] = col;
        }
    }
    if (st->has_where) {
        c.wcol = find_col(t, st->where_col);
        if (c.wcol < 0) RFAIL(r, "unknown column '%s' in WHERE", st->where_col);
        if (t->cols[c.wcol].type == COL_INT && st->where_val.type != VAL_INT) RFAIL(r, "column '%s' is an integer; compare it with an integer", t->cols[c.wcol].name);
        if (t->cols[c.wcol].type == COL_STR && st->where_val.type != VAL_STR) RFAIL(r, "column '%s' is a string; compare it with a 'string'", t->cols[c.wcol].name);
    }
    r->ncols = c.nproj;
    r->colnames = xmalloc((size_t)c.nproj * sizeof(char *));
    r->coltypes = xmalloc((size_t)c.nproj * sizeof(ColType));
    for (int i = 0; i < c.nproj; i++) { r->colnames[i] = xstrdup(t->cols[c.proj[i]].name); r->coltypes[i] = t->cols[c.proj[i]].type; }

    pthread_rwlock_rdlock(&t->lock);
    /* 계획 선택: WHERE가 id에 대한 =, <, <=, >, >= 이면 B+ 트리, 아니면 파일 전체 선형 탐색 */
    if (c.wcol == 0 && st->where_op != OP_NE) {
        r->used_index = 1;
        int64_t v = st->where_val.i, lo = INT64_MIN, hi = INT64_MAX; int empty = 0;
        switch (st->where_op) {
        case OP_EQ: lo = hi = v; break;
        case OP_LT: if (v == INT64_MIN) empty = 1; else hi = v - 1; break;
        case OP_LE: hi = v; break;
        case OP_GT: if (v == INT64_MAX) empty = 1; else lo = v + 1; break;
        default:    lo = v; break;
        }
        if (!empty) bpt_range(t->index, lo, hi, index_visit, &c);
    } else {
        LineReader lr; lr_init(&lr, t->fd, t->file_size);
        char *fields[MAX_COLS], *line; int64_t off;
        while (!limit_reached(&c) && (line = lr_next(&lr, &off))) {
            if (split_row(line, fields, MAX_COLS) != t->ncols) { c.io_error = 1; break; }
            if (row_matches(&c, fields)) emit_row(&c, fields);
        }
        if (lr.io_error) c.io_error = 1;
        free(lr.buf);
    }
    pthread_rwlock_unlock(&t->lock);
    if (c.io_error) RFAIL(r, "I/O error while reading table '%s'", t->name);
}

void db_exec_stmt(Database *db, const Statement *st, Result *r) {
    memset(r, 0, sizeof *r);
    r->ok = 1; r->type = st->type;
    double t0 = now_ms();
    Table *t = get_table(db, st->table, r->err, sizeof r->err);
    if (!t) r->ok = 0;
    else if (st->type == STMT_INSERT) exec_insert(t, st, r);
    else exec_select(t, st, r);
    r->elapsed_ms = now_ms() - t0;
}

int db_exec_script(Database *db, const char *sql, ResultCb cb, void *ud) {
    const char *cur = sql; int errors = 0;
    for (;;) {
        Statement st; Result r; char err[256];
        int rc = parse_statement(&cur, &st, err, sizeof err);
        if (rc == 0) break;
        if (rc < 0) { memset(&r, 0, sizeof r); snprintf(r.err, sizeof r.err, "%s", err); }
        else { db_exec_stmt(db, &st, &r); stmt_free(&st); }
        if (!r.ok) errors++;
        if (cb) cb(&r, ud);
        result_free(&r);
    }
    return errors;
}

/* ═════════════ 5. 결과 출력 ═════════════ */

void result_free(Result *r) {
    for (size_t i = 0; i < r->nrows; i++) {
        for (int j = 0; j < r->ncols; j++) free(r->rows[i][j]);
        free(r->rows[i]);
    }
    free(r->rows);
    for (int j = 0; j < r->ncols; j++) free(r->colnames[j]);
    free(r->colnames); free(r->coltypes);
    r->rows = NULL; r->colnames = NULL; r->coltypes = NULL; r->nrows = 0; r->ncols = 0;
}

static void print_cell(FILE *out, const char *s, int width) {
    int n = 0;
    for (; *s; s++, n++) {                             /* 줄바꿈은 표가 깨지지 않게 바꿔서 출력 */
        if (*s == '\n') { fputs("\\n", out); n++; }
        else if (*s == '\r') { fputs("\\r", out); n++; }
        else fputc(*s, out);
    }
    for (; n < width; n++) fputc(' ', out);
}
static int cell_width(const char *s) {
    int n = 0;
    for (; *s; s++) n += (*s == '\n' || *s == '\r') ? 2 : 1;
    return n;
}
void result_print(const Result *r, FILE *out) {
    if (!r->ok) { fprintf(out, "ERROR: %s\n", r->err); return; }
    if (r->type == STMT_INSERT) {
        if (r->affected == 1) fprintf(out, "INSERT 1 (id %lld)\n", (long long)r->first_id);
        else fprintf(out, "INSERT %lld (id %lld..%lld)\n", (long long)r->affected, (long long)r->first_id, (long long)r->last_id);
        return;
    }
    int *w = xmalloc((size_t)r->ncols * sizeof(int));
    for (int j = 0; j < r->ncols; j++) w[j] = cell_width(r->colnames[j]);
    for (size_t i = 0; i < r->nrows; i++)
        for (int j = 0; j < r->ncols; j++) { int n = cell_width(r->rows[i][j]); if (n > w[j]) w[j] = n; }
    for (int j = 0; j < r->ncols; j++) { fputs(j ? " | " : "", out); print_cell(out, r->colnames[j], w[j]); }
    fputc('\n', out);
    for (int j = 0; j < r->ncols; j++) { fputs(j ? "-+-" : "", out); for (int k = 0; k < w[j]; k++) fputc('-', out); }
    fputc('\n', out);
    for (size_t i = 0; i < r->nrows; i++) {
        for (int j = 0; j < r->ncols; j++) { fputs(j ? " | " : "", out); print_cell(out, r->rows[i][j], w[j]); }
        fputc('\n', out);
    }
    fprintf(out, "(%zu row%s, %s, %.3f ms)\n", r->nrows, r->nrows == 1 ? "" : "s", r->used_index ? "index" : "full scan", r->elapsed_ms);
    free(w);
}

void result_to_json(const Result *r, StrBuf *sb) {
    if (!r->ok) { sb_puts(sb, "{\"ok\":false,\"error\":"); sb_json_str(sb, r->err); sb_putc(sb, '}'); return; }
    if (r->type == STMT_INSERT) {
        sb_printf(sb, "{\"ok\":true,\"type\":\"insert\",\"affected\":%lld,\"first_id\":%lld,\"last_id\":%lld,\"elapsed_ms\":%.3f}",
                  (long long)r->affected, (long long)r->first_id, (long long)r->last_id, r->elapsed_ms);
        return;
    }
    sb_puts(sb, "{\"ok\":true,\"type\":\"select\",\"columns\":[");
    for (int j = 0; j < r->ncols; j++) { if (j) sb_putc(sb, ','); sb_json_str(sb, r->colnames[j]); }
    sb_puts(sb, "],\"rows\":[");
    for (size_t i = 0; i < r->nrows; i++) {
        sb_puts(sb, i ? ",[" : "[");
        for (int j = 0; j < r->ncols; j++) {
            if (j) sb_putc(sb, ',');
            if (r->coltypes[j] == COL_INT) sb_puts(sb, r->rows[i][j]); else sb_json_str(sb, r->rows[i][j]);
        }
        sb_putc(sb, ']');
    }
    sb_printf(sb, "],\"row_count\":%zu,\"access\":\"%s\",\"elapsed_ms\":%.3f}", r->nrows, r->used_index ? "index" : "scan", r->elapsed_ms);
}
