/* mw-session — relay an X session command to every shim process.
 *
 * Stands in for a session manager (or a window manager on its behalf): connects
 * to each shim process's session socket under
 * $XDG_RUNTIME_DIR/xlib-wayland/session and sends "save" or "close", which the
 * process delivers as WM_SAVE_YOURSELF / WM_DELETE_WINDOW to its opted-in
 * top-level windows.  It speaks only Unix sockets; no X or Wayland needed.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "save") && strcmp(argv[1], "close"))) {
        fprintf(stderr, "usage: mw-session save|close\n");
        return 2;
    }
    const char *cmd = argv[1];
    /* The same directory the shim uses (see mw_runtime_dir). */
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char dir[256];
    if (rt && *rt) snprintf(dir, sizeof dir, "%s/xlib-wayland/session", rt);
    else snprintf(dir, sizeof dir, "/tmp/xlib-wayland-%u/session", (unsigned)getuid());

    DIR *dd = opendir(dir);
    if (!dd) {
        printf("MW-SESSION:%s sent=0 (no session dir)\n", cmd);
        return 1;
    }
    int sent = 0;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
            /* MSG_NOSIGNAL: a shim that went away must not kill us before
             * the rest have been told. */
            size_t cl = strlen(cmd);
            if (send(fd, cmd, cl, MSG_NOSIGNAL) == (ssize_t)cl &&
                send(fd, "\n", 1, MSG_NOSIGNAL) == 1)
                sent++;
        }
        close(fd);
    }
    closedir(dd);
    printf("MW-SESSION:%s sent=%d\n", cmd, sent);
    return sent > 0 ? 0 : 1;
}
