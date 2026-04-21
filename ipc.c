/*
 * rondo — Unix domain socket IPC
 *
 * Text-based, newline-delimited command protocol.
 * Socket path: /tmp/.rondo-ipc-<display>
 */
#include "wm.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>

#define IPC_MAX_CLIENTS 16
#define IPC_BUF_SIZE    1024

typedef struct {
    int        fd;
    XtInputId  input_id;
    char       buf[IPC_BUF_SIZE];
    int        len;
} IpcClient;

static int        ipc_listen_fd = -1;
static XtInputId  ipc_listen_id = 0;
static char       ipc_sock_path[256];
static IpcClient  ipc_clients[IPC_MAX_CLIENTS];

/* ── internal helpers ─────────────────────────────────────────────────── */

static void ipc_disconnect(int slot)
{
    IpcClient *c = &ipc_clients[slot];
    if (c->input_id) {
        XtRemoveInput(c->input_id);
        c->input_id = 0;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->len = 0;
}

static void ipc_dispatch(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') return;

    if (strcmp(line, "reload") == 0) {
        reloadconfig(NULL);
    } else if (strncmp(line, "view ", 5) == 0) {
        WmArg arg = {0};
        arg.ui = (unsigned int)atoi(line + 5);
        viewworkspace(&arg);
    } else if (strncmp(line, "move ", 5) == 0) {
        WmArg arg = {0};
        arg.ui = (unsigned int)atoi(line + 5);
        movetoworkspace(&arg);
    } else if (strcmp(line, "quit") == 0) {
        quit(NULL);
    } else if (strcmp(line, "arrange") == 0) {
        arrange();
    } else if (strcmp(line, "float") == 0) {
        togglefloat(NULL);
    } else if (strcmp(line, "fullscreen") == 0) {
        togglefullscreen(NULL);
    } else {
        fprintf(stderr, "rondo: unknown IPC command: '%s'\n", line);
    }
}

static void ipc_client_cb(XtPointer data, int *fd, XtInputId *id)
{
    (void)fd; (void)id;
    int slot = (int)(intptr_t)data;
    IpcClient *c = &ipc_clients[slot];

    int n = (int)read(c->fd, c->buf + c->len,
                      (size_t)(IPC_BUF_SIZE - c->len - 1));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        ipc_disconnect(slot);
        return;
    }
    if (n == 0) {
        ipc_disconnect(slot);
        return;
    }
    c->len += n;
    c->buf[c->len] = '\0';

    /* overflow: no newline found in full buffer */
    if (c->len >= IPC_BUF_SIZE - 1) {
        fprintf(stderr, "rondo: IPC client buffer overflow, disconnecting\n");
        ipc_disconnect(slot);
        return;
    }

    /* process complete newline-delimited commands */
    char *start = c->buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        ipc_dispatch(start);
        start = nl + 1;
    }

    /* shift remaining partial data to front */
    c->len = (int)((c->buf + c->len) - start);
    if (c->len > 0)
        memmove(c->buf, start, (size_t)c->len);
}

static void ipc_accept_cb(XtPointer data, int *fd, XtInputId *id)
{
    (void)data; (void)id;
    struct sockaddr_un addr;
    socklen_t len = sizeof(addr);
    int cfd = accept(*fd, (struct sockaddr *)&addr, &len);
    if (cfd < 0) return;

    int slot = -1;
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        if (ipc_clients[i].fd < 0) { slot = i; break; }
    }
    if (slot < 0) {
        close(cfd);
        return;
    }

    ipc_clients[slot].fd = cfd;
    ipc_clients[slot].len = 0;
    ipc_clients[slot].input_id = XtAppAddInput(
        app, cfd, (XtPointer)XtInputReadMask, ipc_client_cb,
        (XtPointer)(intptr_t)slot);
}

/* ── public interface ─────────────────────────────────────────────────── */

void ipc_init(void)
{
    /* build socket path from display string */
    const char *display = DisplayString(dpy);
    if (!display) display = ":0";
    /* sun_path is 108 bytes; prefix "/tmp/.rondo-ipc-" is 17 chars, leaving 91 */
    char safe[92];
    strncpy(safe, display, sizeof(safe) - 1);
    safe[sizeof(safe) - 1] = '\0';
    for (char *p = safe; *p; p++)
        if (*p == '/') *p = '_';
    snprintf(ipc_sock_path, sizeof(ipc_sock_path),
             "/tmp/.rondo-ipc-%s", safe);

    /* remove stale socket from a previous run */
    unlink(ipc_sock_path);

    ipc_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ipc_listen_fd < 0) {
        perror("rondo: IPC socket");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, ipc_sock_path, sizeof(addr.sun_path) - 1);

    if (bind(ipc_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("rondo: IPC bind");
        close(ipc_listen_fd);
        ipc_listen_fd = -1;
        return;
    }

    if (listen(ipc_listen_fd, SOMAXCONN) < 0) {
        perror("rondo: IPC listen");
        close(ipc_listen_fd);
        ipc_listen_fd = -1;
        return;
    }

    ipc_listen_id = XtAppAddInput(app, ipc_listen_fd,
                                  (XtPointer)XtInputReadMask, ipc_accept_cb, NULL);

    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        ipc_clients[i].fd = -1;
        ipc_clients[i].input_id = 0;
        ipc_clients[i].len = 0;
    }
}

void ipc_cleanup(void)
{
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        if (ipc_clients[i].fd >= 0)
            ipc_disconnect(i);
    }

    if (ipc_listen_id) {
        XtRemoveInput(ipc_listen_id);
        ipc_listen_id = 0;
    }
    if (ipc_listen_fd >= 0) {
        close(ipc_listen_fd);
        ipc_listen_fd = -1;
    }
    if (ipc_sock_path[0])
        unlink(ipc_sock_path);
}