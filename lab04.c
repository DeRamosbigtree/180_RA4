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

// ================= THREAD ARG =================
typedef struct {
    int start_row;
    int rows_to_send;
    int n;
    int **M;
    SlaveInfo slave;
} ThreadArg;

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
            fscanf(fp, "%d %s %d", &id,
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

// ================= THREAD FUNCTION =================
void *slave_thread(void *arg) {
    ThreadArg *data = (ThreadArg *)arg;

    int sock;
    struct sockaddr_in addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("Socket creation failed\n");
        pthread_exit(NULL);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(data->slave.port);
    inet_pton(AF_INET, data->slave.ip, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("Connection to slave %d failed\n", data->slave.id);
        close(sock);
        pthread_exit(NULL);
    }

    printf("Connected to SLAVE %d\n", data->slave.id);

    // Send metadata
    send(sock, &data->rows_to_send, sizeof(int), 0);
    send(sock, &data->n, sizeof(int), 0);

    // Send matrix rows
    for (int r = data->start_row; r < data->start_row + data->rows_to_send; r++) {
        send(sock, data->M[r], data->n * sizeof(int), 0);
    }

    // Receive ACK
    char ack[10];
    recv(sock, ack, sizeof(ack), 0);
    printf("Received ACK from SLAVE %d\n", data->slave.id);

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
        printf("Error creating socket\n");
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        printf("Bind failed\n");
        return;
    }

    if(listen(server_fd, 3) < 0){
        printf("Listen failed\n");
        return;
    }

    printf("Waiting for master...\n");

    client_sock = accept(server_fd, NULL, NULL);
    if (client_sock < 0) {
        printf("Accept failed\n");
        return;
    }

    printf("MASTER connected to SLAVE %d\n", slave_id);

    clock_t t1 = clock();

    int rows, cols;
    recv(client_sock, &rows, sizeof(int), 0);
    recv(client_sock, &cols, sizeof(int), 0);

    int **sub = allocate_matrix(rows, cols);

    for (int i = 0; i < rows; i++) {
        recv(client_sock, sub[i], cols * sizeof(int), 0);
    }

    send(client_sock, "ack", 4, 0);

    clock_t t2 = clock();
    printf("SLAVE %d TIME: %f\n", slave_id,
           (double)(t2 - t1) / CLOCKS_PER_SEC);

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
    fill_matrix_random(M, n);

    int base = n / t;
    int remainder = n % t;
    int start_row = 0;

    pthread_t threads[MAX_SLAVES];
    ThreadArg args[MAX_SLAVES];

    clock_t t1 = clock();

    // CREATE THREADS
    for (int i = 0; i < t; i++) {
        int rows_to_send = base + (i < remainder ? 1 : 0);

        args[i].start_row = start_row;
        args[i].rows_to_send = rows_to_send;
        args[i].n = n;
        args[i].M = M;
        args[i].slave = config.slaves[i];

        pthread_create(&threads[i], NULL, slave_thread, &args[i]);

        start_row += rows_to_send;
    }

    // JOIN THREADS
    for (int i = 0; i < t; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_t t2 = clock();
    printf("MASTER TIME: %f\n",
           (double)(t2 - t1) / CLOCKS_PER_SEC);

    free_matrix(M, n);
}

// ================= MAIN =================
int main(int argc, char *argv[]) {
    if (argc < 5) {
        printf("Usage:\n");
        printf("Master: %s n p 0 config.txt\n", argv[0]);
        printf("Slave:  %s n p 1 config.txt slave_id\n", argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);
    int p = atoi(argv[2]);
    int s = atoi(argv[3]);
    const char *config_file = argv[4];

    if (s == 0) {
        run_master(n, p, config_file);
    } else {
        int slave_id = atoi(argv[5]);
        run_slave(n, p, config_file, slave_id);
    }

    return 0;
}