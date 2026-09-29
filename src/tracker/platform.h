/* Interface entre la logique du tracker et le système d'exploitation. */
#ifndef CM_PLATFORM_H
#define CM_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t pid;
    char app[128];     /* nom d'exécutable normalisé, ex. "firefox" ou "chrome.exe" */
    char path[512];    /* chemin complet de l'exécutable si connu */
    char title[512];   /* titre de la fenêtre (UTF-8) */
} plat_foreground;

typedef struct {
    uint32_t pid;
    char app[128];
} plat_process;

enum {
    PLAT_EV_TIMEOUT = 0, /* délai écoulé, simple échantillonnage */
    PLAT_EV_CHANGE = 1,  /* la fenêtre active ou son titre a (peut-être) changé */
    PLAT_EV_SUSPEND = 2, /* mise en veille imminente */
    PLAT_EV_QUIT = 3     /* arrêt demandé (signal, fermeture de session) */
};

int plat_init(void);
void plat_shutdown(void);

/* 0 si c'est la seule instance, -1 si un tracker tourne déjà. */
int plat_single_instance(const char *db_path);

/* Fonction appelée directement depuis le gestionnaire d'événements système
 * (PLAT_EV_SUSPEND ou PLAT_EV_QUIT) : le système peut nous tuer ou nous
 * suspendre juste après, il faut donc enregistrer immédiatement. */
void plat_set_event_hook(void (*fn)(int ev));

/* Attend un événement système ou l'expiration de timeout_ms. */
int plat_wait(int timeout_ms);

/* 0 si une fenêtre au premier plan a été trouvée. */
int plat_get_foreground(plat_foreground *out);

/* Secondes écoulées depuis la dernière entrée clavier/souris. */
unsigned plat_idle_seconds(void);

/* 1 si la session est verrouillée (ou l'écran de veille actif). */
int plat_is_locked(void);

/* Nom lisible d'une application à partir de son exécutable (peut rester vide). */
void plat_app_display_name(const char *path, char *out, size_t n);

/* Liste les processus de l'utilisateur. Renvoie le nombre écrit dans out. */
int plat_list_processes(plat_process *out, int max);

#endif
