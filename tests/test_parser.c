#include "parser.h"
#include "test.h"
#include <string.h>

static int parse1(const char *sql, Statement *st, char *err) { const char *c = sql; return parse_statement(&c, st, err, 256); }
static int fails(const char *sql) { Statement st; char err[256] = ""; int rc = parse1(sql, &st, err); if (rc == 1) stmt_free(&st); return rc == -1 && err[0]; }

int main(void) {
    Statement st; char err[256];

    /* INSERT 기본형 */
    CHECK(parse1("INSERT INTO Users VALUES ('kim', 25);", &st, err) == 1);
    CHECK(st.type == STMT_INSERT && !strcmp(st.table, "users") && st.nrows == 1 && st.nvals == 2 && st.ncols == 0);
    CHECK(st.vals[0].type == VAL_STR && !strcmp(st.vals[0].s, "kim") && st.vals[1].type == VAL_INT && st.vals[1].i == 25);
    stmt_free(&st);

    /* 컬럼 목록 + 여러 행 + 소문자 키워드 + 주석 + 음수 + 따옴표 이스케이프 */
    CHECK(parse1("-- c\ninsert into t (b, a)\n values ('it''s, ok', -7), ('', 0)", &st, err) == 1);
    CHECK(st.ncols == 2 && !strcmp(st.cols[0], "b") && st.nrows == 2 && st.nvals == 2);
    CHECK(!strcmp(st.vals[0].s, "it's, ok") && st.vals[1].i == -7 && !strcmp(st.vals[2].s, "") && st.vals[3].i == 0);
    stmt_free(&st);

    /* SELECT 변형 */
    CHECK(parse1("SELECT * FROM users;", &st, err) == 1);
    CHECK(st.type == STMT_SELECT && st.star && !st.has_where && st.limit == -1);
    stmt_free(&st);
    CHECK(parse1("select id , name from users where id=42 limit 5", &st, err) == 1);
    CHECK(!st.star && st.ncols == 2 && st.has_where && !strcmp(st.where_col, "id") && st.where_op == OP_EQ && st.where_val.i == 42 && st.limit == 5);
    stmt_free(&st);
    struct { const char *op; CmpOp e; } ops[] = {{"=", OP_EQ}, {"!=", OP_NE}, {"<>", OP_NE}, {"<", OP_LT}, {"<=", OP_LE}, {">", OP_GT}, {">=", OP_GE}};
    for (int i = 0; i < 7; i++) {
        char sql[64]; snprintf(sql, sizeof sql, "SELECT * FROM t WHERE a %s 'x'", ops[i].op);
        CHECK(parse1(sql, &st, err) == 1 && st.where_op == ops[i].e && st.where_val.type == VAL_STR);
        stmt_free(&st);
    }
    CHECK(parse1("SELECT * FROM t WHERE a=9223372036854775807", &st, err) == 1 && st.where_val.i == INT64_MAX);
    stmt_free(&st);

    /* 여러 문장 + 빈 문장 + 오류 후 복구 */
    const char *cur = ";; SELECT * FROM a; BOGUS x y; SELECT * FROM b ;  ";
    CHECK(parse_statement(&cur, &st, err, sizeof err) == 1 && !strcmp(st.table, "a")); stmt_free(&st);
    CHECK(parse_statement(&cur, &st, err, sizeof err) == -1 && strstr(err, "unsupported"));
    CHECK(parse_statement(&cur, &st, err, sizeof err) == 1 && !strcmp(st.table, "b")); stmt_free(&st);
    CHECK(parse_statement(&cur, &st, err, sizeof err) == 0);
    cur = "";            CHECK(parse_statement(&cur, &st, err, sizeof err) == 0);
    cur = "  -- only\n"; CHECK(parse_statement(&cur, &st, err, sizeof err) == 0);

    /* 문법 오류들 */
    CHECK(fails("INSERT users VALUES (1)"));
    CHECK(fails("INSERT INTO users VALUES"));
    CHECK(fails("INSERT INTO users VALUES ()"));
    CHECK(fails("INSERT INTO users VALUES (1,)"));
    CHECK(fails("INSERT INTO users VALUES (1, 'a'"));
    CHECK(fails("INSERT INTO users VALUES (1), (1, 2)"));          /* 행마다 값 개수 다름 */
    CHECK(fails("INSERT INTO users (a, b) VALUES (1)"));           /* 컬럼 수 != 값 수 */
    CHECK(fails("INSERT INTO users VALUES ('unterminated)"));
    CHECK(fails("INSERT INTO users VALUES (99999999999999999999)"));
    CHECK(fails("INSERT INTO users VALUES (1.5)"));
    CHECK(fails("INSERT INTO users VALUES (abc)"));
    CHECK(fails("SELECT FROM users"));
    CHECK(fails("SELECT * users"));
    CHECK(fails("SELECT * FROM"));
    CHECK(fails("SELECT a, FROM users"));
    CHECK(fails("SELECT * FROM users WHERE"));
    CHECK(fails("SELECT * FROM users WHERE id"));
    CHECK(fails("SELECT * FROM users WHERE id = "));
    CHECK(fails("SELECT * FROM users WHERE id == 1"));
    CHECK(fails("SELECT * FROM users LIMIT -1"));
    CHECK(fails("SELECT * FROM users extra"));
    CHECK(fails("SELECT * FROM ../etc"));
    CHECK(fails("DELETE FROM users"));
    CHECK(fails("@"));
    char longid[200]; memset(longid, 'a', 100); strcpy(longid + 100, ";");
    char sql[300]; snprintf(sql, sizeof sql, "SELECT * FROM %s", longid);
    CHECK(fails(sql));
    TEST_END();
}
