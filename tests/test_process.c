#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <errno.h>

static gboolean deny_socket;
static int test_socket(int domain, int type, int protocol) {
    if (deny_socket && domain == AF_NETLINK) { errno = EPERM; return -1; }
    return socket(domain, type, protocol);
}
#define socket(domain, type, protocol) test_socket(domain, type, protocol)
#include "../process-watch.c"
#undef socket

static void changed(ProcessWatch *watch, const GError *error, gpointer data) {
    (void)watch; (void)data;
    g_assert_null(error);
}

static ProcessWatch *new_watch(const char *name) {
    GHashTable *commands = g_hash_table_new(g_str_hash, g_str_equal);
    if (name) g_hash_table_add(commands, (gpointer)name);
    GError *error = NULL;
    ProcessWatch *watch = process_watch_new(commands, TRUE, changed, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(watch);
    g_hash_table_unref(commands);
    return watch;
}

static void test_identity_and_threads(void) {
    char previous[16];
    g_assert_cmpint(prctl(PR_GET_NAME, previous), ==, 0);
    g_assert_cmpint(prctl(PR_SET_NAME, "oma name "), ==, 0);
    ProcessWatch *watch = new_watch("oma name ");
    TrackedProcess *process = g_hash_table_lookup(watch->processes, GINT_TO_POINTER(getpid()));
    g_assert_nonnull(process);
    g_assert_cmpstr(process->name, ==, "oma name ");
    g_assert_cmpuint(process->start, >, 0);
    guint64 start = process->start;
    process->start = 0; /* a stale PID incarnation must be replaced */
    refresh_pid(watch, getpid());
    process = g_hash_table_lookup(watch->processes, GINT_TO_POINTER(getpid()));
    g_assert_cmpuint(process->start, ==, start);
    struct proc_event event = { .what = PROC_EVENT_EXIT };
    event.event_data.exit.process_pid = getpid();
    event.event_data.exit.process_tgid = getpid();
    handle_event(watch, &event); /* a queued old exit must not delete a live PID */
    g_assert_true(process_watch_running(watch));
    g_hash_table_remove_all(watch->processes);
    event.what = PROC_EVENT_COMM;
    event.event_data.comm.process_pid = getpid() + 1;
    event.event_data.comm.process_tgid = getpid();
    handle_event(watch, &event); /* thread names are not process names */
    g_assert_false(process_watch_running(watch));
    event.event_data.comm.process_pid = getpid();
    handle_event(watch, &event);
    g_assert_true(process_watch_running(watch));
    g_assert_cmpint(prctl(PR_SET_NAME, "oma_other"), ==, 0);
    handle_event(watch, &event);
    g_assert_false(process_watch_running(watch));
    process_watch_free(watch);
    g_assert_cmpint(prctl(PR_SET_NAME, previous), ==, 0);
}

static void test_denied_connector(void) {
    deny_socket = TRUE;
    GHashTable *commands = g_hash_table_new(g_str_hash, g_str_equal);
    GError *error = NULL;
    ProcessWatch *watch = process_watch_new(commands, FALSE, changed, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(process_watch_mode(watch), ==, "polling");
    g_assert_cmpint(watch->socket, ==, -1);
    g_assert_cmpuint(watch->ack_source, ==, 0);
    g_assert_cmpuint(watch->scan_source, !=, 0);
    process_watch_free(watch);
    g_hash_table_unref(commands);
    deny_socket = FALSE;
}

static void test_packets_and_acknowledgements(void) {
    ProcessWatch *watch = new_watch(NULL);
    union {
        struct nlmsghdr align;
        guint8 bytes[NLMSG_HDRLEN + sizeof(struct cn_msg) + sizeof(struct proc_event)];
    } packet = {0};
    struct nlmsghdr header = { .nlmsg_len = sizeof(packet.bytes), .nlmsg_type = NLMSG_DONE };
    struct cn_msg connector = { .id = { CN_IDX_PROC, CN_VAL_PROC }, .len = sizeof(struct proc_event),
                                .ack = watch->ack_token };
    struct proc_event event = { .what = PROC_EVENT_NONE };
    memcpy(packet.bytes, &header, sizeof(header));
    memcpy(packet.bytes + NLMSG_HDRLEN, &connector, sizeof(connector));
    memcpy(packet.bytes + NLMSG_HDRLEN + sizeof(connector), &event, sizeof(event));
    g_assert_false(process_packet(watch, packet.bytes, sizeof(packet.bytes)));
    g_assert_false(watch->acknowledged); /* another listener's ACK */
    connector.ack = watch->ack_token + 1U;
    memcpy(packet.bytes + NLMSG_HDRLEN, &connector, sizeof(connector));
    g_assert_false(process_packet(watch, packet.bytes, sizeof(packet.bytes)));
    g_assert_true(watch->acknowledged);
    g_assert_cmpstr(process_watch_mode(watch), ==, "events");
    connector.len = 65535;
    memcpy(packet.bytes + NLMSG_HDRLEN, &connector, sizeof(connector));
    g_assert_true(process_packet(watch, packet.bytes, sizeof(packet.bytes)));
    header.nlmsg_type = NLMSG_OVERRUN;
    memcpy(packet.bytes, &header, sizeof(header));
    g_assert_true(process_packet(watch, packet.bytes, sizeof(packet.bytes)));
    /* Malformed/truncated kernel messages must request a recovery snapshot. */
    for (size_t size = 1; size < sizeof(packet.bytes); size++)
        g_assert_true(process_packet(watch, packet.bytes, size));
    process_watch_free(watch);
}

static void test_subscription_timeout(void) {
    ProcessWatch *watch = new_watch(NULL);
    watch->mode = "connecting";
    g_assert_false(ack_timeout(watch));
    g_assert_cmpstr(process_watch_mode(watch), ==, "polling");
    process_watch_free(watch);
}

static void *group_worker(void *data) {
    (void)data;
    char byte;
    if (write(STDOUT_FILENO, "ready\n", 6) != 6) _exit(2);
    while (read(STDIN_FILENO, &byte, 1) < 0 && errno == EINTR) {}
    return NULL;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--thread-group")) {
        pthread_t thread;
        if (prctl(PR_SET_NAME, "oma_evt_test") != 0
            || pthread_create(&thread, NULL, group_worker, NULL) != 0) return 2;
        pthread_exit(NULL);
    }
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/process/identity-and-threads", test_identity_and_threads);
    g_test_add_func("/process/denied-connector", test_denied_connector);
    g_test_add_func("/process/packets-and-acknowledgements", test_packets_and_acknowledgements);
    g_test_add_func("/process/subscription-timeout", test_subscription_timeout);
    return g_test_run();
}
