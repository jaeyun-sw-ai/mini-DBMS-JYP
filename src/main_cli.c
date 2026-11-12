/* minidb: SQL 처리기 CLI
 *   ./minidb [-d 데이터디렉터리] query.sql [more.sql ...]   SQL 파일 실행
 *   ./minidb [-d 데이터디렉터리]                            표준 입력에서 읽기(터미널이면 대화형) */
#include "db.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_result(const Result *r, void *ud) {
    (void)ud;
    result_print(r, r->ok ? stdout : stderr);
}

static char *read_all(FILE *f) {
    StrBuf sb; sb_init(&sb);
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_append(&sb, buf, n);
    return sb.data;
}

int main(int argc, char **argv) {
    const char *dir = "data";
    int opt;
    while ((opt = getopt(argc, argv, "d:h")) != -1) {
        if (opt == 'd') dir = optarg;
        else { fprintf(stderr, "usage: %s [-d datadir] [file.sql ...]\n", argv[0]); return opt == 'h' ? 0 : 2; }
    }
    Database *db = db_open(dir);
    if (!db) { fprintf(stderr, "ERROR: data directory '%s' not found\n", dir); return 2; }
    int errors = 0;

    if (optind < argc) {
        for (int i = optind; i < argc; i++) {
            FILE *f = fopen(argv[i], "rb");
            if (!f) { fprintf(stderr, "ERROR: cannot open '%s'\n", argv[i]); errors++; continue; }
            char *sql = read_all(f);
            fclose(f);
            if (strlen(sql) == 0) fprintf(stderr, "warning: '%s' is empty\n", argv[i]);
            errors += db_exec_script(db, sql, print_result, NULL);
            free(sql);
        }
    } else if (isatty(STDIN_FILENO)) {                 /* 대화형: ';' 가 나올 때까지 줄을 모았다가 실행 */
        StrBuf acc; sb_init(&acc);
        char line[4096];
        printf("minidb> "); fflush(stdout);
        while (fgets(line, sizeof line, stdin)) {
            if (acc.len == 0 && (!strncmp(line, "exit", 4) || !strncmp(line, "quit", 4))) break;
            sb_puts(&acc, line);
            if (strchr(line, ';')) { db_exec_script(db, acc.data, print_result, NULL); acc.len = 0; acc.data[0] = 0; }
            printf(acc.len ? "   ...> " : "minidb> "); fflush(stdout);
        }
        if (acc.len) db_exec_script(db, acc.data, print_result, NULL);
        sb_free(&acc);
    } else {
        char *sql = read_all(stdin);
        errors += db_exec_script(db, sql, print_result, NULL);
        free(sql);
    }
    db_close(db);
    return errors ? 1 : 0;
}
