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

#include "maths/formatpacking.h"
#include "d3d9_device.h"
#include "d3d9_ffp_vertex.h"
#include "d3d9_replay.h"
#include "d3d9_resources.h"
#include "d3d9_state.h"
#include "d3d9_vs_emulate.h"

// Post-transform vertices (phase 3e)
// ----------------------------------
// D3D9 has no stream out, so the mesh viewer's VS Out data is computed on the CPU: vertex
// shaders run in an interpreter (d3d9_vs_emulate.cpp) and the fixed function pipeline is
// emulated (d3d9_ffp_vertex.cpp).
//
// InitPostVSBuffers reads everything a draw needs from the replay device while it's at the
// event: the vertex declaration, the streams' buffer contents and frequencies, the indices, the
// shader and its constants, the vertex textures (decoded, for texldl), or the fixed function
// state. That snapshot is kept per event, so the vertices can be computed later without the
// device: GetPostVSBuffers computes an instance's vertices the first time it's asked for them.
// The results are presented as pseudo buffers (IDs with no object behind them, like the shader
// constants' in d3d9_pipestate.cpp) that GetBufferData reads: a vertex buffer per instance, laid
// out as the mesh viewer expects its VS Out columns (the position first, then the reflection's
// output signature in order, each element compCount floats), and for indexed draws an index
// buffer of 32-bit indices into it. The unique vertices of an indexed draw are computed once each.
// Nothing is cleared when the replay moves, since the data at an event never changes; only
// ClearReplayCache drops it.
//
// The fixed function pipeline has no reflection, so the pipeline state gets one made up for the
// draw (GetFFPReflection) whose output signature is what the emulation outputs. Pretransformed
// (XYZRHW) vertices are output as they were given, and the MeshFormat doesn't ask to unproject
// them.
//
// DrawPrimitiveUP and DrawIndexedPrimitiveUP data is only in their chunks, and the runtime
// unbinds stream 0 (and the indices) after them. The device keeps a copy at load
// (WrappedD3DDevice9::GetUserPrimitives), which the post-transform data reads, and the pipeline
// state presents it as two more pseudo buffers for the event it's at, so the mesh viewer's VS In
// works for them too.

namespace
{
ResourceId WrappedIDOf(IUnknown *obj)
{
  WrappedD3D9Object *wrapped = obj ? GetWrappedObject(obj) : NULL;
  return wrapped ? wrapped->GetResourceID() : ResourceId();
}

float AsFloat(DWORD d)
{
  float f;
  memcpy(&f, &d, sizeof(f));
  return f;
}

D3D9Emu::Vec4 ColorValue(const D3DCOLORVALUE &c)
{
  return D3D9Emu::Vec4(c.r, c.g, c.b, c.a);
}

D3D9Emu::Vec4 ColorDWORD(DWORD c)
{
  return D3D9Emu::Vec4(float((c >> 16) & 0xFF) / 255.0f, float((c >> 8) & 0xFF) / 255.0f,
                       float(c & 0xFF) / 255.0f, float(c >> 24) / 255.0f);
}

D3D9Emu::Matrix MatrixOf(const D3DMATRIX &m)
{
  D3D9Emu::Matrix ret;
  memcpy(ret.m, &m, sizeof(ret.m));
  return ret;
}

// the render states, indexed by D3DRENDERSTATETYPE
struct RenderStates
{
  DWORD rs[D3DRS_BLENDOPALPHA + 1] = {};

  RenderStates(const D3D9DeviceState &state)
  {
    for(const D3D9RenderStateValue &v : state.RenderStates)
      if((uint32_t)v.State < ARRAY_COUNT(rs))
        rs[v.State] = v.Value;
  }

  DWORD operator[](D3DRENDERSTATETYPE s) const { return rs[s]; }
  float F(D3DRENDERSTATETYPE s) const { return AsFloat(rs[s]); }
};

// what sampling returns for a decoded texel, which has the channels the format is described with
// (see NativeSwizzle in d3d9_rendertexture.cpp): D3D9 fills missing channels with 1, and
// replicates luminance
D3D9Emu::Vec4 SampledValue(D3DFORMAT format, const FloatVector &t)
{
  switch((DWORD)format)
  {
    case D3DFMT_L8:
    case D3DFMT_L16: return D3D9Emu::Vec4(t.x, t.x, t.x, 1.0f);
    case D3DFMT_A8L8:
    case D3DFMT_A4L4: return D3D9Emu::Vec4(t.x, t.x, t.x, t.y);
    case D3DFMT_R16F:
    case D3DFMT_R32F: return D3D9Emu::Vec4(t.x, 1.0f, 1.0f, 1.0f);
    case D3DFMT_G16R16:
    case D3DFMT_G16R16F:
    case D3DFMT_G32R32F:
    case D3DFMT_V8U8:
    case D3DFMT_V16U16: return D3D9Emu::Vec4(t.x, t.y, 1.0f, 1.0f);
    default: return D3D9Emu::Vec4(t.x, t.y, t.z, t.w);
  }
}

// formats with up to 8 bits per colour channel, which D3DSAMP_SRGBTEXTURE converts
bool SRGBReadable(D3DFORMAT format)
{
  switch((DWORD)format)
  {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_R8G8B8:
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_L8:
    case D3DFMT_A8L8:
    case D3DFMT_A4L4:
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5: return true;
    default: return false;
  }
}

// D3DDECLUSAGE as HLSL names it
const char *UsageName(D3D9BC::DeclUsage usage)
{
  static const char *names[] = {
      "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
      "BINORMAL", "TESSFACTOR",  "POSITIONT",    "COLOR",  "FOG",   "DEPTH",    "SAMPLE",
  };
  return uint32_t(usage) < ARRAY_COUNT(names) ? names[uint32_t(usage)] : "UNKNOWN";
}

// a fixed function output layout as a string, to share reflections between draws
rdcstr LayoutKey(const D3D9Emu::FFPOutputLayout &l)
{
  rdcstr ret = StringFormat::Fmt("%d%d%d", l.pretransformed ? 1 : 0, l.fog ? 1 : 0, l.psize ? 1 : 0);
  for(uint32_t i = 0; i < D3D9Emu::MaxTextureStages; i++)
    ret += StringFormat::Fmt(",%u", l.texcoordDims[i]);
  return ret;
}
};    // namespace

struct D3D9Replay::PostVSEvent
{
  // why there's no data, when there can't be any
  rdcstr status;

  D3D9Emu::DrawInputs draw;
  Topology topology = Topology::Unknown;
  uint32_t numInstances = 1;

  bool hasVS = false;
  D3D9Emu::VSEmulator vs;
  D3D9Emu::VSConstants consts;
  D3D9Emu::VSTexture textures[D3D9Emu::NumVertexSamplers];

  D3D9Emu::FFPState ffp;

  rdcarray<SigParameter> signature;

  // computed when they're first asked for, by instance
  std::map<uint32_t, D3D9Emu::PostVSResult> results;
  std::map<uint32_t, ResourceId> vertexBuffers;
  ResourceId indexBuffer;
};

void D3D9Replay::CreatePostVSResources()
{
  m_UPVertexBuffer = ResourceIDGen::GetNewUniqueID();
  ResourceDescription &vb = GetResourceDesc(m_UPVertexBuffer);
  vb.name = "DrawPrimitiveUP vertices";
  vb.autogeneratedName = false;
  vb.type = ResourceType::Buffer;

  m_UPIndexBuffer = ResourceIDGen::GetNewUniqueID();
  ResourceDescription &ib = GetResourceDesc(m_UPIndexBuffer);
  ib.name = "DrawIndexedPrimitiveUP indices";
  ib.autogeneratedName = false;
  ib.type = ResourceType::Buffer;
}

bool D3D9Replay::ReadFFPState(const D3D9DeviceState &state, Topology topology, D3D9Emu::FFPState &ffp)
{
  RenderStates rs(state);

  ffp = D3D9Emu::FFPState();

  for(const D3D9TransformValue &t : state.Transforms)
  {
    const uint32_t s = (uint32_t)t.State;
    if(s == D3DTS_VIEW)
      ffp.view = MatrixOf(t.Matrix);
    else if(s == D3DTS_PROJECTION)
      ffp.projection = MatrixOf(t.Matrix);
    else if(s >= D3DTS_TEXTURE0 && s <= D3DTS_TEXTURE7)
      ffp.texture[s - D3DTS_TEXTURE0] = MatrixOf(t.Matrix);
    else if(s >= 256 && s < 256 + D3D9Emu::NumWorldMatrices)
      ffp.world[s - 256] = MatrixOf(t.Matrix);
  }

  ffp.lighting = rs[D3DRS_LIGHTING] != 0;
  ffp.normalizeNormals = rs[D3DRS_NORMALIZENORMALS] != 0;
  ffp.localViewer = rs[D3DRS_LOCALVIEWER] != 0;
  ffp.colorVertex = rs[D3DRS_COLORVERTEX] != 0;
  ffp.specularEnable = rs[D3DRS_SPECULARENABLE] != 0;
  ffp.diffuseSource = D3D9Emu::ColorSource(rs[D3DRS_DIFFUSEMATERIALSOURCE]);
  ffp.specularSource = D3D9Emu::ColorSource(rs[D3DRS_SPECULARMATERIALSOURCE]);
  ffp.ambientSource = D3D9Emu::ColorSource(rs[D3DRS_AMBIENTMATERIALSOURCE]);
  ffp.emissiveSource = D3D9Emu::ColorSource(rs[D3DRS_EMISSIVEMATERIALSOURCE]);
  ffp.ambient = ColorDWORD(rs[D3DRS_AMBIENT]);

  ffp.material.diffuse = ColorValue(state.Material.Diffuse);
  ffp.material.ambient = ColorValue(state.Material.Ambient);
  ffp.material.specular = ColorValue(state.Material.Specular);
  ffp.material.emissive = ColorValue(state.Material.Emissive);
  ffp.material.power = state.Material.Power;

  for(const D3D9LightState &l : state.Lights)
  {
    if(!l.Enabled || ffp.lights.size() >= D3D9Emu::MaxFFPLights)
      continue;
    D3D9Emu::FFPLight light;
    light.type = D3D9Emu::LightType(l.Light.Type);
    light.diffuse = ColorValue(l.Light.Diffuse);
    light.specular = ColorValue(l.Light.Specular);
    light.ambient = ColorValue(l.Light.Ambient);
    light.position[0] = l.Light.Position.x;
    light.position[1] = l.Light.Position.y;
    light.position[2] = l.Light.Position.z;
    light.direction[0] = l.Light.Direction.x;
    light.direction[1] = l.Light.Direction.y;
    light.direction[2] = l.Light.Direction.z;
    light.range = l.Light.Range;
    light.falloff = l.Light.Falloff;
    light.attenuation[0] = l.Light.Attenuation0;
    light.attenuation[1] = l.Light.Attenuation1;
    light.attenuation[2] = l.Light.Attenuation2;
    light.theta = l.Light.Theta;
    light.phi = l.Light.Phi;
    ffp.lights.push_back(light);
  }

  ffp.vertexBlend = rs[D3DRS_VERTEXBLEND];
  ffp.indexedVertexBlend = rs[D3DRS_INDEXEDVERTEXBLENDENABLE] != 0;
  ffp.tweenFactor = rs.F(D3DRS_TWEENFACTOR);

  ffp.fogEnable = rs[D3DRS_FOGENABLE] != 0;
  ffp.fogVertexMode = D3D9Emu::FogMode(rs[D3DRS_FOGVERTEXMODE]);
  ffp.fogTableMode = D3D9Emu::FogMode(rs[D3DRS_FOGTABLEMODE]);
  ffp.rangeFog = rs[D3DRS_RANGEFOGENABLE] != 0;
  ffp.fogStart = rs.F(D3DRS_FOGSTART);
  ffp.fogEnd = rs.F(D3DRS_FOGEND);
  ffp.fogDensity = rs.F(D3DRS_FOGDENSITY);

  ffp.pointSize = rs.F(D3DRS_POINTSIZE);
  ffp.pointSizeMin = rs.F(D3DRS_POINTSIZE_MIN);
  ffp.pointSizeMax = rs.F(D3DRS_POINTSIZE_MAX);
  ffp.pointScaleEnable = rs[D3DRS_POINTSCALEENABLE] != 0;
  ffp.pointScale[0] = rs.F(D3DRS_POINTSCALE_A);
  ffp.pointScale[1] = rs.F(D3DRS_POINTSCALE_B);
  ffp.pointScale[2] = rs.F(D3DRS_POINTSCALE_C);
  ffp.viewportHeight = float(state.Viewport.Height);
  ffp.points = topology == Topology::PointList;

  // the defaults: stage 0 modulates, the rest are disabled
  DWORD colorOp[D3D9Emu::MaxTextureStages] = {D3DTOP_MODULATE};
  for(uint32_t i = 1; i < D3D9Emu::MaxTextureStages; i++)
    colorOp[i] = D3DTOP_DISABLE;

  for(const D3D9TextureStageStates &stage : state.TextureStageStates)
  {
    if(stage.Stage >= D3D9Emu::MaxTextureStages)
      continue;
    for(const D3D9TextureStageStateValue &v : stage.States)
    {
      if(v.State == D3DTSS_TEXCOORDINDEX)
        ffp.texCoordIndex[stage.Stage] = v.Value;
      else if(v.State == D3DTSS_TEXTURETRANSFORMFLAGS)
        ffp.textureTransformFlags[stage.Stage] = v.Value;
      else if(v.State == D3DTSS_COLOROP)
        colorOp[stage.Stage] = v.Value;
    }
  }

  // with no pixel shader, the stages from the first disabled one on have no texture coordinate.
  // A pixel shader can read any of them.
  if(m_pDevice->GetBindings().ps == NULL)
  {
    ffp.outputStages = 0;
    while(ffp.outputStages < D3D9Emu::MaxTextureStages && colorOp[ffp.outputStages] != D3DTOP_DISABLE)
      ffp.outputStages++;
  }

  return true;
}

void D3D9Replay::ReadVertexElements(rdcarray<D3D9Emu::InputElement> &elements)
{
  elements.clear();

  const D3D9ObjectBindings &bind = m_pDevice->GetBindings();
  IDirect3DVertexDeclaration9 *decl = bind.decl;
  bool release = false;
  if(decl == NULL && bind.declFromFVF)
  {
    // the runtime owns declarations made from FVFs
    if(SUCCEEDED(m_pDevice->GetReal()->GetVertexDeclaration(&decl)) && decl)
      release = true;
    else
      decl = NULL;
  }

  if(decl == NULL)
    return;

  UINT num = 0;
  decl->GetDeclaration(NULL, &num);
  rdcarray<D3DVERTEXELEMENT9> elems;
  elems.resize(num);
  if(num > 0 && SUCCEEDED(decl->GetDeclaration(elems.data(), &num)))
  {
    for(const D3DVERTEXELEMENT9 &e : elems)
    {
      if(e.Stream == 0xFF)
        break;
      D3D9Emu::InputElement el;
      el.stream = e.Stream;
      el.offset = e.Offset;
      el.type = D3D9Emu::DeclType(e.Type);
      el.method = e.Method;
      el.usage = D3D9BC::DeclUsage(e.Usage);
      el.usageIndex = e.UsageIndex;
      elements.push_back(el);
    }
  }

  if(release)
    decl->Release();
}

const ShaderReflection *D3D9Replay::GetFFPReflection(uint32_t eventId, const D3D9DeviceState &state)
{
  const ActionDescription *action = m_pDevice->GetAction(eventId);
  if(action == NULL || !(action->flags & ActionFlags::Drawcall))
    return NULL;

  D3D9Emu::FFPState ffp;
  ReadFFPState(state, GetEventTopology(eventId), ffp);

  rdcarray<D3D9Emu::InputElement> elements;
  ReadVertexElements(elements);
  D3D9Emu::FFPInputs inputs;
  inputs.FromElements(elements);

  const D3D9Emu::FFPOutputLayout layout = D3D9Emu::FFPLayout(ffp, inputs);
  const rdcstr key = LayoutKey(layout);

  std::shared_ptr<ShaderReflection> &refl = m_FFPReflections[key];
  if(!refl)
  {
    refl.reset(new ShaderReflection);
    refl->stage = ShaderStage::Vertex;
    refl->entryPoint = "main";
    refl->outputSignature = D3D9Emu::FFPSignature(layout);
    for(const D3D9Emu::InputElement &el : elements)
    {
      SigParameter p;
      p.semanticName = UsageName(el.usage);
      p.semanticIndex = uint16_t(el.usageIndex);
      p.regIndex = (uint32_t)refl->inputSignature.size();
      p.varType = VarType::Float;
      p.compCount = uint8_t(D3D9Emu::DeclTypeComponents(el.type));
      p.regChannelMask = p.channelUsedMask = uint8_t((1 << p.compCount) - 1);
      refl->inputSignature.push_back(p);
    }
  }

  return refl.get();
}

void D3D9Replay::InitPostVSBuffers(uint32_t eventId)
{
  if(m_PostVS.find(eventId) != m_PostVS.end())
    return;

  const ActionDescription *action = m_pDevice->GetAction(eventId);
  if(action == NULL || !(action->flags & ActionFlags::Drawcall))
    return;

  std::shared_ptr<PostVSEvent> evPtr(new PostVSEvent);
  m_PostVS[eventId] = evPtr;
  PostVSEvent &ev = *evPtr;

  const D3D9ObjectBindings &bind = m_pDevice->GetBindings();

  D3D9DeviceState state;
  m_pDevice->FetchDeviceState(state);

  ev.topology = GetEventTopology(eventId);

  const bool indexed = bool(action->flags & ActionFlags::Indexed);
  // hardware instancing only applies to indexed draws
  ev.numInstances = indexed ? RDCMAX(1U, action->numInstances) : 1U;

  //////////////////////////////////////////////////////////////////////////
  // inputs

  D3D9Emu::DrawInputs &draw = ev.draw;
  ReadVertexElements(draw.elements);

  const D3D9UserPrimitiveData *up = m_pDevice->GetUserPrimitives(eventId);

  draw.streams.resize(D3D9_NUM_STREAMS);
  for(uint32_t i = 0; i < D3D9_NUM_STREAMS; i++)
  {
    bool used = false;
    for(const D3D9Emu::InputElement &el : draw.elements)
      used |= el.stream == i;
    if(!used)
      continue;

    D3D9Emu::InputStream &s = draw.streams[i];
    if(up && i == 0)
    {
      s.data = up->vertices;
      s.stride = up->stride;
      continue;
    }

    ResourceId id = WrappedIDOf(bind.streams[i].buffer);
    if(id != ResourceId())
      GetBufferData(id, bind.streams[i].offset, 0, s.data);
    s.stride = bind.streams[i].stride;
    if(indexed && i < state.StreamFrequencies.size())
      s.frequency = state.StreamFrequencies[i];
  }

  draw.numVertices = action->numIndices;
  if(indexed)
  {
    draw.indexed = true;

    bytebuf idx;
    bool idx32 = false;
    if(up)
    {
      idx = up->indices;
      idx32 = up->index32;
    }
    else if(bind.indices)
    {
      D3DINDEXBUFFER_DESC desc = {};
      bind.indices->GetDesc(&desc);
      idx32 = desc.Format == D3DFMT_INDEX32;
      const uint32_t size = idx32 ? 4 : 2;
      GetBufferData(WrappedIDOf(bind.indices), uint64_t(action->indexOffset) * size,
                    uint64_t(action->numIndices) * size, idx);
      draw.baseVertex = action->baseVertex;
    }
    else
    {
      ev.status = "No index buffer is bound";
      return;
    }

    const size_t count = RDCMIN(idx.size() / (idx32 ? 4 : 2), (size_t)action->numIndices);
    draw.indices.resize(count);
    for(size_t i = 0; i < count; i++)
    {
      if(idx32)
        memcpy(&draw.indices[i], idx.data() + i * 4, 4);
      else
        draw.indices[i] = uint32_t(idx[i * 2]) | (uint32_t(idx[i * 2 + 1]) << 8);
    }
  }
  else
  {
    draw.firstVertex = action->vertexOffset;
  }

  //////////////////////////////////////////////////////////////////////////
  // vertex processing

  ResourceId vsId = WrappedIDOf(bind.vs);
  if(vsId != ResourceId())
  {
    auto it = m_Shaders.find(vsId);
    if(it == m_Shaders.end())
    {
      ev.status = "The vertex shader's bytecode wasn't kept";
      return;
    }

    if(!ev.vs.Init(it->second.GetProgram()))
    {
      ev.status = "The vertex shader can't be emulated: " + ev.vs.GetError();
      return;
    }
    ev.hasVS = true;
    ev.signature = it->second.GetReflection().outputSignature;

    const D3D9ShaderConstants &c = state.VertexShaderConstants;
    D3D9Emu::VSConstants consts;
    consts.floats.resize(c.Float.size());
    for(size_t i = 0; i < c.Float.size(); i++)
      consts.floats[i] = D3D9Emu::Vec4(c.Float[i].x, c.Float[i].y, c.Float[i].z, c.Float[i].w);
    consts.ints.resize(c.Int.size());
    for(size_t i = 0; i < c.Int.size(); i++)
    {
      consts.ints[i].v[0] = c.Int[i].x;
      consts.ints[i].v[1] = c.Int[i].y;
      consts.ints[i].v[2] = c.Int[i].z;
      consts.ints[i].v[3] = c.Int[i].w;
    }
    consts.bools.resize(c.Bool.size());
    for(size_t i = 0; i < c.Bool.size(); i++)
      consts.bools[i] = c.Bool[i] ? 1 : 0;
    ev.consts = ev.vs.ApplyLiterals(consts);

    // vertex textures, for texldl
    bool texldl = false;
    for(const D3D9BC::Instruction &inst : it->second.GetProgram().instructions)
      texldl |= inst.op == D3D9BC::Opcode::TexLdl;

    for(uint32_t s = 0; texldl && s < D3D9Emu::NumVertexSamplers; s++)
      ReadVertexTexture(s, state, ev.textures[s]);
  }
  else
  {
    ReadFFPState(state, ev.topology, ev.ffp);

    D3D9Emu::FFPInputs inputs;
    inputs.FromElements(draw.elements);
    ev.signature = D3D9Emu::FFPSignature(D3D9Emu::FFPLayout(ev.ffp, inputs));
  }
}

void D3D9Replay::ReadVertexTexture(uint32_t sampler, const D3D9DeviceState &state,
                                   D3D9Emu::VSTexture &tex)
{
  tex = D3D9Emu::VSTexture();

  const DWORD apiSampler = D3DVERTEXTEXTURESAMPLER0 + sampler;
  IDirect3DBaseTexture9 *base = m_pDevice->GetBindings().textures[D3D9SamplerToSlot(apiSampler)];
  ResourceId id = WrappedIDOf(base);
  if(base == NULL || id == ResourceId())
    return;

  D3DFORMAT format = D3DFMT_UNKNOWN;
  uint32_t slices = 1;
  switch(base->GetType())
  {
    case D3DRTYPE_TEXTURE:
    {
      D3DSURFACE_DESC desc = {};
      ((IDirect3DTexture9 *)base)->GetLevelDesc(0, &desc);
      format = desc.Format;
      tex.type = D3D9BC::SamplerTextureType::Tex2D;
      break;
    }
    case D3DRTYPE_CUBETEXTURE:
    {
      D3DSURFACE_DESC desc = {};
      ((IDirect3DCubeTexture9 *)base)->GetLevelDesc(0, &desc);
      format = desc.Format;
      tex.type = D3D9BC::SamplerTextureType::Cube;
      slices = 6;
      break;
    }
    case D3DRTYPE_VOLUMETEXTURE:
    {
      D3DVOLUME_DESC desc = {};
      ((IDirect3DVolumeTexture9 *)base)->GetLevelDesc(0, &desc);
      format = desc.Format;
      tex.type = D3D9BC::SamplerTextureType::Volume;
      break;
    }
    default: return;
  }

  const DWORD levels = base->GetLevelCount();
  tex.levels.resize(levels);
  for(DWORD l = 0; l < levels; l++)
  {
    D3D9Emu::VSTexture::Level &level = tex.levels[l];

    uint32_t depth = slices;
    if(tex.type == D3D9BC::SamplerTextureType::Volume)
    {
      D3DVOLUME_DESC desc = {};
      ((IDirect3DVolumeTexture9 *)base)->GetLevelDesc(l, &desc);
      depth = RDCMAX(1U, (uint32_t)desc.Depth);
    }

    for(uint32_t z = 0; z < depth; z++)
    {
      const DecodedSubresource &decoded = GetDecoded(id, Subresource(l, z, 0));
      if(decoded.texels.empty())
      {
        RDCWARN("Couldn't read vertex texture %s level %u slice %u for texldl: %s",
                ToStr(id).c_str(), l, z, decoded.error.c_str());
        tex.levels.clear();
        return;
      }
      if(z == 0)
      {
        level.width = decoded.width;
        level.height = decoded.height;
        level.depth = depth;
        level.texels.reserve(size_t(decoded.width) * decoded.height * depth);
      }
      for(size_t t = 0; t < size_t(level.width) * level.height && t < decoded.texels.size(); t++)
        level.texels.push_back(SampledValue(format, decoded.texels[t]));
    }
  }

  bool srgb = false;
  for(const D3D9SamplerStates &s : state.SamplerStates)
  {
    if(s.Sampler != apiSampler)
      continue;
    for(const D3D9SamplerStateValue &v : s.States)
    {
      switch(v.State)
      {
        case D3DSAMP_ADDRESSU: tex.address[0] = D3D9Emu::TexAddress(v.Value); break;
        case D3DSAMP_ADDRESSV: tex.address[1] = D3D9Emu::TexAddress(v.Value); break;
        case D3DSAMP_ADDRESSW: tex.address[2] = D3D9Emu::TexAddress(v.Value); break;
        case D3DSAMP_BORDERCOLOR: tex.border = ColorDWORD(v.Value); break;
        case D3DSAMP_MAXMIPLEVEL: tex.maxMipLevel = v.Value; break;
        case D3DSAMP_MIPMAPLODBIAS: tex.mipLODBias = AsFloat(v.Value); break;
        case D3DSAMP_MAGFILTER: tex.magFilter = D3D9Emu::TexFilter(v.Value); break;
        case D3DSAMP_MINFILTER: tex.minFilter = D3D9Emu::TexFilter(v.Value); break;
        case D3DSAMP_MIPFILTER: tex.mipFilter = D3D9Emu::TexFilter(v.Value); break;
        case D3DSAMP_SRGBTEXTURE: srgb = v.Value != 0; break;
        default: break;
      }
    }
  }

  // sRGB reads convert the colour channels before filtering, as the GPU did. Only formats with 8
  // bits or fewer per channel are converted: the others can't be read as sRGB.
  if(srgb && SRGBReadable(format))
  {
    for(D3D9Emu::VSTexture::Level &level : tex.levels)
      for(D3D9Emu::Vec4 &t : level.texels)
        for(int c = 0; c < 3; c++)
          t[c] = ConvertSRGBToLinear(t[c]);
  }
}

void D3D9Replay::InitPostVSBuffers(const rdcarray<uint32_t> &passEvents)
{
  // The pass's draws before the current event (GetPassEvents), as D3D11 does: each draw's inputs
  // are read from the device at it, so replay to each one that isn't cached yet. The caller
  // replays back to its event afterwards.
  uint32_t prev = 0;
  for(uint32_t eventId : passEvents)
  {
    if(m_PostVS.find(eventId) != m_PostVS.end())
      continue;

    // a partial replay includes its first event: from prev's draw up to this one without its draw
    m_pDevice->ReplayLog(prev, eventId, eReplay_WithoutDraw);
    prev = eventId;
    // vertex textures are decoded from what they hold here
    ClearReplayCaches();

    InitPostVSBuffers(eventId);
  }
}

const D3D9Emu::PostVSResult *D3D9Replay::GetPostVSResult(uint32_t eventId, uint32_t instance)
{
  auto it = m_PostVS.find(eventId);
  if(it == m_PostVS.end())
    return NULL;

  PostVSEvent &ev = *it->second;
  instance = RDCMIN(instance, ev.numInstances - 1);

  auto res = ev.results.find(instance);
  if(res != ev.results.end())
    return &res->second;

  D3D9Emu::PostVSResult &ret = ev.results[instance];
  if(!ev.status.empty())
  {
    ret.status = ev.status;
    return &ret;
  }

  D3D9Emu::RunPostVS(ev.draw, instance, ev.hasVS ? &ev.vs : NULL, ev.consts, ev.textures,
                     ev.hasVS ? NULL : &ev.ffp, ev.signature, ret);

  if(!ret.status.empty())
    RDCWARN("No post-transform data for event %u: %s", eventId, ret.status.c_str());

  ResourceId vb = ResourceIDGen::GetNewUniqueID();
  ev.vertexBuffers[instance] = vb;
  m_PostVSBuffers[vb] = {eventId, instance, false};

  if(ret.indexed && ev.indexBuffer == ResourceId())
  {
    ev.indexBuffer = ResourceIDGen::GetNewUniqueID();
    m_PostVSBuffers[ev.indexBuffer] = {eventId, instance, true};
  }

  return &ret;
}

MeshFormat D3D9Replay::GetPostVSBuffers(uint32_t eventId, uint32_t instID, uint32_t viewID,
                                        MeshDataStage stage)
{
  MeshFormat ret;

  // there's no geometry or tessellation output, nor multiview
  (void)viewID;
  if(stage != MeshDataStage::VSOut)
    return ret;

  const D3D9Emu::PostVSResult *res = GetPostVSResult(eventId, instID);
  if(res == NULL)
    return ret;

  ret.status = res->status;
  if(res->vertexData.empty())
    return ret;

  PostVSEvent &ev = *m_PostVS[eventId];
  instID = RDCMIN(instID, ev.numInstances - 1);

  ret.vertexResourceId = ev.vertexBuffers[instID];
  ret.vertexByteOffset = 0;
  ret.vertexByteStride = res->stride;
  ret.vertexByteSize = res->vertexData.size();

  if(res->indexed)
  {
    ret.indexResourceId = ev.indexBuffer;
    ret.indexByteStride = 4;
    ret.indexByteSize = res->indices.byteSize();
  }
  ret.indexByteOffset = 0;
  ret.baseVertex = 0;

  // the position is the first element
  const rdcarray<size_t> order = D3D9Emu::PackOrder(ev.signature);
  uint32_t posComps = order.empty() ? 4 : ev.signature[order[0]].compCount;
  ret.format.type = ResourceFormatType::Regular;
  ret.format.compType = CompType::Float;
  ret.format.compByteWidth = 4;
  ret.format.compCount = uint8_t(RDCCLAMP(posComps, 1U, 4U));

  ret.topology = ev.topology;
  ret.numIndices = res->numRows;
  ret.showAlpha = false;
  // D3D9 has no primitive restart
  ret.allowRestart = false;

  ret.unproject = res->hasPosition && res->clipSpace;
  ret.nearPlane = res->nearPlane;
  ret.farPlane = res->farPlane;

  return ret;
}

bool D3D9Replay::IsPostVSBuffer(ResourceId id) const
{
  if(id == ResourceId())
    return false;
  return id == m_UPVertexBuffer || id == m_UPIndexBuffer ||
         m_PostVSBuffers.find(id) != m_PostVSBuffers.end();
}

BufferDescription D3D9Replay::GetPostVSBuffer(ResourceId id)
{
  BufferDescription ret;
  ret.resourceId = id;

  bytebuf data;
  GetPostVSBufferData(id, 0, 0, data);
  ret.length = data.size();

  if(id == m_UPIndexBuffer)
  {
    ret.creationFlags = BufferCategory::Index;
  }
  else if(id == m_UPVertexBuffer)
  {
    ret.creationFlags = BufferCategory::Vertex;
  }
  else
  {
    auto it = m_PostVSBuffers.find(id);
    ret.creationFlags = it != m_PostVSBuffers.end() && it->second.index ? BufferCategory::Index
                                                                        : BufferCategory::Vertex;
  }

  return ret;
}

void D3D9Replay::GetPostVSBufferData(ResourceId id, uint64_t offset, uint64_t len, bytebuf &retData)
{
  retData.clear();

  const byte *data = NULL;
  size_t size = 0;

  if(id == m_UPVertexBuffer || id == m_UPIndexBuffer)
  {
    // the data of the event the pipeline state is at
    const D3D9UserPrimitiveData *up = m_pDevice->GetUserPrimitives(m_PipelineEvent);
    if(up)
    {
      const bytebuf &src = id == m_UPVertexBuffer ? up->vertices : up->indices;
      data = src.data();
      size = src.size();
    }
  }
  else
  {
    auto it = m_PostVSBuffers.find(id);
    if(it == m_PostVSBuffers.end())
      return;
    const D3D9Emu::PostVSResult *res = GetPostVSResult(it->second.eventId, it->second.instance);
    if(res == NULL)
      return;
    if(it->second.index)
    {
      data = (const byte *)res->indices.data();
      size = res->indices.byteSize();
    }
    else
    {
      data = res->vertexData.data();
      size = res->vertexData.size();
    }
  }

  if(data == NULL || offset >= size)
    return;

  uint64_t count = size - offset;
  if(len > 0)
    count = RDCMIN(count, len);

  retData.assign(data + offset, (size_t)count);
}

void D3D9Replay::ClearPostVSCache()
{
  m_PostVS.clear();
  m_PostVSBuffers.clear();
}
