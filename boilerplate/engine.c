/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Implements a multi-container runtime with:
 *   - Unix domain socket control plane (CLI <-> Supervisor IPC)
 *   - Bounded-buffer logging pipeline (producer/consumer with pipes)
 *   - Container isolation via clone() with PID/UTS/mount namespaces
 *   - Kernel monitor integration via ioctl
 *   - Signal handling for graceful shutdown and child reaping
 *
 * Authors:
 *   Sushruth Sukesh        PES1UG24CS486
 *   Sujay Hegde  PES1UG24CS478
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    int pipe_fd;
    pthread_t producer_tid;
    int producer_running;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global supervisor context pointer for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

/* ---------------------------------------------------------------
 * Bounded Buffer Implementation
 *
 * Uses mutex + condition variables for producer-consumer sync.
 * Push blocks when full; pop blocks when empty. Both wake up
 * and return -1 when shutting_down is set and buffer is drained.
 * --------------------------------------------------------------- */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * Producer-side insertion into the bounded buffer.
 * Blocks when buffer is full, wakes consumers, stops on shutdown.
 * Returns 0 on success, -1 on shutdown.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Consumer-side removal from the bounded buffer.
 * Waits while empty. Returns 0 on success, -1 when shutdown
 * and no items remain (buffer fully drained).
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    /* Drain remaining items even during shutdown */
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Logging consumer thread.
 * Pops log chunks from the bounded buffer and appends them to
 * per-container log files under LOG_DIR. Drains all remaining
 * items on shutdown before exiting.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char path[PATH_MAX];
    int fd;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            if (write(fd, item.data, item.length) < 0)
                perror("log write");
            close(fd);
        }
    }

    return NULL;
}

/*
 * Producer thread argument: reads from a container's pipe fd and
 * pushes log chunks into the bounded buffer.
 */
typedef struct {
    supervisor_ctx_t *ctx;
    char container_id[CONTAINER_ID_LEN];
    int pipe_fd;
} producer_arg_t;

void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    strncpy(item.container_id, pa->container_id, CONTAINER_ID_LEN - 1);
    item.container_id[CONTAINER_ID_LEN - 1] = '\0';

    while ((n = read(pa->pipe_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        if (bounded_buffer_push(&pa->ctx->log_buffer, &item) != 0)
            break;
    }

    close(pa->pipe_fd);
    free(pa);
    return NULL;
}

/*
 * Clone child entrypoint.
 * Sets up isolated PID/UTS/mount namespaces, chroots into rootfs,
 * mounts /proc, redirects stdout/stderr, sets nice value, and
 * executes the configured command via /bin/sh -c.
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Set hostname for UTS namespace */
    if (sethostname(cfg->id, strlen(cfg->id)) != 0) {
        perror("sethostname");
        _exit(1);
    }

    /* Redirect stdout and stderr to the logging pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        _exit(1);
    }
    close(cfg->log_write_fd);

    /* chroot into the container rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        _exit(1);
    }
    if (chdir("/") != 0) {
        perror("chdir");
        _exit(1);
    }

    /* Mount /proc inside the container */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc");
        _exit(1);
    }

    /* Set nice value if specified */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) == -1 && errno != 0)
            perror("nice");
    }

    /* Execute the command */
    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    _exit(1);
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/* ---------------------------------------------------------------
 * Signal Handlers
 * --------------------------------------------------------------- */

static void sigchld_handler(int sig)
{
    (void)sig;
    /* Reaping is done in the main event loop via waitpid(WNOHANG) */
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

/*
 * Reap all exited children and update container metadata.
 * Distinguishes between normal exit, manual stop (stop_requested),
 * and hard-limit kill (SIGKILL without stop_requested).
 */
static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;
    container_record_t *rec;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        for (rec = ctx->containers; rec; rec = rec->next) {
            if (rec->host_pid == pid) {
                if (WIFEXITED(status)) {
                    rec->exit_code = WEXITSTATUS(status);
                    rec->exit_signal = 0;
                    if (rec->stop_requested)
                        rec->state = CONTAINER_STOPPED;
                    else
                        rec->state = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    rec->exit_signal = WTERMSIG(status);
                    rec->exit_code = 128 + rec->exit_signal;
                    if (rec->stop_requested)
                        rec->state = CONTAINER_STOPPED;
                    else if (rec->exit_signal == SIGKILL)
                        rec->state = CONTAINER_KILLED;
                    else
                        rec->state = CONTAINER_EXITED;
                }
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, rec->id, pid);

                fprintf(stderr,
                        "[supervisor] Container %s (pid %d) exited: state=%s code=%d signal=%d\n",
                        rec->id, pid, state_to_string(rec->state),
                        rec->exit_code, rec->exit_signal);
                break;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ---------------------------------------------------------------
 * Container Launch
 * --------------------------------------------------------------- */

static int launch_container(supervisor_ctx_t *ctx, const control_request_t *req)
{
    int pipe_fds[2];
    char *stack;
    pid_t child_pid;
    child_config_t cfg;
    container_record_t *rec;
    producer_arg_t *pa;

    /* Check for duplicate running container ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    for (rec = ctx->containers; rec; rec = rec->next) {
        if (strcmp(rec->id, req->container_id) == 0 &&
            (rec->state == CONTAINER_STARTING || rec->state == CONTAINER_RUNNING)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create pipe for container stdout/stderr -> supervisor */
    if (pipe(pipe_fds) != 0)
        return -1;

    /* Prepare child config */
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg.rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg.command, req->command, CHILD_COMMAND_LEN - 1);
    cfg.nice_value = req->nice_value;
    cfg.log_write_fd = pipe_fds[1];

    /* Allocate stack for clone */
    stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    /* Clone with PID, UTS, and mount namespaces */
    child_pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      &cfg);

    if (child_pid < 0) {
        free(stack);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    /* Parent: close write end of pipe */
    close(pipe_fds[1]);

    /* Create container record */
    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        close(pipe_fds[0]);
        free(stack);
        return -1;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid = child_pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->pipe_fd = pipe_fds[0];
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, rec->id, child_pid,
                              rec->soft_limit_bytes, rec->hard_limit_bytes);
    }

    /* Start producer thread for this container's pipe */
    pa = malloc(sizeof(*pa));
    if (pa) {
        pa->ctx = ctx;
        strncpy(pa->container_id, rec->id, CONTAINER_ID_LEN - 1);
        pa->container_id[CONTAINER_ID_LEN - 1] = '\0';
        pa->pipe_fd = pipe_fds[0];
        if (pthread_create(&rec->producer_tid, NULL, producer_thread, pa) == 0) {
            rec->producer_running = 1;
            pthread_detach(rec->producer_tid);
        } else {
            free(pa);
        }
    }

    /* Add to container list */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    fprintf(stderr, "[supervisor] Started container %s (pid %d)\n",
            rec->id, child_pid);

    free(stack);
    return 0;
}

/* ---------------------------------------------------------------
 * Command Handlers (supervisor side)
 * --------------------------------------------------------------- */

static void handle_start(supervisor_ctx_t *ctx, const control_request_t *req,
                         control_response_t *resp)
{
    if (launch_container(ctx, req) == 0) {
        resp->status = 0;
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "Container %s started", req->container_id);
    } else {
        resp->status = 1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "Failed to start container %s", req->container_id);
    }
}

static void handle_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    container_record_t *rec;
    char buf[4096] = {0};
    size_t off = 0;

    off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                    "%-12s %-8s %-12s %-10s %-10s %-6s %-6s\n",
                    "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)", "EXIT", "SIG");

    pthread_mutex_lock(&ctx->metadata_lock);
    for (rec = ctx->containers; rec && off < sizeof(buf) - 1; rec = rec->next) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                        "%-12s %-8d %-12s %-10lu %-10lu %-6d %-6d\n",
                        rec->id, rec->host_pid, state_to_string(rec->state),
                        rec->soft_limit_bytes >> 20,
                        rec->hard_limit_bytes >> 20,
                        rec->exit_code, rec->exit_signal);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
    strncpy(resp->message, buf, CONTROL_MESSAGE_LEN - 1);
}

static void handle_logs(supervisor_ctx_t *ctx, const control_request_t *req,
                        control_response_t *resp)
{
    char path[PATH_MAX];
    int fd;
    ssize_t n;

    (void)ctx;

    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req->container_id);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        resp->status = 1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "No log file for container %s", req->container_id);
        return;
    }

    n = read(fd, resp->message, CONTROL_MESSAGE_LEN - 1);
    if (n > 0)
        resp->message[n] = '\0';
    else
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "(empty log)");
    close(fd);
    resp->status = 0;
}

static void handle_stop(supervisor_ctx_t *ctx, const control_request_t *req,
                        control_response_t *resp)
{
    container_record_t *rec;
    int found = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    for (rec = ctx->containers; rec; rec = rec->next) {
        if (strcmp(rec->id, req->container_id) == 0 &&
            (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING)) {
            rec->stop_requested = 1;
            found = 1;
            kill(rec->host_pid, SIGTERM);
            break;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (found) {
        /* Grace period then force kill */
        usleep(500000);
        pthread_mutex_lock(&ctx->metadata_lock);
        if (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING) {
            kill(rec->host_pid, SIGKILL);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp->status = 0;
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "Container %s stop requested", req->container_id);
    } else {
        resp->status = 1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "Container %s not found or not running", req->container_id);
    }
}

/* ---------------------------------------------------------------
 * Supervisor: long-running daemon process
 * --------------------------------------------------------------- */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    struct sockaddr_un addr;
    struct sigaction sa;
    fd_set readfds;
    struct timeval tv;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Create logs directory */
    mkdir(LOG_DIR, 0755);

    /* 1) Open /dev/container_monitor (non-fatal if absent) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] Warning: could not open /dev/container_monitor: %s\n",
                strerror(errno));

    /* 2) Create Unix domain socket for control plane */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        goto cleanup;
    }

    /* 3) Install signal handlers */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* 4) Start the logging consumer thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create logger");
        goto cleanup;
    }

    fprintf(stderr, "[supervisor] Running. Control socket: %s\n", CONTROL_PATH);

    /* 5) Supervisor event loop */
    while (!ctx.should_stop) {
        FD_ZERO(&readfds);
        FD_SET(ctx.server_fd, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        rc = select(ctx.server_fd + 1, &readfds, NULL, NULL, &tv);

        /* Reap children on every iteration */
        reap_children(&ctx);

        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        if (rc == 0)
            continue;

        if (FD_ISSET(ctx.server_fd, &readfds)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR)
                    continue;
                perror("accept");
                continue;
            }

            control_request_t req;
            control_response_t resp;
            ssize_t n;

            memset(&resp, 0, sizeof(resp));
            n = read(client_fd, &req, sizeof(req));
            if (n == (ssize_t)sizeof(req)) {
                switch (req.kind) {
                case CMD_START:
                case CMD_RUN:
                    handle_start(&ctx, &req, &resp);
                    if (req.kind == CMD_RUN && resp.status == 0) {
                        /* For run: wait for container to exit */
                        container_record_t *rec;
                        int done = 0;
                        while (!done && !ctx.should_stop) {
                            usleep(100000);
                            reap_children(&ctx);
                            pthread_mutex_lock(&ctx.metadata_lock);
                            for (rec = ctx.containers; rec; rec = rec->next) {
                                if (strcmp(rec->id, req.container_id) == 0) {
                                    if (rec->state != CONTAINER_RUNNING &&
                                        rec->state != CONTAINER_STARTING) {
                                        snprintf(resp.message,
                                                 CONTROL_MESSAGE_LEN,
                                                 "Container %s exited with code %d",
                                                 rec->id, rec->exit_code);
                                        resp.status = rec->exit_code;
                                        done = 1;
                                    }
                                    break;
                                }
                            }
                            pthread_mutex_unlock(&ctx.metadata_lock);
                        }
                    }
                    break;
                case CMD_PS:
                    handle_ps(&ctx, &resp);
                    break;
                case CMD_LOGS:
                    handle_logs(&ctx, &req, &resp);
                    break;
                case CMD_STOP:
                    handle_stop(&ctx, &req, &resp);
                    reap_children(&ctx);
                    break;
                default:
                    resp.status = 1;
                    snprintf(resp.message, CONTROL_MESSAGE_LEN,
                             "Unknown command");
                    break;
                }
            } else {
                resp.status = 1;
                snprintf(resp.message, CONTROL_MESSAGE_LEN, "Invalid request");
            }

            write(client_fd, &resp, sizeof(resp));
            close(client_fd);
        }
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* Stop all running containers */
    {
        container_record_t *rec;
        pthread_mutex_lock(&ctx.metadata_lock);
        for (rec = ctx.containers; rec; rec = rec->next) {
            if (rec->state == CONTAINER_RUNNING ||
                rec->state == CONTAINER_STARTING) {
                rec->stop_requested = 1;
                kill(rec->host_pid, SIGTERM);
            }
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
    }

    usleep(500000);

    {
        container_record_t *rec;
        pthread_mutex_lock(&ctx.metadata_lock);
        for (rec = ctx.containers; rec; rec = rec->next) {
            if (rec->state == CONTAINER_RUNNING ||
                rec->state == CONTAINER_STARTING)
                kill(rec->host_pid, SIGKILL);
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
    }

    /* Final reap */
    reap_children(&ctx);

cleanup:
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container records */
    {
        container_record_t *rec = ctx.containers;
        while (rec) {
            container_record_t *next = rec->next;
            free(rec);
            rec = next;
        }
    }

    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] Shutdown complete.\n");
    g_ctx = NULL;
    return 0;
}

/* ---------------------------------------------------------------
 * CLI Client: sends control requests to the supervisor via
 * Unix domain socket (Path B: control IPC).
 * --------------------------------------------------------------- */

/* State for forwarding SIGINT/SIGTERM from the run client to the
 * supervisor. The handler sends a CMD_STOP for this container and
 * returns; the read() in send_control_request is restarted and we
 * continue waiting for the final status from the supervisor. */
static char g_run_container_id[CONTAINER_ID_LEN];

static void run_client_signal_handler(int sig)
{
    (void)sig;
    if (g_run_container_id[0] == '\0')
        return;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        control_request_t stop_req;
        memset(&stop_req, 0, sizeof(stop_req));
        stop_req.kind = CMD_STOP;
        strncpy(stop_req.container_id, g_run_container_id,
                sizeof(stop_req.container_id) - 1);
        (void)write(fd, &stop_req, sizeof(stop_req));
    }
    close(fd);
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor. Is it running?\n");
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    memset(&resp, 0, sizeof(resp));
    n = read(fd, &resp, sizeof(resp));
    if (n > 0)
        printf("%s\n", resp.message);

    close(fd);
    return resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    /* Per spec: if run client receives SIGINT/SIGTERM, forward a stop
     * request to the supervisor and keep waiting for final status. */
    g_run_container_id[0] = '\0';
    strncpy(g_run_container_id, req.container_id,
            sizeof(g_run_container_id) - 1);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = run_client_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
