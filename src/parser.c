#include "parser.h"
#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ───────────── 1단계: 렉서 (글자 → 토큰) ───────────── */
typedef enum { T_EOF, T_IDENT, T_INT, T_STR, T_SYM, T_OP, T_ERR } TokType;
typedef struct {
    const char *p;       /* 다음에 읽을 위치 */
    TokType type;
    StrBuf text;         /* IDENT / STR 내용 */
    int64_t ival;        /* INT 값 */
    CmpOp op;            /* OP 종류 */
    char sym;            /* SYM 글자: , ( ) * ; */
    char err[128];
} Lexer;

static void lex_next(Lexer *lx) {
    const char *p = lx->p;
    lx->text.len = 0; lx->text.data[0] = 0;
    for (;;) {                                           /* 공백과 -- 주석 건너뛰기 */
        while (isspace((unsigned char)*p)) p++;
        if (p[0] == '-' && p[1] == '-') { while (*p && *p != '\n') p++; continue; }
        break;
    }
    unsigned char c = (unsigned char)*p;
    if (!c) { lx->type = T_EOF; lx->p = p; return; }
    if (isalpha(c) || c == '_') {
        while (isalnum((unsigned char)*p) || *p == '_') sb_putc(&lx->text, *p++);
        lx->p = p;
        if (lx->text.len > MAX_IDENT) { lx->type = T_ERR; snprintf(lx->err, sizeof lx->err, "identifier too long (max %d)", MAX_IDENT); return; }
        lx->type = T_IDENT; return;
    }
    if (isdigit(c) || (c == '-' && isdigit((unsigned char)p[1]))) {
        char *end; errno = 0;
        long long v = strtoll(p, &end, 10);
        lx->p = end;
        if (errno == ERANGE) { lx->type = T_ERR; snprintf(lx->err, sizeof lx->err, "integer out of range"); return; }
        if (isalpha((unsigned char)*end) || *end == '_' || *end == '.') {
            lx->p = end + 1; lx->type = T_ERR; snprintf(lx->err, sizeof lx->err, "invalid number (only integers are supported)"); return;
        }
        lx->type = T_INT; lx->ival = v; return;
    }
    if (c == '\'') {                                     /* 문자열: '' 는 작은따옴표 하나 */
        p++;
        for (;;) {
            if (!*p) { lx->p = p; lx->type = T_ERR; snprintf(lx->err, sizeof lx->err, "unterminated string literal"); return; }
            if (*p == '\'') { if (p[1] == '\'') { sb_putc(&lx->text, '\''); p += 2; continue; } p++; break; }
            sb_putc(&lx->text, *p++);
        }
        lx->p = p; lx->type = T_STR; return;
    }
    if (strchr(",()*;", c)) { lx->type = T_SYM; lx->sym = (char)c; lx->p = p + 1; return; }
    lx->type = T_OP;
    if (c == '=') { lx->op = OP_EQ; lx->p = p + 1; return; }
    if (c == '!' && p[1] == '=') { lx->op = OP_NE; lx->p = p + 2; return; }
    if (c == '<' && p[1] == '>') { lx->op = OP_NE; lx->p = p + 2; return; }
    if (c == '<' && p[1] == '=') { lx->op = OP_LE; lx->p = p + 2; return; }
    if (c == '>' && p[1] == '=') { lx->op = OP_GE; lx->p = p + 2; return; }
    if (c == '<') { lx->op = OP_LT; lx->p = p + 1; return; }
    if (c == '>') { lx->op = OP_GT; lx->p = p + 1; return; }
    lx->type = T_ERR; lx->p = p + 1;
    if (isprint(c)) snprintf(lx->err, sizeof lx->err, "unexpected character '%c'", c);
    else snprintf(lx->err, sizeof lx->err, "unexpected byte 0x%02x", c);
}

/* ───────────── 2단계: 파서 (토큰 → Statement) ───────────── */
static int is_kw(const Lexer *lx, const char *kw) { return lx->type == T_IDENT && strcasecmp(lx->text.data, kw) == 0; }
static int is_sym(const Lexer *lx, char s) { return lx->type == T_SYM && lx->sym == s; }

static void describe(const Lexer *lx, char *out, size_t n) {
    switch (lx->type) {
    case T_EOF: snprintf(out, n, "end of input"); break;
    case T_IDENT: snprintf(out, n, "'%.40s'", lx->text.data); break;
    case T_INT: snprintf(out, n, "'%lld'", (long long)lx->ival); break;
    case T_STR: snprintf(out, n, "string '%.40s'", lx->text.data); break;
    case T_SYM: snprintf(out, n, "'%c'", lx->sym); break;
    case T_OP: snprintf(out, n, "operator"); break;
    default: snprintf(out, n, "invalid token"); break;
    }
}

/* 기대한 토큰이 아니면 "expected X but got Y" 오류를 만들고 fail로 점프 */
#define FAIL_EXPECT(what) do { \
        if (lx.type == T_ERR) snprintf(err, errsz, "syntax error: %s", lx.err); \
        else { char d_[64]; describe(&lx, d_, sizeof d_); snprintf(err, errsz, "syntax error: expected %s but got %s", what, d_); } \
        goto fail; } while (0)
#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); goto fail; } while (0)

static char *ident_dup(const Lexer *lx, int lower) {
    char *s = xstrdup(lx->text.data);
    if (lower) for (char *q = s; *q; q++) *q = (char)tolower((unsigned char)*q);
    return s;
}
static void push_col(Statement *st, char *name) {
    st->cols = xrealloc(st->cols, (size_t)(st->ncols + 1) * sizeof(char *));
    st->cols[st->ncols++] = name;
}

int parse_statement(const char **cursor, Statement *st, char *err, size_t errsz) {
    Lexer lx; memset(&lx, 0, sizeof lx);
    lx.p = *cursor; sb_init(&lx.text);
    memset(st, 0, sizeof *st); st->limit = -1;
    size_t nvals_total = 0, vals_cap = 0;

    lex_next(&lx);
    while (is_sym(&lx, ';')) lex_next(&lx);              /* 빈 문장(;;) 무시 */
    if (lx.type == T_EOF) { *cursor = lx.p; sb_free(&lx.text); return 0; }

    if (is_kw(&lx, "insert")) {
        st->type = STMT_INSERT;
        lex_next(&lx); if (!is_kw(&lx, "into")) FAIL_EXPECT("INTO");
        lex_next(&lx); if (lx.type != T_IDENT) FAIL_EXPECT("table name");
        st->table = ident_dup(&lx, 1);
        lex_next(&lx);
        if (is_sym(&lx, '(')) {                          /* 선택: 컬럼 목록 */
            do {
                lex_next(&lx); if (lx.type != T_IDENT) FAIL_EXPECT("column name");
                push_col(st, ident_dup(&lx, 0));
                lex_next(&lx);
            } while (is_sym(&lx, ','));
            if (!is_sym(&lx, ')')) FAIL_EXPECT("')'");
            lex_next(&lx);
        }
        if (!is_kw(&lx, "values")) FAIL_EXPECT("VALUES");
        do {                                             /* (v, ...), (v, ...) */
            lex_next(&lx); if (!is_sym(&lx, '(')) FAIL_EXPECT("'('");
            int n = 0;
            do {
                lex_next(&lx);
                if (lx.type != T_INT && lx.type != T_STR) FAIL_EXPECT("a value (integer or 'string')");
                if (nvals_total == vals_cap) { vals_cap = vals_cap ? vals_cap * 2 : 8; st->vals = xrealloc(st->vals, vals_cap * sizeof(Value)); }
                Value *v = &st->vals[nvals_total++];
                v->s = NULL; v->i = 0;
                if (lx.type == T_INT) { v->type = VAL_INT; v->i = lx.ival; }
                else { v->type = VAL_STR; v->s = xstrdup(lx.text.data); }
                n++;
                lex_next(&lx);
            } while (is_sym(&lx, ','));
            if (!is_sym(&lx, ')')) FAIL_EXPECT("')'");
            if (st->nrows == 0) st->nvals = n;
            else if (n != st->nvals) FAIL("syntax error: row %d has %d values but the first row has %d", st->nrows + 1, n, st->nvals);
            st->nrows++;
            lex_next(&lx);
        } while (is_sym(&lx, ','));
        if (st->ncols && st->ncols != st->nvals) FAIL("syntax error: %d columns listed but %d values given", st->ncols, st->nvals);
    } else if (is_kw(&lx, "select")) {
        st->type = STMT_SELECT;
        lex_next(&lx);
        if (is_sym(&lx, '*')) { st->star = 1; lex_next(&lx); }
        else {
            for (;;) {
                if (lx.type != T_IDENT || is_kw(&lx, "from")) FAIL_EXPECT("'*' or column name");
                push_col(st, ident_dup(&lx, 0));
                lex_next(&lx);
                if (!is_sym(&lx, ',')) break;
                lex_next(&lx);
            }
        }
        if (!is_kw(&lx, "from")) FAIL_EXPECT("FROM");
        lex_next(&lx); if (lx.type != T_IDENT) FAIL_EXPECT("table name");
        st->table = ident_dup(&lx, 1);
        lex_next(&lx);
        if (is_kw(&lx, "where")) {
            lex_next(&lx); if (lx.type != T_IDENT) FAIL_EXPECT("column name");
            st->where_col = ident_dup(&lx, 0);
            lex_next(&lx); if (lx.type != T_OP) FAIL_EXPECT("comparison operator");
            st->where_op = lx.op;
            lex_next(&lx);
            if (lx.type == T_INT) { st->where_val.type = VAL_INT; st->where_val.i = lx.ival; }
            else if (lx.type == T_STR) { st->where_val.type = VAL_STR; st->where_val.s = xstrdup(lx.text.data); }
            else FAIL_EXPECT("a value (integer or 'string')");
            st->has_where = 1;
            lex_next(&lx);
        }
        if (is_kw(&lx, "limit")) {
            lex_next(&lx);
            if (lx.type != T_INT || lx.ival < 0) FAIL_EXPECT("a non-negative integer after LIMIT");
            st->limit = lx.ival;
            lex_next(&lx);
        }
    } else {
        if (lx.type == T_ERR) FAIL("syntax error: %s", lx.err);
        char d[64]; describe(&lx, d, sizeof d);
        FAIL("unsupported statement starting with %s (only INSERT and SELECT are supported)", d);
    }

    if (!is_sym(&lx, ';') && lx.type != T_EOF) FAIL_EXPECT("';' or end of statement");
    *cursor = lx.p;
    sb_free(&lx.text);
    return 1;

fail:
    /* 오류 복구: 다음 ';' 까지 버리고, 그 뒤 문장부터 다시 읽을 수 있게 한다 */
    while (lx.type != T_EOF && !is_sym(&lx, ';')) lex_next(&lx);
    *cursor = lx.p;
    sb_free(&lx.text);
    st->nvals = (int)nvals_total; st->nrows = nvals_total ? 1 : 0;   /* stmt_free가 지금까지 만든 값만 해제하도록 */
    stmt_free(st);
    return -1;
}

void stmt_free(Statement *st) {
    free(st->table);
    for (int i = 0; i < st->ncols; i++) free(st->cols[i]);
    free(st->cols);
    size_t n = (size_t)st->nrows * (size_t)st->nvals;
    for (size_t i = 0; i < n; i++) free(st->vals[i].s);
    free(st->vals);
    free(st->where_col);
    free(st->where_val.s);
    memset(st, 0, sizeof *st);
}
