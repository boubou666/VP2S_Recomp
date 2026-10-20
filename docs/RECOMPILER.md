# Choix de la base du recompilateur

Décision du 2026-10-20. Contexte : `docs/AUDIT.md` (GO pour une recompilation statique).

## Recommandation

**Partir de PS2Recomp, dans un fork, en lui ajoutant la gestion des overlays par emplacement.**

PS2Recomp recompile déjà, sans erreur, l'ELF principal et le plus gros overlay du jeu. Son runtime couvre le noyau, VU0 (macro et microprogrammes), VU1, le GS et l'exécution des modules IOP d'origine. La seule brique manquante bloquante pour nous, les overlays, se greffe proprement sur son mécanisme d'appel dynamique. Écrire notre propre recompilateur reviendrait à refaire ce runtime, soit des dizaines de milliers de lignes, sans bénéfice à ce stade.

## Nos contraintes

| Contrainte | Origine (audit) |
|---|---|
| Binaire sans symboles, compilé avec **Metrowerks CodeWarrior** | ELF strippé ; format d'overlay `MWo3` |
| **25 modules de code**, dont plusieurs à la même adresse | emplacement 2/6 : modules 2 et 3 ; emplacement 3 : 10 scènes ; emplacement 4 : 12 menus |
| Environ 3,3 Mo de code, environ 7 600 fonctions | section 7.4 |
| **≈ 15 200 instructions VU0 macro**, **195 appels de microprogrammes VU0** (`VCALLMS`) | surtout dans le module 1 |
| ≈ 1 870 instructions de microcode VU1 | ELF + module 1 |
| ≈ 1 800 appels indirects (`jalr`) dans le module 3, plus les vtables C++ | section 7.4 |
| Pilote son maison sur l'IOP (`Csm_`, `Csd_`, `Csi_`) | section 7.5 |
| Vidéos MPEG-2 via libmpeg/libipu | section 7.7 |

## PS2Recomp : évaluation

Dépôt : [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp), GPL-3.0, actif (dernier commit le 2026-10-14), environ 3 600 étoiles. Il est utilisé par une vingtaine de portages : *God of War*, *Budokai Tenkaichi 3*, *Shadow of the Colossus*, *Def Jam Vendetta*, *Burnout Dominator*…

Composants :
- `ps2xRecomp` : traduction MIPS R5900 vers C++, instruction par instruction. Il prend en entrée une carte de fonctions exportée de Ghidra (`ExportPS2Functions.java`).
- `ps2xRuntime` : mémoire, table de répartition des fonctions, appels système, GS (rasteriseur logiciel), interpréteurs VU0/VU1.
- `ps2xIOP` : interpréteur R3000 qui **exécute les IRX d'origine**, avec un noyau IOP virtuel et des services HLE (libsd, mcserv, dbcman).

### Tests sur notre jeu

Faits le 2026-10-20 : recompilateur compilé sous Linux (g++ 13, dans l'image `vp2s-toolchain`), cartes de fonctions exportées depuis Ghidra 12.1.4.

| Entrée | Fonctions (Ghidra) | Durée | Sortie | Erreurs | Avertissements |
|---|---|---|---|---|---|
| ELF `SLES_546.45` | 972 + 5 592 étiquettes | 14 s | 6 567 fichiers, 772 000 lignes de C++ | **0** | 245 (sauts indirects) |
| Module 3 (enveloppé en ELF par `tools/wrap_elf.py`) | 2 559 + 17 124 étiquettes | 43 s | 19 686 fichiers, 3,24 M lignes | **0** | 3 428 (sauts indirects) |

L'export Ghidra a aussi **identifié automatiquement 55 appels système** (`CreateThread`, `WaitSema`, `FlushCache`…) et les a reliés aux implémentations du runtime.

### Adéquation à nos contraintes

| Contrainte | Verdict | Détail |
|---|---|---|
| CodeWarrior, sans symboles | ✅ | La traduction se fait instruction par instruction, indépendamment du compilateur. Les tables de saut sont détectées, ou peuvent être déclarées. Testé : 0 erreur. |
| **Overlays** | ❌ **à ajouter** | Le runtime n'a qu'**une table de fonctions indexée par adresse**, et le code généré définit une table globale à base fixe. Deux modules à la même adresse sont impossibles en l'état. Voir « Gestion des overlays ». |
| VU0 macro | ✅ | Traduit par `vu_translator.cpp`. Quelques registres de contrôle `CFC2`/`CTC2` restent non gérés : à vérifier pour ceux qu'utilise le jeu. |
| Microprogrammes VU0 (`VCALLMS`) | ✅ | `VCALLMS` est traduit en appel à l'interpréteur VU0 du runtime (testé dans `ps2xTest`). |
| VU1 | ⚠️ | Interprété, donc lent (le README le reconnaît). À terme : réécrire nos ≈ 1 870 instructions à la main, en shader ou en C++. |
| GS | ⚠️ | Rasteriseur **logiciel**, lent. BT3 et *Shadow of the Colossus* ont écrit leur propre rendu OpenGL natif : c'est le chantier graphique principal. |
| Appels indirects, vtables | ✅ | `dispatchGuestBranch` et `lookupFunction` résolvent la cible à l'exécution. Les points de réentrée manquants s'ajoutent à la main (comme pour BT3). |
| Pilote son IOP maison | ✅ probable | Les IRX d'origine s'exécutent tels quels. À vérifier : les imports et l'accès au SPU2 de `Csd_`. |
| MPEG-2 (FMV) | ⚠️ | libipu passe par le matériel IPU : il faudra un décodage hôte (FFmpeg), à brancher. |

### Ce que montrent les autres portages

- **Budokai Tenkaichi 3** ([z3xox/BT3-Recomp](https://github.com/z3xox/BT3-Recomp)) : le code de gameplay est **un overlay brut** chargé à adresse fixe. Méthode : enveloppe ELF, recompilation séparée, table de fonctions renommée (`g_ps2OverlayFunctionTable`), points de réentrée ajoutés à la main. Mais il n'y a **qu'un seul overlay**, toujours au même endroit, et leur fichier de correctifs spécifiques au jeu dépasse 7 600 lignes.
- **God of War** ([KIexster/god-of-war-recomp](https://github.com/KIexster/god-of-war-recomp), [Waffle565/God-Of-War-Native-Port](https://github.com/Waffle565/God-Of-War-Native-Port)) : ELF unique, sans overlay de code (ses « overlays » sont du microcode VU).
- Aucun portage PS2Recomp connu ne gère **plusieurs overlays à la même adresse**. Côté PS1, [psxrecomp](https://github.com/RetroPortingToolKit/psxrecomp) compile les overlays à l'avance et choisit le code natif par vérification des octets, ce qui est le principe retenu ci-dessous.

## Gestion des overlays — implémentée

Fork : sous-module `external/PS2Recomp`, branche `vp2s/overlay-dispatch`, à partir du commit amont `eb8688b`. Documentation utilisateur : section « Overlay modules » du README du fork.

| Partie | Fichiers |
|---|---|
| Recompilateur : `module_name`, `module_load`, `module_guard_size` dans `[general]`. Noms, en-têtes et fichiers préfixés par le module ; table privée ; octets de garde lus à l'adresse de chargement. | `config_manager.cpp`, `types.h`, `code_generator.*`, `ps2_recompiler.*`, `function_emitter.cpp`, `function_table_emitter.*` |
| Runtime : registre `ps2_overlays` (enregistrement, résolveur par garde ou personnalisé, remplacement ciblé). `lookupFunction`, `hasFunction` et `replaceFunction` consultent le module résident quand l'entrée de la table ELF est vide. | `ps2_overlay_modules.h/.cpp`, `ps2_runtime.cpp`, `ps2_memory.h` |
| Tests | `ps2xTest/src/ps2_overlay_module_tests.cpp` (6 tests) |
| Enveloppe ELF : section `.modhdr` pour l'en-tête du module, qui sert de garde | `tools/wrap_elf.py` |

Validation (2026-10-20) :
- suite de tests PS2Recomp sous Linux (g++ 13) : **451 sur 451**. Les 445 tests d'origine passent toujours (référence mesurée sur le commit amont non modifié : 445 sur 445), plus nos 6 tests ;
- modules 2 et 3, **tous deux à `0x35ec80`**, recompilés en mode module : la garde de chacun est son en-tête (identifiant 2 contre 3) ;
- compilation C++ de tout le module 2 et des fonctions du module 3 dans la même plage d'adresses, avec les deux fichiers d'enregistrement : **2 544 fichiers sur 2 544, 2 542 fonctions définies, 0 symbole en double** (`scripts/check_module_link.py`).

Ce qui n'est pas encore vérifié : l'exécution réelle, qui demande un runtime complet lié au jeu (étape 4 du plan).

### Conception retenue

Ce qui rend l'ajout simple chez nous :
- **l'en-tête de chaque module reste en mémoire à l'adresse de son emplacement** (audit, section 7.3). L'identifiant du module résident se lit donc en mémoire (`emplacement + 4`), sans avoir à intercepter le chargeur ;
- tout appel qui sort d'un module passe déjà par `dispatchGuestBranch` / `lookupFunction`.

Conception :
1. **Recompilation par module** : `tools/wrap_elf.py` produit un ELF par module (fait), puis Ghidra exporte une carte par module.
2. **Préfixe par module dans le recompilateur** : petite modification de `ps2xRecomp`. Chaque sortie a son espace de noms et sa propre table (`g_mod003_FunctionTable`…), au lieu de la table globale. Cela évite le renommage par post-traitement de BT3.
3. **Répartition consciente des emplacements dans le runtime** : `lookupFunction(adresse)`
   - si l'adresse est dans l'ELF ou dans le module 1 (résident) : table directe ;
   - si elle tombe dans un emplacement d'overlay : lire l'identifiant à `emplacement + 4`, puis consulter la table de ce module. Si l'identifiant est inconnu ou incohérent, erreur explicite avec trace ;
   - en option, vérifier une empreinte des premiers octets (garde à la psxrecomp) en mode debug.
4. **`replaceFunction` et les correctifs par jeu** : étendus aux tables de module.

Écart avec ce plan : le recompilateur prend la garde à l'adresse de chargement (les 64 premiers octets, soit l'en-tête du module chez nous) au lieu de lire un identifiant à un décalage fixe. C'est plus générique, et un jeu peut toujours installer un résolveur plus rapide (`ps2_overlays::setResidentResolver`).

## Alternatives écartées

| Option | Pour | Contre | Verdict |
|---|---|---|---|
| **Recompilateur maison** (à la N64Recomp) | Maîtrise totale, overlays natifs, code plus rapide (pas de point de reprise à chaque instruction) | Il faut refaire tout le runtime (noyau, VU0/VU1, GS, IOP : des dizaines de milliers de lignes) | ❌ au départ. À reconsidérer seulement si les performances de la traduction littérale posent problème. |
| **Décompilation « matching »** (splat + CodeWarrior PS2 + decomp.me) | Vrai code source C, portage propre à long terme | ≈ 7 600 fonctions à réécrire à la main pour qu'elles recompilent à l'identique, soit des années. Je n'ai pas vérifié d'outillage `m2c`/decomp.me mûr pour MIPS + CodeWarrior. | ❌ comme base. ✅ plus tard comme **complément** : remplacer des fonctions recompilées par du C écrit à la main, fonction par fonction. |
| **Émulateur amélioré** (PCSX2 + correctifs) | Fonctionne déjà | Ce n'est pas un portage natif | ❌ hors objectif |
| **ps2-web-recomp** (WebAssembly) | Cible le navigateur | Pas notre cible, moins mature | ❌ |

## Points d'attention

- **Licence** : PS2Recomp est en **GPL-3.0**. Un fork et le portage qui en dérive devront être distribués sous GPL-3.0, et jamais avec des données du jeu.
- **Volume de code généré** : environ 4 M de lignes pour l'ELF et le module 3, soit **≈ 10 M de lignes** pour tout le jeu. La première compilation complète prendra du temps : sortie en plusieurs fichiers, compilation parallèle, et pourquoi pas une bibliothèque par module.
- **Performances** : la traduction est littérale (un point de reprise par instruction) et le VU et le GS sont interprétés. Le premier jalon sera lent, ce qui est normal.
- **Compilateur hôte** : le projet est testé surtout avec MSVC. Le recompilateur compile aussi avec g++ 13 (vérifié). Le runtime (raylib, SDL2) reste à compiler.

## Plan proposé

1. ~~**Fork de PS2Recomp**~~ — fait (sous-module `external/PS2Recomp`).
2. ~~**Overlays**~~ — fait (voir « Gestion des overlays — implémentée »).
3. ~~**Pipeline de build**~~ — fait pour la partie recompilation : `tools/pipeline.py` (voir `tools/README.md`). Résultat du 2026-10-20 : **26 entrées sur 26** (ELF + 25 modules), **7 191 fonctions**, **8,69 M lignes de C++** (55 492 fichiers, 338 Mo), **0 erreur**, 6 961 avertissements (sauts indirects). Environ 6 minutes de bout en bout ; une relance sans changement saute tout. Reste à faire : **compiler le runtime et le code généré** et les lier en un exécutable.
4. **Premier démarrage** — en cours. Fait au 2026-10-20 :
   - **`build/port/vp2s.exe` (168 Mo) compilé avec MSVC 14.44** à partir du runtime du fork et des 26 entrées recompilées (`port/CMakeLists.txt`, étape `port` de `tools/pipeline.py`). Unity build par lots de 64 et en-tête précompilé : environ 13 minutes en compilation complète, 0 erreur et 0 avertissement du compilateur ;
   - **le jeu démarre** : runtime et fenêtre initialisés, correctif de jeu appliqué (`port/vp2s_overrides.cpp` : image ISO, disposition mémoire), puis le code du jeu s'exécute (démarrage C, constructeurs, initialisation de l'IOP) ;
   - **disposition mémoire** : VP2 utilise presque toute la RAM (tas de `0x549100` à `0x1FF0000`). Le fork permet désormais de déplacer la zone réservée du runtime (`PS2Runtime::configureReservedMemory`, 2 tests) ; VP2 la place dans la mémoire noyau `0x80000`–`0x100000`. Sans ça, `malloc` échouait et le jeu effaçait sa propre mémoire.

   - **Fonctions du SDK reliées au runtime** (étape `sdk`) : `ps2xAnalyzer` reconnaît par signature **177 fonctions** (160 dans l'ELF : CD, SIF/RPC, IOP, carte mémoire, DMA, GS, libc ; 17 `libmpeg`/`libipu` dans le module 1). Le fork synthétise désormais l'enveloppe d'une liaison même quand Ghidra n'a pas de fonction à cette adresse (13 cas dans l'ELF ; 1 test).
   - **Fonctions atteintes uniquement par pointeur** (étape `entries`) : 26 points d'entrée ajoutés (threads, rappels), dont `0x11d460`.
   - **Rappel CD** : le fork mémorise `sceCdCallback` et le déclenche comme une interruption à la fin de chaque `sceCdRead` (1 test). Sans ça, le thread lecteur du jeu attendait indéfiniment.
   - Le jeu **lit et déchiffre sa table des fichiers** (LBA 1024, voir audit 7.2) et démarre ses threads, dont l'ouvrier RPC du son (`FUN_001086a0`).

   **Blocage actuel** : les appels `sceSifCallRpc` vers l'IOP (client `0x1d5c44`, probablement le pilote son maison `Csm_`/`Csd_`/`Csi_`) ne reçoivent pas de réponse ; aucun module n'est encore chargé (écran noir). Prochaine étape : diagnostiquer le côté IOP (chargement des IRX, serveurs RPC), avec le panneau de debug du runtime (`PS2X_ENABLE_DEBUG_UI=ON`).
5. **Jalon : l'écran titre.**
6. Ensuite : module 3 (exploration), rendu GS natif, VU1 en shader, FMV via FFmpeg.

## Reproduire les tests

Suite de tests du fork (recompilateur `ps2_recomp` compris). La compilation demande `-march=x86-64-v2` sous g++ ; le montage `/deps` est facultatif et réutilise des sources de dépendances déjà téléchargées :

```bash
docker run --rm -v D:\VP2S_Recomp\external\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/build_ps2recomp_tests.sh linux-tests
```

Recompilation d'un module en mode module (4ᵉ argument), avec le recompilateur du fork :

```bash
docker run --rm -e RECOMP=/src/out/linux-tests/ps2xRecomp/ps2_recomp -v D:\VP2S_Recomp\external\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/run_recomp.sh /work/work/recomp/mod003.toml /work/work/recomp/mod003.elf /work/work/recomp/module_mod003 mod003
```

Compilation croisée de deux modules à la même adresse :

```bash
docker run --rm -v D:\VP2S_Recomp\external\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain python3 -I /work/scripts/check_module_link.py /work/work/recomp/module_mod002 /work/work/recomp/module_mod003
```

Évaluation initiale, sur le clone amont non modifié :

```bash
docker run --rm -v D:\Tools\src\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/build_ps2recomp.sh
```

```bash
docker run --rm -v D:\VP2S_Recomp:/work vp2s-toolchain python3 -I /work/tools/wrap_elf.py /work/out/modules/0035ec80_003.bin /work/work/recomp/mod003.elf --flags-from /work/out/iso/SLES_546.45
```

Puis Ghidra headless avec `-postScript ExportPS2Functions.java <toml> <csv>` (script de PS2Recomp), et :

```bash
docker run --rm -v D:\Tools\src\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/run_recomp.sh /work/work/recomp/mod003.toml /work/work/recomp/mod003.elf /work/work/recomp/output_mod003
```
