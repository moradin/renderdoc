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
#include "d3d9_state.h"

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
// * Depth stencils with a lockable format are locked. Others (including INTZ, DF24 and DF16
//   textures) are read with the depth and stencil tests, see d3d9_depth.cpp, which needs shader
//   model 3. D24FS8 and D3DMULTISAMPLE_NONMASKABLE can't be read that way: they have no contents,
//   and the initial contents say why. Multisampled ones store one slice per sample.
// * State blocks: their values can't be read directly, so the state block is applied to the real
//   device, the device state read back, and the device's state set back the way it was. Which
//   states a state block covers comes from its creation: CreateStateBlock's type, or the calls
//   made between BeginStateBlock and EndStateBlock, which are kept with its creation chunk.
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

// The row pitch of memory locked with Pitch, for a level width pixels wide. Drivers report ATI1
// and ATI2's Pitch as one byte per pixel column (16 for 16 pixels), less than a row of their
// blocks, but the GPU reads their blocks tightly packed, one row of blocks after another, and
// apps have to write them that way. So for those this is the size of a row of blocks.
uint32_t D3D9LockedRowPitch(D3DFORMAT format, uint32_t width, uint32_t pitch);

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

// The values of a state block: the device's state after applying it. Only the states the state
// block covers come from it, the rest are whatever the device had when it was read.
struct D3D9StateBlockContents
{
  // indexed by packed slot, like the bindings in BeginCaptureFrame
  rdcarray<ResourceId> Textures;
  rdcarray<ResourceId> StreamSources;
  rdcarray<uint32_t> StreamOffsets;
  rdcarray<uint32_t> StreamStrides;
  ResourceId Indices;
  ResourceId VertexShader;
  ResourceId PixelShader;
  ResourceId VertexDeclaration;
  bool DeclarationFromFVF = false;
  D3D9DeviceState State;
};

DECLARE_REFLECTION_STRUCT(D3D9StateBlockContents);

// The level-0 region the runtime considers dirty, which UpdateTexture copies and then clears.
// Locks on level 0 (without D3DLOCK_NO_DIRTY_UPDATE) and AddDirtyRect/AddDirtyBox add to it, and a
// new texture is entirely dirty. How the runtime keeps it was measured on Windows 11 (see
// d3d9_step8_dirty_regions.md):
// * 2D and volume textures keep up to 6 boxes, as they were added: overlapping, nested or the same
//   box again, each counts. The 7th makes the whole texture dirty.
// * Each cube face keeps one box, which grows to cover every box added to the face.
struct D3D9DirtyRegion
{
  // when set, the whole texture is dirty and Boxes isn't used
  bool Whole = true;
  // otherwise what's dirty. Empty when nothing is.
  rdcarray<D3DBOX> Boxes;

  // box is NULL for the whole texture
  void Add(const D3DBOX *box, bool cubeFace);
  void Clear()
  {
    Whole = false;
    Boxes.clear();
  }
  bool IsDirty() const { return Whole || !Boxes.empty(); }
};

DECLARE_REFLECTION_STRUCT(D3D9DirtyRegion);

// The contents of an object when the frame started.
struct D3D9InitialContentsData
{
  // what each subresource held. Subresources that were never written are left out.
  rdcarray<D3D9SubresourceData> Subresources;
  // for state blocks, one element with their values
  rdcarray<D3D9StateBlockContents> StateBlock;
  // why contents are missing, if they are
  rdcstr Note;
  // for system memory textures, the dirty region of each face (one for 2D and volume textures),
  // which is what an UpdateTexture from it copies. Replay sets it after writing the contents.
  // Empty for other objects, and in captures before version 0x2.
  rdcarray<D3D9DirtyRegion> Dirty;
  // for textures, the most detailed level set with SetLOD, which only D3DPOOL_MANAGED textures
  // use. Replay sets it after writing the contents. 0 in captures before version 0x4.
  uint32_t LOD = 0;
};

DECLARE_REFLECTION_STRUCT(D3D9InitialContentsData);

// sets up dst's row layout for its region and format, and copies the region from src. srcRowPitch
// and srcSlicePitch are the source's layout, as returned by a lock.
void D3D9CopyFromLocked(D3D9SubresourceData &dst, D3DFORMAT format, const void *src,
                        uint32_t srcRowPitch, uint32_t srcSlicePitch);

// reads a whole surface that isn't a depth stencil (or is a lockable one): locked if it can be,
// copied to system memory if it's a render target (resolving it if it's multisampled)
rdcstr D3D9ReadSurfaceContents(IDirect3DDevice9 *dev, IDirect3DSurface9 *surf,
                               D3D9SubresourceData &sub);

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
