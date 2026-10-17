# 3DSMonitor

Plugin Luma3DS (`.3gx`, basé sur CTRPluginFramework) pour enregistrer l'écran
haut d'une Nintendo 3DS **classique (Old 3DS)** sur la carte SD, plus des
scripts PC pour convertir les fichiers.

> État actuel : **Phase 3** – **L+R+Bas** démarre / arrête l'enregistrement
> vidéo de l'écran haut (400×240, 15 fps, RGB565) ; **L+R+Haut** prend une
> capture. `tools/convert.py` produit un `.mp4`, `tools/raw2png.py` un `.png`.
> (La Phase 2 « tout en RAM » a été sautée : 2 s de vidéo ne tiennent pas dans
> les ~4 Mo du plugin.)

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
| `rec_AAAAMMJJ_HHMMSS.raw` | Un enregistrement (heure de la console au démarrage) |
| `shot_AAAAMMJJ_HHMMSS.raw` | Une capture de l'écran haut |
| `3dsmonitor.log` | Journal : démarrage, chaque capture, et pour chaque enregistrement : frames écrites / perdues, débit SD, temps de conversion par frame |

Format `.raw` (défini dans `Includes/RecFormat.hpp`, lu par `tools/rawfile.py`) :
en-tête de 32 octets little-endian (`"3DSR"`, version, largeur, hauteur, format
de pixel, ordre des pixels, format d'origine du jeu, fps, nombre de frames,
frames perdues) suivi des frames en **RGB565**. Les pixels sont stockés dans
l'ordre natif du framebuffer 3DS (par colonnes, image tournée) : c'est l'ordre
le moins coûteux pour la console, les scripts PC remettent l'image droite.

## Scripts PC

Python 3 sur le PC (rien à installer sur la 3DS) :

```sh
python tools/convert.py rec_20261017_181500.raw      # -> rec_20261017_181500.mp4 (ffmpeg)
python tools/raw2png.py shot_20261017_181500.raw     # -> shot_20261017_181500.png
python tools/raw2png.py rec_20261017_181500.raw --frame 30   # une image d'un enregistrement
```

`convert.py` utilise `ffmpeg` du PATH, sinon celui du paquet Python
`imageio-ffmpeg` (`python -m pip install imageio-ffmpeg`). Si le jeu a été quitté
pendant un enregistrement, l'en-tête n'a pas pu être finalisé : les scripts
comptent alors les frames d'après la taille du fichier.

Sous Windows : mettre `convert.bat`, `convert.py`, `raw2png.py` et `rawfile.py`
dans un même dossier avec les `.raw` (ou un sous-dossier `rec`), puis
double-cliquer sur `convert.bat` (ou glisser des `.raw` dessus). Il installe
`imageio-ffmpeg` la première fois si ffmpeg est absent. Python doit être installé
(python.org, cocher « Add python.exe to PATH »).

## Structure

| Chemin | Rôle |
|---|---|
| `Sources/main.cpp` | Plugin : raccourcis, callback OSD, captures d'écran |
| `Sources/Recorder.cpp` | Enregistrement : buffer circulaire, thread d'écriture, statistiques |
| `Sources/Common.cpp` | Log, dossier, en-tête, lecture du framebuffer |
| `Includes/Capture.hpp` | Conversion framebuffer → RGB565 (sans dépendance 3DS, testée sur PC) |
| `Includes/RecFormat.hpp` | En-tête des fichiers `.raw` |
| `Includes/types.h` | Repris du template CTRPF (requis par ses headers) |
| `tools/` | Scripts PC (`rawfile.py`, `raw2png.py`, `convert.py`, `convert.bat`) |
| `tests/host/` | Tests PC de la conversion et des scripts |
| `3DSMonitor.plgInfo` | Métadonnées du `.3gx` (titre, version, `MemorySize: 5MiB`) |
| `3gx.ld`, `Makefile` | Repris du CTRPluginFramework-BlankTemplate |

## Fonctionnement (Phase 3)

- Le callback `OSD::Run` de CTRPF est appelé **dans le thread du jeu**, à chaque
  échange des buffers d'affichage. Pendant un enregistrement, toutes les 1/15 s,
  il convertit le framebuffer gauche de l'écran haut en RGB565 dans une case
  libre d'un **buffer circulaire** (jusqu'à 12 cases de 187,5 Ko, alloué une
  seule fois au démarrage). Si toutes les cases sont pleines, la frame est
  **perdue et comptée** : le jeu n'attend jamais.
- Un **thread d'écriture** vide le buffer dans un seul fichier, en écrivant d'un
  coup toutes les frames prêtes et contiguës (≥ 187,5 Ko par écriture).
- À l'arrêt (ou en cas d'erreur d'écriture / SD pleine / limite FAT32 de 4 Go),
  le reste est écrit, l'en-tête est mis à jour (frames, frames perdues), le
  fichier fermé et les statistiques ajoutées au log.
- Les frames perdues ne sont pas remplacées : la vidéo est alors plus courte
  (et donc accélérée) que la durée réelle. Le log permet de le voir.

## À tester sur la console — Phase 3

But : vérifier qu'un enregistrement fonctionne et **mesurer** ce que la console
tient (débit SD, frames perdues, coût par frame pour le jeu).

1. Copier le nouveau `.3gx` à la place de l'ancien (`SD:/luma/plugins/default.3gx`).
2. Lancer un **jeu 3DS**.
   - **Attendu** : **« 3DSMonitor ready: L+R+DOWN = REC, L+R+UP = screenshot »**.
3. En jeu : tenir **L + R** et appuyer sur **Bas** → **« REC started »** (fond rouge).
   Jouer **environ 20 secondes** en bougeant (scène qui change).
   - Noter si le jeu **ralentit** pendant l'enregistrement.
4. Tenir **L + R** et appuyer sur **Bas** → **« REC stopping... »** puis
   **« REC saved: N frames, M dropped »** (noter N et M).
5. Refaire un enregistrement d'environ 1 minute, puis quitter le jeu par HOME.
6. Sur le PC : copier `SD:/luma/plugins/<TitleID>/rec/` puis convertir avec
   `convert.bat` (double-clic) ou `python convert.py rec_….raw`.
   - **Attendu** : un `.mp4` lisible, image à l'endroit, bonnes couleurs.

**Renvoie-moi dans tous les cas :**
- le fichier `3dsmonitor.log` en entier (les lignes `rec …` donnent les mesures) ;
- les notifications « REC saved: … » ;
- si le jeu a ralenti, et à quel point (léger / très fort) ;
- si la vidéo est fausse : ce que tu vois (saccadée, accélérée, image cassée) ;
- en cas de crash : une photo de l'écran d'erreur.

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
- **Phase 1, 3e essai** (0.1.1) : captures OK, PNG correct (validé).
