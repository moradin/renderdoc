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

#include <math.h>
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

// a running loop or rep
struct VSLoopFrame
{
  // the loop or rep instruction
  uint32_t start = 0;
  int32_t remaining = 0;
  int32_t aL = 0;
  int32_t step = 0;
  // rep doesn't have an aL
  bool isLoop = false;
};

// a running subroutine
struct VSCallFrame
{
  // the call or callnz instruction, and the label it called
  uint32_t call = 0;
  uint32_t label = 0;
  // the instruction to return to
  uint32_t ret = 0;
  // the loops running when it was called
  size_t loopDepth = 0;
};

// the predicate and the flow control state of a run, the same in vertex and pixel shaders
struct FlowRegisters
{
  bool p[4] = {false, false, false, false};
  rdcarray<VSLoopFrame> loops;
  rdcarray<VSCallFrame> calls;

  // the index in loops of the innermost loop (rep has no aL), or -1 outside any
  int32_t InnermostLoop() const
  {
    for(size_t i = loops.size(); i > 0; i--)
      if(loops[i - 1].isLoop)
        return int32_t(i - 1);
    return -1;
  }
  // the loop counter register: the innermost loop's, 0 outside any
  int32_t aL() const
  {
    int32_t l = InnermostLoop();
    return l >= 0 ? loops[l].aL : 0;
  }
};

// the registers of one vertex's run, as they are between two instructions
struct VSRegisters : FlowRegisters
{
  Vec4 r[MaxVSTemps];
  IVec4 a0;
  VSOutputs out;
};

// a register an instruction changed, and what it held before
struct VSRegisterChange
{
  // Temp, AddrOrTexture (a0), Predicate (p0), Loop (aL), or the outputs RastOut, AttrOut and
  // TexCrdOutOrOutput
  D3D9BC::RegType type = D3D9BC::RegType::Temp;
  uint32_t index = 0;
  // the value before: in f for temps and outputs, in i for a0, aL (in x, 0 outside a loop) and
  // p0 (each 0 or 1)
  Vec4 f;
  IVec4 i;
};

// For small helpers called from the interpreters' inner loops. With plain inline, once a helper
// was called from more than one file the whole program optimisation stopped inlining it into the
// vertex interpreter's loop, and VS Out was a quarter slower in Release.
#if defined(_MSC_VER)
#define D3D9_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define D3D9_FORCE_INLINE inline __attribute__((always_inline))
#else
#define D3D9_FORCE_INLINE inline
#endif

// helpers both interpreters use: _sat (NaN gives 0), the comparisons of ifc, breakc and setp, a
// source modifier, and where c2048 and up are for the types above Const.
D3D9_FORCE_INLINE float Saturate(float f)
{
  // NaN saturates to 0
  if(!(f > 0.0f))
    return 0.0f;
  return f > 1.0f ? 1.0f : f;
}

D3D9_FORCE_INLINE bool Compare(D3D9BC::Comparison c, float a, float b)
{
  switch(c)
  {
    case D3D9BC::Comparison::GT: return a > b;
    case D3D9BC::Comparison::EQ: return a == b;
    case D3D9BC::Comparison::GE: return a >= b;
    case D3D9BC::Comparison::LT: return a < b;
    case D3D9BC::Comparison::NE: return a != b;
    case D3D9BC::Comparison::LE: return a <= b;
    default: return false;
  }
}

D3D9_FORCE_INLINE float ApplyMod(D3D9BC::SrcMod mod, float f)
{
  switch(mod)
  {
    case D3D9BC::SrcMod::None: return f;
    case D3D9BC::SrcMod::Neg: return -f;
    case D3D9BC::SrcMod::Bias: return f - 0.5f;
    case D3D9BC::SrcMod::BiasNeg: return -(f - 0.5f);
    case D3D9BC::SrcMod::Sign: return 2.0f * f - 1.0f;
    case D3D9BC::SrcMod::SignNeg: return -(2.0f * f - 1.0f);
    case D3D9BC::SrcMod::Comp: return 1.0f - f;
    case D3D9BC::SrcMod::X2: return 2.0f * f;
    case D3D9BC::SrcMod::X2Neg: return -2.0f * f;
    case D3D9BC::SrcMod::Abs: return fabsf(f);
    case D3D9BC::SrcMod::AbsNeg: return -fabsf(f);
    case D3D9BC::SrcMod::Not: return f != 0.0f ? 0.0f : 1.0f;
    // _dz and _dw only apply to ps_1_4's texld and texcrd
    default: return f;
  }
}

D3D9_FORCE_INLINE uint32_t ConstantRegisterOffset(D3D9BC::RegType type)
{
  switch(type)
  {
    case D3D9BC::RegType::Const2: return 2048;
    case D3D9BC::RegType::Const3: return 4096;
    case D3D9BC::RegType::Const4: return 6144;
    default: return 0;
  }
}

// What the interpreters (this one and the pixel shader one in d3d9_ps_emulate.h) work out about a
// program before running it: its flow control matched up, its labels and its literals.
struct ProgramLayout
{
  struct Literal
  {
    uint32_t reg;
    uint32_t value[4];
  };

  // per instruction: for if/ifc the matching else (or endif), for else the endif, for loop/rep
  // the endloop/endrep and for those the loop, for break/breakc/breakp the end of the loop it
  // breaks out of. ~0U otherwise.
  rdcarray<uint32_t> match;
  // the instruction index of each label, by label number. ~0U where there's none.
  rdcarray<uint32_t> labels;

  rdcarray<Literal> floatLiterals, intLiterals, boolLiterals;

  // fills the above from prog. Returns false, with the reason in error, if its flow control doesn't
  // match up.
  bool Build(const D3D9BC::Program &prog, rdcstr &error);
  // the constants with the literals applied, and padded to cover them
  VSConstants ApplyLiterals(const VSConstants &consts) const;
};

class VSEmulator;

// One vertex's run of a shader, which can go one instruction at a time. VSEmulator::Execute runs
// it to the end; a debugger steps it.
class VSThread : private VSRegisters
{
public:
  // starts a run at the first instruction. emu (initialised), inputs, consts (from
  // emu.ApplyLiterals) and textures (the four vertex samplers, or NULL) must outlive the thread.
  void Start(const VSEmulator &emu, const Vec4 inputs[MaxVSInputs], const VSConstants &consts,
             const VSTexture textures[NumVertexSamplers]);

  // the shader ended, or failed
  bool Finished() const { return m_Finished; }
  bool Failed() const { return !m_Error.empty(); }
  const rdcstr &GetError() const { return m_Error; }
  // the instruction Step runs next
  uint32_t NextInstruction() const { return m_IP; }
  uint32_t InstructionsExecuted() const { return m_Executed; }
  const VSRegisters &Registers() const { return *this; }

  // runs the next instruction, appending the registers it changed to changes if that isn't NULL.
  // Returns false if there's nothing to run, or if the instruction failed: the thread is then
  // finished, with GetError set and the registers as they were before it.
  bool Step(rdcarray<VSRegisterChange> *changes = NULL);
  // runs to the end. Returns false if an instruction failed.
  bool Run();

private:
  int32_t RelValue(const D3D9BC::RelativeAddress &rel) const;
  Vec4 ReadRaw(const D3D9BC::SrcParam &src, int32_t regOffset) const;
  Vec4 Read(const D3D9BC::SrcParam &src, int32_t regOffset = 0) const;
  bool ReadBool(const D3D9BC::SrcParam &src) const;
  IVec4 ReadInt(const D3D9BC::SrcParam &src) const;
  uint8_t WriteMask(const D3D9BC::Instruction &inst) const;
  template <bool Record>
  void Write(const D3D9BC::Instruction &inst, Vec4 val);
  bool Fail(uint32_t ip, const rdcstr &msg);
  template <bool Record, bool Single>
  bool Exec(rdcarray<VSRegisterChange> *changes);

  const VSEmulator *m_Emu = NULL;
  const Vec4 *m_Inputs = NULL;
  const VSConstants *m_Consts = NULL;
  const VSTexture *m_Textures = NULL;

  uint32_t m_IP = 0;
  uint32_t m_Executed = 0;
  bool m_Finished = true;
  rdcstr m_Error;

  // where Write records changes, during Step
  rdcarray<VSRegisterChange> *m_Changes = NULL;
};

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
  friend class VSThread;

  const D3D9BC::Program *m_Prog = NULL;
  D3D9BC::Version m_Version;
  rdcstr m_Error;
  ProgramLayout m_Layout;
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

// an input register a vertex shader declares (dcl_usage v#), and the declaration element with the
// same usage and usage index that fills it
struct VSInputBinding
{
  uint32_t reg = 0;
  const InputElement *element = NULL;
};

// the shader's input registers with an element to fill them. The elements must outlive the result.
rdcarray<VSInputBinding> BindVSInputs(const D3D9BC::Program &prog,
                                      const rdcarray<InputElement> &elements);

// a vertex's input registers: each bound one read from the draw's streams, the rest (0, 0, 0, 1)
void FetchVSInputs(const DrawInputs &draw, const rdcarray<VSInputBinding> &bindings,
                   uint32_t vertex, uint32_t instance, Vec4 inputs[MaxVSInputs]);

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
