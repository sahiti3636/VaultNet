CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -pthread -O2 -std=c11 -D_GNU_SOURCE
LDFLAGS := -pthread

# ── OpenSSL ───────────────────────────────────────────────────────────────
# Use pkg-config when available; fall back to brew --prefix openssl@3 on macOS.
UNAME := $(shell uname)
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS   := $(shell pkg-config --libs   openssl 2>/dev/null)
ifeq ($(OPENSSL_CFLAGS),)
  ifeq ($(UNAME), Darwin)
    OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null || echo /usr/local)
    OPENSSL_CFLAGS := -I$(OPENSSL_PREFIX)/include
    OPENSSL_LIBS   := -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
  else
    OPENSSL_LIBS   := -lssl -lcrypto
  endif
endif
CFLAGS  += $(OPENSSL_CFLAGS)
LDFLAGS += $(OPENSSL_LIBS)

SRCDIR  := src
OBJDIR  := obj
BINDIR  := bin

SERVER_SRCS := $(SRCDIR)/server.c \
               $(SRCDIR)/accounts.c \
               $(SRCDIR)/wal.c \
               $(SRCDIR)/auth.c \
               $(SRCDIR)/audit.c \
               $(SRCDIR)/threadpool.c \
               $(SRCDIR)/netutil.c \
               $(SRCDIR)/tlsutil.c

CLIENT_SRCS := $(SRCDIR)/client.c \
               $(SRCDIR)/netutil.c \
               $(SRCDIR)/tlsutil.c

SERVER_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SERVER_SRCS))
CLIENT_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(CLIENT_SRCS))

SERVER_BIN  := $(BINDIR)/vaultnet_server
CLIENT_BIN  := $(BINDIR)/vaultnet_client

UNIT_SRCS := tests/unit.c \
             $(SRCDIR)/accounts.c \
             $(SRCDIR)/wal.c \
             $(SRCDIR)/auth.c \
             $(SRCDIR)/audit.c \
             $(SRCDIR)/netutil.c

UNIT_BIN  := $(BINDIR)/vaultnet_unit

LOADGEN_SRCS := tests/loadgen.c
LOADGEN_BIN  := $(BINDIR)/vaultnet_loadgen

DEFAULT_PORT := 18080
TLS_CERT     := vaultnet.crt
TLS_KEY      := vaultnet.key

.PHONY: all server client clean tsan asan unit check loadgen bench \
        gen-cert tls-smoke

all: server client

server: $(SERVER_BIN)

client: $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

# ── sanitizer builds ──────────────────────────────────────────────────────

# ThreadSanitizer: detects data races and lock-order violations.
tsan:
	$(CC) -Wall -Wextra -Werror -pthread -O1 -g -std=c11 -D_GNU_SOURCE \
	    $(OPENSSL_CFLAGS) -fsanitize=thread \
	    -I$(SRCDIR) $(SERVER_SRCS) -o $(BINDIR)/vaultnet_server_tsan $(LDFLAGS) \
	    -fsanitize=thread
	$(CC) -Wall -Wextra -Werror -pthread -O1 -g -std=c11 -D_GNU_SOURCE \
	    $(OPENSSL_CFLAGS) -fsanitize=thread \
	    -I$(SRCDIR) $(CLIENT_SRCS) -o $(BINDIR)/vaultnet_client_tsan $(LDFLAGS) \
	    -fsanitize=thread

# AddressSanitizer + UBSan: detects memory errors and undefined behaviour.
asan:
	$(CC) -Wall -Wextra -Werror -pthread -O1 -g -std=c11 -D_GNU_SOURCE \
	    $(OPENSSL_CFLAGS) -fsanitize=address,undefined \
	    -I$(SRCDIR) $(SERVER_SRCS) -o $(BINDIR)/vaultnet_server_asan $(LDFLAGS)
	$(CC) -Wall -Wextra -Werror -pthread -O1 -g -std=c11 -D_GNU_SOURCE \
	    $(OPENSSL_CFLAGS) -fsanitize=address,undefined \
	    -I$(SRCDIR) $(CLIENT_SRCS) -o $(BINDIR)/vaultnet_client_asan $(LDFLAGS)

unit: $(UNIT_BIN)

$(UNIT_BIN): $(UNIT_SRCS) | $(BINDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) $(UNIT_SRCS) -o $@ $(LDFLAGS)

loadgen: $(LOADGEN_BIN)

$(LOADGEN_BIN): $(LOADGEN_SRCS) | $(BINDIR)
	$(CC) $(CFLAGS) $(LOADGEN_SRCS) -o $@ $(LDFLAGS)

# bench: start a fresh server, run loadgen for 12s, shut down.
bench: server loadgen
	@pkill -f "vaultnet_server $(DEFAULT_PORT)" 2>/dev/null; sleep 0.2; true
	@rm -f vaultnet.snap vaultnet.snap.tmp vaultnet.wal vaultnet_audit.log vaultnet.lock
	$(SERVER_BIN) $(DEFAULT_PORT) &
	@sleep 0.8
	$(LOADGEN_BIN) -h 127.0.0.1 -p $(DEFAULT_PORT) -t 12 -c 16 -w 2
	@pkill -f "vaultnet_server $(DEFAULT_PORT)" 2>/dev/null; true
	@rm -f vaultnet.snap vaultnet.snap.tmp vaultnet.wal vaultnet_audit.log vaultnet.lock

# gen-cert: generate a self-signed dev certificate (valid 10 years)
gen-cert:
	bash gen_cert.sh

# tls-smoke: start server with TLS, run a quick smoke test, shut down
tls-smoke: server gen-cert
	@pkill -f "vaultnet_server $(DEFAULT_PORT)" 2>/dev/null; sleep 0.2; true
	@rm -f vaultnet.snap vaultnet.snap.tmp vaultnet.wal vaultnet_audit.log vaultnet.lock
	$(SERVER_BIN) $(DEFAULT_PORT) $(TLS_CERT) $(TLS_KEY) &
	@sleep 0.8
	bash tests/tls_smoke.sh $(DEFAULT_PORT) $(TLS_CERT)
	@pkill -f "vaultnet_server $(DEFAULT_PORT)" 2>/dev/null; true
	@rm -f vaultnet.snap vaultnet.snap.tmp vaultnet.wal vaultnet_audit.log vaultnet.lock

check: unit
	@echo "--- unit tests ---"
	./$(UNIT_BIN)
	@echo "--- concurrency test ---"
	bash test_concurrency.sh
	@echo "--- crash recovery test ---"
	bash tests/crash_test.sh

clean:
	rm -rf $(OBJDIR) $(BINDIR) \
	    vaultnet.snap vaultnet.snap.tmp \
	    vaultnet.wal \
	    vaultnet_audit.log vaultnet.lock \
	    /tmp/vaultnet_unit.snap /tmp/vaultnet_unit.wal
