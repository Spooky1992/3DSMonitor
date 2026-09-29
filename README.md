# ComputerMonitor

Suivi d'activité façon **ManicTime**, écrit en C : un petit service d'arrière-plan
enregistre en continu l'application au premier plan (et le titre de sa fenêtre),
les périodes d'inactivité, les verrouillages de session et les logiciels en cours
d'exécution. Un second outil, `cmon`, en tire des statistiques, une frise
chronologique et un rapport HTML interactif.

- **Windows** (cible principale) : Win32, hook `EVENT_SYSTEM_FOREGROUND`, notifications de session/veille.
- **Linux** : X11 (`_NET_ACTIVE_WINDOW`, XScreenSaver pour l'inactivité). Wayland n'expose pas la fenêtre active : non pris en charge.

## Très peu de ressources

- Pas de polling actif du premier plan : le tracker dort et se réveille sur les
  événements système (changement de fenêtre, verrouillage, veille), plus une
  vérification légère toutes les 2 s (titre, inactivité).
- Seul le segment courant est gardé en mémoire ; **la base n'est écrite que lorsque
  quelque chose change**, plus une mise à jour toutes les 30 s (au plus 30 s perdues en cas de crash).
- SQLite en mode WAL (écritures sans `fsync` systématique), requêtes préparées.
- Relevé des processus une fois par minute (désactivable avec `--procs 0`).
- Quelques Mo de RAM, CPU ≈ 0 %.

## Compilation

Prérequis : CMake ≥ 3.16 et un compilateur C11.

**Windows (MSYS2 / MinGW ou Visual Studio)**

```sh
cmake -S . -B build
cmake --build build --config Release
```

L'amalgamation SQLite est téléchargée automatiquement depuis sqlite.org. Hors
ligne, placez `sqlite3.c` et `sqlite3.h` dans `third_party/sqlite/` (ou passez
`-DCMON_SQLITE_DIR=...`). On obtient `cmon-tracker.exe` (sans fenêtre ni console) et `cmon.exe`.

**Linux (Debian/Ubuntu)**

```sh
sudo apt install build-essential cmake libsqlite3-dev libx11-dev libxss-dev
cmake -S . -B build && cmake --build build
```

## Utilisation

```sh
cmon-tracker            # lance le suivi (ou : cmon autostart on)
cmon                    # résumé du jour
cmon timeline           # frise chronologique du jour
cmon report rapport.html -p month   # rapport HTML interactif
```

Options du tracker : `--idle 300` (secondes avant « inactif »), `--poll 2`,
`--heartbeat 30`, `--procs 60`, `--no-titles` (ne pas enregistrer les titres),
`--db FICHIER`, `-v`.

### Statistiques

| Commande | Contenu |
|---|---|
| `summary` | temps actif / inactif / verrouillé, arrivée et départ, top applications et catégories |
| `timeline` | frise chronologique (`--detail` : chaque titre ; `--min 60` masque les blocs courts) |
| `apps` | classement des applications : temps, %, nombre de sessions, durée moyenne |
| `titles` | fenêtres et documents les plus utilisés (`--app "*chrome*"`) |
| `categories` | temps par catégorie (`--detail` : applications de chaque catégorie) |
| `hours` | répartition par heure de la journée |
| `weekdays` | moyenne par jour de la semaine |
| `days` | bilan quotidien : arrivée, départ, actif, inactif, verrouillé |
| `focus` | concentration : sessions, changements d'app par heure, plus longue période sans pause |
| `procs` | logiciels en cours d'exécution ; `--at "2026-09-29 14:30"` : ce qui tournait à cet instant |
| `report F.html` | rapport autonome : frise de chaque jour, vue détaillée zoomable, graphiques |
| `export F.csv` | tous les segments en CSV (tableur, Python…) |
| `sql "…"` | requête SQL libre |

Périodes (`-p`) : `today`, `yesterday`, `week`, `lastweek`, `month`, `lastmonth`,
`year`, `all`, `7d`/`30d`…, `2026-09-29`, `2026-09`, `2026` ; ou `--from AAAA-MM-JJ --to AAAA-MM-JJ`.

### Catégories

```sh
cmon rule defaults                         # web, développement, bureautique, jeux…
cmon rule add Jeux app "*minecraft*"       # motif sur l'exécutable
cmon rule add "Vidéo" title "*youtube*" 10 # motif sur le titre, priorité 10
cmon rule list / cmon rule del ID
```

### Requêtes libres

Les vues `v_activity` (une ligne par segment, avec `day`, `hour`, `weekday`,
`state`, `app`, `title`, `seconds`) et `v_processes` permettent n'importe quelle statistique :

```sh
cmon sql "SELECT day, SUM(seconds)/3600.0 AS heures FROM v_activity
          WHERE state='active' AND app='code.exe' GROUP BY day"
cmon sql --csv "SELECT hour, app, SUM(seconds) FROM v_activity GROUP BY 1,2" > heures.csv
```

## Données

Base SQLite : `%LOCALAPPDATA%\ComputerMonitor\cmon.db` (Windows) ou
`~/.local/share/computermonitor/cmon.db` (Linux), modifiable via `CMON_DB`.
Tables : `activity` (segments `start`/`end` en secondes Unix, `state` 0 actif,
1 inactif, 2 verrouillé), `apps`, `titles`, `process_runs`, `rules`. Les trous
dans la frise correspondent aux périodes où l'ordinateur était éteint ou en veille.

## Structure

```
src/common/   utilitaires, schéma SQLite
src/tracker/  tracker_main.c (logique), platform_win.c, platform_x11.c
src/cli/      cmon : statistiques, rapport HTML (report_template.html intégré à la compilation)
```
