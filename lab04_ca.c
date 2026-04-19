#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Socket headers
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>

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

typedef struct {
    SlaveInfo slave;
    int **M;
    int n;
    int start_row;
    int rows_to_send;
    int success;
    int core_id;
} ThreadArgs;

// config parser
void read_config(const char *filename, Config *config) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("Error: Could not open file.\n");
        exit(1);
    }

    char role[10];

    while(fscanf(fp, "%s", role) != EOF) {
        if (strcmp(role, "MASTER") == 0) {
            fscanf(fp, "%s %d", config->master_ip, &config->master_port);
        }
        else if (strcmp(role, "SLAVE") == 0) {
            int id;
            fscanf(fp, "%d %s %d",
                   &id,
                   config->slaves[config->slave_count].ip,
                   &config->slaves[config->slave_count].port);

            config->slaves[config->slave_count].id = id;
            config->slave_count++;
        }
    }
    fclose(fp);
}

int send_all(int sock, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = (const char *)buf;

    while (total < len) {
        ssize_t sent = send(sock, p + total, len - total, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sent == 0) return -1;
        total += (size_t)sent;
    }
    return 0;
}

int recv_all(int sock, void *buf, size_t len) {
    size_t total = 0;
    char *p = (char *)buf;

    while (total < len) {
        ssize_t recvd = recv(sock, p + total, len - total, 0);
        if (recvd < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (recvd == 0) return -1;
        total += (size_t)recvd;
    }
    return 0;
}

double get_time_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int **allocate_matrix(int rows, int cols) {
    int **M = malloc(rows * sizeof *M);
    if (!M) return NULL;

    for (int i = 0; i < rows; i++) {
        M[i] = malloc(cols * sizeof(int));
        if (!M[i]) {
            for (int k = 0; k < i; k++) free(M[k]);
            free(M);
            return NULL;
        }
    }
    return M;
}

void fill_matrix_random(int **M, int n) {
    srand((unsigned)time(NULL));
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            M[i][j] = (rand() % 100) + 1;
        }
    }
}

void free_matrix(int **M, int rows) {
    for (int i = 0; i < rows; i++) {
        free(M[i]);
    }
    free(M);
}

// SLAVE
void run_slave (int n, int port, const char *config_file, int slave_id) {
    int server_fd, client_sock;
    struct sockaddr_in addr;

    Config config = {0};
    read_config(config_file, &config);

    printf("SLAVE %d listening on %d\n", slave_id, port);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if(server_fd < 0){
        printf("Error while creating socket\n");
        return;
    }
    printf("Socket created successfully\n");

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        printf("Couldn't bind to the port\n");
        close(server_fd);
        return;
    }
    
    if(listen(server_fd, 3) < 0){
        printf("Error while listening\n");
        close(server_fd);
        return;
    }
    printf("\nListening for incoming connections.....\n");

    client_sock = accept(server_fd, NULL, NULL);
    if (client_sock < 0) {
        printf("Can't accept connection\n");
        close(server_fd);
        return;
    }

    printf("MASTER connected to SLAVE %d\n", slave_id);

    double time_before = get_time_seconds();

    int rows, cols;
    if (recv_all(client_sock, &rows, sizeof(int)) < 0 ||
        recv_all(client_sock, &cols, sizeof(int)) < 0) {
        printf("Error receiving metadata from master\n");
        close(client_sock);
        close(server_fd);
        return;
    }

    int **sub = allocate_matrix(rows, cols);
    if (!sub) {
        printf("Memory allocation failed\n");
        close(client_sock);
        close(server_fd);
        return;
    }

    for (int i = 0; i < rows; i++) {
        if (recv_all(client_sock, sub[i], (size_t)cols * sizeof(int)) < 0) {
            printf("Error receiving row %d from master\n", i);
            free_matrix(sub, rows);
            close(client_sock);
            close(server_fd);
            return;
        }
    }

    char ack[] = "ack";
    if (send_all(client_sock, ack, sizeof(ack)) < 0) {
        printf("Error sending ACK to master\n");
        free_matrix(sub, rows);
        close(client_sock);
        close(server_fd);
        return;
    }

    double time_after = get_time_seconds();

    double elapsed = time_after - time_before;
    printf("SLAVE %d TIME: %f seconds\n", slave_id, elapsed);

    printf("First: %d, Last: %d\n",
       sub[0][0],
       sub[rows-1][cols-1]);

    free_matrix(sub, rows);
    close(client_sock);
    close(server_fd);
}

void *master_send_to_slave(void *arg) {
    ThreadArgs *a = (ThreadArgs *)arg;
    a->success = 0;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(a->core_id, &cpuset);

    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        printf("Warning: could not set affinity for SLAVE %d thread to core %d\n",
               a->slave.id, a->core_id);
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("Socket creation failed for SLAVE %d\n", a->slave.id);
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(a->slave.port);
    inet_pton(AF_INET, a->slave.ip, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("Connection to SLAVE %d failed\n", a->slave.id);
        close(sock);
        return NULL;
    }

    printf("Connected to SLAVE %d on core %d\n", a->slave.id, a->core_id);

    if (send_all(sock, &a->rows_to_send, sizeof(int)) < 0 ||
        send_all(sock, &a->n, sizeof(int)) < 0) {
        printf("Error sending metadata to SLAVE %d\n", a->slave.id);
        close(sock);
        return NULL;
    }

    for (int r = a->start_row; r < a->start_row + a->rows_to_send; r++) {
        if (send_all(sock, a->M[r], (size_t)a->n * sizeof(int)) < 0) {
            printf("Error sending row %d to SLAVE %d\n", r, a->slave.id);
            close(sock);
            return NULL;
        }
    }

    char ack[4];
    if (recv_all(sock, ack, sizeof(ack)) < 0) {
        printf("Error receiving ACK from SLAVE %d\n", a->slave.id);
        close(sock);
        return NULL;
    }

    printf("Received ACK from SLAVE %d\n", a->slave.id);

    a->success = 1;
    close(sock);
    return NULL;
}

// MASTER
void run_master(int n, int p, const char *config_file) {
    Config config = {0};
    read_config(config_file, &config);

    int t = config.slave_count;
    printf("MASTER: %d slaves\n", t);

    int **M = allocate_matrix(n, n);
    if (!M) {
        printf("Memory allocation failed\n");
        return;
    }
    fill_matrix_random(M, n);

    int total_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    int usable_cores = total_cores - 1;
    if (usable_cores < 1) usable_cores = 1;

    printf("MASTER: total cores = %d, usable cores = %d\n", total_cores, usable_cores);

    int base = n / t;
    int remainder = n % t;
    int start_row = 0;

    pthread_t threads[MAX_SLAVES];
    ThreadArgs args[MAX_SLAVES];

    double time_before = get_time_seconds();

    for (int i = 0; i < t; i++) {
        int rows_to_send = base + (i < remainder ? 1 : 0);

        args[i].slave = config.slaves[i];
        args[i].M = M;
        args[i].n = n;
        args[i].start_row = start_row;
        args[i].rows_to_send = rows_to_send;
        args[i].success = 0;
        args[i].core_id = i % usable_cores;

        if (pthread_create(&threads[i], NULL, master_send_to_slave, &args[i]) != 0) {
            printf("Failed to create thread for SLAVE %d\n", config.slaves[i].id);
            free_matrix(M, n);
            return;
        }

        start_row += rows_to_send;
    }

    for (int i = 0; i < t; i++) {
        pthread_join(threads[i], NULL);
    }

    double time_after = get_time_seconds();

    double elapsed = time_after - time_before;
    printf("MASTER TIME: %f seconds\n", elapsed);

    free_matrix(M, n);
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    if (argc < 5) {
        printf("Usage:\n");
        printf("  Master: %s n p 0 config.txt\n", argv[0]);
        printf("  Slave: %s n p 1 config.txt slave_id\n", argv[0]);
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
            printf("Enter slave_id for slave mode\n");
            return 1;
        }
        int slave_id = atoi(argv[5]);
        run_slave(n, p, config_file, slave_id);
    } else {
        printf("Enter correct value for s (0 for master and 1 for slave)");
        return 1;
    }

    return 0;
}