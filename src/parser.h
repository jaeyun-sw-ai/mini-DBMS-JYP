/* parser: SQL 문자열 → Statement 구조체 (토큰화 + 문법 분석)
 * 지원 문법
 *   INSERT INTO t [(col, ...)] VALUES (v, ...)[, (v, ...) ...];
 *   SELECT * | col, ... FROM t [WHERE col (=|!=|<>|<|<=|>|>=) v] [LIMIT n]; */
#ifndef PARSER_H
#define PARSER_H
#include <stddef.h>
#include <stdint.h>

#define MAX_IDENT 64

typedef enum { STMT_INSERT, STMT_SELECT } StmtType;
typedef enum { VAL_INT, VAL_STR } ValType;
typedef enum { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE } CmpOp;
typedef struct { ValType type; int64_t i; char *s; } Value;

typedef struct {
    StmtType type;
    char  *table;            /* 소문자로 정규화된 테이블 이름 */
    char **cols; int ncols;  /* INSERT의 컬럼 목록(없으면 0) / SELECT의 조회 컬럼 */
    int    star;             /* SELECT * */
    Value *vals;             /* INSERT 값: nrows * nvals 개가 한 줄로 */
    int    nrows, nvals;
    int    has_where; char *where_col; CmpOp where_op; Value where_val;
    int64_t limit;           /* 없으면 -1 */
} Statement;

/* *cursor 위치에서 문장 하나를 읽고 cursor를 그 뒤로 옮긴다.
 * 반환: 1 = 성공, 0 = 더 읽을 문장 없음, -1 = 문법 오류(err에 메시지, cursor는 다음 ';' 뒤로 이동) */
int  parse_statement(const char **cursor, Statement *out, char *err, size_t errsz);
void stmt_free(Statement *st);
#endif
