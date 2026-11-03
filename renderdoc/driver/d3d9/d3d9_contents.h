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

#include "d3d9_common.h"

// Resource contents
// -----------------
// Objects with contents are vertex and index buffers, textures (2D, cube and volume) and the
// surfaces that aren't levels of a texture: render targets, depth stencils, offscreen plain
// surfaces and back buffers. Texture levels belong to their texture, which holds the contents.
//
// Captures store the contents of each referenced object as it was when the frame started (the
// initial contents), and the data written by every Lock/Unlock pair during the frame. How the
// initial contents are read depends on where the object lives:
//
// * Buffers, and textures and surfaces in D3DPOOL_MANAGED, D3DPOOL_SYSTEMMEM, D3DPOOL_SCRATCH, or
//   D3DPOOL_DEFAULT with D3DUSAGE_DYNAMIC: locked with D3DLOCK_READONLY.
// * Render targets: copied to system memory with GetRenderTargetData, resolving multisampled
//   surfaces with StretchRect first.
// * Other D3DPOOL_DEFAULT textures can't be locked or copied from. Their contents only change
//   through UpdateTexture and UpdateSurface, so we keep a copy of what's uploaded (the shadow).
// * Depth stencils can only be read if they have a lockable format. Otherwise there are no
//   contents, and the initial contents say why.
//
// Reading contents can be slow, so it's postponed: each object is read the first time the frame
// is about to write to it, when it's released, or at the end of the frame, and only if the frame
// references it.

// how a format's data is laid out: blocks of Width x Height pixels, each Bytes big. Bytes is 0 for
// formats we don't know, whose rows are kept as the driver lays them out.
struct D3D9FormatBlock
{
  uint32_t Width = 1;
  uint32_t Height = 1;
  uint32_t Bytes = 0;
};

D3D9FormatBlock D3D9GetFormatBlock(D3DFORMAT format);

// Part of an object's contents. For textures and surfaces the region is in pixels, for buffers in
// bytes (with Height and Depth 1). Data holds RowCount rows of RowPitch bytes for each slice.
struct D3D9SubresourceData
{
  // the cube face, for cube textures
  uint32_t Face = 0;
  uint32_t Level = 0;
  uint32_t X = 0;
  uint32_t Y = 0;
  uint32_t Z = 0;
  uint32_t Width = 0;
  uint32_t Height = 1;
  uint32_t Depth = 1;
  uint32_t RowPitch = 0;
  uint32_t RowCount = 0;
  bytebuf Data;
};

DECLARE_REFLECTION_STRUCT(D3D9SubresourceData);

// The contents of an object when the frame started.
struct D3D9InitialContentsData
{
  // what each subresource held. Subresources that were never written are left out.
  rdcarray<D3D9SubresourceData> Subresources;
  // why contents are missing, if they are
  rdcstr Note;
};

DECLARE_REFLECTION_STRUCT(D3D9InitialContentsData);

// sets up dst's row layout for its region and format, and copies the region from src. srcRowPitch
// and srcSlicePitch are the source's layout, as returned by a lock.
void D3D9CopyFromLocked(D3D9SubresourceData &dst, D3DFORMAT format, const void *src,
                        uint32_t srcRowPitch, uint32_t srcSlicePitch);

// A level-0 region the runtime considers dirty, for UpdateTexture. Locks on level 0 and
// AddDirtyRect/AddDirtyBox grow it, UpdateTexture clears it. A new texture is entirely dirty.
struct D3D9DirtyRegion
{
  bool Dirty = true;
  // when set, the whole texture is dirty and Box isn't used
  bool Whole = true;
  D3DBOX Box = {};

  void Add(const D3DBOX *box);
  void Clear()
  {
    Dirty = false;
    Whole = false;
  }
};

// An outstanding lock
struct D3D9LockInfo
{
  uint32_t Face = 0;
  uint32_t Level = 0;
  DWORD Flags = 0;
  // the region locked, in pixels or bytes, and where the application writes it
  D3DBOX Region = {};
  byte *Data = NULL;
  uint32_t RowPitch = 0;
  uint32_t SlicePitch = 0;
};

// What we track for an object with contents
struct D3D9ObjectContents
{
  rdcarray<D3D9LockInfo> Locks;

  // one per cube face, only [0] for other textures
  D3D9DirtyRegion Dirty[6];

  // the shadow of a D3DPOOL_DEFAULT texture that can't be read back: each level's data (for each
  // face, indexed by face * levels + level), rows packed. Empty until something is uploaded.
  rdcarray<bytebuf> Shadow;
};
