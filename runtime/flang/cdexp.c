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

#include "mthdecls.h"

#if defined(HOST_WIN) || defined(WINNT) || defined(WIN64) || defined(WIN32) || defined(HOST_MINGW)
static void sincos(float angle, float *psin, float *pcos)
{
    *psin = sin(angle);
    *pcos = cos(angle);
}
#endif

void
__mth_i_cdexp(dcmplx_t *dcmplx, double real, double imag)
{
  double x, y, z;
  x = exp(real);
  sincos(imag, &z, &y);
  y *= x;
  z *= x;
  d_dummy(y, z); /* should leave y & z in appropriate
                  * registers */
}
