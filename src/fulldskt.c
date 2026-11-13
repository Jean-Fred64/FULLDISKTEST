/*
 * FULLDSKT.C - Utilitaire de test integral de disquette type H2testw
 * Cible : MS-DOS 3.30 a 6.22 (16 bits mode reel 8086/8088, modele Small)
 * Compilateur : Open Watcom C (wcl -0 -ms -os -wx)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <fcntl.h>

#define APP_NAME      "FULLDSKT"
#define APP_VERSION   "v1.0.1"
#define APP_DATE      "13/11/2026"

#define BLOCK32K_SIZE 32768U
#define BLOCK512_SIZE 512U
#define MAX_B_FILES   100U
#define MAX_S_FILES   100U

/* Chaine d'identification statique conservee dans le segment de donnees */
static const char app_ident[] =
    APP_NAME " " APP_VERSION " (" APP_DATE ") - 16-bit DOS Floppy Integrity Tester";

/* Tampon principal de 32 Ko en segment de donnees statique (pile DOS preservee) */
static unsigned char buffer[BLOCK32K_SIZE];

/* Motif de reference deterministe de 64 octets (divise 512 et 32768) */
static const unsigned char ref_pattern[64] =
    "0123456789ABCDEF-H2TESTW-DOS622-55AA-00FF-VERIF-SECTEURS-OK!--\r\n";

/* Structures de geometrie du media */
struct disk_geometry {
    unsigned int  bytes_per_sector;
    unsigned char sectors_per_cluster;
    unsigned int  reserved_sectors;
    unsigned char num_fats;
    unsigned int  root_entries;
    unsigned int  total_sectors;
    unsigned char media_descriptor;
    unsigned int  sectors_per_fat;
    unsigned int  sectors_per_track;
    unsigned int  num_heads;
    unsigned int  first_data_sector;
    unsigned int  total_cylinders;
};

/* Variables globales pour le gestionnaire d'erreur critique INT 24h */
static volatile unsigned char g_crit_err = 0;
static volatile unsigned int  g_crit_code = 0;

/*
 * Gestionnaire d'erreur critique DOS (INT 24h)
 * Retourne _HARDERR_FAIL (supporte depuis DOS 3.30) pour eviter le blocage
 * interactif "Abort, Retry, Ignore, Fail?" lors d'un secteur defectueux.
 */
int __far crit_err_handler(unsigned deverr, unsigned errcode, unsigned __far *devhdr)
{
    (void)deverr;
    (void)devhdr;
    g_crit_err = 1;
    g_crit_code = errcode;
    return _HARDERR_FAIL;
}

/* Lecture d'un entier 16 bits Little-Endian dans un tampon d'octets */
static unsigned int read_le16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* Conversion d'un secteur logique (LBA) en coordonnees physiques CHS */
static void lba_to_chs(unsigned long lba,
                       const struct disk_geometry *geom,
                       unsigned int *cyl,
                       unsigned int *head,
                       unsigned int *sect)
{
    unsigned long spc_track;
    unsigned long temp;

    if (geom->sectors_per_track == 0U || geom->num_heads == 0U) {
        *cyl = 0U;
        *head = 0U;
        *sect = 1U;
        return;
    }

    spc_track = (unsigned long)geom->sectors_per_track;
    temp = lba / spc_track;
    *sect = (unsigned int)((lba % spc_track) + 1UL);
    *head = (unsigned int)(temp % (unsigned long)geom->num_heads);
    *cyl  = (unsigned int)(temp / (unsigned long)geom->num_heads);
}

/* Purge du cache DOS (INT 21h AH=0Dh) et de SmartDrive (INT 2Fh AX=4A10h BX=0001h) */
static void flush_disk_caches(unsigned char bios_drive)
{
    union REGS r;

    /* 1. Vidage des tampons internes de MS-DOS (Disk Reset) */
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x0D;
    int86(0x21, &r, &r);

    /* 2. Vidage du cache SmartDrive / caches compatibles */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4A10;
    r.x.bx = 0x0001;
    int86(0x2F, &r, &r);

    /* 3. Reinitialisation du controleur de disquette BIOS (INT 13h AH=00h) */
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x00;
    r.h.dl = bios_drive;
    int86(0x13, &r, &r);
}

/* Attente d'une touche via DOS INT 21h AH=08h (sans echo, gere Ctrl+C) */
static unsigned char wait_key(void)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0x08;
    int86(0x21, &r, &r);
    if (r.h.al == 0U) {
        /* Touche etendue : consommer le second code */
        r.h.ah = 0x08;
        int86(0x21, &r, &r);
        return 0U;
    }
    return r.h.al;
}

/* Description textuelle du format de disquette selon le nombre total de secteurs */
static const char *describe_media(const struct disk_geometry *g)
{
    switch (g->total_sectors) {
        case 320U:  return "5,25\" 160 Ko (SS/DD, 40 pistes, 8 sect/piste)";
        case 360U:  return "5,25\" 180 Ko (SS/DD, 40 pistes, 9 sect/piste)";
        case 640U:  return "5,25\" 320 Ko (DS/DD, 40 pistes, 8 sect/piste)";
        case 720U:  return "5,25\" 360 Ko (DS/DD, 40 pistes, 9 sect/piste)";
        case 1440U: return "3,5\"  720 Ko (DS/DD, 80 pistes, 9 sect/piste)";
        case 2400U: return "5,25\" 1,2 Mo (DS/HD, 80 pistes, 15 sect/piste)";
        case 2880U: return "3,5\"  1,44 Mo (DS/HD, 80 pistes, 18 sect/piste)";
        case 5760U: return "3,5\"  2,88 Mo (DS/ED, 80 pistes, 36 sect/piste)";
        default:    return "Format FAT12 personnalise / standard";
    }
}

/*
 * Detection dynamique de la geometrie du media :
 * 1) Lecture physique du secteur de boot (Cyl 0, Tete 0, Sect 1) via BIOS INT 13h AH=02h
 * 2) Repli sur l'appel IOCTL DOS INT 21h AX=440Dh CX=0860h (Get Device Parameters)
 * 3) Validation et consolidation via INT 21h AH=36h (_dos_getdiskfree)
 */
static int detect_geometry(unsigned int drive_num,
                           unsigned char bios_drive,
                           struct disk_geometry *geom,
                           unsigned long *out_free_bytes,
                           unsigned long *out_initial_used_sectors)
{
    union REGS r;
    struct SREGS sr;
    struct _diskfree_t df;
    unsigned char ioctl_param[40];
    int retry;
    int bpb_ok;
    unsigned int root_dir_sectors;
    unsigned int data_sectors;
    unsigned int used_clusters;

    memset(geom, 0, sizeof(*geom));
    bpb_ok = 0;

    /* Etape 1 : Lecture BIOS INT 13h AH=02h du secteur de boot (C=0, H=0, S=1) */
    for (retry = 0; retry < 3; retry++) {
        memset(buffer, 0, 512U);
        memset(&r, 0, sizeof(r));
        memset(&sr, 0, sizeof(sr));
        segread(&sr);
        r.h.ah = 0x02;          /* Lecture de secteurs */
        r.h.al = 0x01;          /* 1 secteur */
        r.h.ch = 0x00;          /* Cylindre 0 */
        r.h.cl = 0x01;          /* Secteur 1 */
        r.h.dh = 0x00;          /* Tete 0 */
        r.h.dl = bios_drive;    /* 0x00 = A:, 0x01 = B: */
        sr.es  = sr.ds;
        r.x.bx = (unsigned int)buffer;
        int86x(0x13, &r, &r, &sr);

        if (r.x.cflag == 0U) {
            unsigned int bps = read_le16(&buffer[11]);
            unsigned char spc = buffer[13];
            unsigned int spt = read_le16(&buffer[24]);
            unsigned int heads = read_le16(&buffer[26]);
            unsigned int tot16 = read_le16(&buffer[19]);

            if (bps == 512U && spc > 0U && spt > 0U && heads > 0U && tot16 > 0U) {
                geom->bytes_per_sector    = bps;
                geom->sectors_per_cluster = spc;
                geom->reserved_sectors    = read_le16(&buffer[14]);
                geom->num_fats            = buffer[16];
                geom->root_entries        = read_le16(&buffer[17]);
                geom->total_sectors       = tot16;
                geom->media_descriptor    = buffer[21];
                geom->sectors_per_fat     = read_le16(&buffer[22]);
                geom->sectors_per_track   = spt;
                geom->num_heads           = heads;
                bpb_ok = 1;
                break;
            }
        }

        /* Reset du controleur disquette avant nouvelle tentative */
        memset(&r, 0, sizeof(r));
        r.h.ah = 0x00;
        r.h.dl = bios_drive;
        int86(0x13, &r, &r);
    }

    /* Etape 2 : Repli sur INT 21h AX=440Dh CX=0860h (IOCTL Get Device Parameters) */
    if (!bpb_ok) {
        memset(ioctl_param, 0, sizeof(ioctl_param));
        ioctl_param[0] = 0x01; /* Bit 0 = 1 : BPB du media courant */
        memset(&r, 0, sizeof(r));
        memset(&sr, 0, sizeof(sr));
        segread(&sr);
        r.x.ax = 0x440D;
        r.h.bl = (unsigned char)drive_num;
        r.x.cx = 0x0860;
        r.x.dx = (unsigned int)ioctl_param;
        intdosx(&r, &r, &sr);

        if (r.x.cflag == 0U && read_le16(&ioctl_param[7]) == 512U) {
            geom->bytes_per_sector    = read_le16(&ioctl_param[7]);
            geom->sectors_per_cluster = ioctl_param[9];
            geom->reserved_sectors    = read_le16(&ioctl_param[10]);
            geom->num_fats            = ioctl_param[12];
            geom->root_entries        = read_le16(&ioctl_param[13]);
            geom->total_sectors       = read_le16(&ioctl_param[15]);
            geom->media_descriptor    = ioctl_param[17];
            geom->sectors_per_fat     = read_le16(&ioctl_param[18]);
            geom->sectors_per_track   = read_le16(&ioctl_param[20]);
            geom->num_heads           = read_le16(&ioctl_param[22]);
            if (geom->total_sectors > 0U && geom->sectors_per_track > 0U && geom->num_heads > 0U) {
                bpb_ok = 1;
            }
        }
    }

    /* Etape 3 : Verification de l'espace disque via DOS (_dos_getdiskfree / INT 21h AH=36h) */
    g_crit_err = 0;
    if (_dos_getdiskfree(drive_num, &df) != 0U || g_crit_err != 0U || df.sectors_per_cluster == 0xFFFFU) {
        return -1;
    }

    /* Si le BPB n'a pu etre lu ni par INT 13h ni par IOCTL, deduction depuis getdiskfree */
    if (!bpb_ok) {
        geom->bytes_per_sector    = df.bytes_per_sector;
        geom->sectors_per_cluster = (unsigned char)df.sectors_per_cluster;
        geom->reserved_sectors    = 1U;
        geom->num_fats            = 2U;
        data_sectors = df.total_clusters * df.sectors_per_cluster;
        if (data_sectors <= 720U) {
            geom->root_entries      = 112U;
            geom->sectors_per_fat   = 2U;
            geom->sectors_per_track = 9U;
            geom->num_heads         = 2U;
            geom->total_sectors     = 720U;
            geom->media_descriptor  = 0xFDU;
        } else if (data_sectors <= 1440U) {
            geom->root_entries      = 112U;
            geom->sectors_per_fat   = 3U;
            geom->sectors_per_track = 9U;
            geom->num_heads         = 2U;
            geom->total_sectors     = 1440U;
            geom->media_descriptor  = 0xF9U;
        } else if (data_sectors <= 2400U) {
            geom->root_entries      = 224U;
            geom->sectors_per_fat   = 7U;
            geom->sectors_per_track = 15U;
            geom->num_heads         = 2U;
            geom->total_sectors     = 2400U;
            geom->media_descriptor  = 0xF9U;
        } else if (data_sectors <= 2880U) {
            geom->root_entries      = 224U;
            geom->sectors_per_fat   = 9U;
            geom->sectors_per_track = 18U;
            geom->num_heads         = 2U;
            geom->total_sectors     = 2880U;
            geom->media_descriptor  = 0xF0U;
        } else {
            geom->root_entries      = 240U;
            geom->sectors_per_fat   = 9U;
            geom->sectors_per_track = 36U;
            geom->num_heads         = 2U;
            geom->total_sectors     = 5760U;
            geom->media_descriptor  = 0xF0U;
        }
    }

    root_dir_sectors = (geom->root_entries * 32U + (geom->bytes_per_sector - 1U)) / geom->bytes_per_sector;
    geom->first_data_sector = geom->reserved_sectors +
                              ((unsigned int)geom->num_fats * geom->sectors_per_fat) +
                              root_dir_sectors;

    if (geom->sectors_per_track > 0U && geom->num_heads > 0U) {
        geom->total_cylinders = geom->total_sectors / (geom->sectors_per_track * geom->num_heads);
    } else {
        geom->total_cylinders = 0U;
    }

    *out_free_bytes = (unsigned long)df.avail_clusters *
                      (unsigned long)df.sectors_per_cluster *
                      (unsigned long)df.bytes_per_sector;

    if (df.total_clusters >= df.avail_clusters) {
        used_clusters = df.total_clusters - df.avail_clusters;
    } else {
        used_clusters = 0U;
    }
    *out_initial_used_sectors = (unsigned long)used_clusters * (unsigned long)df.sectors_per_cluster;

    return 0;
}

/* Suppression de tous les fichiers *.H2W presents sur le lecteur cible */
static unsigned int cleanup_h2w_files(char drive_letter, unsigned int b_count, unsigned int s_count)
{
    char path[20];
    char pattern[20];
    struct find_t ff;
    unsigned int removed_count;
    unsigned int i;
    unsigned int rc;

    removed_count = 0U;

    for (i = 0U; i < b_count; i++) {
        sprintf(path, "%c:\\B%02u.H2W", drive_letter, i);
        if (remove(path) == 0) {
            removed_count++;
        }
    }

    for (i = 0U; i < s_count; i++) {
        sprintf(path, "%c:\\S%02u.H2W", drive_letter, i);
        if (remove(path) == 0) {
            removed_count++;
        }
    }

    /* Balayage complementaire de securite pour tout fichier *.H2W residuel */
    sprintf(pattern, "%c:\\*.H2W", drive_letter);
    rc = _dos_findfirst(pattern, _A_NORMAL | _A_ARCH, &ff);
    while (rc == 0U) {
        sprintf(path, "%c:\\%s", drive_letter, ff.name);
        if (remove(path) == 0) {
            removed_count++;
        }
        rc = _dos_findnext(&ff);
    }

    return removed_count;
}

/* Lecture de l'espace libre en octets sur le lecteur cible */
static unsigned long get_free_bytes(unsigned int drive_num)
{
    struct _diskfree_t df;

    g_crit_err = 0;
    if (_dos_getdiskfree(drive_num, &df) != 0U || g_crit_err != 0U || df.sectors_per_cluster == 0xFFFFU) {
        return 0UL;
    }
    return (unsigned long)df.avail_clusters *
           (unsigned long)df.sectors_per_cluster *
           (unsigned long)df.bytes_per_sector;
}

/* Affichage d'une barre de progression ASCII 7 bits (30 caracteres '#' et '.') */
static void update_progress(const char *prefix,
                            unsigned long current,
                            unsigned long total,
                            int reset)
{
    static unsigned int last_pct    = 0xFFFFU;
    static unsigned int last_filled = 0xFFFFU;
    unsigned int pct;
    unsigned int filled;
    unsigned int k;
    char bar[31];

    if (reset != 0) {
        last_pct    = 0xFFFFU;
        last_filled = 0xFFFFU;
    }

    if (total == 0UL || current >= total) {
        filled = 30U;
        pct    = 100U;
    } else {
        filled = (unsigned int)((current * 30UL) / total);
        pct    = (unsigned int)((current * 100UL) / total);
        if (filled > 30U) {
            filled = 30U;
        }
        if (pct > 100U) {
            pct = 100U;
        }
    }

    if (reset == 0 && pct == last_pct && filled == last_filled) {
        return;
    }

    for (k = 0U; k < 30U; k++) {
        bar[k] = (k < filled) ? '#' : '.';
    }
    bar[30] = '\0';

    printf("\r%s : [%s] %3u%%", prefix, bar, pct);
    fflush(stdout);

    last_pct    = pct;
    last_filled = filled;
}

/* Wrapper de progression pour la phase [4/4] Verification */
static void update_verify_progress(unsigned long bytes_verified,
                                   unsigned long total_verify_bytes,
                                   int reset)
{
    update_progress("[4/4] Verification", bytes_verified, total_verify_bytes, reset);
}

/* Affichage de la banniere principale (strictement < 80 colonnes) */
static void print_banner(void)
{
    if (app_ident[0] != '\0') {
        printf("====================================================================\n");
        printf("  " APP_NAME " " APP_VERSION " (" APP_DATE ") - TEST INTEGRAL TYPE H2TESTW\n");
        printf("  Compatible 8086/8088 & MS-DOS 3.30-6.22\n");
        printf("====================================================================\n");
    }
}

/* Affichage de l'ecran d'aide /? (calibre pour console 80x25 sans defilement) */
static void print_help(void)
{
    print_banner();
    printf("Usage : FULLDSKT [A:|B:] [/Y] [/K] [/V] [/?]\n\n");
    printf("Parametres et commutateurs :\n");
    printf("  A: ou B:  Lecteur de disquette cible (par defaut : A:, securise A:/B:)\n");
    printf("  /Y        Mode automatique : supprime les fichiers .H2W en fin de test\n");
    printf("            sans demander de confirmation clavier\n");
    printf("  /K        Keep : conserve les fichiers Bxx.H2W et Sxx.H2W sur la\n");
    printf("            disquette (test de retention magnetique ou 2nd lecteur)\n");
    printf("  /V        Verify only : saute l'ecriture et verifie directement les\n");
    printf("            fichiers .H2W deja presents (crees avec /K)\n");
    printf("  /?        Affiche cet ecran d'aide et les options disponibles\n\n");
    printf("Phases du test :\n");
    printf("  [1/4] Motif deterministe (32 Ko)    [3/4] Saturation 512 o (Sxx.H2W)\n");
    printf("  [2/4] Ecriture blocs 32 Ko (Bxx)    [4/4] Purge caches & verification\n");
}

int main(int argc, char *argv[])
{
    unsigned int drive_num;
    unsigned char bios_drive;
    char drive_letter;
    unsigned int cur_drive;
    unsigned char auto_mode;
    unsigned char keep_mode;
    unsigned char verify_only;
    int arg_idx;
    struct disk_geometry geom;
    unsigned long free_bytes;
    unsigned long initial_used_sectors;
    unsigned long total_bytes_written;
    unsigned long total_bytes_verified;
    unsigned long total_verify_bytes;
    unsigned long bytes_verified;
    unsigned long file_start_lba;
    unsigned long err_lba;
    unsigned int cyl;
    unsigned int head;
    unsigned int sect;
    unsigned int b_count;
    unsigned int s_count;
    unsigned long total_write_target;
    unsigned long bytes_written_phase2;
    unsigned char bar_on_line;
    unsigned int i;
    unsigned int j;
    unsigned int written;
    unsigned int bytes_read;
    unsigned int rc;
    int handle;
    int err_flag;
    unsigned int error_count;
    unsigned char key;
    char filepath[20];

    /* Configuration par defaut : Lecteur A: */
    drive_num    = 1U;
    bios_drive   = 0x00U;
    drive_letter = 'A';
    auto_mode    = 0U;
    keep_mode    = 0U;
    verify_only  = 0U;

    /* Analyse des arguments de la ligne de commande */
    for (arg_idx = 1; arg_idx < argc; arg_idx++) {
        const char *arg = argv[arg_idx];
        if (strcmp(arg, "/?") == 0 || strcmp(arg, "-?") == 0 ||
            strcmp(arg, "/h") == 0 || strcmp(arg, "/H") == 0 ||
            strcmp(arg, "-h") == 0 || strcmp(arg, "-H") == 0 ||
            stricmp(arg, "--help") == 0) {
            print_help();
            return 0;
        } else if (strcmp(arg, "A:") == 0 || strcmp(arg, "a:") == 0 ||
                   strcmp(arg, "A") == 0  || strcmp(arg, "a") == 0) {
            drive_num    = 1U;
            bios_drive   = 0x00U;
            drive_letter = 'A';
        } else if (strcmp(arg, "B:") == 0 || strcmp(arg, "b:") == 0 ||
                   strcmp(arg, "B") == 0  || strcmp(arg, "b") == 0) {
            drive_num    = 2U;
            bios_drive   = 0x01U;
            drive_letter = 'B';
        } else if (strcmp(arg, "/Y") == 0 || strcmp(arg, "/y") == 0 ||
                   strcmp(arg, "-Y") == 0 || strcmp(arg, "-y") == 0) {
            auto_mode = 1U;
        } else if (strcmp(arg, "/K") == 0 || strcmp(arg, "/k") == 0 ||
                   strcmp(arg, "-K") == 0 || strcmp(arg, "-k") == 0) {
            keep_mode = 1U;
        } else if (strcmp(arg, "/V") == 0 || strcmp(arg, "/v") == 0 ||
                   strcmp(arg, "-V") == 0 || strcmp(arg, "-v") == 0) {
            verify_only = 1U;
        } else {
            printf("\n[ERREUR] Argument invalide : %s\n", arg);
            printf("Usage : FULLDSKT [A:|B:] [/Y] [/K] [/V] [/?]\n");
            printf("Seuls les lecteurs de disquette A: et B: sont autorises.\n");
            return 1;
        }
    }

    /* Securite lecteur courant : interdiction d'execution depuis A:, B: ou le lecteur teste */
    _dos_getdrive(&cur_drive);
    if (cur_drive == drive_num || cur_drive == 1U || cur_drive == 2U) {
        printf("\n[ERREUR] Ce programme ne doit pas etre lance depuis A: ou B: !\n");
        printf("Placez-vous sur un disque dur (ex: C:\\) avant d'executer FULLDSKT.\n");
        return 1;
    }
    if (argc > 0 && argv[0] != NULL && argv[0][0] != '\0' && argv[0][1] == ':') {
        char prog_drv = argv[0][0];
        if (prog_drv >= 'a' && prog_drv <= 'z') {
            prog_drv = (char)(prog_drv - ('a' - 'A'));
        }
        if (prog_drv == drive_letter || prog_drv == 'A' || prog_drv == 'B') {
            printf("\n[ERREUR] FULLDSKT ne doit pas etre execute depuis le lecteur %c: !\n", prog_drv);
            printf("Copiez FULLDSKT.EXE sur un disque dur (ex: C:\\) avant de le lancer.\n");
            return 1;
        }
    }

    /* Installation du gestionnaire d'erreurs critiques DOS (INT 24h) */
    _harderr(crit_err_handler);

    print_banner();
    printf("Lecteur cible : %c:\n", drive_letter);
    printf("Inserez une disquette formatee et vide dans %c:\n", drive_letter);

    if (!auto_mode) {
        printf("Appuyez sur une touche pour continuer (ou Echap pour annuler)...\n");
        key = wait_key();
        if (key == 0x1BU || key == 0x03U) {
            printf("Operation annulee par l'utilisateur.\n");
            return 1;
        }
    }

    /* Nettoyage preventif d'eventuels fichiers .H2W residuels d'un test precedent */
    if (!verify_only) {
        cleanup_h2w_files(drive_letter, 0U, 0U);
    }
    flush_disk_caches(bios_drive);

    /* Detection automatique de la geometrie du media */
    if (detect_geometry(drive_num, bios_drive, &geom, &free_bytes, &initial_used_sectors) != 0) {
        printf("\n[ERREUR] Impossible de lire la disquette dans %c: (absente ou non formatee).\n",
               drive_letter);
        return 1;
    }

    if (verify_only) {
        initial_used_sectors = 0UL;
    }

    printf("\n--- Geometrie du media detectee sur %c: ---\n", drive_letter);
    printf("  Format        : %s\n", describe_media(&geom));
    printf("  Capacite      : %lu Ko (%u secteurs de %u octets)\n",
           ((unsigned long)geom.total_sectors * (unsigned long)geom.bytes_per_sector) / 1024UL,
           geom.total_sectors,
           geom.bytes_per_sector);
    printf("  Geometrie CHS : %u cylindres, %u tete(s), %u secteurs/piste\n",
           geom.total_cylinders,
           geom.num_heads,
           geom.sectors_per_track);
    printf("  Cluster FAT12 : %u secteur(s)/cluster (%u octets), %u entrees racine\n",
           (unsigned int)geom.sectors_per_cluster,
           (unsigned int)geom.sectors_per_cluster * geom.bytes_per_sector,
           geom.root_entries);
    printf("  Zone donnees  : debut LBA %u | Espace libre : %lu octets\n\n",
           geom.first_data_sector,
           free_bytes);

    if (!verify_only && free_bytes == 0UL) {
        printf("[ERREUR] La disquette dans %c: est deja pleine (0 octet libre) !\n", drive_letter);
        return 1;
    }

    /* --- 1. PREPARATION DU TAMPON DE REFERENCE DE 32 KO --- */
    printf("[1/4] Initialisation du motif de reference en memoire (32 Ko)...\n");
    for (i = 0U; i < BLOCK32K_SIZE; i += 64U) {
        memcpy(&buffer[i], ref_pattern, 64U);
    }

    err_flag            = 0;
    error_count         = 0U;
    b_count             = 0U;
    s_count             = 0U;
    total_bytes_written = 0UL;

    if (verify_only) {
        for (i = 0U; i < MAX_B_FILES; i++) {
            sprintf(filepath, "%c:\\B%02u.H2W", drive_letter, i);
            g_crit_err = 0;
            if (_dos_open(filepath, O_RDONLY, &handle) == 0U && g_crit_err == 0U) {
                _dos_close(handle);
                b_count++;
                total_bytes_written += (unsigned long)BLOCK32K_SIZE;
            } else {
                break;
            }
        }
        for (i = 0U; i < MAX_S_FILES; i++) {
            sprintf(filepath, "%c:\\S%02u.H2W", drive_letter, i);
            g_crit_err = 0;
            if (_dos_open(filepath, O_RDONLY, &handle) == 0U && g_crit_err == 0U) {
                _dos_close(handle);
                s_count++;
                total_bytes_written += (unsigned long)BLOCK512_SIZE;
            } else {
                break;
            }
        }
        printf("[2/4] Mode verification seule (/V) : %u bloc(s) de 32 Ko detecte(s).\n", b_count);
        printf("[3/4] Mode verification seule (/V) : %u bloc(s) de 512 o detecte(s).\n", s_count);
        if (b_count == 0U && s_count == 0U) {
            err_flag = 1;
        }
    } else {
        /* --- 2. ECRITURE DES BLOCS DE 32 KO (B00.H2W...) --- */
        free_bytes = get_free_bytes(drive_num);
        total_write_target = (free_bytes / (unsigned long)BLOCK32K_SIZE) * (unsigned long)BLOCK32K_SIZE;
        if (total_write_target > ((unsigned long)MAX_B_FILES * (unsigned long)BLOCK32K_SIZE)) {
            total_write_target = (unsigned long)MAX_B_FILES * (unsigned long)BLOCK32K_SIZE;
        }
        bytes_written_phase2 = 0UL;

        if (total_write_target > 0UL) {
            update_progress("[2/4] Ecriture    ", 0UL, total_write_target, 1);
            bar_on_line = 1U;

            for (i = 0U; i < MAX_B_FILES; i++) {
                free_bytes = get_free_bytes(drive_num);
                if (free_bytes < (unsigned long)BLOCK32K_SIZE) {
                    break;
                }

                sprintf(filepath, "%c:\\B%02u.H2W", drive_letter, i);
                file_start_lba = (unsigned long)geom.first_data_sector +
                                 initial_used_sectors +
                                 ((unsigned long)i * 64UL);

                g_crit_err = 0;
                rc = _dos_creat(filepath, _A_NORMAL, &handle);
                if (rc != 0U || g_crit_err != 0U) {
                    if (bar_on_line != 0U) {
                        printf("\n");
                        bar_on_line = 0U;
                    }
                    lba_to_chs(file_start_lba, &geom, &cyl, &head, &sect);
                    printf("  [ERREUR] Echec creation %s -> LBA %lu (Cyl %u, Tete %u, Sect %u)\n",
                           filepath, file_start_lba, cyl, head, sect);
                    err_flag = 1;
                    error_count++;
                    break;
                }

                written = 0U;
                g_crit_err = 0;
                rc = _dos_write(handle, buffer, BLOCK32K_SIZE, &written);
                _dos_close(handle);

                if (rc != 0U || g_crit_err != 0U || written != BLOCK32K_SIZE) {
                    if (bar_on_line != 0U) {
                        printf("\n");
                        bar_on_line = 0U;
                    }
                    err_lba = file_start_lba + (unsigned long)(written / geom.bytes_per_sector);
                    lba_to_chs(err_lba, &geom, &cyl, &head, &sect);
                    printf("  [ERREUR] Echec ecriture %s (%u/%u o) -> LBA %lu (Cyl %u, Tete %u, Sect %u)\n",
                           filepath, written, BLOCK32K_SIZE, err_lba, cyl, head, sect);
                    remove(filepath);
                    err_flag = 1;
                    error_count++;
                    break;
                }

                b_count++;
                total_bytes_written += (unsigned long)BLOCK32K_SIZE;
                bytes_written_phase2 += (unsigned long)BLOCK32K_SIZE;
                update_progress("[2/4] Ecriture    ", bytes_written_phase2, total_write_target, 0);
                bar_on_line = 1U;
            }

            if (bar_on_line != 0U) {
                update_progress("[2/4] Ecriture    ", total_write_target, total_write_target, 0);
                printf("\n");
                bar_on_line = 0U;
            }
        }

        /* --- 3. SATURATION FINALE PAR BLOCS DE 512 OCTETS (S00.H2W...) --- */
        printf("[3/4] Saturation des derniers secteurs libres par blocs de 512 octets...\n");
        for (i = 0U; i < MAX_S_FILES; i++) {
            free_bytes = get_free_bytes(drive_num);
            if (free_bytes == 0UL) {
                break;
            }

            sprintf(filepath, "%c:\\S%02u.H2W", drive_letter, i);
            file_start_lba = (unsigned long)geom.first_data_sector +
                             initial_used_sectors +
                             ((unsigned long)b_count * 64UL) +
                             ((unsigned long)i * (unsigned long)geom.sectors_per_cluster);

            g_crit_err = 0;
            rc = _dos_creat(filepath, _A_NORMAL, &handle);
            if (rc != 0U || g_crit_err != 0U) {
                lba_to_chs(file_start_lba, &geom, &cyl, &head, &sect);
                printf("  [ERREUR] Echec creation %s -> LBA %lu (Cyl %u, Tete %u, Sect %u)\n",
                       filepath, file_start_lba, cyl, head, sect);
                err_flag = 1;
                error_count++;
                break;
            }

            written = 0U;
            g_crit_err = 0;
            rc = _dos_write(handle, buffer, BLOCK512_SIZE, &written);
            _dos_close(handle);

            if (rc != 0U || g_crit_err != 0U || written != BLOCK512_SIZE) {
                err_lba = file_start_lba + (unsigned long)(written / geom.bytes_per_sector);
                lba_to_chs(err_lba, &geom, &cyl, &head, &sect);
                printf("  [ERREUR] Echec ecriture %s (%u/%u o) -> LBA %lu (Cyl %u, Tete %u, Sect %u)\n",
                       filepath, written, BLOCK512_SIZE, err_lba, cyl, head, sect);
                remove(filepath);
                err_flag = 1;
                error_count++;
                break;
            }

            s_count++;
            total_bytes_written += (unsigned long)BLOCK512_SIZE;
            printf("  Secteur S%02u.H2W (512 o) [OK]\n", i);
        }

        free_bytes = get_free_bytes(drive_num);
        printf("  Saturation terminee : %u bloc(s) de 32 Ko, %u bloc(s) de 512 o (%lu octets libres).\n",
               b_count, s_count, free_bytes);

        if ((b_count == 0U && s_count == 0U) || free_bytes != 0UL) {
            err_flag = 1;
        }
    }

    /* --- 4. PURGE DU CACHE ET RELECTURE / COMPARAISON OCTET PAR OCTET --- */
    printf("[4/4] Purge des caches (DOS + SmartDrive) et verification physique...\n");
    flush_disk_caches(bios_drive);

    total_bytes_verified = 0UL;
    total_verify_bytes   = ((unsigned long)b_count * (unsigned long)BLOCK32K_SIZE) +
                           ((unsigned long)s_count * (unsigned long)BLOCK512_SIZE);
    bytes_verified       = 0UL;

    update_verify_progress(0UL, total_verify_bytes, 1);
    bar_on_line = 1U;

    /* Verification des fichiers de 32 Ko (B00.H2W...) */
    for (i = 0U; i < b_count; i++) {
        sprintf(filepath, "%c:\\B%02u.H2W", drive_letter, i);
        file_start_lba = (unsigned long)geom.first_data_sector +
                         initial_used_sectors +
                         ((unsigned long)i * 64UL);

        memset(buffer, 0, BLOCK32K_SIZE);
        g_crit_err = 0;
        rc = _dos_open(filepath, O_RDONLY, &handle);
        if (rc != 0U || g_crit_err != 0U) {
            if (bar_on_line != 0U) {
                printf("\n");
                bar_on_line = 0U;
            }
            lba_to_chs(file_start_lba, &geom, &cyl, &head, &sect);
            printf("  [ERREUR] Ouverture impossible de %s -> LBA %lu (Cyl %u, Tete %u, Sect %u)\n",
                   filepath, file_start_lba, cyl, head, sect);
            err_flag = 1;
            error_count++;
            bytes_verified += (unsigned long)BLOCK32K_SIZE;
            update_verify_progress(bytes_verified, total_verify_bytes, 1);
            bar_on_line = 1U;
            continue;
        }

        bytes_read = 0U;
        g_crit_err = 0;
        rc = _dos_read(handle, buffer, BLOCK32K_SIZE, &bytes_read);
        _dos_close(handle);

        if (rc != 0U || g_crit_err != 0U || bytes_read != BLOCK32K_SIZE) {
            if (bar_on_line != 0U) {
                printf("\n");
                bar_on_line = 0U;
            }
            err_lba = file_start_lba + (unsigned long)(bytes_read / geom.bytes_per_sector);
            lba_to_chs(err_lba, &geom, &cyl, &head, &sect);
            printf("  [ERREUR] Lecture physique incomplet/echec sur %s (%u/%u o) -> LBA %lu (Cyl %u, Tete %u, Sect %u)\n",
                   filepath, bytes_read, BLOCK32K_SIZE, err_lba, cyl, head, sect);
            err_flag = 1;
            error_count++;
        }

        for (j = 0U; j < bytes_read; j++) {
            unsigned char expected = ref_pattern[j & 63U];
            if (buffer[j] != expected) {
                if (bar_on_line != 0U) {
                    printf("\n");
                    bar_on_line = 0U;
                }
                err_lba = file_start_lba + (unsigned long)(j / geom.bytes_per_sector);
                lba_to_chs(err_lba, &geom, &cyl, &head, &sect);
                printf("  [ERREUR] Corruption sur %s (offset %u : attendu 0x%02X, lu 0x%02X) -> LBA %lu (Cyl %u, Tete %u, Sect %u)\n",
                       filepath, j, (unsigned int)expected, (unsigned int)buffer[j],
                       err_lba, cyl, head, sect);
                err_flag = 1;
                error_count++;
                /* Avancer a la fin du secteur de 512 octets courant pour eviter d'inonder l'affichage */
                j |= 511U;
            }
        }

        if (bytes_read == BLOCK32K_SIZE) {
            total_bytes_verified += (unsigned long)BLOCK32K_SIZE;
        }
        bytes_verified += (unsigned long)BLOCK32K_SIZE;
        update_verify_progress(bytes_verified, total_verify_bytes, (bar_on_line == 0U) ? 1 : 0);
        bar_on_line = 1U;
    }

    /* Verification des fichiers de 512 octets (S00.H2W...) */
    for (i = 0U; i < s_count; i++) {
        sprintf(filepath, "%c:\\S%02u.H2W", drive_letter, i);
        file_start_lba = (unsigned long)geom.first_data_sector +
                         initial_used_sectors +
                         ((unsigned long)b_count * 64UL) +
                         ((unsigned long)i * (unsigned long)geom.sectors_per_cluster);

        memset(buffer, 0, BLOCK512_SIZE);
        g_crit_err = 0;
        rc = _dos_open(filepath, O_RDONLY, &handle);
        if (rc != 0U || g_crit_err != 0U) {
            if (bar_on_line != 0U) {
                printf("\n");
                bar_on_line = 0U;
            }
            lba_to_chs(file_start_lba, &geom, &cyl, &head, &sect);
            printf("  [ERREUR] Ouverture impossible de %s -> LBA %lu (Cyl %u, Tete %u, Sect %u)\n",
                   filepath, file_start_lba, cyl, head, sect);
            err_flag = 1;
            error_count++;
            bytes_verified += (unsigned long)BLOCK512_SIZE;
            update_verify_progress(bytes_verified, total_verify_bytes, 1);
            bar_on_line = 1U;
            continue;
        }

        bytes_read = 0U;
        g_crit_err = 0;
        rc = _dos_read(handle, buffer, BLOCK512_SIZE, &bytes_read);
        _dos_close(handle);

        if (rc != 0U || g_crit_err != 0U || bytes_read != BLOCK512_SIZE) {
            if (bar_on_line != 0U) {
                printf("\n");
                bar_on_line = 0U;
            }
            lba_to_chs(file_start_lba, &geom, &cyl, &head, &sect);
            printf("  [ERREUR] Lecture physique incomplet/echec sur %s (%u/%u o) -> LBA %lu (Cyl %u, Tete %u, Sect %u)\n",
                   filepath, bytes_read, BLOCK512_SIZE, file_start_lba, cyl, head, sect);
            err_flag = 1;
            error_count++;
        }

        for (j = 0U; j < bytes_read; j++) {
            unsigned char expected = ref_pattern[j & 63U];
            if (buffer[j] != expected) {
                if (bar_on_line != 0U) {
                    printf("\n");
                    bar_on_line = 0U;
                }
                lba_to_chs(file_start_lba, &geom, &cyl, &head, &sect);
                printf("  [ERREUR] Corruption sur %s (offset %u : attendu 0x%02X, lu 0x%02X) -> LBA %lu (Cyl %u, Tete %u, Sect %u)\n",
                       filepath, j, (unsigned int)expected, (unsigned int)buffer[j],
                       file_start_lba, cyl, head, sect);
                err_flag = 1;
                error_count++;
                break;
            }
        }

        if (bytes_read == BLOCK512_SIZE) {
            total_bytes_verified += (unsigned long)BLOCK512_SIZE;
        }
        bytes_verified += (unsigned long)BLOCK512_SIZE;
        update_verify_progress(bytes_verified, total_verify_bytes, (bar_on_line == 0U) ? 1 : 0);
        bar_on_line = 1U;
    }

    update_verify_progress(total_verify_bytes, total_verify_bytes, (bar_on_line == 0U) ? 1 : 0);
    printf("\n");

    /* --- BILAN FINAL --- */
    printf("\n==========================================================\n");
    if (err_flag == 0) {
        printf("  SUCCES : 100%% des secteurs ecrits et relus sans erreur !\n");
    } else {
        printf("  ECHEC  : %u erreur(s) detectee(s) (ecriture/lecture/corruption) !\n", error_count);
    }
    printf("==========================================================\n");
    printf("  Octets ecrits   : %lu octets (%u fichiers .H2W)\n",
           total_bytes_written, b_count + s_count);
    printf("  Octets verifies : %lu octets\n", total_bytes_verified);
    printf("  Octets libres   : %lu octets sur %c:\n", get_free_bytes(drive_num), drive_letter);

    /* --- NETTOYAGE INTERACTIF DES FICHIERS .H2W --- */
    printf("\nAppuyez sur une touche pour effacer les fichiers de test .H2W sur %c:\n", drive_letter);
    printf("(ou 'N' / Echap pour les conserver)...\n");

    if (keep_mode) {
        key = 'N';
    } else if (!auto_mode) {
        key = wait_key();
    } else {
        key = 'O';
    }

    if (key == 'N' || key == 'n' || key == 0x1BU) {
        printf("Fichiers .H2W conserves sur %c:.\n", drive_letter);
    } else {
        unsigned int deleted = cleanup_h2w_files(drive_letter, b_count, s_count);
        flush_disk_caches(bios_drive);
        printf("Disquette nettoyee (%u fichier(s) .H2W supprime(s), %lu octets libres).\n",
               deleted, get_free_bytes(drive_num));
    }

    return (err_flag == 0) ? 0 : 1;
}

