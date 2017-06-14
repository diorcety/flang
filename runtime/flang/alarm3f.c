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

/*	alarm3f.c - Implements LIB3F alarm subprogram.  */

#if !defined(HOST_WIN) && !defined(WINNT) && !defined(WIN64) && !defined(WIN32) && !defined(HOST_MINGW)
#include <signal.h>
#include "ent3f.h"

/*
extern void (*signal(int, void (*)(int)))(int);
*/
extern int alarm();

int ENT3F(ALARM, alarm)(int *time, void (*proc)())
{
  if (*time)
    signal(SIGALRM, proc);
  return alarm(*time);
}
#endif
