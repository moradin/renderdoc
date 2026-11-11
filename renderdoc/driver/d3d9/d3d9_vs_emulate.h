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

// Post-transform vertices for the mesh viewer, computed on the CPU (phase 3e). This file has the
// vertex shader interpreter (vs_1_1 to vs_3_0, on the program d3d9_bytecode.h parses), reading
// vertices from the streams with a vertex declaration, and running a whole draw
// (RunPostVS) through either the interpreter or the fixed function emulation in
// d3d9_ffp_vertex.h. Like the bytecode parser it doesn't need any Windows headers, so it's unit
// tested on every platform. The replay side, which fills these structs from the device, is
// d3d9_postvs.cpp.

#include "api/replay/renderdoc_replay.h"
#include "d3d9_bytecode.h"

namespace D3D9Emu
{
struct Vec4
{
  float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  Vec4() = default;
  Vec4(float x, float y, float z, float w)
  {
    v[0] = x;
    v[1] = y;
    v[2] = z;
    v[3] = w;
  }
  float &operator[](size_t i) { return v[i]; }
  float operator[](size_t i) const { return v[i]; }
};

struct IVec4
{
  int32_t v[4] = {0, 0, 0, 0};
};

// the constant registers the shader reads: the device's, before the shader's def/defi/defb
// literals are applied (the emulator applies those itself)
struct VSConstants
{
  rdcarray<Vec4> floats;
  rdcarray<IVec4> ints;
  // 0 or 1
  rdcarray<uint32_t> bools;
};

// D3DTEXTUREADDRESS
enum class TexAddress : uint32_t
{
  Wrap = 1,
  Mirror = 2,
  Clamp = 3,
  Border = 4,
  MirrorOnce = 5,
};

// D3DTEXTUREFILTERTYPE. Sampling treats every filter above Point as Linear: texldl has no
// derivatives for anisotropic filtering, and the GPU filtered those as Linear.
enum class TexFilter : uint32_t
{
  None = 0,
  Point = 1,
  Linear = 2,
  Anisotropic = 3,
};

// A texture a vs_3_0 shader samples with texldl, decoded to the values sampling returns (missing
// channels already filled the way D3D9 fills them, and sRGB already converted when the sampler
// reads it as sRGB). SampleLevel filters as a GPU's hardware vertex processing does, measured
// natively (see STATUS.md, "texldl filtering").
struct VSTexture
{
  D3D9BC::SamplerTextureType type = D3D9BC::SamplerTextureType::Tex2D;

  struct Level
  {
    uint32_t width = 0;
    uint32_t height = 0;
    // the volume's depth, or 6 faces for a cube
    uint32_t depth = 1;
    // x fastest, then y, then the slice or face
    rdcarray<Vec4> texels;
  };
  rdcarray<Level> levels;

  TexAddress address[3] = {TexAddress::Wrap, TexAddress::Wrap, TexAddress::Wrap};
  Vec4 border;
  // D3DSAMP_MAXMIPLEVEL: the most detailed level that can be used
  uint32_t maxMipLevel = 0;
  float mipLODBias = 0.0f;
  TexFilter magFilter = TexFilter::Point;
  TexFilter minFilter = TexFilter::Point;
  TexFilter mipFilter = TexFilter::None;

  bool Bound() const { return !levels.empty(); }
  Vec4 SampleLevel(const Vec4 &coord, float lod) const;

private:
  Vec4 SampleTexels(uint32_t level, const Vec4 &coord, bool linear) const;
};

enum
{
  MaxVSInputs = 16,
  MaxVSTemps = 32,
  NumVertexSamplers = 4,
};

// The vertex shader's outputs. Before vs_3_0 they're the fixed registers oPos, oFog and oPts
// (rast, in that order, the scalars in x), oD0-1 (attr) and oT0-7 (tex). vs_3_0's o0-o11 are tex.
struct VSOutputs
{
  Vec4 rast[3];
  Vec4 attr[2];
  Vec4 tex[12];
};

// the register a signature element (from the shader's reflection, see d3d9_shader_reflect.cpp)
// is stored in, or NULL
const Vec4 *OutputRegister(const VSOutputs &out, const D3D9BC::Version &version,
                           const SigParameter &sig);

class VSEmulator
{
public:
  // checks the program can be run and matches up its flow control. Returns false, with the
  // reason in GetError(), if it can't.
  bool Init(const D3D9BC::Program &prog);
  const rdcstr &GetError() const { return m_Error; }
  const D3D9BC::Version &GetVersion() const { return m_Version; }
  // the program given to Init, which must outlive the emulator
  const D3D9BC::Program &GetProgram() const { return *m_Prog; }

  // the constants with the shader's literals applied, and padded to cover them
  VSConstants ApplyLiterals(const VSConstants &consts) const;

  // runs the shader for one vertex, with constants from ApplyLiterals. textures are the four
  // vertex texture samplers. Returns false with error set if execution failed: outputs then have
  // what was written until then.
  bool Execute(const Vec4 inputs[MaxVSInputs], const VSConstants &consts,
               const VSTexture textures[NumVertexSamplers], VSOutputs &out, rdcstr &error) const;

private:
  struct Literal
  {
    uint32_t reg;
    uint32_t value[4];
  };

  const D3D9BC::Program *m_Prog = NULL;
  D3D9BC::Version m_Version;
  rdcstr m_Error;

  // per instruction: for if/ifc the matching else (or endif), for else the endif, for loop/rep
  // the endloop/endrep and for those the loop, for break/breakc/breakp the end of the loop it
  // breaks out of. ~0U otherwise.
  rdcarray<uint32_t> m_Match;
  // the instruction index of each label, by label number. ~0U where there's none.
  rdcarray<uint32_t> m_Labels;

  rdcarray<Literal> m_FloatLiterals, m_IntLiterals, m_BoolLiterals;
};

//////////////////////////////////////////////////////////////////////////
// reading vertices

// D3DDECLTYPE
enum class DeclType : uint8_t
{
  Float1 = 0,
  Float2,
  Float3,
  Float4,
  D3DColor,
  UByte4,
  Short2,
  Short4,
  UByte4N,
  Short2N,
  Short4N,
  UShort2N,
  UShort4N,
  UDec3,
  Dec3N,
  Float16_2,
  Float16_4,
  Unused,
};

// the size in bytes of a declaration type, 0 for unused or unknown ones
uint32_t DeclTypeSize(DeclType type);
// the number of components a declaration type has
uint32_t DeclTypeComponents(DeclType type);
// decodes one element. Missing components are filled with (0, 0, 0, 1), as the input assembler
// does. data must hold DeclTypeSize(type) bytes.
Vec4 DecodeDeclType(DeclType type, const byte *data);

// a D3DVERTEXELEMENT9, with the usage numbered like D3DDECLUSAGE
struct InputElement
{
  uint32_t stream = 0;
  uint32_t offset = 0;
  DeclType type = DeclType::Float4;
  // D3DDECLMETHOD, only the default (0) reads anything
  uint8_t method = 0;
  D3D9BC::DeclUsage usage = D3D9BC::DeclUsage::Position;
  uint32_t usageIndex = 0;
};

// D3DSTREAMSOURCE_INDEXEDDATA and D3DSTREAMSOURCE_INSTANCEDATA
enum : uint32_t
{
  StreamIndexedData = 1U << 30,
  StreamInstanceData = 2U << 30,
};

struct InputStream
{
  // the buffer's contents from the stream's offset on
  bytebuf data;
  uint32_t stride = 0;
  // the stream source frequency, with its flags
  uint32_t frequency = 1;
};

// the element of a stream a vertex reads: the vertex number for vertex data, instance /
// divider for instance data, and vertex / divider for a plain frequency (the non-indexed
// geometry instancing of software vertex processing)
uint32_t StreamElementIndex(const InputStream &stream, uint32_t vertex, uint32_t instance);

// reads one element for a vertex and instance. Returns false (and (0, 0, 0, 1)) if it's
// outside the stream's data.
bool FetchElement(const InputElement &el, const InputStream *streams, uint32_t numStreams,
                  uint32_t vertex, uint32_t instance, Vec4 &ret);

//////////////////////////////////////////////////////////////////////////
// running a draw

struct FFPState;

struct DrawInputs
{
  rdcarray<InputElement> elements;
  rdcarray<InputStream> streams;

  // the vertices a non-indexed draw reads start at firstVertex. An indexed draw's row i reads
  // vertex indices[i] + baseVertex.
  bool indexed = false;
  rdcarray<uint32_t> indices;
  int32_t baseVertex = 0;
  uint32_t firstVertex = 0;
  // the number of rows (vertices drawn)
  uint32_t numVertices = 0;
};

// what a draw's post-transform vertices are, for one instance
struct PostVSResult
{
  // why there's no data, if there's none
  rdcstr status;

  // the vertices, tightly packed floats: the position element first, then the rest in signature
  // order, each with compCount floats (the layout the mesh viewer gives VS Out columns)
  bytebuf vertexData;
  uint32_t stride = 0;
  uint32_t numUnique = 0;

  // for indexed draws, each row's vertex in vertexData. The unique vertex indices are sorted.
  bool indexed = false;
  rdcarray<uint32_t> indices;
  uint32_t numRows = 0;

  // the position is clip space (oPos, or the fixed function's projection), not pretransformed
  bool clipSpace = false;
  bool hasPosition = false;
  float nearPlane = 0.1f;
  float farPlane = 100.0f;
};

// the order signature elements are stored in: the position first, the others as they are
const rdcarray<size_t> PackOrder(const rdcarray<SigParameter> &sig);

// estimates the near and far planes from clip space positions, as D3D11's post-VS does
void EstimateNearFar(const byte *data, uint32_t stride, uint32_t count, float &nearPlane,
                     float &farPlane);

// runs a draw's vertices for one instance, through vs (with its output signature in
// signature, consts from vs->ApplyLiterals) or, without vs, the fixed function pipeline (with
// FFPSignature's signature)
void RunPostVS(const DrawInputs &draw, uint32_t instance, const VSEmulator *vs,
               const VSConstants &consts, const VSTexture textures[NumVertexSamplers],
               const FFPState *ffp, const rdcarray<SigParameter> &signature, PostVSResult &result);

};    // namespace D3D9Emu
