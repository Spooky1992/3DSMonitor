# 3DSMonitor

Plugin Luma3DS (`.3gx`, basé sur CTRPluginFramework) pour enregistrer l'écran
haut d'une Nintendo 3DS **classique (Old 3DS)** sur la carte SD, plus des
scripts PC pour convertir les fichiers.

> État actuel : **Phase 1** – **L+R+Bas** enregistre une capture de l'écran
> haut (fichier `.raw`) ; `tools/raw2png.py` la convertit en PNG. Pas encore de
> vidéo.

**Limite importante** : un plugin `.3gx` ne fonctionne que dans les **jeux
3DS**. Les jeux DS/DSi (cartouche ou DSiWare) et GBA (Virtual Console) tournent
dans un mode où Luma ne s'exécute pas : impossible de les enregistrer ainsi.

## Compilation

La source de vérité est la CI GitHub Actions (`.github/workflows/build.yml`) :
- job `build` : compile le plugin dans le conteneur `pablomk7/libctrpf:0.8.0`
  (devkitARM + libctru + CTRPluginFramework 0.8.0 + 3gxtool, même image que le
  template officiel CTRPF). Le `.3gx` est téléchargeable dans **Actions → run →
  Artifacts → `3DSMonitor-3gx`** (zip contenant `3DSMonitor.3gx`) ;
- job `host-tests` : compile et exécute sur PC les tests de la conversion du
  framebuffer et des scripts PC (`tests/host/run.sh`).

Compilation locale (Docker) :

```sh
docker run --rm -v "$PWD":/src -w /src pablomk7/libctrpf:0.8.0 make
tests/host/run.sh   # tests PC (g++ + python3)
```

## Installation sur la console

Prérequis : Luma3DS avec le Plugin Loader (testé : 13.3.2).

1. Copier le `.3gx` sur la SD, au choix :
   - pour **tous les jeux** : `SD:/luma/plugins/default.3gx` (renommer le fichier) ;
   - pour **un seul jeu** : `SD:/luma/plugins/<TitleID>/3DSMonitor.3gx`
     (TitleID sur 16 caractères hexa majuscules).
2. Menu Rosalina (**L + Bas + Select**) → **Plugin Loader: [Enabled]**.
3. Lancer un **jeu 3DS** depuis le menu HOME.

Le plugin crée `SD:/luma/plugins/<TitleID>/rec/` pour ses fichiers. Ce dossier
ne contient pas de `.3gx` : Luma continue donc de charger `default.3gx`.

## Fichiers produits

Dans `SD:/luma/plugins/<TitleID>/rec/` :

| Fichier | Contenu |
|---|---|
| `shot_AAAAMMJJ_HHMMSS.raw` | Une capture de l'écran haut (heure de la console) |
| `3dsmonitor.log` | Journal : démarrage, chaque capture (format du jeu, temps de conversion et d'écriture, erreurs) |

Format `.raw` (défini dans `Includes/RecFormat.hpp`, lu par `tools/rawfile.py`) :
en-tête de 32 octets little-endian (`"3DSR"`, version, largeur, hauteur, format
de pixel, ordre des pixels, format d'origine du jeu, fps, nombre de frames,
frames perdues) suivi des frames en **RGB565**. Les pixels sont stockés dans
l'ordre natif du framebuffer 3DS (par colonnes, image tournée) : c'est l'ordre
le moins coûteux pour la console, les scripts PC remettent l'image droite.

## Scripts PC

Python 3, bibliothèque standard uniquement (rien à installer) :

```sh
python tools/raw2png.py shot_20261017_181500.raw     # -> shot_20261017_181500.png
```

## Structure

| Chemin | Rôle |
|---|---|
| `Sources/main.cpp` | Plugin : raccourci, callback de capture, écriture SD, log |
| `Includes/Capture.hpp` | Conversion framebuffer → RGB565 (sans dépendance 3DS, testée sur PC) |
| `Includes/RecFormat.hpp` | En-tête des fichiers `.raw` |
| `Includes/types.h` | Repris du template CTRPF (requis par ses headers) |
| `tools/` | Scripts PC (`rawfile.py`, `raw2png.py`) |
| `tests/host/` | Tests PC de la conversion et des scripts |
| `3DSMonitor.plgInfo` | Métadonnées du `.3gx` (titre, version, `MemorySize: 5MiB`) |
| `3gx.ld`, `Makefile` | Repris du CTRPluginFramework-BlankTemplate |

## Fonctionnement (Phase 1)

- Le callback `OSD::Run` de CTRPF est appelé **dans le thread du jeu**, à chaque
  échange des buffers d'affichage. Quand une capture est demandée, il convertit
  le framebuffer gauche de l'écran haut en RGB565 dans un buffer de 192 Ko
  alloué une seule fois au démarrage, puis rend la main.
- Le thread du plugin (boucle principale CTRPF) lit les touches, écrit le fichier
  sur la SD et le log : le jeu n'attend jamais la carte SD.

## À tester sur la console — Phase 1

But : vérifier que la capture lit correctement l'écran du jeu et l'écrit sur la SD.

1. Copier le nouveau `.3gx` à la place de l'ancien (`SD:/luma/plugins/default.3gx`).
2. Lancer un **jeu 3DS**.
   - **Attendu** : flash au lancement (Luma), puis la notification
     **« 3DSMonitor ready: L+R+DOWN = screenshot »**.
3. En jeu, sur un écran bien reconnaissable, appuyer **en même temps** sur
   **L + R + Bas (croix directionnelle)** (tenir L et R, puis appuyer sur Bas).
   - **Attendu** : notification **« Saved shot_….raw »** au bout d'une fraction
     de seconde. Le jeu ne doit pas se figer.
4. Faire 2 ou 3 captures sur des écrans différents (dont une avec la 3D activée
   si possible), puis éteindre la console.
5. Sur le PC : récupérer `SD:/luma/plugins/<TitleID>/rec/` (un nouveau dossier
   au nom hexa dans `plugins`) et lancer
   `python tools/raw2png.py <fichier>.raw` sur chaque capture.
   - **Attendu** : un PNG 400×240 identique à l'écran du jeu (bon sens, bonnes
     couleurs). Avec la 3D, image de l'œil gauche.

**Si ça échoue, renvoie-moi :**
- le fichier `3dsmonitor.log` (en entier) ;
- la notification affichée (texte exact, surtout si elle est rouge) ;
- si l'image est fausse : le PNG obtenu + la ligne affichée par `raw2png.py`
  (elle donne le format d'origine du jeu) et une photo de l'écran réel ;
- le nom du jeu ;
- en cas de crash : le dernier fichier de `SD:/luma/dumps/arm11/`.

## Historique des tests

- **Phase 0** (validée sur console, Luma 13.3.2, Old 3DS) : le plugin se charge
  dans un jeu 3DS, le menu s'ouvre avec Select.
- **Phase 1, 1er essai** (Pokémon Rubis Oméga, `00040000011C4000`) : crash
  « data abort » dans `free()` après L+R+START. Analyse de l'écran de crash :
  le jeu se terminait (L+R+START déclenche un reset logiciel dans certains
  jeux), puis `delete menu` en fin de `main()` plantait dans les destructeurs
  du menu CTRPF 0.8.0 (objet déjà libéré). Corrigé : combo **L+R+Bas** et plus
  de destruction du menu à la sortie.
- **Phase 1, 2e essai** (même jeu) : « Screenshot failed ». Log :
  `fmt=33/65 stride=720 addr=98300000`. Le jeu est en BGR8 (`fmt & 7 = 1`) ; CTRPF
  fournit l'adresse **physique** de la VRAM avec le bit 31 (`PA_FROM_VA`), que
  `Process::CheckAddress()` refuse. Corrigé en 0.1.1 : ces adresses sont acceptées
  si elles tombent en VRAM ou en FCRAM.
