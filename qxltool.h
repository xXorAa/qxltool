/*--------------------------------------------------------------.
| qxltool.h : Access QXL.WIN files from other operating systems |
|                                                               |
| (c) Jonathan Hudson, April 1998                               |
| No Warranty                                                   |
`--------------------------------------------------------------*/

#ifndef _QXLTOOL_H
#define _QXLTOOL_H

#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include "version.h"

#define UNIX2QL    283996800
#define QL2UNIX   -(UNIX2QL)

#define VERSTR   VERSION ", " __DATE__

#if defined __GNUC__
#define PACKED  __attribute__ ((packed))
#else
#define PACKED
#endif

#ifdef __unix__
# define DEFQXL "/dos/QXL.WIN"
# define SHELL "SHELL"
# define TARGET "Linux"
#elif defined (__WINNT)
# define DEFQXL "C:\\QXL.WIN"
# define SHELL "COMSPEC" 
# define TARGET "WIN32"
#else
# define DEFQXL "win2_tmp_qxl.win"
# define TARGET "QDOS"
# define SHELL  "SHELL"
#endif

#ifdef NEED_U_CHAR
 typedef uint8_t u_char;
#endif

#ifdef NEED_U_SHORT
 typedef uint16_t u_short;
#endif
#ifdef NEED_U_LONG
 typedef uint32_t u_long;
#endif

#ifdef HAVE_READLINE_READLINE_H
# include <readline/readline.h>
#endif

#ifdef HAVE_READLINE_HISTORY_H
# include <readline/history.h>
#else
// Oh dear cygwin B20 defines readline.h but not history.h
extern void add_history(char *);
#endif

#define QLPATH_MAX 36

typedef struct
{
    char id[4];
    uint16_t namlen;
    u_char name[20];
    uint16_t dummy0 PACKED;
    uint16_t rand;
    uint16_t access;
    uint16_t dummy1;
    uint16_t sectc;
    uint16_t dummy2[3];
    uint16_t total;
    uint16_t free;
    uint16_t formatted;
    uint16_t dummy3;
    uint16_t first;
    uint16_t direct;
    uint32_t dlen PACKED; 
    uint16_t dummy4[3];
    uint16_t map[1];
} HEADER;

typedef struct
{
    uint32_t length;
    uint16_t type;
    uint32_t data PACKED;
    uint32_t  dummy1 PACKED;
    uint16_t nlen;
    u_char name[QLPATH_MAX];
    uint32_t date PACKED;
    uint16_t dummy2;
    uint16_t map;
    uint32_t dummy3;
} QLDIR;

typedef struct 
{
    union 
    {
        char xtcc[4];
        int32_t x;
    } x;
    uint32_t dlen;
} XTCC;

/* On-disk structures must match the QXL.WIN layout exactly */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert (sizeof (QLDIR) == 64, "QLDIR must be 64 bytes");
_Static_assert (offsetof (HEADER, map) == 64, "HEADER map must be at offset 64");
_Static_assert (sizeof (XTCC) == 8, "XTCC must be 8 bytes");
#endif

typedef struct
{
    int fd;
    HEADER h;
    QLDIR curd;
    QLDIR root;
    QLDIR tmpd;    
    QLDIR lastd;
    FILE *fp;
    short close;
    char fn[PATH_MAX];
    int mode;
    short fmode;
    /* Partition window: byte offset of the filesystem within the image, and
       whether the media is 16-bit byte-swapped (Q40 card).  base==0/swap==0
       means a plain .WIN file, preserving legacy behaviour. */
    off_t base;
    int   swap;
} QXL;

#ifndef O_BINARY
# define O_BINARY 0
#endif

#ifndef offsetof
# define offsetof(sname,fname) ((long)&((sname *)0)->fname)
#endif

/* Callbacks may return a pointer (QLDIR *) so must be pointer sized */
typedef intptr_t (*PCALLBACK)(QXL *, QLDIR *, void *, void *, uint16_t);
typedef int (*ACTFUNC)(QXL *, short , char **);

typedef struct
{
    char *name;
    ACTFUNC func;
    short flag;
} JTBL;

#define DO_RECURSE (1 << 0)
#define DO_BEST    (1 << 1)
#define DO_WILD    (1 << 2)

#define QX_TEXT    (1 << 0)
#define QX_OPEN    (1 << 1)
#define QX_FAIL_SILENTLY (1 << 14)
#endif
