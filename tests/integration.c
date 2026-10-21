/* SPDX-License-Identifier: GPL-2.0-or-later
 * Synthetic test boundaries; never open a real serial port or desktop display.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <util.h>

#ifdef NDEBUG
#error "The test harness requires assertions"
#endif

static int fixture_serial = -1;
static pid_t running_bridge = -1;

static FILE *
fixture_fopen(const char *path, const char *mode)
{
    if (strcmp(path, "/var/run/dmesg.boot") != 0)
        return fopen(path, mode);
    FILE *stream = tmpfile();
    assert(stream != NULL);
    fputs("com0 at isa0 port 0x200/8 irq 5: ns16550a, 16 byte fifo\n", stream);
    rewind(stream);
    return stream;
}

static int
fixture_open(const char *path, int flags, ...)
{
    assert(strcmp(path, "/dev/cua00") == 0);
    assert(fixture_serial >= 0);
    int fd = dup(fixture_serial);
    if (fd >= 0)
        assert(fcntl(fd, F_SETFL, flags & O_NONBLOCK) == 0);
    return fd;
}

static XRROutputInfo *
fixture_output(Display *dpy, XRRScreenResources *resources, RROutput output)
{
    XRROutputInfo *info = XRRGetOutputInfo(dpy, resources, output);
    if (info != NULL) {
        assert(info->nameLen >= 6);
        memcpy(info->name, "LVDS-1", 6);
        info->nameLen = 6;
    }
    return info;
}

static Status
fixture_transform(Display *dpy, RRCrtc crtc,
    XRRCrtcTransformAttributes **attributes)
{
    (void)dpy;
    (void)crtc;
    *attributes = calloc(1, sizeof(**attributes));
    assert(*attributes != NULL);
    for (int i = 0; i < 3; ++i)
        (*attributes)->currentTransform.matrix[i][i] = 65536;
    return 1;
}

#define fopen fixture_fopen
#define open fixture_open
#define XRRGetOutputInfo fixture_output
#define XRRGetCrtcTransform fixture_transform
#define main bridge_main
#include "../x61-pen.c"
#undef main
#undef XRRGetCrtcTransform
#undef XRRGetOutputInfo
#undef open
#undef fopen

static void
stop_test_child(void)
{
    if (running_bridge > 0) {
        kill(running_bridge, SIGKILL);
        while (waitpid(running_bridge, NULL, 0) == -1 && errno == EINTR) {}
    }
}

static void
stream_tests(void)
{
    struct parser parser = {0};
    uint32_t state = 0x51d4;
    unsigned char data[] = {0xa1, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0, 0};
    struct sample value = decode(data);
    assert(value.x == 65535 && value.y == 65535 && value.pressure == 1023);
    assert(value.proximity && value.tip && !value.side && !value.eraser);

    for (unsigned i = 0; i < 100000; ++i) {
        state = state * 1664525U + 1013904223U;
        int length = frame(&parser, (unsigned char)(state >> 24));
        assert(length == 0 || length == 9 || length == 11);
        assert(parser.n < sizeof(parser.b));
        assert(parser.size == 0 || parser.size == 9 || parser.size == 11);
    }
    for (unsigned i = 0; i < sizeof(data); ++i)
        assert(frame(&parser, data[i]) == (i + 1 == sizeof(data) ? 9 : 0));
    assert(memcmp(parser.b, data, sizeof(data)) == 0);
    puts("PASS: extreme coordinates, pressure and deterministic stream recovery");
}

static int
same_termios(int fd, const struct termios *expected)
{
    struct termios actual;
    assert(tcgetattr(fd, &actual) == 0);
    return actual.c_iflag == expected->c_iflag &&
        actual.c_oflag == expected->c_oflag &&
        actual.c_cflag == expected->c_cflag &&
        /* Returning to canonical mode can set this kernel-managed state. */
        (actual.c_lflag & ~PENDIN) == (expected->c_lflag & ~PENDIN) &&
        memcmp(actual.c_cc, expected->c_cc, sizeof(actual.c_cc)) == 0 &&
        cfgetispeed(&actual) == cfgetispeed(expected) &&
        cfgetospeed(&actual) == cfgetospeed(expected);
}

static unsigned
pointer_state(Display *observer, int *x, int *y)
{
    Window root, child;
    int local_x, local_y;
    unsigned mask;
    assert(XQueryPointer(observer, DefaultRootWindow(observer), &root, &child,
        x, y, &local_x, &local_y, &mask));
    return mask & (Button1Mask | Button3Mask);
}

static void
service_serial(int master, unsigned *starts, unsigned *stops)
{
    static const unsigned char query_reply[] = {
        0xc0, 0x30, 0, 0x24, 0, 0x7f, 1, 0, 0, 1, 0x30
    };
    static const unsigned char pen[] = {0xa3, 1, 2, 3, 4, 5, 0x6a, 0, 0};
    unsigned char commands[64];
    ssize_t count = read(master, commands, sizeof(commands));
    if (count < 0) {
        assert(errno == EAGAIN || errno == EINTR);
        return;
    }
    for (ssize_t i = 0; i < count; ++i) {
        if (commands[i] == '*')
            assert(write(master, query_reply, sizeof(query_reply)) ==
                (ssize_t)sizeof(query_reply));
        else if (commands[i] == '1') {
            ++*starts;
            assert(write(master, pen, sizeof(pen)) == (ssize_t)sizeof(pen));
        } else if (commands[i] == '0')
            ++*stops;
    }
}

static void
lifecycle_test(Display *observer, int signal_number, int watchdog)
{
    int master, slave, x = 0, y = 0, status;
    unsigned starts = 0, stops = 0;
    struct termios saved;
    assert(openpty(&master, &slave, NULL, NULL, NULL) == 0);
    assert(tcgetattr(slave, &saved) == 0);
    assert(fcntl(master, F_SETFL, O_NONBLOCK) == 0);
    fixture_serial = slave;
    fflush(NULL);
    pid_t child = fork();
    assert(child != -1);
    if (child == 0) {
        close(master);
        close(ConnectionNumber(observer));
        char *arguments[] = {"x61-pen", NULL};
        exit(bridge_main(1, arguments));
    }
    running_bridge = child;
    int pressed = 0;
    int64_t deadline = milliseconds() + 5000;
    while (milliseconds() < deadline) {
        service_serial(master, &starts, &stops);
        if (pointer_state(observer, &x, &y) == (Button1Mask | Button3Mask)) {
            pressed = 1;
            break;
        }
        usleep(10000);
    }
    assert(pressed && starts == 1);
    struct panel panel = {0, 0, 1024, 768, RR_Rotate_0};
    struct sample sample = {.x = 523, .y = 1553};
    struct sample maximum = {.x = 24576, .y = 18432};
    int expected_x, expected_y;
    position(sample, maximum, panel, &expected_x, &expected_y);
    assert(x == expected_x && y == expected_y);

    if (watchdog) {
        deadline = milliseconds() + 2000;
        while (milliseconds() < deadline && pointer_state(observer, &x, &y))
            usleep(10000);
        assert(pointer_state(observer, &x, &y) == 0);
    }
    assert(kill(child, signal_number) == 0);
    int restored = 0;
    deadline = milliseconds() + 3000;
    while (milliseconds() < deadline) {
        service_serial(master, &starts, &stops);
        if (pointer_state(observer, &x, &y) == 0 && stops >= 2 &&
            same_termios(slave, &saved)) {
            restored = 1;
            break;
        }
        usleep(10000);
    }
    if (!restored) {
        struct termios actual;
        assert(tcgetattr(slave, &actual) == 0);
        fprintf(stderr, "cleanup: buttons=%u stops=%u termios=%d\n",
            pointer_state(observer, &x, &y), stops, same_termios(slave, &saved));
        fprintf(stderr, "termios flags: expected=%lx/%lx/%lx/%lx actual=%lx/%lx/%lx/%lx\n",
            (unsigned long)saved.c_iflag, (unsigned long)saved.c_oflag,
            (unsigned long)saved.c_cflag, (unsigned long)saved.c_lflag,
            (unsigned long)actual.c_iflag, (unsigned long)actual.c_oflag,
            (unsigned long)actual.c_cflag, (unsigned long)actual.c_lflag);
        fprintf(stderr, "termios speeds: expected=%lu/%lu actual=%lu/%lu\n",
            (unsigned long)cfgetispeed(&saved), (unsigned long)cfgetospeed(&saved),
            (unsigned long)cfgetispeed(&actual), (unsigned long)cfgetospeed(&actual));
    }
    assert(restored);
    deadline = milliseconds() + 3000;
    pid_t reaped = 0;
    while (milliseconds() < deadline) {
        reaped = waitpid(child, &status, WNOHANG);
        if (reaped == child)
            break;
        assert(reaped == 0 || (reaped == -1 && errno == EINTR));
        usleep(10000);
    }
    assert(reaped == child);
    running_bridge = -1;
    if (signal_number == SIGKILL)
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    else
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(master);
    close(slave);
    fixture_serial = -1;
    printf("PASS: %s, button release, stop command and termios restoration\n",
        watchdog ? "stale-input watchdog and SIGTERM" :
        signal_number == SIGKILL ? "main-process SIGKILL" : "SIGTERM");
}

int
main(void)
{
    atexit(stop_test_child);
    stream_tests();
    const char *test_display = getenv("X61_PEN_TEST_DISPLAY");
    assert(test_display != NULL);
    assert(strcmp(test_display, ":0") != 0);
    assert(getenv("DISPLAY") != NULL);
    assert(strcmp(getenv("DISPLAY"), test_display) == 0);
    Display *observer = XOpenDisplay(test_display);
    assert(observer != NULL);
    assert(DisplayWidth(observer, DefaultScreen(observer)) == 1024);
    assert(DisplayHeight(observer, DefaultScreen(observer)) == 768);
    lifecycle_test(observer, SIGTERM, 0);
    lifecycle_test(observer, SIGKILL, 0);
    lifecycle_test(observer, SIGTERM, 1);
    XCloseDisplay(observer);
    puts("PASS: isolated X11/serial lifecycle tests");
    return 0;
}
