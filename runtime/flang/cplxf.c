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

#include "stdioInterf.h"
#include "fioMacros.h"
#include <time.h>
#if !defined(HOST_WIN) && !defined(WINNT) && !defined(WIN64) && !defined(WIN32) && !defined(HOST_MINGW)
#include <sys/time.h>
#include <unistd.h>
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

extern double __fort_second();
extern long __fort_getoptn(char *, long);

/*
 * Hacks to return complex-valued functions.
 * mergec & mergedc for the Cray are defined in miscsup_com.c.
 */

/*
 * For these targets, the first argument is a pointer to a complex
 * temporary in which the value of the complex function is stored.
 */

typedef struct {
  float real;
  float imag;
} cmplx_t;

typedef struct {
  double real;
  double imag;
} dcmplx_t;

void ENTF90(MERGEC, mergec)(cmplx_t *res, cmplx_t *tsource, cmplx_t *fsource,
                            void *mask, __INT_T *size)
{
  if (__fort_varying_log(mask, size)) {
    res->real = tsource->real;
    res->imag = tsource->imag;
  } else {
    res->real = fsource->real;
    res->imag = fsource->imag;
  }
}

void ENTF90(MERGEDC, mergedc)(dcmplx_t *res, dcmplx_t *tsource,
                              dcmplx_t *fsource, void *mask, __INT_T *size)
{
  if (__fort_varying_log(mask, size)) {
    res->real = tsource->real;
    res->imag = tsource->imag;
  } else {
    res->real = fsource->real;
    res->imag = fsource->imag;
  }
}

