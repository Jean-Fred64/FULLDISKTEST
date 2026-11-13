# FULLDSKT — Utilitaire de test d'intégrité magnétique pour disquettes (MS-DOS)

`FULLDSKT.EXE` est un utilitaire natif 16 bits pour **MS-DOS (versions 3.30 à 6.22)** inspiré du principe de *H2testw*. Il permet de vérifier l'intégrité physique et magnétique complète des disquettes 5,25" et 3,5" en saturant intégralement l'espace libre du volume FAT12, en purgeant les caches matériels et logiciels, puis en relisant et comparant chaque octet avec conversion directe des erreurs en coordonnées physiques **CHS (Cylindre / Tête / Secteur)**.

---

## Caractéristiques principales

* **Compatibilité processeur et OS maximale** : Compilé en instructions strictes **Intel 8086/8088** (`-0`), modèle mémoire *Small* (`-ms`), compatible avec les PC XT, AT, 286, 386, 486 et Pentium sous **MS-DOS 3.30 à 6.22** (ainsi que FreeDOS et DOSBox-X).
* **Détection automatique multi-formats (160 Ko à 2,88 Mo)** : Lecture dynamique du *BIOS Parameter Block* (BPB) via `INT 13h AH=02h` et `INT 21h AX=440Dh` sans aucun paramètre manuel de capacité :
  * **5,25"** : 160 Ko, 180 Ko, 320 Ko, **360 Ko (DS/DD)**, **1,2 Mo (DS/HD)**
  * **3,5"** : **720 Ko (DS/DD)**, **1,44 Mo (DS/HD)**, 2,88 Mo (DS/ED)
* **Saturation totale jusqu'à 0 octet libre** :
  1. Écriture séquentielle par blocs de **32 Ko** (`B00.H2W`, `B01.H2W`…).
  2. Saturation des derniers clusters libres par blocs de **512 octets** (`S00.H2W`, `S01.H2W`…) tout en respectant la limite des **112 entrées** du répertoire racine des disquettes 360 Ko et 720 Ko.
* **Purge des caches avant relecture** : Vidage des tampons internes MS-DOS (`INT 21h AH=0Dh`), vidage du cache SmartDrive (`INT 2Fh AX=4A10h BX=0001h`) et réinitialisation matérielle du contrôleur disquette (`INT 13h AH=00h`).
* **Diagnostic physique CHS précis** : En cas d'erreur de lecture ou de divergence binaire entre l'octet écrit et l'octet relu, résolution de la chaîne de clusters FAT12 et affichage exact du secteur logique (**LBA**) et de sa position physique (**Cylindre, Tête, Secteur**).
* **Gestion des erreurs critiques (`INT 24h`)** : Détournement du gestionnaire d'erreurs critiques DOS (`_HARDERR_FAIL`) afin d'éviter le blocage sur l'invite interactive *"Abandon, Reprise, Échec ?"* lors de la rencontre d'un secteur physiquement illisible ou d'une disquette protégée en écriture.
* **Sécurité anti-écrasement** : Interdiction stricte de cibler un disque dur (`C:` ou supérieur) et interdiction d'exécuter `FULLDSKT.EXE` depuis le lecteur en cours de test.

---

## Utilisation en ligne de commande

Copiez `FULLDSKT.EXE` sur votre disque dur (`C:\DOS\` ou `C:\UTILS\`) ou sur une disquette système distincte du lecteur à tester.

```dos
FULLDSKT [lecteur:] [/Y] [/K] [/V] [/?]
```

### Paramètres et commutateurs

| Option | Description |
| :--- | :--- |
| `A:` ou `B:` | Lecteur de disquette cible (par défaut : `A:`). |
| `/Y` | Mode automatique : supprime automatiquement les fichiers `.H2W` en fin de test sans demander de confirmation clavier. |
| `/K` | *Keep* : conserve les fichiers de test (`Bxx.H2W` et `Sxx.H2W`) sur la disquette à l'issue de l'écriture et de la vérification (utile pour tester la rétention magnétique à froid ou sur un second lecteur). |
| `/V` | *Verify only* : saute la phase d'écriture et vérifie directement l'intégrité des fichiers `.H2W` déjà présents sur la disquette (créés précédemment avec `/K`). |
| `/?` | Affiche l'aide et les options disponibles. |

### Exemples d'utilisation

* **Test complet standard sur le lecteur A: (avec confirmation avant nettoyage) :**
  ```dos
  FULLDSKT A:
  ```
* **Test en série de plusieurs disquettes 5,25" 360 Ko sur B: sans pause finale :**
  ```dos
  FULLDSKT B: /Y
  ```
* **Test de rétention magnétique en deux temps (écriture puis relecture le lendemain sans réécrire) :**
  ```dos
  FULLDSKT A: /Y /K
  REM --- Plus tard ou sur un autre lecteur ---
  FULLDSKT A: /Y /V
  ```

---

## Architecture du projet

```text
FULLDISKTEST/
├── .gemini/
│   └── instructions.md     # Spécifications techniques 16 bits et contraintes C89
├── build/
│   └── FULLDSKT.EXE        # Exécutable MS-DOS 16 bits compilé
├── script/
│   └── FULLDSKT.BAT        # Prototype batch d'origine
├── src/
│   └── fulldskt.c          # Code source C89 complet (mode réel 16 bits)
├── test/
│   ├── template_360.img    # Image disquette FAT12 vierge 5,25" 360 Ko
│   ├── template_720.img    # Image disquette FAT12 vierge 3,5" 720 Ko
│   ├── template_120.img    # Image disquette FAT12 vierge 5,25" 1,2 Mo
│   ├── template_144.img    # Image disquette FAT12 vierge 3,5" 1,44 Mo
│   └── run_test.bat        # Script de validation automatisée sous DOSBox-X
├── build.bat               # Script de compilation Open Watcom v2
└── README.md               # Documentation du projet
```

---

## Compilation (Open Watcom v2)

Le programme est écrit en **C89 strict** afin de garantir une compatibilité totale avec les compilateurs 16 bits historiques et modernes. Le tampon principal de 32 Ko (`static unsigned char buffer[32768]`) est alloué statiquement dans le segment de données (`DGROUP`) pour préserver la pile MS-DOS limitée (2 à 4 Ko).

### Prérequis
* **Open Watcom C/C++ v2** (compilateur 16 bits `wcl`).

### Commande de compilation
Depuis le répertoire racine du projet (ou via `build.bat`) :

```bat
wcl -0 -ms -os -wx -zq "-fe=build\FULLDSKT.EXE" src\fulldskt.c
```

* `-0` : Génération de code stricte 8086/8088.
* `-ms` : Modèle mémoire *Small* (segment de code unique 64 Ko, segment de données unique 64 Ko).
* `-os` : Optimisation de la taille du binaire.
* `-wx` : Activation de tous les avertissements du compilateur.

---

## Validation et banc de test (DOSBox-X)

L'ensemble des formats FAT12 a été validé en boucle fermée sous **DOSBox-X** :

| Format disquette | Géométrie (C / H / S) | Fichiers générés | Capacité saturée | Statut |
| :--- | :---: | :---: | :---: | :---: |
| **5,25" 360 Ko (DS/DD)** | 40 / 2 / 9 | 11 × 32 Ko + 2 × 512 o | 361 472 octets (`0` libre) | Validé |
| **3,5" 720 Ko (DS/DD)** | 80 / 2 / 9 | 22 × 32 Ko + 9 × 512 o | 725 504 octets (`0` libre) | Validé |
| **5,25" 1,2 Mo (DS/HD)** | 80 / 2 / 15 | 37 × 32 Ko + 3 × 512 o | 1 213 952 octets (`0` libre) | Validé |
| **3,5" 1,44 Mo (DS/HD)** | 80 / 2 / 18 | 44 × 32 Ko + 31 × 512 o | 1 457 664 octets (`0` libre) | Validé |
| **MS-DOS 3.30 (`ver set 3.30`)** | 40 / 2 / 9 | 11 × 32 Ko + 2 × 512 o | 361 472 octets (`0` libre) | Validé |
| **Injection corruption (LBA 100)** | 40 / 2 / 9 | Altération `0x30 -> 0xCF` | Détection `Cyl 5, Tête 1, Sect 2` | Validé |