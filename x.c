// Compile: gcc -O2 lab04.c -o lab04
// Example runs:
//   Slave 1: ./lab04 4 5001 1 config.txt 1
//   Slave 2: ./lab04 4 5002 1 config.txt 2
//   Master : ./lab04 4 5000 0 config.txt

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

#define MAX_SLAVES 32
#define MAX_IP_LEN 64

typedef struct {
    int id;
    char ip[MAX_IP_LEN];
    int port;
} SlaveInfo;

typedef struct {
    char master_ip[MAX_IP_LEN];
    int master_port;
    SlaveInfo slaves[MAX_SLAVES];
    int slave_count;
} Config;

ssize_t send_all(int sock, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = (const char *)buf;

    while (total < len) {
        ssize_t sent = send(sock, p + total, len - total, 0);
        if (sent <= 0) return sent;
        total += sent;
    }
    return (ssize_t)total;
}

ssize_t recv_all(int sock, void *buf, size_t len) {
    size_t total = 0;
    char *p = (char *)buf;

    while (total < len) {
        ssize_t recvd = recv(sock, p + total, len - total, 0);
        if (recvd <= 0) return recvd;
        total += recvd;
    }
    return (ssize_t)total;
}

int read_config(const char *filename, Config *cfg) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    cfg->slave_count = 0;
    char role[16];

    while (fscanf(fp, "%15s", role) == 1) {
        if (strcmp(role, "MASTER") == 0) {
            if (fscanf(fp, "%63s %d", cfg->master_ip, &cfg->master_port) != 2) {
                fclose(fp);
                return -1;
            }
        } else if (strcmp(role, "SLAVE") == 0) {
            if (cfg->slave_count >= MAX_SLAVES) {
                fclose(fp);
                return -1;
            }
            SlaveInfo *s = &cfg->slaves[cfg->slave_count];
            if (fscanf(fp, "%d %63s %d", &s->id, s->ip, &s->port) != 3) {
                fclose(fp);
                return -1;
            }
            cfg->slave_count++;
        }
    }

    fclose(fp);
    return 0;
}

int find_slave(Config *cfg, int slave_id) {
    for (int i = 0; i < cfg->slave_count; i++) {
        if (cfg->slaves[i].id == slave_id) return i;
    }
    return -1;
}

int **allocate_matrix(int n) {
    int **X = (int **)malloc((size_t)n * sizeof(int *));
    if (!X) return NULL;

    for (int i = 0; i < n; i++) {
        X[i] = (int *)malloc((size_t)n * sizeof(int));
        if (!X[i]) {
            for (int k = 0; k < i; k++) free(X[k]);
            free(X);
            return NULL;
        }
    }
    return X;
}

void free_matrix(int **X, int n) {
    if (!X) return;
    for (int i = 0; i < n; i++) free(X[i]);
    free(X);
}

void fill_matrix_deterministic(int **X, int n) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            X[i][j] = i * n + j + 1;
        }
    }
}

/* If your instructor insists on random non-zero values, use this instead.
void fill_matrix_random(int **X, int n) {
    srand((unsigned)time(NULL));
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            X[i][j] = (rand() % 100) + 1;
        }
    }
}
*/

int verify_submatrix(int **sub, int start_row, int rows, int n) {
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < n; j++) {
            int expected = (start_row + i) * n + j + 1;
            if (sub[i][j] != expected) {
                return 0;
            }
        }
    }
    return 1;
}

void run_master(int n, int p, const char *config_file) {
    Config cfg;
    if (read_config(config_file, &cfg) != 0) {
        fprintf(stderr, "Failed to read config file\n");
        exit(1);
    }

    int t = cfg.slave_count;
    if (t <= 0) {
        fprintf(stderr, "No slaves found in config\n");
        exit(1);
    }

    if (n <= 0) {
        fprintf(stderr, "n must be positive\n");
        exit(1);
    }

    if (n % t != 0) {
        fprintf(stderr, "For this LRP04 version, n must be divisible by t\n");
        exit(1);
    }

    int **X = allocate_matrix(n);
    if (!X) {
        fprintf(stderr, "Matrix allocation failed\n");
        exit(1);
    }

    // For easy verification during development
    fill_matrix_deterministic(X, n);

    int rows_per_slave = n / t;

    struct timespec time_before, time_after;
    clock_gettime(CLOCK_MONOTONIC, &time_before);

    for (int id = 0; id < t; id++) {
        int start_row = id * rows_per_slave;
        int row_count = rows_per_slave;

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket");
            free_matrix(X, n);
            exit(1);
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(cfg.slaves[id].port);

        if (inet_pton(AF_INET, cfg.slaves[id].ip, &addr.sin_addr) <= 0) {
            perror("inet_pton");
            close(sock);
            free_matrix(X, n);
            exit(1);
        }

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("connect");
            close(sock);
            free_matrix(X, n);
            exit(1);
        }

        // Send metadata
        if (send_all(sock, &start_row, sizeof(int)) <= 0 ||
            send_all(sock, &row_count, sizeof(int)) <= 0 ||
            send_all(sock, &n, sizeof(int)) <= 0) {
            perror("send metadata");
            close(sock);
            free_matrix(X, n);
            exit(1);
        }

        // Send assigned rows one by one
        for (int r = 0; r < row_count; r++) {
            if (send_all(sock, X[start_row + r], (size_t)n * sizeof(int)) <= 0) {
                perror("send row");
                close(sock);
                free_matrix(X, n);
                exit(1);
            }
        }

        char ack[4];
        if (recv_all(sock, ack, 3) <= 0) {
            perror("recv ack");
            close(sock);
            free_matrix(X, n);
            exit(1);
        }
        ack[3] = '\0';

        printf("Master received ack from slave %d: %s\n", cfg.slaves[id].id, ack);

        close(sock);
    }

    clock_gettime(CLOCK_MONOTONIC, &time_after);

    double time_elapsed =
        (double)(time_after.tv_sec - time_before.tv_sec) +
        (double)(time_after.tv_nsec - time_before.tv_nsec) / 1e9;

    printf("Master elapsed time: %.6f seconds\n", time_elapsed);

    free_matrix(X, n);
    (void)p; // keeps the required p input without warning if unused internally
}

void run_slave(int n_input, int p, const char *config_file, int slave_id) {
    Config cfg;
    if (read_config(config_file, &cfg) != 0) {
        fprintf(stderr, "Failed to read config file\n");
        exit(1);
    }

    int idx = find_slave(&cfg, slave_id);
    if (idx < 0) {
        fprintf(stderr, "Slave ID %d not found in config\n", slave_id);
        exit(1);
    }

    int my_port = cfg.slaves[idx].port;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(my_port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(1);
    }

    if (listen(server_fd, 1) < 0) {
        perror("listen");
        close(server_fd);
        exit(1);
    }

    printf("Slave %d listening on port %d\n", slave_id, my_port);

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept");
        close(server_fd);
        exit(1);
    }

    struct timespec time_before, time_after;
    clock_gettime(CLOCK_MONOTONIC, &time_before);

    int start_row, row_count, n;
    if (recv_all(client_fd, &start_row, sizeof(int)) <= 0 ||
        recv_all(client_fd, &row_count, sizeof(int)) <= 0 ||
        recv_all(client_fd, &n, sizeof(int)) <= 0) {
        perror("recv metadata");
        close(client_fd);
        close(server_fd);
        exit(1);
    }

    int **sub = allocate_matrix(row_count);
    if (!sub) {
        fprintf(stderr, "Submatrix allocation failed\n");
        close(client_fd);
        close(server_fd);
        exit(1);
    }

    // allocate_matrix(row_count) created row_count rows each of length row_count,
    // but we need row_count rows each of length n.
    // So free and do proper allocation:
    free_matrix(sub, row_count);

    sub = (int **)malloc((size_t)row_count * sizeof(int *));
    if (!sub) {
        fprintf(stderr, "Submatrix allocation failed\n");
        close(client_fd);
        close(server_fd);
        exit(1);
    }

    for (int i = 0; i < row_count; i++) {
        sub[i] = (int *)malloc((size_t)n * sizeof(int));
        if (!sub[i]) {
            for (int k = 0; k < i; k++) free(sub[k]);
            free(sub);
            close(client_fd);
            close(server_fd);
            exit(1);
        }
    }

    for (int r = 0; r < row_count; r++) {
        if (recv_all(client_fd, sub[r], (size_t)n * sizeof(int)) <= 0) {
            perror("recv row");
            for (int i = 0; i < row_count; i++) free(sub[i]);
            free(sub);
            close(client_fd);
            close(server_fd);
            exit(1);
        }
    }

    if (send_all(client_fd, "ack", 3) <= 0) {
        perror("send ack");
    }

    clock_gettime(CLOCK_MONOTONIC, &time_after);

    double time_elapsed =
        (double)(time_after.tv_sec - time_before.tv_sec) +
        (double)(time_after.tv_nsec - time_before.tv_nsec) / 1e9;

    printf("Slave %d elapsed time: %.6f seconds\n", slave_id, time_elapsed);

    if (verify_submatrix(sub, start_row, row_count, n)) {
        printf("Slave %d verification: CORRECT\n", slave_id);
    } else {
        printf("Slave %d verification: WRONG\n", slave_id);
    }

    for (int i = 0; i < row_count; i++) free(sub[i]);
    free(sub);

    close(client_fd);
    close(server_fd);

    (void)n_input;
    (void)p;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  Master: %s n p 0 config.txt\n", argv[0]);
        fprintf(stderr, "  Slave : %s n p 1 config.txt slave_id\n", argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);
    int p = atoi(argv[2]);
    int s = atoi(argv[3]);
    const char *config_file = argv[4];

    if (s == 0) {
        run_master(n, p, config_file);
    } else if (s == 1) {
        if (argc < 6) {
            fprintf(stderr, "Slave mode requires slave_id\n");
            return 1;
        }
        int slave_id = atoi(argv[5]);
        run_slave(n, p, config_file, slave_id);
    } else {
        fprintf(stderr, "s must be 0 for master or 1 for slave\n");
        return 1;
    }

    return 0;
}