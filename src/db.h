/* db: DB 엔진 = 저장소(테이블 파일) + 인덱스(B+ 트리) + 실행기(INSERT/SELECT)
 *
 *   data/<table>.schema : "id:int,name:string,age:int"  (미리 존재한다고 가정, 첫 컬럼은 반드시 id:int)
 *   data/<table>.csv    : 한 줄 = 한 레코드. "id,필드,필드\n"  (필드 안의 , \ 줄바꿈은 \ 로 이스케이프)
 *
 * 동시성: 테이블마다 읽기-쓰기 락 1개. SELECT는 여러 스레드가 동시에, INSERT는 혼자만. */
#ifndef DB_H
#define DB_H
#include "bptree.h"
#include "parser.h"
#include "util.h"
#include <pthread.h>
#include <stdio.h>

#define MAX_COLS 32

typedef enum { COL_INT, COL_STR } ColType;
typedef struct { char name[MAX_IDENT + 1]; ColType type; } Column;

typedef struct Table {
    char     name[MAX_IDENT + 1];
    Column   cols[MAX_COLS]; int ncols;
    int      fd;                 /* 데이터 파일 */
    int64_t  file_size;          /* 유효한 데이터의 끝 = 다음 행이 쓰일 오프셋 */
    int64_t  next_id;            /* 자동 부여될 다음 id */
    BPTree  *index;              /* id → 행의 파일 오프셋 */
    pthread_rwlock_t lock;
    struct Table *next;
} Table;

typedef struct {
    char   dir[1024];
    Table *tables;               /* 한 번 연 테이블 목록 (지연 로딩) */
    pthread_mutex_t mu;          /* 목록 보호 */
} Database;

typedef struct {
    int      ok; char err[256];
    StmtType type;
    /* SELECT 결과 */
    int      ncols; char **colnames; ColType *coltypes;
    char  ***rows; size_t nrows, cap;
    int      used_index;         /* 1 = B+ 트리 사용, 0 = 파일 선형 탐색 */
    /* INSERT 결과 */
    int64_t  affected, first_id, last_id;
    double   elapsed_ms;
} Result;

typedef void (*ResultCb)(const Result *r, void *ud);

Database *db_open(const char *dir);
void      db_close(Database *db);
void      db_exec_stmt(Database *db, const Statement *st, Result *r);                 /* 문장 1개 실행 */
int       db_exec_script(Database *db, const char *sql, ResultCb cb, void *ud);       /* 여러 문장 실행, 실패한 문장 수 반환 */
void      result_free(Result *r);
void      result_print(const Result *r, FILE *out);                                   /* CLI용 표 출력 */
void      result_to_json(const Result *r, StrBuf *sb);                                /* API용 JSON */
#endif
