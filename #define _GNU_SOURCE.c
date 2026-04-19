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

// ================= CONFIG =================
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

// ================= MATRIX =================
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
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            M[i][j] = (rand() % 100) + 1;
}

void free_matrix(int **M, int rows) {
    for (int i = 0; i < rows; i++)
        free(M[i]);
    free(M);
}

// ================= THREAD STRUCT =================
typedef struct {
    int start_row;
    int rows_to_send;
    int n;
    int **M;
    SlaveInfo slave;
} ThreadArgs;

// ================= THREAD FUNCTION =================
void *send_to_slave(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("Socket creation failed\n");
        pthread_exit(NULL);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(args->slave.port);
    inet_pton(AF_INET, args->slave.ip, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("Connection to slave %d failed\n", args->slave.id);
        pthread_exit(NULL);
    }

    printf("Thread connected to SLAVE %d\n", args->slave.id);

    // Send metadata
    send(sock, &args->rows_to_send, sizeof(int), 0);
    send(sock, &args->n, sizeof(int), 0);

    // Send rows
    for (int r = args->start_row; r < args->start_row + args->rows_to_send; r++) {
        send(sock, args->M[r], args->n * sizeof(int), 0);
    }

    // Receive ACK
    char ack[10];
    recv(sock, ack, sizeof(ack), 0);

    printf("ACK from SLAVE %d\n", args->slave.id);

    close(sock);
    pthread_exit(NULL);
}

// ================= SLAVE =================
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

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        printf("Couldn't bind to the port\n");
        return;
    }

    if(listen(server_fd, 3) < 0){
        printf("Error while listening\n");
        return;
    }

    printf("Listening for incoming connections...\n");

    client_sock = accept(server_fd, NULL, NULL);
    if (client_sock < 0) {
        printf("Can't accept connection\n");
        return;
    }

    printf("MASTER connected to SLAVE %d\n", slave_id);

    clock_t time_before = clock();

    int rows, cols;
    recv(client_sock, &rows, sizeof(int), 0);
    recv(client_sock, &cols, sizeof(int), 0);

    int **sub = allocate_matrix(rows, cols);
    if (!sub) {
        printf("Memory allocation failed\n");
        return;
    }

    for (int i = 0; i < rows; i++) {
        recv(client_sock, sub[i], cols * sizeof(int), 0);
    }

    send(client_sock, "ack", 4, 0);

    clock_t time_after = clock();

    double elapsed = (double)(time_after - time_before) / CLOCKS_PER_SEC;
    printf("SLAVE %d TIME: %f seconds\n", slave_id, elapsed);

    printf("First: %d, Last: %d\n",
       sub[0][0],
       sub[rows-1][cols-1]);

    free_matrix(sub, rows);
    close(client_sock);
    close(server_fd);
}

// ================= MASTER =================
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

    pthread_t threads[MAX_SLAVES];
    ThreadArgs args[MAX_SLAVES];

    int base = n / t;
    int remainder = n % t;
    int start_row = 0;

    clock_t time_before = clock();

    for (int i = 0; i < t; i++) {
        int rows_to_send = base + (i < remainder ? 1 : 0);

        args[i].start_row = start_row;
        args[i].rows_to_send = rows_to_send;
        args[i].n = n;
        args[i].M = M;
        args[i].slave = config.slaves[i];

        pthread_create(&threads[i], NULL, send_to_slave, &args[i]);

        start_row += rows_to_send;
    }

    for (int i = 0; i < t; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_t time_after = clock();

    double elapsed = (double)(time_after - time_before) / CLOCKS_PER_SEC;
    printf("MASTER TIME: %f seconds\n", elapsed);

    free_matrix(M, n);
}

// ================= MAIN =================
int main(int argc, char *argv[]) {
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
            printf("Enter slave_id\n");
            return 1;
        }
        int slave_id = atoi(argv[5]);
        run_slave(n, p, config_file, slave_id);
    } else {
        printf("Invalid mode\n");
    }

    return 0;
}