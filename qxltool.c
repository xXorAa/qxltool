/*--------------------------------------------------------------.
| qxltool.c : Access QXL.WIN files from other operating systems |
|                                                               |
| (c) Jonathan Hudson, 1998 - 1999                     		|
| No Warranty                                                   |
`--------------------------------------------------------------*/
#define _GNU_SOURCE


#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <stddef.h>
#include <sys/types.h>

#if defined(__unix__) || defined(__APPLE__)
# include <fnmatch.h>
# define FNMATCH fnmatch
# define FNEQUAL 0
#elif defined(__WINNT) || defined(__GO32)
# ifdef HAVE_GETOPT_H
#  include <getopt.h>
# else
#  include <getopt.c>
# endif
# ifndef HAVE_FNMATCH 
#  include <fnmatch.c>
# endif
# define FNMATCH(a,b,c) fnmatch(a,b)
# define FNEQUAL 1
#elif defined(__QDOS__)
# include <qdos.h>
/* Sad but true, this is completed fucked up (again) */
# define FNMATCH(a,b,c) fnmatch(b,a)
# define FNEQUAL 1
#else
# error "Unknown system type"
#endif

#include <zlib.h>

#include "qxltool.h"
#include "q40card.h"

#define LINSIZ (256)

static int help (QXL *, short mflag, char **av);
static QLDIR *readqldir (QXL *, QLDIR * cur);
static u_short nextcluster (QXL *, u_short, short);
static void updatecluster (QXL *, u_short, u_short);

#ifndef HAVE_STPCPY
/* Must not be called stpcpy: GCC recognises strcpy+strlen as the stpcpy
   idiom and turns the body into a call to stpcpy, i.e. itself, giving an
   infinite loop (seen with mingw, which lacks stpcpy). */
static char *qxl_stpcpy (char *s1, const char *s2)
{
    size_t n = strlen (s2);
    memcpy (s1, s2, n + 1);
    return s1 + n;
}
# define stpcpy qxl_stpcpy
#endif


#ifndef WORDS_BIGENDIAN
u_short swapword (u_short val)
{
    return (u_short) (val << 8) + (val >> 8);
}

uint32_t swaplong (uint32_t val)
{
    return (uint32_t) (((uint32_t) swapword (val & 0xFFFF) << 16) |
		       (uint32_t) swapword (val >> 16));
}

# define SW(a,c) (a)->c = swapword((a)->c)
# define SL(a,c) (a)->c = swaplong((a)->c)
#else
u_short swapword (u_short val)
{
    return val;
}

uint32_t swaplong (uint32_t val)
{
    return val;
}

# define SW(a,c)
# define SL(a,c)
#endif

#ifdef __QDOS__
char _prog_name[] = "qxltool";
char _copyright[] = "(c) Jonathan R Hudson 1998";
char _version[] = VERSION;
char *_endmsg = NULL;

#include <fcntl.h>
#include <qdos.h>
#include <qptr.h>
void myconsetup (chanid_t ch, WINDOWDEF_t * w);

struct WINDOWDEF _condetails =
{6, 1, 0, 4, 512, 256, 0, 0};
void (*_consetup) () = myconsetup;

void myconsetup (chanid_t ch, WINDOWDEF_t * w)
{
    char *p;
    int ns, q1, q2, q3, q4;

    iop_flim (ch, -1, (WM_wsiz_t *) & w->width);
    if ((p = getenv ("QXL_CON")))
    {
	ns = sscanf (p, "%dx%da%dx%d", &q1, &q2, &q3, &q4);
	switch (ns)
	{
	    case 4:
		w->y_origin = q4;
	    case 3:
		w->x_origin = q3;
	    case 2:
		w->height = q2;
	    case 1:
		w->width = q1;
		break;
	}
    }
    consetup_qpac (ch, w);
}

static char *lastdir (char *ws)
{
    char *p;
    char *q = ws;
    struct stat s;

    for (p = ws; *p; p++)
    {
	if (*p == '_')
	{
	    char c;

	    p++;
	    c = *p;
	    *p = 0;
	    if (stat (ws, &s) == 0 && S_ISDIR (s.st_mode))
	    {
		q = p;
	    }
	    *p = c;
	}
    }
    return q;
}

#endif

static char *strim (char *s)
{
    char *p;
    short n;

    n = strlen (s);
    p = s + n;
    while ((u_char) *--p <= ' ' && n--)
    {
	*p = '\0';
    }
    return s;
}

static void h_normalise (HEADER * h)
{
    SW (h, namlen);
    SW (h, rand);
    SW (h, access);
    SW (h, sectc);
    SW (h, total);
    SW (h, free);
    SW (h, direct);
    SW (h, formatted);
    SW (h, first);
    SL (h, dlen);
    SW (h, map[0]);
}

/* ---- Partition-window I/O ------------------------------------------------
   All raw filesystem I/O goes through these.  They (a) add qxl->base, the
   byte offset of the filesystem within the underlying image, and (b) apply
   the 16-bit byte transpose if the media is byte-swapped (Q40 card).  For a
   plain .WIN file base==0 and swap==0, so these reduce to the old
   lseek+read/write and existing behaviour is unchanged.
   Note: offsets are relative to the filesystem start; qxl->h.sectc is the
   sector (block) size in bytes, and the QLWA header/group map are at
   filesystem offset 0.  The transpose is only valid on even lengths and
   even file offsets, which every QLWA access satisfies. */

static void qbswap16 (void *buf, size_t n)
{
    unsigned char *p = buf; size_t i;
    for (i = 0; i + 1 < n; i += 2) {
        unsigned char t = p[i]; p[i] = p[i+1]; p[i+1] = t;
    }
}

static ssize_t qxl_read (QXL * qxl, off_t off, void *buf, size_t n)
{
    ssize_t r;
    if (lseek (qxl->fd, qxl->base + off, SEEK_SET) < 0)
        return -1;
    if ((r = read (qxl->fd, buf, n)) < 0)
        return -1;
    if (qxl->swap && (size_t) r == n)
        qbswap16 (buf, n);
    return r;
}

static ssize_t qxl_write (QXL * qxl, off_t off, const void *buf, size_t n)
{
    ssize_t w;
    if (qxl->swap) {
        unsigned char *tmp = alloca (n ? n : 1);
        memcpy (tmp, buf, n);
        qbswap16 (tmp, n);
        if (lseek (qxl->fd, qxl->base + off, SEEK_SET) < 0)
            return -1;
        w = write (qxl->fd, tmp, n);
    } else {
        if (lseek (qxl->fd, qxl->base + off, SEEK_SET) < 0)
            return -1;
        w = write (qxl->fd, buf, n);
    }
    return w;
}

static void writeheader (QXL * qxl)
{
    HEADER h;
    int i;
    
    qxl->h.access++;
    h = qxl->h;
    h.sectc >>= 9;
    for(i = 0; i < sizeof(h.name); i++)
    {
        if(h.name[i] == 0) h.name[i] = ' ';
    }
    h_normalise (&h);
    qxl_write (qxl, 0, &h, sizeof (HEADER));
    strim ((char *) h.name); 
}

static void readheader (QXL * qxl)
{
    qxl_read (qxl, 0, &qxl->h, sizeof (HEADER));
    h_normalise (&qxl->h);
    if (memcmp (qxl->h.id, "QLWA", 4) == 0)
    {
	/* Now create a fake directory record for the root directory */
	qxl->root.length = qxl->h.dlen;
	qxl->root.map = qxl->h.direct;
	qxl->root.type = 0xff;
	*qxl->root.name = 0;
	qxl->root.nlen = 0;
	qxl->h.sectc <<= 9;
	strim ((char *) qxl->h.name);
    }
    else
    {
	fputs ("Not a QXL file\n", stderr);
	exit (0);
    }
}

static void d_normalise (QLDIR * d)
{
    SL (d, length);
    SW (d, type);
    SL (d, data);
    SW (d, nlen);
    SL (d, date);
    SW (d, map);
}

static intptr_t d_list (QXL * qxl, QLDIR * d, void *a, void *pb, u_short c)
{
    struct tm *tm;
    char s[128];
    size_t n;
    char *p;
    int flen, m;
    
    flen = d->length - sizeof (QLDIR);

    if(flen >=0)
    {
        p = (char *) d->name;
        m = d->nlen;
        if (d->length==0 && m == 0)
        {
            m=strlen(p);
        }
        if (qxl->curd.nlen)
        {
            p += qxl->curd.nlen + 1;
            m -= (qxl->curd.nlen + 1);
        }
        
        n = sprintf (s, "%-36.*s %s %8d ", m, p,
                     (d->length==0 ? "DELETED" : ""), flen);
    
        if (d->type != 0xff)
        {
            size_t k;
            
            time_t t = (time_t) d->date;
            tm = localtime (&t);
#if defined (HAVE_STRFTIME)
            k = strftime (s + n, sizeof (s) - n,
                          "%d-%m-%y %H:%M:%S ", tm);
#else
            k = sprintf (s + n, "%02d-%02d-%02d %02d:%02d:%02d ",
                         tm->tm_mday, tm->tm_mon + 1, tm->tm_year % 100,
                         tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif
            n += k;
        }
        else
        {
            strcpy (s + n, "      (Directory) ");
            n += 18;
        }
        
        
        if (d->type == 1)
        {
            sprintf (s + n, "(%" PRIu32 ") ", d->data);
        }
        fputs (s, qxl->fp);
        fputc ('\n', qxl->fp);
    }
    return 0;
}

/* quick and dirty version for now */

#if !defined (FNM_CASEFOLD)
# define FNM_CASEFOLD 0
#endif

static intptr_t qfindwild (QXL * qxl, QLDIR * d, void *fn, void *pft, u_short flag)
{
    QLDIR *pd = NULL;
    u_short ft = *(u_short *)pft;
    
    if (ft == 0 || (ft == d->type))
    {
	char fnm[40];

	memcpy (fnm, (char *) d->name, d->nlen);
	*(fnm + d->nlen) = 0;

	if (FNEQUAL == FNMATCH (fn, fnm, FNM_CASEFOLD))
	{
	    pd = malloc (sizeof (QLDIR));
	    *pd = *d;
	}
    }
    return (intptr_t) pd;
}

static intptr_t qfindbest (QXL * qxl, QLDIR * d, void *fn, void *pft, u_short flag)
{
    size_t wlen = strlen (fn);
    u_short ft = *(u_short *)pft;

    if ((ft == d->type))
    {
	if ((d->nlen > qxl->lastd.nlen) && (d->nlen <= wlen) &&
	    memcmp ((char *) d->name, (char *)fn, d->nlen) == 0
            && *((char *)fn+d->nlen) == '_')
	{
	    memcpy (&qxl->lastd, d, sizeof (QLDIR));
	}
    }
    return 0;
}

static intptr_t qmatch (QXL * qxl, QLDIR * d, void *fn, void *pft, u_short flag)
{
    QLDIR *pd = NULL;
    size_t wlen = strlen ((char *)fn);
    u_short ft = *(u_short *)pft;

    if (ft == 0 || (ft == d->type))
    {
	if (d->nlen == wlen && memcmp ((char *) d->name, (char *)fn,
                                            wlen) == 0)
	{
	    pd = malloc (sizeof (QLDIR));
	    *pd = *d;
	}
    }
    return (intptr_t) pd;
}

static intptr_t processdir (QXL * qxl, QLDIR * buf, QLDIR * cur,
			    PCALLBACK func, char *fn, void *pft, short flag)
{
    QLDIR *d, *last, *z;
    intptr_t res = 0;
    last = buf + cur->length / sizeof (QLDIR);
    
    for (d = buf + 1; d < last; d++)
    {
	d_normalise (d);
	d->date += QL2UNIX;
	if ((res = func (qxl, d, fn, pft, flag)) == 0)
	{
	    if (d->type == 0xff && (flag & DO_RECURSE))
	    {
		z = readqldir (qxl, d);
                qxl->tmpd = *d;
		res = processdir (qxl, z, d, func, fn, pft, flag);
                qxl->tmpd = *cur;    
		free (z);
	    }
	}
	else
	    qxl->lastd = *cur;
	if (res)
	    break;
    }
    return res;
}

static u_short getcluster (QXL * qxl, u_short o)
{
    u_short c, n;
    qxl->h.free--;
    c = nextcluster (qxl, qxl->h.first, 0);
    n = qxl->h.first;
    if (o)
	updatecluster (qxl, o, n);
    updatecluster (qxl, n, 0);
    qxl->h.first = c;
    return n;
}

static u_short nextcluster (QXL * qxl, u_short c, short flag)
{
    u_long newoff;
    u_short d;

    newoff = c * 2 + offsetof (HEADER, map);
    qxl_read (qxl, newoff, &d, sizeof (short));
    d = swapword (d);
    if (d == 0 && flag)
    {
	if (qxl->h.free)
	{
	    d = getcluster (qxl, c);
	}
	else
	{
	    fputs ("Disk is full!\n", stderr);
	    exit (0);
	}
    }
    return d;
}

static QLDIR *readqldir (QXL * qxl, QLDIR * cur)
{
    u_char *buf, *p;
    int dlen, rlen;
    u_short fcluster;

    dlen = cur->length;
    p = buf = malloc (dlen);

    for (fcluster = cur->map; (fcluster && dlen);)
    {
	if (dlen < qxl->h.sectc)
	    rlen = dlen;
	else
	    rlen = qxl->h.sectc;
	qxl_read (qxl, fcluster * qxl->h.sectc, p, rlen);
	p += rlen;
	dlen -= rlen;
	fcluster = nextcluster (qxl, fcluster, 0);
    }
    return (QLDIR *) buf;
}

static void writeqldir (QXL * qxl, QLDIR * cur, QLDIR * buf)
{
    u_char *p;
    int dlen, rlen;
    u_short clu;
    u_short ifree;

    dlen = cur->length;
    p = (u_char *) buf;
    ifree = qxl->h.free;

    for (clu = cur->map; dlen;)
    {
	if (dlen < qxl->h.sectc)
	    rlen = dlen;
	else
	    rlen = qxl->h.sectc;
	qxl_write (qxl, clu * qxl->h.sectc, p, rlen);
	p += rlen;
	dlen -= rlen;
	if (dlen)
	    clu = nextcluster (qxl, clu, 1);
    }
    if (ifree != qxl->h.free)
	writeheader (qxl);
}

static int openqxl (QXL * qxl, char *fn)
{
    /* Support "card.img#N": open AHDI slot N of a Q40 card image. */
    char *hash = strrchr (fn, '#');
    if (hash && hash != fn && isdigit ((unsigned char) hash[1]))
    {
	int slot = atoi (hash + 1);
	char *img = strndup (fn, hash - fn);
	int partfd = qxcard_open_partition (img, slot, qxl->mode, &qxl->base, &qxl->swap);
	if (partfd == -1)
	{
	    free (img);
	    return -1;
	}
	if (partfd > 0)
	{
	    qxl->fd = partfd;
	    readheader (qxl);
	    memcpy (&qxl->curd, &qxl->root, sizeof (QLDIR));
	    strcpy (qxl->fn, fn);
	    free (img);
	    return qxl->fd;
	}
	/* not a card; fall through and open the literal filename */
	free (img);
    }

    if ((qxl->fd = open (fn, qxl->mode)) != -1)
    {
	readheader (qxl);
	memcpy (&qxl->curd, &qxl->root, sizeof (QLDIR));
	strcpy (qxl->fn, fn);
    }
    return qxl->fd;
}

static QLDIR *find_file (QXL * qxl, char *fn, u_short ft, QLDIR * cur,
			 short flag)
{
    intptr_t res = 0;
    QLDIR *buf;
    char wanted[40];
    int n;
    PCALLBACK cb;
    
    if ((n = cur->nlen))
    {
	memcpy (wanted, (char *) cur->name, n);
	*(wanted + n) = '_';
	n++;
    }
    strcpy (wanted + n, fn);

    cb = (flag & DO_BEST) ? qfindbest : (flag & DO_WILD) ? qfindwild : qmatch;
    buf = readqldir (qxl, cur);
    qxl->tmpd = *buf;    
    res = processdir (qxl, buf, cur, cb, wanted, &ft, flag);
    free (buf);
    return (QLDIR *) res;
}


static int quit (QXL * qxl, short mflag, char **av)
{
    exit (0);
    return 0;
}

static int qcd (QXL * qxl, short mflag, char **av)
{
    QLDIR *d;
    char *p = (av) ? *av : NULL;
    int rv = 0;


    if (p == NULL || *p ==0 ||  strcmp(p,"/") == 0 || strcmp(p,"_") == 0)
    {
        memcpy (&qxl->curd, &qxl->root, sizeof (QLDIR));
    }
    else if(strcmp(p,"..") == 0)
    {
        char foo[QLPATH_MAX+1];
        memcpy(foo, (char *)qxl->curd.name, qxl->curd.nlen);
        *(foo+qxl->curd.nlen) =0;
        d = find_file (qxl, foo, 0xff, &qxl->root, DO_RECURSE);
        memcpy (&qxl->curd, &qxl->lastd, sizeof (QLDIR));
    }
    else if (strcmp(p, ".") != 0)
    {
        if (*(p + strlen (p) - 1) == '_')
        {
            *(p + strlen (p) - 1) = 0;
        }
        
        d = find_file (qxl, p, 0xff, &qxl->curd, DO_RECURSE);
        if (d)
        {
            qxl->curd = *d;
            free (d);
        }
        else
        {
            if((mflag & QX_FAIL_SILENTLY) == 0)
                fprintf (stderr, "Not found %s\n", p);
            rv = -1;
        }
    }
    /* else no op, stay in current */
    return rv;
}

static int qlslr (QXL * qxl, short mflag, char **av)
{
    QLDIR *buf;
    char *cmd = *av;
    u_short ft = 0;
    
    if (cmd && *cmd)
    {
	fputs ("ls[lr] takes no parameters (currently), cd first\n", stderr);
    }
    else
    {
	buf = readqldir (qxl, &qxl->curd);
        qxl->tmpd = *buf;    
	processdir (qxl, buf, &qxl->curd, d_list, NULL, &ft, DO_RECURSE);
	free (buf);
    }
    return 0;
}

static int qls (QXL * qxl, short mflag, char **av)
{
    QLDIR *buf;
    char *cmd = *av;
    u_short ft = 0;
    
    if (cmd && *cmd)
    {
	fputs ("ls[lr] takes no parameters (currently), cd first\n", stderr);
     }
    else
    {
	buf = readqldir (qxl, &qxl->curd);
        qxl->tmpd = *buf;
	processdir (qxl, buf, &qxl->curd, d_list, NULL, &ft, 0);
	free (buf);
    }
    return 0;
}

static int qdump (QXL * qxl, short mflag, char **av)
{
    QLDIR *buf;
    char *cmd = *av;
    u_short ft = 1;
    
    if (cmd && *cmd)
    {
	fputs ("ls[lr] takes no parameters (currently), cd first\n", stderr);
    }
    else
    {
	buf = readqldir (qxl, &qxl->curd);
        qxl->tmpd = *buf;    
	processdir (qxl, buf, &qxl->curd, d_list, NULL, &ft, 1);
	free (buf);
    }
    return 0;
}


static int qpwd (QXL * qxl, short mflag, char **av)
{
    char name[QLPATH_MAX+1];

    if(qxl->curd.nlen)
    {
        memcpy(name, (char *)qxl->curd.name, qxl->curd.nlen);
        *(name+qxl->curd.nlen) = 0;
    }
    else
    {
        strcpy(name, "/");
    }
    fprintf (qxl->fp, "%s\n", name);
    return 0;
}


static void cpfile (QXL * qxl, QLDIR * d)
{
    u_char *buf;
    int dlen, rlen;
    u_short cluster;
    short off = sizeof (QLDIR);

    buf = (u_char *) alloca (qxl->h.sectc);
    dlen = d->length;
    cluster = d->map;

    if (qxl->fp)
    {
	for (cluster = d->map; cluster; cluster = nextcluster (qxl, cluster, 0))
	{
	    if (dlen < qxl->h.sectc)
		rlen = dlen;
	    else
		rlen = qxl->h.sectc;
	    qxl_read (qxl, cluster * qxl->h.sectc, buf, rlen);

	    fwrite (buf + off, 1, rlen - off, qxl->fp);

	    dlen -= rlen;
	    off = 0;
	}
    }

    if (d->type == 1 && qxl->close == 1 && d->data)
    {
#ifndef __QDOS__
	XTCC xtcc;

	fseek (qxl->fp, -8, SEEK_CUR);
	fread (&xtcc, 1, 8, qxl->fp);
	if (memcmp (xtcc.x.xtcc, "XTcc", 4))
	{
	    memcpy (xtcc.x.xtcc, "XTcc", 4);
	    xtcc.dlen = swaplong (d->data);
	    fwrite (&xtcc, 1, 8, qxl->fp);
	}
#else
	{
	    struct qdirect qd;
	    qd.d_datalen = d->data;
	    qd.d_type = 1;
	    qd.d_length = d->length;
	    qd.d_access = 0;
	    fs_heads (fgetchid (qxl->fp), -1, &qd, 14);
	}
#endif
    }
}


static int qcp (QXL * qxl, short mflag, char **av)
{
    QLDIR *d;
    char *p = *av;

    if (p)
    {
	d = find_file (qxl, p, 0, &qxl->curd, DO_RECURSE);
	if (d)
	{
	    cpfile (qxl, d);
	    free (d);
	}
	else
	    fputs ("Not found\n", stderr);
    }
    return p == NULL;
}

static void updatecluster (QXL * qxl, u_short f, u_short c)
{
    c = swapword (c);
    qxl_write (qxl, f * 2 + sizeof (QLDIR), &c, sizeof (short));
}

static void updatefree (QXL * qxl, QLDIR * d)
{
    u_short c, cn, f;

    for (c = d->map; c;)
    {
	cn = nextcluster (qxl, c, 0);
	f = qxl->h.first;
	qxl->h.first = c;
	updatecluster (qxl, c, f);
	c = cn;
	qxl->h.free++;
    }
    writeheader (qxl);
}

static void updatedir (QXL * qxl, QLDIR * d)
{
    QLDIR *e, *f;

    e = readqldir (qxl, &qxl->lastd);
    for (f = e + 1; f < (e + qxl->lastd.length / sizeof (QLDIR)); f++)
    {
	if (d->map == swapword (f->map))
	{
	    memset (f, 0, sizeof (QLDIR));
	    break;
	}
    }
    writeqldir (qxl, &qxl->lastd, e);
    free (e);
}

static void rmfile (QXL * qxl, QLDIR * d)
{
    char fn[40];
    memcpy (fn, d->name, d->nlen);
    *(fn + d->nlen) = 0;
    updatefree (qxl, d);
    updatedir (qxl, d);
}

static int qrm (QXL * qxl, short flag, char **av)
{
    QLDIR *d;
    char *p = *av;

    if (p)
    {
	if ((qxl->mode & O_RDWR) == O_RDWR)
	{
	    d = find_file (qxl, p, 0, &qxl->curd, DO_RECURSE);
	    if (d)
	    {
		if (d->type != 0xff)
		{
		    rmfile (qxl, d);
		    free (d);
		}
		else
		    fputs ("Use rmdir to delete empty directories\n", stderr);
	    }
	    else
		fputs ("Not found\n", stderr);

	}
	else
	    fputs ("readonly file system\n", stderr);
    }

    return p == NULL;
}

static int dircount (QXL * qxl, QLDIR * d)
{
    QLDIR *e, *f;
    int n = 0;

    e = readqldir (qxl, d);
    for (f = e + 1; f < (e + d->length / sizeof (QLDIR)); f++)
    {
	if (f->nlen && swaplong (f->length) >= sizeof (QLDIR))
	{
	    n++;
	}
    }
    free (e);
    return n;
}


static int qrmdir (QXL * qxl, short mflag, char **av)
{
    QLDIR *d;
    char *p = *av;

    if (p)
    {
	d = find_file (qxl, p, 0xff, &qxl->curd, DO_RECURSE);
	if (d)
	{
	    int dn;

	    if ((dn = dircount (qxl, d)) == 0)
	    {
		rmfile (qxl, d);
		free (d);
	    }
	    else
	    {
		fprintf (stderr,
			 "Directory must be empty to delete (%d)\n", dn);
	    }
	}
	else
	    fputs ("Not found\n", stderr);
    }
    return p == NULL;
}

static char **openout (QXL * qxl, char *s, short flag)
{

    char **av, **pav;
    char *r0, *r1, *r2, *p;
    int l, n = 0, nl;

    l = strlen (s);
    pav = av = (char **) malloc (sizeof (char *) * 256);

    if ((r1 = r0 = strpbrk (s, ">|")) != NULL)
    {
	r1++;
	if (*r1 == '>')
	    r1++;
	while (*r1 <= ' ')
	    r1++;
	r2 = r1;
	while (*r2 > ' ')
	    r2++;
	n = r2 - r1;
	p = alloca (n + 1);
	memcpy (p, r1, n);
	*(p + n) = 0;

	if (*r0 == '|')
	{
	    qxl->fp = popen (p, "wb");
	    qxl->close = 2;
	}
	else if (*r0 == '>')
	{
	    char *mode;
	    if (*(r0 + 1) == '>')
	    {
		mode = (flag & QX_TEXT) ? "a" : "ab";
	    }
	    else
		mode = (flag & QX_TEXT) ? "w+" : "w+b";
	    qxl->fp = fopen (p, mode);
	    qxl->close = 1;
	}
	if ((nl = l - (r2 - s)) > 0)
	{
	    memmove (r0, r2, nl);
	}
	*(r0 + nl) = 0;
    }

    if (qxl->fp == NULL)
    {
	qxl->fp = stdout;
	qxl->close = 0;
    }

    for (n = 0, r0 = s; (*av = strtok (r0, "\t ")); r0 = 0, n++, av++)
	;
    pav = (char **) realloc (pav, sizeof (char *) * (n + 1));
    *(pav + n) = NULL;
    return pav;
}

static void closeout (QXL * qxl, char **pav)
{
    if (pav)
	free (pav);

    if (qxl->close == 1)
    {
	fclose (qxl->fp);
    }
    else if (qxl->close == 2)
    {
	pclose (qxl->fp);
    }
    qxl->fp = NULL;
}

static double unify (QXL * qxl, u_short x, char *f)
{
    double ts;

    ts = x * qxl->h.sectc;
    if (ts > 1024 * 1024)
    {
	ts /= 1024 * 1024;
	*f = 'M';
    }
    else if (ts > 1024)
    {
	ts /= 1024;
	*f = 'K';
    }
    else
	*f = ' ';
    return ts;
}


static int qinfo (QXL * qxl, short mflag, char **p)
{
    double ts, fs;
    char tsb, fsb;
    struct stat st;

    fstat (qxl->fd, &st);

    ts = unify (qxl, qxl->h.total, &tsb);
    fs = unify (qxl, qxl->h.free, &fsb);

    fprintf (qxl->fp, "Info for QXL file %s %s\n",
	     qxl->fn, (qxl->mode == (O_RDONLY | O_BINARY)) ? "(RO)" : "");

    fprintf (qxl->fp, "Label %-.*s", qxl->h.namlen, qxl->h.name);
    if (strlen ((char *) qxl->h.name) != qxl->h.namlen)
    {
	fprintf (qxl->fp, " [%s]", qxl->h.name);
    }

    fprintf (qxl->fp, "\nSector size %d bytes\n", qxl->h.sectc);
    fprintf (qxl->fp, "Total sectors %d (%.2f %cb, on disk %d b)\n",
	     qxl->h.total, ts, tsb, (int) st.st_size);
    fprintf (qxl->fp, "Free  sectors %d (%.2f %cb)\n",
	     qxl->h.free, fs, fsb);
    fprintf (qxl->fp, "Formatted %d, (map len %d*512), rand %d, access %d\n",
	     qxl->h.formatted, qxl->h.formatted, qxl->h.rand, qxl->h.access);
    return 0;
}

/* Create fn1 in directory ld. If fp is NULL a directory is made,
   otherwise flen bytes (or the whole file if flen < 0) are copied from
   fp. A non zero dspace gives the data space of an executable, otherwise
   it is taken from an XTcc trailer (or the QDOS file header) */
static void makenewfile (QXL * qxl, char *fn1, QLDIR * ld, FILE * fp,
                         time_t ftim, long flen, uint32_t dspace)
{

    QLDIR *e, *f;
    short got = 0;
    u_long ddata;
    u_char *buf;
    int n, m, nt;
    u_short c;

    e = readqldir (qxl, ld);

    for (f = e + 1; f < e + ld->length / sizeof (QLDIR); f++)
    {
	if (swaplong (f->length) < sizeof (QLDIR) && swapword (f->type)
            != 0xff)
	{
	    got = 1;
	    break;
	}
    }

    if (got == 0)
    {
	e = realloc (e, ld->length + sizeof (QLDIR));
	f = e + ld->length / sizeof (QLDIR);
	ld->length += sizeof (QLDIR);
    }

    buf = (u_char *) alloca (qxl->h.sectc);

    memset (buf, 0, qxl->h.sectc);
    memset (f, 0, sizeof (QLDIR));

    nt = 0;
    ddata = 0;

    if (fp)
    {
	int ilen, off, nr;
	long rem = flen;

	if (dspace)
	{
	    ddata = dspace;
	}
	else
	{
#ifndef __QDOS__
	    XTCC xtcc;
	    long pos = ftell (fp);
	    fseek (fp, -8, SEEK_END);
	    if (fread (&xtcc, 1, 8, fp) == 8 &&
		memcmp (xtcc.x.xtcc, "XTcc", 4) == 0)
	    {
		ddata = swaplong (xtcc.dlen);
	    }
	    fseek (fp, pos, SEEK_SET);
#else
	    struct qdirect qd;
	    if (fs_headr (fgetchid (fp), -1, &qd, sizeof (qd)) >= 0)
	    {
		ddata = qd.d_datalen;
	    }
#endif
	}
	for (nr = -1, off = sizeof (QLDIR), c = 0; nr != 0;)
	{
	    ilen = qxl->h.sectc - off;
	    if (rem >= 0 && ilen > rem)
		ilen = rem;
	    if ((nr = fread (buf + off, 1, ilen, fp)) > 0)
	    {
		c = getcluster (qxl, c);
		if (off)
		{
		f->map = c;
		}
		qxl_write (qxl, c * qxl->h.sectc, buf, nr + off);
		off = 0;
		nt += nr;
		if (rem >= 0)
		    rem -= nr;
	    }
	}
	if (ddata)
	{
	    f->type = 1;
	}
    }
    else
    {
	f->date = 0;
	f->map = c = getcluster (qxl, 0);
	qxl_write (qxl, c * qxl->h.sectc, buf, qxl->h.sectc);
	f->type = 0xff;
    }

    f->date = ftim;
    f->length = nt + sizeof (QLDIR);
    f->data = ddata;

    m = strlen (fn1);
    n = 0;

    if (ld->map == qxl->curd.map)
    {
	if ((n = ld->nlen))
	{
	    memcpy ((char *) f->name, (char *) ld->name, n);
	    *(f->name + n) = '_';
	    n++;
	}
    }

    if (m + n > QLPATH_MAX)
    {
	m = QLPATH_MAX - n;
    }

    memcpy ((char *) f->name + n, fn1, m);
    f->nlen = m + n;
    
    d_normalise (f);
    writeqldir (qxl, ld, e);
    free (e);

    if (ld->map == qxl->curd.map)
    {
	qxl->curd = *ld;
    }

    /* Must also update the parent */
    if (got == 0)
    {
	char fnm[40];
	QLDIR *dd;
        
	memcpy (fnm, (char *) ld->name, ld->nlen);
	*(fnm + ld->nlen) = 0;
	if ((dd = find_file (qxl, fnm, 0xff, &qxl->root, DO_RECURSE)))
	{
	    e = readqldir (qxl, &qxl->lastd);
	    for (f = e + 1; f < (e + qxl->lastd.length / sizeof (QLDIR)); f++)
	    {
		if (dd->map == swapword (f->map))
		{
		    f->length = swaplong (ld->length);
		    break;
		}
	    }
	    writeqldir (qxl, &qxl->lastd, e);
	    free (e);
	    free (dd);
	}
	else
	{
	    qxl->root.length += sizeof (QLDIR);
	    qxl->h.dlen = qxl->root.length;
	}
    }
    writeheader (qxl);
}

static int qmkdir (QXL * qxl, short mflag, char **av)
{
    char *p = *av;
    QLDIR ld, *d = NULL;
    char *star;

    if (p)
    {
        char ocd[40], ndir[40],*q;
        *ocd = 0;
        if(qxl->curd.nlen)
        {
            memcpy(ocd, (char *)qxl->curd.name, qxl->curd.nlen);
            *(ocd + qxl->curd.nlen) = 0;
            q = stpcpy(ndir, ocd);
            *q++ = '_';
            strcpy(q, p);
            p = ndir;
            qcd(qxl, 0, 0);
        }
        
	if ((qxl->mode & O_RDWR) == O_RDWR)
	{
	    char wild[42];
	    p = stpcpy (wild, p);
	    if (*(p - 1) == '_')
	    {
		p--;
		*p = 0;
	    }

	    if ((d = find_file (qxl, wild, 0, &qxl->curd, DO_RECURSE)) == NULL)
	    {
		star = p;
		*p++ = '_';
		*p++ = '*';
		*p = 0;
		d = find_file (qxl, wild, 0, &qxl->curd, DO_RECURSE | DO_WILD);
		*star = 0;
	    }

	    if (d)
	    {
		printf ("Cannot mkdir for %s as %-.*s exists\n",
			wild, d->nlen, d->name);
		free (d);
	    }
	    else
	    {
                
		qxl->lastd = qxl->root;
                
		find_file (qxl, wild, 0xff, &qxl->curd, DO_RECURSE | DO_BEST);
		ld = qxl->lastd;
		if (ld.nlen == 0)
		{
		    ld = qxl->curd;
		}

                makenewfile (qxl, wild, &ld, NULL, 0, -1, 0);
	    }
            if(*ocd)
            {
                q = ocd;
                qcd(qxl, 0, &q);
            }
	}
	else
	{
	    fputs ("read only filesystem\n", stderr);
	}

    }
    return p == NULL;
}

/* Write flen bytes (all of it if flen < 0) from fp to the QXL file as
   fn1, relative to the current directory, replacing any existing file.
   src names the source for messages. ftm is a Unix time. dspace, if non
   zero, is the data space for an executable (see makenewfile) */
static int putfile (QXL * qxl, const char *src, char *fn1, FILE * fp,
                    long flen, time_t ftm, uint32_t dspace)
{
    QLDIR ld, dl, *d;
    long dlen = 0;
    int rv = -1;
    char ocd[40], nfil[40], *q;

    if ((qxl->mode & O_RDWR) != O_RDWR)
    {
        fputs ("read only filesystem\n", stderr);
        return -1;
    }

    *ocd = 0;
    if(qxl->curd.nlen)
    {
        memcpy(ocd, (char *)qxl->curd.name, qxl->curd.nlen);
        *(ocd + qxl->curd.nlen) = 0;
        q = stpcpy(nfil, ocd);
        *q++ = '_';
        strcpy(q, fn1);
        fn1 = nfil;
        qcd(qxl, 0, 0);
    }

    if ((d = find_file (qxl, fn1, 0, &qxl->curd, DO_RECURSE)) != NULL)
    {
        if (d->type != 0xff)
        {
            dlen = d->length;
            dl = qxl->lastd;
        }
        else
        {
            dlen = -1;
            fprintf (stderr, "Can't overwrite directory \"%s\"\n", src);
        }
    }

    if (dlen >= 0)
    {
        long size;
        int need;

        qxl->lastd = qxl->root;
        find_file (qxl, fn1, 0xff, &qxl->curd, DO_RECURSE | DO_BEST);
        ld = qxl->lastd;
        if (ld.nlen == 0)
        {
            ld = qxl->curd;
        }

        if (flen < 0)
        {
            struct stat st;
            fstat (fileno (fp), &st);
            size = (long) st.st_size;
        }
        else
            size = flen;

        need = ((sizeof (QLDIR) + size) - dlen) / qxl->h.sectc;

        if (need > (qxl->h.free - 1))
        {
            fprintf (stderr,
                     "%s %d No room (%d free sectors, %ld b wanted %d)\n",
                     src, qxl->h.sectc,
                     qxl->h.free,
                     (long) (size + sizeof (QLDIR)),
                     need
                );
        }
        else
        {
            if (d)
            {
                qxl->lastd = dl;
                rmfile (qxl, d);
            }
            makenewfile (qxl, fn1, &ld, fp, ftm + UNIX2QL, flen, dspace);
            rv = 0;
        }
    }
    if (d)
        free (d);
    if(*ocd)
    {
        q = ocd;
        qcd(qxl, 0, &q);
    }
    return rv;
}

static int qwrite (QXL * qxl, short mflag, char **av)
{
    FILE *fp;
    char *fn0, *fn1;
    char ntmp[40];

    if ((fn0 = *av) == NULL)
        return -1;

    if ((qxl->mode & O_RDWR) != O_RDWR)
    {
        fputs ("read only filesystem\n", stderr);
        return 0;
    }

    fn1 = *(av + 1);
    if (fn1 == NULL)
    {
        char *p;
#ifndef __QDOS__
        p = strrchr (fn0, '/');
        if (p == NULL)
        {
            p = strrchr (fn0, '\\');
        }
        if (p)
            p++;
        else
            p = fn0;
#else
        p = lastdir (fn0);
#endif
        fn1 = strcpy(ntmp, p);
        for (p = fn1; *p; p++)
        {
            if (*p == '.')
                *p = '_';
        }
    }

    if ((fp = fopen (fn0, (mflag & QX_TEXT) ? "r" : "rb")) == NULL)
    {
        perror (fn0);
        return -1;
    }
    else
    {
        struct stat st;
        int rv;
        fstat (fileno (fp), &st);
        rv = putfile (qxl, fn0, fn1, fp, -1, st.st_ctime, 0);
        fclose (fp);
        return rv;
    }
}

/*------------------------------------------------------------------.
| unzip : extract a (native) zip file into the current QXL directory |
|                                                                    |
| Handles stored and deflated (via zlib) entries. Executables get    |
| their data space from the QDOS/SMS extra field in the zip, or      |
| failing that from an XTcc trailer on the data (which is removed).  |
`------------------------------------------------------------------*/

#define ZBUFSIZ 16384

/* Copy csize bytes of zip entry data from zf to tf, inflating it if
   method is 8 (deflate). Returns 0 on success, with the uncompressed
   length and CRC in *ulen and *crc, -1 for bad data, -2 if tf could not
   be written */
static int zip_copy (FILE * zf, FILE * tf, uint32_t method, uint32_t csize,
		     uint32_t * ulen, uint32_t * crc)
{
    u_char *ibuf, *obuf;
    z_stream zs;
    int rv = 0, zrv = Z_OK;
    size_t n;

    *ulen = 0;
    *crc = crc32 (0L, Z_NULL, 0);
    ibuf = malloc (ZBUFSIZ);
    obuf = malloc (ZBUFSIZ);
    memset (&zs, 0, sizeof (zs));

    if (ibuf == NULL || obuf == NULL ||
	(method == 8 && inflateInit2 (&zs, -MAX_WBITS) != Z_OK))
    {
	fputs ("Out of memory\n", stderr);
	free (ibuf);
	free (obuf);
	return -1;
    }

    while (rv == 0 && zrv != Z_STREAM_END)
    {
	/* Refill the input */
	if (zs.avail_in == 0 && csize)
	{
	    n = (csize < ZBUFSIZ) ? csize : ZBUFSIZ;
	    if (fread (ibuf, 1, n, zf) != n)
	    {
		rv = -1;
		break;
	    }
	    csize -= n;
	    zs.next_in = ibuf;
	    zs.avail_in = n;
	}

	if (method == 8)
	{
	    zs.next_out = obuf;
	    zs.avail_out = ZBUFSIZ;
	    zrv = inflate (&zs, Z_NO_FLUSH);
	    if (zrv != Z_OK && zrv != Z_STREAM_END)
	    {
		rv = -1;	/* corrupt, or truncated (Z_BUF_ERROR) */
		break;
	    }
	    n = ZBUFSIZ - zs.avail_out;
	}
	else
	{
	    if (zs.avail_in == 0)
		break;		/* all copied */
	    memcpy (obuf, zs.next_in, zs.avail_in);
	    n = zs.avail_in;
	    zs.avail_in = 0;
	}

	if (n)
	{
	    if (fwrite (obuf, 1, n, tf) != n)
		rv = -2;
	    *crc = crc32 (*crc, obuf, n);
	    *ulen += n;
	}
    }

    if (method == 8)
	inflateEnd (&zs);
    free (ibuf);
    free (obuf);
    return rv;
}

static uint32_t zrd16 (const u_char * p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8);
}

static uint32_t zrd32 (const u_char * p)
{
    return zrd16 (p) | (zrd16 (p + 2) << 16);
}

static uint32_t berd32 (const u_char * p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
	((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

/* Look for the QDOS/SMS extra field (ID 0xfb4a) in a zip extra block.
   Info-ZIP writes it in one of two forms, each followed by the 64 byte
   (big endian) QDOS file header:
     "QDOS02\0\0" + header
     "QZHD"       + header   (Jonathan Hudson's QDOS zip)
   The type is at header offset 5 and the data space at offset 6 */
static int zip_qdosextra (const u_char * x, long xlen, int *type,
			  uint32_t * dspace)
{
    while (xlen >= 4)
    {
	uint32_t id = zrd16 (x);
	long sz = zrd16 (x + 2);
	const u_char *d = x + 4;
	long off = -1;

	if (sz > xlen - 4)
	    break;
	if (id == 0xfb4a || id == 0x4afb)
	{
	    if (sz >= 4 && memcmp (d, "QZHD", 4) == 0)
		off = 4;
	    else if (sz >= 8 && memcmp (d, "QDOS", 4) == 0)
		off = 8;
	    if (off >= 0 && sz >= off + 10)
	    {
		*type = d[off + 5];
		*dspace = berd32 (d + off + 6);
		return 1;
	    }
	}
	x += 4 + sz;
	xlen -= 4 + sz;
    }
    return 0;
}

/* Unix modification time from an extended timestamp (0x5455) field */
static int zip_utime (const u_char * x, long xlen, time_t * t)
{
    while (xlen >= 4)
    {
	uint32_t id = zrd16 (x);
	long sz = zrd16 (x + 2);

	if (sz > xlen - 4)
	    break;
	if (id == 0x5455 && sz >= 5 && (x[4] & 1))
	{
	    *t = (time_t) (int32_t) zrd32 (x + 5);
	    return 1;
	}
	x += 4 + sz;
	xlen -= 4 + sz;
    }
    return 0;
}

/* Make QDOS directory qname[0..len) (relative to the current
   directory) unless it already exists */
static void zip_mkdir (QXL * qxl, const char *qname, int len)
{
    char dn[QLPATH_MAX + 1], *p = dn;
    QLDIR *d;

    memcpy (dn, qname, len);
    dn[len] = 0;
    if ((d = find_file (qxl, dn, 0xff, &qxl->curd, DO_RECURSE)) != NULL)
    {
	free (d);
    }
    else
    {
	fprintf (qxl->fp, "  creating: %s\n", dn);
	qmkdir (qxl, 0, &p);
    }
}

/* Extract the entry described by central directory record ce */
static int zip_extract (QXL * qxl, FILE * zf, const u_char * ce)
{
    char zname[256], qname[QLPATH_MAX + 1], sep[QLPATH_MAX + 1];
    u_char lh[30], *lx = NULL;
    uint32_t flags, method, crc, csize, usize, nlen, xlen, lhoff, lxlen;
    uint32_t dspace = 0;
    int isdir, qtype = 0, gotq, k, pre, rv = -1;
    long flen;
    const u_char *cx;
    char *s;
    time_t ftm;
    FILE *tf;
    uint32_t ulen, ucrc;
    int zrv;

    flags = zrd16 (ce + 8);
    method = zrd16 (ce + 10);
    crc = zrd32 (ce + 16);
    csize = zrd32 (ce + 20);
    usize = zrd32 (ce + 24);
    nlen = zrd16 (ce + 28);
    xlen = zrd16 (ce + 30);
    lhoff = zrd32 (ce + 42);
    cx = ce + 46 + nlen;

    if (nlen >= sizeof (zname))
	nlen = sizeof (zname) - 1;
    memcpy (zname, ce + 46, nlen);
    zname[nlen] = 0;

    /* QDOS name : directory separators and '.' become '_' */
    isdir = (nlen && zname[nlen - 1] == '/');
    pre = (qxl->curd.nlen) ? qxl->curd.nlen + 1 : 0;
    for (s = zname; *s == '/'; s++)
	;
    for (k = 0; *s && !(isdir && s[1] == 0); s++)
    {
	if (k + pre >= QLPATH_MAX)
	{
	    fprintf (stderr, "%s: name too long for QDOS, skipped\n", zname);
	    return -1;
	}
	sep[k] = (*s == '/' || *s == '\\');
	qname[k] = (sep[k] || *s == '.') ? '_' : *s;
	k++;
    }
    qname[k] = 0;
    if (k == 0)
	return 0;

    /* Create any directories in the path */
    for (pre = 0; pre < k; pre++)
    {
	if (sep[pre])
	    zip_mkdir (qxl, qname, pre);
    }
    if (isdir)
    {
	zip_mkdir (qxl, qname, k);
	return 0;
    }

    if (flags & 1)
    {
	fprintf (stderr, "%s: encrypted, skipped\n", zname);
	return -1;
    }
    if (method != 0 && method != 8)
    {
	fprintf (stderr, "%s: compression method %d not supported, skipped\n",
		 zname, (int) method);
	return -1;
    }
    if (csize == 0xffffffffUL || usize == 0xffffffffUL ||
	lhoff == 0xffffffffUL)
    {
	fprintf (stderr, "%s: zip64 not supported, skipped\n", zname);
	return -1;
    }

    /* Local header, and its extra field, which may hold the QDOS header */
    if (fseek (zf, (long) lhoff, SEEK_SET) != 0 ||
	fread (lh, 1, 30, zf) != 30 || zrd32 (lh) != 0x04034b50UL)
    {
	fprintf (stderr, "%s: bad local header, skipped\n", zname);
	return -1;
    }
    lxlen = zrd16 (lh + 28);
    fseek (zf, (long) zrd16 (lh + 26), SEEK_CUR);
    if (lxlen)
    {
	lx = malloc (lxlen);
	if (lx == NULL || fread (lx, 1, lxlen, zf) != lxlen)
	{
	    fprintf (stderr, "%s: bad local header, skipped\n", zname);
	    free (lx);
	    return -1;
	}
    }

    gotq = zip_qdosextra (cx, xlen, &qtype, &dspace);
    if (!gotq && lx)
	gotq = zip_qdosextra (lx, lxlen, &qtype, &dspace);
    if (!zip_utime (cx, xlen, &ftm) && !(lx && zip_utime (lx, lxlen, &ftm)))
    {
	struct tm tm;
	uint32_t dt = zrd16 (ce + 12), dd = zrd16 (ce + 14);

	memset (&tm, 0, sizeof (tm));
	tm.tm_sec = (dt & 0x1f) * 2;
	tm.tm_min = (dt >> 5) & 0x3f;
	tm.tm_hour = dt >> 11;
	tm.tm_mday = dd & 0x1f;
	tm.tm_mon = ((dd >> 5) & 0x0f) - 1;
	tm.tm_year = (dd >> 9) + 80;
	tm.tm_isdst = -1;
	ftm = mktime (&tm);
    }

    if ((tf = tmpfile ()) == NULL)
    {
	perror ("tmpfile");
	free (lx);
	return -1;
    }

    zrv = zip_copy (zf, tf, method, csize, &ulen, &ucrc);
    if (zrv != 0 || ulen != usize || ucrc != crc)
    {
	fprintf (stderr, "%s: bad zip data (%s), skipped\n", zname,
		 (zrv == -2) ? "write error" : "CRC or length");
    }
    else
    {
	const char *how = "";

	flen = (long) usize;
	if (gotq)
	{
	    /* QDOS header in the zip is definitive */
	    if (qtype != 1)
		dspace = 0;
	    how = " (QDOS extra field)";
	}
	else if (usize >= 8)
	{
	    u_char t[8];

	    fseek (tf, flen - 8, SEEK_SET);
	    if (fread (t, 1, 8, tf) == 8 && memcmp (t, "XTcc", 4) == 0)
	    {
		dspace = berd32 (t + 4);
		flen -= 8;	/* the trailer is not part of the file */
		how = " (XTcc trailer)";
	    }
	}

	rewind (tf);
	if ((rv = putfile (qxl, zname, qname, tf, flen, ftm, dspace)) == 0)
	{
	    fprintf (qxl->fp, "  %s: %s -> %s", (method == 8) ?
		     "inflating" : "extracting", zname, qname);
	    if (dspace)
		fprintf (qxl->fp, " [data space %" PRIu32 "%s]", dspace, how);
	    fputc ('\n', qxl->fp);
	}
    }
    fclose (tf);
    free (lx);
    return rv;
}

static int qunzip (QXL * qxl, short mflag, char **av)
{
    char *zfn = *av;
    FILE *zf;
    u_char *buf = NULL, *cd = NULL, *p, *e;
    long zsize, n;
    uint32_t cdoff, cdsize, nent, i;
    int nok = 0, nbad = 0;

    if (zfn == NULL)
	return -1;

    if ((qxl->mode & O_RDWR) != O_RDWR)
    {
	fputs ("read only filesystem\n", stderr);
	return 0;
    }

    if ((zf = fopen (zfn, "rb")) == NULL)
    {
	perror (zfn);
	return 0;
    }

    /* Find the end of central directory record, allowing for a comment */
    fseek (zf, 0, SEEK_END);
    zsize = ftell (zf);
    n = (zsize < 65535 + 22) ? zsize : 65535 + 22;
    p = NULL;
    if (n >= 22 && (buf = malloc (n)) != NULL &&
	fseek (zf, zsize - n, SEEK_SET) == 0 &&
	fread (buf, 1, n, zf) == (size_t) n)
    {
	for (p = buf + n - 22; p >= buf; p--)
	{
	    if (zrd32 (p) == 0x06054b50UL)
		break;
	}
	if (p < buf)
	    p = NULL;
    }

    if (p == NULL)
    {
	fprintf (stderr, "%s: not a zip file\n", zfn);
    }
    else
    {
	nent = zrd16 (p + 10);
	cdsize = zrd32 (p + 12);
	cdoff = zrd32 (p + 16);

	if (cdoff == 0xffffffffUL || cdsize == 0xffffffffUL)
	{
	    fprintf (stderr, "%s: zip64 not supported\n", zfn);
	}
	else if ((cd = malloc (cdsize + 1)) == NULL ||
		 fseek (zf, (long) cdoff, SEEK_SET) != 0 ||
		 fread (cd, 1, cdsize, zf) != cdsize)
	{
	    fprintf (stderr, "%s: can't read zip directory\n", zfn);
	}
	else
	{
	    for (p = cd, e = cd + cdsize, i = 0; i < nent; i++)
	    {
		if (e - p < 46 || zrd32 (p) != 0x02014b50UL ||
		    e - p < 46 + (long) (zrd16 (p + 28) + zrd16 (p + 30) +
					 zrd16 (p + 32)))
		{
		    fprintf (stderr, "%s: corrupt zip directory\n", zfn);
		    nbad++;
		    break;
		}
		if (zip_extract (qxl, zf, p) == 0)
		    nok++;
		else
		    nbad++;
		p += 46 + zrd16 (p + 28) + zrd16 (p + 30) + zrd16 (p + 32);
	    }
	    fprintf (qxl->fp, "%s: %d entr%s extracted\n", zfn, nok,
		     (nok == 1) ? "y" : "ies");
	    if (nbad)
		fprintf (stderr, "%s: %d entr%s not extracted\n", zfn, nbad,
			 (nbad == 1) ? "y" : "ies");
	}
    }

    free (cd);
    free (buf);
    fclose (zf);
    return 0;
}

static int version (QXL * qxl, short mflag, char **av)
{
    fputs ("qxltool " VERSTR " for " TARGET "\n", stdout);
    return 0;
}


static char *defargs (char *av)
{
    char *fn;
    if (av)
    {
	fn = av;
    }
    else if ((fn = getenv ("QXL_FILE")) == NULL)
    {
	fn = DEFQXL;
    }
    return fn;
}

static int qxlopen (QXL * qxl, short mflag, char **av)
{
    char *fn = defargs (*av);
    if (qxl->fd <= 0)
    {
	openqxl (qxl, fn);
	return 0;
    }
    else
	return 1;
}

static int qclose (QXL * qxl, short mflag, char **av)
{
    if (qxl->fd > 0)
    {
	close (qxl->fd);
	qxl->fd = -1;
	*qxl->fn = 0;
	return 0;
    }
    return 1;
}

static void concatargs (char **av, char *cmd)
{
    char *c = cmd;

    if (*av)
    {
	while (*av)
	{
	    c = stpcpy (c, *av);
	    *c++ = ' ';
	    av++;
	}
	--c;
    }
    *c = 0;
}

static void init (QXL * qxl, char *lab, u_short bs, u_short ts)
{
    char *l;
    short ns;
    u_short i, j, k, m;
    size_t n;

    if ((qxl->mode & O_RDWR) == O_RDWR)
    {
	l = alloca (bs);

	memset (&qxl->h, 0, sizeof (HEADER));
	memset (qxl->h.name, ' ', 20);

	qxl->h.total = ts;
	qxl->h.sectc = bs;
        qxl->h.dummy3 = 1; 
	n = strlen (lab);
	if (n > 20)
	    n = 20;

	memcpy (qxl->h.name, lab, n);
	qxl->h.namlen = n;
	qxl->h.access = 0;
	memcpy (qxl->h.id, "QLWA", 4);
        ns = (qxl->h.total * 2 + bs - 1) / bs + 1;
        {
            off_t poff = sizeof (QLDIR);
            for (i=1; i<=ns-1; i++)
            {
                j=swapword(i);
                qxl_write (qxl, poff, &j, sizeof (short));
                poff += sizeof (short);
            }
            qxl->h.map[0]=1;  /* writeheader (over)writes this ! */
            j=0;
            qxl_write(qxl, poff, &j, 2); poff += 2; i++; /* EOF MAP */

            qxl->h.formatted = (qxl->h.total*2+511+0x40)/512;
                             /* this appears to be the length of map in sectors..*/

            qxl->h.direct = i-1;
            qxl_write(qxl, poff, &j, 2); poff += 2; i++; /* EOF DIR */

            qxl->h.first = i-1;

            for (i = qxl->h.first; i < qxl->h.total - 1;)
            {
                i++;
                j = swapword (i);
                qxl_write (qxl, poff, &j, sizeof (short));
                poff += sizeof (short);
            }

            j = 0;
            qxl_write (qxl, poff, &j, sizeof (short));
        }

	qxl->h.free = qxl->h.total - ns - 1;
	qxl->h.dlen = sizeof (QLDIR);
	srand (time (NULL));
	qxl->h.rand = rand ();

	writeheader (qxl);

	l = alloca (bs);
	memset (l, 0, bs);
	fputs ("Formatting ....", stdout);

	k = j = qxl->h.total / 20;
	m = 5;

	for (i = qxl->h.direct; (size_t) i < qxl->h.total; i++)
	{
	    if (qxl_write (qxl, (off_t) i * bs, l, bs) != (ssize_t) bs)
	    {
		perror ("Format");
		exit (0);
	    }
	    if (i > k)
	    {
		printf (" %3d%%\b\b\b\b\b", m);
		fflush (stdout);
		k += j;
		m += 5;
	    }
	}
	fputs (" Done\n", stdout);
#ifdef mc68000
	fputs(" you must fix the geometry before SMSQ can use this filesystem\n",stdout);
#endif
#if 0
	qxl->root.length = qxl->h.dlen;
	qxl->root.map = qxl->h.direct;
	qxl->root.type = 0xff;
	*qxl->root.name = 0;
	qxl->root.nlen = 0;
	qxl->lastd = qxl->root;
	qxl->curd = qxl->root;
#else
	readheader(qxl);
#endif
    }
    else
    {
	fputs ("read only filesystem\n", stderr);
    }
}

static int qinit (QXL * qxl, short mflag, char **av)
{
    char lab[80];
    u_short bs = qxl->h.sectc;
    u_short ts = qxl->h.total;
    concatargs (av, lab);
    init (qxl, lab, bs, ts);
    qcd(qxl, 0, 0);
    return 0;
}

#ifdef HAVE_LIBREADLINE
char * READLINE(char *a, char *b)
{
    char *c = NULL;
    if(isatty(fileno(stdin)))
    {
        if((c=readline(a)) != NULL && b)
        {
            strcpy(b, c);
        }
    }
    else if((c=fgets(b,LINSIZ,stdin)) != NULL)
    {
        char *d;
        if((d = strchr(b,'\n')))
        {
            *d = 0;
        }
    }
    return c;
}

void FREECMD(char *a, char *b)
{
    if(isatty(fileno(stdin)))
    {
        if(*a)
            add_history(a);
        free(b);
    }
    if(!isatty((fileno(stdout))))
    {
        fputs("EOM\b\b\b\n", stdout);
        fflush(stdout);
    }
}

char * DUPCMD(char *b)
{
    char *a;
    if(isatty(fileno(stdin)))
    {
        a = b;
    }
    else
    {
        a = strdup(b);
    }
    return a;
}
#else
char * READLINE(char *a, char *b)
{
    char *c = NULL;
    if(isatty(fileno(stdin)))
    {
        fputs(a, stdout);
        fflush(stdout);
    }
    if((c=fgets(b, LINSIZ, stdin)) != NULL)
    {
        char *d;
        if((d = strchr(b,'\n')))
        {
            *d = 0;
        }
    }
    return c;
}
void FREECMD(char *a, char *b)
{
    if(!isatty(fileno(stdin)) && !isatty(fileno(stdout)))
    {
        fputs("EOM\b\b\b\n", stdout);
        fflush(stdout);
    }
}
# define DUPCMD(b) strdup(b)
#endif

#ifdef mc68000
# define READCMD(a,b,c) b = READLINE(a,c)

static int qfixgeometry (QXL * qxl, short mflag, char **av)
{
  char *spc=NULL;
  char *hpc=NULL;
  char *p = (av) ? *av : NULL;
  unsigned short sects,heads;
  char *pcmd;
  char cmd[LINSIZ];

  if (p) printf("ignoring parameter..\n");
  
  printf("current geometry: %d Sects/track %d Heads/cyl\n",qxl->h.dummy2[0],qxl->h.dummy2[1]);
  
  if ((READCMD ("Secs/track : ", pcmd, cmd)) != NULL)
  {
      spc = DUPCMD(pcmd);
  }
  else
  {
      printf("geometry not changed\n");
      return 0;
  }
  if ((READCMD ("Heads/cyl : ", pcmd, cmd)) != NULL)
  {
      hpc = DUPCMD(pcmd);
  }
  else return 0;
  
  sects=strtoul(spc,NULL,0);
  heads=strtoul(hpc,NULL,0);
  qxl->h.dummy2[0]=sects;
  qxl->h.dummy2[1]=heads;
  printf("setting to %d/%d\n",sects,heads);
  writeheader(qxl);
  free(hpc);
  free(spc);
  
  return 0;
}
#endif

static int qformat (QXL * qxl, short mflag, char **av)
{
    char cmd[LINSIZ];
    char *pcmd;
    char lab[32];

    char *fn = NULL, *size = NULL, *label = NULL;

    if (*av)
    {
	fn = *av++;
	if (*av)
	{
	    size = *av++;
	    if (*av)
	    {
		concatargs (av, lab);
		label = lab;
	    }
	}
    }

    if (label == NULL || *label == 0)
    {
	if ((pcmd = READLINE ("QXL File name : ", cmd)) != NULL)
	{
	    fn = DUPCMD (pcmd);
	    if ((pcmd = READLINE ("Size in Mb : ", cmd)) != NULL)
	    {
		size = DUPCMD (pcmd);
		if ((pcmd = READLINE ("Label  : ", cmd)) != NULL)
		{
		    label = DUPCMD (pcmd);
		}
	    }
	}
    }

    if (label)
    {
        int fmode = O_CREAT | O_BINARY | O_RDWR;
	QXL *q = malloc (sizeof (QXL));
        
        if(qxl->fmode == 0)
        {
            fmode |= O_EXCL;
        }
        
	if (q &&
	    (q->fd = open (fn, fmode, 0666)) != -1)
	{
	    static short mbs[] =
	    {32, 64, 128, 256, 0};
	    unsigned long req;
	    u_short ss, bs, ts;
	    short i;

	    /* Clamp to the supported range before sizing the file, so
	       the file matches the filesystem written into it */
	    req = strtoul (size, NULL, 10);
	    ss = (req < 1) ? 1 : (req > 256) ? 256 : req;
	    if (ss != req)
	    {
		fprintf (stderr, "Size must be 1 to 256 Mb, using %d Mb\n", ss);
	    }

#ifdef HAVE_FTRUNCATE
            ftruncate(q->fd, (off_t) ss << 20);
#endif
	    for (i = 0; mbs[i]; i++)
	    {
		if (ss <= mbs[i])
		    break;
	    }

            bs = (1 << i);
	    {
		/* The header holds the cluster count in 16 bits, so sizes
		   on a cluster size boundary (32/64/128/256 Mb) would give
		   65536 and wrap to 0. Drop the last cluster instead. */
		unsigned long nts = ((unsigned long) ss << 20) / (bs << 9);
		ts = (nts > 0xFFFF) ? 0xFFFF : nts;
	    }
	    bs <<= 9;
            q->mode = fmode;
	    init (q, label, bs, ts);
	    close (q->fd);
	}
	else
	    perror ("New QXL");
        if(q) free (q);
    }
    else
	fputc ('\n', stdout);
    if (label != lab)
    {
	if (fn)
	    free (fn);
	if (size)
	    free (size);
	if (label)
	    free (label);
    }

    return 0;
}


static int qsystem (QXL * qxl, short mflag, char **av)
{
    char cmd[1024], *c;

    concatargs (av, cmd);

    if (*cmd == 0)
    {
	if ((c = getenv (SHELL)))
	{
	    strcpy (cmd, c);
	}
    }

    if (*cmd)
    {
	system (cmd);
    }
    return 0;
}

static intptr_t clone_cb (QXL * sqxl, QLDIR * d, void *q, void *pb, u_short c)
{
    QXL *dqxl = (QXL *)q;
    char *sd, *dd;
    int nsd;
    char df[128];
    char *p;
    int nfn;

    if(d->nlen)
    {
        sd = (char *)pb;
        nsd = strlen(sd);
        dd = sd + nsd + 1;

        strcpy(df, dd);
        if(*df && *df != '_')
        {
            strcat(df, "_");
        }
        
        if(d->type == 0xff)
        {
            qcd(dqxl, 0, NULL);
            p = strcat(df, (char *)d->name); // cast for c68, alas
            if(qcd(dqxl, QX_FAIL_SILENTLY, &p))
                qmkdir(dqxl, 0, &p);
        }
        else /*if (flen > 0)*/
        {
            FILE *ofp;
            p = (char *)sqxl->tmpd.name;
            nfn = strlen(p);
            if(nfn && *(d->name+nfn) == '_') nfn++;
            p = strcat(df, p);

           ofp = sqxl->fp;
           if ((sqxl->fp = tmpfile()) != NULL)
           {
               cpfile(sqxl, d);
               rewind(sqxl->fp);
               qcd(dqxl, 0, 0);
               qcd(dqxl, 0, &p);
               putfile(dqxl, (char *)d->name, (char *)d->name + nfn,
                       sqxl->fp, -1, d->date, (d->type == 1) ? d->data : 0);
               fclose(sqxl->fp);
           }
           else
               perror("tmpfile");
           sqxl->fp = ofp;
        }
    }
    return 0;
}

static int makeclone (QXL * sqxl, QXL *dqxl, char *paths)
{
    QLDIR *buf;
    buf = readqldir (sqxl, &sqxl->curd);
    sqxl->tmpd = *buf;    
    processdir (sqxl, buf, &sqxl->curd, clone_cb, (void *)dqxl, paths, 1);
    free (buf);
    return 0;
}


#define COPY_IN 'I'
#define COPY_OUT 'O'

#ifdef GO32
# define SEP ';'
#else
# define SEP ':'
#endif

static char *initpath(QXL *qxl, char *dpath)
{

    if(qxl->curd.nlen)
    {
        dpath = stpcpy(dpath, (char *)qxl->curd.name);
        *dpath++ = '_';
        *dpath = 0;
    }

    return dpath;
}

static int qclone(QXL * qxl, short mflag, char **av)
{
    QXL *sqxl, *dqxl, *lxq = NULL;
    int rv = 0;
    
    short in_out = 0;
    char *from;
    char *to;
    char *sep;
    char *newfile = 0;
    char *startat = 0;
    char *p = NULL,*q = NULL;
    
    from = *av;
    to = *(av+1);
    if(from && to)
    {
        char *paths;
        
        if((sep = strchr(from, SEP)) != NULL)
        {
            in_out = COPY_IN;
            p = from;
            q = to;
        }
        else if((sep = strchr(to, SEP)) != NULL)
        {
            in_out = COPY_OUT;        
            p = to;
            q = from;        
        }
        if(p && q)
        {
            char *dpath;
            
            *sep = 0;
            newfile = strdup(p);
            startat = strdup(sep+1);
            if(strcmp(startat, "_") == 0)
            {
                *startat = 0;
            }

            if(strcmp(q,"_") == 0)
            {
                *q = 0;
            }

            paths = malloc(strlen(q) + strlen(startat) + 4);
            lxq = malloc (sizeof (QXL));
            memset (lxq, 0, sizeof (QXL));

            if(in_out == COPY_OUT)
            {
                sqxl = qxl;
                dqxl = lxq;
                dqxl->mode = O_BINARY|O_RDWR;
                if(openqxl(dqxl, newfile) != -1)
                {
                    dpath = initpath(qxl, paths);
                    dpath = stpcpy(dpath, q);
                    dpath++;
                    strcpy(dpath, startat);
                }
                else rv = -1;
            }
            else
            {
                sqxl = lxq;
                dqxl = qxl;
                sqxl->mode = O_BINARY;
                if(openqxl(sqxl, newfile) != -1)
                {
                    dpath= stpcpy(paths, startat);
                    dpath++;
                    dpath = initpath(qxl, dpath);
                    strcpy(dpath, q);
                }
                else rv = -1;
            }

            if(rv == 0)
            {
                char *scd = paths;
                int n = strlen(paths);
                
                if(n > sqxl->curd.nlen)
                {
                    scd += sqxl->curd.nlen;
                    if(*scd == '_') scd++;
                }
                
                if(*scd == 0 || qcd(sqxl, 0, &scd) == 0)
                {
                    if(qcd(dqxl, QX_FAIL_SILENTLY, &dpath))
                    {
                        qmkdir(dqxl, 0, &dpath);
                        qcd(dqxl, 0, &dpath);
                    }
                    makeclone (sqxl, dqxl, paths);
                }
                
                if(newfile) free (newfile);
                if(startat) free (startat);
            }
            if(lxq) qclose(lxq, 0, NULL);
        }
    }
    else
    {
        rv = -1;
    }
    return rv;
}

static int qchgmode(QXL *qxl)
{
    char *p,*q;
    p = strdup(qxl->fn);
    q = malloc(qxl->curd.nlen+1);
    memcpy(q, qxl->curd.name, qxl->curd.nlen);
    *(q+qxl->curd.nlen) = 0;
    qclose(qxl, 0, NULL);
    openqxl(qxl, p);
    qcd(qxl, QX_FAIL_SILENTLY, &q);
    free (p);
    free (q);    
    return 0;
}

static int qrwmode (QXL* qxl, short mflag, char **av)
{
    qxl->mode &= ~O_RDONLY;
    qxl->mode |= O_RDWR;
    return qchgmode(qxl);
}

static int qromode (QXL* qxl, short mflag, char **av)
{
    qxl->mode &= ~O_RDWR;
    qxl->mode |= O_RDONLY;
    return qchgmode(qxl);
}

static JTBL jtbl[] =
{
    {"cd", qcd, QX_OPEN},
    {"lslr", qlslr, QX_TEXT | QX_OPEN},
    {"ls", qls, QX_TEXT | QX_OPEN},
    {"dump", qdump, QX_OPEN},
    {"pwd", qpwd, QX_TEXT | QX_OPEN},
    {"cat", qcp, QX_OPEN},
    {"cp", qcp, QX_OPEN},
#ifdef NEED_DOS_FUNCTIONS
    {"tcat", qcp, QX_TEXT | QX_OPEN},
    {"tcp", qcp, QX_TEXT | QX_OPEN},
    {"twr", qwrite, QX_TEXT | QX_OPEN},
#endif
    {"info", qinfo, QX_TEXT | QX_OPEN},
    {"quit", quit},
    {"exit", quit},
    {"init", qinit, QX_OPEN},
    {"wr", qwrite, QX_OPEN},
    {"unzip", qunzip, QX_OPEN},
    {"rmdir", qrmdir, QX_OPEN},
    {"rm", qrm, QX_OPEN},
    {"mkdir", qmkdir, QX_OPEN},
    {"version", version},
    {"help", help},
    {"open", qxlopen},
    {"close", qclose, QX_OPEN},
    {"format", qformat},
    {"shell", qsystem},
    {"clone", qclone, QX_OPEN},
    {"RO", qromode, QX_OPEN},
    {"RW", qrwmode, QX_OPEN},
#ifdef mc68000
    {"fix_geometry", qfixgeometry},
#endif
    {NULL, NULL}
};

static int help (QXL * qxl, short mflag, char **av)
{
    JTBL *j;
    fputs ("Commands in qxltool " VERSION "\n\n", qxl->fp);

    for (j = jtbl; j->name; j++)
    {
	fprintf (qxl->fp, "\t%s\n", j->name);
    }
    fputs ("\nRead the fine manual for more information\n", qxl->fp);
    return 0;
}

static void ShowCmdHelp()
{
    char *txt[] =
    {
        "Usage: qxltool [-W] [-w] [-h] [QXL_FILE] [params]",
        "If QXL_FILE is not defined on the command line, then the environment",
        "variable is named QXL_FILE is used",
        "",
        "qxltool defaults to a readonly QXL.WIN file to protect your data,",
        "if you're feeling brave, you can run qxltool is read-write mode",
        "by giving the '-w' option.",
        "",
        "\tqxltool -w /cdrom/qxl.win",
        "",
        "qxltool can attempt to format and use a new QXL.WIN file by giving",
        "more than three parameters:"
        "",
        "\tqxltool -w file_name size_in_MB Label0 ... LabelN ",
        "",
        "for example:",
        "",
        "\tqxltool -w /tmp/qxl.win 8 QXL Disk Label",
        "",
        "Would create an 8Mb QXL file (/tmp/qxl.win) with a disk label",
        "\"QXL Disk Label\".",
        "",
        "In order to format an existing QXL file (or partition), then you must",
        "give a -W option. This is to protect people from accidentally",
        "formatting an existing file/partition.",
        "",
        "\tqxltool -W /tmp/oldqxl.win 8 QXL Disk Label", "",
        "Q40/Q60 CF-card and HDD image commands (run 'qxltool cardhelp' for details):",
        "\tcardinfo IMAGE       inspect a card image or a .WIN file",
        "\tcardbuild ... -o OUT build a card image from .WIN files",
        "\tcardextract IMAGE    extract big-endian .WIN partitions",
        "\tcardappend | carddrop | cardclonetable | cardswap | cardfixgeom",
        "", "Good Luck!",
        NULL};
    char **p;
    for(p = txt;*p;p++)
    {
        fputs(*p, stderr);
        fputc('\n', stderr);
    }
}


int main (int ac, char **av)
{

    char *fn;
    int c;
    int defmod = O_RDONLY | O_BINARY;
    char *lcmd = NULL;
    
    QXL *qxl = malloc (sizeof (QXL));
    memset (qxl, 0, sizeof (QXL));
#ifdef __unix__
    signal (SIGPIPE, SIG_IGN);
#endif
    while ((c = getopt (ac, av, "wWc:")) != EOF)
    {
	switch (c)
	{
            case 'W':
                qxl->fmode = 1;
	    case 'w':
		defmod = O_BINARY | O_RDWR;
		break;
            case 'c':
                lcmd = strdup(optarg);
                break;
	}
    }

    fn = defargs (*(av + optind));

    /* Q40/Q60 card subcommand dispatch: intercept before the QXL open path.
       Supports both `qxltool cardinfo IMAGE` and `qxltool -c "cardinfo IMAGE"`. */
    {
        char *first = (ac - optind > 0) ? *(av + optind) : NULL;
        if (lcmd == NULL && first && strncmp (first, "card", 4) == 0) {
            int c = qxcard_dispatch (ac - optind, av + optind);
            if (c >= 0)
                return c;
        }
        if (lcmd && strncmp (lcmd, "card", 4) == 0) {
            char cmd_[LINSIZ];
            char *tok, *argv_[64];
            int cargc = 0;
            strncpy (cmd_, lcmd, sizeof (cmd_) - 1);
            cmd_[sizeof (cmd_) - 1] = 0;
            for (tok = strtok (cmd_, " \t"); tok && cargc < 63; tok = strtok (NULL, " \t"))
                argv_[cargc++] = tok;
            argv_[cargc] = NULL;
            if (cargc) {
                int c = qxcard_dispatch (cargc, argv_);
                if (c >= 0)
                    return c;
            }
        }
    }

    qxl->mode |= defmod;

    if (ac - optind > 2)
    {
	qformat (qxl, 0, av + optind);
    }

    if ((openqxl (qxl, fn)) != -1)
    {
	char cmd[LINSIZ];
	char *pcmd, *ptr;

	while (1)
	{
	    JTBL *j;
	    char **args;

            if(lcmd)
            {
#ifndef HAVE_LIBREADLINE
                pcmd = strcpy(cmd, lcmd);
#else  
                pcmd = lcmd;
#endif
            }
            else if((pcmd = READLINE ("> ", cmd)) == NULL)
            {
                break;
            }

	    if ((ptr = strchr (pcmd, '#')))
		*ptr = 0;

	    ptr = pcmd + strspn (pcmd, "\t ");

	    for (j = jtbl; j->name; j++)
	    {
		int n = strlen (j->name);
		if (memcmp (ptr, j->name, n) == 0)
		{
		    strim (ptr);
		    while ((u_char) *(ptr + n) > ' ')
			n++;
		    n += strspn (ptr + n, "\t ");
		    if (qxl->fd <= 0 && j->flag & QX_OPEN)
		    {
			fputs ("That requires an open QXL_FILE\n", stderr);
		    }
		    else
		    {
			args = openout (qxl, ptr + n, j->flag);
			if ((j->func) (qxl, j->flag, args))
			{
			    fputs ("Invalid argument(s)\n", stderr);
			}
			closeout (qxl, args);
		    }
		    break;
		}
	    }
	    if (*pcmd && j->name == NULL)
	    {
		fprintf (stdout, "Invalid command %s\n", pcmd);
	    }
	    FREECMD (cmd, pcmd);
            if(lcmd) break;
	}
	if(!lcmd && isatty(fileno(stdout)))
            fputs ("quit\n", stdout);
    }
    else
    {
        ShowCmdHelp();
        return 1;   /* failed to open the file/partition */
    }

    return 0;
}
