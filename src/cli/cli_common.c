#include <stdlib.h>
#include <string.h>

#include "../common/cm_db.h"
#include "../common/cm_util.h"
#include "cli.h"

static char *dup_col(sqlite3_stmt *st, int col)
{
    const char *s = (const char *)sqlite3_column_text(st, col);
    size_t n = s ? strlen(s) : 0;
    char *d = (char *)malloc(n + 1);
    if (!d) { perror("malloc"); exit(1); }
    memcpy(d, s ? s : "", n);
    d[n] = '\0';
    return d;
}

int cli_load_segments(cli_ctx *c, cli_segs *out, int with_titles)
{
    sqlite3_stmt *st;
    const char *sql =
        "SELECT a.start, a.end, a.state, COALESCE(a.app_id,0), COALESCE(a.title_id,0),"
        " COALESCE(p.name,''), COALESCE(NULLIF(p.display,''), p.name, ''),"
        " CASE WHEN ?4 THEN COALESCE(t.title,'') ELSE '' END,"
        " CASE WHEN a.state=0 AND p.id IS NOT NULL THEN " CM_SQL_CATEGORY " ELSE '' END"
        " FROM activity a LEFT JOIN apps p ON p.id=a.app_id LEFT JOIN titles t ON t.id=a.title_id"
        " WHERE a.end > ?1 AND a.start < ?2"
        "   AND (?3 IS NULL OR lower(p.name) GLOB lower(?3) OR lower(p.display) GLOB lower(?3))"
        " ORDER BY a.start, a.id";

    memset(out, 0, sizeof *out);
    if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQLite: %s\n", sqlite3_errmsg(c->db));
        return -1;
    }
    sqlite3_bind_int64(st, 1, c->from);
    sqlite3_bind_int64(st, 2, c->to);
    if (c->app) sqlite3_bind_text(st, 3, c->app, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 4, with_titles);

    while (sqlite3_step(st) == SQLITE_ROW) {
        cli_seg *s;
        int64_t a = sqlite3_column_int64(st, 0), b = sqlite3_column_int64(st, 1);
        if (a < c->from) a = c->from;
        if (b > c->to) b = c->to;
        if (b <= a) continue;
        if (out->n == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 1024;
            out->v = (cli_seg *)realloc(out->v, (size_t)out->cap * sizeof *out->v);
            if (!out->v) { perror("realloc"); exit(1); }
        }
        s = &out->v[out->n++];
        s->start = a;
        s->end = b;
        s->state = sqlite3_column_int(st, 2);
        s->app_id = sqlite3_column_int64(st, 3);
        s->title_id = sqlite3_column_int64(st, 4);
        s->app = dup_col(st, 5);
        s->display = dup_col(st, 6);
        s->title = dup_col(st, 7);
        s->category = dup_col(st, 8);
    }
    sqlite3_finalize(st);
    return 0;
}

void cli_free_segments(cli_segs *s)
{
    for (int i = 0; i < s->n; i++) {
        free(s->v[i].app);
        free(s->v[i].display);
        free(s->v[i].title);
        free(s->v[i].category);
    }
    free(s->v);
    memset(s, 0, sizeof *s);
}

void cli_bar(double f, int width)
{
    static const char *eighths[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
    int units, full, rest, printed;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    units = (int)(f * width * 8 + 0.5);
    full = units / 8;
    rest = units % 8;
    for (int i = 0; i < full; i++) fputs("█", stdout);
    fputs(eighths[rest], stdout);
    printed = full + (rest ? 1 : 0);
    for (int i = printed; i < width; i++) putchar(' ');
}

int cli_utf8_width(const char *s)
{
    int w = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) w++;
    return w;
}

void cli_print_cell(const char *s, int width)
{
    int w = cli_utf8_width(s);
    if (w <= width) {
        fputs(s, stdout);
        for (int i = w; i < width; i++) putchar(' ');
        return;
    }
    /* tronque à width-1 caractères + "…" */
    {
        int count = 0;
        const char *p = s;
        while (*p) {
            if (((unsigned char)*p & 0xC0) != 0x80) {
                if (count == width - 1) break;
                count++;
            }
            p++;
        }
        fwrite(s, 1, (size_t)(p - s), stdout);
        fputs("…", stdout);
    }
}

const char *cli_weekday_name(int wday)
{
    static const char *names[] = {"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"};
    return names[((wday % 7) + 7) % 7];
}

int64_t cli_day_start(int64_t t)
{
    struct tm tm;
    cm_localtime(t, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return cm_mktime_local(&tm);
}

int64_t cli_next_day(int64_t day_start)
{
    struct tm tm;
    cm_localtime(day_start, &tm);
    tm.tm_mday += 1;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return cm_mktime_local(&tm);
}

void cli_csv_field(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"') fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}
