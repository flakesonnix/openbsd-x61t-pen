/* SPDX-License-Identifier: GPL-2.0-or-later
 * ISDV4 coordinate/query layout derived from linuxwacom include/isdv4.h,
 * Copyright 2010 Red Hat, Inc., licensed GPL-2.0-or-later.
 * Local OpenBSD X61 pointer bridge. No kernel changes or Linux input API.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xrandr.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct parser { unsigned char b[11]; unsigned n, size; };
struct sample { unsigned x, y, pressure, proximity, tip, side, eraser; };
struct panel { int x, y, width, height; Rotation rotation; };
static int serial = -1, have_termios, guard_write = -1, guard_status = -1;
static pid_t guard_pid = -1;
static struct termios original;
static Display *display;
static unsigned buttons;
static volatile sig_atomic_t stopping;

static void stop(int sig) { (void)sig; stopping = 1; }

static int
frame(struct parser *p, unsigned char byte)
{
    if (byte & 0x80) {
        p->n = 0;
        p->size = (byte & 0x10) ? 0 : ((byte & 0x40) ? 11 : 9);
    }
    if (!p->size || (!p->n && !(byte & 0x80)))
        return 0;
    p->b[p->n++] = byte;
    if (p->n != p->size)
        return 0;
    p->n = 0;
    p->size = 0;
    return (p->b[0] & 0x40) ? 11 : 9;
}

static struct sample
decode(const unsigned char *b)
{
    struct sample s = {0};
    s.x = (b[1] << 9) | (b[2] << 2) | ((b[6] >> 5) & 3);
    s.y = (b[3] << 9) | (b[4] << 2) | ((b[6] >> 3) & 3);
    s.pressure = ((b[6] & 7) << 7) | b[5];
    s.proximity = !!(b[0] & 0x20);
    s.tip = !!(b[0] & 1);
    s.side = !!(b[0] & 2);
    s.eraser = !!(b[0] & 4);
    return s;
}

static void
position(struct sample s, struct sample max, struct panel p, int *x, int *y)
{
    double u = (double)s.x / max.x, v = (double)s.y / max.y, t;
    switch (p.rotation) {
    case RR_Rotate_90: t = u; u = 1-v; v = t; break;
    case RR_Rotate_180: u = 1-u; v = 1-v; break;
    case RR_Rotate_270: t = u; u = v; v = 1-t; break;
    }
    *x = p.x + (int)(u * (p.width-1) + 0.5);
    *y = p.y + (int)(v * (p.height-1) + 0.5);
}

static void
set_buttons(unsigned wanted)
{
    unsigned i;
    for (i = 0; i < 2; i++)
        if ((buttons ^ wanted) & (1U << i))
            XTestFakeButtonEvent(display, i ? 3 : 1,
                !!(wanted & (1U << i)), CurrentTime);
    buttons = wanted;
    XFlush(display);
}

static void
cleanup(void)
{
    if (display) {
        set_buttons(0);
        XSync(display, False);
        if (guard_pid <= 0) XSetCloseDownMode(display,DestroyAll);
        XCloseDisplay(display);
        display = NULL;
    }
    if (serial >= 0) {
        /* Once running, the guard owns stop/restore, including after SIGKILL. */
        if (guard_pid <= 0 && have_termios) {
            if (write(serial, "0", 1) != 1) perror("stop tablet");
            if (tcsetattr(serial, TCSADRAIN, &original) == -1)
                perror("restore termios");
        }
        close(serial);
        serial = -1;
    }
    if (guard_write >= 0) {
        close(guard_write);
        guard_write = -1;
    }
    if (guard_pid > 0) {
        while (waitpid(guard_pid, NULL, 0) == -1 && errno == EINTR) {}
        guard_pid = -1;
    }
    if (guard_status >= 0) { close(guard_status); guard_status = -1; }
}

static int
guard_error(Display *d, XErrorEvent *e)
{
    (void)d;
    /* Normal shutdown may already have destroyed the main client's marker. */
    if (e->error_code != BadValue)
        fprintf(stderr,"cleanup guard X error=%u\n",e->error_code);
    return 0;
}

static int64_t
milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}

static int
xio_error(Display *d)
{
    (void)d;
    /* A disconnected X server has no surviving button state to release. */
    display = NULL;
    cleanup();
    _exit(1);
}

static int
get_panel(struct panel *p)
{
    XRRScreenResources *r = XRRGetScreenResourcesCurrent(display,
        DefaultRootWindow(display));
    int i, found = 0;
    if (!r) return 0;
    for (i = 0; i < r->noutput; i++) {
        XRROutputInfo *o = XRRGetOutputInfo(display, r, r->outputs[i]);
        if (o && o->connection == RR_Connected && o->crtc &&
            o->nameLen == 6 && !memcmp(o->name, "LVDS-1", 6)) {
            XRRCrtcInfo *c = XRRGetCrtcInfo(display, r, o->crtc);
            if (c) {
                *p = (struct panel){c->x, c->y, (int)c->width,
                    (int)c->height, c->rotation};
                found = p->width > 0 && p->height > 0 &&
                    !(p->rotation & (RR_Reflect_X | RR_Reflect_Y));
                XRRCrtcTransformAttributes *a = NULL;
                if (!XRRGetCrtcTransform(display,o->crtc,&a) || !a)
                    found = 0;
                if (a) {
                    for (int row=0;row<3;row++)
                        for (int col=0;col<3;col++)
                            if (a->currentTransform.matrix[row][col] !=
                                (row==col?65536:0)) found=0;
                    XFree(a);
                }
                XRRFreeCrtcInfo(c);
            }
        }
        if (o) XRRFreeOutputInfo(o);
    }
    XRRFreeScreenResources(r);
    return found;
}

static int
query(struct sample *max)
{
    struct parser parser = {0};
    struct pollfd f = {serial, POLLIN, 0};
    struct timespec start, now;
    unsigned char b[512];
    ssize_t n, i;
    if (write(serial, "0", 1) != 1) return 0;
    usleep(250000);
    if (tcflush(serial, TCIFLUSH) == -1 || write(serial, "*", 1) != 1)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!stopping) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 3) break;
        if (poll(&f, 1, 100) <= 0) continue;
        if (f.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        n = read(serial, b, sizeof(b));
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) break;
        for (i = 0; i < n; i++) {
            if (frame(&parser, b[i]) == 11 && !(parser.b[0] & 0x3f)) {
                *max = decode(parser.b);
                if (max->x && max->y && max->pressure) return 1;
            }
        }
    }
    return 0;
}

static void
self_test(void)
{
    struct parser p = {0};
    unsigned char q[] = {0xc0,0x30,0,0x24,0,0x7f,1,0,0,1,0x30};
    unsigned char b[] = {0xa3,1,2,3,4,5,0x6a,0,0};
    struct sample s = decode(b), max = decode(q);
    struct panel panel = {0,0,1024,768,RR_Rotate_0};
    unsigned i;
    int x, y;
    assert(s.x == 523 && s.y == 1553 && s.pressure == 261);
    assert(s.proximity && s.tip && s.side && !s.eraser);
    assert(max.x == 24576 && max.y == 18432 && max.pressure == 255);
    assert(!frame(&p, 0));
    for (i = 0; i < 3; i++) assert(!frame(&p, b[i]));
    for (i = 0; i < sizeof(q); i++) assert(frame(&p,q[i]) == (i==10?11:0));
    for (i = 0; i < sizeof(b); i++) assert(frame(&p,b[i]) == (i==8?9:0));
    for (i = 0; i < 3; i++) assert(!frame(&p,b[i]));
    p=(struct parser){0}; /* A frame deadline must discard a stale partial. */
    for (i = 3; i < sizeof(b); i++) assert(!frame(&p,b[i]));
    assert(!frame(&p,0x90)); assert(!frame(&p,0));
    for (i = 0; i < sizeof(b); i++) assert(frame(&p,b[i]) == (i==8?9:0));
    s.x = 0; s.y = 0;
    position(s,max,panel,&x,&y); assert(x==0 && y==0);
    panel.rotation=RR_Rotate_90;
    position(s,max,panel,&x,&y); assert(x==1023 && y==0);
    panel.rotation=RR_Rotate_180;
    position(s,max,panel,&x,&y); assert(x==1023 && y==767);
    panel.rotation=RR_Rotate_270;
    position(s,max,panel,&x,&y); assert(x==0 && y==767);
    s.x=max.x; s.y=max.y; panel.rotation=RR_Rotate_0;
    position(s,max,panel,&x,&y); assert(x==1023 && y==767);
    puts("PASS: query/pen decode, partial frames, resync, touch rejection, four rotations");
}

int
main(int argc, char **argv)
{
    struct parser parser = {0};
    struct sample max;
    struct panel panel;
    struct termios raw;
    struct pollfd f[3];
    unsigned char b[512];
    unsigned long frames=0, tips=0, motions=0, clicks=0;
    int pipes[2], ready[2], ev, err, major, minor, x, y, verbose=0, matched=0;
    int timed_out=0;
    int64_t last_valid, frame_start=0;
    Pixmap marker;
    ssize_t n, i;
    FILE *boot;
    char line[512];
    if (argc == 2 && !strcmp(argv[1],"--self-test")) { self_test(); return 0; }
    if (argc == 3 && !strcmp(argv[1],"--decode")) {
        int c, size;
        FILE *input = fopen(argv[2],"rb");
        if (!input) { perror(argv[2]); return 1; }
        while ((c=fgetc(input)) != EOF) {
            size=frame(&parser,(unsigned char)c);
            if (size==9) { frames++; tips+=decode(parser.b).tip; }
        }
        fclose(input);
        printf("replay pen_frames=%lu tip_frames=%lu\n",frames,tips);
        return frames ? 0 : 1;
    }
    if (argc==2 && !strcmp(argv[1],"-v")) verbose=1;
    else if (argc!=1) { fprintf(stderr,"usage: x61-pen [-v|--self-test|--decode FILE]\n"); return 1; }
    boot=fopen("/var/run/dmesg.boot","r");
    if (!boot) { perror("dmesg.boot"); return 1; }
    while (fgets(line,sizeof(line),boot))
        if (!strncmp(line,"com0 at isa0 port 0x200/8 irq 5:",
            sizeof("com0 at isa0 port 0x200/8 irq 5:")-1)) matched=1;
    fclose(boot);
    if (!matched) { fprintf(stderr,"Refusing port: current boot does not confirm com0 at 0x200/8 IRQ5\n"); return 1; }
    signal(SIGINT,stop); signal(SIGTERM,stop); signal(SIGHUP,stop);
    signal(SIGQUIT,stop); signal(SIGTSTP,stop);
    signal(SIGPIPE,SIG_IGN);
    atexit(cleanup);
    serial=open("/dev/cua00",O_RDWR|O_NOCTTY|O_NONBLOCK);
    if (serial<0) { perror("/dev/cua00"); return 1; }
    if (ioctl(serial,TIOCEXCL)==-1 || tcgetattr(serial,&original)==-1) { perror("serial setup"); return 1; }
    have_termios=1;
    raw=original;
    cfmakeraw(&raw);
    raw.c_iflag=IGNBRK|IGNPAR;
    raw.c_cflag &= ~(CSIZE|PARENB|CSTOPB|CRTSCTS|MDMBUF);
    raw.c_cflag |= CS8|CREAD|CLOCAL;
    raw.c_cc[VMIN]=0; raw.c_cc[VTIME]=0;
    cfsetispeed(&raw,B19200); cfsetospeed(&raw,B19200);
    if (tcsetattr(serial,TCSANOW,&raw)==-1 || !query(&max)) {
        fprintf(stderr,"No valid ISDV4 query at 19200; stopping\n"); return 1;
    }
    display=XOpenDisplay(NULL);
    if (!display) { fprintf(stderr,"Cannot open authorized user X display\n"); return 1; }
    XSetIOErrorHandler(xio_error);
    if (!XTestQueryExtension(display,&ev,&err,&major,&minor) ||
        !XRRQueryExtension(display,&ev,&err) || !get_panel(&panel)) {
        fprintf(stderr,"XTEST/RandR/LVDS-1 unavailable\n"); return 1;
    }
    /* Keep the marker/client identity reserved until the guard retires it. */
    XSetCloseDownMode(display,RetainTemporary);
    marker=XCreatePixmap(display,DefaultRootWindow(display),1,1,1);
    XSync(display,False);
    if (pipe(pipes)==-1 || pipe(ready)==-1 || (guard_pid=fork())==-1) {
        perror("cleanup guard"); return 1;
    }
    if (guard_pid==0) {
        unsigned char next;
        close(pipes[1]); close(ready[0]); guard_pid=-1;
        signal(SIGINT,SIG_IGN); signal(SIGTERM,SIG_IGN);
        signal(SIGHUP,SIG_IGN); signal(SIGQUIT,SIG_IGN); signal(SIGTSTP,SIG_IGN);
        close(ConnectionNumber(display)); display=XOpenDisplay(NULL);
        if (!display) { cleanup(); _exit(1); }
        XSetErrorHandler(guard_error);
        if (pledge("stdio tty unix",NULL)==-1) { cleanup(); _exit(1); }
        if (write(ready[1],"R",1)!=1) { cleanup(); _exit(1); }
        while (read(pipes[0],&next,1)==1) {}
        /* Retire the original X client before releasing, so queued requests
         * from that connection cannot re-press a button after our release. */
        XGrabServer(display);
        XKillClient(display,marker);
        XTestFakeButtonEvent(display,1,False,CurrentTime);
        XTestFakeButtonEvent(display,3,False,CurrentTime);
        XSync(display,False); XUngrabServer(display);
        XCloseDisplay(display);
        if (write(serial,"0",1)!=1) perror("guard stop tablet");
        if (tcsetattr(serial,TCSADRAIN,&original)==-1) perror("guard restore termios");
        close(serial);
        _exit(0);
    }
    close(pipes[0]); close(ready[1]); guard_write=pipes[1]; guard_status=ready[0];
    struct pollfd ack={guard_status,POLLIN,0};
    char response=0;
    if (poll(&ack,1,3000)<=0 || read(guard_status,&response,1)!=1 || response!='R') {
        fprintf(stderr,"cleanup guard did not become ready\n");
        kill(guard_pid,SIGKILL);
        while (waitpid(guard_pid,NULL,0)==-1 && errno==EINTR) {}
        guard_pid=-1;
        return 1;
    }
    XRRSelectInput(display,DefaultRootWindow(display),RRScreenChangeNotifyMask|
        RRCrtcChangeNotifyMask|RROutputChangeNotifyMask);
    if (pledge("stdio tty unix proc",NULL)==-1) { perror("pledge"); return 1; }
    fprintf(stderr,"ISDV4 19200 max=%ux%u pressure=%u; panel=%dx%d rotation=%u; guard=%d\n",
        max.x,max.y,max.pressure,panel.width,panel.height,panel.rotation,guard_pid);
    f[0]=(struct pollfd){serial,POLLIN,0};
    f[1]=(struct pollfd){ConnectionNumber(display),POLLIN,0};
    f[2]=(struct pollfd){guard_status,POLLIN,0};
    if (write(serial,"1",1)!=1) { perror("sampling"); return 1; }
    last_valid=milliseconds();
    while (!stopping) {
        if (!timed_out && milliseconds()-last_valid>=500) {
            set_buttons(0); parser=(struct parser){0}; timed_out=1;
        }
        if (parser.n && milliseconds()-frame_start>100) parser=(struct parser){0};
        while (XPending(display)) {
            XEvent event; XNextEvent(display,&event);
            if (event.type==ev || event.type==ev+RRNotify) {
                struct panel updated;
                XRRUpdateConfiguration(&event);
                if (!get_panel(&updated)) { set_buttons(0); stopping=1; }
                else if (panel.x!=updated.x || panel.y!=updated.y ||
                    panel.width!=updated.width || panel.height!=updated.height ||
                    panel.rotation!=updated.rotation) { set_buttons(0); panel=updated; }
            }
        }
        int count=poll(f,3,100);
        if (count<0) { if (errno==EINTR) continue; perror("poll"); break; }
        if (!count) continue;
        if ((f[0].revents|f[1].revents|f[2].revents)&(POLLERR|POLLHUP|POLLNVAL)) break;
        if (!(f[0].revents&POLLIN)) continue;
        n=read(serial,b,sizeof(b));
        if (n<0 && (errno==EAGAIN || errno==EINTR)) continue;
        if (n<=0) { perror("serial read"); break; }
        for (i=0;i<n && !stopping;i++) {
            if (b[i]&0x80) frame_start=milliseconds();
            if (frame(&parser,b[i])!=9) continue;
            struct sample s=decode(parser.b);
            frames++; tips+=s.tip;
            if (!s.proximity || s.x>max.x || s.y>max.y) { set_buttons(0); continue; }
            last_valid=milliseconds(); timed_out=0;
            position(s,max,panel,&x,&y);
            XTestFakeMotionEvent(display,DefaultScreen(display),x,y,CurrentTime);
            unsigned wanted=s.eraser?0:(s.tip | (s.side<<1));
            clicks+=!!((wanted&1) && !(buttons&1));
            set_buttons(wanted); motions++;
            if (verbose) fprintf(stderr,"pen x=%u y=%u pressure=%u prox=%u tip=%u side=%u eraser=%u -> %d,%d buttons=%u\n",
                s.x,s.y,s.pressure,s.proximity,s.tip,s.side,s.eraser,x,y,wanted);
        }
    }
    fprintf(stderr,"stopped: frames=%lu tip_frames=%lu motions=%lu tip_presses=%lu\n",frames,tips,motions,clicks);
    return 0;
}
