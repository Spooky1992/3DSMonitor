#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#include "../common/cm_db.h"
#include "../common/cm_util.h"
#include "cli.h"

/* ---------- périodes ---------- */

static int64_t make_date(int y, int m, int d)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    return cm_mktime_local(&tm);
}

static int parse_date(const char *s, int64_t *day)
{
    int y, m, d;
    char extra;
    if (sscanf(s, "%d-%d-%d%c", &y, &m, &d, &extra) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
    *day = make_date(y, m, d);
    return 0;
}

static void label_range(cli_ctx *c, const char *name)
{
    char a[16], b[16];
    cm_fmt_date(c->from, a, sizeof a);
    cm_fmt_date(c->to - 1, b, sizeof b);
    if (strcmp(a, b) == 0)
        snprintf(c->period_label, sizeof c->period_label, "%s%s%s%s", name ? name : "", name ? " (" : "", a,
                 name ? ")" : "");
    else
        snprintf(c->period_label, sizeof c->period_label, "%s%sdu %s au %s%s", name ? name : "", name ? " (" : "",
                 a, b, name ? ")" : "");
}

int cli_set_period(cli_ctx *c, const char *spec)
{
    int64_t now = cm_now(), today = cli_day_start(now);
    struct tm tm;
    int y, m, n;
    char extra;
    const char *name = NULL;

    cm_localtime(today, &tm);
    if (!strcmp(spec, "today") || !strcmp(spec, "aujourdhui")) {
        c->from = today; c->to = cli_next_day(today); name = "aujourd'hui";
    } else if (!strcmp(spec, "yesterday") || !strcmp(spec, "hier")) {
        c->from = make_date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday - 1); c->to = today; name = "hier";
    } else if (!strcmp(spec, "week") || !strcmp(spec, "semaine")) {
        int back = (tm.tm_wday + 6) % 7; /* lundi = début de semaine */
        c->from = make_date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday - back);
        c->to = cli_next_day(today); name = "cette semaine";
    } else if (!strcmp(spec, "lastweek")) {
        int back = (tm.tm_wday + 6) % 7 + 7;
        c->from = make_date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday - back);
        c->to = make_date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday - back + 7); name = "semaine dernière";
    } else if (!strcmp(spec, "month") || !strcmp(spec, "mois")) {
        c->from = make_date(tm.tm_year + 1900, tm.tm_mon + 1, 1);
        c->to = make_date(tm.tm_year + 1900, tm.tm_mon + 2, 1); name = "ce mois";
    } else if (!strcmp(spec, "lastmonth")) {
        c->from = make_date(tm.tm_year + 1900, tm.tm_mon, 1);
        c->to = make_date(tm.tm_year + 1900, tm.tm_mon + 1, 1); name = "mois dernier";
    } else if (!strcmp(spec, "year") || !strcmp(spec, "annee")) {
        c->from = make_date(tm.tm_year + 1900, 1, 1);
        c->to = make_date(tm.tm_year + 1901, 1, 1); name = "cette année";
    } else if (!strcmp(spec, "all") || !strcmp(spec, "tout")) {
        c->from = 0; c->to = now + 86400;
        cm_strlcpy(c->period_label, "tout l'historique", sizeof c->period_label);
        c->period_given = 1;
        return 0;
    } else if (sscanf(spec, "%dd%c", &n, &extra) == 1 && n > 0) {
        c->from = make_date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday - (n - 1));
        c->to = cli_next_day(today);
        char nm[40];
        snprintf(nm, sizeof nm, "%d derniers jours", n);
        label_range(c, nm);
        c->period_given = 1;
        return 0;
    } else if (parse_date(spec, &c->from) == 0) {
        c->to = cli_next_day(c->from);
    } else if (sscanf(spec, "%d-%d%c", &y, &m, &extra) == 2 && m >= 1 && m <= 12) {
        c->from = make_date(y, m, 1); c->to = make_date(y, m + 1, 1);
    } else if (sscanf(spec, "%d%c", &y, &extra) == 1 && y > 1970) {
        c->from = make_date(y, 1, 1); c->to = make_date(y + 1, 1, 1);
    } else {
        fprintf(stderr, "Période invalide : %s\n", spec);
        return -1;
    }
    label_range(c, name);
    c->period_given = 1;
    return 0;
}

void cli_default_period(cli_ctx *c, const char *spec)
{
    if (!c->period_given) cli_set_period(c, spec);
}

/* ---------- commandes utilitaires ---------- */

static int cmd_status(cli_ctx *c)
{
    sqlite3_stmt *st;
    int64_t first = 0, last = 0, rows = 0, apps = 0, titles = 0, procs = 0;
    FILE *f;
    long size = 0;
    char a[32], b[32], d[32];

    f = fopen(c->db_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
    }
    if (sqlite3_prepare_v2(c->db,
                           "SELECT (SELECT MIN(start) FROM activity), (SELECT MAX(end) FROM activity),"
                           " (SELECT COUNT(*) FROM activity), (SELECT COUNT(*) FROM apps),"
                           " (SELECT COUNT(*) FROM titles), (SELECT COUNT(*) FROM process_runs)",
                           -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            first = sqlite3_column_int64(st, 0);
            last = sqlite3_column_int64(st, 1);
            rows = sqlite3_column_int64(st, 2);
            apps = sqlite3_column_int64(st, 3);
            titles = sqlite3_column_int64(st, 4);
            procs = sqlite3_column_int64(st, 5);
        }
        sqlite3_finalize(st);
    }
    printf("Base de données   : %s (%.1f Mo)\n", c->db_path, size / 1048576.0);
    printf("Segments          : %lld   applications : %lld   titres : %lld   processus : %lld\n",
           (long long)rows, (long long)apps, (long long)titles, (long long)procs);
    if (rows) {
        cm_fmt_date(first, a, sizeof a);
        cm_fmt_date(last, b, sizeof b);
        cm_fmt_clock(last, d, sizeof d);
        printf("Historique        : du %s au %s (dernier enregistrement %s)\n", a, b, d);
        if (cm_now() - last < 120)
            printf("Tracker           : actif\n");
        else
            printf("Tracker           : aucun enregistrement récent (arrêté ?)\n");
    } else {
        printf("Aucune donnée : lancez cmon-tracker.\n");
    }
    return 0;
}

static int cmd_sql(cli_ctx *c)
{
    const char *sql;
    if (c->npos < 1) {
        fprintf(stderr, "usage : cmon sql \"SELECT ...\"  (vues utiles : v_activity, v_processes)\n");
        return 2;
    }
    sql = c->pos[0];
    while (sql && *sql) {
        sqlite3_stmt *st;
        const char *tail;
        int cols;
        if (sqlite3_prepare_v2(c->db, sql, -1, &st, &tail) != SQLITE_OK) {
            fprintf(stderr, "SQLite: %s\n", sqlite3_errmsg(c->db));
            return 1;
        }
        sql = tail;
        if (!st) continue;
        cols = sqlite3_column_count(st);
        for (int i = 0; i < cols; i++) {
            if (c->csv) cli_csv_field(stdout, sqlite3_column_name(st, i));
            else fputs(sqlite3_column_name(st, i), stdout);
            putchar(i + 1 < cols ? (c->csv ? ',' : '\t') : '\n');
        }
        while (sqlite3_step(st) == SQLITE_ROW) {
            for (int i = 0; i < cols; i++) {
                const char *v = (const char *)sqlite3_column_text(st, i);
                if (c->csv) cli_csv_field(stdout, v ? v : "");
                else fputs(v ? v : "NULL", stdout);
                putchar(i + 1 < cols ? (c->csv ? ',' : '\t') : '\n');
            }
        }
        if (sqlite3_errcode(c->db) != SQLITE_OK && sqlite3_errcode(c->db) != SQLITE_DONE &&
            sqlite3_errcode(c->db) != SQLITE_ROW)
            fprintf(stderr, "SQLite: %s\n", sqlite3_errmsg(c->db));
        sqlite3_finalize(st);
    }
    return 0;
}

static const char *DEFAULT_RULES[][3] = {
    {"Navigation web", "app", "*chrome*"},  {"Navigation web", "app", "*firefox*"},
    {"Navigation web", "app", "msedge*"},   {"Navigation web", "app", "*brave*"},
    {"Navigation web", "app", "opera*"},    {"Navigation web", "app", "vivaldi*"},
    {"Développement", "app", "code*"},      {"Développement", "app", "devenv*"},
    {"Développement", "app", "*idea*"},     {"Développement", "app", "clion*"},
    {"Développement", "app", "pycharm*"},   {"Développement", "app", "*terminal*"},
    {"Développement", "app", "cmd.exe"},    {"Développement", "app", "powershell*"},
    {"Développement", "app", "pwsh*"},      {"Développement", "app", "*vim*"},
    {"Développement", "app", "emacs*"},     {"Développement", "app", "*term*"},
    {"Développement", "app", "sublime*"},   {"Développement", "app", "notepad++*"},
    {"Bureautique", "app", "winword*"},     {"Bureautique", "app", "excel*"},
    {"Bureautique", "app", "powerpnt*"},    {"Bureautique", "app", "soffice*"},
    {"Bureautique", "app", "libreoffice*"}, {"Bureautique", "app", "acrobat*"},
    {"Bureautique", "app", "notepad.exe"},  {"Bureautique", "app", "obsidian*"},
    {"Communication", "app", "outlook*"},   {"Communication", "app", "thunderbird*"},
    {"Communication", "app", "*teams*"},    {"Communication", "app", "slack*"},
    {"Communication", "app", "discord*"},   {"Communication", "app", "zoom*"},
    {"Communication", "app", "telegram*"},  {"Communication", "app", "signal*"},
    {"Communication", "app", "whatsapp*"},
    {"Multimédia", "app", "spotify*"},      {"Multimédia", "app", "vlc*"},
    {"Jeux", "app", "steam*"},              {"Jeux", "app", "*epicgames*"},
    {"Système", "app", "explorer.exe"},     {"Système", "app", "taskmgr.exe"},
    {"Système", "app", "nautilus"},         {"Système", "app", "dolphin"},
    {"Vidéo en ligne", "title", "*youtube*"}, {"Vidéo en ligne", "title", "*netflix*"},
    {"Vidéo en ligne", "title", "*twitch*"},  {"Réseaux sociaux", "title", "*reddit*"},
    {"Réseaux sociaux", "title", "*facebook*"}, {"Réseaux sociaux", "title", "*instagram*"},
    {"Réseaux sociaux", "title", "* / x"},     {"Réseaux sociaux", "title", "*linkedin*"},
    {"Développement", "title", "*github*"},     {"Développement", "title", "*stack overflow*"},
};

static int insert_rule(cli_ctx *c, const char *cat, const char *field, const char *pattern, int prio)
{
    sqlite3_stmt *st;
    int rc;
    if (strcmp(field, "app") != 0 && strcmp(field, "title") != 0) {
        fprintf(stderr, "Le champ doit être 'app' ou 'title'\n");
        return -1;
    }
    if (sqlite3_prepare_v2(c->db, "INSERT INTO rules(category,field,pattern,priority) VALUES(?1,?2,?3,?4)", -1,
                           &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, cat, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, field, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 4, prio);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int cmd_rule(cli_ctx *c)
{
    const char *sub = c->npos ? c->pos[0] : "list";
    if (!strcmp(sub, "list")) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(c->db, "SELECT id, priority, category, field, pattern FROM rules "
                                      "ORDER BY category, priority DESC, id", -1, &st, NULL) != SQLITE_OK)
            return 1;
        printf("%-5s %-5s %-23s %-6s %s\n", "ID", "PRIO", "CATÉGORIE", "CHAMP", "MOTIF");
        while (sqlite3_step(st) == SQLITE_ROW) {
            printf("%-5lld %-5d ", (long long)sqlite3_column_int64(st, 0), sqlite3_column_int(st, 1));
            cli_print_cell((const char *)sqlite3_column_text(st, 2), 22);
            printf(" %-6s %s\n", sqlite3_column_text(st, 3), sqlite3_column_text(st, 4));
        }
        sqlite3_finalize(st);
        return 0;
    }
    if (!strcmp(sub, "add")) {
        if (c->npos < 4) {
            fprintf(stderr, "usage : cmon rule add CATÉGORIE app|title MOTIF [PRIORITÉ]\n"
                            "  ex. : cmon rule add Jeux app \"*minecraft*\"\n");
            return 2;
        }
        if (insert_rule(c, c->pos[1], c->pos[2], c->pos[3], c->npos > 4 ? atoi(c->pos[4]) : 0) != 0) return 1;
        printf("Règle ajoutée.\n");
        return 0;
    }
    if (!strcmp(sub, "del") || !strcmp(sub, "rm")) {
        char sql[128];
        if (c->npos < 2) { fprintf(stderr, "usage : cmon rule del ID\n"); return 2; }
        snprintf(sql, sizeof sql, "DELETE FROM rules WHERE id=%lld", atoll(c->pos[1]));
        if (cm_db_exec(c->db, sql) != 0) return 1;
        printf("%d règle(s) supprimée(s).\n", sqlite3_changes(c->db));
        return 0;
    }
    if (!strcmp(sub, "defaults")) {
        int n = (int)(sizeof DEFAULT_RULES / sizeof DEFAULT_RULES[0]);
        cm_db_exec(c->db, "BEGIN");
        for (int i = 0; i < n; i++)
            insert_rule(c, DEFAULT_RULES[i][0], DEFAULT_RULES[i][1], DEFAULT_RULES[i][2],
                        !strcmp(DEFAULT_RULES[i][1], "title") ? 10 : 0);
        cm_db_exec(c->db, "COMMIT");
        printf("%d règles par défaut ajoutées (les règles sur les titres sont prioritaires).\n", n);
        return 0;
    }
    fprintf(stderr, "sous-commande inconnue : %s (list, add, del, defaults)\n", sub);
    return 2;
}

/* ---------- démarrage automatique ---------- */

static int tracker_path(char *out, size_t n)
{
#ifdef _WIN32
    wchar_t w[MAX_PATH * 2];
    char buf[MAX_PATH * 3];
    DWORD len = GetModuleFileNameW(NULL, w, MAX_PATH * 2);
    if (!len || !WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof buf, NULL, NULL)) return -1;
    *(char *)cm_basename(buf) = '\0';
    snprintf(out, n, "%scmon-tracker.exe", buf);
#else
    char buf[1024];
    ssize_t r = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (r <= 0) return -1;
    buf[r] = '\0';
    *(char *)cm_basename(buf) = '\0';
    snprintf(out, n, "%scmon-tracker", buf);
#endif
    return 0;
}

static int cmd_autostart(cli_ctx *c)
{
    const char *sub = c->npos ? c->pos[0] : "status";
    char exe[1100];
    if (tracker_path(exe, sizeof exe) != 0) return 1;
#ifdef _WIN32
    {
        HKEY key;
        const wchar_t *run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        if (RegCreateKeyExW(HKEY_CURRENT_USER, run, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS)
            return 1;
        if (!strcmp(sub, "on")) {
            wchar_t wexe[MAX_PATH * 2], cmd[MAX_PATH * 2 + 4];
            MultiByteToWideChar(CP_UTF8, 0, exe, -1, wexe, MAX_PATH * 2);
            _snwprintf(cmd, MAX_PATH * 2 + 4, L"\"%ls\"", wexe);
            cmd[MAX_PATH * 2 + 3] = 0;
            RegSetValueExW(key, L"ComputerMonitor", 0, REG_SZ, (const BYTE *)cmd,
                           (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
            printf("Démarrage automatique activé : %s\n", exe);
        } else if (!strcmp(sub, "off")) {
            RegDeleteValueW(key, L"ComputerMonitor");
            printf("Démarrage automatique désactivé.\n");
        } else {
            DWORD type, size = 0;
            int on = RegQueryValueExW(key, L"ComputerMonitor", NULL, &type, NULL, &size) == ERROR_SUCCESS;
            printf("Démarrage automatique : %s\n", on ? "activé" : "désactivé");
        }
        RegCloseKey(key);
    }
#else
    {
        char path[1100];
        const char *home = getenv("HOME");
        const char *xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg) snprintf(path, sizeof path, "%s/autostart/cmon-tracker.desktop", xdg);
        else if (home) snprintf(path, sizeof path, "%s/.config/autostart/cmon-tracker.desktop", home);
        else return 1;
        if (!strcmp(sub, "on")) {
            FILE *f;
            cm_mkdirs_for_file(path);
            f = fopen(path, "w");
            if (!f) { perror(path); return 1; }
            fprintf(f, "[Desktop Entry]\nType=Application\nName=ComputerMonitor\n"
                       "Comment=Suivi de l'activité (cmon-tracker)\nExec=\"%s\"\n"
                       "X-GNOME-Autostart-enabled=true\nNoDisplay=true\n", exe);
            fclose(f);
            printf("Démarrage automatique activé : %s\n", path);
        } else if (!strcmp(sub, "off")) {
            remove(path);
            printf("Démarrage automatique désactivé.\n");
        } else {
            FILE *f = fopen(path, "r");
            printf("Démarrage automatique : %s\n", f ? "activé" : "désactivé");
            if (f) fclose(f);
        }
    }
#endif
    return 0;
}

/* ---------- aide et répartition ---------- */

static void usage(void)
{
    printf(
        "cmon %s - statistiques d'utilisation de l'ordinateur\n\n"
        "usage : cmon [COMMANDE] [OPTIONS]\n\n"
        "Statistiques :\n"
        "  summary      résumé de la période (défaut)\n"
        "  timeline     frise chronologique détaillée (--detail : chaque titre, --min S)\n"
        "  apps         classement des applications\n"
        "  titles       classement des fenêtres / documents (--app MOTIF)\n"
        "  categories   temps par catégorie (voir 'rule')\n"
        "  hours        répartition par heure de la journée\n"
        "  weekdays     répartition par jour de la semaine\n"
        "  days         bilan jour par jour (arrivée, départ, temps actif…)\n"
        "  focus        concentration : sessions, changements de fenêtre, records\n"
        "  procs        logiciels en cours d'exécution (--at \"AAAA-MM-JJ HH:MM\")\n\n"
        "Rapports et données :\n"
        "  report FICHIER.html   rapport HTML interactif (frise, graphiques)\n"
        "  export FICHIER.csv    export CSV de tous les segments\n"
        "  sql \"REQUÊTE\"         requête SQL libre (vues v_activity, v_processes)\n\n"
        "Configuration :\n"
        "  rule list|add|del|defaults   règles de catégories\n"
        "  autostart on|off|status      lancer le tracker à l'ouverture de session\n"
        "  status                       état de la base et du tracker\n\n"
        "Options :\n"
        "  -p, --period P   today, yesterday, week, lastweek, month, lastmonth, year, all,\n"
        "                   7d / 30d (N derniers jours), AAAA-MM-JJ, AAAA-MM, AAAA\n"
        "  --from DATE --to DATE   période personnalisée (dates incluses)\n"
        "  -n, --limit N    nombre de lignes (défaut 20)\n"
        "  --app MOTIF      filtre sur l'application (motif GLOB, ex. \"*chrome*\")\n"
        "  --csv            sortie CSV (commande sql)\n"
        "  --db FICHIER     base SQLite à utiliser\n",
        CM_VERSION);
}

typedef struct {
    const char *name;
    int (*fn)(cli_ctx *);
    const char *default_period;
} command;

static const command COMMANDS[] = {
    {"summary", cmd_summary, "today"},       {"timeline", cmd_timeline, "today"},
    {"apps", cmd_apps, "today"},             {"titles", cmd_titles, "today"},
    {"categories", cmd_categories, "today"}, {"hours", cmd_hours, "7d"},
    {"weekdays", cmd_weekdays, "all"},       {"days", cmd_days, "30d"},
    {"focus", cmd_focus, "today"},           {"procs", cmd_procs, "today"},
    {"report", cmd_report, "30d"},           {"export", cmd_export, "all"},
    {"sql", cmd_sql, "all"},                 {"rule", cmd_rule, "all"},
    {"autostart", cmd_autostart, "all"},     {"status", cmd_status, "all"},
};

int main(int argc, char **argv)
{
    cli_ctx c;
    const char *cmd = NULL, *from = NULL, *to = NULL;
    const command *found = NULL;
    int rc;

#ifdef _WIN32
    /* arguments et console en UTF-8 */
    {
        int wargc;
        wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv) {
            argv = (char **)calloc((size_t)wargc + 1, sizeof *argv);
            for (int i = 0; i < wargc; i++) {
                int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
                argv[i] = (char *)malloc((size_t)len);
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], len, NULL, NULL);
            }
            argc = wargc;
            LocalFree(wargv);
        }
        SetConsoleOutputCP(CP_UTF8);
    }
#endif

    memset(&c, 0, sizeof c);
    c.limit = 20;
    c.pos = (char **)calloc((size_t)argc + 1, sizeof *c.pos);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEED_VALUE() do { if (!v) { fprintf(stderr, "%s : valeur manquante\n", a); return 2; } i++; } while (0)
        if (!strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "help")) { usage(); return 0; }
        else if (!strcmp(a, "--version")) { printf("%s\n", CM_VERSION); return 0; }
        else if (!strcmp(a, "--db")) { NEED_VALUE(); cm_strlcpy(c.db_path, v, sizeof c.db_path); }
        else if (!strcmp(a, "-p") || !strcmp(a, "--period")) { NEED_VALUE(); if (cli_set_period(&c, v)) return 2; }
        else if (!strcmp(a, "--from")) { NEED_VALUE(); from = v; }
        else if (!strcmp(a, "--to")) { NEED_VALUE(); to = v; }
        else if (!strcmp(a, "-n") || !strcmp(a, "--limit")) { NEED_VALUE(); c.limit = atoi(v); }
        else if (!strcmp(a, "--app")) { NEED_VALUE(); c.app = v; }
        else if (!strcmp(a, "--at")) { NEED_VALUE(); c.at = v; }
        else if (!strcmp(a, "--min")) { NEED_VALUE(); c.min_secs = atoi(v); }
        else if (!strcmp(a, "--detail")) c.detail = 1;
        else if (!strcmp(a, "--csv")) c.csv = 1;
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "option inconnue : %s\n", a); return 2; }
        else if (!cmd) cmd = a;
        else c.pos[c.npos++] = argv[i];
#undef NEED_VALUE
    }
    if (c.limit <= 0) c.limit = 1000000;

    if (from || to) {
        int64_t f = 0, t = cm_now();
        if (from && parse_date(from, &f) != 0) { fprintf(stderr, "Date invalide : %s\n", from); return 2; }
        if (to) {
            if (parse_date(to, &t) != 0) { fprintf(stderr, "Date invalide : %s\n", to); return 2; }
            t = cli_next_day(t);
        } else {
            t = cli_next_day(cli_day_start(t));
        }
        c.from = f;
        c.to = t;
        c.period_given = 1;
        if (from) {
            char a[16], b[16];
            cm_fmt_date(c.from, a, sizeof a);
            cm_fmt_date(c.to - 1, b, sizeof b);
            snprintf(c.period_label, sizeof c.period_label, "du %s au %s", a, b);
        } else {
            cm_strlcpy(c.period_label, "jusqu'au ", sizeof c.period_label);
            cm_fmt_date(c.to - 1, c.period_label + strlen(c.period_label), 16);
        }
    }

    if (!cmd) cmd = "summary";
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++)
        if (!strcmp(COMMANDS[i].name, cmd)) found = &COMMANDS[i];
    if (!found) {
        fprintf(stderr, "Commande inconnue : %s (cmon --help)\n", cmd);
        return 2;
    }
    cli_default_period(&c, found->default_period);

    if (!c.db_path[0] && cm_default_db_path(c.db_path, sizeof c.db_path) != 0) {
        fprintf(stderr, "Impossible de déterminer le dossier de données\n");
        return 1;
    }
    c.db = cm_db_open(c.db_path, 0);
    if (!c.db) return 1;
    rc = found->fn(&c);
    sqlite3_close(c.db);
    free(c.pos);
    return rc;
}
