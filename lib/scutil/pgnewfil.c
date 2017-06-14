/*
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/*
 * create a new file name
 * arguments are a prefix and suffix
 * test that the file does not exist
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(HOST_OSX)
#include <string.h>
#endif

/* #if defined(USETEMPNAM) || defined(HOST_WIN) || defined(WIN64) ||
 * defined(WIN32) || defined(HOST_MINGW) */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#if defined(HOST_WIN) || defined(WINNT) || defined(WIN64) || defined(WIN32) || defined(HOST_MINGW)
#include <direct.h>
#include <io.h>
extern unsigned long getpid(void);
#else
#include <unistd.h>
#endif

#if DEBUG
int pgnewfil_debug = 0;
#endif
extern size_t strlen();

/*
 * copy chars from q to p, terminate string, return end of string
 */
static char *
add(char *p, char *q)
{
  while (*q != '\0')
    *p++ = *q++;
  *p = '\0';
  return p;
} /* add */

/* 64 characters to use */
static char chars[] = {
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-+"};

/*
 * add 'n' chars based on a number value
 */
static char *
addn(char *p, unsigned long val, int n)
{
  unsigned long d;
  int i;

  i = 0;
  while (i++ < n) {
    d = val & 0x3f;
    val >>= 6;
    *p++ = chars[d];
    if (val == 0)
      break;
  }
  *p = '\0';
  return p;
} /* addn */

static int next = 0; /* counter of files created */
static long pgrand = 0;
static unsigned long pid = 0;

#if defined(USETEMPNAM) || defined(HOST_WIN) || defined(WIN64) || defined(WIN32) || defined(HOST_MINGW) /* { */

/*
 * call tempnam to generate a filename using the prefix
 * then append the suffix to that
 */
static char *
gentmp(char *pfx, char *sfx)
{
  char *nulldir = (char *)0;
  char *tempname, *filename, *p;
  unsigned long sec;
  int l;
  extern long time(long *);
  tempname = tempnam(nulldir, pfx);
  if (tempname == NULL)
    return NULL;
  l = strlen(tempname) + 32;
  if (sfx)
    l += strlen(sfx);
  filename = (char *)malloc(l);
  if (filename == NULL)
    return NULL;
  if (pgrand == 0) { /* first time, create seed */
    char *q;
    q = getenv("PATH");
    pgrand = (long)q;
    q = getenv("USER");
    if (q != NULL) {
      int n = 0;
      while (*q != '\0') {
        pgrand ^= (long)(*q++) << n++;
      }
    }
    pgrand ^= (long)filename >> 4;
    pgrand ^= time((long *)0);
    pid = getpid();
#if DEBUG
    if (pgnewfil_debug & 2) {
      /* for testing, make this as 'nonrandom' as possible */
      pgrand = 1;
      pid = 2;
    }
#endif
  }
  p = add(filename, tempname);
#if DEBUG
  if (pgnewfil_debug & 2) {
    /* for testing, make eliminate the random part of the temp name */
    char *q, *last;
    last = NULL;
    for (q = filename; *q; ++q) {
      if (*q == '/'
#if defined(HOST_WIN) || defined(WINNT) || defined(WIN64) || defined(WIN32) || defined(HOST_MINGW)
          || *(p - 1) != '\\'
#endif
          ) {
        last = q;
      }
    }
    if (last)
      *(last + 1) = '\0';
  }
#endif
  p = addn(p, next++, 4);
  *p++ = chars[pgrand & 0x3f];
  p = addn(p, pid, 8);
  pgrand = (pgrand << 16) + pgrand * 3;
  p = addn(p, pgrand, 4);
  pgrand = (pgrand << 16) + pgrand * 3;
  p = addn(p, pgrand ^ (((long)(pfx) ^ (long)(sfx)) >> 5), 4);
  if (sfx)
    p = add(p, sfx);
  return filename;
} /* gentmp */

#else /* }else{ */

extern long time(long *);
#if !defined(P_tmpdir)
#define P_tmpdir "/tmp"
#endif

/*
 * use the PID, TIME, the address of a the arguments,
 * the address of a dynamically allocated buffer,
 * and a counter to generate a filename,
 * prepending the prefix and appending the suffix to that
 */
static char *
gentmp(char *pfx, char *sfx)
{
  char *nulldir = (char *)0;
  char *filename;
  char *tmpdir;
  char *p, *q;
  int l;

  tmpdir = getenv("TMPDIR");
  if (tmpdir != NULL && tmpdir[0] != '\0') {
    int err;
    struct stat buf;
    err = stat(tmpdir, &buf);
    if (err != 0 || !S_ISDIR(buf.st_mode)) {
      /* no such directory */
      tmpdir = NULL;
    }
  }
  if (tmpdir == NULL || tmpdir[0] == '\0') {
    tmpdir = getenv("TMP");
    if (tmpdir != NULL && tmpdir[0] != '\0') {
      int err;
      struct stat buf;
      err = stat(tmpdir, &buf);
      if (err != 0 || !S_ISDIR(buf.st_mode)) {
        /* no such directory */
        tmpdir = NULL;
      }
    }
  }
  if (tmpdir == NULL || tmpdir[0] == '\0') {
    tmpdir = P_tmpdir;
  }
  l = strlen(tmpdir) + 32;
  if (pfx)
    l += strlen(pfx);
  if (sfx)
    l += strlen(sfx);
  filename = (char *)malloc(l);
  filename[0] = '\0';
  if (pgrand == 0) { /* first time, create seed */
    q = getenv("PATH");
    pgrand = (long)q;
    q = getenv("USER");
    if (q == NULL)
      q = getenv("USERNAME");
    if (q != NULL) {
      int n = 0;
      while (*q != '\0') {
        pgrand ^= (long)(*q++) << n++;
      }
    }
    pgrand ^= (long)filename >> 4;
    pgrand ^= time((long *)0);
    pid = getpid();
#if DEBUG
    if (pgnewfil_debug & 2) {
      /* for testing, make this as 'nonrandom' as possible */
      pgrand = 1;
      pid = 2;
    }
#endif
  }
  p = add(filename, tmpdir);
  if (*(p - 1) != '/'
#if defined(HOST_WIN) || defined(WINNT) || defined(WIN64) || defined(WIN32) || defined(HOST_MINGW)
      && *(p - 1) != '\\'
#endif
      )
    p = add(p, "/");
  if (pfx) {
    p = add(p, pfx);
  }
  *p++ = chars[pgrand & 0x3f];
  p = addn(p, pid, 8);
  pgrand = (pgrand << 16) + pgrand * 3;
  p = addn(p, pgrand, 4);
  pgrand = (pgrand << 16) + pgrand * 3;
  p = addn(p, pgrand ^ (((long)(pfx) ^ (long)(sfx)) >> 5), 4);

  if (sfx != NULL)
    p = add(p, sfx);
  return filename;
} /* gentmp */

#endif /* } */

/*
 * generate a new filename until we find one that doesn't exist
 */
char *
pg_newfile(char *pfx, char *sfx)
{
  char *filename;
  int fd, r;

  while (1) {
    filename = gentmp(pfx, sfx);
    if (filename == NULL)
      break;
    r = access(filename, 0);
    if (r == -1 && errno == ENOENT)
      break;
    free(filename); /* it was allocated */
    filename = NULL;
    if (r == -1) /* could not access it for some other reason */
      break;
  }
#if DEBUG
  if (pgnewfil_debug & 1) {
    fprintf(stderr, "pg_newfil(%s,%s) returns %s\n", pfx ? pfx : "",
            sfx ? sfx : "", filename ? filename : "");
  }
#endif
  return filename;
} /* pg_newfile */

/*
 * generate a new filename until we find one that doesn't exist
 */
char *
pg_makenewfile(char *pfx, char *sfx, int make)
{
  char *filename;
  int fd, r;

  while (1) {
    filename = gentmp(pfx, sfx);
    if (filename == NULL)
      break;
    r = access(filename, 0);
    if (r == -1 && errno == ENOENT) {
      if (!make) {
        break;
      } else {
#if defined(HOST_WIN) || defined(WINNT) || defined(WIN64) || defined(WIN32) || defined(HOST_MINGW)
        fd = _open(filename, _O_CREAT | _O_BINARY | _O_EXCL | _O_RDWR, _S_IWRITE);
#else
        fd = open(filename, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
#endif
        if (fd >= 0) {
          /* we have exclusive access, write something to create a nonempty file
           */
          int ee;
          ee = write(fd, "pgnf", 4);
          close(fd);
          break;
        }
      }
    } else if (r == -1) {
      free(filename); /* it was allocated */
      filename = NULL;
      break; /* could not create temp file */
    }
    free(filename); /* it was allocated */
  }
#if DEBUG
  if (pgnewfil_debug & 1) {
    fprintf(stderr, "pg_makenewfile(%s,%s,%d) returns %s\n", pfx ? pfx : "",
            sfx ? sfx : "", make, filename ? filename : "");
  }
#endif
  return filename;
} /* pg_makenewfile */

/*
 * generate a new filename until we find one that doesn't exist
 */
char *
pg_makenewdir(char *pfx, char *sfx, int make)
{
  char *filename;
  int fd, r;

  while (1) {
    filename = gentmp(pfx, sfx);
    if (filename == NULL)
      break;
    r = access(filename, 0);
    if (r == -1 && errno == ENOENT) {
      if (make) {
        int err;
#if defined(HOST_WIN) || defined(WINNT) || defined(WIN64) || defined(WIN32) || defined(HOST_MINGW)
        err = _mkdir(filename);
#else
        err = mkdir(filename, S_IRWXG | S_IRWXO | S_IXUSR | S_IWUSR | S_IRUSR);
#endif
        if (err < 0) {
          perror("mkdir");
        }
      }
      break;
    } else if (r == -1) {
      free(filename);
      filename = NULL;
      break;
    }
    free(filename); /* it was allocated */
  }
#if DEBUG
  if (pgnewfil_debug & 1) {
    fprintf(stderr, "pg_makenewdir(%s,%s,%d) returns %s\n", pfx ? pfx : "",
            sfx ? sfx : "", make, filename ? filename : "");
  }
#endif
  return filename;
} /* pg_makenewdir */
