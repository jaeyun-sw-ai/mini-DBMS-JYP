# minidb — 미니 DBMS (C)

SQL 처리기(Phase 1) + B+ 트리 인덱스(Phase 2) + 스레드 풀 API 서버(Phase 3).
외부 라이브러리 없이 C11 + pthread + POSIX 소켓만 사용합니다.

## 한눈에 보는 구조

```
 [Phase 1] ./minidb query.sql ─────┐
                                   │ SQL 문자열
 [Phase 3] HTTP 클라이언트         ▼
    │ POST /query            ┌───────────────────────── DB 엔진 (db_exec_script) ─────────────────────────┐
    ▼                        │                                                                            │
 accept(main 스레드)         │  parser.c            db.c 실행기                  db.c 저장소              │
    │ fd                     │  글자→토큰→Statement  INSERT / SELECT             data/<table>.csv         │
    ▼                        │                         │                          ▲   ▲                    │
 작업 큐 ─▶ 워커 스레드 N개 ─▶│                         │ WHERE id ...  ──▶ [Phase 2] bptree.c ─ 오프셋 ─┘   │
 (threadpool.c)              │                         │                  id → 파일 오프셋 (pread 1번)     │
                             │                         └ 그 외 조건 ────▶ 파일 전체 선형 탐색 ─────────────┘
                             └────────────────────────────────────────────────────────────────────────────┘
```

| 파일 | 역할 |
|---|---|
| `src/parser.c` | 렉서 + 파서. SQL 텍스트 → `Statement` |
| `src/db.c` | 테이블 열기(스키마 읽기, 인덱스 재구축), INSERT/SELECT 실행, 결과 출력(표/JSON) |
| `src/bptree.c` | 메모리 기반 B+ 트리 (삽입, 단건 검색, 범위 검색) |
| `src/main_cli.c` | CLI (`minidb`) |
| `src/threadpool.c`, `src/server.c` | 스레드 풀, HTTP API 서버 (`minidb-server`) |
| `src/bench.c` | 100만 건 삽입 + 속도 비교 (`bench`) |
| `tests/` | 단위 테스트 4개 + 기능 테스트 2개 |

## 빌드와 실행

```sh
make            # minidb, minidb-server, bench
make test       # 단위 테스트 + 기능 테스트 전부
make benchmark  # 100만 건 성능 비교
```

테이블은 이미 있다고 가정합니다. `data/<테이블>.schema` 파일 한 줄이 스키마입니다(첫 컬럼은 반드시 `id:int`).

```
data/users.schema :  id:int,name:string,age:int
```

### Phase 1 — CLI

```sh
./minidb data/sample.sql          # SQL 파일 실행 (-d 로 데이터 디렉터리 지정, 기본 data/)
echo "SELECT * FROM users;" | ./minidb
./minidb                          # 대화형
```

지원 SQL

```sql
INSERT INTO users VALUES ('kim', 25);                       -- id는 자동 부여
INSERT INTO users (age, name) VALUES (31, 'lee'), (28, 'park');
SELECT * FROM users;
SELECT name, age FROM users WHERE id = 2;                   -- B+ 트리
SELECT * FROM users WHERE age >= 28 LIMIT 10;               -- 선형 탐색
```

WHERE 연산자: `= != <> < <= > >=` (조건 1개). 타입: `int`(64비트 정수), `string`.

### Phase 2 — 인덱스와 성능 비교

- INSERT 때 `next_id`를 부여하고 `(id → 그 행의 파일 오프셋)`을 B+ 트리에 등록합니다.
- `WHERE id =, <, <=, >, >=` → B+ 트리로 오프셋을 찾아 그 행만 읽습니다. 범위 조건은 리프 연결 리스트를 따라갑니다.
- 그 외 컬럼(또는 `id !=`) → 파일 전체를 읽으며 비교합니다.
- 결과에 어떤 경로를 썼는지 표시됩니다(CLI: `index` / `full scan`, API: `"access"`).
- 프로그램을 다시 켜면 데이터 파일을 한 번 훑어 B+ 트리를 다시 만듭니다(인덱스는 메모리에만 존재).

측정 결과 (`./bench -n 1000000`, 이 저장소를 만든 환경 기준):

| 항목 | 결과 |
|---|---|
| 100만 건 INSERT (SQL 처리기 경유) | 약 0.84초 |
| `WHERE id = ?` (B+ 트리) | 평균 0.0024 ms |
| `WHERE name = ?` (선형 탐색) | 평균 52 ms |
| 차이 | 약 2만 배 |
| 재시작 시 인덱스 재구축 | 약 0.09초 |

CLI로 직접 넣어 보려면: `./bench -g bulk.sql -n 1000000` 으로 SQL 파일을 만든 뒤 `./minidb -d <디렉터리> bulk.sql`
(스키마는 `id:int,name:string,age:int,email:string`).

### Phase 3 — API 서버

```sh
./minidb-server -p 8080 -t 8 -q 256 -d data     # 포트, 워커 수, 큐 크기, 데이터 디렉터리
curl -X POST localhost:8080/query -d "INSERT INTO users VALUES ('kim', 25);"
curl -X POST localhost:8080/query -d "SELECT * FROM users WHERE id = 1;"
curl localhost:8080/health
```

```json
{"results":[{"ok":true,"type":"select","columns":["id","name","age"],
             "rows":[[1,"kim",25]],"row_count":1,"access":"index","elapsed_ms":0.004}],"ok":true}
```

| 상황 | 상태 코드 |
|---|---|
| 모든 문장 성공 | 200 |
| SQL 오류(문법/없는 테이블 등), 빈 본문 | 400 (문장별 `error` 포함) |
| 없는 경로 / 잘못된 메서드 | 404 / 405 |
| Content-Length 없음 / 본문 8MB 초과 | 411 / 413 |
| 요청을 5초 안에 다 안 보냄 | 408 |
| 작업 큐가 가득 참 | 503 |

## 동시성 설계

| 보호 대상 | 락 | 이유 |
|---|---|---|
| 테이블 1개(파일 끝 위치, next_id, B+ 트리) | 테이블별 읽기-쓰기 락 | SELECT끼리는 동시에, INSERT는 혼자. "id 부여 → 파일 쓰기 → 인덱스 등록"이 한 덩어리로 보임 |
| 열린 테이블 목록 | 뮤텍스 | 두 스레드가 같은 테이블을 동시에 처음 여는 것을 방지 |
| 작업 큐 | 뮤텍스 + 조건 변수 | 워커는 일이 없으면 잠들고, 큐가 가득 차면 새 연결은 즉시 503 |
| 데이터 파일(프로세스 간) | `flock` | 서버가 켜진 상태에서 CLI가 같은 테이블을 쓰면 인덱스가 어긋나므로 거부 |

파일 읽기는 `pread`(위치를 인자로 받음)만 써서 스레드끼리 파일 위치를 공유하지 않습니다.

## 처리한 엣지 케이스

- 값 안의 쉼표·역슬래시·줄바꿈·작은따옴표(`''`)·빈 문자열·한글
- 여러 행 INSERT 중 하나라도 타입이 틀리면 전체 거부, 쓰기 실패 시 되돌림
- 문법 오류가 난 문장만 건너뛰고 다음 문장 계속 실행
- 쓰다가 죽어서 줄바꿈 없이 끝난 마지막 행 → 다시 열 때 잘라냄
- 손상된 데이터 파일, 중복 id, 잘못된 스키마 → 열기 거부
- 정수 범위 초과, `id > INT64_MAX` 같은 경계, 테이블 이름으로 경로 탈출(`../`) 불가
- 느린/말 없는 클라이언트 타임아웃, 연결만 하고 끊는 클라이언트, `Expect: 100-continue`

## 일부러 만들지 않은 것

CREATE TABLE, UPDATE/DELETE, NULL, AND/OR, JOIN, 트랜잭션, 디스크 기반 인덱스, HTTP keep-alive.
`fsync`를 호출하지 않으므로 정전 시 마지막 몇 건은 유실될 수 있습니다.

## 스스로 확인 (서술형)

1. `SELECT * FROM users WHERE id = 7;` 한 줄이 결과로 나오기까지 거치는 함수를 순서대로 쓰고, 각 함수가 넘겨주는 것을 한 단어씩 적어 보세요.
2. B+ 트리의 value에 행 전체가 아니라 "파일 오프셋"만 저장했습니다. 이 선택이 아끼는 것은 무엇이고, 그 비용은 언제 누가 치르나요?
3. 리프 분할은 key를 위로 "복사"하고 내부 노드 분할은 "이동"합니다. 리프에서도 이동시키면 어떤 검색이 깨지나요?
4. `exec_insert`에서 타입 검사는 락 밖에서, id 부여는 락 안에서 합니다. id 부여를 락 밖으로 빼면 어떤 순서로 실행될 때 무엇이 잘못되나요?
5. 스레드 풀 워커가 `task.fn(task.arg)`를 뮤텍스를 쥔 채로 호출하도록 바꾸면 서버는 어떻게 동작하나요?
6. "스레드 풀"과 "읽기-쓰기 락"은 같은 축의 개념인가요, 다른 축인가요? 각각 무엇을 제한하나요?
