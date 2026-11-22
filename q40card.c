/*--------------------------------------------------------------.
| q40card.c : Q40/Q60 CF-card & HDD image support for qxltool   |
|                                                                |
| Model (per q40card_SPEC.md, verified against a real 4GB card): |
|  - Sector 0 = Atari/AHDI partition table (up to 4 entries)     |
|  - Each partition = a QLWA (".WIN") filesystem                 |
|  - Q40 drives store BOTH the table and partition contents      |
|    byte-swapped (16-bit units) because of the ISA word-window  |
|    read.  QWA partitions use CHS addressing and need the header |
|    geometry (sectors/track, heads) patched.                    |
|  This module is a self-contained container layer; it does not  |
|  touch the interactive filesystem engine in qxltool.c.         |
`--------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>

#include "q40card.h"

#define SECTOR  512
#define MAX_PARTS 4

/* QLWA header field offsets (big-endian view) -- see q40card_SPEC.md */
#define QWA_ID_OFF   0x00
#define QWA_NAME_OFF 0x04
#define QWA_SCTG_OFF 0x22   /* sectors per group */
#define QWA_SCTT_OFF 0x24   /* sectors per track  */
#define QWA_TRKC_OFF 0x26   /* tracks per cyl (heads) */
#define QWA_CYLD_OFF 0x28   /* cylinders */
#define QWA_NGRP_OFF 0x2A   /* number of groups */
#define QWA_FGRP_OFF 0x2C
#define QWA_FSCT_OFF 0x3A   /* first sector (written, never read) */

/* Atari/AHDI root sector field offsets */
#define ART_ID_OFF      0x000
#define ART_HDSIZ_OFF   0x1C2
#define ART_P0_OFF      0x1C6
#define ART_BSLST_OFF   0x1F6
#define ART_BSLCNT_OFF  0x1FA
#define ART_CKSUM_OFF   0x1FE
#define ART_ID_NORM     0x6002
#define ART_ID_SWAP     0x0260

typedef struct {
    unsigned char flag;
    unsigned char type[3];
    uint32_t start;   /* linear 512-byte LBA */
    uint32_t size;    /* sectors */
    int      slot;    /* physical AHDI slot 1..4 (1-based) */
} ATARI_PART;

typedef struct {
    int          order;       /* ORDER_BE or ORDER_SWAPPED for the table */
    int          n;
    ATARI_PART   parts[MAX_PARTS];
    uint32_t     hdsiz;
} ROOT;

static const int ORDER_BE = 0;       /* big-endian (normal) table */
static const int ORDER_SWAPPED = 1;  /* 16-bit-swapped table (Q40) */

/* ---- simple big-endian field accessors on a byte buffer (pointer-to-field) ---- */
static uint16_t rd16 (const unsigned char *p){ return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t rd32 (const unsigned char *p){ return ((uint32_t)rd16(p)<<16)|rd16(p+2); }
static void wr16 (unsigned char *p, uint16_t v){ p[0]=(unsigned char)(v>>8); p[1]=(unsigned char)(v&0xff); }
static void wr32 (unsigned char *p, uint32_t v){ wr16(p,(uint16_t)(v>>16)); wr16(p+2,(uint16_t)(v&0xffff)); }

void swab16buf (void *buf, size_t n)
{
    unsigned char *p = buf; size_t i;
    for (i = 0; i + 1 < n; i += 2) {
        unsigned char t = p[i]; p[i] = p[i+1]; p[i+1] = t;
    }
}

/* ---- file helpers ---- */
static long file_size (const char *path)
{
    struct stat st;
    if (stat (path, &st) != 0) return -1;
    return (long) st.st_size;
}

static int dev_sectors (int fd)
{
    off_t end = lseek (fd, 0, SEEK_END);
    return (int) (end / SECTOR);
}

static int read_sectors (int fd, uint32_t start, unsigned char *buf, uint32_t nsec)
{
    if (lseek (fd, (off_t) start * SECTOR, SEEK_SET) < 0) return -1;
    size_t want = (size_t) nsec * SECTOR, got = 0;
    while (got < want) {
        ssize_t r = read (fd, buf + got, want - got);
        if (r <= 0) return -1;
        got += (size_t) r;
    }
    return 0;
}

static int write_sectors (int fd, uint32_t start, const unsigned char *buf, uint32_t nsec)
{
    if (lseek (fd, (off_t) start * SECTOR, SEEK_SET) < 0) return -1;
    size_t want = (size_t) nsec * SECTOR, put = 0;
    while (put < want) {
        ssize_t w = write (fd, buf + put, want - put);
        if (w <= 0) return -1;
        put += (size_t) w;
    }
    return 0;
}

/* ---- AHDI root-sector parse/build ---- */

/* Parse sector 0 into a ROOT.  Adopts the byte order that yields
   ART_ID==0x6002.  Returns 1 on success (valid AHDI), 0 if not AHDI. */
static int parse_root_sector (const unsigned char *raw512, ROOT *r)
{
    unsigned char sw[512];
    const unsigned char *t;
    int i;

    memset (r, 0, sizeof (*r));

    if (rd16 (raw512 + ART_ID_OFF) == ART_ID_NORM) {
        t = raw512; r->order = ORDER_BE;
    } else {
        memcpy (sw, raw512, 512);
        swab16buf (sw, 512);
        if (rd16 (sw + ART_ID_OFF) != ART_ID_NORM)
            return 0;                      /* not an AHDI card */
        t = sw; r->order = ORDER_SWAPPED;
    }

    r->hdsiz = rd32 (t + ART_HDSIZ_OFF);

    /* Reject if a DOS/Windows MBR signature would hijack this device.
       `t` is always the big-endian (un-swapped) view, so read it directly. */
    {
        uint16_t ck = rd16 (t + 0x1FE);
        if (ck == 0x55AA || ck == 0xAA55)
            fprintf (stderr, "warning: DOS MBR signature at 0x1FE -- use a separate IDE "
                     "target if mixing Atari and MBR schemes\n");
    }

    r->n = 0;
    for (i = 0; i < MAX_PARTS; i++) {
        const unsigned char *e = t + ART_P0_OFF + i * 12;
        ATARI_PART *p = &r->parts[r->n];
        p->flag  = e[0];
        memcpy (p->type, e + 1, 3);
        p->start = rd32 (e + 4);
        p->size  = rd32 (e + 8);
        p->slot  = i + 1;          /* physical AHDI slot, 1-based */
        if ((p->flag & 1) && p->size)
            r->n++;
    }
    return 1;
}

static void build_root_sector (unsigned char *s, const ATARI_PART *parts,
                               int n, uint32_t card_sectors)
{
    int i;
    memset (s, 0, SECTOR);
    wr16 (s + ART_ID_OFF, ART_ID_NORM);
    wr32 (s + ART_HDSIZ_OFF, card_sectors);
    for (i = 0; i < MAX_PARTS; i++) {
        unsigned char *e = s + ART_P0_OFF + i * 12;
        /* the given parts[] are in physical-slot order (slot == index+1) */
        if (parts[i].flag & 1) {
            e[0] = parts[i].flag;                 /* 0x01 valid QWA */
            memcpy (e + 1, parts[i].type, 3);     /* "QWA" */
            wr32 (e + 4, parts[i].start);
            wr32 (e + 8, parts[i].size);
        }
    }
    wr32 (s + ART_BSLST_OFF, 0);
    wr32 (s + ART_BSLCNT_OFF, 0);
    wr16 (s + ART_CKSUM_OFF, 0);
}

/* ---- QLWA geometry patch ---- */

/* Compute cylinders the way ftwa.asm does: from the filesystem span
   (ngrp*sctg), NOT the container byte size. */
static uint32_t qwa_cyld (uint32_t ngrp, uint32_t sctg, uint32_t heads, uint32_t spt)
{
    if (!heads || !spt) return 0;
    return ((ngrp * sctg - 1) / (heads * spt)) + 1;
}

/* Patch sctt/trkc/cyld into a big-endian QLWA header view.
   Return nonzero if the resulting cylinder count exceeds the CHS ceiling. */
static int qwa_patch_geom (unsigned char *hdr, uint16_t spt, uint16_t heads)
{
    uint32_t sctg = rd16 (hdr + QWA_SCTG_OFF);
    uint32_t ngrp = rd16 (hdr + QWA_NGRP_OFF);
    uint32_t cyl  = qwa_cyld (ngrp, sctg, heads, spt);
    int exceeded = 0;

    if (cyl > 65535) exceeded = 1;
    else if (cyl > 16383)
        fprintf (stderr, "warning: %u cylinders > 16383 (most ATA devices report less)\n", cyl);

    wr16 (hdr + QWA_SCTT_OFF, spt);
    wr16 (hdr + QWA_TRKC_OFF, heads);
    wr16 (hdr + QWA_CYLD_OFF, (uint16_t) cyl);
    return exceeded;
}

/* Read a partition's raw first sector; transpose to a big-endian header view.
   Returns 1 if it looks like a QLWA (.WIN); *content_swap set if raw media is
   byte-swapped ('LQAW'). */
static int qwa_read_hdr (int fd, uint32_t lba, unsigned char *hdr512, int *content_swap)
{
    unsigned char raw[512];
    if (read_sectors (fd, lba, raw, 1) != 0) return 0;

    *content_swap = 0;
    if (memcmp (raw, "QLWA", 4) == 0) {
        memcpy (hdr512, raw, 512);
    } else if (memcmp (raw, "LQAW", 4) == 0) {
        memcpy (hdr512, raw, 512);
        swab16buf (hdr512, 512);
        *content_swap = 1;
    } else {
        return 0;
    }
    return 1;
}

static int harvest_from_card (const char *path, uint16_t *spt, uint16_t *heads,
                              int *table_order, int *content_swap)
{
    int fd = open (path, O_RDONLY);
    unsigned char raw[512], hdr[512];
    ROOT r;
    int i;

    if (fd < 0) { perror ("--geometry-from"); return -1; }
    if (read_sectors (fd, 0, raw, 1) != 0) { close (fd); return -1; }
    if (!parse_root_sector (raw, &r)) {
        fprintf (stderr, "--geometry-from: not an AHDI card\n");
        close (fd); return -1;
    }
    *table_order = r.order;
    for (i = 0; i < r.n; i++) {
        int cs = 0;
        if (r.parts[i].start > 0 && qwa_read_hdr (fd, r.parts[i].start, hdr, &cs)) {
            *spt   = rd16 (hdr + QWA_SCTT_OFF);
            *heads = rd16 (hdr + QWA_TRKC_OFF);
            *content_swap = cs;
            close (fd);
            return 0;
        }
    }
    fprintf (stderr, "--geometry-from: no QLWA partition found\n");
    close (fd); return -1;
}

/* ==================================================================== */
/*  Commands                                                            */
/* ==================================================================== */

static int cmd_swap (int ac, char **av)
{
    int in, out;
    long n;
    unsigned char *buf;

    if (ac < 2) { fprintf (stderr, "usage: cardswap IN OUT\n"); return 1; }
    if ((n = file_size (av[0])) < 0) { perror (av[0]); return 1; }
    if (n % 2) { fprintf (stderr, "cardswap: odd file length %ld\n", n); return 1; }
    buf = malloc ((size_t) n);
    if (!buf) { perror ("malloc"); return 1; }
    in = open (av[0], O_RDONLY);
    if (in < 0) { perror (av[0]); free (buf); return 1; }
    if (read (in, buf, (size_t) n) != (ssize_t) n) { perror ("read"); close (in); free (buf); return 1; }
    close (in);
    swab16buf (buf, (size_t) n);
    out = open (av[1], O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { perror (av[1]); free (buf); return 1; }
    if (write (out, buf, (size_t) n) != (ssize_t) n) { perror ("write"); close (out); free (buf); return 1; }
    close (out);
    free (buf);
    return 0;
}

static const char *ordername (int o){ return o == ORDER_SWAPPED ? "swapped" : "big-endian"; }

static void info_qlwa_hdr (const unsigned char *hdr, int content_swap)
{
    unsigned name_len = rd16 (hdr + QWA_NAME_OFF);
    printf ("  id=%c%c%c%c  bytes=%s  name=%.*s\n",
            hdr[0], hdr[1], hdr[2], hdr[3], content_swap ? "swapped" : "big-endian",
            name_len, (const char *)(hdr + QWA_NAME_OFF + 2));
    printf ("  geometry: sctt=%u trkc=%u cyld=%u   sctg=%u ngrp=%u fsct=%u\n",
            rd16 (hdr + QWA_SCTT_OFF), rd16 (hdr + QWA_TRKC_OFF), rd16 (hdr + QWA_CYLD_OFF),
            rd16 (hdr + QWA_SCTG_OFF), rd16 (hdr + QWA_NGRP_OFF), rd32 (hdr + QWA_FSCT_OFF));
    if (rd16 (hdr + QWA_SCTT_OFF) == 0)
        printf ("  geometry is 0/0 (QPC2/Q68 style) -- MUST be patched for native QWA; "
                "cardbuild does this\n");
}

static int cmd_info (int ac, char **av)
{
    const char *path;
    int fd;
    unsigned char raw[512];
    ROOT r;

    if (ac < 1) { fprintf (stderr, "usage: cardinfo FILE\n"); return 1; }
    path = av[0];
    fd = open (path, O_RDONLY);
    if (fd < 0) { perror (path); return 1; }
    if (read_sectors (fd, 0, raw, 1) != 0) { perror ("read"); close (fd); return 1; }

    if (parse_root_sector (raw, &r)) {
        int i;
        printf ("Q40/Q60 card image: %s\n", path);
        printf ("partition table order: %s\n", ordername (r.order));
        printf ("disk size: %u sectors (~%.1f MiB)\n", r.hdsiz, r.hdsiz / 2048.0);
        for (i = 0; i < r.n; i++) {
            const ATARI_PART *p = &r.parts[i];
            unsigned char hdr[512];
            int cs = 0, ok;
            printf ("partition %d (slot %d): type=%.3s start=%u size=%u end=%u%s\n",
                    i + 1, p->slot, p->type, p->start, p->size, p->start + p->size - 1,
                    (p->start + p->size) > r.hdsiz ? "   [OVERFLOWS DISK]" : "");
            if (memcmp (p->type, "QWA", 3) == 0) {
                ok = qwa_read_hdr (fd, p->start, hdr, &cs);
                if (ok) {
                    printf ("       QLWA "); info_qlwa_hdr (hdr, cs);
                } else {
                    printf ("       (no recognisable QLWA signature at start sector)\n");
                }
            }
        }
        close (fd);
        return 0;
    }

    /* Not a card: treat path as a bare big-endian .WIN file. */
    {
        unsigned char hdr[512];
        int cs = 0;
        if (!qwa_read_hdr (fd, 0, hdr, &cs) || memcmp (hdr, "QLWA", 4)) {
            fprintf (stderr, "%s: not a QXL/.WIN file or Q40 card image\n", path);
            close (fd); return 1;
        }
        printf ("QXL/WIN file: %s\n", path);
        info_qlwa_hdr (hdr, 0);
        close (fd);
        return 0;
    }
}

static int cmd_fixgeom (int ac, char **av)
{
    const char *fn;
    uint16_t spt, heads;
    int fd;
    unsigned char hdr[512];

    if (ac < 3) { fprintf (stderr, "usage: cardfixgeom WIN spt heads\n"); return 1; }
    fn = av[0]; spt = (uint16_t) strtoul (av[1], NULL, 0); heads = (uint16_t) strtoul (av[2], NULL, 0);
    fd = open (fn, O_RDWR);
    if (fd < 0) { perror (fn); return 1; }
    if (read_sectors (fd, 0, hdr, 1) != 0 || memcmp (hdr, "QLWA", 4)) {
        fprintf (stderr, "%s: not a QXL/.WIN file\n", fn); close (fd); return 1;
    }
    qwa_patch_geom (hdr, spt, heads);
    if (write_sectors (fd, 0, hdr, 1) != 0) { perror ("write"); close (fd); return 1; }
    close (fd);
    printf ("patched sctt=%u trkc=%u cyld=%u\n", spt, heads, rd16 (hdr + QWA_CYLD_OFF));
    return 0;
}

/* ---------------- cardbuild ---------------- */

static int cmd_build (int ac, char **av)
{
    const char *out = NULL, *geom_from = NULL;
    uint32_t card_sectors = 0, first_sector = 1;
    uint16_t heads = 16, spt = 63;
    int have_geom = 0, content_swap = 1, table_swap = 1, keep_geom = 0;
    int target = 1, yes = 0;
    char **layout = malloc (sizeof (char *) * (ac + 1));
    int nlay = 0, i, fd, rc = 1;
    ATARI_PART parts[MAX_PARTS];
    uint32_t cur;

    for (i = 0; i < ac; i++) {
        char *a = av[i];
        if (strcmp (a, "-o") == 0 && i + 1 < ac) out = av[++i];
        else if (strcmp (a, "--geometry-from") == 0 && i + 1 < ac) geom_from = av[++i];
        else if (strcmp (a, "--heads") == 0 && i + 1 < ac) { heads = (uint16_t)strtoul (av[++i], NULL, 0); have_geom = 1; }
        else if (strcmp (a, "--sectors") == 0 && i + 1 < ac) { spt = (uint16_t)strtoul (av[++i], NULL, 0); have_geom = 1; }
        else if (strcmp (a, "--card-sectors") == 0 && i + 1 < ac) card_sectors = (uint32_t)strtoul (av[++i], NULL, 0);
        else if (strcmp (a, "--first-sector") == 0 && i + 1 < ac) first_sector = (uint32_t)strtoul (av[++i], NULL, 0);
        else if (strcmp (a, "--target") == 0 && i + 1 < ac) target = atoi (av[++i]);
        else if (strcmp (a, "--no-swap") == 0) content_swap = 0;
        else if (strcmp (a, "--no-table-swap") == 0) table_swap = 0;
        else if (strcmp (a, "--keep-geometry") == 0) keep_geom = 1;
        else if (strcmp (a, "--yes") == 0) yes = 1;
        else layout[nlay++] = a;
    }

    if (nlay < 1 || !out) {
        fprintf (stderr, "usage: cardbuild LAYOUT... -o OUT\n"
                 "  LAYOUT = FILE.WIN | blank:MiB   "
                 "options: --geometry-from --heads --sectors --card-sectors "
                 "--first-sector --target --no-swap --no-table-swap --keep-geometry --yes\n");
        goto done;
    }

    if (geom_from) {
        int order, cswap;
        if (harvest_from_card (geom_from, &spt, &heads, &order, &cswap) != 0) goto done;
        have_geom = 1;
        content_swap = cswap;                 /* reproduce harvested content order */
        table_swap   = (order == ORDER_SWAPPED);   /* reproduce harvested table order */
    }
    if (!have_geom) { spt = 63; heads = 16; }

    if (strncmp (out, "/dev/", 5) == 0 && !yes) {
        fprintf (stderr, "writing to a device %s requires --yes\n", out); goto done;
    }

    /* lay out partitions */
    memset (parts, 0, sizeof (parts));
    cur = first_sector;
    for (i = 0; i < nlay; i++) {
        long sz;
        parts[i].slot = i + 1;
        if (strncmp (layout[i], "blank:", 6) == 0) {
            sz = (long) strtol (layout[i] + 6, NULL, 0);
            parts[i].size = (uint32_t)(sz * 2048);
            parts[i].flag = 0x01; memcpy (parts[i].type, "QWA", 3);
            parts[i].start = cur; cur += parts[i].size;
        } else {
            if ((sz = file_size (layout[i])) < 0) { perror (layout[i]); goto done; }
            if (sz % 512) { fprintf (stderr, "%s: not sector-aligned\n", layout[i]); goto done; }
            parts[i].size = (uint32_t)(sz / 512);
            parts[i].flag = 0x01; memcpy (parts[i].type, "QWA", 3);
            parts[i].start = cur; cur += parts[i].size;
        }
    }
    if (nlay > MAX_PARTS) { fprintf (stderr, "error: >4 partitions\n"); goto done; }
    if (!card_sectors) card_sectors = cur;
    if (cur > card_sectors) {
        fprintf (stderr, "error: partitions need %u sectors but card is %u\n", cur, card_sectors);
        goto done;
    }

    /* geometry-cylinder guard: dry-run the patch on each real .WIN */
    if (!keep_geom) {
        for (i = 0; i < nlay; i++) {
            unsigned char hdr[512];
            if (strncmp (layout[i], "blank:", 6) == 0) continue;
            fd = open (layout[i], O_RDONLY);
            if (fd < 0 || read_sectors (fd, 0, hdr, 1) != 0) { perror (layout[i]); if (fd >= 0) close (fd); goto done; }
            close (fd);
            if (memcmp (hdr, "QLWA", 4)) { fprintf (stderr, "%s: not a QLWA .WIN\n", layout[i]); goto done; }
            if (qwa_patch_geom (hdr, spt, heads)) {
                fprintf (stderr, "error: >65535 cylinders\n"); goto done;
            }
        }
    }

    fd = open (out, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror (out); goto done; }
    if (ftruncate (fd, (off_t) card_sectors * SECTOR) != 0) { perror ("ftruncate"); close (fd); goto done; }

    {
        unsigned char s0[512];
        build_root_sector (s0, parts, nlay, card_sectors);
        if (table_swap) swab16buf (s0, 512);
        if (write_sectors (fd, 0, s0, 1) != 0) { perror ("write s0"); close (fd); goto done; }
    }

    for (i = 0; i < nlay; i++) {
        unsigned char *buf;
        uint32_t sec = parts[i].size;
        if (strncmp (layout[i], "blank:", 6) == 0) {
            buf = calloc (1, (size_t) sec * SECTOR);
            if (!buf) { perror ("calloc"); close (fd); goto done; }
        } else {
            long sz = file_size (layout[i]);
            int src = open (layout[i], O_RDONLY);
            buf = malloc ((size_t) sz);
            if (!buf || src < 0) { perror ("buf/src"); if (src >= 0) close (src); free (buf); close (fd); goto done; }
            if (read (src, buf, (size_t) sz) != (ssize_t) sz) { perror ("read"); close (src); free (buf); close (fd); goto done; }
            close (src);
        }
        if (memcmp (buf, "QLWA", 4) == 0) {
            if (!keep_geom) qwa_patch_geom (buf, spt, heads);
            wr32 (buf + QWA_FSCT_OFF, parts[i].start);   /* first sector, cosmetic */
        }
        if (content_swap) swab16buf (buf, (size_t) sec * SECTOR);
        if (write_sectors (fd, parts[i].start, buf, sec) != 0) { perror ("write part"); free (buf); close (fd); goto done; }
        free (buf);
        printf ("WIN_DRIVE %d,%d,0,%d\n", i + 1, target, i + 1);
        printf ("  wrote %s -> sector %u (size %u)\n", layout[i], parts[i].start, sec);
    }

    close (fd);
    printf ("card built: %s  (%u sectors, %u partitions)\n", out, card_sectors, nlay);
    rc = 0;
done:
    free (layout);
    return rc;
}

/* ---------------- cardextract ---------------- */

static int cmd_extract (int ac, char **av)
{
    const char *image = NULL, *outfile = NULL, *outdir = ".";
    int sel = 0, i, fd, rc = 1;
    unsigned char raw[512];
    ROOT r;
    char namebuf[256];

    for (i = 0; i < ac; i++) {
        char *a = av[i];
        if (strcmp (a, "--partition") == 0 && i + 1 < ac) sel = atoi (av[++i]);
        else if (strcmp (a, "-o") == 0 && i + 1 < ac) outfile = av[++i];
        else if (strcmp (a, "-d") == 0 && i + 1 < ac) outdir = av[++i];
        else image = a;
    }
    if (!image) { fprintf (stderr, "usage: cardextract IMAGE [--partition N] [-o FILE | -d DIR]\n"); return 1; }
    if (outfile && sel == 0) { fprintf (stderr, "-o requires --partition N\n"); return 1; }

    fd = open (image, O_RDONLY);
    if (fd < 0) { perror (image); return 1; }
    if (read_sectors (fd, 0, raw, 1) != 0 || !parse_root_sector (raw, &r)) {
        fprintf (stderr, "%s: not a Q40 card image\n", image); close (fd); return 1;
    }

    for (i = 0; i < r.n; i++) {
        const ATARI_PART *p = &r.parts[i];
        const char *name;
        unsigned char *buf;
        int out;
        if (sel && p->slot != sel) continue;
        if (memcmp (p->type, "QWA", 3)) continue;

        if (outfile) name = outfile;
        else { snprintf (namebuf, sizeof (namebuf), "%s/part%d.win", outdir, p->slot); name = namebuf; }

        buf = malloc ((size_t) p->size * SECTOR);
        if (!buf) { perror ("malloc"); close (fd); return 1; }
        if (read_sectors (fd, p->start, buf, p->size) != 0) { perror ("read"); free (buf); close (fd); return 1; }
        swab16buf (buf, (size_t) p->size * SECTOR);
        out = open (name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (out < 0) { perror (name); free (buf); close (fd); return 1; }
        if (write (out, buf, (size_t) p->size * SECTOR) != (ssize_t) p->size * SECTOR) {
            perror ("write"); close (out); free (buf); close (fd); return 1;
        }
        close (out); free (buf);
        printf ("extracted partition %d -> %s\n", i + 1, name);
        rc = 0;
        if (sel) break;
    }
    if (sel && rc) fprintf (stderr, "partition %d not found\n", sel);
    close (fd);
    return rc;
}

/* ---------------- carddrop / cardappend / cardclonetable ---------------- */

static int cmd_drop (int ac, char **av)
{
    const char *dev = NULL; int sel = 0, yes = 0, i;
    int fd;
    unsigned char raw[512], s0[512];
    ROOT r;
    for (i = 0; i < ac; i++) {
        if (strcmp (av[i], "--partition") == 0 && i + 1 < ac) sel = atoi (av[++i]);
        else if (strcmp (av[i], "--yes") == 0) yes = 1;
        else dev = av[i];
    }
    if (!dev || !sel) { fprintf (stderr, "usage: carddrop DEVICE --partition N --yes\n"); return 1; }
    if (strncmp (dev, "/dev/", 5) == 0 && !yes) { fprintf (stderr, "device %s needs --yes\n", dev); return 1; }
    fd = open (dev, O_RDWR);
    if (fd < 0) { perror (dev); return 1; }
    if (read_sectors (fd, 0, raw, 1) != 0 || !parse_root_sector (raw, &r)) {
        fprintf (stderr, "%s: not a Q40 card\n", dev); close (fd); return 1; }
    memcpy (s0, raw, 512);
    if (sel < 1 || sel > MAX_PARTS) { fprintf (stderr, "bad partition %d\n", sel); close (fd); return 1; }
    memset (s0 + ART_P0_OFF + (sel - 1) * 12, 0, 12);
    if (r.n > 0 && sel <= r.n && sel != r.n)
        fprintf (stderr, "notice: dropping non-final partition %d leaves a hole append won't reuse\n", sel);
    if (write_sectors (fd, 0, s0, 1) != 0) { perror ("write"); close (fd); return 1; }
    close (fd);
    printf ("dropped partition %d\n", sel);
    return 0;
}

static int cmd_append (int ac, char **av)
{
    const char *dev = NULL, *win = NULL;
    int slot = 0, yes = 0, content_swap = -1, table_swap = -1, keep_geom = 0;
    uint32_t card_sectors = 0, start = 0; int i, card_given = 0;
    int fd = -1, rc = 1;
    ROOT r;
    unsigned char raw[512];

    for (i = 0; i < ac; i++) {
        char *a = av[i];
        if (strcmp (a, "--win") == 0 && i + 1 < ac) win = av[++i];
        else if (strcmp (a, "--slot") == 0 && i + 1 < ac) slot = atoi (av[++i]);
        else if (strcmp (a, "--card-sectors") == 0 && i + 1 < ac) { card_sectors = (uint32_t)strtoul (av[++i], NULL, 0); card_given = 1; }
        else if (strcmp (a, "--no-swap") == 0) content_swap = 0;
        else if (strcmp (a, "--keep-geometry") == 0) keep_geom = 1;
        else if (strcmp (a, "--yes") == 0) yes = 1;
        else dev = a;
    }
    if (!dev || !win) { fprintf (stderr, "usage: cardappend DEVICE --win F.WIN [--slot] --yes\n"); return 1; }
    if (strncmp (dev, "/dev/", 5) == 0 && !yes) { fprintf (stderr, "device %s needs --yes\n", dev); return 1; }

    fd = open (dev, O_RDWR);
    if (fd < 0) { perror (dev); return 1; }
    if (read_sectors (fd, 0, raw, 1) != 0 || !parse_root_sector (raw, &r)) {
        fprintf (stderr, "%s: not a Q40 card\n", dev); close (fd); return 1; }

    if (!card_sectors) card_sectors = dev_sectors (fd);

    {
        long sz = file_size (win);
        unsigned char *buf;
        uint32_t sec, pend;
        int slotno, k, active[5];
        unsigned char bev[512];

        if (sz < 0 || sz % 512) { fprintf (stderr, "%s: bad size\n", win); close (fd); return 1; }
        sec = (uint32_t)(sz / 512);

        /* work on the big-endian view of the current table */
        memcpy (bev, raw, 512);
        if (r.order == ORDER_SWAPPED) swab16buf (bev, 512);
        for (k = 0; k < 5; k++) active[k] = 0;
        for (k = 0; k < r.n; k++) {
            const unsigned char *e = bev + ART_P0_OFF + k * 12;
            if (e[0] & 1) active[k + 1] = 1;
        }

        /* choose slot */
        slotno = slot;
        if (!slotno)
            for (k = 1; k <= MAX_PARTS; k++) if (!active[k]) { slotno = k; break; }
        if (!slotno) { fprintf (stderr, "all four slots full\n"); close (fd); return 1; }

        /* start = after the last used partition (append into tail space) */
        {
            uint32_t maxend = 0;
            for (k = 0; k < r.n; k++) {
                const unsigned char *e = bev + ART_P0_OFF + k * 12;
                if (e[0] & 1) {
                    uint32_t s = rd32 (e + 4), z = rd32 (e + 8);
                    if (s + z > maxend) maxend = s + z;
                }
            }
            start = maxend;
        }
        pend = start + sec;
        if (card_given && pend > card_sectors) {
            fprintf (stderr, "error: partition overruns card (%u > %u)\n", pend, card_sectors);
            close (fd); return 1;
        }
        if (!card_given) {
            /* auto-size: extend the file to fit the appended partition */
            card_sectors = pend;
            if (ftruncate (fd, (off_t) card_sectors * SECTOR) != 0) {
                perror ("ftruncate"); close (fd); return 1;
            }
        }
        if (content_swap < 0) content_swap = 1;
        if (table_swap < 0) table_swap = (r.order == ORDER_SWAPPED);

        buf = malloc ((size_t) sz);
        {
            int src = open (win, O_RDONLY);
            if (src < 0 || read (src, buf, (size_t) sz) != (ssize_t) sz) {
                perror (win); if (src >= 0) close (src); free (buf); close (fd); return 1;
            }
            close (src);
        }
        if (memcmp (buf, "QLWA", 4) == 0) {
            if (!keep_geom) qwa_patch_geom (buf, 63, 16);
            wr32 (buf + QWA_FSCT_OFF, start);
        }
        if (content_swap) swab16buf (buf, (size_t) sz);

        /* payload FIRST, verify, THEN sector 0 */
        if (write_sectors (fd, start, buf, sec) != 0) { perror ("write payload"); free (buf); close (fd); return 1; }
        {
            unsigned char *chk = malloc ((size_t) sz);
            if (read_sectors (fd, start, chk, sec) != 0 || memcmp (chk, buf, (size_t) sz) != 0) {
                fprintf (stderr, "write verify failed\n"); free (chk); free (buf); close (fd); return 1;
            }
            free (chk);
        }
        free (buf);

        {
            unsigned char s0[512];
            ATARI_PART pp[MAX_PARTS];
            memset (pp, 0, sizeof (pp));
            for (k = 0; k < r.n; k++) {
                const unsigned char *e = bev + ART_P0_OFF + (r.parts[k].slot - 1) * 12;
                if (e[0] & 1) {
                    int idx = r.parts[k].slot - 1;
                    pp[idx].flag = e[0]; memcpy (pp[idx].type, e + 1, 3);
                    pp[idx].start = rd32 (e + 4); pp[idx].size = rd32 (e + 8);
                    pp[idx].slot = r.parts[k].slot;
                }
            }
            pp[slotno - 1].flag = 0x01; memcpy (pp[slotno - 1].type, "QWA", 3);
            pp[slotno - 1].start = start; pp[slotno - 1].size = sec;
            pp[slotno - 1].slot = slotno;
            build_root_sector (s0, pp, MAX_PARTS, card_sectors);
            if (table_swap) swab16buf (s0, 512);
            if (write_sectors (fd, 0, s0, 1) != 0) { perror ("write s0"); close (fd); return 1; }
            printf ("appended %s -> slot %d at sector %u\n", win, slotno, start);
        }
        rc = 0;
    }
    if (fd >= 0) close (fd);
    return rc;
}

static int cmd_clonetable (int ac, char **av)
{
    const char *src = NULL, *dst = NULL; int yes = 0, i;
    int sfd, dfd;
    unsigned char s0[512]; ROOT r;
    for (i = 0; i < ac; i++) {
        if (strcmp (av[i], "--yes") == 0) yes = 1;
        else if (!src) src = av[i]; else dst = av[i];
    }
    if (!src || !dst) { fprintf (stderr, "usage: cardclonetable SRC DST --yes\n"); return 1; }
    if (strncmp (dst, "/dev/", 5) == 0 && !yes) { fprintf (stderr, "device %s needs --yes\n", dst); return 1; }
    sfd = open (src, O_RDONLY); dfd = open (dst, O_WRONLY | O_CREAT, 0666);
    if (sfd < 0 || dfd < 0 || read_sectors (sfd, 0, s0, 1) != 0 || !parse_root_sector (s0, &r)) {
        fprintf (stderr, "clone-table failed\n"); if (sfd >= 0) close (sfd); if (dfd >= 0) close (dfd); return 1; }
    if (write_sectors (dfd, 0, s0, 1) != 0) { perror ("write"); close (sfd); close (dfd); return 1; }
    printf ("cloned table (order=%s); highest referenced end=%u\n",
            ordername (r.order), r.parts[r.n - 1].start + r.parts[r.n - 1].size);
    close (sfd); close (dfd);
    return 0;
}

/* ---------------- dispatch ---------------- */

static int cmd_help (void)
{
    printf ("Q40/Q60 card commands:\n"
            "  cardswap IN OUT            swap every 16-bit word of a file\n"
            "  cardinfo FILE              inspect a card image or a .WIN file\n"
            "  cardfixgeom WIN spt heads  patch CHS geometry into a .WIN\n"
            "  cardbuild LAYOUT... -o OUT build a card image from .WIN files\n"
            "  cardextract IMAGE          extract big-endian .WIN partitions\n"
            "  cardappend DEVICE --win F  append a .WIN into free tail space\n"
            "  carddrop DEVICE --part N   clear a partition entry\n"
            "  cardclonetable SRC DST     clone sector 0 between cards\n");
    return 1;
}

int qxcard_dispatch (int ac, char **av)
{
    if (ac < 1 || !av[0]) return -1;   /* nothing to dispatch */
    if (strcmp (av[0], "cardswap") == 0) return cmd_swap (ac - 1, av + 1);
    if (strcmp (av[0], "cardinfo") == 0) return cmd_info (ac - 1, av + 1);
    if (strcmp (av[0], "cardfixgeom") == 0) return cmd_fixgeom (ac - 1, av + 1);
    if (strcmp (av[0], "cardbuild") == 0) return cmd_build (ac - 1, av + 1);
    if (strcmp (av[0], "cardextract") == 0) return cmd_extract (ac - 1, av + 1);
    if (strcmp (av[0], "carddrop") == 0) return cmd_drop (ac - 1, av + 1);
    if (strcmp (av[0], "cardappend") == 0) return cmd_append (ac - 1, av + 1);
    if (strcmp (av[0], "cardclonetable") == 0) return cmd_clonetable (ac - 1, av + 1);
    if (strcmp (av[0], "cardhelp") == 0) return cmd_help ();
    return -1;                         /* not a card command */
}
