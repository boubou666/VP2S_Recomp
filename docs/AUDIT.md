# Audit de faisabilité — port natif PC de Valkyrie Profile 2 : Silmeria

Version analysée : **SLES-54645** (PAL, France), ISO de 4,35 Go. Audit du 2026-10-20 (phases 1 et 2).

## Verdict

**GO pour une recompilation statique, sous conditions** (décision de fin de phase 2, voir section 8).

La phase 1 n'avait trouvé rien de bloquant. La phase 2 a levé les principales inconnues :
- la compression **SLZ** (4 types) est réimplémentée et validée ;
- le chiffrement **SLE** est cassé : la clé est récupérée et validée ;
- le format d'overlay **`MWo3`** (Metrowerks CodeWarrior) est décodé ;
- **≈ 3,3 Mo de code** (≈ 7 600 fonctions) sont extraits et se désassemblent proprement.

Les emplacements du « mode B » (combat ?) déclarés dans l'ELF sont **introuvables sur le disque**, quelle que soit la méthode statique employée. Ce sont probablement des vestiges de l'éditeur de liens, mais seule une capture mémoire pendant un combat peut le confirmer (section 7.6).

## 1. Structure du disque

| Zone (LBA) | Taille | Contenu |
|---|---|---|
| 0–820 | 1,6 Mo | Système de fichiers ISO9660 : `SLES_546.45` (exécutable), `IOPRP300.IMG` (modules IOP), `SYSTEM.CNF` |
| 1024–1041 | 36 Ko | Données à haute entropie : **probablement la table des fichiers** (chiffrée ou compressée) |
| 1042–254993 | ≈ 500 Mo | Zéros (remplissage pour placer les données vers l'extérieur du DVD) |
| 254994–2279407 | ≈ 4,1 Go | **Archive cachée**, invisible pour ISO9660, lue par secteurs bruts |

Contenu identifié dans l'archive :
- **Compression SLZ de tri-Ace** (`SLZ` + type 0 à 3 + taille compressée + taille décompressée + suivant). Le premier bloc est le module IOP `sio2man` compressé. Seuls 82 blocs SLZ démarrent sur une frontière de secteur ; les autres fichiers sont dans des **conteneurs** non alignés.
- Tables de blocs `ANIM` / `CHAR` (FourCC en little-endian) : animations et personnages.
- `SEQW` (1 825 occurrences) et `spcm` (100) : probablement les voix et la musique en streaming.
- Deux grandes zones à entropie de 8,0 (≈ 1,3 Go au début, et la fin du disque) : probablement les **vidéos FMV**, dans un conteneur non standard (aucun en-tête MPEG-PS trouvé).

## 2. Exécutable principal (`SLES_546.45`)

- ELF MIPS R5900 (Emotion Engine), point d'entrée `0x100008`, segment principal de 822 Ko (`0x100000`–`0x1e7d80`).
- **Aucun symbole** : table des symboles vide et noms de sections effacés.
- Ghidra 12.1.4 + plugin EE : **972 fonctions**, ≈ 67 000 instructions.
- Peu d'instructions spécifiques à la PS2 : **24 VU0 macro**, **110 MMI** (128 bits), 1 947 `lq`/`sq`, 1 253 FPU.
- Bibliothèques **Sony SDK 3.0** identifiées : libcdvd, libdma, libgraph (3.0.1), libkernl, libmc, libpad2, libdbc, sifrpc, stdio. Elles sont standard, donc remplaçables par une implémentation PC (HLE) au lieu d'être recompilées.

Fonctions clés repérées :

| Adresse | Rôle |
|---|---|
| `FUN_00119a60` (8 Ko, la plus grosse) | **Installateur sur disque dur** (corrigé en phase 2) : crée la partition `PP.SLES-54645` et y écrit `vp2.bin` via fileXio. Ce n'est pas la couche de lecture. |
| `FUN_00100230` | Démarrage de l'IOP (`cdrom0:\IOPRP300.IMG;1`) |
| `FUN_001025d0` | Point d'entrée SLZ : signature `SLZ`/`SLE`, déchiffrement, choix du type, chaînage des blocs |
| `FUN_00101ea0` / `FUN_001018d0` / `FUN_00101370` | Décompresseurs SLZ de type 1 / 2 / 3 |
| `FUN_00102360`, `FUN_001059b0` | Déchiffrement SLE, ouverture de flux SLZ |

Le jeu **sait lire toutes ses données depuis un seul fichier `vp2.bin`** (installation sur le disque dur PS2). L'archive cachée du DVD est donc, en pratique, ce même `vp2.bin`. C'est très favorable pour le port.

## 3. Overlays

365 segments de taille nulle dans le fichier, répartis sur 20 emplacements mémoire (≈ 7 Mo cumulés). Leur contenu est chargé depuis l'archive à l'exécution.

| Emplacement | Nombre | Taille max | Remarque |
|---|---|---|---|
| `0x1e7d80`, `0x35ec80`, `0x36d800` | 1 + 3 + 1 | 1,2–1,7 Mo | Probablement des tampons ou tas, pas du code |
| `0x48ea00`, `0x495000` | 10 + 12 | 26–105 Ko | Modules de jeu (scènes, systèmes) ? |
| `0x514100` | 21 | 60 Ko | idem |
| `0x523280` … `0x52e680` | **4 × 15** | 15 Ko | Emplacements parallèles identiques |
| `0x532280` | 57 | 33 Ko | |
| `0x53a400`, `0x53d880` | **2 × 45** | 13 Ko | Emplacements parallèles identiques |
| `0x540d00` | 12 | 7,5 Ko | |
| `0x542b00` … `0x547780` | **4 × 24** | 6,5 Ko | Emplacements parallèles identiques |

**Hypothèse à vérifier** : les groupes de 4 emplacements identiques correspondent aux 4 membres d'équipe en combat, et les groupes de 2 aux ennemis (modules d'action et d'IA chargés en parallèle).

Conséquence pour une recompilation statique : chaque overlay doit être extrait, décompressé, puis recompilé **pour son adresse de chargement**. Le runtime doit aussi savoir lequel est résident dans chaque emplacement pour dispatcher les appels.

## 4. Microcode VU

Dans l'exécutable principal :
- **un programme VU1 principal d'environ 1 150 instructions** (≈ 9 Ko), envoyé en 5 commandes VIF `MPG` (de `0x14a294` à `0x14c6a8`) vers les adresses VU `0x0dc`–`0x55a`. C'est probablement le moteur de rendu 3D ;
- une mini-routine de 11 instructions (`0x145d14`).

Cette taille reste **gérable** : on peut d'abord l'interpréter, puis le traduire à la main en shader ou en C. Les overlays peuvent contenir d'autres programmes VU (effets de combat, par exemple), à vérifier.

## 5. Points favorables et défavorables

**Favorables**
- SDK Sony standard (3.0), reconnaissable par signature et remplaçable en HLE.
- Données **compressées** (SLZ est le format du moteur tri-Ace partagé avec Star Ocean 3). *Phase 2 : seul le code est chiffré (SLE), et le chiffrement est cassé.*
- Couche d'accès aux fichiers centralisée (`vp2.bin`).
- Microcode VU1 de taille raisonnable dans l'exécutable.
- Peu de MMI et de VU0 dans l'exécutable principal.
- Ghidra et le plugin EE reconnaissent le binaire automatiquement.

**Défavorables**
- Aucun symbole.
- L'essentiel du code de jeu est dans ≈ 365 overlays **pas encore extraits** : la vraie taille du code reste inconnue.
- Table des fichiers (LBA 1024) non décodée, possiblement chiffrée.
- Formats propriétaires : conteneurs, `SEQW`, `spcm`, FMV.
- Modules IOP maison (pilote son…) à réimplémenter en HLE.
- Aucun projet communautaire existant (décompilation ou recomp) sur lequel s'appuyer.

## 6. Stratégie recommandée

1. **Recompilation statique de l'EE** : exécutable principal + chaque overlay à son adresse, traduits de MIPS R5900 vers C.
2. **HLE du SDK** : libcdvd, libpad2, libmc et libgraph réimplémentées pour PC ; la couche `FUN_00119a60` est redirigée vers `vp2.bin` extrait de l'ISO.
3. **Graphismes** : une couche d'émulation du GS d'abord (pour obtenir une image), puis un rendu natif.
4. **VU1** : interpréteur VU au départ, traduction ensuite.
5. **IOP / audio** : HLE des modules son à partir de leurs commandes RPC.

Premier jalon visé : **l'écran titre affiché nativement**.

Ordre de grandeur, sous réserve de la phase 2 : **6 à 18 mois et plus** de travail itératif à temps partiel, avec des tests à chaque étape.

## 7. Phase 2 — résultats

### 7.1 Compression SLZ

Réimplémentée dans `tools/slz.py` à partir des routines du jeu. Ce sont des variantes de LZSS : un octet ou un mot de drapeaux, des références avec une distance sur 12 bits et une longueur sur 4 bits.

| Type | Algorithme | Fin du flux |
|---|---|---|
| 0 | stocké | taille |
| 1 | LZSS sur octets | distance 0 |
| 2 | LZSS sur octets + RLE (longueur 0xF = remplissage) | taille décompressée |
| 3 | LZSS sur mots de 16 bits (longueur + 2 mots) | distance 0 |

Validation : les 82 blocs alignés sur un secteur se décompressent à la taille exacte de leur en-tête. Sur tout le disque, **25 558 fichiers** (1,6 Go compressés, **2,72 Gio décompressés**) passent, avec seulement 2 échecs.

### 7.2 Chiffrement SLE

`SLE` est un bloc SLZ dont la charge utile est chiffrée : `p = (c - k) XOR clé[i % 16]`, où `k = 3, 6, 9…` (modulo 256).

La clé (16 octets, lue par le jeu à `0x1d5930`, en BSS, posée à l'exécution) a été **récupérée par analyse de fréquences** sur les deux blocs SLE alignés. Elle a été validée par décompression à la taille exacte de ces deux blocs, puis des 125 autres. La clé est dans `tools/slz.py`.

**Seul le code est chiffré** : les 127 blocs SLE sont tous des modules ou des overlays.

La zone de la LBA 1024 (36 Ko) n'utilise pas ce chiffrement. Sa nature reste inconnue : ce n'est peut-être pas une table des fichiers.

### 7.3 Modules et overlays

En-tête commun (0x80 octets), avec l'adresse et les tailles aux mêmes positions :
`[0]` 0 ou `'MWo3'` · `[1]` identifiant · `[2]` adresse de chargement · `[3]` taille du code (en-tête compris) · `[4]` taille des données · `[5]` taille du BSS · `[6..7]` début et fin de la table des constructeurs C++ · `[0x20..]` **nom du fichier d'origine** (`MWo3` seulement).

**L'en-tête reste en mémoire à l'adresse de l'emplacement, et le code commence à `emplacement + 0x80`.** Ça a été vérifié par les cibles `jal` internes du module 3 : 2 261 sur 3 523 tombent sur un prologue avec cette base, contre 138 sans en-tête. `FUN_001210a0` lit ensuite l'en-tête en mémoire : il efface le BSS (`+0x14`) et appelle les constructeurs (`+0x18`..`+0x1c`).

`MWo3` correspond très probablement au format d'overlay de **Metrowerks CodeWarrior** : le jeu serait compilé avec CodeWarrior, pas avec GCC.

Les modules sont des **morceaux de chaînes SLZ** : `FUN_00102ac0` renvoie le n-ième bloc d'une chaîne. La **table des emplacements** est à `0x1c8a30` dans l'ELF : 21 adresses, indexées par numéro d'emplacement. Elle est utilisée par les méthodes virtuelles `FUN_00100e70` / `FUN_00100f60` (classe « chargeur », vtable vers `0x1a4a1c`) et par `FUN_00410cc0` (module 3, emplacements 3 et 4).

Les petits overlays de scène sont des entrées **`GRP`** de **conteneurs à répertoire**. Format du conteneur (lu par `FUN_00422c80`) : `+0x4` = nombre d'entrées, puis des entrées de 16 octets à partir de `+0x10` (étiquette, paramètre, taille, décalage). Étiquettes vues : `ANIM`, `CHAR`, `SIG`, `ATRC`, `GRP`, `FACE`, `SYS`, `DCMS`, `ECS`, `ESM`, `FISP`, `AAM`, `APAM`, `PAMM`, `MRTA`, `PAM`.

| N° | Emplacement | Déclarés (ELF) | Trouvés | Code | Contenu |
|---|---|---|---|---|---|
| 1 | `0x1e7d80` | 1 | **1** | 1 200 Ko | **module 1 : socle résident** (chargé avec le titre ; contient libmpeg/libipu, le décodeur vidéo) |
| 2 / 6 | `0x35ec80` | 3 | 2 | 1 254 Ko | module 2 (titre / carte mémoire) ; **module 3, moteur d'exploration** (portes, caméras, physique) |
| 3 | `0x48ea00` | 10 | **10** | 78 Ko | scènes : `SPWorldMap.bin`, `SPStaffRoll.bin`, `SPDragonHall.bin`… |
| 4 | `0x495000` | 12 | **12** | 550 Ko | camp : `CampEquip.ovl`, `CampParty.ovl`, `CampSaveLoad.ovl`… |
| 5, 7–20 | `0x4aea80`, `0x36d800`, `0x514100`…`0x547780` | 1 + 1 + 336 | **0** | — | « mode B », voir 7.6 |

Les tailles trouvées correspondent **exactement** aux segments déclarés dans l'ELF : taille décompressée + BSS = taille du segment. Les modules sont dupliqués sur le disque (131 occurrences pour 25 uniques), sans doute pour limiter les déplacements de la tête de lecture.

### 7.4 Mesure du code

| | Code | Fonctions | VU0 macro | MMI | Microcode VU1 |
|---|---|---|---|---|---|
| ELF principal | ≈ 270 Ko | 972 (Ghidra) | 24 | 110 | 1 programme, 1 161 instr. |
| Module 1 (socle résident) | 1 200 Ko | ≈ 2 000 (estimation) | **9 863** | 1 027 | 3 envois, 709 instr. |
| Module 3 (exploration) | 1 165 Ko | **3 214 (Ghidra)**, 0 erreur de décompilation | 4 433 | 192 | aucun |
| Module 2 + scènes + camp | ≈ 720 Ko | ≈ 1 300 (estimation) | ≈ 900 | ≈ 40 | aucun |
| **Total mesuré** | **≈ 3,3 Mo** | **≈ 7 600** | **≈ 15 200** | **≈ 1 400** | **≈ 1 870 instr.** |

Les estimations comptent les prologues `addiu sp,sp,-N` × 1,25 (calibré sur l'ELF : 780 prologues pour 972 fonctions Ghidra). Le module 3 contient aussi **1 782 appels indirects** (`jalr`) : le recompilateur devra dispatcher les pointeurs de fonction à l'exécution.

### 7.5 Modules IOP

| Sony (standard, remplaçables) | tri-Ace (à réimplémenter) |
|---|---|
| `sio2man`, `sio2d`, `Dbc_Manager`, `ds2o_d` (DualShock 2), `Sound_Device_Library` (libsd), `mcman`, `mcserv` | `Csm_`, `Csd_`, `Csi_` (≈ 50 Ko au total) : très probablement le **pilote son** |

### 7.6 Le « mode B » introuvable

La table des emplacements décrit deux configurations mémoire :
- **Mode A (exploration)** : le module 3 à `0x35ec80`, plus les scènes (3) et le camp (4). Tout est trouvé.
- **Mode B** : un module de `0xe580` à `0x35ec80` (6), puis un module de 1,7 Mo à `0x36d800` (7, juste après : `0x35ec80 + 0xeb80`), puis 13 petits emplacements (8 à 20), dont des groupes parallèles 4×15, 2×45 et 4×24. Plus l'emplacement 5 (`0x4aea80`, 0x100 octets). **Rien n'est trouvé.**

Méthodes employées, toutes négatives :
1. en-tête de module au premier niveau de chaque morceau de chaîne (25 558 chaînes) ;
2. code brut sans en-tête (cibles `jal` sur des prologues, à chaque emplacement candidat) ;
3. en-têtes et blocs SLZ imbriqués dans les données décompressées ;
4. en-têtes relâchés : adresse du mode B dans n'importe lequel des 8 premiers mots, sur les 25 648 en-têtes SLZ/SLE du disque ;
5. overlays de données (pointeurs internes vers leur propre plage) ;
6. en-têtes bruts dans les secteurs non compressés.

De plus, **aucun code extrait n'appelle ces emplacements** (zéro `jal`), et les 128 blocs `SLE` du disque se déchiffrent tous avec la clé connue. Le seul chargeur générique passe par une vtable, avec un numéro calculé à l'exécution.

**Hypothèse la plus probable** : ce sont des déclarations d'overlays restées dans le script de liens (développement, ou ancienne organisation du combat), et le combat est intégré au moteur 3D des modules 1 et 3. **Pour le confirmer** : prendre une sauvegarde d'état PCSX2 **pendant un combat** et lire `eeMemory.bin` aux adresses `0x35ec80`, `0x36d800` et `0x514100`+. On y verra quels modules sont résidents.

### 7.7 Vidéos

Le module 1 contient **libmpeg/libipu** de Sony (`sceMpegGetPicture`, décodage des macroblocs) : les FMV sont du **MPEG-2** dans un conteneur maison. Sur PC, une bibliothèque standard (FFmpeg) pourra les décoder une fois le conteneur compris.

### 7.8 Outillage

REA 6.7.0 **n'ouvre pas** ce binaire sous Windows : son connecteur Ghidra n'y accepte que les PE et le 6502, et IDA, Kuna et Hopper n'ont pas de voie MIPS. Le travail s'est fait en pilotant Ghidra en headless directement. REA pourrait servir sous Linux (WSL), ce qui reste à tester.

## 8. Décision go/no-go

**GO pour la recompilation statique**, pour ces raisons :
- volume de code **maîtrisable** (≈ 3,3 Mo mesurés, ≈ 7 600 fonctions) ;
- désassemblage et décompilation **propres** avec un compilateur unique (CodeWarrior) ;
- **toutes les couches de stockage du code sont cassées** (SLZ, SLE, MWo3) ;
- **peu de MMI** (≈ 1 400), **microcode VU1 de taille modeste** (≈ 1 870 instructions au total) ;
- SDK et modules IOP Sony standard, remplaçables en HLE.

Conditions et risques à traiter tôt :
1. **VU0 en mode macro** (≈ 15 200 instructions, surtout dans le module 1) : le recompilateur doit les traduire (en SSE). C'est un chantier central, pas un détail.
2. **Appels indirects** (≈ 1 800 dans le module 3, plus les vtables) : il faut une table de dispatch couvrant l'ELF et le module résident de chaque emplacement.
3. **Mode B introuvable** (section 7.6) : à confirmer par une capture mémoire pendant un combat.
4. **Pilote son maison** (`Csm_`/`Csd_`/`Csi_`) à réimplémenter à partir de ses commandes RPC.
5. **Rendu GS** et conteneur **FMV** inconnu.

## 9. Prochaines étapes

1. **Capture mémoire en combat** (PCSX2, sauvegarde d'état) pour trancher la question du mode B et vérifier quels modules sont résidents.
2. ~~**Extracteur complet**~~ — **fait** : `tools/extract.py` (voir `tools/README.md`). En 30 s, il sort les 25 modules prêts pour le recompilateur (tous conformes à leur segment ELF), les modules IOP, l'index des 25 558 chaînes et, en option, toutes les données. La disposition exacte de `vp2.bin` (installation sur disque dur) n'est pas reproduite : le port lira l'ISO directement.
3. **Choisir la base du recompilateur** (évaluer PS2Recomp), puis recompiler l'ELF principal et le module 2 (titre).
4. **HLE minimale** : noyau EE, libcdvd redirigée vers `vp2.bin`, libpad2, et un GS « logiciel » pour voir une image.
5. **Jalon : l'écran titre** affiché nativement.

## Annexe — environnement et reproduction

| Outil | Emplacement |
|---|---|
| Image Docker `vp2s-toolchain` (7-Zip, binutils MIPS, Python + pyelftools/capstone) | `toolchain/Dockerfile` |
| Ghidra 12.1.4 | `D:\Tools\ghidra_12.1.4_PUBLIC` (JDK 21 Temurin via `JAVA_HOME_OVERRIDE`) |
| Plugin Emotion Engine v2.1.38 | `%APPDATA%\ghidra\ghidra_12.1.4_PUBLIC\Extensions` |
| REA 6.7.0 (MCP) | enregistré dans `~/.claude.json` (sauvegarde : `.claude.json.rea.backup`) |
| Projet Ghidra | `ghidra/VP2S.gpr` |

Scripts (lancés avec `docker run --rm -v D:\VP2S_Recomp:/work vp2s-toolchain ...`) :
- `sh /work/scripts/extract_iso.sh` : liste et extrait le système de fichiers ISO dans `extracted/`
- `sh /work/scripts/elf_overview.sh`, `sh /work/scripts/elf_strings.sh` : en-têtes et chaînes de l'ELF
- `python3 -I /work/scripts/scan_blob.py` : carte d'entropie de l'archive cachée
- `python3 -I /work/scripts/extents.py` : zones de données et de zéros
- `python3 -I /work/scripts/count_slz.py` : blocs SLZ et FourCC
- `python3 -I /work/scripts/elf_layout.py` : emplacements d'overlays et microcode VU

- `python3 -I /work/scripts/test_slz.py` : décompresse tous les blocs alignés (validation SLZ)
- `python3 -I /work/scripts/recover_key.py` : récupère et valide la clé SLE
- `python3 -I /work/scripts/scan_chunks.py` : inventaire de tous les blocs SLZ/SLE → `work/chunks.tsv`, modules → `work/modules/`
- `python3 -I /work/scripts/scan_inner.py` : décompression complète, overlays `MWo3` → `work/overlays/`
- `python3 -I /work/scripts/scan_parts.py` : **référence** — tous les modules uniques, morceau par morceau → `work/parts/`
- `python3 -I /work/scripts/check_base.py <module>` : vérifie la base de chargement par les cibles `jal`
- `python3 -I /work/scripts/scan_grp.py` : conteneurs à répertoire et entrées `GRP`
- `scan_raw_overlays.py`, `scan_nested.py`, `peek_all.py`, `scan_data_overlays.py`, `sle_failures.py`, `calls_into_slots.py` : recherches du mode B (section 7.6)
- `python3 -I /work/scripts/module_stats.py` : statistiques de code par emplacement
- `python3 -I /work/scripts/list_irx.py` : modules IOP

Bibliothèque : `tools/slz.py` (SLZ types 0 à 3, déchiffrement SLE, chaînes de blocs, clé).

**Outil de référence : `tools/extract.py`** (voir `tools/README.md`). Il remplace en un seul passage les scripts d'exploration ci-dessus pour l'extraction. `scripts/check_extract.py` le recoupe avec `scan_parts.py` : 25 modules sur 25 identiques.

Scripts Ghidra (`ghidra_scripts/`, via `analyzeHeadless`) : `AuditStats.java`, `FindKeyFunctions.java`, `FindSlzCandidates.java`, `FindScalar.java`, `Decompile.java`.

> Ne jamais versionner ni redistribuer l'ISO, `extracted/` ou toute donnée issue du jeu.
