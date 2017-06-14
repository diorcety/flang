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

/* must include ent3f.h AFTER io3f.h */
#if defined(HOST_WIN) || defined(WINNT) || defined(WIN64) || defined(WIN32) || defined(HOST_MINGW)
#include <windows.h>
#endif
#include "io3f.h"
#include "ent3f.h"
#include "stdioInterf.h"

extern FILE *__getfile3f();

#if defined(HOST_WIN) || defined(WINNT) || defined(WIN64) || defined(WIN32) || defined(HOST_MINGW)
static int
fsync (int fd)
{
  HANDLE h = (HANDLE) _get_osfhandle (fd);
  DWORD err;

  if (h == INVALID_HANDLE_VALUE)
    {
      errno = EBADF;
      return -1;
    }

  if (!FlushFileBuffers (h))
    {
      /* Translate some Windows errors into rough approximations of Unix
       * errors.  MSDN is useless as usual - in this case it doesn't
       * document the full range of errors.
       */
      err = GetLastError ();
      switch (err)
	{
	  /* eg. Trying to fsync a tty. */
	case ERROR_INVALID_HANDLE:
	  errno = EINVAL;
	  break;

	default:
	  errno = EIO;
	}
      return -1;
    }

  return 0;
}
#endif

void ENT3F(FSYNC, fsync)(lu) int *lu;
{
  FILE *f;

  f = __getfile3f(*lu);
  if (f)
    fsync(__io_getfd(f));
  return;
}
