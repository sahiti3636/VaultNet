/*
 * client.c – VaultNet interactive command-line client
 *
 * Usage: vaultnet_client [host [port]]
 *
 * Connects to the server, then enters a read-eval loop:
 *   - read a line from stdin
 *   - send it to the server
 *   - print every response line until a line starting with OK/ERR/END
 *
 * Multi-line responses (HISTORY) are terminated by a line starting with "END".
 */

#include "protocol.h"
#include "netutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

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
    const char *host = DEFAULT_HOST;
    int         port = DEFAULT_PORT;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    int fd = connect_to_server(host, port);
    if (fd < 0) return 1;

    fprintf(stdout, "Connected to %s:%d  (type HELP for commands, QUIT to exit)\n",
            host, port);

    char line[MAX_LINE];
    ssize_t n;

    /* Print the server's initial greeting */
    n = readline_fd(fd, line, sizeof(line));
    if (n <= 0) { fprintf(stderr, "server closed immediately\n"); close(fd); return 1; }
    printf("< %s\n", line);

    /* Interactive loop */
    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break; /* EOF / Ctrl-D */

        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        /* Send to server */
        if (sendline_fd(fd, line) < 0) {
            perror("sendline_fd");
            break;
        }

        /* Detect QUIT so we can exit after the reply */
        int quitting = (strcasecmp(line, "QUIT") == 0);

        /* Read response lines until a final status (OK / ERR / END) */
        for (;;) {
            n = readline_fd(fd, line, sizeof(line));
            if (n < 0) { perror("readline_fd"); goto done; }
            if (n == 0) { printf("(server closed connection)\n"); goto done; }

            printf("< %s\n", line);

            /* END terminates multi-line responses */
            if (strncmp(line, "END", 3) == 0) break;
            /* Single-line terminal statuses */
            if (strncmp(line, "OK",  2) == 0 ||
                strncmp(line, "ERR", 3) == 0) break;
        }

        if (quitting) break;
    }

done:
    close(fd);
    return 0;
}
