#include "db.h"
#include "test.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[64];
static void write_file(const char *name, const char *content, int append) {
    char path[256]; snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, append ? "a" : "w"); fputs(content, f); fclose(f);
}

/* 마지막 결과를 복사해 두는 콜백 */
typedef struct { int n, ok, used_index; size_t nrows; int64_t first_id, last_id, affected; char err[256]; char cell[8][8][128]; } Last;
static void keep(const Result *r, void *ud) {
    Last *l = ud;
    l->n++; l->ok = r->ok; l->used_index = r->used_index; l->nrows = r->nrows;
    l->first_id = r->first_id; l->last_id = r->last_id; l->affected = r->affected;
    snprintf(l->err, sizeof l->err, "%s", r->err);
    for (size_t i = 0; i < r->nrows && i < 8; i++)
        for (int j = 0; j < r->ncols && j < 8; j++) snprintf(l->cell[i][j], 128, "%s", r->rows[i][j]);
}
static Last run(Database *db, const char *sql) { Last l; memset(&l, 0, sizeof l); db_exec_script(db, sql, keep, &l); return l; }
static int err_has(Database *db, const char *sql, const char *needle) { Last l = run(db, sql); return !l.ok && strstr(l.err, needle) != NULL; }

/* 동시성 테스트용 스레드 */
static Database *g_db;
static void *writer(void *arg) {
    char sql[128];
    for (int i = 0; i < 500; i++) {
        snprintf(sql, sizeof sql, "INSERT INTO users VALUES ('w%ld_%d', %d);", (long)(intptr_t)arg, i, i);
        if (db_exec_script(g_db, sql, NULL, NULL)) return (void *)1;
    }
    return NULL;
}
static void *reader(void *arg) {
    (void)arg;
    for (int i = 0; i < 300; i++) {
        Last l = run(g_db, i % 2 ? "SELECT * FROM users WHERE id = 3;" : "SELECT * FROM users WHERE age = 7 LIMIT 5;");
        if (!l.ok) return (void *)1;
        if (i % 2 && (l.nrows != 1 || strcmp(l.cell[0][1], "park"))) return (void *)1;
    }
    return NULL;
}

int main(void) {
    strcpy(g_dir, "/tmp/minidb_test_XXXXXX");
    if (!mkdtemp(g_dir)) return 1;
    write_file("users.schema", "id:int, name:string, age:int\n", 0);
    CHECK(db_open("/no/such/dir") == NULL);
    Database *db = db_open(g_dir);
    CHECK(db != NULL);
    Last l;

    /* 빈 테이블 조회 */
    l = run(db, "SELECT * FROM users;");                 CHECK(l.ok && l.nrows == 0);
    l = run(db, "SELECT * FROM users WHERE id = 1;");    CHECK(l.ok && l.nrows == 0 && l.used_index);

    /* INSERT: id 자동 부여 */
    l = run(db, "INSERT INTO users VALUES ('kim', 25);");                      CHECK(l.ok && l.affected == 1 && l.first_id == 1);
    l = run(db, "INSERT INTO users (age, name) VALUES (31, 'lee'), (28, 'park');"); CHECK(l.ok && l.affected == 2 && l.first_id == 2 && l.last_id == 3);

    /* SELECT: 인덱스 경로 vs 선형 탐색 경로 */
    l = run(db, "SELECT * FROM users WHERE id = 2;");
    CHECK(l.ok && l.nrows == 1 && l.used_index && !strcmp(l.cell[0][0], "2") && !strcmp(l.cell[0][1], "lee") && !strcmp(l.cell[0][2], "31"));
    l = run(db, "SELECT name FROM users WHERE name = 'park';");  CHECK(l.ok && l.nrows == 1 && !l.used_index && !strcmp(l.cell[0][0], "park"));
    l = run(db, "SELECT AGE, ID FROM users WHERE age > 25;");    CHECK(l.nrows == 2 && !strcmp(l.cell[0][0], "31") && !strcmp(l.cell[0][1], "2"));
    l = run(db, "SELECT * FROM users WHERE id >= 2;");           CHECK(l.nrows == 2 && l.used_index);
    l = run(db, "SELECT * FROM users WHERE id < 2;");            CHECK(l.nrows == 1 && l.used_index);
    l = run(db, "SELECT * FROM users WHERE id != 2;");           CHECK(l.nrows == 2 && !l.used_index);
    l = run(db, "SELECT * FROM users WHERE id = 999;");          CHECK(l.ok && l.nrows == 0);
    l = run(db, "SELECT * FROM users WHERE id = -1;");           CHECK(l.ok && l.nrows == 0);
    l = run(db, "SELECT * FROM users WHERE id > 9223372036854775807;"); CHECK(l.ok && l.nrows == 0);
    l = run(db, "SELECT * FROM users WHERE id < -9223372036854775807;"); CHECK(l.ok && l.nrows == 0);
    l = run(db, "SELECT * FROM users LIMIT 2;");                 CHECK(l.nrows == 2);
    l = run(db, "SELECT * FROM users LIMIT 0;");                 CHECK(l.ok && l.nrows == 0);
    l = run(db, "SELECT * FROM users WHERE id >= 1 LIMIT 1;");   CHECK(l.nrows == 1);
    l = run(db, "SELECT * FROM users WHERE name < 'l';");        CHECK(l.nrows == 1 && !strcmp(l.cell[0][1], "kim"));

    /* 특수 문자: 쉼표, 역슬래시, 줄바꿈, 따옴표, 빈 문자열, 한글 */
    l = run(db, "INSERT INTO users VALUES ('a,b\\c\nd''e', -3), ('', 0), ('홍길동', 40);");  CHECK(l.ok && l.first_id == 4);
    l = run(db, "SELECT * FROM users WHERE id = 4;");            CHECK(!strcmp(l.cell[0][1], "a,b\\c\nd'e") && !strcmp(l.cell[0][2], "-3"));
    l = run(db, "SELECT id FROM users WHERE name = '';");        CHECK(l.nrows == 1 && !strcmp(l.cell[0][0], "5"));
    l = run(db, "SELECT id FROM users WHERE name = '홍길동';");   CHECK(l.nrows == 1 && !strcmp(l.cell[0][0], "6"));
    l = run(db, "SELECT id FROM users WHERE name = 'a,b\\c\nd''e';"); CHECK(l.nrows == 1);

    /* 의미 오류 */
    CHECK(err_has(db, "SELECT * FROM nope;", "does not exist"));
    CHECK(err_has(db, "INSERT INTO nope VALUES (1);", "does not exist"));
    CHECK(err_has(db, "SELECT nope FROM users;", "unknown column"));
    CHECK(err_has(db, "SELECT * FROM users WHERE nope = 1;", "unknown column"));
    CHECK(err_has(db, "SELECT * FROM users WHERE id = 'x';", "integer"));
    CHECK(err_has(db, "SELECT * FROM users WHERE name = 1;", "string"));
    CHECK(err_has(db, "INSERT INTO users VALUES ('x');", "needs 2 values"));
    CHECK(err_has(db, "INSERT INTO users VALUES (1, 'x', 2);", "needs 2 values"));
    CHECK(err_has(db, "INSERT INTO users VALUES (1, 2);", "expects a 'string'"));
    CHECK(err_has(db, "INSERT INTO users VALUES ('x', 'y');", "expects an integer"));
    CHECK(err_has(db, "INSERT INTO users (id, name, age) VALUES (9, 'x', 1);", "automatically"));
    CHECK(err_has(db, "INSERT INTO users (name) VALUES ('x');", "missing value"));
    CHECK(err_has(db, "INSERT INTO users (name, name) VALUES ('x', 'y');", "twice"));
    CHECK(err_has(db, "INSERT INTO users (name, zzz) VALUES ('x', 1);", "unknown column"));
    /* 여러 행 중 하나라도 틀리면 아무것도 들어가지 않는다 */
    CHECK(err_has(db, "INSERT INTO users VALUES ('ok', 1), ('bad', 'x');", "row 2"));
    l = run(db, "SELECT * FROM users;");                         CHECK(l.nrows == 6);
    /* 스크립트: 오류 문장이 있어도 나머지는 실행 */
    Last m; memset(&m, 0, sizeof m);
    CHECK(db_exec_script(db, "SELECT * FROM users; SELEC x; INSERT INTO users VALUES ('z', 1);", keep, &m) == 1);
    CHECK(m.n == 3 && m.ok && m.first_id == 7);
    db_close(db);

    /* 영속성: 다시 열면 파일에서 B+ 트리를 재구축하고 id가 이어진다 */
    db = db_open(g_dir);
    l = run(db, "SELECT * FROM users WHERE id = 4;");            CHECK(l.nrows == 1 && l.used_index && !strcmp(l.cell[0][1], "a,b\\c\nd'e"));
    l = run(db, "INSERT INTO users VALUES ('after', 1);");       CHECK(l.ok && l.first_id == 8);
    /* 같은 파일을 다른 핸들로 동시에 열 수 없다 */
    Database *db2 = db_open(g_dir);
    CHECK(err_has(db2, "SELECT * FROM users;", "in use"));
    db_close(db2);
    db_close(db);

    /* 크래시 복구: 줄바꿈 없이 끝난 마지막 조각은 버린다 */
    write_file("users.csv", "9,torn-ro", 1);
    db = db_open(g_dir);
    l = run(db, "SELECT * FROM users;");                         CHECK(l.ok && l.nrows == 8);
    l = run(db, "INSERT INTO users VALUES ('nine', 9);");        CHECK(l.ok && l.first_id == 9);
    l = run(db, "SELECT name FROM users WHERE id = 9;");         CHECK(l.nrows == 1 && !strcmp(l.cell[0][0], "nine"));
    db_close(db);

    /* 손상된 파일 / 잘못된 스키마는 열기를 거부 */
    write_file("bad.schema", "id:int,v:int\n", 0);   write_file("bad.csv", "1,2\nxx\n", 0);
    write_file("dup.schema", "id:int,v:int\n", 0);   write_file("dup.csv", "1,2\n1,3\n", 0);
    write_file("noid.schema", "name:string\n", 0);
    write_file("badtype.schema", "id:int,v:float\n", 0);
    write_file("empty.schema", "", 0);
    db = db_open(g_dir);
    CHECK(err_has(db, "SELECT * FROM bad;", "corrupted"));
    CHECK(err_has(db, "SELECT * FROM dup;", "duplicate id"));
    CHECK(err_has(db, "SELECT * FROM noid;", "first column must be id"));
    CHECK(err_has(db, "SELECT * FROM badtype;", "unknown type"));
    CHECK(err_has(db, "SELECT * FROM empty;", "first column must be id"));

    /* 긴 행 (읽기 버퍼보다 큰 값) */
    size_t big = 300000; char *sql = malloc(big + 100);
    strcpy(sql, "INSERT INTO users VALUES ('"); memset(sql + strlen(sql), 'x', big); strcpy(sql + 27 + big, "', 1);");
    l = run(db, sql);                                            CHECK(l.ok && l.first_id == 10);
    Statement st; Result r; const char *cur = "SELECT name FROM users WHERE id = 10;"; char err[256];
    CHECK(parse_statement(&cur, &st, err, sizeof err) == 1);
    db_exec_stmt(db, &st, &r);                                   CHECK(r.ok && r.nrows == 1 && strlen(r.rows[0][0]) == big);
    result_free(&r); stmt_free(&st); free(sql);

    /* JSON 출력 */
    cur = "SELECT id, name FROM users WHERE id = 4;";
    parse_statement(&cur, &st, err, sizeof err); db_exec_stmt(db, &st, &r);
    StrBuf sb; sb_init(&sb); result_to_json(&r, &sb);
    CHECK(strstr(sb.data, "\"rows\":[[4,\"a,b\\\\c\\nd'e\"]]") && strstr(sb.data, "\"access\":\"index\""));
    sb_free(&sb); result_free(&r); stmt_free(&st);

    /* 동시성: 쓰기 8스레드 × 500행 + 읽기 8스레드 → id가 빠짐없이, 겹치지 않게 부여 */
    g_db = db;
    pthread_t w[8], rd[8]; void *ret; int thread_errors = 0;
    for (intptr_t i = 0; i < 8; i++) { pthread_create(&w[i], NULL, writer, (void *)i); pthread_create(&rd[i], NULL, reader, NULL); }
    for (int i = 0; i < 8; i++) { pthread_join(w[i], &ret); thread_errors += ret != NULL; pthread_join(rd[i], &ret); thread_errors += ret != NULL; }
    CHECK(thread_errors == 0);
    l = run(db, "SELECT id FROM users;");                        CHECK(l.nrows == 10 + 4000);
    l = run(db, "SELECT id FROM users WHERE id > 10;");          CHECK(l.nrows == 4000 && l.used_index);
    l = run(db, "SELECT id FROM users WHERE id = 4010;");        CHECK(l.nrows == 1);
    l = run(db, "SELECT id FROM users WHERE id = 4011;");        CHECK(l.nrows == 0);
    CHECK(bpt_validate(db->tables->index));
    db_close(db);
    db = db_open(g_dir);                                         /* 동시 쓰기 후에도 파일이 온전한지 재확인 */
    l = run(db, "SELECT id FROM users WHERE age = 499;");        CHECK(l.ok && l.nrows == 8);
    db_close(db);

    char cmd[128]; snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (system(cmd)) return 1;
    TEST_END();
}
