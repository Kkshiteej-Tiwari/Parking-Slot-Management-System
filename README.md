# Client-Server Based Parking Slot Management System with Concurrency Control and IPC

**Course:** EGC 301P — Operating Systems Lab  
**Project Type:** Mini Project (Individual)  
**Language:** C (POSIX / Linux)

---

## Table of Contents

1. [Problem Statement](#1-problem-statement)  
2. [System Architecture](#2-system-architecture)  
3. [Implementation of Required OS Concepts](#3-implementation-of-required-os-concepts)  
   - 3.1 [Role-Based Authorization](#31-role-based-authorization)  
   - 3.2 [File Locking](#32-file-locking)  
   - 3.3 [Concurrency Control](#33-concurrency-control)  
   - 3.4 [Data Consistency](#34-data-consistency)  
   - 3.5 [Socket Programming](#35-socket-programming)  
   - 3.6 [Inter-Process Communication (IPC)](#36-inter-process-communication-ipc)  
4. [File Structure & Data Formats](#4-file-structure--data-formats)  
5. [Features & Command Reference](#5-features--command-reference)  
6. [Compilation & Execution](#6-compilation--execution)  
7. [Sample Output](#7-sample-output)  
8. [Challenges Faced & Solutions](#8-challenges-faced--solutions)  
9. [Conclusion](#9-conclusion)  

---

## 1. Problem Statement

Managing parking slots in a multi-user environment presents challenges of concurrent access, data integrity, and role-based access control. When multiple users attempt to book or release the same parking slot simultaneously, issues such as double booking, lost updates, and race conditions can occur.

This project implements a **multi-user parking slot management system** using a **client-server architecture** in C. The system allows:

- **Users** to view available parking slots, book a slot, and release their own slots.  
- **Operators** to perform all user operations plus view the system activity log.  
- **Administrators** to perform all operations plus manage user accounts (add/remove users, view all users).  

The server handles multiple simultaneous client connections using **multithreading** with **mutexes** and **semaphores** for synchronization. **File locking** (`flock`) is used for safe concurrent file access. **TCP sockets** provide client-server communication, and **POSIX shared memory** serves as an IPC mechanism for a centralized activity log.

---

## 2. System Architecture

```
 ┌─────────────┐       TCP Socket (port 8080)       ┌─────────────────────────┐
 │  client.c   │ ◄─────────────────────────────────► │       server.c          │
 │             │     Commands / Responses            │                         │
 │ • Login UI  │                                     │ • Authentication module │
 │ • Menu UI   │                                     │ • Thread per client     │
 │ • Send cmds │                                     │ • Mutex + Semaphore     │
 └─────────────┘                                     │ • File locking (flock)  │
                                                     │ • Shared memory log     │
 ┌─────────────┐                                     └────────────┬────────────┘
 │  client.c   │ ◄──── (multiple clients) ────────►               │
 └─────────────┘                                        ┌─────────┴──────────┐
                                                        │  parking_data.txt  │
 ┌─────────────┐                                        │  users.txt         │
 │  client.c   │ ◄──── (multiple clients) ────────►     │  Shared Memory Log │
 └─────────────┘                                        └────────────────────┘
```

**Source files:**

| File | Lines | Description |
|------|-------|-------------|
| `server.c` | 666 | Multi-threaded server — all core OS logic |
| `client.c` | 233 | Interactive terminal client |

---

## 3. Implementation of Required OS Concepts

### 3.1 Role-Based Authorization

**Guideline Requirement (Section 4.1):** Implement user roles, define access control, restrict operations by permissions.

**Implementation:**

Three roles are defined with integer constants:

```c
#define ROLE_ADMIN      0
#define ROLE_OPERATOR   1
#define ROLE_USER       2
```

User credentials are stored in `users.txt` in CSV format (`username,password,role`). On login, the server reads the file and validates the credentials via the `authenticate()` function (server.c, line 165–191). The authenticated role is sent back to the client and stored for the session.

Every command in the `handle_client()` thread checks the role before execution:

```c
// Example: Only ADMIN can add users
} else if (strncmp(buf, "ADD_USER", 8) == 0) {
    if (role != ROLE_ADMIN) {
        snprintf(response, BUF_SIZE, "ERROR: Access denied. Admins only.\n");
    } else {
        // ... perform add_user operation
    }
}
```

**Permission Matrix:**

| Command | ADMIN | OPERATOR | USER |
|---------|:-----:|:--------:|:----:|
| VIEW_SLOTS | ✔ | ✔ | ✔ |
| BOOK_SLOT | ✔ | ✔ | ✔ |
| RELEASE_SLOT (own) | ✔ | ✔ | ✔ |
| RELEASE_SLOT (any) | ✔ | ✔ | ✘ |
| VIEW_LOG | ✔ | ✔ | ✘ |
| ADD_USER | ✔ | ✘ | ✘ |
| REMOVE_USER | ✔ | ✘ | ✘ |
| VIEW_USERS | ✔ | ✘ | ✘ |

For `RELEASE_SLOT`, an additional ownership check prevents regular users from releasing slots booked by others (server.c, line 321–325):

```c
} else if (role == ROLE_USER && strcmp(slots[idx].booked_by, username) != 0) {
    snprintf(response, resp_size,
             "ERROR: Access denied. Slot %d is booked by %s.\n",
             slot_id, slots[idx].booked_by);
}
```

---

### 3.2 File Locking

**Guideline Requirement (Section 4.2):** Ensure safe file access with advisory/mandatory locking and read/write locks.

**Implementation:**

Three helper functions wrap the `flock()` system call (server.c, lines 138–140):

```c
static void lock_shared(int fd)    { flock(fd, LOCK_SH); }   // Read lock
static void lock_exclusive(int fd) { flock(fd, LOCK_EX); }   // Write lock
static void unlock_file(int fd)    { flock(fd, LOCK_UN); }   // Unlock
```

**Usage pattern:**

- **Read operations** (`view_slots`, `authenticate`, `view_users`) use `LOCK_SH` (shared lock) — multiple readers can hold it simultaneously.
- **Write operations** (`book_slot`, `release_slot`, `add_user`, `remove_user`) use `LOCK_EX` (exclusive lock) — blocks all other readers and writers.

Example from `view_slots()` (server.c, lines 196–220):

```c
int fd = open(PARKING_FILE, O_RDONLY);
lock_shared(fd);           // Shared lock — allows concurrent reads

FILE *fp = fdopen(dup(fd), "r");
// ... read and format slot data ...
fclose(fp);

unlock_file(fd);           // Release lock
close(fd);
```

Example from `book_slot()` (server.c, lines 260–292):

```c
int fd = open(PARKING_FILE, O_RDWR);
lock_exclusive(fd);        // Exclusive lock — no concurrent access

ParkingSlot slots[MAX_SLOTS];
int n = read_slots(slots, MAX_SLOTS);
// ... modify slot data ...
write_slots(slots, n);

unlock_file(fd);           // Release lock
close(fd);
```

This ensures that no two threads can write to the parking data file at the same time, and that reads are not served stale data during a write.

---

### 3.3 Concurrency Control

**Guideline Requirement (Section 4.3):** Handle multiple processes/threads executing simultaneously using mutexes and semaphores.

**Implementation:**

**a) Pthreads — Thread-per-Client Model**

The server spawns a new POSIX thread for each incoming client connection (server.c, lines 650–653):

```c
pthread_t tid;
pthread_create(&tid, NULL, handle_client, client_fd);
pthread_detach(tid);
```

Each thread runs `handle_client()`, which independently handles authentication and command processing for that client.

**b) Semaphore — Connection Limiting**

A POSIX semaphore limits the maximum number of concurrent clients to 10 (server.c, line 603):

```c
sem_init(&client_sem, 0, MAX_CLIENTS);   // Initialise with count 10
```

Before spawning a thread, the server waits on the semaphore (line 648):

```c
sem_wait(&client_sem);    // Decrements count; blocks if count == 0
```

When a client disconnects, the thread posts to the semaphore (line 574):

```c
sem_post(&client_sem);    // Increments count; unblocks a waiting accept
```

**c) Mutex — Critical Section Protection**

A `pthread_mutex_t` protects the critical section in slot booking and release operations (server.c, line 81):

```c
static pthread_mutex_t slot_mutex = PTHREAD_MUTEX_INITIALIZER;
```

Used in `book_slot()` (lines 258, 292) and `release_slot()` (lines 303, 342):

```c
pthread_mutex_lock(&slot_mutex);
// ... critical section: read → check → modify → write ...
pthread_mutex_unlock(&slot_mutex);
```

A separate `log_mutex` protects the shared memory log writes (line 85).

---

### 3.4 Data Consistency

**Guideline Requirement (Section 4.4):** Maintain correctness under concurrent access; prevent race conditions, dirty reads, and lost updates.

**Implementation:**

Data consistency is achieved through a **two-layer locking strategy**:

1. **Mutex** (application-level) — serialises all slot modification operations across threads.
2. **File lock** (OS-level) — ensures no external process can corrupt the file during a read-modify-write cycle.

**Preventing Double Booking:**

Inside the mutex-protected critical section, `book_slot()` performs an atomic read-check-write (server.c, lines 268–288):

```c
pthread_mutex_lock(&slot_mutex);           // Layer 1: Mutex
lock_exclusive(fd);                         // Layer 2: File lock

ParkingSlot slots[MAX_SLOTS];
int n = read_slots(slots, MAX_SLOTS);       // READ

if (slots[idx].occupied) {                  // CHECK
    // Slot already booked — reject
} else {
    slots[idx].occupied = 1;                // MODIFY
    write_slots(slots, n);                  // WRITE
}

unlock_file(fd);
pthread_mutex_unlock(&slot_mutex);
```

This guarantees:

- **No race conditions:** Only one thread can enter the booking critical section at a time.
- **No dirty reads:** The file lock prevents reading partially-written data.
- **No lost updates:** The mutex ensures read-modify-write is atomic — no interleaving between threads.
- **No double booking:** The slot status is checked inside the locked section, so two threads cannot both see a slot as free.

---

### 3.5 Socket Programming

**Guideline Requirement (Section 4.5):** Implement client-server communication using sockets.

**Implementation:**

The system uses **TCP sockets** (`AF_INET`, `SOCK_STREAM`) for reliable, connection-oriented communication.

**Server side** (server.c, lines 607–627):

```c
server_fd = socket(AF_INET, SOCK_STREAM, 0);        // Create socket
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, ...);  // Allow port reuse

struct sockaddr_in addr;
addr.sin_family      = AF_INET;
addr.sin_addr.s_addr = INADDR_ANY;                   // Bind to all interfaces
addr.sin_port        = htons(PORT);                   // Port 8080

bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
listen(server_fd, MAX_CLIENTS);                       // Backlog of 10

// Accept loop
while (running) {
    int *client_fd = malloc(sizeof(int));
    *client_fd = accept(server_fd, ...);              // Accept connection
    // ... spawn thread ...
}
```

**Client side** (client.c, lines 82–101):

```c
sock = socket(AF_INET, SOCK_STREAM, 0);              // Create socket

serv_addr.sin_family = AF_INET;
serv_addr.sin_port   = htons(SERVER_PORT);
inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr);  // 127.0.0.1

connect(sock, (struct sockaddr *)&serv_addr, ...);    // Connect to server
```

**Protocol:** Text-based command/response. The client sends a command string (e.g., `"BOOK_SLOT 5"`), and the server sends back a response string. Communication uses `send()` and `recv()` calls.

---

### 3.6 Inter-Process Communication (IPC)

**Guideline Requirement (Section 4.6):** Demonstrate IPC using at least one mechanism (shared memory, message queues, pipes, or signals).

**Implementation:**

**POSIX Shared Memory** is used to implement a centralized activity log that is accessible across all server threads and persists in kernel-managed memory.

**Shared memory segment creation** (server.c, lines 96–105):

```c
static void init_shared_memory(void) {
    shm_id = shmget(SHM_KEY, sizeof(SharedLog), IPC_CREAT | 0666);
    shared_log = (SharedLog *)shmat(shm_id, NULL, 0);
    shared_log->count = 0;
}
```

**Data structure in shared memory** (server.c, lines 67–77):

```c
typedef struct {
    char timestamp[32];
    char user[64];
    char action[128];
} LogEntry;

typedef struct {
    int      count;                        // Total entries written
    LogEntry entries[MAX_LOG_ENTRIES];      // Circular buffer (100 entries)
} SharedLog;
```

**Writing to the log** — `add_log_entry()` (server.c, lines 107–116):

```c
static void add_log_entry(const char *user, const char *action) {
    pthread_mutex_lock(&log_mutex);
    int idx = shared_log->count % MAX_LOG_ENTRIES;     // Circular index
    get_timestamp(shared_log->entries[idx].timestamp, ...);
    strncpy(shared_log->entries[idx].user, user, 63);
    strncpy(shared_log->entries[idx].action, action, 127);
    shared_log->count++;
    pthread_mutex_unlock(&log_mutex);
}
```

Every significant action (login, logout, booking, releasing) writes a timestamped entry to the shared memory log. The log is a circular buffer — when it exceeds 100 entries, the oldest entries are overwritten.

**Cleanup** on server shutdown (server.c, lines 660–661):

```c
shmdt(shared_log);                        // Detach from shared memory
shmctl(shm_id, IPC_RMID, NULL);           // Remove shared memory segment
```

---

## 4. File Structure & Data Formats

```
OS_PROJECT/
├── server.c              # Server source code (666 lines)
├── client.c              # Client source code (233 lines)
├── report.md             # This report
├── users.txt             # Auto-created: user credentials
└── parking_data.txt      # Auto-created: parking slot states
```

**users.txt** (CSV: username, password, role):

```
admin,admin123,0
operator1,op123,1
user1,user123,2
```

**parking_data.txt** (CSV: slot_id, occupied, booked_by):

```
1,0,NONE
2,1,user1
3,0,NONE
...
20,0,NONE
```

Both files are auto-created with default values when the server starts for the first time.

---

## 5. Features & Command Reference

| # | Feature | Command | Description |
|---|---------|---------|-------------|
| 1 | View Slots | `VIEW_SLOTS` | Displays all 20 slots with status and booker |
| 2 | Book Slot | `BOOK_SLOT <id>` | Books a free slot for the logged-in user |
| 3 | Release Slot | `RELEASE_SLOT <id>` | Releases a booked slot (ownership enforced for USERs) |
| 4 | View Log | `VIEW_LOG` | Shows the activity log from shared memory (ADMIN/OPERATOR) |
| 5 | Add User | `ADD_USER <u> <p> <r>` | Creates a new user account (ADMIN only) |
| 6 | Remove User | `REMOVE_USER <u>` | Deletes a user account (ADMIN only) |
| 7 | View Users | `VIEW_USERS` | Lists all registered users and roles (ADMIN only) |
| 8 | Logout | `LOGOUT` | Disconnects from the server |

---

## 6. Compilation & Execution

```bash
# Compile the server (requires -pthread for POSIX threads)
gcc -o server server.c -pthread

# Compile the client
gcc -o client client.c

# Start the server (Terminal 1)
./server

# Start a client (Terminal 2 — can open multiple terminals)
./client
```

**Default Login Credentials:**

| Username | Password | Role |
|----------|----------|------|
| admin | admin123 | ADMIN |
| operator1 | op123 | OPERATOR |
| user1 | user123 | USER |

---

## 7. Sample Output

### Server Startup

```
=============================================
  PARKING SLOT MANAGEMENT SYSTEM - SERVER
=============================================

[INIT] Default users.txt created.
[INIT] Default parking_data.txt created (20 slots).
[INIT] Shared memory segment created (IPC).
[INIT] Semaphore initialised (max 10 clients).
[SERVER] Listening on port 8080...
```

### Client Login & Booking

```
=============================================
  PARKING SLOT MANAGEMENT SYSTEM - CLIENT
=============================================

Connected to server at 127.0.0.1:8080

--- Login ---
Username: user1
Password: user123

Login successful! Role: USER

╔══════════════════════════════════════════╗
║     PARKING MANAGEMENT SYSTEM            ║
║     Logged in as: USER                   ║
╠══════════════════════════════════════════╣
║  [1] View Parking Slots                  ║
║  [2] Book a Slot                         ║
║  [3] Release a Slot                      ║
║  [8] Logout                              ║
╚══════════════════════════════════════════╝
Enter choice: 2
Enter slot number to book (1-20): 5
SUCCESS: Slot 5 booked by user1.
```

### View Slots Output

```
Slot#    Status     Booked By
--------------------------------------
1        FREE       ---
2        FREE       ---
3        FREE       ---
4        FREE       ---
5        BOOKED     user1
6        FREE       ---
...
20       FREE       ---
```

### Admin — View Activity Log

```
=== ACTIVITY LOG (4 entries) ===
[2026-04-30 21:00:01] SYSTEM       | Server started
[2026-04-30 21:00:15] admin        | Logged in (role=ADMIN)
[2026-04-30 21:00:20] user1        | Logged in (role=USER)
[2026-04-30 21:00:35] user1        | Booked slot 5
```

### Double Booking Prevention

```
# Client 1 books slot 3
Enter slot number to book (1-20): 3
SUCCESS: Slot 3 booked by user1.

# Client 2 tries to book the same slot
Enter slot number to book (1-20): 3
ERROR: Slot 3 is already booked by user1.
```

### Role-Based Access Denial

```
# USER tries option 4 (View Log)
Access denied. Admins/Operators only.

# OPERATOR tries option 5 (Add User)
Access denied. Admins only.
```

---

## 8. Challenges Faced & Solutions

### Challenge 1: Race Condition in Slot Booking

**Problem:** When two clients attempt to book the same slot simultaneously, both threads could read the slot as free, leading to a double booking.

**Solution:** A two-layer locking strategy — `pthread_mutex_lock()` serialises all booking operations at the thread level, and `flock(fd, LOCK_EX)` provides file-level exclusive access. The check-and-modify happens atomically inside both locks.

### Challenge 2: File Descriptor Leaks with fdopen

**Problem:** Using `fdopen()` on a file descriptor ties the fd to the FILE stream — closing the FILE also closes the fd, which conflicts with the separate `flock/close` calls.

**Solution:** Used `fdopen(dup(fd), "r")` to create a duplicate file descriptor for the FILE stream. This allows independent closing of the FILE stream and the original fd used for locking.

### Challenge 3: Concurrent Client Limit

**Problem:** Without a limit, the server could exhaust system resources (threads, file descriptors) with too many simultaneous connections.

**Solution:** A POSIX semaphore initialised to `MAX_CLIENTS (10)` gates the accept loop. Each new connection decrements the semaphore (`sem_wait`), and each disconnection increments it (`sem_post`). When all slots are taken, new connections wait.

### Challenge 4: Safe Shared Memory Access

**Problem:** Multiple threads writing to the shared memory activity log simultaneously could produce corrupted entries.

**Solution:** A separate `pthread_mutex_t log_mutex` protects all reads and writes to the shared memory log. The circular buffer design (modulo indexing) prevents the need for memory reallocation.

### Challenge 5: Graceful Server Shutdown

**Problem:** Pressing Ctrl+C during operation could leave shared memory segments, open sockets, and file locks in an inconsistent state.

**Solution:** A `SIGINT` signal handler sets a `volatile int running` flag to 0 and closes the server socket. The main loop exits cleanly, and the cleanup code destroys the mutex, semaphore, and shared memory segment (`shmctl(IPC_RMID)`).

---

## 9. Conclusion

This project successfully demonstrates all six mandatory OS concepts required by the EGC 301P mini project guidelines:

1. **Role-Based Authorization** — Three-tier access control (Admin/Operator/User) with per-command permission checks.
2. **File Locking** — Advisory locking via `flock()` with shared locks for reads and exclusive locks for writes.
3. **Concurrency Control** — Thread-per-client model using pthreads, mutex for critical section protection, and semaphore for connection limiting.
4. **Data Consistency** — Two-layer locking prevents race conditions, double booking, dirty reads, and lost updates.
5. **Socket Programming** — TCP client-server model with text-based command/response protocol.
6. **IPC (Shared Memory)** — POSIX shared memory (`shmget`/`shmat`) for a circular activity log buffer.

The system is robust, handles edge cases (invalid input, self-deletion prevention, ownership enforcement), and provides a clean, interactive terminal interface for all user roles.
