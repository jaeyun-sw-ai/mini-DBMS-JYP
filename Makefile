CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -pthread -Isrc
ENGINE   = src/util.c src/bptree.c src/parser.c src/db.c
HDRS     = $(wildcard src/*.h) tests/test.h
BIN      = minidb minidb-server bench
TESTS    = build/test_bptree build/test_parser build/test_db build/test_threadpool

all: $(BIN)

minidb: src/main_cli.c $(ENGINE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ src/main_cli.c $(ENGINE)
minidb-server: src/server.c src/threadpool.c $(ENGINE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ src/server.c src/threadpool.c $(ENGINE)
bench: src/bench.c $(ENGINE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ src/bench.c $(ENGINE)

# 단위 테스트. B+ 트리 테스트는 노드 크기를 4로 줄여 분할이 아주 자주 일어나게 한다.
build/test_bptree: tests/test_bptree.c src/bptree.c src/util.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -DBPT_MAX_KEYS=4 -o $@ tests/test_bptree.c src/bptree.c src/util.c
build/test_parser: tests/test_parser.c src/parser.c src/util.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_parser.c src/parser.c src/util.c
build/test_db: tests/test_db.c $(ENGINE) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_db.c $(ENGINE)
build/test_threadpool: tests/test_threadpool.c src/threadpool.c src/util.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_threadpool.c src/threadpool.c src/util.c

unit: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done
functional: all
	@sh tests/functional_cli.sh
	@python3 tests/functional_server.py
test: unit functional
	@echo "ALL TESTS PASSED"

benchmark: bench
	./bench -n 1000000

clean:
	rm -rf $(BIN) build bench_data

.PHONY: all unit functional test benchmark clean
