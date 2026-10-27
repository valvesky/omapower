#include "process-watch.h"

#include <glib-unix.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define FALLBACK_MS 500
#define RECONCILE_SECONDS 60

typedef struct {
    ProcessWatch *watch;
    pid_t pid;
    guint64 start;
    gchar *name;
    int pidfd;
    guint source;
} TrackedProcess;

struct ProcessWatch {
    GHashTable *commands;
    GHashTable *processes;
    ProcessWatchChanged changed;
    gpointer data;
    int socket;
    guint socket_source, ack_source, scan_source, notify_source;
    gboolean acknowledged;
    guint32 ack_token;
    const char *mode;
    GError *error;
};

static void remove_source(guint *source) {
    if (*source) g_source_remove(*source);
    *source = 0;
}

static gboolean notify_changed(gpointer data) {
    ProcessWatch *watch = data;
    watch->notify_source = 0;
    watch->changed(watch, watch->error, watch->data);
    return G_SOURCE_REMOVE;
}

static void notify(ProcessWatch *watch) {
    if (!watch->notify_source)
        watch->notify_source = g_idle_add_full(G_PRIORITY_DEFAULT, notify_changed, watch, NULL);
}

static void tracked_free(gpointer data) {
    TrackedProcess *process = data;
    remove_source(&process->source);
    if (process->pidfd >= 0) close(process->pidfd);
    g_free(process->name);
    g_free(process);
}

static gboolean process_exited(gint fd, GIOCondition condition, gpointer data) {
    (void)fd; (void)condition;
    TrackedProcess *process = data;
    ProcessWatch *watch = process->watch;
    pid_t pid = process->pid;
    process->source = 0;
    g_hash_table_remove(watch->processes, GINT_TO_POINTER(pid));
    notify(watch);
    return G_SOURCE_REMOVE;
}

static gboolean read_at(int dir, const char *file, char *buffer, size_t capacity) {
    int fd = openat(dir, file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return FALSE;
    ssize_t length;
    do length = read(fd, buffer, capacity - 1); while (length < 0 && errno == EINTR);
    close(fd);
    if (length <= 0 || (size_t)length == capacity - 1) return FALSE;
    buffer[length] = '\0';
    return TRUE;
}

/* Open the directory once: UID, comm and start time must refer to the same PID
 * incarnation. Only the final newline in comm is a delimiter, not whitespace. */
static gboolean process_info(ProcessWatch *watch, pid_t pid, char name[32], guint64 *start) {
    char path[64], statbuf[4096];
    g_snprintf(path, sizeof(path), "/proc/%d", pid);
    int dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return FALSE;
    struct stat st;
    gboolean valid = fstat(dir, &st) == 0 && st.st_uid == getuid()
        && read_at(dir, "comm", name, 32);
    if (valid) {
        size_t length = strlen(name);
        if (length && name[length - 1] == '\n') name[length - 1] = '\0';
        valid = g_hash_table_contains(watch->commands, name)
            && read_at(dir, "stat", statbuf, sizeof(statbuf));
    }
    close(dir);
    if (!valid) return FALSE;
    char *tail = strrchr(statbuf, ')');
    if (!tail || tail[1] != ' ' || !tail[2] || tail[3] != ' '
        || tail[2] == 'X' || tail[2] == 'x') return FALSE;
    if (tail[2] == 'Z') {
        /* A zombie leader can still have executing threads. A process pidfd
         * becomes readable only when the entire thread group has exited. */
        gboolean group_alive = FALSE;
#ifdef SYS_pidfd_open
        int fd = syscall(SYS_pidfd_open, pid, 0);
        if (fd >= 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            group_alive = poll(&pfd, 1, 0) == 0;
            close(fd);
        }
#endif
        if (!group_alive) return FALSE;
    }
    tail += 4; /* field 4; starttime is field 22 */
    for (int field = 4; field < 22; field++) {
        tail = strchr(tail, ' ');
        if (!tail) return FALSE;
        tail++;
    }
    char *end;
    errno = 0;
    *start = g_ascii_strtoull(tail, &end, 10);
    return errno == 0 && end != tail && (*end == ' ' || *end == '\n' || !*end);
}

static void refresh_pid(ProcessWatch *watch, pid_t pid) {
    if (pid <= 0 || !g_hash_table_size(watch->commands)) return;
    char name[32];
    guint64 start;
    TrackedProcess *old = g_hash_table_lookup(watch->processes, GINT_TO_POINTER(pid));
    if (!process_info(watch, pid, name, &start)) {
        if (g_hash_table_remove(watch->processes, GINT_TO_POINTER(pid))) notify(watch);
        return;
    }
    if (old && old->start == start && !strcmp(old->name, name)) return;
    TrackedProcess *process = g_new0(TrackedProcess, 1);
    process->watch = watch;
    process->pid = pid;
    process->start = start;
    process->name = g_strdup(name);
    process->pidfd = -1;
#ifdef SYS_pidfd_open
    process->pidfd = syscall(SYS_pidfd_open, pid, 0);
#endif
    if (process->pidfd >= 0) {
        /* A PID can exit/recycle between reading /proc and pidfd_open(). */
        char current_name[32];
        guint64 current_start;
        struct pollfd pfd = { .fd = process->pidfd, .events = POLLIN };
        if (poll(&pfd, 1, 0) != 0 || !process_info(watch, pid, current_name, &current_start)
            || current_start != start || strcmp(name, current_name)) {
            tracked_free(process);
            if (g_hash_table_remove(watch->processes, GINT_TO_POINTER(pid))) notify(watch);
            return;
        }
        process->source = g_unix_fd_add(process->pidfd, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                       process_exited, process);
    }
    g_hash_table_replace(watch->processes, GINT_TO_POINTER(pid), process);
    notify(watch);
}

static gboolean scan(ProcessWatch *watch, GError **error) {
    if (!g_hash_table_size(watch->commands)) {
        if (g_hash_table_size(watch->processes)) {
            g_hash_table_remove_all(watch->processes);
            notify(watch);
        }
        return TRUE;
    }
    DIR *proc = opendir("/proc");
    if (!proc) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "open /proc: %s", g_strerror(errno));
        return FALSE;
    }
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    int saved_errno = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(proc);
        if (!entry) { saved_errno = errno; break; }
        if (!g_ascii_isdigit(entry->d_name[0])) continue;
        char *end;
        gint64 number = g_ascii_strtoll(entry->d_name, &end, 10);
        if (*end || number <= 0 || number > G_MAXINT) continue;
        pid_t pid = (pid_t)number;
        g_hash_table_add(seen, GINT_TO_POINTER(pid));
        refresh_pid(watch, pid);
    }
    closedir(proc);
    if (!saved_errno) {
        GHashTableIter iter;
        gpointer key;
        g_hash_table_iter_init(&iter, watch->processes);
        while (g_hash_table_iter_next(&iter, &key, NULL)) {
            if (!g_hash_table_contains(seen, key)) { g_hash_table_iter_remove(&iter); notify(watch); }
        }
    }
    g_hash_table_unref(seen);
    if (saved_errno) g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                                "read /proc: %s", g_strerror(saved_errno));
    return saved_errno == 0;
}

static gboolean rescan(gpointer data) {
    ProcessWatch *watch = data;
    if (!scan(watch, &watch->error)) {
        watch->scan_source = 0;
        notify(watch);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean subscription(ProcessWatch *watch, enum proc_cn_mcast_op operation) {
    guint8 packet[NLMSG_HDRLEN + sizeof(struct cn_msg) + sizeof(operation)] = {0};
    struct nlmsghdr header = { .nlmsg_len = sizeof(packet), .nlmsg_type = NLMSG_DONE };
    struct cn_msg connector = { .id = { CN_IDX_PROC, CN_VAL_PROC },
                                .ack = watch->ack_token, .len = sizeof(operation) };
    memcpy(packet, &header, sizeof(header));
    memcpy(packet + NLMSG_HDRLEN, &connector, sizeof(connector));
    memcpy(packet + NLMSG_HDRLEN + sizeof(connector), &operation, sizeof(operation));
    struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
    return sendto(watch->socket, packet, sizeof(packet), 0,
                  (struct sockaddr *)&kernel, sizeof(kernel)) == (ssize_t)sizeof(packet);
}

static void close_socket(ProcessWatch *watch) {
    remove_source(&watch->socket_source);
    remove_source(&watch->ack_source);
    if (watch->socket >= 0) {
        subscription(watch, PROC_CN_MCAST_IGNORE);
        close(watch->socket);
        watch->socket = -1;
    }
}

static void fallback(ProcessWatch *watch, const char *reason) {
    g_printerr("omapower: process events unavailable (%s); polling /proc every %d ms\n", reason, FALLBACK_MS);
    close_socket(watch);
    remove_source(&watch->scan_source);
    watch->mode = "polling";
    watch->scan_source = g_timeout_add(FALLBACK_MS, rescan, watch);
    if (!scan(watch, &watch->error)) notify(watch);
    notify(watch);
}

static gboolean ack_timeout(gpointer data) {
    ProcessWatch *watch = data;
    watch->ack_source = 0;
    fallback(watch, "subscription acknowledgement timed out");
    return G_SOURCE_REMOVE;
}

/* Process connector messages are best-effort. Never trust user-sent netlink
 * messages, truncated packets, or a thread's comm as the leader's name. */
static void handle_event(ProcessWatch *watch, const struct proc_event *event) {
    switch (event->what) {
    case PROC_EVENT_NONE:
        if (event->event_data.ack.err) {
            fallback(watch, g_strerror(event->event_data.ack.err));
        } else if (!watch->acknowledged) {
            watch->acknowledged = TRUE;
            remove_source(&watch->ack_source);
            watch->mode = "events";
            notify(watch);
        }
        break;
    case PROC_EVENT_FORK:
        if (event->event_data.fork.child_pid == event->event_data.fork.child_tgid)
            refresh_pid(watch, event->event_data.fork.child_tgid);
        break;
    case PROC_EVENT_EXEC:
        refresh_pid(watch, event->event_data.exec.process_tgid);
        break;
    case PROC_EVENT_UID:
        refresh_pid(watch, event->event_data.id.process_tgid);
        break;
    case PROC_EVENT_COMM:
        if (event->event_data.comm.process_pid == event->event_data.comm.process_tgid)
            refresh_pid(watch, event->event_data.comm.process_tgid);
        break;
    case PROC_EVENT_EXIT:
        /* Refresh rather than blindly remove: a queued exit may belong to an
         * older PID incarnation. pidfds also report exits if netlink drops one. */
        refresh_pid(watch, event->event_data.exit.process_tgid);
        break;
    default:
        break;
    }
}

/* Return TRUE when a full snapshot is needed to recover an incomplete stream. */
static gboolean process_packet(ProcessWatch *watch, const guint8 *bytes, size_t length) {
    gboolean recover = FALSE;
    size_t offset = 0;
    while (length - offset >= NLMSG_HDRLEN) {
        struct nlmsghdr header;
        memcpy(&header, bytes + offset, sizeof(header));
        if (header.nlmsg_len < NLMSG_HDRLEN || header.nlmsg_len > length - offset) return TRUE;
        size_t step = NLMSG_ALIGN(header.nlmsg_len);
        if (step > length - offset) return TRUE;
        const guint8 *payload = bytes + offset + NLMSG_HDRLEN;
        size_t payload_length = header.nlmsg_len - NLMSG_HDRLEN;
        offset += step;
        if (header.nlmsg_type == NLMSG_OVERRUN) { recover = TRUE; continue; }
        if (header.nlmsg_type != NLMSG_DONE) continue;
        if (payload_length < sizeof(struct cn_msg)) { recover = TRUE; continue; }
        struct cn_msg connector;
        memcpy(&connector, payload, sizeof(connector));
        if (connector.id.idx != CN_IDX_PROC || connector.id.val != CN_VAL_PROC) continue;
        if (connector.len < sizeof(struct proc_event)
            || connector.len > payload_length - sizeof(connector)) {
            recover = TRUE; continue;
        }
        struct proc_event event;
        memcpy(&event, payload + sizeof(connector), sizeof(event));
        /* Acknowledgements are multicast too. Do not accept another client's
         * subscription acknowledgement as proof that ours succeeded. */
        if (event.what == PROC_EVENT_NONE && connector.ack != watch->ack_token + 1U) continue;
        handle_event(watch, &event);
        if (event.what == PROC_EVENT_NONE && event.event_data.ack.err) return TRUE;
    }
    return recover || offset != length;
}

static gboolean socket_ready(gint fd, GIOCondition condition, gpointer data) {
    ProcessWatch *watch = data;
    gboolean recover = (condition & G_IO_ERR) != 0;
    if (condition & (G_IO_HUP | G_IO_NVAL)) {
        watch->socket_source = 0;
        fallback(watch, "process event socket disconnected");
        return G_SOURCE_REMOVE;
    }
    for (unsigned batch = 0; batch < 256; batch++) {
        union { struct nlmsghdr alignment; guint8 bytes[65536]; } buffer;
        struct sockaddr_nl sender = {0};
        struct iovec iov = { .iov_base = buffer.bytes, .iov_len = sizeof(buffer.bytes) };
        struct msghdr message = { .msg_name = &sender, .msg_namelen = sizeof(sender),
                                 .msg_iov = &iov, .msg_iovlen = 1 };
        ssize_t length = recvmsg(fd, &message, MSG_DONTWAIT);
        if (length < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOBUFS) { recover = TRUE; continue; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            watch->socket_source = 0;
            fallback(watch, g_strerror(errno));
            return G_SOURCE_REMOVE;
        }
        if (!length) {
            watch->socket_source = 0;
            fallback(watch, "process event socket closed");
            return G_SOURCE_REMOVE;
        }
        if (message.msg_namelen < sizeof(sender) || sender.nl_family != AF_NETLINK || sender.nl_pid != 0) continue;
        if (message.msg_flags & MSG_TRUNC) { recover = TRUE; continue; }
        recover |= process_packet(watch, buffer.bytes, (size_t)length);
        if (watch->socket < 0) return G_SOURCE_REMOVE;
    }
    if (recover && !scan(watch, &watch->error)) notify(watch);
    return G_SOURCE_CONTINUE;
}

ProcessWatch *process_watch_new(GHashTable *commands, gboolean force_poll,
                              ProcessWatchChanged changed, gpointer data, GError **error) {
    ProcessWatch *watch = g_new0(ProcessWatch, 1);
    watch->commands = g_hash_table_ref(commands);
    watch->processes = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, tracked_free);
    watch->changed = changed;
    watch->data = data;
    watch->socket = -1;
    watch->ack_token = g_random_int();
    watch->mode = "connecting";
    if (force_poll) {
        fallback(watch, "--poll requested");
    } else {
        watch->socket = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_CONNECTOR);
        struct sockaddr_nl address = { .nl_family = AF_NETLINK, .nl_groups = CN_IDX_PROC };
        int receive_buffer = 1024 * 1024;
        if (watch->socket >= 0)
            setsockopt(watch->socket, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
        if (watch->socket < 0 || bind(watch->socket, (struct sockaddr *)&address, sizeof(address)) < 0
            || !subscription(watch, PROC_CN_MCAST_LISTEN)) {
            fallback(watch, g_strerror(errno));
        } else {
            watch->socket_source = g_unix_fd_add(watch->socket, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                                socket_ready, watch);
            watch->ack_source = g_timeout_add_seconds(1, ack_timeout, watch);
            watch->scan_source = g_timeout_add_seconds(RECONCILE_SECONDS, rescan, watch);
        }
    }
    if (watch->error) {
        g_propagate_error(error, g_steal_pointer(&watch->error));
        process_watch_free(watch);
        return NULL;
    }
    /* Subscribe before the snapshot; queued events reconcile startup races. */
    if (!scan(watch, error)) { process_watch_free(watch); return NULL; }
    return watch;
}

gboolean process_watch_set_commands(ProcessWatch *watch, GHashTable *commands, GError **error) {
    GHashTable *replacement = g_hash_table_ref(commands);
    g_hash_table_unref(watch->commands);
    watch->commands = replacement;
    return scan(watch, error);
}

GHashTable *process_watch_active(ProcessWatch *watch) {
    GHashTable *active = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, watch->processes);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        g_hash_table_add(active, g_strdup(((TrackedProcess *)value)->name));
    return active;
}

gboolean process_watch_running(ProcessWatch *watch) { return g_hash_table_size(watch->processes) > 0; }
const char *process_watch_mode(ProcessWatch *watch) { return watch->mode; }

void process_watch_free(ProcessWatch *watch) {
    if (!watch) return;
    close_socket(watch);
    remove_source(&watch->scan_source);
    remove_source(&watch->notify_source);
    g_hash_table_unref(watch->processes);
    g_hash_table_unref(watch->commands);
    g_clear_error(&watch->error);
    g_free(watch);
}
