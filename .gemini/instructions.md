# Contraintes techniques du projet FULLDISKTEST (MS-DOS 3.30 - 6.22)

Tu développes un utilitaire natif MS-DOS 16 bits nommé `FULLDSKT.EXE` destiné à tester l'intégrité physique des disquettes (160 Ko à 2,88 Mo, en priorité 5,25" 360 Ko / 1,2 Mo et 3,5" 720 Ko / 1,44 Mo).

## Règles de compilation (Open Watcom v2)
- Compilateur : `wcl` (Open Watcom C 16 bits).
- Options obligatoires : `-0` (instructions strictes 8086/8088), `-ms` (modèle mémoire Small), `-os` (optimisation taille), `-wx` (alertes max).
- Commande de build : `wcl -0 -ms -os -zq "-fe=build\FULLDSKT.EXE" src\fulldskt.c`
- Avec `-wx` activé, tout avertissement de conversion implicite (`unsigned long` vers `unsigned int`) ou de variable inutilisée doit être corrigé par un cast explicite.

## Contraintes du langage C89 / Mode réel 16 bits
1. **Norme C89 stricte** : Déclare toutes les variables locales en début de bloc (`{`). Pas de commentaires `//` (utilise uniquement `/* ... */`), pas de `<stdint.h>` ni `<stdbool.h>`. Utilise `unsigned char`, `unsigned int` (16 bits), `unsigned long` (32 bits).
2. **Gestion mémoire** : La pile DOS fait 2 à 4 Ko. Ne déclare JAMAIS de grands tableaux en variables locales. Déclare le tampon principal de 32 768 octets en `static unsigned char buffer[32768];`.
3. **Appels système DOS / BIOS** : Utilise `<dos.h>` (`int86`, `intdos`, `union REGS`) ou les fonctions bas niveau `_dos_creat`, `_dos_write`, `_dos_read`, `_dos_close` pour contrôler finement les codes d'erreur et le vidage des tampons (`INT 21h AH=0Dh` + `INT 2Fh AX=4A10h BX=0001h`).
4. **Convention 8.3 & FAT12** : Tous les fichiers générés sur la disquette cible doivent respecter le format 8.3 (`B00.H2W`... pour les blocs de 32 Ko, `S00.H2W`... pour les blocs de 512 octets) et rester sous la limite des 112 entrées du répertoire racine FAT12 (360 Ko / 720 Ko).
5. **Support multi-formats automatique (5,25" et 3,5")** : Lire dynamiquement la géométrie du média (secteurs/piste, têtes, secteurs/cluster, total secteurs) via le BPB (`INT 21h AX=440Dh CX=0860h` ou lecture BIOS `INT 13h AH=02h` du secteur 0 — ne pas appeler `INT 25h` via `int86` à cause du `POPF` résiduel sur la pile) afin d'adapter automatiquement la saturation et le calcul des coordonnées CHS (Cylindre/Tête/Secteur) en cas d'erreur, sans paramètre manuel de capacité.

## Exécution et banc de test (PowerShell / DOSBox-X)
- **Compilation depuis PowerShell** : Exécuter `cmd /c build.bat` (après avoir vérifié `WATCOM=C:\WATCOM` et `C:\WATCOM\BINNT64` dans le `PATH`).
- **Test en émulation DOSBox-X** : Ne jamais monter directement `test\template_*.img` en écriture. Copier d'abord l'image modèle vers `test\work.img`, puis exécuter `"D:\JeanFred\Mes Documents\Divers\DOSBox-X\bin\mingw-build\mingw\dosbox-x.exe"` en testant en priorité le format 5,25" 360 Ko (`test\template_360.img`), puis 3,5" 1,44 Mo (`test\template_144.img`).