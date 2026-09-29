/* Implémentation Linux / X11 : événements PropertyNotify (_NET_ACTIVE_WINDOW,
 * _NET_WM_NAME) pour ne se réveiller que lorsque quelque chose change. */
#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#ifdef CM_HAVE_XSS
#include <X11/extensions/scrnsaver.h>
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/cm_util.h"
#include "platform.h"

static Display *g_dpy;
static Window g_root, g_watched;
static Atom A_ACTIVE, A_NAME, A_UTF8, A_PID;
static volatile sig_atomic_t g_quit;
static int g_lock_fd = -1;
#ifdef CM_HAVE_XSS
static int g_has_xss;
static XScreenSaverInfo *g_xss;
#endif

static int ignore_x_errors(Display *d, XErrorEvent *e)
{
    (void)d; (void)e; /* BadWindow quand une fenêtre disparaît : normal */
    return 0;
}

static void on_signal(int sig) { (void)sig; g_quit = 1; }

void plat_set_event_hook(void (*fn)(int ev)) { (void)fn; /* signaux gérés dans plat_wait */ }

int plat_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "cmon-tracker: impossible d'ouvrir l'affichage X11 (DISPLAY=%s)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "");
        return -1;
    }
    XSetErrorHandler(ignore_x_errors);
    g_root = DefaultRootWindow(g_dpy);
    A_ACTIVE = XInternAtom(g_dpy, "_NET_ACTIVE_WINDOW", False);
    A_NAME = XInternAtom(g_dpy, "_NET_WM_NAME", False);
    A_UTF8 = XInternAtom(g_dpy, "UTF8_STRING", False);
    A_PID = XInternAtom(g_dpy, "_NET_WM_PID", False);
    XSelectInput(g_dpy, g_root, PropertyChangeMask);
#ifdef CM_HAVE_XSS
    {
        int ev, err;
        g_has_xss = XScreenSaverQueryExtension(g_dpy, &ev, &err);
        if (g_has_xss) g_xss = XScreenSaverAllocInfo();
    }
#endif
    XFlush(g_dpy);
    return 0;
}

void plat_shutdown(void)
{
#ifdef CM_HAVE_XSS
    if (g_xss) XFree(g_xss);
#endif
    if (g_dpy) XCloseDisplay(g_dpy);
    if (g_lock_fd >= 0) close(g_lock_fd);
}

int plat_single_instance(const char *db_path)
{
    char lock[1100];
    snprintf(lock, sizeof lock, "%s.lock", db_path);
    g_lock_fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (g_lock_fd < 0) return 0; /* pas de verrou possible : on continue quand même */
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) return -1;
    return 0;
}

static Window active_window(void)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    Window w = 0;
    if (XGetWindowProperty(g_dpy, g_root, A_ACTIVE, 0, 1, False, XA_WINDOW, &type, &fmt, &n, &after, &data) ==
            Success && data && n == 1)
        w = *(Window *)data;
    if (data) XFree(data);
    return w;
}

/* Surveille les changements de titre de la fenêtre active uniquement. */
static void watch_window(Window w)
{
    if (w == g_watched) return;
    if (g_watched) XSelectInput(g_dpy, g_watched, NoEventMask);
    g_watched = w;
    if (w) XSelectInput(g_dpy, w, PropertyChangeMask);
}

int plat_wait(int timeout_ms)
{
    int changed = 0;
    if (g_quit) return PLAT_EV_QUIT;
    XFlush(g_dpy);
    if (!XPending(g_dpy)) {
        fd_set fds;
        struct timeval tv;
        int fd = ConnectionNumber(g_dpy);
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(fd + 1, &fds, NULL, NULL, &tv) < 0 && errno != EINTR) return PLAT_EV_QUIT;
    }
    if (g_quit) return PLAT_EV_QUIT;
    while (XPending(g_dpy)) {
        XEvent ev;
        XNextEvent(g_dpy, &ev);
        if (ev.type == PropertyNotify) {
            Atom a = ev.xproperty.atom;
            if ((ev.xproperty.window == g_root && a == A_ACTIVE) ||
                (ev.xproperty.window == g_watched && (a == A_NAME || a == XA_WM_NAME)))
                changed = 1;
        }
    }
    return changed ? PLAT_EV_CHANGE : PLAT_EV_TIMEOUT;
}

static void window_title(Window w, char *out, size_t n)
{
    Atom type;
    int fmt;
    unsigned long len, after;
    unsigned char *data = NULL;
    out[0] = '\0';
    if (XGetWindowProperty(g_dpy, w, A_NAME, 0, 1024, False, A_UTF8, &type, &fmt, &len, &after, &data) ==
            Success && data && len) {
        cm_strlcpy(out, (char *)data, n);
    } else {
        XTextProperty tp;
        if (XGetWMName(g_dpy, w, &tp) && tp.value) {
            cm_strlcpy(out, (char *)tp.value, n);
            XFree(tp.value);
        }
    }
    if (data) XFree(data);
}

static uint32_t window_pid(Window w)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    uint32_t pid = 0;
    if (XGetWindowProperty(g_dpy, w, A_PID, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) ==
            Success && data && n == 1)
        pid = (uint32_t)*(unsigned long *)data;
    if (data) XFree(data);
    return pid;
}

static int read_small_file(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    size_t r;
    if (!f) return -1;
    r = fread(out, 1, n - 1, f);
    fclose(f);
    out[r] = '\0';
    while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ')) out[--r] = '\0';
    return (int)r;
}

/* Nom d'application pour un pid : basename de /proc/pid/exe, sinon /proc/pid/comm. */
static int pid_app(uint32_t pid, char *app, size_t an, char *path, size_t pn)
{
    char p[64], buf[512];
    ssize_t r;
    snprintf(p, sizeof p, "/proc/%u/exe", pid);
    r = readlink(p, buf, sizeof buf - 1);
    if (r > 0) {
        buf[r] = '\0';
        if (path) cm_strlcpy(path, buf, pn);
        cm_strlcpy(app, cm_basename(buf), an);
        return 0;
    }
    snprintf(p, sizeof p, "/proc/%u/comm", pid);
    return read_small_file(p, app, an) > 0 ? 0 : -1;
}

int plat_get_foreground(plat_foreground *out)
{
    Window w;
    memset(out, 0, sizeof *out);
    w = active_window();
    watch_window(w);
    if (!w) return -1;
    window_title(w, out->title, sizeof out->title);
    out->pid = window_pid(w);
    if (!out->pid || pid_app(out->pid, out->app, sizeof out->app, out->path, sizeof out->path) != 0) {
        /* pas de pid (fenêtre distante…) : on utilise la classe WM_CLASS */
        XClassHint ch;
        if (XGetClassHint(g_dpy, w, &ch)) {
            cm_strlcpy(out->app, ch.res_name ? ch.res_name : "inconnu", sizeof out->app);
            if (ch.res_name) XFree(ch.res_name);
            if (ch.res_class) XFree(ch.res_class);
        } else {
            return -1; /* fenêtre détruite entre-temps : pas de premier plan */
        }
    }
    return 0;
}

unsigned plat_idle_seconds(void)
{
#ifdef CM_HAVE_XSS
    if (g_has_xss && XScreenSaverQueryInfo(g_dpy, g_root, g_xss)) return (unsigned)(g_xss->idle / 1000);
#endif
    return 0;
}

int plat_is_locked(void)
{
#ifdef CM_HAVE_XSS
    if (g_has_xss && XScreenSaverQueryInfo(g_dpy, g_root, g_xss)) return g_xss->state == ScreenSaverOn;
#endif
    return 0;
}

void plat_app_display_name(const char *path, char *out, size_t n)
{
    Window w = active_window();
    XClassHint ch;
    (void)path;
    out[0] = '\0';
    /* WM_CLASS "classe" est généralement le nom lisible (ex. "Firefox", "Code") */
    if (w && XGetClassHint(g_dpy, w, &ch)) {
        if (ch.res_class) cm_strlcpy(out, ch.res_class, n);
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
}

int plat_list_processes(plat_process *out, int max)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    uid_t me = getuid();
    int count = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL && count < max) {
        char p[300], cmd[8];
        struct stat st;
        char *end;
        unsigned long pid = strtoul(e->d_name, &end, 10);
        if (*end || pid == 0) continue;
        snprintf(p, sizeof p, "/proc/%lu", pid);
        if (stat(p, &st) != 0 || st.st_uid != me) continue;
        /* ignorer threads noyau / zombies (cmdline vide) */
        snprintf(p, sizeof p, "/proc/%lu/cmdline", pid);
        if (read_small_file(p, cmd, sizeof cmd) <= 0) continue;
        if (pid_app((uint32_t)pid, out[count].app, sizeof out[count].app, NULL, 0) != 0) continue;
        out[count].pid = (uint32_t)pid;
        count++;
    }
    closedir(d);
    return count;
}
