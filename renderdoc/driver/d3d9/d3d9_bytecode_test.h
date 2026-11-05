/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2026 Baldur Karlsson
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

// Helpers for the D3D9 bytecode unit tests, which assemble shaders by hand since there's no
// compiler for shader model 1-3 on every platform. Only included by the tests.

#include <string.h>
#include "d3d9_bytecode.h"

namespace D3D9BCTest
{
using namespace D3D9BC;

const uint32_t RelBit = 0x00002000;
const uint32_t PredBit = 0x10000000;
const uint32_t CoIssue = 0x40000000;

inline uint32_t VersionToken(bool pixel, uint8_t major, uint8_t minor)
{
  return (pixel ? 0xFFFF0000U : 0xFFFE0000U) | (uint32_t(major) << 8) | minor;
}

inline uint32_t EndTok()
{
  return 0x0000FFFF;
}

inline uint32_t CommentTok(uint32_t length)
{
  return 0xFFFE | (length << 16);
}

// an instruction token. 1_x shaders have a length of 0.
inline uint32_t Inst(Opcode op, uint32_t control, uint32_t length)
{
  return uint32_t(op) | (control << 16) | (length << 24);
}

inline uint32_t Swz(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
  return x | (y << 2) | (z << 4) | (w << 6);
}

inline uint32_t RegBits(RegType type)
{
  uint32_t t = uint32_t(type);
  return ((t & 0x7) << 28) | ((t & 0x18) << 8);
}

inline uint32_t Dst(RegType type, uint32_t index, uint32_t mask = 0xF, uint32_t mods = 0,
                    int shift = 0)
{
  return 0x80000000U | RegBits(type) | (index & 0x7FF) | (mask << 16) | (mods << 20) |
         ((uint32_t(shift) & 0xF) << 24);
}

inline uint32_t Src(RegType type, uint32_t index, uint32_t swizzle = 0xE4, SrcMod mod = SrcMod::None)
{
  return 0x80000000U | RegBits(type) | (index & 0x7FF) | (swizzle << 16) | (uint32_t(mod) << 24);
}

inline uint32_t Dcl(DeclUsage usage, uint32_t index)
{
  return 0x80000000U | uint32_t(usage) | (index << 16);
}

inline uint32_t DclNone()
{
  return 0x80000000U;
}

inline uint32_t DclSampler(uint32_t type)
{
  return 0x80000000U | (type << 27);
}

inline uint32_t FloatBits(float f)
{
  uint32_t ret;
  memcpy(&ret, &f, sizeof(ret));
  return ret;
}

};    // namespace D3D9BCTest
