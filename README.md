# 3DSMonitor

Plugin Luma3DS (`.3gx`, basé sur CTRPluginFramework) pour enregistrer l'écran
haut d'une Nintendo 3DS **classique (Old 3DS)** sur la carte SD, plus un script
PC pour convertir l'enregistrement en `.mp4`.

> État actuel : **Phase 0** – squelette + plugin « hello world ». Aucun
> enregistrement pour l'instant.

## Compilation

La source de vérité est la CI GitHub Actions (`.github/workflows/build.yml`) :
chaque push compile le plugin dans le conteneur `pablomk7/libctrpf:0.8.0`
(devkitARM + libctru + CTRPluginFramework 0.8.0 + 3gxtool, même image que le
template officiel CTRPF). Le `.3gx` est téléchargeable dans l'onglet
**Actions → run → Artifacts → `3DSMonitor-3gx`** (zip contenant `3DSMonitor.3gx`).

Compilation locale (Docker) :

```sh
docker run --rm -v "$PWD":/src -w /src pablomk7/libctrpf:0.8.0 make
```

Ou avec devkitPro installé et CTRPF dans `$DEVKITPRO/libctrpf` : `make`.

## Installation sur la console

Prérequis : Luma3DS récent (avec le Plugin Loader intégré à Rosalina).

1. Copier `3DSMonitor.3gx` sur la SD, au choix :
   - pour **un seul jeu** : `/luma/plugins/<TitleID>/3DSMonitor.3gx`
     (TitleID sur 16 caractères hexa majuscules, ex. `0004000000055D00`) ;
   - pour **tous les jeux** : `/luma/plugins/default.3gx` (renommer le fichier).
2. Ouvrir le menu Rosalina (**L + Bas + Select**) et mettre
   **Plugin Loader: [Enabled]**.
3. Lancer le jeu.

## Structure

| Chemin | Rôle |
|---|---|
| `Sources/`, `Includes/` | Code du plugin (`Includes/types.h` vient du template CTRPF, requis par ses headers) |
| `3DSMonitor.plgInfo` | Métadonnées du `.3gx` (titre, version, `MemorySize: 5MiB`) |
| `3gx.ld`, `Makefile` | Repris du CTRPluginFramework-BlankTemplate |

## À tester sur la console — Phase 0

But : vérifier que le plugin se charge et que la chaîne de build est bonne.

1. Installer le `.3gx` produit par la CI (voir ci-dessus), de préférence pour un
   seul jeu que tu connais bien.
2. Lancer le jeu.
   - **Attendu** : quelques secondes après le démarrage (délai de boot CTRPF,
     `WaitTimeToBoot`), une notification **« 3DSMonitor loaded »** apparaît
     en haut de l'écran.
   - Un **flash jaune** de l'écran haut suivi d'un plantage = échec de
     l'initialisation GSP dans CTRPF (`pluginInit.cpp`) : à me signaler.
3. En jeu, appuyer sur **Select** pour ouvrir le menu CTRPF (raccourci par
   défaut : `Preferences::MenuHotkeys = Key::Select` dans la source CTRPF).
   - **Attendu** : un menu « 3DSMonitor » avec une entrée **Hello**. La
     sélectionner (A) affiche une boîte « Hello from 3DSMonitor! ».
4. Fermer le menu, jouer une minute : aucun ralentissement ni crash attendu.

**Si ça échoue, renvoie-moi :**
- la version de Luma3DS (menu Rosalina, en haut) ;
- le chemin exact où tu as copié le `.3gx` et le TitleID du jeu ;
- ce que tu observes : rien du tout / écran figé / crash (copie l'écran d'erreur
  Luma s'il y en a un, ou le fichier `/luma/dumps/arm11/*.dmp` le plus récent) ;
- si la notification apparaît mais que le menu ne s'ouvre pas avec Select,
  dis-le (le jeu utilise peut-être Select, on changera le raccourci).
