/* Outil en ligne de commande "cmon" : statistiques et rapports. */
#ifndef CM_CLI_H
#define CM_CLI_H

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    sqlite3 *db;
    char db_path[1024];
    int64_t from, to;       /* période [from, to[ en secondes Unix */
    char period_label[96];
    int period_given;
    int limit;              /* -n */
    const char *app;        /* --app motif GLOB */
    const char *at;         /* --at "YYYY-MM-DD HH:MM" */
    int detail;             /* --detail */
    int min_secs;           /* --min */
    int csv;                /* --csv */
    int npos;               /* arguments positionnels (après la commande) */
    char **pos;
} cli_ctx;

/* Segment d'activité découpé à la période demandée. */
typedef struct {
    int64_t start, end;
    int state;
    int64_t app_id, title_id;
    char *app;      /* nom d'exécutable */
    char *display;  /* nom lisible */
    char *title;
    char *category;
} cli_seg;

typedef struct {
    cli_seg *v;
    int n, cap;
} cli_segs;

/* Temps d'exécution cumulé d'un logiciel (union des intervalles de ses processus). */
typedef struct {
    char name[128];
    int64_t running, first, last;
    int launches;
} cli_proc;

/* cli_main.c */
int cli_set_period(cli_ctx *c, const char *spec);
void cli_default_period(cli_ctx *c, const char *spec);

/* cli_common.c */
int cli_load_segments(cli_ctx *c, cli_segs *out, int with_titles);
void cli_free_segments(cli_segs *s);
void cli_bar(double fraction, int width);
void cli_print_cell(const char *s, int width); /* tronque/complète en colonnes (UTF-8) */
int cli_utf8_width(const char *s);
const char *cli_weekday_name(int wday); /* 0 = dimanche */
int64_t cli_day_start(int64_t t);
int64_t cli_next_day(int64_t day_start);
void cli_csv_field(FILE *f, const char *s);

/* cli_stats.c */
int cmd_summary(cli_ctx *c);
int cmd_apps(cli_ctx *c);
int cmd_titles(cli_ctx *c);
int cmd_categories(cli_ctx *c);
int cmd_timeline(cli_ctx *c);
int cmd_hours(cli_ctx *c);
int cmd_weekdays(cli_ctx *c);
int cmd_days(cli_ctx *c);
int cmd_focus(cli_ctx *c);
int cmd_procs(cli_ctx *c);
int cli_load_procs(cli_ctx *c, cli_proc **out); /* trié par temps décroissant, renvoie le nombre ou -1 */

/* cli_report.c */
int cmd_report(cli_ctx *c);
int cmd_export(cli_ctx *c);

#endif
