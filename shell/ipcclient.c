/* fanshell-qs-ipc <target> <function>: `fanshell-qs-shell ipc call` for key
 * bindings. It sends the same line down the same socket (ipc.cpp), but with
 * nothing to load: fanshell-qs-shell links Qt, and libmpv with all of
 * ffmpeg's codecs behind it, which takes 400 ms on a laptop CPU before the
 * first byte is sent, on every key press. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int first = argc > 1 && !strcmp(argv[1], "call") ? 2 : 1;
    if (argc - first != 2) {
        fprintf(stderr, "usage: fanshell-qs-ipc <target> <function>\n");
        return 2;
    }

    /* IpcServer::socketPath(), which keys it by the X display. */
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) {
        dir = getenv("TMPDIR");
        if (!dir || !*dir)
            dir = "/tmp";
    }
    const char *display = getenv("DISPLAY");
    if (!display)
        display = ":0";
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    int n = snprintf(addr.sun_path, sizeof addr.sun_path,
                     "%s/fanshell-qs-shell-%s.sock", dir, display);
    if (n < 0 || (size_t)n >= sizeof addr.sun_path) {
        fprintf(stderr, "fanshell-qs-ipc: socket path too long\n");
        return 1;
    }
    size_t key = strlen(dir) + strlen("/fanshell-qs-shell-");
    for (char *c = addr.sun_path + key; *c && strcmp(c, ".sock"); ++c) {
        if (*c == '/')
            *c = '_';
        else if (*c == ':')
            *c = 'x';
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "fanshell-qs-ipc: no running instance (socket %s)\n",
                addr.sun_path);
        return 1;
    }
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

    char line[256];
    n = snprintf(line, sizeof line, "%s %s\n", argv[first], argv[first + 1]);
    if (n < 0 || (size_t)n >= sizeof line || write(fd, line, (size_t)n) != n)
        return 1;
    char reply[16];
    ssize_t got = read(fd, reply, sizeof reply);
    return got >= 2 && !memcmp(reply, "ok", 2) ? 0 : 1;
}
