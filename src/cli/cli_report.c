/* Rapport HTML interactif et export CSV. */
#include <stdlib.h>
#include <string.h>

#include "../common/cm_db.h"
#include "../common/cm_util.h"
#include "cli.h"
#include "report_template.h" /* généré par CMake à partir de report_template.html */

/* Table de hachage minimale id (int64) -> index. */
typedef struct {
    int64_t *keys;
    int *vals;
    size_t cap;
} idmap;

static void idmap_init(idmap *m, size_t n)
{
    m->cap = 64;
    while (m->cap < n * 2) m->cap *= 2;
    m->keys = (int64_t *)calloc(m->cap, sizeof *m->keys);
    m->vals = (int *)calloc(m->cap, sizeof *m->vals);
    if (!m->keys || !m->vals) { perror("calloc"); exit(1); }
}

/* Renvoie l'index existant, ou -1 après avoir réservé la case (valeur à remplir via *slot). */
static int idmap_find(idmap *m, int64_t key, int **slot)
{
    size_t i = (size_t)((uint64_t)key * 0x9E3779B97F4A7C15ull >> 20) & (m->cap - 1);
    while (m->keys[i] && m->keys[i] != key) i = (i + 1) & (m->cap - 1);
    if (m->keys[i] == key) return m->vals[i];
    m->keys[i] = key;
    *slot = &m->vals[i];
    return -1;
}

static void idmap_free(idmap *m)
{
    free(m->keys);
    free(m->vals);
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        case '<': fputs("\\u003c", f); break; /* jamais de "</script>" dans les données */
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", *p);
            else fputc(*p, f);
        }
    }
    fputc('"', f);
}

static void write_data(cli_ctx *c, FILE *f, const cli_segs *s)
{
    idmap apps, titles;
    const char **cats = NULL;
    int napps = 1, ntitles = 1, ncats = 0, first;
    int *app_idx = (int *)malloc((size_t)(s->n + 1) * sizeof(int));
    int *title_idx = (int *)malloc((size_t)(s->n + 1) * sizeof(int));
    int *cat_idx = (int *)malloc((size_t)(s->n + 1) * sizeof(int));
    char gen[64];
    cli_proc *procs;
    int nprocs;

    if (!app_idx || !title_idx || !cat_idx) { perror("malloc"); exit(1); }
    idmap_init(&apps, 256);
    idmap_init(&titles, (size_t)s->n + 1);
    cats = (const char **)calloc((size_t)s->n + 1, sizeof *cats);

    /* indexation : chaque application / titre / catégorie n'apparaît qu'une fois */
    for (int i = 0; i < s->n; i++) {
        const cli_seg *g = &s->v[i];
        int *slot = NULL, k;
        app_idx[i] = title_idx[i] = 0;
        if (g->app_id) {
            k = idmap_find(&apps, g->app_id, &slot);
            if (k < 0) *slot = k = napps++;
            app_idx[i] = k;
        }
        if (g->title_id && g->title[0]) {
            k = idmap_find(&titles, g->title_id, &slot);
            if (k < 0) *slot = k = ntitles++;
            title_idx[i] = k;
        }
        for (k = 0; k < ncats; k++)
            if (!strcmp(cats[k], g->category)) break;
        if (k == ncats) cats[ncats++] = g->category;
        cat_idx[i] = k;
    }

    {
        struct tm tm;
        cm_localtime(cm_now(), &tm);
        strftime(gen, sizeof gen, "%Y-%m-%d %H:%M", &tm);
    }
    fputs("{\"period\":", f);
    json_str(f, c->period_label);
    fputs(",\"generated\":", f);
    json_str(f, gen);
    fprintf(f, ",\"from\":%lld,\"to\":%lld", (long long)c->from, (long long)c->to);

    /* applications (index 0 = aucune) */
    {
        const cli_seg **by = (const cli_seg **)calloc((size_t)napps, sizeof *by);
        for (int i = 0; i < s->n; i++)
            if (app_idx[i] && !by[app_idx[i]]) by[app_idx[i]] = &s->v[i];
        fputs(",\n\"apps\":[{\"n\":\"\",\"d\":\"(bureau)\"}", f);
        for (int i = 1; i < napps; i++) {
            fputs(",{\"n\":", f);
            json_str(f, by[i]->app);
            fputs(",\"d\":", f);
            json_str(f, by[i]->display);
            fputc('}', f);
        }
        fputc(']', f);
        free(by);
    }
    /* titres (index 0 = vide) */
    {
        const char **by = (const char **)calloc((size_t)ntitles, sizeof *by);
        for (int i = 0; i < s->n; i++)
            if (title_idx[i] && !by[title_idx[i]]) by[title_idx[i]] = s->v[i].title;
        fputs(",\n\"titles\":[\"\"", f);
        for (int i = 1; i < ntitles; i++) {
            fputc(',', f);
            json_str(f, by[i]);
        }
        fputc(']', f);
        free(by);
    }
    fputs(",\n\"cats\":[", f);
    for (int i = 0; i < ncats; i++) {
        if (i) fputc(',', f);
        json_str(f, cats[i]);
    }
    fputs("],\n\"segs\":[", f);
    first = 1;
    for (int i = 0; i < s->n; i++) {
        const cli_seg *g = &s->v[i];
        fprintf(f, "%s[%lld,%lld,%d,%d,%d,%d]", first ? "" : ",", (long long)g->start, (long long)(g->end - g->start),
                g->state, app_idx[i], title_idx[i], cat_idx[i]);
        if (i % 8 == 7) fputc('\n', f);
        first = 0;
    }
    fputs("],\n\"procs\":[", f);
    nprocs = cli_load_procs(c, &procs);
    for (int i = 0; i < nprocs && i < 100; i++) {
        fputs(i ? ",{\"n\":" : "{\"n\":", f);
        json_str(f, procs[i].name);
        fprintf(f, ",\"s\":%lld,\"l\":%d}", (long long)procs[i].running, procs[i].launches);
    }
    free(procs);
    fputs("]}", f);

    idmap_free(&apps);
    idmap_free(&titles);
    free(cats);
    free(app_idx);
    free(title_idx);
    free(cat_idx);
}

int cmd_report(cli_ctx *c)
{
    const char *out = c->npos ? c->pos[0] : "cmon-report.html";
    const char *marker = "/*__CMON_DATA__*/";
    const char *pos = strstr(CMON_REPORT_TEMPLATE, marker);
    cli_segs s;
    FILE *f;

    if (!pos) { fprintf(stderr, "modèle de rapport invalide\n"); return 1; }
    if (cli_load_segments(c, &s, 1) != 0) return 1;
    f = fopen(out, "wb");
    if (!f) { perror(out); cli_free_segments(&s); return 1; }
    fwrite(CMON_REPORT_TEMPLATE, 1, (size_t)(pos - CMON_REPORT_TEMPLATE), f);
    write_data(c, f, &s);
    fputs(pos + strlen(marker), f);
    fclose(f);
    printf("Rapport (%s, %d segments) écrit dans %s\n", c->period_label, s.n, out);
    cli_free_segments(&s);
    return 0;
}

int cmd_export(cli_ctx *c)
{
    const char *out = c->npos ? c->pos[0] : "-";
    cli_segs s;
    FILE *f;
    static const char *STATES[] = {"active", "idle", "locked"};

    if (cli_load_segments(c, &s, 1) != 0) return 1;
    f = strcmp(out, "-") ? fopen(out, "wb") : stdout;
    if (!f) { perror(out); cli_free_segments(&s); return 1; }
    fputs("start,end,seconds,state,app,app_display,category,title\n", f);
    for (int i = 0; i < s.n; i++) {
        const cli_seg *g = &s.v[i];
        char a[32], b[32], da[16], db[16];
        cm_fmt_date(g->start, da, sizeof da);
        cm_fmt_clock(g->start, a, sizeof a);
        cm_fmt_date(g->end, db, sizeof db);
        cm_fmt_clock(g->end, b, sizeof b);
        fprintf(f, "%s %s,%s %s,%lld,%s,", da, a, db, b, (long long)(g->end - g->start), STATES[g->state % 3]);
        cli_csv_field(f, g->app);
        fputc(',', f);
        cli_csv_field(f, g->display);
        fputc(',', f);
        cli_csv_field(f, g->category);
        fputc(',', f);
        cli_csv_field(f, g->title);
        fputc('\n', f);
    }
    if (f != stdout) {
        fclose(f);
        fprintf(stderr, "%d segments exportés dans %s (%s)\n", s.n, out, c->period_label);
    }
    cli_free_segments(&s);
    return 0;
}
