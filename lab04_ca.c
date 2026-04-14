#define _GNU_SOURCE

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <sched.h>

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

// Core affinity
void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == -1) {
        perror("sched_setaffinity");
        exit(1);
    }
}

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
            fscanf(fp, "%s" "%d", config->master_ip, &config->master_port);
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

    // Pin slave to a CPU 
    set_affinity(slave_id % sysconf(_SC_NPROCESSORS_ONLN));

    printf("SLAVE %d pinned to a CPU core\n", slave_id);

    // Read from the configuration file what is the IP address of the master;
    Config config = {0};
    read_config(config_file, &config);

    printf("SLAVE %d listening on %d\n", slave_id, port);

    // Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if(server_fd < 0){
        printf("Error while creating socket\n");
        return;
    }
    printf("Socket created successfully\n");
    
    // Initialize the server address by the port and IP:
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    // Bind 
    if(bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        printf("Couldn't bind to the port\n");
        return;
    }
    // printf("Done with binding\n");
    
    // Turn on the socket to listen for incoming connections:
    if(listen(server_fd, 3) < 0){
        printf("Error while listening\n");
        return;
    }
    printf("\nListening for incoming connections.....\n");

    // Wait for master
    client_sock = accept(server_fd, NULL, NULL);
    if (client_sock < 0) {
        printf("Can't accept connection\n");
        return;
    }

    printf("MASTER connected to SLAVE %d\n", slave_id);

    // Start timer
    clock_t time_before = clock();

    // Receive metadata
    int rows, cols;
    recv(client_sock, &rows, sizeof(int), 0);
    recv(client_sock, &cols, sizeof(int), 0);

    // Allocate
    int **sub = allocate_matrix(rows, cols);
    if (!sub) {
        printf("Memory allocation failed\n");
        return;
    }

    // Receive data
    for (int i = 0; i < rows; i++) {
        recv(client_sock, sub[i], cols * sizeof(int), 0);
    }

    // Send ack
    send(client_sock, "ack", 4, 0);

    // ---- timing end ----
    clock_t time_after = clock();

    double elapsed = (double)(time_after - time_before) / CLOCKS_PER_SEC;
    printf("SLAVE %d TIME: %f seconds\n", slave_id, elapsed);

    // Verification
    printf("First: %d, Last: %d\n",
       sub[0][0],
       sub[rows-1][cols-1]);

    // Cleanup
    free_matrix(sub, rows);
    close(client_sock);
    close(server_fd);

}

// MASTER
void run_master(int n, int p, const char *config_file) {
    // (b) Read config
    Config config = {0};
    read_config(config_file, &config);

    int t = config.slave_count;
    printf("MASTER: %d slaves\n", t);

    // (a) Create random matrix
    int **M = allocate_matrix(n, n);
    if (!M) {
        printf("Memory allocation failed\n");
        return;
    }
    fill_matrix_random(M, n);

    // (c) Divide matrix
    int base = n / t;
    int remainder = n % t;
    int start_row = 0;

    int socks[MAX_SLAVES];

    // (d) time_before
    clock_t time_before = clock();

    // (e) Connect + send to each slave
    for (int i = 0; i < t; i++) {
        int rows_to_send = base + (i < remainder ? 1 : 0);

        // Create socket
        socks[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (socks[i] < 0) {
            printf("Socket creation failed\n");
            return;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config.slaves[i].port);
        inet_pton(AF_INET, config.slaves[i].ip, &addr.sin_addr);

        // Connect to slave
        if (connect(socks[i], (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            printf("Connection to slave %d failed\n", config.slaves[i].id);
            return;
        }

        printf("Connected to SLAVE %d\n", config.slaves[i].id);

        // Send metadata
        send(socks[i], &rows_to_send, sizeof(int), 0);
        send(socks[i], &n, sizeof(int), 0);

        // Send submatrix rows
        for (int r = start_row; r < start_row + rows_to_send; r++) {
            send(socks[i], M[r], n * sizeof(int), 0);
        }

        start_row += rows_to_send;
    }

    // (f,g) Receive ACKs from all slaves
    for (int i = 0; i < t; i++) {
        char ack[10];
        recv(socks[i], ack, sizeof(ack), 0);

        printf("Received ACK from SLAVE %d\n", config.slaves[i].id);

        close(socks[i]);
    }

    // (f) time_after
    clock_t time_after = clock();

    double elapsed = (double)(time_after - time_before) / CLOCKS_PER_SEC;
    printf("MASTER TIME: %f seconds\n", elapsed);

    free_matrix(M, n);
}

int main(int argc, char *argv[]) {
   // (1) Read n, s, p as user input
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

    // (2) If s = 0, then
    if (s == 0) {
        // call function to run master
        run_master(n, p, config_file);
    } else if (s == 1) {
        if (argc < 6) {
            printf("Enter slave_id for slave mode\n");
            return 1;
        }
        int slave_id = atoi(argv[5]);
        // call function to run slave
        run_slave(n, p, config_file, slave_id);
    } else {
        printf("Enter correct value for s (0 for master and 1 for slave)");
        return 1;
    }

    return 0;
}