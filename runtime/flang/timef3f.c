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

/* clang-format off */

/*	timef3f.c - Implements timef subprogram.  */

/* assumes the Unix times system call */
/* how do we do this for WINNT */
#include "ent3f.h"

#define _LIBC_LIMITS_H_
#if !defined(HOST_WIN) && !defined(WINNT) && !defined(WIN64) && !defined(WIN32) && !defined(HOST_MINGW)
#include <unistd.h>
#endif
#include <sys/types.h>
#if !defined(HOST_WIN) && !defined(WINNT) && !defined(WIN64) && !defined(WIN32) && !defined(HOST_MINGW)
#include <sys/times.h>
#else
#include <sys/timeb.h>
#include <sys/types.h>
#include <winsock2.h>
 
int gettimeofday(struct timeval* t,void* timezone);
 
// from linux's sys/times.h
 
//#include <features.h>
 
#define __need_clock_t
#include <time.h>
 
 
/* Structure describing CPU time used by a process and its children.  */
struct tms
  {
    clock_t tms_utime;          /* User CPU time.  */
    clock_t tms_stime;          /* System CPU time.  */
 
    clock_t tms_cutime;         /* User CPU time of dead children.  */
    clock_t tms_cstime;         /* System CPU time of dead children.  */
  };
 
/* Store the CPU time used by this process and all its
   dead children (and their dead children) in BUFFER.
   Return the elapsed real time, or (clock_t) -1 for errors.
   All times are in CLK_TCKths of a second.  */
clock_t times (struct tms *__buffer);
 
typedef long long suseconds_t ;

#endif
#include <limits.h>

#ifndef CLK_TCK
#define CLK_TCK sysconf(_SC_CLK_TCK)
#endif

static clock_t start = 0;

double ENT3F(TIMEF, timef)(float *tarray)
{
  struct tms b;
  clock_t current;
  double duration;
  double inv_ticks = 1 / (double)CLK_TCK;

  times(&b);
  if (start == 0) {
    start = b.tms_utime + b.tms_stime;
    current = start;
  } else
    current = b.tms_utime + b.tms_stime;

  duration = ((double)(current - start)) * inv_ticks;
  return duration;
}

#if defined(HOST_WIN) || defined(WINNT) || defined(WIN64) || defined(WIN32) || defined(HOST_MINGW)
int gettimeofday(struct timeval* t,void* timezone)
{       struct _timeb timebuffer;
        _ftime( &timebuffer );
        t->tv_sec=timebuffer.time;
        t->tv_usec=1000*timebuffer.millitm;
		return 0;
}
 
clock_t times (struct tms *__buffer) {
 
	__buffer->tms_utime = clock();
	__buffer->tms_stime = 0;
	__buffer->tms_cstime = 0;
	__buffer->tms_cutime = 0;
	return __buffer->tms_utime;
}
#endif