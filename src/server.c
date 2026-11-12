/* minidb-server: DB 엔진을 HTTP API로 감싼 서버
 *   POST /query   본문 = SQL 텍스트(여러 문장 가능) → JSON 결과
 *   GET  /health  → {"status":"ok"}
 *
 * 구조: main 스레드는 accept만 → 연결(fd)을 작업 큐에 넣음 → 스레드 풀의 워커가 꺼내서 처리 */
#include "db.h"
#include "threadpool.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_HEADER (16 * 1024)
#define MAX_BODY   (8 * 1024 * 1024)

static Database *g_db;
static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int send_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        buf += n; len -= (size_t)n;
    }
    return 0;
}
static void respond(int fd, int status, const char *reason, const char *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, reason, len);
    if (send_all(fd, head, (size_t)n) == 0) send_all(fd, body, len);
}
static void respond_error(int fd, int status, const char *reason, const char *msg) {
    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "{\"ok\":false,\"error\":"); sb_json_str(&sb, msg); sb_putc(&sb, '}');
    respond(fd, status, reason, sb.data, sb.len);
    sb_free(&sb);
}

/* 헤더에서 name의 값을 찾는다(대소문자 무시). 없으면 NULL */
static const char *find_header(const char *headers, const char *name) {
    size_t nl = strlen(name);
    for (const char *p = strstr(headers, "\r\n"); p; p = strstr(p, "\r\n")) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') { p += nl + 1; while (*p == ' ' || *p == '\t') p++; return p; }
    }
    return NULL;
}

typedef struct { StrBuf *sb; int count; } JsonCtx;
static void json_result(const Result *r, void *ud) {
    JsonCtx *c = ud;
    if (c->count++) sb_putc(c->sb, ',');
    result_to_json(r, c->sb);
}

static void handle_client(void *arg) {
    int fd = (int)(intptr_t)arg;
    struct timeval tv = {5, 0};                          /* 느리거나 말이 없는 클라이언트가 워커를 붙잡지 못하게 */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    StrBuf req; sb_init(&req);
    char buf[8192], *hdr_end = NULL;
    /* 1) 헤더 끝(빈 줄)까지 읽기 */
    while (!(hdr_end = strstr(req.data, "\r\n\r\n"))) {
        if (req.len > MAX_HEADER) { respond_error(fd, 431, "Request Header Fields Too Large", "headers too large"); goto done; }
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) goto done;                           /* 아무것도 안 보내고 끊음 */
        if (n < 0) { respond_error(fd, 408, "Request Timeout", "timed out waiting for the request"); goto done; }
        if (memchr(buf, 0, (size_t)n)) { respond_error(fd, 400, "Bad Request", "request contains a NUL byte"); goto done; }
        sb_append(&req, buf, (size_t)n);
    }
    size_t hdr_len = (size_t)(hdr_end - req.data) + 4;

    /* 2) 요청 줄: METHOD PATH VERSION */
    char method[8], path[256];
    if (sscanf(req.data, "%7s %255s", method, path) != 2) { respond_error(fd, 400, "Bad Request", "malformed request line"); goto done; }
    char *q = strchr(path, '?'); if (q) *q = 0;

    if (!strcmp(path, "/health")) {
        if (strcmp(method, "GET")) respond_error(fd, 405, "Method Not Allowed", "use GET /health");
        else respond(fd, 200, "OK", "{\"status\":\"ok\"}", 15);
        goto done;
    }
    if (strcmp(path, "/query")) { respond_error(fd, 404, "Not Found", "unknown path (use POST /query or GET /health)"); goto done; }
    if (strcmp(method, "POST")) { respond_error(fd, 405, "Method Not Allowed", "use POST /query with the SQL text as the body"); goto done; }

    /* 3) 본문 길이 확인 후 본문 읽기 */
    hdr_end[2] = 0;                                      /* 헤더 구간만 검색하도록 잠깐 끊는다 */
    const char *cl = find_header(req.data, "Content-Length");
    int expect_continue = find_header(req.data, "Expect") != NULL;
    hdr_end[2] = '\r';
    if (!cl) { respond_error(fd, 411, "Length Required", "Content-Length header is required"); goto done; }
    char *end; errno = 0;
    long long clen = strtoll(cl, &end, 10);
    if (errno || end == cl || clen < 0) { respond_error(fd, 400, "Bad Request", "invalid Content-Length"); goto done; }
    if (clen > MAX_BODY) { respond_error(fd, 413, "Payload Too Large", "request body too large (max 8 MB)"); goto done; }
    if (expect_continue) send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    while (req.len - hdr_len < (size_t)clen) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) { respond_error(fd, 400, "Bad Request", "body shorter than Content-Length"); goto done; }
        if (n < 0) { respond_error(fd, 408, "Request Timeout", "timed out waiting for the body"); goto done; }
        if (memchr(buf, 0, (size_t)n)) { respond_error(fd, 400, "Bad Request", "request contains a NUL byte"); goto done; }
        sb_append(&req, buf, (size_t)n);
    }
    char *sql = req.data + hdr_len;
    sql[clen] = 0;                                       /* 선언한 길이까지만 SQL로 본다 */

    /* 4) DB 엔진 호출 → JSON 응답 */
    StrBuf out; sb_init(&out);
    sb_puts(&out, "{\"results\":[");
    JsonCtx ctx = {&out, 0};
    int errors = db_exec_script(g_db, sql, json_result, &ctx);
    sb_printf(&out, "],\"ok\":%s}", errors ? "false" : "true");
    if (ctx.count == 0) respond_error(fd, 400, "Bad Request", "empty query: send SQL text as the request body");
    else if (errors) respond(fd, 400, "Bad Request", out.data, out.len);
    else respond(fd, 200, "OK", out.data, out.len);
    sb_free(&out);
done:
    sb_free(&req);
    close(fd);
}

int main(int argc, char **argv) {
    const char *dir = "data"; int port = 8080, nthreads = 8, qcap = 256, opt;
    while ((opt = getopt(argc, argv, "d:p:t:q:h")) != -1) {
        if (opt == 'd') dir = optarg;
        else if (opt == 'p') port = atoi(optarg);
        else if (opt == 't') nthreads = atoi(optarg);
        else if (opt == 'q') qcap = atoi(optarg);
        else { fprintf(stderr, "usage: %s [-d datadir] [-p port] [-t threads] [-q queue_size]\n", argv[0]); return opt == 'h' ? 0 : 2; }
    }
    if (port < 1 || port > 65535 || nthreads < 1 || nthreads > 1024 || qcap < 1) { fprintf(stderr, "ERROR: invalid option value\n"); return 2; }
    if (!(g_db = db_open(dir))) { fprintf(stderr, "ERROR: data directory '%s' not found\n", dir); return 2; }

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;                           /* SA_RESTART 없음 → accept가 EINTR로 깨어난다 */
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY); addr.sin_port = htons((uint16_t)port);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, 512) < 0) {
        fprintf(stderr, "ERROR: cannot listen on port %d: %s\n", port, strerror(errno)); return 1;
    }
    ThreadPool *tp = tp_create(nthreads, qcap);
    if (!tp) { fprintf(stderr, "ERROR: cannot create thread pool\n"); return 1; }
    fprintf(stderr, "minidb-server listening on :%d (threads=%d, queue=%d, data=%s)\n", port, nthreads, qcap, dir);

    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR || errno == ECONNABORTED) continue; perror("accept"); break; }
        if (tp_submit(tp, handle_client, (void *)(intptr_t)cfd) < 0) {   /* 큐가 가득 참 → 기다리게 하지 않고 바로 거절 */
            respond_error(cfd, 503, "Service Unavailable", "server is busy, try again");
            close(cfd);
        }
    }
    fprintf(stderr, "shutting down...\n");
    close(lfd);
    tp_destroy(tp);                                      /* 처리 중/대기 중인 요청을 끝낸 뒤 */
    db_close(g_db);
    return 0;
}
