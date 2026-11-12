#!/bin/sh
# Phase 1/2 기능 테스트: 실제 CLI 바이너리에 SQL 파일을 넣고 출력을 확인한다.
set -u
BIN="$(pwd)/minidb"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT
fail=0; n=0
check() { n=$((n+1)); if ! eval "$2"; then echo "FAIL functional_cli: $1"; fail=$((fail+1)); fi; }

printf 'id:int,name:string,age:int\n' > "$DIR/users.schema"

cat > "$DIR/a.sql" <<'SQL'
INSERT INTO users (name, age) VALUES ('kim', 25);
INSERT INTO users VALUES ('lee', 31), ('park', 28);
SELECT * FROM users;
SQL
out=$("$BIN" -d "$DIR" "$DIR/a.sql" 2>&1); rc=$?
check "exit code 0 on success"        '[ $rc -eq 0 ]'
check "insert reports id"             'echo "$out" | grep -q "INSERT 1 (id 1)"'
check "multi-row insert reports ids"  'echo "$out" | grep -q "INSERT 2 (id 2..3)"'
check "select prints 3 rows"          'echo "$out" | grep -q "(3 rows, full scan"'
check "data file has 3 lines"         '[ "$(wc -l < "$DIR/users.csv")" -eq 3 ]'

# 두 번째 실행: 파일에 저장된 데이터가 남아 있고, id 조회는 인덱스를 쓴다
printf "SELECT name FROM users WHERE id = 2;" > "$DIR/b.sql"
out=$("$BIN" -d "$DIR" "$DIR/b.sql" 2>&1)
check "persisted row found by id"     'echo "$out" | grep -q "^lee"'
check "id lookup uses index"          'echo "$out" | grep -q "(1 row, index"'

printf "SELECT name FROM users WHERE age = 28;" > "$DIR/c.sql"
out=$("$BIN" -d "$DIR" "$DIR/c.sql" 2>&1)
check "non-id lookup uses full scan"  'echo "$out" | grep -q "park" && echo "$out" | grep -q "full scan"'

# 오류가 섞인 파일: 오류는 보고하되 나머지 문장은 실행, 종료 코드는 1
printf "SELEC * FROM users;\nSELECT * FROM ghost;\nINSERT INTO users VALUES ('choi', 40);\n" > "$DIR/d.sql"
out=$("$BIN" -d "$DIR" "$DIR/d.sql" 2>&1); rc=$?
check "exit code 1 on error"          '[ $rc -eq 1 ]'
check "syntax error reported"         'echo "$out" | grep -q "ERROR: unsupported statement"'
check "missing table reported"        'echo "$out" | grep -q "does not exist"'
check "later statement still ran"     'echo "$out" | grep -q "INSERT 1 (id 4)"'

# 표준 입력, 빈 파일, 없는 파일, 없는 디렉터리
out=$(echo "SELECT id FROM users WHERE id >= 3;" | "$BIN" -d "$DIR" 2>&1)
check "stdin input works"             'echo "$out" | grep -q "(2 rows, index"'
: > "$DIR/empty.sql"
"$BIN" -d "$DIR" "$DIR/empty.sql" >/dev/null 2>&1; rc=$?; check "empty file is ok" '[ $rc -eq 0 ]'
"$BIN" -d "$DIR" "$DIR/nope.sql" >/dev/null 2>&1; rc=$?; check "missing file fails" '[ $rc -eq 1 ]'
"$BIN" -d "$DIR/nope" "$DIR/a.sql" >/dev/null 2>&1; rc=$?; check "missing data dir fails" '[ $rc -eq 2 ]'

# Phase 2: bench가 만든 SQL 파일을 CLI로 넣고 인덱스 조회 (2만 행)
mkdir "$DIR/bulk"; printf 'id:int,name:string,age:int,email:string\n' > "$DIR/bulk/users.schema"
"$(pwd)/bench" -g "$DIR/bulk.sql" -n 20000 >/dev/null
"$BIN" -d "$DIR/bulk" "$DIR/bulk.sql" >/dev/null 2>&1; rc=$?; check "bulk load via CLI" '[ $rc -eq 0 ]'
printf "SELECT name FROM users WHERE id = 20000; SELECT id FROM users WHERE email = 'user_777@example.com';" > "$DIR/e.sql"
out=$("$BIN" -d "$DIR/bulk" "$DIR/e.sql" 2>&1)
check "bulk row found via index"      'echo "$out" | grep -q "user_20000" && echo "$out" | grep -q "(1 row, index"'
check "bulk row found via scan"       'echo "$out" | grep -q "^777" && echo "$out" | grep -q "(1 row, full scan"'

printf "%-28s %5d checks, %d failed\n" "tests/functional_cli.sh" "$n" "$fail"
[ "$fail" -eq 0 ]
