/* bench: 대량 INSERT 후 "id 검색(B+ 트리)" 과 "다른 필드 검색(선형 탐색)" 속도 비교
 *   ./bench [-n 행수] [-d 디렉터리]      SQL 처리기를 그대로 거쳐서 삽입/조회
 *   ./bench -g out.sql [-n 행수]         CLI에 넣을 수 있는 INSERT SQL 파일만 생성 */
#include "db.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BATCH 1000

static void build_insert(StrBuf *sb, long from, long to) {
    sb->len = 0; sb->data[0] = 0;
    sb_puts(sb, "INSERT INTO users (name, age, email) VALUES ");
    for (long i = from; i <= to; i++)
        sb_printf(sb, "%s('user_%ld', %ld, 'user_%ld@example.com')", i > from ? "," : "", i, 20 + i % 50, i);
    sb_puts(sb, ";\n");
}

typedef struct { size_t rows; int errors; int used_index; } Tally;
static void tally(const Result *r, void *ud) {
    Tally *t = ud;
    if (!r->ok) { t->errors++; fprintf(stderr, "ERROR: %s\n", r->err); }
    t->rows += r->nrows; t->used_index = r->used_index;
}

/* 같은 형태의 쿼리를 reps번 실행해 평균 시간을 잰다 */
static double time_query(Database *db, const char *fmt, long n, int reps, Tally *t) {
    char sql[256];
    double t0 = now_ms();
    for (int i = 0; i < reps; i++) {
        long target = 1 + (long)(((unsigned long)rand() * 2654435761UL) % (unsigned long)n);
        snprintf(sql, sizeof sql, fmt, target);
        db_exec_script(db, sql, tally, t);
    }
    return (now_ms() - t0) / reps;
}

int main(int argc, char **argv) {
    long n = 1000000; const char *dir = "bench_data", *gen = NULL; int opt;
    while ((opt = getopt(argc, argv, "n:d:g:")) != -1) {
        if (opt == 'n') n = atol(optarg);
        else if (opt == 'd') dir = optarg;
        else if (opt == 'g') gen = optarg;
        else { fprintf(stderr, "usage: %s [-n rows] [-d dir] [-g out.sql]\n", argv[0]); return 2; }
    }
    if (n < 1) { fprintf(stderr, "ERROR: -n must be >= 1\n"); return 2; }
    StrBuf sb; sb_init(&sb);

    if (gen) {
        FILE *f = fopen(gen, "w");
        if (!f) { perror(gen); return 1; }
        for (long i = 1; i <= n; i += BATCH) { build_insert(&sb, i, i + BATCH - 1 > n ? n : i + BATCH - 1); fputs(sb.data, f); }
        fclose(f);
        printf("wrote %ld rows of INSERT statements to %s\n", n, gen);
        return 0;
    }

    char path[1200];
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/users.schema", dir);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    fputs("id:int,name:string,age:int,email:string\n", f); fclose(f);
    snprintf(path, sizeof path, "%s/users.csv", dir);
    unlink(path);                                        /* 매번 빈 테이블에서 시작 */

    Database *db = db_open(dir);
    Tally t = {0, 0, 0};
    printf("== 1. INSERT %ld rows (SQL 처리기 경유, 문장당 %d행) ==\n", n, BATCH);
    double t0 = now_ms();
    for (long i = 1; i <= n; i += BATCH) {
        build_insert(&sb, i, i + BATCH - 1 > n ? n : i + BATCH - 1);
        db_exec_script(db, sb.data, tally, &t);
    }
    double ins = now_ms() - t0;
    if (t.errors) return 1;
    printf("   %.0f ms  (%.0f rows/sec)\n", ins, n / (ins / 1000.0));

    srand(42);
    printf("== 2. SELECT 비교 (무작위 대상, 평균) ==\n");
    Tally ti = {0, 0, 0}, ts = {0, 0, 0}, te = {0, 0, 0};
    double idx = time_query(db, "SELECT * FROM users WHERE id = %ld;", n, 10000, &ti);
    double scan = time_query(db, "SELECT * FROM users WHERE name = 'user_%ld';", n, 10, &ts);
    double scan2 = time_query(db, "SELECT * FROM users WHERE email = 'user_%ld@example.com';", n, 10, &te);
    if (ti.rows != 10000 || ts.rows != 10 || te.rows != 10 || !ti.used_index || ts.used_index) {
        fprintf(stderr, "ERROR: unexpected query results\n"); return 1;
    }
    printf("   %-34s %12s %10s\n", "query", "avg time", "access");
    printf("   %-34s %9.4f ms %10s\n", "WHERE id = ?      (10000 runs)", idx, "B+ tree");
    printf("   %-34s %9.4f ms %10s\n", "WHERE name = ?    (10 runs)", scan, "full scan");
    printf("   %-34s %9.4f ms %10s\n", "WHERE email = ?   (10 runs)", scan2, "full scan");
    printf("   → 인덱스 검색이 선형 탐색보다 약 %.0f배 빠름\n", scan / idx);

    db_close(db);
    t0 = now_ms();
    db = db_open(dir);
    Tally tr = {0, 0, 0};
    db_exec_script(db, "SELECT * FROM users WHERE id = 1;", tally, &tr);
    printf("== 3. 재시작 후 인덱스 재구축 (파일 → B+ 트리) ==\n   %.0f ms\n", now_ms() - t0);
    db_close(db);
    sb_free(&sb);
    return 0;
}
