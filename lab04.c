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
    return total;
}

ssize_t recv_all(int sock, void *buf, size_t len) {
    size_t total = 0;
    char *p = (char *)buf;

    while (total < len) {
        ssize_t recvd = recv(sock, p + total, len - total, 0);
        if (recvd <= 0) return recvd;
        total += recvd;
    }
    return total;
}

double get_time_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
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
            fscanf(fp, "%63s %d", cfg->master_ip, &cfg->master_port);
        } else if (strcmp(role, "SLAVE") == 0) {
            if (cfg->slave_count >= MAX_SLAVES) {
                fprintf(stderr, "Too many slaves in config file\n");
                fclose(fp);
                return -1;
            }
            SlaveInfo *s = &cfg->slaves[cfg->slave_count++];
            fscanf(fp, "%d %63s %d", &s->id, s->ip, &s->port);
        }
    }

    fclose(fp);
    return 0;
}

int find_slave_index(Config *cfg, int slave_id) {
    for (int i = 0; i < cfg->slave_count; i++) {
        if (cfg->slaves[i].id == slave_id) return i;
    }
    return -1;
}

int *create_matrix(int n) {
    long long total = (long long)n * n;
    int *M = (int *)malloc(total * sizeof(int));
    if (!M) return NULL;

    for (int r = 0; r < n; r++) {
        for (int c = 0; c < n; c++) {
            M[r * n + c] = r * n + c + 1;
        }
    }

    return M;
}

int verify_submatrix(int *sub, int start_row, int rows, int n) {
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < n; c++) {
            int expected = (start_row + r) * n + c + 1;
            if (sub[r * n + c] != expected) {
                return 0;
            }
        }
    }
    return 1;
}

void run_master(int n, const char *config_file) {
    Config cfg;
    if (read_config(config_file, &cfg) != 0) {
        exit(1);
    }

    int t = cfg.slave_count;
    if (t <= 0) {
        fprintf(stderr, "No slaves found in config file\n");
        exit(1);
    }

    if (n % t != 0) {
        fprintf(stderr, "n must be divisible by number of slaves\n");
        exit(1);
    }

    int *M = create_matrix(n);
    if (!M) {
        fprintf(stderr, "Failed to allocate matrix\n");
        exit(1);
    }

    int rows_per_slave = n / t;
    double time_before = get_time_sec();

    for (int i = 0; i < t; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket");
            free(M);
            exit(1);
        }

        struct sockaddr_in slave_addr;
        memset(&slave_addr, 0, sizeof(slave_addr));
        slave_addr.sin_family = AF_INET;
        slave_addr.sin_port = htons(cfg.slaves[i].port);

        if (inet_pton(AF_INET, cfg.slaves[i].ip, &slave_addr.sin_addr) <= 0) {
            perror("inet_pton");
            close(sock);
            free(M);
            exit(1);
        }

        if (connect(sock, (struct sockaddr *)&slave_addr, sizeof(slave_addr)) < 0) {
            perror("connect");
            close(sock);
            free(M);
            exit(1);
        }

        int start_row = i * rows_per_slave;
        int rows = rows_per_slave;

        if (send_all(sock, &start_row, sizeof(int)) <= 0 ||
            send_all(sock, &rows, sizeof(int)) <= 0 ||
            send_all(sock, &n, sizeof(int)) <= 0 ||
            send_all(sock, &M[start_row * n], (size_t)rows * n * sizeof(int)) <= 0) {
            perror("send_all");
            close(sock);
            free(M);
            exit(1);
        }

        char ack[4];
        if (recv_all(sock, ack, 3) <= 0) {
            perror("recv_all ack");
            close(sock);
            free(M);
            exit(1);
        }
        ack[3] = '\0';

        close(sock);
    }

    double time_after = get_time_sec();
    double time_elapsed = time_after - time_before;

    printf("Master elapsed time: %.6f seconds\n", time_elapsed);

    free(M);
}

void run_slave(const char *config_file, int slave_id) {
    Config cfg;
    if (read_config(config_file, &cfg) != 0) {
        exit(1);
    }

    int idx = find_slave_index(&cfg, slave_id);
    if (idx < 0) {
        fprintf(stderr, "Slave ID %d not found in config file\n", slave_id);
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

    struct sockaddr_in my_addr;
    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_addr.s_addr = INADDR_ANY;
    my_addr.sin_port = htons(my_port);

    if (bind(server_fd, (struct sockaddr *)&my_addr, sizeof(my_addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(1);
    }

    if (listen(server_fd, 1) < 0) {
        perror("listen");
        close(server_fd);
        exit(1);
    }

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept");
        close(server_fd);
        exit(1);
    }

    double time_before = get_time_sec();

    int start_row, rows, n;

    if (recv_all(client_fd, &start_row, sizeof(int)) <= 0 ||
        recv_all(client_fd, &rows, sizeof(int)) <= 0 ||
        recv_all(client_fd, &n, sizeof(int)) <= 0) {
        perror("recv_all header");
        close(client_fd);
        close(server_fd);
        exit(1);
    }

    int *sub = (int *)malloc((size_t)rows * n * sizeof(int));
    if (!sub) {
        fprintf(stderr, "Failed to allocate submatrix\n");
        close(client_fd);
        close(server_fd);
        exit(1);
    }

    if (recv_all(client_fd, sub, (size_t)rows * n * sizeof(int)) <= 0) {
        perror("recv_all data");
        free(sub);
        close(client_fd);
        close(server_fd);
        exit(1);
    }

    if (send_all(client_fd, "ack", 3) <= 0) {
        perror("send_all ack");
        free(sub);
        close(client_fd);
        close(server_fd);
        exit(1);
    }

    double time_after = get_time_sec();
    double time_elapsed = time_after - time_before;

    printf("Slave %d elapsed time: %.6f seconds\n", slave_id, time_elapsed);

    if (verify_submatrix(sub, start_row, rows, n)) {
        printf("Slave %d verification: CORRECT\n", slave_id);
    } else {
        printf("Slave %d verification: WRONG\n", slave_id);
    }

    free(sub);
    close(client_fd);
    close(server_fd);
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

    (void)p;

    if (s == 0) {
        run_master(n, config_file);
    } else if (s == 1) {
        if (argc < 6) {
            fprintf(stderr, "Slave mode requires slave_id\n");
            return 1;
        }
        int slave_id = atoi(argv[5]);
        run_slave(config_file, slave_id);
    } else {
        fprintf(stderr, "Invalid status s. Use 0 for master, 1 for slave.\n");
        return 1;
    }

    return 0;
}