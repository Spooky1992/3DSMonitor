#include "cm_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

int64_t cm_now(void) { return (int64_t)time(NULL); }

int cm_localtime(int64_t t, struct tm *out)
{
    time_t tt = (time_t)t;
#ifdef _WIN32
    return localtime_s(out, &tt) == 0 ? 0 : -1;
#else
    return localtime_r(&tt, out) ? 0 : -1;
#endif
}

int64_t cm_mktime_local(struct tm *tm)
{
    tm->tm_isdst = -1;
    return (int64_t)mktime(tm);
}

void cm_strlcpy(char *dst, const char *src, size_t n)
{
    size_t len;
    if (n == 0) return;
    if (!src) src = "";
    len = strlen(src);
    if (len >= n) {
        len = n - 1;
        /* ne pas couper au milieu d'un caractère UTF-8 */
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void cm_str_tolower(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x80) *s = (char)tolower((unsigned char)*s);
}

const char *cm_basename(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

void cm_fmt_duration(int64_t s, char *buf, size_t n)
{
    if (s < 0) s = 0;
    if (s >= 3600)
        snprintf(buf, n, "%lldh %02lldm", (long long)(s / 3600), (long long)(s % 3600 / 60));
    else if (s >= 60)
        snprintf(buf, n, "%lldm %02llds", (long long)(s / 60), (long long)(s % 60));
    else
        snprintf(buf, n, "%llds", (long long)s);
}

void cm_fmt_clock(int64_t t, char *buf, size_t n)
{
    struct tm tm;
    if (cm_localtime(t, &tm) != 0) { cm_strlcpy(buf, "--:--:--", n); return; }
    strftime(buf, n, "%H:%M:%S", &tm);
}

void cm_fmt_date(int64_t t, char *buf, size_t n)
{
    struct tm tm;
    if (cm_localtime(t, &tm) != 0) { cm_strlcpy(buf, "0000-00-00", n); return; }
    strftime(buf, n, "%Y-%m-%d", &tm);
}

#ifdef _WIN32
static int mkdir_utf8(const char *p)
{
    wchar_t w[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, p, -1, w, (int)(sizeof w / sizeof w[0]))) return -1;
    if (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    return -1;
}
#else
static int mkdir_utf8(const char *p)
{
    if (mkdir(p, 0700) == 0 || errno == EEXIST) return 0;
    return -1;
}
#endif

int cm_mkdirs_for_file(const char *path)
{
    char tmp[1024];
    cm_strlcpy(tmp, path, sizeof tmp);
    char *last = NULL;
    for (char *p = tmp; *p; p++)
        if (*p == '/' || *p == '\\') last = p;
    if (!last) return 0;
    *last = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            if (!(p > tmp && p[-1] == ':')) mkdir_utf8(tmp); /* ignore "C:" */
            *p = c;
        }
    }
    return mkdir_utf8(tmp);
}

int cm_default_db_path(char *buf, size_t n)
{
    const char *env = getenv("CMON_DB");
    if (env && *env) {
        cm_strlcpy(buf, env, n);
        return cm_mkdirs_for_file(buf);
    }
#ifdef _WIN32
    wchar_t *w = _wgetenv(L"LOCALAPPDATA");
    char base[MAX_PATH * 3];
    if (!w || !WideCharToMultiByte(CP_UTF8, 0, w, -1, base, (int)sizeof base, NULL, NULL))
        return -1;
    snprintf(buf, n, "%s\\ComputerMonitor\\cmon.db", base);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(buf, n, "%s/computermonitor/cmon.db", xdg);
    else if (home && *home)
        snprintf(buf, n, "%s/.local/share/computermonitor/cmon.db", home);
    else
        return -1;
#endif
    return cm_mkdirs_for_file(buf);
}
