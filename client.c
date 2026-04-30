/*
 * ============================================================================
 * PARKING SLOT MANAGEMENT SYSTEM - CLIENT
 * ============================================================================
 * EGC 301P - Operating Systems Lab Mini Project
 *
 * Connects to the parking server via TCP sockets (Concept 4.5).
 * Provides an interactive menu-driven interface with role-based options.
 *
 * Compile: gcc -o client client.c
 * Run:     ./client
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 8080
#define BUF_SIZE    2048

#define ROLE_ADMIN    0
#define ROLE_OPERATOR 1
#define ROLE_USER     2

/* ── Send a command and receive the server's response ────────────────────── */
static int send_command(int sock, const char *cmd, char *resp, size_t resp_size) {
    if (send(sock, cmd, strlen(cmd), 0) < 0) {
        perror("send");
        return -1;
    }
    memset(resp, 0, resp_size);
    int bytes = recv(sock, resp, resp_size - 1, 0);
    if (bytes <= 0) {
        printf("Server disconnected.\n");
        return -1;
    }
    resp[bytes] = '\0';
    return 0;
}

/* ── Display the menu based on user role ─────────────────────────────────── */
static void show_menu(int role) {
    const char *role_name = (role == ROLE_ADMIN)    ? "ADMIN" :
                            (role == ROLE_OPERATOR) ? "OPERATOR" : "USER";
    printf("\n");
    printf("╔══════════════════════════════════════════╗\n");
    printf("║     PARKING MANAGEMENT SYSTEM            ║\n");
    printf("║     Logged in as: %-23s║\n", role_name);
    printf("╠══════════════════════════════════════════╣\n");
    printf("║  [1] View Parking Slots                  ║\n");
    printf("║  [2] Book a Slot                         ║\n");
    printf("║  [3] Release a Slot                      ║\n");
    if (role == ROLE_ADMIN || role == ROLE_OPERATOR) {
    printf("║  [4] View Activity Log                   ║\n");
    }
    if (role == ROLE_ADMIN) {
    printf("║  [5] Add User                            ║\n");
    printf("║  [6] Remove User                         ║\n");
    printf("║  [7] View All Users                      ║\n");
    }
    printf("║  [8] Logout                              ║\n");
    printf("╚══════════════════════════════════════════╝\n");
    printf("Enter choice: ");
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    int sock;
    struct sockaddr_in serv_addr;
    char buf[BUF_SIZE], response[BUF_SIZE];

    printf("=============================================\n");
    printf("  PARKING SLOT MANAGEMENT SYSTEM - CLIENT\n");
    printf("=============================================\n\n");

    /* ── Create socket (Concept 4.5: Socket Programming) ── */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); exit(EXIT_FAILURE); }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton"); exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        printf("ERROR: Could not connect to server at %s:%d\n", SERVER_IP, SERVER_PORT);
        printf("Make sure the server is running first.\n");
        exit(EXIT_FAILURE);
    }

    printf("Connected to server at %s:%d\n\n", SERVER_IP, SERVER_PORT);

    /* ── Login ── */
    char username[64], password[64];
    printf("--- Login ---\n");
    printf("Username: ");
    scanf("%63s", username);
    printf("Password: ");
    scanf("%63s", password);

    snprintf(buf, BUF_SIZE, "%s %s", username, password);
    if (send_command(sock, buf, response, BUF_SIZE) < 0) {
        close(sock);
        return 1;
    }

    int role;
    if (strncmp(response, "AUTH_OK", 7) == 0) {
        sscanf(response + 8, "%d", &role);
        const char *rn = (role == 0) ? "ADMIN" : (role == 1) ? "OPERATOR" : "USER";
        printf("\nLogin successful! Role: %s\n", rn);
    } else {
        printf("\nLogin failed. Invalid credentials.\n");
        close(sock);
        return 1;
    }

    /* ── Command Loop ── */
    int running = 1;
    while (running) {
        show_menu(role);

        int choice;
        if (scanf("%d", &choice) != 1) {
            /* Clear invalid input */
            while (getchar() != '\n');
            printf("Invalid input. Please enter a number.\n");
            continue;
        }

        switch (choice) {
        case 1: /* View Slots */
            if (send_command(sock, "VIEW_SLOTS", response, BUF_SIZE) == 0)
                printf("%s", response);
            break;

        case 2: { /* Book Slot */
            int sid;
            printf("Enter slot number to book (1-20): ");
            scanf("%d", &sid);
            snprintf(buf, BUF_SIZE, "BOOK_SLOT %d", sid);
            if (send_command(sock, buf, response, BUF_SIZE) == 0)
                printf("%s", response);
            break;
        }

        case 3: { /* Release Slot */
            int sid;
            printf("Enter slot number to release (1-20): ");
            scanf("%d", &sid);
            snprintf(buf, BUF_SIZE, "RELEASE_SLOT %d", sid);
            if (send_command(sock, buf, response, BUF_SIZE) == 0)
                printf("%s", response);
            break;
        }

        case 4: /* View Log */
            if (role == ROLE_USER) {
                printf("Access denied. Admins/Operators only.\n");
            } else {
                if (send_command(sock, "VIEW_LOG", response, BUF_SIZE) == 0)
                    printf("%s", response);
            }
            break;

        case 5: { /* Add User (Admin) */
            if (role != ROLE_ADMIN) {
                printf("Access denied. Admins only.\n");
                break;
            }
            char nu[64], np[64];
            int  nr;
            printf("New username: ");
            scanf("%63s", nu);
            printf("New password: ");
            scanf("%63s", np);
            printf("Role (0=ADMIN, 1=OPERATOR, 2=USER): ");
            scanf("%d", &nr);
            snprintf(buf, BUF_SIZE, "ADD_USER %s %s %d", nu, np, nr);
            if (send_command(sock, buf, response, BUF_SIZE) == 0)
                printf("%s", response);
            break;
        }

        case 6: { /* Remove User (Admin) */
            if (role != ROLE_ADMIN) {
                printf("Access denied. Admins only.\n");
                break;
            }
            char tu[64];
            printf("Username to remove: ");
            scanf("%63s", tu);
            snprintf(buf, BUF_SIZE, "REMOVE_USER %s", tu);
            if (send_command(sock, buf, response, BUF_SIZE) == 0)
                printf("%s", response);
            break;
        }

        case 7: /* View Users (Admin) */
            if (role != ROLE_ADMIN) {
                printf("Access denied. Admins only.\n");
            } else {
                if (send_command(sock, "VIEW_USERS", response, BUF_SIZE) == 0)
                    printf("%s", response);
            }
            break;

        case 8: /* Logout */
            send_command(sock, "LOGOUT", response, BUF_SIZE);
            printf("Logged out. Goodbye!\n");
            running = 0;
            break;

        default:
            printf("Invalid option. Try again.\n");
            break;
        }
    }

    close(sock);
    return 0;
}
