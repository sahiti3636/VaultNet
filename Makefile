CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -pthread -O2 -std=c11 -D_GNU_SOURCE
LDFLAGS := -pthread

SRCDIR  := src
OBJDIR  := obj
BINDIR  := bin

SERVER_SRCS := $(SRCDIR)/server.c \
               $(SRCDIR)/accounts.c \
               $(SRCDIR)/auth.c \
               $(SRCDIR)/audit.c \
               $(SRCDIR)/threadpool.c \
               $(SRCDIR)/netutil.c

CLIENT_SRCS := $(SRCDIR)/client.c \
               $(SRCDIR)/netutil.c

SERVER_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SERVER_SRCS))
CLIENT_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(CLIENT_SRCS))

SERVER_BIN  := $(BINDIR)/vaultnet_server
CLIENT_BIN  := $(BINDIR)/vaultnet_client

.PHONY: all server client clean

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

clean:
	rm -rf $(OBJDIR) $(BINDIR) vaultnet_accounts.dat vaultnet_audit.log
