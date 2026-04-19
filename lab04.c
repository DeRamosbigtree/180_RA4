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
    int start_row;
    int rows_to_send;
    int n;
    int **M;
    SlaveInfo slave;
} ThreadArg;

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
            fscanf(fp, "%d %s %d", &id, config->slaves[config->slave_count].ip, &config->slaves[config->slave_count].port);

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

// Threads
void *slave_thread(void *arg) {
    ThreadArg *data = (ThreadArg *)arg;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;

    if (sock < 0) {
        printf("Socket creation failed\n");
        pthread_exit(NULL);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(data->slave.port);
    inet_pton(AF_INET, data->slave.ip, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("Connection failed SLAVE %d\n", data->slave.id);
        close(sock);
        pthread_exit(NULL);
    }

    printf("Connected to SLAVE %d\n", data->slave.id);

    // send metadata
    send_all(sock, &data->rows_to_send, sizeof(int));
    send_all(sock, &data->n, sizeof(int));

    // send matrix rows
    for (int r = data->start_row;
         r < data->start_row + data->rows_to_send;
         r++) {
        send_all(sock, data->M[r], data->n * sizeof(int));
    }

    // receive ack
    char ack[4];
    recv_all(sock, ack, sizeof(ack));
    printf("ACK from SLAVE %d\n", data->slave.id);

    close(sock);
    pthread_exit(NULL);
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
    printf("\nListening for incoming connections.....\n");

    client_sock = accept(server_fd, NULL, NULL);
    if (client_sock < 0) {
        printf("Can't accept connection\n");
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


    // lets fix this to print the whole matrix
    printf("First: %d, Last: %d\n",
       sub[0][0],
       sub[rows-1][cols-1]);

    free_matrix(sub, rows);
    close(client_sock);
    close(server_fd);
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

    int base = n / t;
    int remainder = n % t;
    int start_row = 0;

    pthread_t threads[MAX_SLAVES];
    ThreadArg args[MAX_SLAVES];

    double time_before = get_time_seconds();

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

    for (int i = 0; i < t; i++)
        pthread_join(threads[i], NULL);
       
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