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
 * \brief Millisecond CPU stopwatch for internal timing
 *
 *  Return the elapsed user+system CPU time in milliseconds
 *  since the most recent call.  Very much not thread-safe.
 */

#if !defined(HOST_WIN) && !defined(WINNT) && !defined(WIN64) && !defined(WIN32) && !defined(HOST_MINGW)
#include <sys/times.h>
#include <unistd.h>
#else
#include <sys/timeb.h>
#include <sys/types.h>
#include <time.h>
struct tms
  {
    clock_t tms_utime;          /* User CPU time.  */
    clock_t tms_stime;          /* System CPU time.  */
 
    clock_t tms_cutime;         /* User CPU time of dead children.  */
    clock_t tms_cstime;         /* System CPU time of dead children.  */
  };
clock_t times (struct tms *__buffer) {
 
	__buffer->tms_utime = clock();
	__buffer->tms_stime = 0;
	__buffer->tms_cstime = 0;
	__buffer->tms_cutime = 0;
	return __buffer->tms_utime;
}
#endif
#include "scutil.h"

unsigned long
getcpu(void)
{
  static long ticks_per_second = -1;
  static unsigned long last = 0;

  struct tms tms;
  unsigned long now, elapsed;

  /* Initialize ticks_per_second. */
#ifdef _SC_CLK_TCK
  if (ticks_per_second <= 0)
    ticks_per_second = sysconf(_SC_CLK_TCK);
#endif /* _SC_CLK_TCK */
  if (ticks_per_second <= 0)
    ticks_per_second = 60; /* a traditional UNIX "jiffy" */

  times(&tms);
  now = tms.tms_utime + tms.tms_stime;
  now *= 1000; /* milliseconds */
  now /= ticks_per_second;

  elapsed = now - last;
  last = now;
  return elapsed;
}
