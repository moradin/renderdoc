/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2025-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

// workaround for bug with latest windows SDK version. wchar.h requires intrinsic that's not
// available on VS2015 _mm_loadu_si64. We also just don't want to pull in build-time dependencies
// on AVX if we can help it

// include this header first (on older SDKs it might not include this but it's not a heavy header
#include <immintrin.h>

// if this isn't defined (by new compiler versions) then define it now. This may break in future if
// this becomes a real function instead of a define...
#ifndef _mm_loadu_si64
#define _mm_loadu_si64(p) _mm_loadl_epi64((__m128i const *)(p))
#endif

// include the real wchar.h. To avoid self-referential includes we assume it sits in a ucrt folder
#include <../ucrt/wchar.h>

// don't call normal functions to avoid warnings about pulling in avx instructions
#define wmemcmp wmemcmp_simple

inline int wmemcmp_simple(const wchar_t *a, const wchar_t *b, size_t n)
{
  size_t i = 0;
  for(; i < n; ++i)
  {
    if(a[i] != b[i])
      return a[i] < b[i] ? -1 : 1;
  }
  return 0;
}

#define wmemchr wmemchr_simple

inline const wchar_t *wmemchr_simple(const wchar_t *s, wchar_t c, size_t n)
{
  size_t i = 0;
  for(; i < n; ++i)
  {
    if(s[i] == c)
      return s + i;
  }
  return NULL;
}
