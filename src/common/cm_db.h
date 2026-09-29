/* Accès à la base SQLite : schéma et petites aides. */
#ifndef CM_DB_H
#define CM_DB_H

#include <sqlite3.h>
#include <stdint.h>

sqlite3 *cm_db_open(const char *path, int readonly);
int cm_db_exec(sqlite3 *db, const char *sql);

/* Renvoie l'id de l'application `name` ou 0 si inconnue. */
int64_t cm_db_app_find(sqlite3 *db, const char *name);
/* Crée l'application si besoin (met à jour chemin/nom affiché s'ils sont fournis). */
int64_t cm_db_app_get(sqlite3 *db, const char *name, const char *path, const char *display);
int64_t cm_db_title_get(sqlite3 *db, int64_t app_id, const char *title);

/* Expression SQL donnant la catégorie d'une ligne jointe avec les alias p (apps) et t (titles). */
#define CM_SQL_CATEGORY \
    "COALESCE((SELECT r.category FROM rules r WHERE " \
    "(r.field='app' AND lower(p.name) GLOB lower(r.pattern)) OR " \
    "(r.field='title' AND lower(COALESCE(t.title,'')) GLOB lower(r.pattern)) " \
    "ORDER BY r.priority DESC, r.id LIMIT 1),'(sans catégorie)')"

#endif
