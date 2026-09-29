/* Utilitaires communs (temps, chemins, chaînes). */
#ifndef CM_UTIL_H
#define CM_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define CM_VERSION "0.1.0"

/* États d'un segment d'activité. */
enum {
    CM_STATE_ACTIVE = 0, /* l'utilisateur travaille dans une application */
    CM_STATE_IDLE = 1,   /* aucune entrée clavier/souris depuis le seuil */
    CM_STATE_LOCKED = 2  /* session verrouillée / économiseur d'écran */
};

int64_t cm_now(void);
int cm_localtime(int64_t t, struct tm *out);
int64_t cm_mktime_local(struct tm *tm);

/* Chemin de la base par défaut (UTF-8). Crée le dossier parent.
 * La variable d'environnement CMON_DB permet de le surcharger. */
int cm_default_db_path(char *buf, size_t n);
int cm_mkdirs_for_file(const char *path_utf8);

void cm_strlcpy(char *dst, const char *src, size_t n);
void cm_str_tolower(char *s);
const char *cm_basename(const char *path);

/* "2h 05m", "12m 03s", "42s" */
void cm_fmt_duration(int64_t secs, char *buf, size_t n);
/* "HH:MM:SS" en heure locale */
void cm_fmt_clock(int64_t t, char *buf, size_t n);
/* "YYYY-MM-DD" en heure locale */
void cm_fmt_date(int64_t t, char *buf, size_t n);

#endif
