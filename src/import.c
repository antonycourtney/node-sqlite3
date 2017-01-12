
#include "import.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>

/*
** Render output like fprintf().  Except, if the output is going to the
** console and if this is running on a Windows machine, translate the
** output from UTF-8 into MBCS.
*/
#if defined(_WIN32) || defined(WIN32)
void utf8_printf(FILE *out, const char *zFormat, ...){
  va_list ap;
  va_start(ap, zFormat);
  if( stdout_is_console && (out==stdout || out==stderr) ){
    char *z1 = sqlite3_vmprintf(zFormat, ap);
    char *z2 = sqlite3_win32_utf8_to_mbcs_v2(z1, 0);
    sqlite3_free(z1);
    fputs(z2, out);
    sqlite3_free(z2);
  }else{
    vfprintf(out, zFormat, ap);
  }
  va_end(ap);
}
#elif !defined(utf8_printf)
# define utf8_printf fprintf
#endif

/*
** Render output like fprintf().  This should not be used on anything that
** includes string formatting (e.g. "%s").
*/
#if !defined(raw_printf)
# define raw_printf fprintf
#endif

/*
** Compute a string length that is limited to what can be stored in
** lower 30 bits of a 32-bit signed integer.
*/
static int strlen30(const char *z){
  const char *z2 = z;
  while( *z2 ){ z2++; }
  return 0x3fffffff & (int)(z2 - z);
}

/*
** True if an interrupt (Control-C) has been received.
*/
static volatile int seenInterrupt = 0;

/*
** subset of ShellState we need for csv import code
*/
/*
** State information about the database connection is contained in an
** instance of the following structure.
*/
typedef struct ShellState ShellState;
struct ShellState {
  int mode;              /* An output mode setting */
  char colSeparator[20]; /* Column separator character for several modes */
  char rowSeparator[20]; /* Row separator character for MODE_Ascii */
};

/*
** These are the allowed modes.
*/
#define MODE_Line     0  /* One column per line.  Blank line between records */
#define MODE_Column   1  /* One record per line in neat columns */
#define MODE_List     2  /* One record per line with a separator */
#define MODE_Semi     3  /* Same as MODE_List but append ";" to each line */
#define MODE_Html     4  /* Generate an XHTML table */
#define MODE_Insert   5  /* Generate SQL "insert" statements */
#define MODE_Tcl      6  /* Generate ANSI-C or TCL quoted elements */
#define MODE_Csv      7  /* Quote strings, numbers are plain */
#define MODE_Explain  8  /* Like MODE_Column, but do not truncate data */
#define MODE_Ascii    9  /* Use ASCII unit and record separators (0x1F/0x1E) */
#define MODE_Pretty  10  /* Pretty-print schemas */

/*
** These are the column/row/line separators used by the various
** import/export modes.
*/
#define SEP_Column    "|"
#define SEP_Row       "\n"
#define SEP_Tab       "\t"
#define SEP_Space     " "
#define SEP_Comma     ","
#define SEP_CrLf      "\r\n"
#define SEP_Unit      "\x1F"
#define SEP_Record    "\x1E"

/*
** An object used to read a CSV and other files for import.
*/
typedef struct ImportCtx ImportCtx;
struct ImportCtx {
  const char *zFile;  /* Name of the input file */
  FILE *in;           /* Read the CSV text from this input stream */
  char *z;            /* Accumulated text for a field */
  int n;              /* Number of bytes in z */
  int nAlloc;         /* Space allocated for z[] */
  int nLine;          /* Current line number */
  int cTerm;          /* Character that terminated the most recent field */
  int cColSep;        /* The column separator character.  (Usually ",") */
  int cRowSep;        /* The row separator character.  (Usually "\n") */
};

/* Append a single byte to z[] */
static void import_append_char(ImportCtx *p, int c){
  if( p->n+1>=p->nAlloc ){
    p->nAlloc += p->nAlloc + 100;
    p->z = sqlite3_realloc64(p->z, p->nAlloc);
    if( p->z==0 ){
      raw_printf(stderr, "out of memory\n");
      exit(1);
    }
  }
  p->z[p->n++] = (char)c;
}

/* Read a single field of CSV text.  Compatible with rfc4180 and extended
** with the option of having a separator other than ",".
**
**   +  Input comes from p->in.
**   +  Store results in p->z of length p->n.  Space to hold p->z comes
**      from sqlite3_malloc64().
**   +  Use p->cSep as the column separator.  The default is ",".
**   +  Use p->rSep as the row separator.  The default is "\n".
**   +  Keep track of the line number in p->nLine.
**   +  Store the character that terminates the field in p->cTerm.  Store
**      EOF on end-of-file.
**   +  Report syntax errors on stderr
*/
static char *SQLITE_CDECL csv_read_one_field(ImportCtx *p){
  int c;
  int cSep = p->cColSep;
  int rSep = p->cRowSep;
  p->n = 0;
  c = fgetc(p->in);
  if( c==EOF || seenInterrupt ){
    p->cTerm = EOF;
    return 0;
  }
  if( c=='"' ){
    int pc, ppc;
    int startLine = p->nLine;
    int cQuote = c;
    pc = ppc = 0;
    while( 1 ){
      c = fgetc(p->in);
      if( c==rSep ) p->nLine++;
      if( c==cQuote ){
        if( pc==cQuote ){
          pc = 0;
          continue;
        }
      }
      if( (c==cSep && pc==cQuote)
       || (c==rSep && pc==cQuote)
       || (c==rSep && pc=='\r' && ppc==cQuote)
       || (c==EOF && pc==cQuote)
      ){
        do{ p->n--; }while( p->z[p->n]!=cQuote );
        p->cTerm = c;
        break;
      }
      if( pc==cQuote && c!='\r' ){
        utf8_printf(stderr, "%s:%d: unescaped %c character\n",
                p->zFile, p->nLine, cQuote);
      }
      if( c==EOF ){
        utf8_printf(stderr, "%s:%d: unterminated %c-quoted field\n",
                p->zFile, startLine, cQuote);
        p->cTerm = c;
        break;
      }
      import_append_char(p, c);
      ppc = pc;
      pc = c;
    }
  }else{
    while( c!=EOF && c!=cSep && c!=rSep ){
      import_append_char(p, c);
      c = fgetc(p->in);
    }
    if( c==rSep ){
      p->nLine++;
      if( p->n>0 && p->z[p->n-1]=='\r' ) p->n--;
    }
    p->cTerm = c;
  }
  if( p->z ) p->z[p->n] = 0;
  return p->z;
}

/* Read a single field of ASCII delimited text.
**
**   +  Input comes from p->in.
**   +  Store results in p->z of length p->n.  Space to hold p->z comes
**      from sqlite3_malloc64().
**   +  Use p->cSep as the column separator.  The default is "\x1F".
**   +  Use p->rSep as the row separator.  The default is "\x1E".
**   +  Keep track of the row number in p->nLine.
**   +  Store the character that terminates the field in p->cTerm.  Store
**      EOF on end-of-file.
**   +  Report syntax errors on stderr
*/
static char *SQLITE_CDECL ascii_read_one_field(ImportCtx *p){
  int c;
  int cSep = p->cColSep;
  int rSep = p->cRowSep;
  p->n = 0;
  c = fgetc(p->in);
  if( c==EOF || seenInterrupt ){
    p->cTerm = EOF;
    return 0;
  }
  while( c!=EOF && c!=cSep && c!=rSep ){
    import_append_char(p, c);
    c = fgetc(p->in);
  }
  if( c==rSep ){
    p->nLine++;
  }
  p->cTerm = c;
  if( p->z ) p->z[p->n] = 0;
  return p->z;
}

int sqlite_import(sqlite3 *db, const char *zFile, const char *zTable) {
  struct ShellState ss;
  struct ShellState *p = &ss;  /* TODO: replace */
  int rc = 0;
  sqlite3_stmt *pStmt = NULL; /* A statement */
  int nCol;                   /* Number of columns in the table */
  int nByte;                  /* Number of bytes in an SQL string */
  int i, j;                   /* Loop counters */
  int needCommit;             /* True to COMMIT or ROLLBACK at end */
  int nSep;                   /* Number of bytes in p->colSeparator[] */
  char *zSql;                 /* An SQL statement */
  ImportCtx sCtx;             /* Reader context */
  char *(SQLITE_CDECL *xRead)(ImportCtx*); /* Func to read one value */
  int (SQLITE_CDECL *xCloser)(FILE*);      /* Func to close file */

  p->mode = MODE_Csv;
  sqlite3_snprintf(sizeof(p->colSeparator), p->colSeparator, SEP_Comma);
  sqlite3_snprintf(sizeof(p->rowSeparator), p->rowSeparator, SEP_CrLf);
  seenInterrupt = 0;
  memset(&sCtx, 0, sizeof(sCtx));
  nSep = strlen30(p->colSeparator);
  if( nSep==0 ){
    raw_printf(stderr,
               "Error: non-null column separator required for import\n");
    return 1;
  }
  if( nSep>1 ){
    raw_printf(stderr, "Error: multi-character column separators not allowed"
                    " for import\n");
    return 1;
  }
  nSep = strlen30(p->rowSeparator);
  if( nSep==0 ){
    raw_printf(stderr, "Error: non-null row separator required for import\n");
    return 1;
  }
  if( nSep==2 && p->mode==MODE_Csv && strcmp(p->rowSeparator, SEP_CrLf)==0 ){
    /* When importing CSV (only), if the row separator is set to the
    ** default output row separator, change it to the default input
    ** row separator.  This avoids having to maintain different input
    ** and output row separators. */
    sqlite3_snprintf(sizeof(p->rowSeparator), p->rowSeparator, SEP_Row);
    nSep = strlen30(p->rowSeparator);
  }
  if( nSep>1 ){
    raw_printf(stderr, "Error: multi-character row separators not allowed"
                    " for import\n");
    return 1;
  }
  sCtx.zFile = zFile;
  sCtx.nLine = 1;
  if( sCtx.zFile[0]=='|' ){
  #ifdef SQLITE_OMIT_POPEN
    raw_printf(stderr, "Error: pipes are not supported in this OS\n");
    return 1;
  #else
    sCtx.in = popen(sCtx.zFile+1, "r");
    sCtx.zFile = "<pipe>";
    xCloser = pclose;
  #endif
  }else{
    sCtx.in = fopen(sCtx.zFile, "rb");
    xCloser = fclose;
  }
  if( p->mode==MODE_Ascii ){
    xRead = ascii_read_one_field;
  }else{
    xRead = csv_read_one_field;
  }
  if( sCtx.in==0 ){
    utf8_printf(stderr, "Error: cannot open \"%s\"\n", zFile);
    return 1;
  }
  sCtx.cColSep = p->colSeparator[0];
  sCtx.cRowSep = p->rowSeparator[0];
  zSql = sqlite3_mprintf("SELECT * FROM %s", zTable);
  if( zSql==0 ){
    raw_printf(stderr, "Error: out of memory\n");
    xCloser(sCtx.in);
    return 1;
  }
  nByte = strlen30(zSql);
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  import_append_char(&sCtx, 0);    /* To ensure sCtx.z is allocated */
  if( rc && sqlite3_strglob("no such table: *", sqlite3_errmsg(db))==0 ){
    char *zCreate = sqlite3_mprintf("CREATE TABLE %s", zTable);
    char cSep = '(';
    while( xRead(&sCtx) ){
      zCreate = sqlite3_mprintf("%z%c\n  \"%w\" TEXT", zCreate, cSep, sCtx.z);
      cSep = ',';
      if( sCtx.cTerm!=sCtx.cColSep ) break;
    }
    if( cSep=='(' ){
      sqlite3_free(zCreate);
      sqlite3_free(sCtx.z);
      xCloser(sCtx.in);
      utf8_printf(stderr,"%s: empty file\n", sCtx.zFile);
      return 1;
    }
    zCreate = sqlite3_mprintf("%z\n)", zCreate);
    rc = sqlite3_exec(db, zCreate, 0, 0, 0);
    sqlite3_free(zCreate);
    if( rc ){
      utf8_printf(stderr, "CREATE TABLE %s(...) failed: %s\n", zTable,
              sqlite3_errmsg(db));
      sqlite3_free(sCtx.z);
      xCloser(sCtx.in);
      return 1;
    }
    rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  }
  sqlite3_free(zSql);
  if( rc ){
    if (pStmt) sqlite3_finalize(pStmt);
    utf8_printf(stderr,"Error: %s\n", sqlite3_errmsg(db));
    xCloser(sCtx.in);
    return 1;
  }
  nCol = sqlite3_column_count(pStmt);
  sqlite3_finalize(pStmt);
  pStmt = 0;
  if( nCol==0 ) return 0; /* no columns, no error */
  zSql = sqlite3_malloc64( nByte*2 + 20 + nCol*2 );
  if( zSql==0 ){
    raw_printf(stderr, "Error: out of memory\n");
    xCloser(sCtx.in);
    return 1;
  }
  sqlite3_snprintf(nByte+20, zSql, "INSERT INTO \"%w\" VALUES(?", zTable);
  j = strlen30(zSql);
  for(i=1; i<nCol; i++){
    zSql[j++] = ',';
    zSql[j++] = '?';
  }
  zSql[j++] = ')';
  zSql[j] = 0;
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);
  if( rc ){
    utf8_printf(stderr, "Error: %s\n", sqlite3_errmsg(db));
    if (pStmt) sqlite3_finalize(pStmt);
    xCloser(sCtx.in);
    return 1;
  }
  needCommit = sqlite3_get_autocommit(db);
  if( needCommit ) sqlite3_exec(db, "BEGIN", 0, 0, 0);
  do{
    int startLine = sCtx.nLine;
    for(i=0; i<nCol; i++){
      char *z = xRead(&sCtx);
      /*
      ** Did we reach end-of-file before finding any columns?
      ** If so, stop instead of NULL filling the remaining columns.
      */
      if( z==0 && i==0 ) break;
      /*
      ** Did we reach end-of-file OR end-of-line before finding any
      ** columns in ASCII mode?  If so, stop instead of NULL filling
      ** the remaining columns.
      */
      if( p->mode==MODE_Ascii && (z==0 || z[0]==0) && i==0 ) break;
      sqlite3_bind_text(pStmt, i+1, z, -1, SQLITE_TRANSIENT);
      if( i<nCol-1 && sCtx.cTerm!=sCtx.cColSep ){
        utf8_printf(stderr, "%s:%d: expected %d columns but found %d - "
                        "filling the rest with NULL\n",
                        sCtx.zFile, startLine, nCol, i+1);
        i += 2;
        while( i<=nCol ){ sqlite3_bind_null(pStmt, i); i++; }
      }
    }
    if( sCtx.cTerm==sCtx.cColSep ){
      do{
        xRead(&sCtx);
        i++;
      }while( sCtx.cTerm==sCtx.cColSep );
      utf8_printf(stderr, "%s:%d: expected %d columns but found %d - "
                      "extras ignored\n",
                      sCtx.zFile, startLine, nCol, i);
    }
    if( i>=nCol ){
      sqlite3_step(pStmt);
      rc = sqlite3_reset(pStmt);
      if( rc!=SQLITE_OK ){
        utf8_printf(stderr, "%s:%d: INSERT failed: %s\n", sCtx.zFile,
                    startLine, sqlite3_errmsg(db));
      }
    }
  }while( sCtx.cTerm!=EOF );

  xCloser(sCtx.in);
  sqlite3_free(sCtx.z);
  sqlite3_finalize(pStmt);
  if( needCommit ) sqlite3_exec(db, "COMMIT", 0, 0, 0);

  return 0;
}
