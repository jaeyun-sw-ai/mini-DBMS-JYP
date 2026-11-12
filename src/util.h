/* util: 공용 도우미 (가변 문자열 버퍼, 안전한 malloc, 시간 측정) */
#ifndef UTIL_H
#define UTIL_H
#include <stddef.h>

typedef struct { char *data; size_t len, cap; } StrBuf;   /* data는 항상 '\0'으로 끝난다 */

void  *xmalloc(size_t n);
void  *xrealloc(void *p, size_t n);
char  *xstrdup(const char *s);

void   sb_init(StrBuf *sb);
void   sb_free(StrBuf *sb);
void   sb_append(StrBuf *sb, const char *s, size_t n);
void   sb_puts(StrBuf *sb, const char *s);
void   sb_putc(StrBuf *sb, char c);
void   sb_printf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void   sb_json_str(StrBuf *sb, const char *s);            /* "..." 형태로 JSON 이스케이프 */

double now_ms(void);
#endif
