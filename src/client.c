/*
 * client.c – VaultNet interactive command-line client
 *
 * Usage: vaultnet_client [host [port [--tls]]]
 *
 *   --tls   connect with TLS (accepts self-signed certificates)
 *
 * Connects to the server, then enters a read-eval loop:
 *   - read a line from stdin
 *   - send it to the server
 *   - print every response line until a line starting with OK/ERR/END
 */

#include "protocol.h"
#include "tlsutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/ssl.h>

#define DEFAULT_HOST "127.0.0.1"

static int connect_to_server(const char *host, int port) {
    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "client: cannot resolve host '%s'\n", host);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[]) {
    const char *host  = DEFAULT_HOST;
    int         port  = DEFAULT_PORT;
    int         use_tls = 0;

    /* Parse positional arguments and --tls in any order. */
    int positionals = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tls") == 0) {
            use_tls = 1;
        } else if (positionals++ == 0) {
            host = argv[i];
        } else if (positionals == 2) {
            char *end = NULL;
            long parsed = strtol(argv[i], &end, 10);
            if (!end || *end != '\0' || parsed < 1 || parsed > 65535) {
                fprintf(stderr, "Invalid port: %s\n", argv[i]);
                return 1;
            }
            port = (int)parsed;
        } else {
            fprintf(stderr, "Usage: %s [host [port]] [--tls]\n", argv[0]);
            return 1;
        }
    }

    int fd = connect_to_server(host, port);
    if (fd < 0) return 1;

    VNConn vc;
    vc.fd  = fd;
    vc.ssl = NULL;

    if (use_tls) {
        SSL_CTX *ctx = tls_client_ctx(0);  /* 0 = accept self-signed */
        if (!ctx) { close(fd); return 1; }
        SSL *ssl = SSL_new(ctx);
        SSL_CTX_free(ctx);
        if (!ssl) { close(fd); return 1; }
        SSL_set_fd(ssl, fd);
        if (SSL_connect(ssl) <= 0) {
            fprintf(stderr, "TLS handshake failed\n");
            SSL_free(ssl);
            close(fd);
            return 1;
        }
        vc.ssl = ssl;
        fprintf(stdout, "TLS connected to %s:%d\n", host, port);
    } else {
        fprintf(stdout, "Connected to %s:%d  (type HELP for commands, QUIT to exit)\n",
                host, port);
    }

    char line[MAX_LINE];
    ssize_t n;

    /* Print the server's initial greeting */
    n = vn_readline(&vc, line, sizeof(line));
    if (n <= 0) { fprintf(stderr, "server closed immediately\n"); vn_close(&vc); return 1; }
    printf("< %s\n", line);

    /* Interactive loop */
    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        if (vn_sendline(&vc, line) < 0) {
            perror("vn_sendline");
            break;
        }

        int quitting = (strcasecmp(line, "QUIT") == 0);
        int history  = (strcasecmp(line, "HISTORY") == 0);

        for (;;) {
            n = vn_readline(&vc, line, sizeof(line));
            if (n < 0) { perror("vn_readline"); goto done; }
            if (n == 0) { printf("(server closed connection)\n"); goto done; }

            printf("< %s\n", line);

            if (strncmp(line, "END", 3) == 0) break;
            if (strncmp(line, "ERR", 3) == 0 ||
                (!history && strncmp(line, "OK", 2) == 0)) break;
        }

        if (quitting) break;
    }

done:
    vn_close(&vc);
    return 0;
}
