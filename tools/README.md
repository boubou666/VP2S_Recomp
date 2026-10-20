# Outils

Outils d'atelier, lancés une fois sur la machine de développement. Ils ne font pas partie du port : le port sera du code natif issu de la recompilation statique du MIPS du jeu. `extract.py` fournit au recompilateur le code des modules, qui est chiffré et compressé sur le disque.

## `extract.py` — extracteur du disque

Python 3.10 ou plus, sans dépendance (pas de 7-Zip, pas de paquets tiers).

```bash
docker run --rm -v D:\VP2S_Recomp:/work vp2s-toolchain python3 -I /work/tools/extract.py "/work/Valkyrie Profile 2 - Silmeria (France).iso" /work/out
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

## `slz.py` — compression SLZ / chiffrement SLE

Bibliothèque utilisée par l'extracteur : SLZ types 0 à 3, déchiffrement SLE (la clé est incluse), lecture des chaînes de blocs. Les formats sont décrits dans `docs/AUDIT.md`, sections 7.1 à 7.3.

> `out/`, `work/` et `extracted/` contiennent des données du jeu : ne jamais les versionner ni les redistribuer.
