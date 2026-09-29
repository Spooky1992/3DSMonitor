/* Statistiques affichées dans le terminal. */
#include <stdlib.h>
#include <string.h>

#include "../common/cm_db.h"
#include "../common/cm_util.h"
#include "cli.h"

/* ---------- agrégats simples par clé texte ---------- */

typedef struct {
    const char *key;
    const char *label;
    const char *extra;
    int64_t secs;
    int count;
} agg;

typedef struct {
    agg *v;
    int n, cap;
} aggs;

static agg *agg_get(aggs *a, const char *key, const char *label)
{
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->v[i].key, key)) return &a->v[i];
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 64;
        a->v = (agg *)realloc(a->v, (size_t)a->cap * sizeof *a->v);
        if (!a->v) { perror("realloc"); exit(1); }
    }
    memset(&a->v[a->n], 0, sizeof a->v[0]);
    a->v[a->n].key = key;
    a->v[a->n].label = label;
    return &a->v[a->n++];
}

static int agg_cmp(const void *x, const void *y)
{
    const agg *a = (const agg *)x, *b = (const agg *)y;
    return (b->secs > a->secs) - (b->secs < a->secs);
}

static void agg_sort(aggs *a) { if (a->n) qsort(a->v, (size_t)a->n, sizeof *a->v, agg_cmp); }

/* ---------- sessions de concentration ----------
 * Une session = suite ininterrompue de segments actifs dans la même application
 * (les changements de titre ne coupent pas la session). */

typedef struct {
    int64_t start, end;
    const cli_seg *seg; /* premier segment (nom de l'application) */
} session;

static session *build_sessions(const cli_segs *s, int *count)
{
    session *out = (session *)malloc((size_t)(s->n + 1) * sizeof *out);
    int n = 0;
    if (!out) { perror("malloc"); exit(1); }
    for (int i = 0; i < s->n; i++) {
        const cli_seg *g = &s->v[i];
        if (g->state != CM_STATE_ACTIVE) continue;
        if (n && out[n - 1].seg->app_id == g->app_id && g->start - out[n - 1].end <= 1) {
            out[n - 1].end = g->end;
            continue;
        }
        out[n].start = g->start;
        out[n].end = g->end;
        out[n].seg = g;
        n++;
    }
    *count = n;
    return out;
}

/* Découpe [start,end[ en morceaux ne franchissant pas les heures pleines. */
typedef void (*chunk_fn)(void *ctx, const cli_seg *g, int64_t t, int64_t len, const struct tm *tm);

static void for_each_hour_chunk(const cli_seg *g, chunk_fn fn, void *ctx)
{
    int64_t t = g->start;
    while (t < g->end) {
        struct tm tm;
        int64_t next, len;
        cm_localtime(t, &tm);
        next = t + (3600 - (tm.tm_min * 60 + tm.tm_sec));
        len = (next < g->end ? next : g->end) - t;
        fn(ctx, g, t, len, &tm);
        t += len;
    }
}

static void print_header(cli_ctx *c, const char *what)
{
    printf("── %s ── %s", what, c->period_label);
    if (c->app) printf(" ── application : %s", c->app);
    printf("\n\n");
}

static void dur(int64_t s, char *buf) { cm_fmt_duration(s, buf, 32); }

static const char *state_name(int st)
{
    return st == CM_STATE_ACTIVE ? "actif" : st == CM_STATE_IDLE ? "inactif" : "verrouillé";
}

/* ---------- summary ---------- */

int cmd_summary(cli_ctx *c)
{
    cli_segs s;
    aggs apps = {0}, cats = {0};
    int64_t tot[3] = {0, 0, 0}, first = 0, last = 0;
    int nsess, has_cat = 0;
    session *sess, *longest = NULL;
    char a[32], b[32], d[32];

    if (cli_load_segments(c, &s, 0) != 0) return 1;
    print_header(c, "Résumé");
    if (s.n == 0) {
        printf("Aucune donnée pour cette période.\n");
        cli_free_segments(&s);
        return 0;
    }
    for (int i = 0; i < s.n; i++) {
        const cli_seg *g = &s.v[i];
        int64_t len = g->end - g->start;
        tot[g->state] += len;
        if (g->state == CM_STATE_ACTIVE) {
            if (!first) first = g->start;
            last = g->end;
            if (g->app[0]) agg_get(&apps, g->app, g->display)->secs += len;
            agg_get(&cats, g->category, g->category)->secs += len;
            if (strcmp(g->category, "(sans catégorie)") != 0) has_cat = 1;
        }
    }
    sess = build_sessions(&s, &nsess);
    for (int i = 0; i < nsess; i++)
        if (!longest || sess[i].end - sess[i].start > longest->end - longest->start) longest = &sess[i];

    dur(tot[0], a);
    printf("  Temps actif          %s\n", a);
    if (first) {
        cm_fmt_clock(first, a, sizeof a);
        cm_fmt_clock(last, b, sizeof b);
        printf("  Première activité    %s    dernière : %s\n", a, b);
    }
    dur(tot[1], a);
    dur(tot[2], b);
    printf("  Inactif              %s    verrouillé : %s\n", a, b);
    printf("  Applications         %d\n", apps.n);
    if (nsess) {
        double per_hour = tot[0] > 0 ? (nsess - 1) * 3600.0 / (double)tot[0] : 0;
        dur(tot[0] / nsess, a);
        printf("  Changements d'app    %d  (%.0f par heure active)\n", nsess - 1, per_hour);
        printf("  Session moyenne      %s\n", a);
        dur(longest->end - longest->start, a);
        cm_fmt_clock(longest->start, b, sizeof b);
        cm_fmt_clock(longest->end, d, sizeof d);
        printf("  Plus longue session  %s dans %s (%s → %s)\n", a, longest->seg->display, b, d);
    }

    agg_sort(&apps);
    printf("\n  Top applications\n");
    for (int i = 0; i < apps.n && i < 8; i++) {
        double f = tot[0] ? (double)apps.v[i].secs / (double)tot[0] : 0;
        dur(apps.v[i].secs, a);
        printf("    ");
        cli_print_cell(apps.v[i].label, 26);
        printf(" %9s %5.1f%% ", a, f * 100);
        cli_bar(f, 25);
        putchar('\n');
    }
    if (has_cat) {
        agg_sort(&cats);
        printf("\n  Catégories\n");
        for (int i = 0; i < cats.n && i < 8; i++) {
            double f = tot[0] ? (double)cats.v[i].secs / (double)tot[0] : 0;
            dur(cats.v[i].secs, a);
            printf("    ");
            cli_print_cell(cats.v[i].label, 26);
            printf(" %9s %5.1f%% ", a, f * 100);
            cli_bar(f, 25);
            putchar('\n');
        }
    }
    free(sess);
    free(apps.v);
    free(cats.v);
    cli_free_segments(&s);
    return 0;
}

/* ---------- apps ---------- */

int cmd_apps(cli_ctx *c)
{
    cli_segs s;
    aggs apps = {0};
    int64_t total = 0;
    int nsess;
    session *sess;
    char a[32], b[32];

    if (cli_load_segments(c, &s, 0) != 0) return 1;
    print_header(c, "Applications");
    for (int i = 0; i < s.n; i++) {
        const cli_seg *g = &s.v[i];
        if (g->state != CM_STATE_ACTIVE || !g->app[0]) continue;
        agg *x = agg_get(&apps, g->app, g->display);
        x->secs += g->end - g->start;
        x->extra = g->category;
        total += g->end - g->start;
    }
    sess = build_sessions(&s, &nsess);
    for (int i = 0; i < nsess; i++)
        if (sess[i].seg->app[0]) agg_get(&apps, sess[i].seg->app, sess[i].seg->display)->count++;
    agg_sort(&apps);

    printf("  %-3s %-28s %-21s %9s %6s  %-20s %8s %9s\n", "#", "APPLICATION", "EXÉCUTABLE", "TEMPS", "%", "",
           "SESSIONS", "MOY.");
    for (int i = 0; i < apps.n && i < c->limit; i++) {
        agg *x = &apps.v[i];
        double f = total ? (double)x->secs / (double)total : 0;
        dur(x->secs, a);
        dur(x->count ? x->secs / x->count : 0, b);
        printf("  %-3d ", i + 1);
        cli_print_cell(x->label, 28);
        putchar(' ');
        cli_print_cell(x->key, 20);
        printf(" %9s %5.1f%%  ", a, f * 100);
        cli_bar(f, 20);
        printf(" %8d %9s\n", x->count, b);
    }
    dur(total, a);
    printf("\n  Total actif : %s, %d applications\n", a, apps.n);
    free(sess);
    free(apps.v);
    cli_free_segments(&s);
    return 0;
}

/* ---------- titles ---------- */

int cmd_titles(cli_ctx *c)
{
    sqlite3_stmt *st;
    int64_t total = 0;
    int rank = 0;
    char a[32];
    const char *sql =
        "SELECT COALESCE(NULLIF(p.display,''),p.name), COALESCE(t.title,''),"
        " SUM(MIN(a.end,?2)-MAX(a.start,?1)) AS d, COUNT(*),"
        " SUM(SUM(MIN(a.end,?2)-MAX(a.start,?1))) OVER ()"
        " FROM activity a JOIN apps p ON p.id=a.app_id LEFT JOIN titles t ON t.id=a.title_id"
        " WHERE a.state=0 AND a.end>?1 AND a.start<?2"
        "   AND (?3 IS NULL OR lower(p.name) GLOB lower(?3) OR lower(p.display) GLOB lower(?3))"
        " GROUP BY a.app_id, a.title_id ORDER BY d DESC LIMIT ?4";

    print_header(c, "Fenêtres et documents");
    if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQLite: %s\n", sqlite3_errmsg(c->db));
        return 1;
    }
    sqlite3_bind_int64(st, 1, c->from);
    sqlite3_bind_int64(st, 2, c->to);
    if (c->app) sqlite3_bind_text(st, 3, c->app, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 4, c->limit);
    printf("  %-3s %9s %6s  %-20s %-60s\n", "#", "TEMPS", "%", "APPLICATION", "TITRE");
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t d = sqlite3_column_int64(st, 2);
        const char *title = (const char *)sqlite3_column_text(st, 1);
        total = sqlite3_column_int64(st, 4);
        dur(d, a);
        printf("  %-3d %9s %5.1f%%  ", ++rank, a, total ? 100.0 * (double)d / (double)total : 0);
        cli_print_cell((const char *)sqlite3_column_text(st, 0), 20);
        putchar(' ');
        cli_print_cell(title && *title ? title : "(sans titre)", 70);
        putchar('\n');
    }
    sqlite3_finalize(st);
    if (!rank) printf("Aucune donnée pour cette période.\n");
    return 0;
}

/* ---------- categories ---------- */

int cmd_categories(cli_ctx *c)
{
    cli_segs s;
    aggs cats = {0};
    int64_t total = 0;
    char a[32];

    if (cli_load_segments(c, &s, 0) != 0) return 1;
    print_header(c, "Catégories");
    for (int i = 0; i < s.n; i++) {
        const cli_seg *g = &s.v[i];
        if (g->state != CM_STATE_ACTIVE) continue;
        agg_get(&cats, g->category, g->category)->secs += g->end - g->start;
        total += g->end - g->start;
    }
    agg_sort(&cats);
    for (int i = 0; i < cats.n && i < c->limit; i++) {
        double f = total ? (double)cats.v[i].secs / (double)total : 0;
        dur(cats.v[i].secs, a);
        printf("  ");
        cli_print_cell(cats.v[i].label, 26);
        printf(" %9s %5.1f%%  ", a, f * 100);
        cli_bar(f, 30);
        putchar('\n');
        if (c->detail) {
            aggs apps = {0};
            for (int k = 0; k < s.n; k++) {
                const cli_seg *g = &s.v[k];
                if (g->state == CM_STATE_ACTIVE && !strcmp(g->category, cats.v[i].key))
                    agg_get(&apps, g->app, g->display)->secs += g->end - g->start;
            }
            agg_sort(&apps);
            for (int k = 0; k < apps.n && k < 5; k++) {
                dur(apps.v[k].secs, a);
                printf("      · ");
                cli_print_cell(apps.v[k].label, 20);
                printf(" %9s\n", a);
            }
            free(apps.v);
        }
    }
    if (cats.n == 1 && !strcmp(cats.v[0].key, "(sans catégorie)"))
        printf("\n  Astuce : 'cmon rule defaults' ajoute des catégories courantes,\n"
               "           'cmon rule add Jeux app \"*minecraft*\"' crée les vôtres.\n");
    free(cats.v);
    cli_free_segments(&s);
    return 0;
}

/* ---------- timeline ---------- */

int cmd_timeline(cli_ctx *c)
{
    cli_segs s;
    char day[16] = "", a[32], b[32], d[32];
    int shown = 0;

    if (cli_load_segments(c, &s, 1) != 0) return 1;
    print_header(c, "Frise chronologique");
    for (int i = 0; i < s.n;) {
        const cli_seg *g = &s.v[i], *best = g;
        int64_t start = g->start, end = g->end, bestlen = g->end - g->start;
        int j = i + 1;
        char dd[16];

        /* regroupe les segments contigus de la même application (sauf --detail) */
        while (!c->detail && j < s.n && s.v[j].state == g->state && s.v[j].app_id == g->app_id &&
               s.v[j].start - end <= 1) {
            if (s.v[j].end - s.v[j].start > bestlen) {
                best = &s.v[j];
                bestlen = s.v[j].end - s.v[j].start;
            }
            end = s.v[j].end;
            j++;
        }
        if (i > 0 && start - s.v[i - 1].end > 60) {
            dur(start - s.v[i - 1].end, a);
            printf("      ┄┄┄ ordinateur éteint ou en veille (%s) ┄┄┄\n", a);
        }
        i = j;
        if (end - start < c->min_secs) continue;

        cm_fmt_date(start, dd, sizeof dd);
        if (strcmp(dd, day) != 0) {
            struct tm tm;
            cm_localtime(start, &tm);
            printf("%s%s %s\n", shown ? "\n" : "", cli_weekday_name(tm.tm_wday), dd);
            cm_strlcpy(day, dd, sizeof day);
        }
        cm_fmt_clock(start, a, sizeof a);
        cm_fmt_clock(end, b, sizeof b);
        dur(end - start, d);
        printf("  %s → %s %9s  ", a, b, d);
        if (g->state != CM_STATE_ACTIVE) {
            printf("[%s]\n", state_name(g->state));
        } else if (!g->app[0]) {
            printf("(bureau)\n");
        } else {
            cli_print_cell(g->display, 24);
            printf(" │ ");
            cli_print_cell(best->title, 70);
            putchar('\n');
        }
        shown++;
    }
    if (!shown) printf("Aucune donnée pour cette période.\n");
    cli_free_segments(&s);
    return 0;
}

/* ---------- hours / weekdays / days ---------- */

typedef struct {
    int64_t hours[24];
    int64_t wday[7];
    int wday_days[7];
    int ndays;
    int last_key;
    /* jours */
    struct day_row {
        int key;
        int64_t day_start, first, last, secs[3];
        int wday;
    } *days;
    int nd, capd;
} time_stats;

static void chunk_cb(void *ctx, const cli_seg *g, int64_t t, int64_t len, const struct tm *tm)
{
    time_stats *ts = (time_stats *)ctx;
    int key = (tm->tm_year + 1900) * 400 + tm->tm_yday;
    struct day_row *r;
    if (!ts->nd || ts->days[ts->nd - 1].key != key) {
        if (ts->nd == ts->capd) {
            ts->capd = ts->capd ? ts->capd * 2 : 64;
            ts->days = (struct day_row *)realloc(ts->days, (size_t)ts->capd * sizeof *ts->days);
            if (!ts->days) { perror("realloc"); exit(1); }
        }
        r = &ts->days[ts->nd++];
        memset(r, 0, sizeof *r);
        r->key = key;
        r->day_start = cli_day_start(t);
        r->wday = tm->tm_wday;
    }
    r = &ts->days[ts->nd - 1];
    r->secs[g->state] += len;
    if (g->state == CM_STATE_ACTIVE) {
        if (!r->first) r->first = t;
        r->last = t + len;
        ts->hours[tm->tm_hour] += len;
        ts->wday[tm->tm_wday] += len;
    }
}

static int load_time_stats(cli_ctx *c, time_stats *ts)
{
    cli_segs s;
    memset(ts, 0, sizeof *ts);
    if (cli_load_segments(c, &s, 0) != 0) return -1;
    for (int i = 0; i < s.n; i++) for_each_hour_chunk(&s.v[i], chunk_cb, ts);
    for (int i = 0; i < ts->nd; i++)
        if (ts->days[i].secs[CM_STATE_ACTIVE] > 0) {
            ts->ndays++;
            ts->wday_days[ts->days[i].wday]++;
        }
    cli_free_segments(&s);
    return 0;
}

int cmd_hours(cli_ctx *c)
{
    time_stats ts;
    int64_t max = 0, total = 0;
    char a[32], b[32];
    if (load_time_stats(c, &ts) != 0) return 1;
    print_header(c, "Activité par heure");
    for (int h = 0; h < 24; h++) {
        if (ts.hours[h] > max) max = ts.hours[h];
        total += ts.hours[h];
    }
    if (!total) { printf("Aucune donnée pour cette période.\n"); free(ts.days); return 0; }
    printf("  %-5s %-40s %9s  %s\n", "HEURE", "", "TOTAL", "MOY./JOUR ACTIF");
    for (int h = 0; h < 24; h++) {
        dur(ts.hours[h], a);
        dur(ts.ndays ? ts.hours[h] / ts.ndays : 0, b);
        printf("  %02dh   ", h);
        cli_bar(max ? (double)ts.hours[h] / (double)max : 0, 40);
        printf(" %9s  %s\n", a, b);
    }
    printf("\n  %d jour(s) avec activité\n", ts.ndays);
    free(ts.days);
    return 0;
}

int cmd_weekdays(cli_ctx *c)
{
    time_stats ts;
    int64_t max = 0;
    char a[32], b[32];
    if (load_time_stats(c, &ts) != 0) return 1;
    print_header(c, "Activité par jour de la semaine");
    for (int i = 0; i < 7; i++) {
        int64_t avg = ts.wday_days[i] ? ts.wday[i] / ts.wday_days[i] : 0;
        if (avg > max) max = avg;
    }
    printf("  %-9s %-36s %10s %9s %5s\n", "JOUR", "MOYENNE PAR JOUR ACTIF", "MOYENNE", "TOTAL", "JOURS");
    for (int k = 1; k <= 7; k++) {
        int i = k % 7; /* lundi → dimanche */
        int64_t avg = ts.wday_days[i] ? ts.wday[i] / ts.wday_days[i] : 0;
        dur(avg, a);
        dur(ts.wday[i], b);
        printf("  %-9s ", cli_weekday_name(i));
        cli_bar(max ? (double)avg / (double)max : 0, 36);
        printf(" %10s %9s %5d\n", a, b, ts.wday_days[i]);
    }
    free(ts.days);
    return 0;
}

int cmd_days(cli_ctx *c)
{
    time_stats ts;
    int64_t max = 0, sum[3] = {0, 0, 0}, first_sum = 0, last_sum = 0;
    char a[32], b[32], d[32], e[32], f[32];
    if (load_time_stats(c, &ts) != 0) return 1;
    print_header(c, "Bilan par jour");
    for (int i = 0; i < ts.nd; i++)
        if (ts.days[i].secs[0] > max) max = ts.days[i].secs[0];
    printf("  %-10s %-9s %-9s %-9s %9s %-24s %9s %9s\n", "DATE", "JOUR", "ARRIVÉE", "DÉPART", "ACTIF", "",
           "INACTIF", "VERROU");
    for (int i = 0; i < ts.nd; i++) {
        struct day_row *r = &ts.days[i];
        char date[16];
        if (!r->secs[0] && !r->secs[1] && !r->secs[2]) continue;
        cm_fmt_date(r->day_start, date, sizeof date);
        if (r->first) {
            cm_fmt_clock(r->first, a, sizeof a);
            cm_fmt_clock(r->last, b, sizeof b);
            a[5] = b[5] = '\0';
            first_sum += r->first - r->day_start;
            last_sum += r->last - r->day_start;
        } else {
            strcpy(a, "-");
            strcpy(b, "-");
        }
        dur(r->secs[0], d);
        dur(r->secs[1], e);
        dur(r->secs[2], f);
        printf("  %-10s %-9s %-8s %-8s %9s ", date, cli_weekday_name(r->wday), a, b, d);
        cli_bar(max ? (double)r->secs[0] / (double)max : 0, 24);
        printf(" %9s %9s\n", e, f);
        for (int k = 0; k < 3; k++) sum[k] += r->secs[k];
    }
    if (ts.ndays) {
        int64_t fa = first_sum / ts.ndays, la = last_sum / ts.ndays;
        dur(sum[0] / ts.ndays, d);
        dur(sum[0], e);
        printf("\n  Moyenne sur %d jour(s) actif(s) : %s actif/jour, arrivée %02lld:%02lld, départ %02lld:%02lld"
               " (total %s)\n",
               ts.ndays, d, (long long)(fa / 3600), (long long)(fa % 3600 / 60), (long long)(la / 3600),
               (long long)(la % 3600 / 60), e);
    } else {
        printf("Aucune donnée pour cette période.\n");
    }
    free(ts.days);
    return 0;
}

/* ---------- focus ---------- */

static int session_cmp_len(const void *x, const void *y)
{
    const session *a = (const session *)x, *b = (const session *)y;
    int64_t la = a->end - a->start, lb = b->end - b->start;
    return (lb > la) - (lb < la);
}

int cmd_focus(cli_ctx *c)
{
    static const struct { int64_t max; const char *label; } BUCKETS[] = {
        {10, "< 10 s"}, {60, "10 s – 1 min"}, {300, "1 – 5 min"}, {900, "5 – 15 min"},
        {1800, "15 – 30 min"}, {3600, "30 – 60 min"}, {INT64_MAX, "> 1 h"}};
    enum { NB = sizeof BUCKETS / sizeof BUCKETS[0] };
    cli_segs s;
    session *sess;
    int nsess, counts[NB] = {0};
    int64_t secs[NB] = {0}, total = 0, streak = 0, best_streak = 0, best_start = 0, streak_start = 0, prev_end = 0;
    char a[32], b[32], d[32];

    if (cli_load_segments(c, &s, 0) != 0) return 1;
    print_header(c, "Concentration");
    sess = build_sessions(&s, &nsess);
    if (!nsess) {
        printf("Aucune donnée pour cette période.\n");
        free(sess);
        cli_free_segments(&s);
        return 0;
    }

    /* plus longue période active sans pause (inactivité, verrouillage, arrêt) */
    for (int i = 0; i < s.n; i++) {
        const cli_seg *g = &s.v[i];
        if (g->state == CM_STATE_ACTIVE && streak && g->start - prev_end <= 1) {
            streak += g->end - g->start;
        } else if (g->state == CM_STATE_ACTIVE) {
            streak = g->end - g->start;
            streak_start = g->start;
        } else {
            streak = 0;
        }
        prev_end = g->end;
        if (streak > best_streak) {
            best_streak = streak;
            best_start = streak_start;
        }
    }

    for (int i = 0; i < nsess; i++) {
        int64_t len = sess[i].end - sess[i].start;
        int k = 0;
        while (len >= BUCKETS[k].max) k++;
        counts[k]++;
        secs[k] += len;
        total += len;
    }
    dur(total / nsess, a);
    printf("  Sessions (app ininterrompue)   %d, durée moyenne %s\n", nsess, a);
    printf("  Changements d'application      %.1f par heure active\n",
           total ? (nsess - 1) * 3600.0 / (double)total : 0.0);
    dur(best_streak, a);
    cm_fmt_clock(best_start, b, sizeof b);
    cm_fmt_date(best_start, d, sizeof d);
    printf("  Plus longue période sans pause %s (à partir de %s %s)\n\n", a, d, b);

    printf("  %-15s %8s %10s %6s\n", "DURÉE SESSION", "NOMBRE", "TEMPS", "%");
    for (int k = 0; k < NB; k++) {
        double f = total ? (double)secs[k] / (double)total : 0;
        dur(secs[k], a);
        printf("  ");
        cli_print_cell(BUCKETS[k].label, 15);
        printf(" %8d %10s %5.1f%% ", counts[k], a, f * 100);
        cli_bar(f, 25);
        putchar('\n');
    }

    qsort(sess, (size_t)nsess, sizeof *sess, session_cmp_len);
    printf("\n  Sessions les plus longues\n");
    for (int i = 0; i < nsess && i < (c->limit < 10 ? c->limit : 10); i++) {
        cm_fmt_date(sess[i].start, d, sizeof d);
        cm_fmt_clock(sess[i].start, a, sizeof a);
        cm_fmt_clock(sess[i].end, b, sizeof b);
        printf("    %s %s → %s  ", d, a, b);
        dur(sess[i].end - sess[i].start, a);
        printf("%9s  %s\n", a, sess[i].seg->display);
    }
    free(sess);
    cli_free_segments(&s);
    return 0;
}

/* ---------- procs ---------- */

static int proc_cmp(const void *x, const void *y)
{
    const cli_proc *a = (const cli_proc *)x, *b = (const cli_proc *)y;
    return (b->running > a->running) - (b->running < a->running);
}

static int parse_at(const char *s, int64_t *out)
{
    int y, mo, d, h, mi;
    struct tm tm;
    if (sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
        memset(&tm, 0, sizeof tm);
        tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi;
    } else if (sscanf(s, "%d:%d", &h, &mi) == 2) {
        cm_localtime(cm_now(), &tm);
        tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = 0;
    } else {
        return -1;
    }
    *out = cm_mktime_local(&tm);
    return 0;
}

static int procs_at(cli_ctx *c)
{
    sqlite3_stmt *st;
    int64_t t;
    int n = 0;
    char a[32];
    if (parse_at(c->at, &t) != 0) {
        fprintf(stderr, "--at attend \"AAAA-MM-JJ HH:MM\" ou \"HH:MM\"\n");
        return 2;
    }
    cm_fmt_date(t, a, sizeof a);
    printf("── Logiciels en cours d'exécution le %s à %s ──\n\n", a, c->at + (strchr(c->at, ' ') ? 11 : 0));
    if (sqlite3_prepare_v2(c->db,
                           "SELECT COALESCE(NULLIF(p.display,''),p.name), p.name FROM activity a"
                           " JOIN apps p ON p.id=a.app_id WHERE a.state=0 AND a.start<=?1 AND a.end>?1 LIMIT 1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, t);
        if (sqlite3_step(st) == SQLITE_ROW)
            printf("  Au premier plan : %s (%s)\n\n", sqlite3_column_text(st, 0), sqlite3_column_text(st, 1));
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(c->db,
                           "SELECT p.name, COUNT(*), MIN(r.start) FROM process_runs r JOIN apps p ON p.id=r.app_id"
                           " WHERE r.start<=?1 AND r.end>=?1 AND (?2 IS NULL OR lower(p.name) GLOB lower(?2))"
                           " GROUP BY p.id ORDER BY p.name",
                           -1, &st, NULL) != SQLITE_OK)
        return 1;
    sqlite3_bind_int64(st, 1, t);
    if (c->app) sqlite3_bind_text(st, 2, c->app, -1, SQLITE_STATIC);
    printf("  %-32s %9s  %s\n", "PROCESSUS", "INSTANCES", "LANCÉ LE");
    while (sqlite3_step(st) == SQLITE_ROW) {
        char when[16], date[16];
        cm_fmt_clock(sqlite3_column_int64(st, 2), when, sizeof when);
        cm_fmt_date(sqlite3_column_int64(st, 2), date, sizeof date);
        printf("  ");
        cli_print_cell((const char *)sqlite3_column_text(st, 0), 32);
        printf(" %9d  %s %s\n", sqlite3_column_int(st, 1), date, when);
        n++;
    }
    sqlite3_finalize(st);
    if (!n) printf("  (aucun relevé de processus à ce moment)\n");
    return 0;
}

int cli_load_procs(cli_ctx *c, cli_proc **out)
{
    sqlite3_stmt *st;
    cli_proc *v = NULL;
    int n = 0, cap = 0;
    int64_t cs = 0, ce = 0;

    *out = NULL;
    if (sqlite3_prepare_v2(c->db,
                           "SELECT p.name, MAX(r.start,?1), MIN(r.end,?2) FROM process_runs r"
                           " JOIN apps p ON p.id=r.app_id WHERE r.end>=?1 AND r.start<?2"
                           " AND (?3 IS NULL OR lower(p.name) GLOB lower(?3)) ORDER BY p.name, r.start",
                           -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQLite: %s\n", sqlite3_errmsg(c->db));
        return -1;
    }
    sqlite3_bind_int64(st, 1, c->from);
    sqlite3_bind_int64(st, 2, c->to);
    if (c->app) sqlite3_bind_text(st, 3, c->app, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        int64_t s0 = sqlite3_column_int64(st, 1), e0 = sqlite3_column_int64(st, 2);
        if (!n || strcmp(v[n - 1].name, name) != 0) {
            if (n) v[n - 1].running += ce - cs;
            if (n == cap) {
                cap = cap ? cap * 2 : 128;
                v = (cli_proc *)realloc(v, (size_t)cap * sizeof *v);
                if (!v) { perror("realloc"); exit(1); }
            }
            memset(&v[n], 0, sizeof v[n]);
            cm_strlcpy(v[n].name, name, sizeof v[n].name);
            v[n].first = s0;
            n++;
            cs = s0;
            ce = e0;
        } else if (s0 > ce) {
            /* union des intervalles : plusieurs processus d'un même logiciel ne comptent qu'une fois */
            v[n - 1].running += ce - cs;
            cs = s0;
            ce = e0;
        } else if (e0 > ce) {
            ce = e0;
        }
        v[n - 1].launches++;
        if (e0 > v[n - 1].last) v[n - 1].last = e0;
    }
    if (n) v[n - 1].running += ce - cs;
    sqlite3_finalize(st);
    if (n) qsort(v, (size_t)n, sizeof *v, proc_cmp);
    *out = v;
    return n;
}

int cmd_procs(cli_ctx *c)
{
    cli_proc *v;
    int n;
    int64_t first_seen = 0, span;
    char a[32], b[32], d[32];

    if (c->at) return procs_at(c);
    print_header(c, "Logiciels en cours d'exécution");
    n = cli_load_procs(c, &v);
    if (n < 0) return 1;
    if (!n) {
        printf("Aucun relevé de processus pour cette période.\n");
        return 0;
    }
    for (int i = 0; i < n; i++)
        if (!first_seen || v[i].first < first_seen) first_seen = v[i].first;
    span = (c->to < cm_now() ? c->to : cm_now()) - (c->from > first_seen ? c->from : first_seen);
    printf("  %-32s %10s %6s %9s  %-11s  %-11s\n", "PROCESSUS", "EN COURS", "%", "INSTANCES", "PREMIER", "DERNIER");
    for (int i = 0; i < n && i < c->limit; i++) {
        char fd[16], ld[16];
        dur(v[i].running, a);
        cm_fmt_date(v[i].first, fd, sizeof fd);
        cm_fmt_clock(v[i].first, b, sizeof b);
        cm_fmt_date(v[i].last, ld, sizeof ld);
        cm_fmt_clock(v[i].last, d, sizeof d);
        b[5] = d[5] = '\0';
        printf("  ");
        cli_print_cell(v[i].name, 32);
        printf(" %10s %5.1f%% %9d  %s %s  %s %s\n", a, span > 0 ? 100.0 * (double)v[i].running / (double)span : 0,
               v[i].launches, fd + 5, b, ld + 5, d);
    }
    printf("\n  %d logiciels distincts. Voir aussi : cmon procs --at \"HH:MM\"\n", n);
    free(v);
    return 0;
}
