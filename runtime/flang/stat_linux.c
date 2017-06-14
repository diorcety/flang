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

/** \file
 * \brief Fill in statistics structure (Linux version)
 */
#if !defined(HOST_WIN) && !defined(WINNT) && !defined(WIN64) && !defined(WIN32) && !defined(HOST_MINGW)
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <string.h>
#include <unistd.h>
#include "timeBlk.h"
#include "fioMacros.h"

union ieee {
  double d;
  int i[2];
};

extern void *__fort_sbrk(int);

/* these little routines had to go somewhere, so here they are. */

void
__fort_setarg(void)
{
}

static void nodename(s) char *s;
{
  struct utsname u0;

  uname(&u0);
  strcpy(s, u0.nodename);
}

void __fort_gettb(t) struct tb *t;
{
  struct timeval tv0;
  struct timezone tz0;
  struct rusage rs0, rc0;

  /* Use an approximation here to avoid using inexact arithmetic */
  unsigned long long tapprox;
  union ieee v;

  gettimeofday(&tv0, &tz0);
  getrusage(RUSAGE_SELF, &rs0);
  getrusage(RUSAGE_CHILDREN, &rc0);
  v.i[0] = 0;
  v.i[1] = 0x3d700000;                       /* 2 ** -40 */
  tapprox = (unsigned long long)tv0.tv_usec; /* around 2**30 */
  tapprox = tapprox * 1099512UL;             /* mpy by a 21 bit value */
  tapprox &= 0xfffffffffffc0000ULL;          /* Lop off enough to be exact */
  t->r = (double)tv0.tv_sec + v.d * (double)tapprox;
  /* printf("BDL %d %d %22.15le\n",tv0.tv_sec,tv0.tv_usec,t->r); */

  tapprox = rs0.ru_utime.tv_usec;
  tapprox = tapprox * 1099512UL;
  tapprox &= 0xfffffffffffc0000ULL;
  t->u = (double)rs0.ru_utime.tv_sec + v.d * (double)tapprox;

  tapprox = rs0.ru_stime.tv_usec;
  tapprox = tapprox * 1099512UL;
  tapprox &= 0xfffffffffffc0000ULL;
  t->s = (double)rs0.ru_stime.tv_sec + v.d * (double)tapprox;

  tapprox = rc0.ru_utime.tv_usec;
  tapprox = tapprox * 1099512UL;
  tapprox &= 0xfffffffffffc0000ULL;
  t->u += (double)rc0.ru_utime.tv_sec + v.d * (double)tapprox;

  tapprox = rc0.ru_stime.tv_usec;
  tapprox = tapprox * 1099512UL;
  tapprox &= 0xfffffffffffc0000ULL;
  t->s += (double)rc0.ru_stime.tv_sec + v.d * (double)tapprox;
  t->maxrss = rs0.ru_maxrss;
  t->minflt = rs0.ru_minflt;
  t->majflt = rs0.ru_majflt;
  t->nsignals = rs0.ru_nsignals;
  t->nvcsw = rs0.ru_nvcsw;
  t->nivcsw = rs0.ru_nivcsw;
  t->sbrk = (double)((long)sbrk(0));
  t->gsbrk = (GET_DIST_HEAPZ == 0 ? 0.0 : (double)((long)__fort_sbrk(0)));
  nodename(t->host);
}

static double first = 0.0;

double
__fort_second()
{
  struct timeval v;
  struct timezone t;
  double d;
  int s;

  s = gettimeofday(&v, &t);
  if (s == -1) {
    __fort_abortp("gettimeofday");
  }
  d = (double)v.tv_sec + (double)v.tv_usec / 1000000;
  if (first == 0.0) {
    first = d;
  }
  return (d - first);
}

void
__fort_set_second(double d)
{
  first = d;
}
#else
	
#include <windows.h>
void
__fort_setarg(void)
{
}

static double first = 0.0;

double
__fort_second()
{
  double d;
  FILETIME ft;
  ULARGE_INTEGER ul;
  GetSystemTimeAsFileTime(&ft);
  // Fill ULARGE_INTEGER low and high parts.
  ul.LowPart = ft.dwLowDateTime;
  ul.HighPart = ft.dwHighDateTime;
  // Convert to microseconds.
  ul.QuadPart /= 10ULL;
  // Remove Windows to UNIX Epoch delta.
  ul.QuadPart -= 11644473600000000ULL;
  // Modulo to retrieve the microseconds.
  d = (double)ul.QuadPart / 1000000;
  if (first == 0.0) {
    first = d;
  }
  return (d - first);
}

void
__fort_set_second(double d)
{
  first = d;
}

void __fort_gettb(t) struct tb *t;
{
}

#endif


