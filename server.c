/*
 * ============================================================================
 * PARKING SLOT MANAGEMENT SYSTEM - SERVER
 * ============================================================================
 * EGC 301P - Operating Systems Lab Mini Project
 *
 * OS Concepts Demonstrated:
 *   1. Role-Based Authorization  (ADMIN / OPERATOR / USER)
 *   2. File Locking              (flock - shared & exclusive)
 *   3. Concurrency Control       (pthreads, mutex, semaphore)
 *   4. Data Consistency          (mutex + file locks prevent race conditions)
 *   5. Socket Programming        (TCP client-server)
 *   6. IPC - Shared Memory       (shmget/shmat for activity log)
 *
 * Compile: gcc -o server server.c -pthread
 * Run:     ./server
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Configuration ───────────────────────────────────────────────────────── */
#define PORT            8080
#define MAX_CLIENTS     10
#define MAX_SLOTS       20
#define BUF_SIZE        2048
#define MAX_USERS       50
#define MAX_LOG_ENTRIES  100
#define USERS_FILE      "users.txt"
#define PARKING_FILE    "parking_data.txt"
#define SHM_KEY         0x1234

/* ── Role Definitions ────────────────────────────────────────────────────── */
#define ROLE_ADMIN      0
#define ROLE_OPERATOR   1
#define ROLE_USER       2

/* ── Data Structures ─────────────────────────────────────────────────────── */
typedef struct {
    int  slot_id;
    int  occupied;          /* 0 = FREE, 1 = BOOKED */
    char booked_by[64];
} ParkingSlot;

typedef struct {
    char username[64];
    char password[64];
    int  role;              /* ROLE_ADMIN / ROLE_OPERATOR / ROLE_USER */
} User;

typedef struct {
    char timestamp[32];
    char user[64];
    char action[128];
} LogEntry;

/* Shared memory layout for the activity log (IPC) */
typedef struct {
    int      count;                      /* total entries written   */
    LogEntry entries[MAX_LOG_ENTRIES];    /* circular buffer         */
} SharedLog;

/* ── Globals ─────────────────────────────────────────────────────────────── */
static int             server_fd   = -1;
static pthread_mutex_t slot_mutex  = PTHREAD_MUTEX_INITIALIZER;
static sem_t           client_sem;       /* limits concurrent clients       */
static int             shm_id     = -1;
static SharedLog      *shared_log = NULL;
static pthread_mutex_t log_mutex  = PTHREAD_MUTEX_INITIALIZER;
static volatile int    running    = 1;

/* ── Utility: get current timestamp string ───────────────────────────────── */
static void get_timestamp(char *buf, size_t len) {
    time_t     now = time(NULL);
    struct tm *t   = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", t);
}

/* ── Shared-Memory Activity Log (IPC - Concept 4.6) ─────────────────────── */
static void init_shared_memory(void) {
    shm_id = shmget(SHM_KEY, sizeof(SharedLog), IPC_CREAT | 0666);
    if (shm_id < 0) { perror("shmget"); exit(EXIT_FAILURE); }

    shared_log = (SharedLog *)shmat(shm_id, NULL, 0);
    if (shared_log == (void *)-1) { perror("shmat"); exit(EXIT_FAILURE); }

    shared_log->count = 0;
    printf("[INIT] Shared memory segment created (IPC).\n");
}

static void add_log_entry(const char *user, const char *action) {
    pthread_mutex_lock(&log_mutex);
    int idx = shared_log->count % MAX_LOG_ENTRIES;
    get_timestamp(shared_log->entries[idx].timestamp,
                  sizeof(shared_log->entries[idx].timestamp));
    strncpy(shared_log->entries[idx].user, user, 63);
    strncpy(shared_log->entries[idx].action, action, 127);
    shared_log->count++;
    pthread_mutex_unlock(&log_mutex);
}

static void format_log(char *buf, size_t buf_size) {
    pthread_mutex_lock(&log_mutex);
    int total = shared_log->count;
    int start = (total > MAX_LOG_ENTRIES) ? total - MAX_LOG_ENTRIES : 0;
    int n     = (total > MAX_LOG_ENTRIES) ? MAX_LOG_ENTRIES : total;

    snprintf(buf, buf_size, "=== ACTIVITY LOG (%d entries) ===\n", n);
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % MAX_LOG_ENTRIES;
        char line[256];
        snprintf(line, sizeof(line), "[%s] %-12s | %s\n",
                 shared_log->entries[idx].timestamp,
                 shared_log->entries[idx].user,
                 shared_log->entries[idx].action);
        strncat(buf, line, buf_size - strlen(buf) - 1);
    }
    pthread_mutex_unlock(&log_mutex);
}

/* ── File Locking Helpers (Concept 4.2) ──────────────────────────────────── */
static void lock_shared(int fd)    { flock(fd, LOCK_SH); }
static void lock_exclusive(int fd) { flock(fd, LOCK_EX); }
static void unlock_file(int fd)    { flock(fd, LOCK_UN); }

/* ── Default Data File Initialisation ────────────────────────────────────── */
static void init_users_file(void) {
    if (access(USERS_FILE, F_OK) == 0) return;
    FILE *fp = fopen(USERS_FILE, "w");
    if (!fp) { perror("fopen users"); exit(EXIT_FAILURE); }
    fprintf(fp, "admin,admin123,0\n");
    fprintf(fp, "operator1,op123,1\n");
    fprintf(fp, "user1,user123,2\n");
    fclose(fp);
    printf("[INIT] Default %s created.\n", USERS_FILE);
}

static void init_parking_file(void) {
    if (access(PARKING_FILE, F_OK) == 0) return;
    FILE *fp = fopen(PARKING_FILE, "w");
    if (!fp) { perror("fopen parking"); exit(EXIT_FAILURE); }
    for (int i = 1; i <= MAX_SLOTS; i++)
        fprintf(fp, "%d,0,NONE\n", i);
    fclose(fp);
    printf("[INIT] Default %s created (%d slots).\n", PARKING_FILE, MAX_SLOTS);
}

/* ── Authentication (Concept 4.1 - Role-Based Auth) ──────────────────────── */
static int authenticate(const char *uname, const char *passwd, int *role_out) {
    int fd = open(USERS_FILE, O_RDONLY);
    if (fd < 0) return 0;
    lock_shared(fd);                        /* shared lock for reading */

    FILE *fp = fdopen(dup(fd), "r");
    if (!fp) { unlock_file(fd); close(fd); return 0; }

    char line[256];
    int  found = 0;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';
        char u[64], p[64];
        int  r;
        if (sscanf(line, "%63[^,],%63[^,],%d", u, p, &r) == 3) {
            if (strcmp(u, uname) == 0 && strcmp(p, passwd) == 0) {
                *role_out = r;
                found = 1;
                break;
            }
        }
    }
    fclose(fp);
    unlock_file(fd);
    close(fd);
    return found;
}

/* ── Parking Slot Operations ─────────────────────────────────────────────── */

/* VIEW SLOTS — uses shared (read) file lock */
static void view_slots(char *response, size_t resp_size) {
    int fd = open(PARKING_FILE, O_RDONLY);
    if (fd < 0) { snprintf(response, resp_size, "ERROR: Cannot open parking data.\n"); return; }
    lock_shared(fd);

    FILE *fp = fdopen(dup(fd), "r");
    snprintf(response, resp_size,
             "\n%-8s %-10s %-15s\n"
             "--------------------------------------\n",
             "Slot#", "Status", "Booked By");

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        int sid, occ; char who[64];
        if (sscanf(line, "%d,%d,%63s", &sid, &occ, who) == 3) {
            char row[128];
            snprintf(row, sizeof(row), "%-8d %-10s %-15s\n",
                     sid, occ ? "BOOKED" : "FREE", occ ? who : "---");
            strncat(response, row, resp_size - strlen(response) - 1);
        }
    }
    fclose(fp);
    unlock_file(fd);
    close(fd);
}

/* Helper: read all slots from file (caller must hold appropriate lock) */
static int read_slots(ParkingSlot slots[], int max) {
    FILE *fp = fopen(PARKING_FILE, "r");
    if (!fp) return 0;
    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) && n < max) {
        sscanf(line, "%d,%d,%63s", &slots[n].slot_id,
               &slots[n].occupied, slots[n].booked_by);
        n++;
    }
    fclose(fp);
    return n;
}

/* Helper: write all slots back to file */
static void write_slots(ParkingSlot slots[], int n) {
    FILE *fp = fopen(PARKING_FILE, "w");
    if (!fp) return;
    for (int i = 0; i < n; i++)
        fprintf(fp, "%d,%d,%s\n", slots[i].slot_id,
                slots[i].occupied, slots[i].booked_by);
    fclose(fp);
}

/*
 * BOOK SLOT — Concurrency Control (Concept 4.3) & Data Consistency (4.4)
 * Uses: mutex lock → exclusive file lock → check → modify → unlock
 */
static void book_slot(int slot_id, const char *username,
                      char *response, size_t resp_size) {
    if (slot_id < 1 || slot_id > MAX_SLOTS) {
        snprintf(response, resp_size, "ERROR: Invalid slot number (1-%d).\n", MAX_SLOTS);
        return;
    }

    pthread_mutex_lock(&slot_mutex);            /* Concept 4.3: mutex      */

    int fd = open(PARKING_FILE, O_RDWR);
    if (fd < 0) {
        pthread_mutex_unlock(&slot_mutex);
        snprintf(response, resp_size, "ERROR: Cannot open parking data.\n");
        return;
    }
    lock_exclusive(fd);                          /* Concept 4.2: file lock  */

    ParkingSlot slots[MAX_SLOTS];
    int n = read_slots(slots, MAX_SLOTS);

    int idx = slot_id - 1;
    if (idx >= n) {
        snprintf(response, resp_size, "ERROR: Slot %d not found.\n", slot_id);
    } else if (slots[idx].occupied) {
        /* Concept 4.4: prevent double booking */
        snprintf(response, resp_size,
                 "ERROR: Slot %d is already booked by %s.\n",
                 slot_id, slots[idx].booked_by);
    } else {
        slots[idx].occupied = 1;
        strncpy(slots[idx].booked_by, username, 63);
        write_slots(slots, n);
        snprintf(response, resp_size,
                 "SUCCESS: Slot %d booked by %s.\n", slot_id, username);
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "Booked slot %d", slot_id);
        add_log_entry(username, log_msg);
    }

    unlock_file(fd);
    close(fd);
    pthread_mutex_unlock(&slot_mutex);
}

/* RELEASE SLOT */
static void release_slot(int slot_id, const char *username, int role,
                         char *response, size_t resp_size) {
    if (slot_id < 1 || slot_id > MAX_SLOTS) {
        snprintf(response, resp_size, "ERROR: Invalid slot number (1-%d).\n", MAX_SLOTS);
        return;
    }

    pthread_mutex_lock(&slot_mutex);

    int fd = open(PARKING_FILE, O_RDWR);
    if (fd < 0) {
        pthread_mutex_unlock(&slot_mutex);
        snprintf(response, resp_size, "ERROR: Cannot open parking data.\n");
        return;
    }
    lock_exclusive(fd);

    ParkingSlot slots[MAX_SLOTS];
    int n = read_slots(slots, MAX_SLOTS);
    int idx = slot_id - 1;

    if (idx >= n) {
        snprintf(response, resp_size, "ERROR: Slot %d not found.\n", slot_id);
    } else if (!slots[idx].occupied) {
        snprintf(response, resp_size, "ERROR: Slot %d is already free.\n", slot_id);
    } else if (role == ROLE_USER && strcmp(slots[idx].booked_by, username) != 0) {
        /* Role-based: USERs can only release their own slots */
        snprintf(response, resp_size,
                 "ERROR: Access denied. Slot %d is booked by %s.\n",
                 slot_id, slots[idx].booked_by);
    } else {
        char prev_user[64];
        strncpy(prev_user, slots[idx].booked_by, 63);
        slots[idx].occupied = 0;
        strcpy(slots[idx].booked_by, "NONE");
        write_slots(slots, n);
        snprintf(response, resp_size,
                 "SUCCESS: Slot %d released (was booked by %s).\n",
                 slot_id, prev_user);
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "Released slot %d", slot_id);
        add_log_entry(username, log_msg);
    }

    unlock_file(fd);
    close(fd);
    pthread_mutex_unlock(&slot_mutex);
}

/* ── Admin Operations ────────────────────────────────────────────────────── */

static void add_user(const char *new_user, const char *new_pass, int new_role,
                     char *response, size_t resp_size) {
    int fd = open(USERS_FILE, O_RDWR | O_APPEND);
    if (fd < 0) {
        snprintf(response, resp_size, "ERROR: Cannot open users file.\n");
        return;
    }
    lock_exclusive(fd);

    /* Check if user already exists */
    FILE *fp = fdopen(dup(fd), "r");
    char line[256];
    while (fp && fgets(line, sizeof(line), fp)) {
        char u[64];
        if (sscanf(line, "%63[^,]", u) == 1 && strcmp(u, new_user) == 0) {
            fclose(fp);
            unlock_file(fd); close(fd);
            snprintf(response, resp_size, "ERROR: User '%s' already exists.\n", new_user);
            return;
        }
    }
    if (fp) fclose(fp);

    /* Append new user */
    char entry[256];
    int len = snprintf(entry, sizeof(entry), "%s,%s,%d\n", new_user, new_pass, new_role);
    write(fd, entry, len);

    unlock_file(fd);
    close(fd);
    snprintf(response, resp_size, "SUCCESS: User '%s' added with role %d.\n",
             new_user, new_role);
}

static void remove_user(const char *target, const char *requester,
                        char *response, size_t resp_size) {
    if (strcmp(target, requester) == 0) {
        snprintf(response, resp_size, "ERROR: Cannot remove yourself.\n");
        return;
    }

    int fd = open(USERS_FILE, O_RDWR);
    if (fd < 0) {
        snprintf(response, resp_size, "ERROR: Cannot open users file.\n");
        return;
    }
    lock_exclusive(fd);

    FILE *fp = fdopen(dup(fd), "r");
    User users[MAX_USERS];
    int n = 0, found = -1;
    char line[256];
    while (fp && fgets(line, sizeof(line), fp) && n < MAX_USERS) {
        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "%63[^,],%63[^,],%d",
                   users[n].username, users[n].password, &users[n].role) == 3) {
            if (strcmp(users[n].username, target) == 0) found = n;
            n++;
        }
    }
    if (fp) fclose(fp);

    if (found < 0) {
        unlock_file(fd); close(fd);
        snprintf(response, resp_size, "ERROR: User '%s' not found.\n", target);
        return;
    }

    /* Rewrite file without the removed user */
    FILE *fw = fopen(USERS_FILE, "w");
    for (int i = 0; i < n; i++) {
        if (i != found)
            fprintf(fw, "%s,%s,%d\n", users[i].username,
                    users[i].password, users[i].role);
    }
    fclose(fw);

    unlock_file(fd);
    close(fd);
    snprintf(response, resp_size, "SUCCESS: User '%s' removed.\n", target);
}

static void view_users(char *response, size_t resp_size) {
    int fd = open(USERS_FILE, O_RDONLY);
    if (fd < 0) { snprintf(response, resp_size, "ERROR: Cannot open users file.\n"); return; }
    lock_shared(fd);

    FILE *fp = fdopen(dup(fd), "r");
    const char *role_names[] = {"ADMIN", "OPERATOR", "USER"};
    snprintf(response, resp_size,
             "\n%-15s %-12s\n"
             "-----------------------------\n", "Username", "Role");
    char line[256];
    while (fp && fgets(line, sizeof(line), fp)) {
        char u[64], p[64]; int r;
        if (sscanf(line, "%63[^,],%63[^,],%d", u, p, &r) == 3) {
            char row[128];
            snprintf(row, sizeof(row), "%-15s %-12s\n", u,
                     (r >= 0 && r <= 2) ? role_names[r] : "UNKNOWN");
            strncat(response, row, resp_size - strlen(response) - 1);
        }
    }
    if (fp) fclose(fp);
    unlock_file(fd); close(fd);
}

/* ── Client Handler Thread (Concepts 4.3, 4.5) ──────────────────────────── */
static void *handle_client(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    char buf[BUF_SIZE], response[BUF_SIZE];

    /* ── Step 1: Authentication ── */
    memset(buf, 0, BUF_SIZE);
    int bytes = recv(client_fd, buf, BUF_SIZE - 1, 0);
    if (bytes <= 0) goto cleanup;

    char username[64], password[64];
    if (sscanf(buf, "%63s %63s", username, password) != 2) {
        send(client_fd, "AUTH_FAIL", 9, 0);
        goto cleanup;
    }

    int role;
    if (!authenticate(username, password, &role)) {
        send(client_fd, "AUTH_FAIL", 9, 0);
        printf("[AUTH] Failed login attempt: %s\n", username);
        goto cleanup;
    }

    /* Send auth success with role */
    snprintf(response, BUF_SIZE, "AUTH_OK %d", role);
    send(client_fd, response, strlen(response), 0);
    printf("[AUTH] User '%s' logged in (role=%d).\n", username, role);

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Logged in (role=%s)",
             role == 0 ? "ADMIN" : role == 1 ? "OPERATOR" : "USER");
    add_log_entry(username, log_msg);

    /* ── Step 2: Command Loop ── */
    while (running) {
        memset(buf, 0, BUF_SIZE);
        bytes = recv(client_fd, buf, BUF_SIZE - 1, 0);
        if (bytes <= 0) break;
        buf[bytes] = '\0';

        /* Trim newline */
        buf[strcspn(buf, "\r\n")] = '\0';

        memset(response, 0, BUF_SIZE);

        if (strcmp(buf, "VIEW_SLOTS") == 0) {
            /* All roles can view */
            view_slots(response, BUF_SIZE);

        } else if (strncmp(buf, "BOOK_SLOT", 9) == 0) {
            int sid;
            if (sscanf(buf + 9, "%d", &sid) == 1)
                book_slot(sid, username, response, BUF_SIZE);
            else
                snprintf(response, BUF_SIZE, "ERROR: Usage: BOOK_SLOT <slot_id>\n");

        } else if (strncmp(buf, "RELEASE_SLOT", 12) == 0) {
            int sid;
            if (sscanf(buf + 12, "%d", &sid) == 1)
                release_slot(sid, username, role, response, BUF_SIZE);
            else
                snprintf(response, BUF_SIZE, "ERROR: Usage: RELEASE_SLOT <slot_id>\n");

        } else if (strcmp(buf, "VIEW_LOG") == 0) {
            if (role == ROLE_USER) {
                snprintf(response, BUF_SIZE, "ERROR: Access denied. Admins/Operators only.\n");
            } else {
                format_log(response, BUF_SIZE);
            }

        } else if (strncmp(buf, "ADD_USER", 8) == 0) {
            if (role != ROLE_ADMIN) {
                snprintf(response, BUF_SIZE, "ERROR: Access denied. Admins only.\n");
            } else {
                char nu[64], np[64]; int nr;
                if (sscanf(buf + 8, "%63s %63s %d", nu, np, &nr) == 3) {
                    if (nr < 0 || nr > 2)
                        snprintf(response, BUF_SIZE, "ERROR: Role must be 0(ADMIN), 1(OPERATOR), or 2(USER).\n");
                    else
                        add_user(nu, np, nr, response, BUF_SIZE);
                } else {
                    snprintf(response, BUF_SIZE, "ERROR: Usage: ADD_USER <username> <password> <role 0/1/2>\n");
                }
            }

        } else if (strncmp(buf, "REMOVE_USER", 11) == 0) {
            if (role != ROLE_ADMIN) {
                snprintf(response, BUF_SIZE, "ERROR: Access denied. Admins only.\n");
            } else {
                char tu[64];
                if (sscanf(buf + 11, "%63s", tu) == 1)
                    remove_user(tu, username, response, BUF_SIZE);
                else
                    snprintf(response, BUF_SIZE, "ERROR: Usage: REMOVE_USER <username>\n");
            }

        } else if (strcmp(buf, "VIEW_USERS") == 0) {
            if (role != ROLE_ADMIN) {
                snprintf(response, BUF_SIZE, "ERROR: Access denied. Admins only.\n");
            } else {
                view_users(response, BUF_SIZE);
            }

        } else if (strcmp(buf, "LOGOUT") == 0) {
            snprintf(response, BUF_SIZE, "GOODBYE");
            send(client_fd, response, strlen(response), 0);
            printf("[INFO] User '%s' logged out.\n", username);
            add_log_entry(username, "Logged out");
            break;

        } else {
            snprintf(response, BUF_SIZE, "ERROR: Unknown command.\n");
        }

        send(client_fd, response, strlen(response), 0);
    }

cleanup:
    close(client_fd);
    sem_post(&client_sem);       /* Concept 4.3: release semaphore slot */
    return NULL;
}

/* ── Signal Handler for Graceful Shutdown ────────────────────────────────── */
static void sigint_handler(int sig) {
    (void)sig;
    printf("\n[SERVER] Shutting down...\n");
    running = 0;
    if (server_fd >= 0) close(server_fd);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    /* Register signal handler */
    signal(SIGINT, sigint_handler);

    printf("=============================================\n");
    printf("  PARKING SLOT MANAGEMENT SYSTEM - SERVER\n");
    printf("=============================================\n\n");

    /* Initialise data files */
    init_users_file();
    init_parking_file();

    /* Initialise shared memory for activity log (Concept 4.6: IPC) */
    init_shared_memory();

    /* Initialise semaphore to limit concurrent clients (Concept 4.3) */
    sem_init(&client_sem, 0, MAX_CLIENTS);
    printf("[INIT] Semaphore initialised (max %d clients).\n", MAX_CLIENTS);

    /* ── Socket setup (Concept 4.5) ── */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(EXIT_FAILURE);
    }

    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("listen"); exit(EXIT_FAILURE);
    }

    printf("[SERVER] Listening on port %d...\n\n", PORT);
    add_log_entry("SYSTEM", "Server started");

    /* ── Accept loop ── */
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int *client_fd = malloc(sizeof(int));
        *client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &clen);

        if (*client_fd < 0) {
            free(client_fd);
            if (!running) break;
            perror("accept");
            continue;
        }

        printf("[CONN] New connection from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        /* Concept 4.3: semaphore limits concurrent connections */
        sem_wait(&client_sem);

        /* Concept 4.3: spawn thread per client */
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, client_fd);
        pthread_detach(tid);
    }

    /* Cleanup */
    sem_destroy(&client_sem);
    pthread_mutex_destroy(&slot_mutex);
    pthread_mutex_destroy(&log_mutex);
    if (shared_log) shmdt(shared_log);
    if (shm_id >= 0) shmctl(shm_id, IPC_RMID, NULL);
    printf("[SERVER] Cleanup complete. Goodbye.\n");

    return 0;
}
