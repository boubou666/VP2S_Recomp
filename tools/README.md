# Outils

Outils d'atelier, lancés une fois sur la machine de développement. Ils ne font pas partie du port : le port sera du code natif issu de la recompilation statique du MIPS du jeu. `extract.py` fournit au recompilateur le code des modules, qui est chiffré et compressé sur le disque.

## `pipeline.py` — de l'ISO au C++ recompilé

Orchestrateur, à lancer sous Windows depuis la racine du projet (Python 3.10 ou plus, sans dépendance). Tout tourne en natif, sans Docker :

```bash
python tools/pipeline.py
```

| Étape | Rôle | Sortie (`build/`) |
|---|---|---|
| `extract` | `extract.py` | `extract/` |
| `wrap` | copie de l'ELF principal, `wrap_elf.py` pour chaque module ; les initialiseurs statiques (fin de `.data`, avant la table de constructeurs de l'en-tête) deviennent une section exécutable `.init` | `elf/main.elf`, `elf/modNNN.elf` |
| `ghidra` | analyse Ghidra headless (processeur `r5900` imposé) et export PS2Recomp, en une session | `ghidra/export/<nom>.toml`, `<nom>_functions.csv` |
| `tool` | compile `ps2_recomp` et `ps2_analyzer` depuis le fork (`external/PS2Recomp`) avec MSVC | `tests/` |
| `sdk` | `ps2_analyzer` reconnaît les fonctions du SDK Sony par signature, pour les relier aux implémentations C++ du runtime | `sdk/<nom>.toml` |
| `entries` | `find_entry_points.py` : fonctions que Ghidra ne voit pas (atteintes par pointeur, y compris depuis un autre fichier comme une vtable de l'ELF principal vers le module cœur ; constructeurs des modules ; fonctions de l'ELF principal appelées seulement depuis un overlay) ; elles deviennent des `entry_points` du recompilateur | `entries/<nom>.txt` |
| `recomp` | `ps2_recomp` sur chaque entrée, avec la carte Ghidra et les liaisons SDK fusionnées (à adresse égale, le nom SDK l'emporte) ; les modules en mode module (`module_name = "modNNN"`). La sortie passe par `recomp-staging/` et seuls les fichiers modifiés sont recopiés, pour que MSVC ne recompile que ce qui a changé | `recomp/<nom>/`, `recomp/<nom>.log` |
| `report` | chiffres par entrée et totaux | `report.md`, `report.json` |
| `port` | compile `port/` (runtime du fork + code généré) avec MSVC (VS 2022 Build Tools, via `vswhere` ou `VP2S_VS_DIR`) | `port/vp2s.exe` |
| `test` | compile et lance les suites de tests du fork avec MSVC (`ps2x_tests` et les 4 suites IOP) | `tests/<suite>.log` |

Lancer le jeu : `build\port\vp2s.exe build\extract\iso\SLES_546.45`. L'image ISO est cherchée via `VP2S_ISO`, sinon en remontant depuis le dossier de l'ELF. `VP2S_DIAG=1` affiche toutes les 5 secondes le PC, les modules résidents par emplacement, les threads du jeu (état, attente, PC), l'état de l'ouvrier son et, avec le préfixe `[vp2s-iop]`, le côté IOP : modules chargés via SIF, clients et serveurs RPC, état de l'émulateur IOP et de ses threads, derniers appels RPC ; avec le préfixe `[vp2s-gs]`, le côté GS : registres d'affichage, tampon présenté, pixels non noirs par tampon, activité GS récente. Il enregistre aussi une capture de l'écran du jeu (`vp2s_diag_N.png`), prise directement dans l'image produite par le GS du runtime, même fenêtre réduite. **F9** enregistre une capture à tout moment (`vp2s_shot_N.png`). `VP2S_DUMP=<adresse>:<taille>` écrit cette zone de mémoire dans `vp2s_dump.bin`, et `VP2S_WATCH=<adresse>,<adresse>…` affiche ces mots mémoire à chaque passage. Le décodage des vidéos passe par FFmpeg (`-DVP2S_FFMPEG=OFF` pour l'enlever) : à la première compilation, le fork télécharge FFmpeg 7.1 LGPL en DLL (35 Mo) dans `build/port/ThirdParty`. `VP2S_PROFILE=<secondes>` échantillonne les threads du jeu environ 1 000 fois par seconde et affiche à cet intervalle (préfixe `[vp2s-prof]`) les fonctions où le thread le plus occupé passe son temps, en exclusif et en inclusif. Les noms viennent des symboles publics de `vp2s.pdb`, produit à l'édition de liens sans recompilation. Les fonctions statiques sont attribuées au symbole public qui les précède, d'où quelques noms trompeurs (`std::invalid_argument`…). L'échantillonnage ralentit le jeu : ne pas mesurer la vitesse en même temps. Les autres threads occupés plus de 30 % du temps (les threads de dessin du GS) sont aussi listés, avec leurs fonctions principales. Le portage active le dessin GS asynchrone du fork (`GSCpuBackend::SetAsyncRasterization`) : les primitives sont dessinées par des threads dédiés, ce qui rend les lectures directes de la VRAM (diagnostic des pixels non noirs) approximatives pendant un dessin. `VP2S_GS_SYNC=1` revient au dessin immédiat sur le thread du jeu, pour comparer. Le débogueur du runtime (ImGui) s'ouvre avec **F1** ; il est compilé par défaut (`-DVP2S_DEBUG_UI=OFF` pour l'enlever).

Les fonctions du SDK que la base de signatures ne reconnaît pas sont liées à la main dans `MANUAL_BINDINGS` (`tools/pipeline.py`), par exemple l'enveloppe `sceSifLoadModuleBuffer` en `0x1400f8`. L'étape `recomp` régénère une entrée dès que sa configuration change.

Chaque étape saute le travail dont les sorties sont plus récentes que les entrées. Options : `--only <étape>`, `--from <étape>`, `--force`, `--inputs main,mod003`.

Prérequis : Visual Studio 2022 Build Tools, Ghidra (`GHIDRA_INSTALL_DIR`), et le **zip** de l'extension Emotion Engine pour cette version de Ghidra (`VP2S_GHIDRA_EE_ZIP`, par défaut dans `D:\Tools\downloads`). Le dossier facultatif `build/deps` contient les sources des dépendances CMake (`<nom>-src`), pour compiler sans réseau.

> Pourquoi le zip de l'extension : lancé via l'alias `python` de Windows (Store ou gestionnaire d'installation Python), Python et ses processus enfants voient un `%APPDATA%` redirigé. Ghidra n'y trouve pas les extensions installées et se rabat **silencieusement** sur un langage MIPS64 générique. L'orchestrateur donne donc à Ghidra son propre dossier de réglages (`XDG_CONFIG_HOME` → `build/ghidra/settings`), y installe l'extension, et refuse de continuer si un autre langage que `r5900` apparaît dans le journal.

Pour compiler un autre état du fork (un worktree git, par exemple) sans toucher aux dossiers par défaut, `pipeline.py` lit `VP2S_FORK` (dossier du fork), `VP2S_TEST_BUILD` et `VP2S_PORT_BUILD` (noms des dossiers de compilation sous `build/`) :

```bash
VP2S_FORK=D:/VP2S_Recomp/build/wt/gs-fast VP2S_PORT_BUILD=port-gs python tools/pipeline.py --only port
```

Une archive FFmpeg déjà téléchargée par une autre compilation du portage est réutilisée. L'étape `test` ne compile que les suites définies par le fork choisi.

## `extract.py` — extracteur du disque

Python 3.10 ou plus, sans dépendance (pas de 7-Zip, pas de paquets tiers).

```bash
python -I tools/extract.py "Valkyrie Profile 2 - Silmeria (France).iso" out
```

Ajouter `--assets` pour décompresser aussi toutes les données (≈ 2,7 Gio, environ 1 minute). `--jobs N` limite le nombre de processus.

Contenu de `out/` :

| Chemin | Contenu |
|---|---|
| `iso/` | fichiers ISO9660 : `SLES_546.45`, `IOPRP300.IMG`, `SYSTEM.CNF` |
| `modules/` | les 25 modules de code, déchiffrés et décompressés, **tels qu'en mémoire** : l'en-tête de 0x80 octets est à l'adresse de chargement, le code à `+0x80` |
| `modules.json` | pour chaque module : identifiant, nom d'origine, adresse, emplacements, tailles (code, données, BSS), constructeurs, empreinte, occurrences sur le disque, et cohérence avec le segment déclaré dans l'ELF |
| `iop/` | les 10 modules IOP (`sio2man`, `mcman`, `Csm_`/`Csd_`/`Csi_`…) |
| `chunks.tsv` | index des 25 558 chaînes SLZ/SLE : décalage, LBA, nombre de morceaux, nature de chaque morceau |
| `assets/` | (`--assets`) chaque morceau décompressé, `<décalage>_<morceau>.<nature>` |
| `manifest.json` | résumé, erreurs, et emplacements trouvés / déclarés |

Résultat attendu sur SLES-54645 : 25 642 en-têtes, 25 558 chaînes, 0 erreur, 25 modules (tous conformes à leur segment ELF), 10 modules IOP.

## `wrap_elf.py` — module → ELF

Enveloppe un module extrait dans un ELF R5900 minimal, chargé à son adresse, avec l'en-tête du module en mémoire (section `.modhdr`, qui sert de garde au mode module de PS2Recomp), puis `.text`, `.data` et `.bss` d'après l'en-tête.

## `slz.py` — compression SLZ / chiffrement SLE

Bibliothèque utilisée par l'extracteur : SLZ types 0 à 3, déchiffrement SLE (la clé n'est pas fournie : variable `VP2S_SLE_KEY` ou fichier `build/sle_key.txt`, 32 chiffres hexadécimaux ; voir `load_sle_key`), lecture des chaînes de blocs. Les formats sont décrits dans `docs/AUDIT.md`, sections 7.1 à 7.3.

> `out/`, `work/` et `extracted/` contiennent des données du jeu : ne jamais les versionner ni les redistribuer.
