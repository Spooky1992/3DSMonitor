/* cmon-tracker : service d'arrière-plan qui enregistre l'application au premier plan.
 *
 * Principe : on ne garde en mémoire que le segment courant (état + application +
 * titre). La base n'est touchée que lorsqu'il change, plus un "battement" périodique
 * qui repousse la fin du segment courant (en cas de crash on perd au plus quelques
 * secondes). Aucun travail n'est fait entre deux réveils. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/cm_db.h"
#include "../common/cm_util.h"
#include "platform.h"

#define MAX_PROCS 4096
#define APP_CACHE 64

static struct {
    char db_path[1024];
    unsigned idle_threshold; /* s sans entrée clavier/souris avant de passer "inactif" */
    int poll_ms;             /* intervalle de vérification (titre, inactivité) */
    int heartbeat;           /* s entre deux mises à jour de la fin du segment courant */
    int proc_interval;       /* s entre deux relevés des processus (0 = désactivé) */
    int no_titles;
    int verbose;
} opt = {"", 300, 2000, 30, 60, 0, 0};

static sqlite3 *db;
static sqlite3_stmt *st_insert, *st_update_end, *st_delete, *st_proc_insert, *st_proc_close;

typedef struct {
    int64_t row;
    int state;
    int64_t app_id, title_id;
    int64_t start, end;
    char app[128];
    char title[512];
} segment;

static segment cur, prev; /* prev : dernier segment fermé, pour recoller les micro-coupures */
static int64_t last_sample, last_flush, last_procs;
static volatile int stopping;

typedef struct {
    uint32_t pid;
    char app[128];
    int64_t row;
    int seen;
} proc_run;

static proc_run *runs;
static int nruns;
static plat_process *proc_buf;

static struct {
    char name[128];
    int64_t id;
} app_cache[APP_CACHE];
static int app_cache_next;

#define LOG(...) do { if (opt.verbose) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while (0)

static int prepare(const char *sql, sqlite3_stmt **st)
{
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, st, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQLite: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static void run_stmt(sqlite3_stmt *st)
{
    if (sqlite3_step(st) != SQLITE_DONE) LOG("SQLite: %s\n", sqlite3_errmsg(db));
    sqlite3_reset(st);
}

/* ---------- identifiants d'application (avec petit cache mémoire) ---------- */

static int64_t fg_app_id(const plat_foreground *fg)
{
    char display[256] = "";
    sqlite3_stmt *st;
    int64_t id;
    int need_display = 1;

    for (int i = 0; i < APP_CACHE; i++)
        if (app_cache[i].id && strcmp(app_cache[i].name, fg->app) == 0) return app_cache[i].id;

    /* nouvelle application pour cette exécution : calcule le nom lisible si inconnu */
    if (sqlite3_prepare_v2(db, "SELECT display FROM apps WHERE name=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, fg->app, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *d = (const char *)sqlite3_column_text(st, 0);
            need_display = !d || !*d;
        }
        sqlite3_finalize(st);
    }
    if (need_display) plat_app_display_name(fg->path, display, sizeof display);
    id = cm_db_app_get(db, fg->app, fg->path, display);

    cm_strlcpy(app_cache[app_cache_next].name, fg->app, sizeof app_cache[0].name);
    app_cache[app_cache_next].id = id;
    app_cache_next = (app_cache_next + 1) % APP_CACHE;
    return id;
}

/* ---------- segments d'activité ---------- */

static void seg_close(int64_t end)
{
    if (!cur.row) return;
    if (end <= cur.start) {
        sqlite3_bind_int64(st_delete, 1, cur.row);
        run_stmt(st_delete);
        prev.row = 0;
    } else {
        sqlite3_bind_int64(st_update_end, 1, end);
        sqlite3_bind_int64(st_update_end, 2, cur.row);
        run_stmt(st_update_end);
        cur.end = end;
        prev = cur;
    }
    cur.row = 0;
}

static void seg_open(int state, int64_t app_id, int64_t title_id, int64_t start, const char *app,
                     const char *title)
{
    /* A -> B (0 s) -> A : on reprend simplement la ligne précédente */
    if (prev.row && prev.end == start && prev.state == state && prev.app_id == app_id &&
        prev.title_id == title_id) {
        cur = prev;
        prev.row = 0;
        return;
    }
    sqlite3_bind_int64(st_insert, 1, start);
    sqlite3_bind_int(st_insert, 2, state);
    if (app_id) sqlite3_bind_int64(st_insert, 3, app_id); else sqlite3_bind_null(st_insert, 3);
    if (title_id) sqlite3_bind_int64(st_insert, 4, title_id); else sqlite3_bind_null(st_insert, 4);
    run_stmt(st_insert);
    cur.row = sqlite3_last_insert_rowid(db);
    cur.state = state;
    cur.app_id = app_id;
    cur.title_id = title_id;
    cur.start = cur.end = start;
    cm_strlcpy(cur.app, app, sizeof cur.app);
    cm_strlcpy(cur.title, title, sizeof cur.title);
    LOG("[%lld] %s %s | %s\n", (long long)start,
        state == CM_STATE_ACTIVE ? "ACTIF" : state == CM_STATE_IDLE ? "INACTIF" : "VERROUILLE", app, title);
}

/* ---------- processus en cours d'exécution ---------- */

static void procs_close_all(int64_t end)
{
    if (nruns == 0) return;
    cm_db_exec(db, "BEGIN");
    for (int i = 0; i < nruns; i++) {
        sqlite3_bind_int64(st_proc_close, 1, end);
        sqlite3_bind_int64(st_proc_close, 2, runs[i].row);
        run_stmt(st_proc_close);
    }
    cm_db_exec(db, "COMMIT");
    nruns = 0;
}

static void procs_sample(int64_t now)
{
    int n = plat_list_processes(proc_buf, MAX_PROCS);
    cm_db_exec(db, "BEGIN");
    for (int i = 0; i < nruns; i++) runs[i].seen = 0;
    for (int k = 0; k < n; k++) {
        int found = 0;
        for (int i = 0; i < nruns; i++) {
            if (runs[i].pid == proc_buf[k].pid && strcmp(runs[i].app, proc_buf[k].app) == 0) {
                runs[i].seen = found = 1;
                break;
            }
        }
        if (found || nruns >= MAX_PROCS) continue;
        {
            int64_t app_id = cm_db_app_get(db, proc_buf[k].app, NULL, NULL);
            proc_run *r = &runs[nruns++];
            r->pid = proc_buf[k].pid;
            cm_strlcpy(r->app, proc_buf[k].app, sizeof r->app);
            r->seen = 1;
            sqlite3_bind_int64(st_proc_insert, 1, app_id);
            sqlite3_bind_int64(st_proc_insert, 2, r->pid);
            sqlite3_bind_int64(st_proc_insert, 3, now);
            run_stmt(st_proc_insert);
            r->row = sqlite3_last_insert_rowid(db);
        }
    }
    for (int i = 0; i < nruns;) {
        if (!runs[i].seen) {
            sqlite3_bind_int64(st_proc_close, 1, now);
            sqlite3_bind_int64(st_proc_close, 2, runs[i].row);
            run_stmt(st_proc_close);
            runs[i] = runs[--nruns];
        } else {
            i++;
        }
    }
    cm_db_exec(db, "COMMIT");
    last_procs = now;
}

/* ---------- boucle principale ---------- */

static void flush(int64_t now)
{
    if (cur.row) {
        sqlite3_bind_int64(st_update_end, 1, now);
        sqlite3_bind_int64(st_update_end, 2, cur.row);
        run_stmt(st_update_end);
        cur.end = now;
    }
    if (nruns) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "UPDATE process_runs SET end=?1 WHERE closed=0", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    last_flush = now;
}

static void sample(void)
{
    int64_t now = cm_now();
    int64_t gap = opt.poll_ms / 1000 * 3 + 30;
    int state;
    unsigned idle = 0;
    plat_foreground fg;
    int64_t at, app_id = 0, title_id = 0;

    /* Trou dans les relevés (veille, hibernation) ou horloge qui recule :
     * on ferme tout à la date du dernier relevé. */
    if (last_sample && (now - last_sample > gap || now < last_sample)) {
        LOG("interruption de %lld s détectée\n", (long long)(now - last_sample));
        seg_close(last_sample);
        procs_close_all(last_sample);
        prev.row = 0;
    }
    last_sample = now;

    memset(&fg, 0, sizeof fg);
    if (plat_is_locked()) {
        state = CM_STATE_LOCKED;
    } else if ((idle = plat_idle_seconds()) >= opt.idle_threshold) {
        state = CM_STATE_IDLE;
    } else {
        state = CM_STATE_ACTIVE;
        if (plat_get_foreground(&fg) != 0) fg.app[0] = '\0';
        if (opt.no_titles || !fg.app[0]) fg.title[0] = '\0';
    }

    if (cur.row && cur.state == state && strcmp(cur.app, fg.app) == 0 && strcmp(cur.title, fg.title) == 0)
        return; /* rien n'a changé : aucune écriture */

    at = now;
    /* L'inactivité a commencé au dernier événement clavier/souris, pas maintenant. */
    if (state == CM_STATE_IDLE && cur.row && cur.state == CM_STATE_ACTIVE) {
        at = now - (int64_t)idle;
        if (at < cur.start) at = cur.start;
    }
    if (fg.app[0]) {
        app_id = fg_app_id(&fg);
        title_id = cm_db_title_get(db, app_id, fg.title);
    }
    seg_close(at);
    seg_open(state, app_id, title_id, at, fg.app, fg.title);
}

static void finish(void)
{
    int64_t now = cm_now();
    seg_close(now);
    procs_close_all(now);
}

/* Appelé directement par la plateforme juste avant une veille ou un arrêt. */
static void on_system_event(int ev)
{
    LOG("événement système %d\n", ev);
    finish();
    if (ev == PLAT_EV_QUIT) stopping = 1;
}

static void usage(void)
{
    printf("cmon-tracker %s - enregistre l'activité au premier plan\n\n"
           "Options :\n"
           "  --db FICHIER         base SQLite (défaut : dossier de données utilisateur)\n"
           "  --idle SECONDES      inactivité avant de passer en \"absent\" (défaut 300)\n"
           "  --poll SECONDES      intervalle de vérification (défaut 2)\n"
           "  --heartbeat SECONDES sauvegarde du segment courant (défaut 30)\n"
           "  --procs SECONDES     relevé des processus lancés, 0 = désactivé (défaut 60)\n"
           "  --no-titles          ne pas enregistrer les titres de fenêtres\n"
           "  -v, --verbose        affiche les changements détectés\n",
           CM_VERSION);
}

static int parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEED_VALUE() do { if (!v) { fprintf(stderr, "%s : valeur manquante\n", a); return -1; } i++; } while (0)
        if (!strcmp(a, "--db")) { NEED_VALUE(); cm_strlcpy(opt.db_path, v, sizeof opt.db_path); }
        else if (!strcmp(a, "--idle")) { NEED_VALUE(); opt.idle_threshold = (unsigned)atoi(v); }
        else if (!strcmp(a, "--poll")) { NEED_VALUE(); opt.poll_ms = atoi(v) * 1000; }
        else if (!strcmp(a, "--heartbeat")) { NEED_VALUE(); opt.heartbeat = atoi(v); }
        else if (!strcmp(a, "--procs")) { NEED_VALUE(); opt.proc_interval = atoi(v); }
        else if (!strcmp(a, "--no-titles")) opt.no_titles = 1;
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) opt.verbose = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); exit(0); }
        else if (!strcmp(a, "--version")) { printf("%s\n", CM_VERSION); exit(0); }
        else { fprintf(stderr, "option inconnue : %s\n", a); return -1; }
#undef NEED_VALUE
    }
    if (opt.poll_ms < 500) opt.poll_ms = 500;
    if (opt.heartbeat < 5) opt.heartbeat = 5;
    if (opt.idle_threshold < 10) opt.idle_threshold = 10;
    return 0;
}

int main(int argc, char **argv)
{
    if (parse_args(argc, argv) != 0) return 2;
    if (!opt.db_path[0] && cm_default_db_path(opt.db_path, sizeof opt.db_path) != 0) {
        fprintf(stderr, "Impossible de déterminer le dossier de données\n");
        return 1;
    }
    if (opt.db_path[0]) cm_mkdirs_for_file(opt.db_path);
    if (plat_single_instance(opt.db_path) != 0) {
        fprintf(stderr, "cmon-tracker est déjà en cours d'exécution\n");
        return 1;
    }
    db = cm_db_open(opt.db_path, 0);
    if (!db) return 1;
    if (prepare("INSERT INTO activity(start,end,state,app_id,title_id) VALUES(?1,?1,?2,?3,?4)", &st_insert) ||
        prepare("UPDATE activity SET end=?1 WHERE id=?2", &st_update_end) ||
        prepare("DELETE FROM activity WHERE id=?1", &st_delete) ||
        prepare("INSERT INTO process_runs(app_id,pid,start,end) VALUES(?1,?2,?3,?3)", &st_proc_insert) ||
        prepare("UPDATE process_runs SET end=?1, closed=1 WHERE id=?2", &st_proc_close))
        return 1;
    /* Processus restés "ouverts" après un arrêt brutal : fin = dernier battement. */
    cm_db_exec(db, "UPDATE process_runs SET closed=1 WHERE closed=0");

    proc_buf = (plat_process *)calloc(MAX_PROCS, sizeof *proc_buf);
    runs = (proc_run *)calloc(MAX_PROCS, sizeof *runs);
    if (!proc_buf || !runs) return 1;

    if (plat_init() != 0) return 1;
    plat_set_event_hook(on_system_event);
    LOG("cmon-tracker %s, base : %s\n", CM_VERSION, opt.db_path);

    last_flush = cm_now();
    sample();
    if (opt.proc_interval > 0) procs_sample(cm_now());

    while (!stopping) {
        int ev = plat_wait(opt.poll_ms);
        int64_t now;
        if (ev == PLAT_EV_QUIT || stopping) break;
        sample();
        now = cm_now();
        if (opt.proc_interval > 0 && now - last_procs >= opt.proc_interval) procs_sample(now);
        if (now - last_flush >= opt.heartbeat) flush(now);
    }

    finish();
    sqlite3_finalize(st_insert);
    sqlite3_finalize(st_update_end);
    sqlite3_finalize(st_delete);
    sqlite3_finalize(st_proc_insert);
    sqlite3_finalize(st_proc_close);
    sqlite3_close(db);
    plat_shutdown();
    free(proc_buf);
    free(runs);
    LOG("arrêt\n");
    return 0;
}
