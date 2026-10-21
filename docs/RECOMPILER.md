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

Fork : sous-module `external/PS2Recomp`, branche unique `main` (partagée avec le portage de Drakengard), à partir du commit amont `eb8688b`. Documentation utilisateur : section « Overlay modules » du README du fork.

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

   - **Diagnostic** : `VP2S_DIAG=1` affiche les threads du jeu, l'ouvrier son et le côté IOP (`[vp2s-iop]` : modules SIF, clients et serveurs RPC, émulateur IOP, derniers appels RPC). Le débogueur ImGui du runtime est compilé (`VP2S_DEBUG_UI`, activé par défaut) et s'ouvre avec F1.
   - **Sémaphore à `max_count = 0`** : le constructeur statique de l'objet son (`0x151a20`) crée sa sémaphore avec `max_count = 0`. Le noyau EE ne valide pas ce champ, mais le runtime refusait la création : `WaitSema(-1)` revenait aussitôt et l'ouvrier son (priorité 42) tournait en boucle, ce qui affamait le thread principal (priorité 45). Le fork accepte désormais `max_count = 0` comme sémaphore sans borne, comme Play! (1 test).
   - **Chargement des IRX** : le jeu lit 10 modules sur le disque, les copie en mémoire IOP (`sceSifAllocIopHeap` puis DMA SIF) et appelle `sceSifLoadModuleBuffer` (enveloppe en `0x1400f8`, liée à la main dans `MANUAL_BINDINGS` de `tools/pipeline.py`, car la base de signatures ne la reconnaît pas). L'émulateur IOP du runtime ne lisait l'image qu'en mémoire EE. Le fork la lit maintenant en mémoire IOP (`loadModuleBufferFromIop`, 1 test).
   - Résultat au 2026-10-20 : **les 10 IRX sont chargés et émulés** (15 threads IOP, 6 serveurs RPC, environ 7 M d'instructions IOP). Le **pilote son répond** aux `sceSifCallRpc` (SID `0x1234`), et les serveurs `0x80001300` à `0x8000131f` sont liés. Le jeu entre ensuite dans sa boucle d'images (`0x100800`–`0x10085c` : VSync, mise à jour d'un objet d'état, envoi au GS).

   - **Pilote son et tas IOP** : l'init du pilote son (`Csi_`, RPC n° 1) alloue 764 Ko en mémoire IOP. Le tas de l'émulateur ne réutilisait jamais un bloc libéré et ne couvrait que 896 Ko, si bien que l'init échouait. `Csd_` n'installait alors pas ses gestionnaires de commandes SIF, et le transfert de la banque son (état de démarrage 6) attendait sans fin. Le fork alloue désormais comme `sysmem` (*first-fit* dans un pool unique au-dessus des modules) et transmet les commandes SIF de l'EE vers l'IOP (`sceSifSendCmd`, avec les arguments 5 et 6 dans `t0`/`t1` selon l'EABI). 1 test pour chaque point.
   - **Clé de déchiffrement des overlays** : les fichiers `SLE` sont déchiffrés avec une clé que le jeu calcule sur VU0 (microprogramme chargé depuis `0x145d00`), à partir de 4 mots renvoyés par le pilote son (commande n° 14 de `Csi_`, le complément de constantes de son `.data`). L'addition du FMAC PS2 perd les bits du petit opérande avant d'additionner. Une addition IEEE, même arrondie vers zéro, donnait (clé) au lieu de (clé), et l'overlay du titre sortait en charabia. Le fork modélise maintenant cette addition (PCSX2 code ces 4 cas en dur sous le nom « TriAce hack »). 1 test, qui exécute le microprogramme réel.
   - **Code que Ghidra ne voit pas** (`wrap_elf.py`, `find_entry_points.py`, recompilateur) :
     - initialiseurs statiques CodeWarrior placés en fin de `.data`, avant la table de constructeurs de l'en-tête (`+0x18`…`+0x1c`) : section exécutable `.init` ;
     - fonctions à cheval sur la fin de `.text` déclarée (modules 2, 16, 19 et 22) ;
     - fonctions atteintes par pointeur d'un fichier à l'autre (vtables de l'ELF principal vers le module cœur), par `jal` d'un overlay vers l'ELF principal ou vers un autre module, et leurs appels de proche en proche ;
     - fonctions alignées derrière du remplissage, après un `jr ra`, un `j` ou un `b`.
     Au total, environ 4 200 points d'entrée.
   - **Pipeline natif** : plus de Docker. Extraction et enveloppes ELF en Python, outils du fork et tests compilés avec MSVC (étapes `tool` et `test`).
   - **Recompilations incrémentales** : l'étape `recomp` ne remplace que les fichiers dont le contenu change. Le unity build est groupé par entrée et par tranche de 8 Ko d'adresses (`UNITY_BUILD_MODE GROUP`) au lieu de lots positionnels, qu'un seul fichier ajouté décalait tous. Une régénération touchant quelques fonctions se recompile en 2 à 5 minutes au lieu de 13.

   État au 2026-10-21 : la clé est juste, et les overlays **cœur (module 1) et titre (module 2) sont chargés, résidents, constructeurs exécutés**. Le jeu tourne dans leur code sans plantage pendant au moins une minute.

   - **L'image s'affiche** : l'écran « noir » venait de mes captures (fenêtre réduite, ou capture raylib prise avant le dessin). Le jeu affichait déjà, en français, « Impossible de détecter la manette analogique (DUALSHOCK®2) ».
   - **Manette** : VP2 lit la manette avec les vrais pilotes IOP (`sio2man`, `sio2d`, `ds2o_d`, `Dbc_Manager`), qui parlent au matériel SIO2. Le fork émule maintenant le contrôleur SIO2 et une DualShock 2 sur le port 0 (modes numérique, analogique et pression, avec les commandes de configuration), alimentée par la manette ou le clavier du PC via `IopHost::readPadState`. Il gère aussi le DMA des canaux 11 et 12 dans `dmacman`, l'appel `sceSifSetDmaIntr` (`sifman` n° 32, dont le rappel libère la sémaphore DMA de `Dbc_Manager`), et le DMA SIF depuis n'importe quelle zone de RAM IOP. Enfin, les VBlanks de l'EE recalent l'horloge de l'IOP, sinon les threads cadencés par VBlank ne tournaient presque jamais. Suite de tests `ps2_iop_sio2_tests`.
   - Résultat au 2026-10-21 : manette détectée, écran « Choisir format écran : □ 16:9 / △ 4:3 », et les entrées clavier sont prises en compte. Avec △, le jeu passe à la suite.

   - **FFmpeg activé** (`VP2S_FFMPEG`, par défaut à ON) : le fork télécharge FFmpeg 7.1 LGPL en DLL (`System233/ffmpeg-msvc-prebuilt`, 35 Mo, sous `build/port/ThirdParty`) et copie les DLL à côté de `vp2s.exe`. Les accesseurs `libmpeg` voisins de ceux reconnus par signature (`sceMpegIsEnd`, `sceMpegIsRefBuffEmpty`, `sceMpegSetDecodeMode`) sont aussi reliés au runtime (`MANUAL_BINDINGS`).

   - **Vidéo d'introduction** : le lecteur du jeu (`FUN_002b94a0`, module cœur) attend 512 Ko de son de la vidéo (`objet+0xc999c`, environ 2,7 s) avant de demander sa première image. L'émulation `libmpeg` du runtime arrêtait de consommer le flux à 8 images d'avance, si bien que le son plafonnait vers 290 Ko et que la lecture CD en flux attendait de la place dans son anneau. La vraie `libmpeg` met en mémoire la vidéo compressée et peut donc démultiplexer des secondes à l'avance. Le fork accepte maintenant 128 images d'avance (environ 150 Mo en 640×448).
   - Résultat au 2026-10-21 : **la vidéo d'introduction se joue**, en 640×448 au format cinéma, avec le logo SQUARE ENIX, le logo tri-Ace puis la cinématique d'ouverture sous-titrée en français.

   - **Vitesse** : la vidéo est cadencée sur les VSync, et le runtime n'émet une VSync que lorsque le temps réel **et** les cycles EE comptés sont écoulés. Or plusieurs travaux de l'hôte prenaient du temps réel sans compter de cycles : rasterisation GS logicielle, décodage FFmpeg, ordonnanceur IOP appelé tous les 32 cycles. Le profileur `VP2S_PROFILE` (voir `tools/README.md`) a guidé ces corrections dans le fork :
     - lectures de timer en attente active : un tick de cycles est crédité ;
     - temps de décodage FFmpeg crédité en cycles EE, puisqu'il remplace l'IPU qui tourne en parallèle de l'EE ;
     - IOP exécuté par lots de 256 cycles, au lieu d'un passage à chaque point de contrôle : c'était un tiers du temps du thread EE ;
     - filtrage bilinéaire du GS en entier (les appels CRT d'arrondi dominaient), avec un raccourci quand le texel est aligné ;
     - cache de texture du GS sur 8 pages au lieu d'une : il recopiait 8 Ko par texel dès qu'une primitive chevauchait deux pages ;
     - **50 Hz en PAL** : le jeu appelle `sceGsResetGraph(0, 1, 3, 1)`, soit le mode PAL. Le runtime cadençait tout en NTSC 59,94 Hz. Les VBlank, l'IOP, l'horloge des vidéos et le débit du flux CD suivent maintenant `EeScheduler::fieldRate()`.
   - Résultat au 2026-10-21 : vidéo à environ 24 images/s pour 25 (environ 48 VSync/s pour 50), soit environ 96 % de la vitesse réelle, contre environ 11 images/s auparavant. La cinématique dure plus de 4 minutes. **Start** (Entrée) la passe.

   - **Rendu GS plus rapide** (branche `vp2s/gs-fast`, fusionnée dans `vp2s/overlay-dispatch` au commit 4b3fb3e). La scène qui suit la vidéo dessine environ 160 triangles texturés par VSync via VU1, soit environ 5,5 millions de pixels (une vingtaine d'écrans de couches translucides). Le rasteriseur logiciel la dessinait à environ 1,3 VSync/s :
     - l'état du GS est décodé une fois par primitive, et non plus à chaque pixel ; les triangles ne parcourent que la partie utile de chaque ligne ;
     - dessin asynchrone (`GSCpuBackend::SetAsyncRasterization`, activé par le portage ; `VP2S_GS_SYNC=1` pour comparer) : les primitives vont dans une file que 8 threads dessinent dans l'ordre, chacun sur ses bandes de 4 lignes entrelacées, si bien que chaque pixel garde son ordre de dessin ;
     - la file ne se vide que pour les lectures de VRAM hors dessin (transferts vers l'hôte, copies locales, présentation) : le rendu vers texture passe par une barrière dans la file, les envois d'image sont mis en file et écrits par bandes, `TEXFLUSH` est un marqueur, et les chargements de palette vont dans des copies référencées par les primitives ;
     - chaque thread garde les textures déjà converties en RGBA (palette et TEXA appliquées, tuiles de 32×32 décodées à la demande), pour les primitives de plus de 1 024 pixels, rafraîchies au `TEXFLUSH` comme le cache de pages ;
     - le VU1 ne parcourt plus ses pipelines aux cycles où rien n'est dû ;
     - un test vérifie que le dessin asynchrone laisse exactement la même VRAM que le dessin immédiat (mélange, profondeur, bilinéaire, envoi d'image sur une texture lue, rendu vers texture).
   - **Fin de vidéo** : `sceMpegIsEnd` attendait la fin du flux CD du runtime, que VP2 ne produit jamais (il lit la vidéo avec ses propres `sceCdRead`) : le lecteur restait sur un écran noir après la dernière image. Sans flux du runtime, la fin du flux MPEG suffit maintenant.
   - **Son** (commit `e0bc8f0` du fork) : SPU2 émulé côté IOP (deux cœurs, 48 voix ADPCM avec enveloppes ADSR, AutoDMA, réverbération, DMA 4/7) ; les gestionnaires VBlank IOP sont délivrés (le pilote son de VP2 réveille ainsi son thread de flux) ; les rappels `sceMpegStrDATA` reçoivent le son de la vidéo. La sortie passe par un étirement temporel WSOLA (hauteur conservée) qui absorbe les écarts de vitesse ; `PS2X_SPU2_WAV=<fichier>` enregistre la sortie.
   - Résultat au 2026-10-21 : menu et vidéo à 50 VSync/s (cible PAL), avec le son. La vidéo va jusqu'au bout (5 547 images) ou se passe avec **Start**, puis le jeu enchaîne sur une scène animée (fond vert, plume, formes bleues) qui tourne à environ 10 à 13 VSync/s. Après environ 70 s de temps de jeu dans cette scène, l'écran « Press Start » n'est pas encore apparu.
   - **Géométrie VU1 de l'écran titre** : cette « scène animée » était en fait l'écran titre, dont toute la géométrie VU1 (ciel, logo, menu) était fausse. Une trace des microprogrammes VU1 (données VIF en entrée, instructions, paquets XGKICK) a remonté la chaîne :
     - le programme 0x6e0 appelle son chargeur par `IADDIU vi1, vi0, 1301` puis `JALR vi15, vi1`. L'interpréteur appliquait à `JR`/`JALR` la règle des branches conditionnelles, qui lisent l'ancienne valeur d'un VI écrit par l'instruction précédente. Il sautait donc à l'ancienne valeur de vi1, et le programme partait dans une boucle sans fin. PCSX2 (microVU) n'applique ce retard qu'aux branches conditionnelles. `JR`/`JALR` lisent maintenant la nouvelle valeur ;
     - le runtime coupait chaque microprogramme à 65 536 cycles. Le programme égaré était donc abandonné en plein milieu, avec la mémoire VU à moitié écrite (adresses des tampons de sortie, constantes de la caméra), et le programme suivant (0x880) écrivait ses sommets par-dessus les matrices. Sur la console, `MSCAL`/`MSCNT` attend la fin du programme : le plafond passe à 2^24 cycles, avec un avertissement si un programme l'atteint ;
     - un GIFtag `FLG=3` (« disable ») se comporte comme IMAGE sur le GS. XGKICK l'arrêtait comme une instruction réservée, et le GS l'ignorait sans avancer dans le paquet : les deux le traitent maintenant comme IMAGE.
   - Résultat au 2026-10-21 : **l'écran titre s'affiche correctement** (ciel, logo de la valkyrie, NOUVELLE PARTIE / CHARGER PARTIE / OPTIONS, copyright), à environ 23 à 30 VSync/s sur 50.
   - **NOUVELLE PARTIE** lance la cinématique d'ouverture (générique sur les paysages), à environ 50 VSync/s, puis le jeu charge les modules 3 et 4 (`field=mod003 scene=mod004`) : l'écran reste alors noir, c'est le prochain blocage.
   - **Son de la cinématique d'ouverture** : son flux audio est compressé (en-tête de 32 octets, puis données à forte entropie), contrairement au PCM brut de la vidéo d'intro. Les paquets DATA portent leur canal dans le 4e octet du sous-en-tête (`ff 90 00 01`), alors que le runtime le prenait dans le quartet bas du 2e : tout partait sur le canal 0 (chemin PCM) et le SPU2 jouait les octets compressés, d'où les salves de bruit blanc. Le jeu les reçoit maintenant sur son callback du canal 1, qui alimente le décodeur de `Csd_.irx` sur l'IOP.
   - **Musique** (écran titre et cinématique) : `Csd_` décode sur l'IOP des coefficients que l'EE confie au VU1. Une tâche DMA VIF1 enchaîne trois microprogrammes : déquantification (0x6e0), fenêtrage (0x1410), IMDCT avec conversion en entiers (0x1690). L'EE relit ensuite le résultat en 0x1100E000–0x1100FFFF (`0x105b00`, `PPACH` vers 16 bits) et renvoie les trames PCM de 0x1040 octets à l'IOP, qui les envoie au SPU2. Le runtime ne faisait passer par le chemin lent que 0x11004000–0x1100BFFF. La mémoire de données VU1 était donc lue dans la RAM (0x1100E000 → RAM 0x0100E000), et la musique restait muette. Toute la fenêtre 0x11000000–0x1100FFFF passe maintenant par le chemin lent, et la musique sort. Il reste des coupures périodiques de 50 à 230 ms (trames qui n'arrivent pas à temps), à traiter.
   - **Runtime commun avec Drakengard** (2026-10-22) : le fork suit maintenant le runtime commun (bc208c4, branche `main`), qui fusionne le travail générique de la session Drakengard. Interpréteur VU 3 à 4 fois plus rapide, identique au cycle près sur 3 000 exécutions VU1 enregistrées sur l'écran titre de VP2 (`PS2X_VU1_RECORD`, outil `ps2_vu1_replay`). Corrections VIF0, SPR et FPU, thread GS (`PS2X_GS_SYNC=1` pour le désactiver), contrôle de VRAM par un second GS (`PS2X_GS_SHADOW_CHECK=1`). L'écran titre tourne en temps réel, mais n'affiche qu'environ 13 images distinctes par seconde : la présentation attend le thread GS, ce sera corrigé avec l'unification des deux rendus GS. `VP2S_RECOMP` (pipeline) compile le code recompilé d'un autre fork dans son propre dossier.
   - Au passage : l'interpréteur IOP ne fusionnait pas `LWL`/`LWR` avec un chargement en attente du même registre (lectures non alignées fausses). Corrigé, avec un test.

   **Points ouverts** :
   - **Vitesse de l'écran titre** (environ 23 à 30 VSync/s sur 50) : il faudrait un recompilateur VU1 et un rendu GS sur GPU (ou vectorisé) pour atteindre la pleine vitesse ; le son saccade tant que le jeu tourne nettement sous le temps réel.
   - Les captures (`vp2s_diag_N.png`) ne font que 640×256 dans cette scène : à vérifier si elles montrent bien tout l'écran.

   Clavier (sans manette branchée), touches repérées par leur **position sur un clavier QWERTY** (sur AZERTY, □ est donc la touche W) : flèches = croix directionnelle, WASD = stick gauche, Z = □, X = ✕, C = ○, V = △, Q/E = L1/R1, 1/3 = L2/R2, Ctrl gauche/droit = L3/R3, Entrée = Start, Maj droite = Select.
5. **Jalon : l'écran titre.** Atteint le 2026-10-21 (rendu correct, pas encore à pleine vitesse).
6. Ensuite : module 3 (exploration), rendu GS natif, VU1 en shader, FMV via FFmpeg.

## Reproduire les tests

Suites de tests du fork (`ps2x_tests`, recompilateur compris, et les 4 suites IOP), compilées avec MSVC :

```bash
python tools/pipeline.py --only test
```

Sous Linux (Docker, facultatif), la compilation demande `-march=x86-64-v2` sous g++ ; le montage `/deps` est facultatif et réutilise des sources de dépendances déjà téléchargées :

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
