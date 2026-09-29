#include "cm_db.h"

#include <stdio.h>
#include <string.h>

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS apps("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL UNIQUE,"          /* nom d'exécutable normalisé (ex. code.exe) */
    "  path TEXT NOT NULL DEFAULT '',"      /* dernier chemin complet connu */
    "  display TEXT NOT NULL DEFAULT '');"  /* nom lisible (ex. Visual Studio Code) */
    "CREATE TABLE IF NOT EXISTS titles("
    "  id INTEGER PRIMARY KEY,"
    "  app_id INTEGER NOT NULL REFERENCES apps(id),"
    "  title TEXT NOT NULL,"
    "  UNIQUE(app_id, title));"
    /* Une ligne = un intervalle continu dans un même état / application / titre.
     * start/end en secondes Unix (UTC). */
    "CREATE TABLE IF NOT EXISTS activity("
    "  id INTEGER PRIMARY KEY,"
    "  start INTEGER NOT NULL,"
    "  end INTEGER NOT NULL,"
    "  state INTEGER NOT NULL,"             /* 0 actif, 1 inactif, 2 verrouillé */
    "  app_id INTEGER REFERENCES apps(id),"
    "  title_id INTEGER REFERENCES titles(id));"
    "CREATE INDEX IF NOT EXISTS activity_start ON activity(start);"
    "CREATE INDEX IF NOT EXISTS activity_end ON activity(end);"
    /* Processus en cours d'exécution (échantillonnés). */
    "CREATE TABLE IF NOT EXISTS process_runs("
    "  id INTEGER PRIMARY KEY,"
    "  app_id INTEGER NOT NULL REFERENCES apps(id),"
    "  pid INTEGER NOT NULL,"
    "  start INTEGER NOT NULL,"
    "  end INTEGER NOT NULL,"
    "  closed INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS process_runs_start ON process_runs(start);"
    "CREATE INDEX IF NOT EXISTS process_runs_open ON process_runs(closed) WHERE closed=0;"
    /* Règles de catégorisation (motifs GLOB : *, ?, [abc]). */
    "CREATE TABLE IF NOT EXISTS rules("
    "  id INTEGER PRIMARY KEY,"
    "  category TEXT NOT NULL,"
    "  field TEXT NOT NULL CHECK(field IN ('app','title')),"
    "  pattern TEXT NOT NULL,"
    "  priority INTEGER NOT NULL DEFAULT 0);"
    /* Vue pratique pour les requêtes libres (cmon sql ...). */
    "CREATE VIEW IF NOT EXISTS v_activity AS SELECT"
    "  a.id, a.start, a.end, a.end - a.start AS seconds,"
    "  datetime(a.start,'unixepoch','localtime') AS start_local,"
    "  datetime(a.end,'unixepoch','localtime') AS end_local,"
    "  date(a.start,'unixepoch','localtime') AS day,"
    "  CAST(strftime('%H',a.start,'unixepoch','localtime') AS INTEGER) AS hour,"
    "  CAST(strftime('%w',a.start,'unixepoch','localtime') AS INTEGER) AS weekday,"
    "  CASE a.state WHEN 0 THEN 'active' WHEN 1 THEN 'idle' ELSE 'locked' END AS state,"
    "  p.name AS app, COALESCE(NULLIF(p.display,''), p.name) AS app_display,"
    "  t.title AS title"
    "  FROM activity a LEFT JOIN apps p ON p.id=a.app_id LEFT JOIN titles t ON t.id=a.title_id;"
    "CREATE VIEW IF NOT EXISTS v_processes AS SELECT"
    "  r.id, r.pid, p.name AS app, r.start, r.end, r.end - r.start AS seconds,"
    "  datetime(r.start,'unixepoch','localtime') AS start_local,"
    "  datetime(r.end,'unixepoch','localtime') AS end_local, r.closed"
    "  FROM process_runs r JOIN apps p ON p.id=r.app_id;"
    "PRAGMA user_version=1;";

int cm_db_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "SQLite: %s\n", err ? err : "erreur");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

sqlite3 *cm_db_open(const char *path, int readonly)
{
    sqlite3 *db = NULL;
    int flags = readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) {
        fprintf(stderr, "Impossible d'ouvrir la base %s : %s\n", path, db ? sqlite3_errmsg(db) : "?");
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    if (!readonly) {
        /* WAL : le tracker écrit pendant que les rapports lisent, écritures peu coûteuses. */
        cm_db_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
        if (cm_db_exec(db, SCHEMA) != 0) {
            sqlite3_close(db);
            return NULL;
        }
    }
    return db;
}

int64_t cm_db_app_find(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st;
    int64_t id = 0;
    if (sqlite3_prepare_v2(db, "SELECT id FROM apps WHERE name=?1", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return id;
}

int64_t cm_db_app_get(sqlite3 *db, const char *name, const char *path, const char *display)
{
    sqlite3_stmt *st;
    const char *sql =
        "INSERT INTO apps(name,path,display) VALUES(?1,COALESCE(?2,''),COALESCE(?3,'')) "
        "ON CONFLICT(name) DO UPDATE SET "
        "path=CASE WHEN COALESCE(?2,'')<>'' THEN ?2 ELSE path END, "
        "display=CASE WHEN COALESCE(?3,'')<>'' THEN ?3 ELSE display END";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (path && *path) sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    if (display && *display) sqlite3_bind_text(st, 3, display, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return cm_db_app_find(db, name);
}

int64_t cm_db_title_get(sqlite3 *db, int64_t app_id, const char *title)
{
    sqlite3_stmt *st;
    int64_t id = 0;
    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO titles(app_id,title) VALUES(?1,?2)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, app_id);
        sqlite3_bind_text(st, 2, title, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "SELECT id FROM titles WHERE app_id=?1 AND title=?2", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, app_id);
    sqlite3_bind_text(st, 2, title, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return id;
}
