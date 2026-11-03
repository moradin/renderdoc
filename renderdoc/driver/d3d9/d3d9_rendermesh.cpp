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

#include "maths/camera.h"
#include "maths/matrix.h"
#include "d3d9_device.h"
#include "d3d9_replay.h"
#include "d3d9_resources.h"

// The mesh viewer
// ---------------
// D3D9 vertex declarations can't describe every format the mesh viewer is given, and the vertex
// data's buffers belong to the replay, so the positions (and the secondary element) are decoded
// on the CPU, with the index buffer expanded, into a vertex buffer of our own (MeshVertex). That's
// cached per draw until the next replay. The draw then needs no index buffer, keeps its
// topology, and works the same for any format.
//
// D3D9 has no geometry shaders, so lit shading gets its face normal from the derivatives of the
// view space position in the pixel shader instead of D3D11's geometry shader. Only the input
// vertex data (the VS input stage) is shown, post-transform data comes with phase 3e.

// in d3d9_rendertexture.cpp
void D3D9SetDebugDrawState(IDirect3DDevice9 *dev);

// c0-c3: model view projection, c4: x = positions are homogenous, yz = sprite size, w = exploder
// scale, c5: xyz = exploder centre, w = exploder slider, c6-c9: inverse projection. The matrices
// are row_major like D3D11's mesh cbuffer, so Matrix4f's data goes in as it is.
const char *D3D9MeshVS = R"(
row_major float4x4 ModelViewProj : register(c0);
float4 params : register(c4);
float4 exploder : register(c5);
row_major float4x4 InvProj : register(c6);

struct a2v
{
  float4 pos : POSITION;
  float4 secondary : TEXCOORD0;
  float vid : TEXCOORD1;
};

struct v2f
{
  float4 pos : POSITION;
  float4 secondary : TEXCOORD0;
  float4 view : TEXCOORD1;
};

float3 Gradient(float idx)
{
  if(idx < 0.5f)
    return float3(0.004f, 0.002f, 0.025f);
  if(idx < 1.5f)
    return float3(0.305f, 0.001f, 0.337f);
  if(idx < 2.5f)
    return float3(0.665f, 0.033f, 0.133f);
  if(idx < 3.5f)
    return float3(1.000f, 0.468f, 0.000f);
  return float3(1.000f, 1.000f, 1.000f);
}

v2f main(a2v IN)
{
  float4 pos = IN.pos;
  float3 secondary = IN.secondary.xyz;

  // the exploder, as in mesh.hlsl with float maths
  if(params.w > 0.0f)
  {
    float nonLinear = 4.0f * params.w * exploder.w * exploder.w * exploder.w;
    float3 offset = pos.xyz - exploder.xyz;
    float3 dir = offset * rsqrt(max(dot(offset, offset), 1.192092896e-07f));
    float bits = fmod(floor(IN.vid / 2.0f), 16.0f);
    pos.xyz += dir * (nonLinear * ((bits / 15.0f) * 1.5f - 0.5f));

    float m = fmod(IN.vid, 32.0f) / 16.0f;
    float g = abs(m - 2.0f * floor(m));
    float startIdx = floor(g * 4.0f);
    secondary = lerp(Gradient(startIdx), Gradient(min(startIdx + 1.0f, 4.0f)),
                     g - startIdx * 0.25f);
  }

  if(params.x < 0.5f)
    pos = float4(pos.xyz, 1.0f);

  v2f OUT;
  OUT.pos = mul(pos, ModelViewProj);

  // sprites for highlighted vertices, the corners by vertex number
  float corner = fmod(IN.vid, 4.0f);
  float2 sprite = float2(corner < 1.5f ? -1.0f : 1.0f, fmod(corner, 2.0f) > 0.5f ? 1.0f : -1.0f);
  OUT.pos.xy += params.yz * 0.01f * sprite * OUT.pos.w;

  OUT.secondary = float4(secondary, IN.secondary.w);
  OUT.view = mul(OUT.pos, InvProj);
  return OUT;
}
)";

// c0: the colour, c1.x: the display mode, MESHDISPLAY_* in hlsl_cbuffers.h
const char *D3D9MeshPS = R"(
float4 colour : register(c0);
float4 mode : register(c1);

float4 main(float4 secondary : TEXCOORD0, float4 view : TEXCOORD1) : COLOR0
{
  float type = mode.x;

  float3 p = view.xyz / view.w;
  float3 norm = normalize(cross(ddx(p), ddy(p)));
  float3 lightDir = normalize(float3(0, -0.3f, -1));

  if(type > 2.5f && type < 3.5f)
    return float4(secondary.xyz, 1);
  if(type > 5.5f)
    return float4(secondary.www, 1);
  if(type > 1.5f && type < 2.5f)
    return float4(colour.xyz * abs(dot(lightDir, norm)), 1);
  if(type > 3.5f && type < 4.5f)
    return float4(secondary.xyz * abs(dot(lightDir, norm)), 1);
  return float4(colour.xyz, 1);
}
)";

namespace
{
// the vertex layout of meshDecl, see InitDebugRender
struct MeshVertex
{
  Vec4f pos;
  Vec4f secondary;
  float vid;
};

// MESHDISPLAY_* in hlsl_cbuffers.h
enum
{
  MeshDisplaySolid = 1,
  MeshDisplaySecondary = 3,
  MeshDisplaySecondaryAlpha = 6,
};

float MeshDisplayMode(const MeshDisplay &cfg)
{
  switch(cfg.visualisationMode)
  {
    case Visualisation::Lit:
    case Visualisation::Explode: return float(cfg.visualisationMode);
    case Visualisation::Secondary:
      return float(cfg.second.showAlpha ? MeshDisplaySecondaryAlpha : MeshDisplaySecondary);
    default: return float(MeshDisplaySolid);
  }
}

uint64_t HashValue(uint64_t hash, uint64_t val)
{
  return (hash << 5) + hash + val;
}

uint64_t HashFormat(uint64_t hash, const MeshFormat &fmt)
{
  uint64_t id;
  memcpy(&id, &fmt.vertexResourceId, sizeof(id));
  hash = HashValue(hash, id);
  memcpy(&id, &fmt.indexResourceId, sizeof(id));
  hash = HashValue(hash, id);
  hash = HashValue(hash, fmt.vertexByteOffset);
  hash = HashValue(hash, fmt.vertexByteStride);
  hash = HashValue(hash, fmt.indexByteOffset);
  hash = HashValue(hash, fmt.indexByteStride);
  hash = HashValue(hash, (uint64_t)fmt.baseVertex);
  hash = HashValue(hash, fmt.numIndices);
  hash = HashValue(hash, (uint64_t)fmt.topology);
  hash = HashValue(hash, fmt.instanced ? fmt.instStepRate + 1 : 0);
  hash = HashValue(hash, (uint64_t)fmt.format.type);
  hash = HashValue(hash, (uint64_t)fmt.format.compType);
  hash = HashValue(hash, fmt.format.compCount);
  hash = HashValue(hash, fmt.format.compByteWidth);
  hash = HashValue(hash, fmt.format.BGRAOrder() ? 1 : 0);
  return hash;
}

// the primitive type and count for a number of vertices, or false for topologies D3D9 can't draw
bool D3D9Primitive(Topology topo, uint32_t numVerts, D3DPRIMITIVETYPE &type, uint32_t &count)
{
  switch(topo)
  {
    case Topology::PointList:
      type = D3DPT_POINTLIST;
      count = numVerts;
      break;
    case Topology::LineList:
      type = D3DPT_LINELIST;
      count = numVerts / 2;
      break;
    case Topology::LineStrip:
      type = D3DPT_LINESTRIP;
      count = numVerts >= 2 ? numVerts - 1 : 0;
      break;
    case Topology::TriangleList:
      type = D3DPT_TRIANGLELIST;
      count = numVerts / 3;
      break;
    case Topology::TriangleStrip:
      type = D3DPT_TRIANGLESTRIP;
      count = numVerts >= 3 ? numVerts - 2 : 0;
      break;
    case Topology::TriangleFan:
      type = D3DPT_TRIANGLEFAN;
      count = numVerts >= 3 ? numVerts - 2 : 0;
      break;
    default:
      // anything else shows its vertices
      type = D3DPT_POINTLIST;
      count = numVerts;
      break;
  }
  return count > 0;
}

void SetMeshConstants(IDirect3DDevice9 *dev, const Matrix4f &mvp, const Matrix4f &invProj,
                      bool homogenous, Vec2f spriteSize, const MeshDisplay *exploder)
{
  dev->SetVertexShaderConstantF(0, mvp.Data(), 4);

  float params[8] = {homogenous ? 1.0f : 0.0f, spriteSize.x, spriteSize.y, 0.0f};
  if(exploder && exploder->visualisationMode == Visualisation::Explode)
  {
    params[3] = exploder->exploderScale;
    params[4] = (exploder->minBounds.x + exploder->maxBounds.x) * 0.5f;
    params[5] = (exploder->minBounds.y + exploder->maxBounds.y) * 0.5f;
    params[6] = (exploder->minBounds.z + exploder->maxBounds.z) * 0.5f;
    params[7] = exploder->vtxExploderSliderSNorm;
  }
  dev->SetVertexShaderConstantF(4, params, 2);

  dev->SetVertexShaderConstantF(6, invProj.Data(), 4);
}

void SetMeshColour(IDirect3DDevice9 *dev, Vec4f col, float mode)
{
  const float consts[8] = {col.x, col.y, col.z, col.w, mode, 0.0f, 0.0f, 0.0f};
  dev->SetPixelShaderConstantF(0, consts, 2);
}

// draws helper vertices given as positions
void DrawPositions(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, uint32_t count,
                   const FloatVector *positions, uint32_t numVerts)
{
  if(count == 0 || numVerts == 0)
    return;

  rdcarray<MeshVertex> verts;
  verts.resize(numVerts);
  for(uint32_t i = 0; i < numVerts; i++)
  {
    verts[i].pos = Vec4f(positions[i].x, positions[i].y, positions[i].z, positions[i].w);
    verts[i].secondary = Vec4f();
    verts[i].vid = float(i);
  }

  dev->DrawPrimitiveUP(type, count, verts.data(), sizeof(MeshVertex));
}
};

const D3D9Replay::MeshVertices *D3D9Replay::GetMeshVertices(uint32_t eventId, const MeshFormat &pos,
                                                            const MeshFormat *secondary,
                                                            uint32_t curInstance)
{
  uint64_t key = HashValue(5381, eventId);
  key = HashFormat(key, pos);
  if(secondary)
  {
    key = HashFormat(HashValue(key, 1), *secondary);
    if(secondary->instanced)
      key = HashValue(key, curInstance);
  }

  auto it = m_MeshCache.find(key);
  if(it != m_MeshCache.end())
    return it->second.vb ? &it->second : NULL;

  MeshVertices &ret = m_MeshCache[key];

  // HighlightCache reads the buffers and applies the indices and base vertex
  HighlightCache posData;
  posData.driver = this;
  MeshDisplay posCfg;
  posCfg.position = pos;
  posCfg.type = MeshDataStage::VSIn;
  posData.CacheHighlightingData(eventId, posCfg);

  // the secondary element uses the position's indices
  HighlightCache secData;
  secData.driver = this;
  MeshDisplay secCfg;
  if(secondary && secondary->vertexResourceId != ResourceId())
  {
    secCfg.position = *secondary;
    secCfg.position.indexResourceId = pos.indexResourceId;
    secCfg.position.indexByteOffset = pos.indexByteOffset;
    secCfg.position.indexByteStride = pos.indexByteStride;
    secCfg.position.baseVertex = pos.baseVertex;
    secCfg.position.numIndices = pos.numIndices;
    secCfg.position.topology = pos.topology;
    secCfg.position.allowRestart = pos.allowRestart;
    secCfg.position.restartIndex = pos.restartIndex;
    secCfg.type = MeshDataStage::VSIn;

    // per-instance data is the same for every vertex
    if(secondary->instanced)
    {
      if(secondary->instStepRate)
        secCfg.position.vertexByteOffset +=
            uint64_t(secondary->vertexByteStride) * (curInstance / secondary->instStepRate);
      secCfg.position.indexByteStride = 0;
      secCfg.position.indexResourceId = ResourceId();
    }

    secData.CacheHighlightingData(eventId, secCfg);
  }
  else
  {
    secondary = NULL;
  }

  const uint32_t numVerts = pos.numIndices;
  if(numVerts == 0 || posData.vertexData.empty())
    return NULL;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  HRESULT hr = dev->CreateVertexBuffer(UINT(numVerts * sizeof(MeshVertex)), D3DUSAGE_WRITEONLY, 0,
                                       D3DPOOL_DEFAULT, &ret.vb, NULL);
  MeshVertex *verts = NULL;
  if(SUCCEEDED(hr))
    hr = ret.vb->Lock(0, 0, (void **)&verts, 0);

  if(FAILED(hr) || verts == NULL)
  {
    RDCERR("Couldn't create the mesh viewer's vertex buffer: %s", ToStr(hr).c_str());
    SAFE_RELEASE(ret.vb);
    return NULL;
  }

  const byte *posBegin = posData.vertexData.data();
  const byte *posEnd = posBegin + posData.vertexData.size();
  const byte *secBegin = secData.vertexData.data();
  const byte *secEnd = secBegin + secData.vertexData.size();

  for(uint32_t i = 0; i < numVerts; i++)
  {
    bool valid = true;
    FloatVector p = posData.InterpretVertex(posBegin, i, posCfg, posEnd, true, valid);

    FloatVector s(0.0f, 0.0f, 0.0f, 1.0f);
    if(secondary)
    {
      bool secValid = true;
      if(secondary->instanced)
        s = HighlightCache::InterpretVertex(secBegin, 0, secondary->vertexByteStride,
                                            secondary->format, secEnd, secValid);
      else
        s = secData.InterpretVertex(secBegin, i, secCfg, secEnd, true, secValid);
    }

    verts[i].pos = Vec4f(p.x, p.y, p.z, p.w);
    verts[i].secondary = Vec4f(s.x, s.y, s.z, s.w);
    // the vertex index, as SV_VertexID would give it, for the exploder
    uint32_t vid = i;
    if(posData.idxData && i < posData.indices.size())
      vid = posData.indices[i];
    verts[i].vid = float(vid);
  }

  ret.vb->Unlock();
  ret.numVerts = numVerts;
  return &ret;
}

void D3D9Replay::RenderMesh(uint32_t eventId, const rdcarray<MeshFormat> &secondaryDraws,
                            const MeshDisplay &cfg)
{
  if(cfg.position.vertexResourceId == ResourceId() || cfg.position.numIndices == 0)
    return;

  OutputTarget target;
  if(!GetOutputTarget(target) || !InitDebugRender() || m_Debug.meshVS == NULL)
    return;

  // the vertices first: reading the buffers doesn't touch the device's state
  const MeshFormat *second = NULL;
  if(cfg.visualisationMode == Visualisation::Secondary && cfg.second.vertexResourceId != ResourceId())
    second = &cfg.second;
  const MeshVertices *mesh = GetMeshVertices(eventId, cfg.position, second, cfg.curInstance);

  rdcarray<const MeshVertices *> secondaryMeshes;
  for(const MeshFormat &fmt : secondaryDraws)
    if(fmt.vertexResourceId != ResourceId())
      secondaryMeshes.push_back(GetMeshVertices(eventId, fmt, NULL, 0));

  if(cfg.highlightVert != ~0U)
    m_HighlightCache.CacheHighlightingData(eventId, cfg);

  const float outW = float(target.width), outH = float(target.height);

  const float nearPlane = cfg.cam ? ((Camera *)cfg.cam)->GetNear() : 0.1f;
  const float farPlane = cfg.cam ? ((Camera *)cfg.cam)->GetFar() : 100000.0f;

  const Matrix4f projMat = Matrix4f::Perspective(90.0f, nearPlane, farPlane, outW / outH);
  const Matrix4f invProj = projMat.Inverse();
  const Matrix4f camMat = cfg.cam ? ((Camera *)cfg.cam)->GetMatrix() : Matrix4f::Identity();
  const Matrix4f axisMapMat = Matrix4f(cfg.axisMapping);

  const Matrix4f helperMVP = projMat.Mul(camMat.Mul(axisMapMat));
  Matrix4f meshMVP = helperMVP;
  Matrix4f guessProjInv;

  if(cfg.position.unproject)
  {
    // the derivation of the projection matrix might not be right (it could be an orthographic
    // projection), but it'll likely be close enough
    Matrix4f guessProj =
        cfg.position.farPlane != FLT_MAX
            ? Matrix4f::Perspective(cfg.fov, cfg.position.nearPlane, cfg.position.farPlane, cfg.aspect)
            : Matrix4f::ReversePerspective(cfg.fov, cfg.position.nearPlane, cfg.aspect);

    if(cfg.ortho)
      guessProj = Matrix4f::Orthographic(cfg.position.nearPlane, cfg.position.farPlane);

    if(cfg.position.flipY)
      guessProj[5] *= -1.0f;

    guessProjInv = guessProj.Inverse();
    meshMVP = projMat.Mul(camMat.Mul(guessProjInv));
  }

  D3DCAPS9 caps = {};
  IDirect3DDevice9 *dev = m_pDevice->GetReal();
  dev->GetDeviceCaps(&caps);

  auto drawMesh = [&](const MeshVertices *verts, Topology topo) {
    D3DPRIMITIVETYPE type = D3DPT_POINTLIST;
    uint32_t count = 0;
    if(verts == NULL || !D3D9Primitive(topo, verts->numVerts, type, count))
      return;
    if(count > caps.MaxPrimitiveCount)
    {
      RDCWARN("Mesh has %u primitives, only drawing the first %u", count, caps.MaxPrimitiveCount);
      count = caps.MaxPrimitiveCount;
    }
    dev->SetStreamSource(0, verts->vb, 0, sizeof(MeshVertex));
    dev->DrawPrimitive(type, 0, count);
  };

  D3D9DeviceState state;
  if(!m_pDevice->SaveReplayDrawState(state))
    return;

  const bool ownScene = SUCCEEDED(dev->BeginScene());

  D3D9SetDebugDrawState(dev);
  dev->SetRenderTarget(0, target.rt);
  dev->SetDepthStencilSurface(target.ds);
  const D3DVIEWPORT9 viewport = {0, 0, (DWORD)target.width, (DWORD)target.height, 0.0f, 1.0f};
  dev->SetViewport(&viewport);

  dev->SetVertexShader(m_Debug.meshVS);
  dev->SetPixelShader(m_Debug.meshPS);
  dev->SetVertexDeclaration(m_Debug.meshDecl);
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, m_Debug.srgbWrite ? TRUE : FALSE);

  const DWORD depth = target.ds ? D3DZB_TRUE : D3DZB_FALSE;

  // the other draws in the pass, in wireframe
  SetMeshConstants(dev, helperMVP, invProj, false, Vec2f(), NULL);
  dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
  for(size_t i = 0, m = 0; i < secondaryDraws.size(); i++)
  {
    const MeshFormat &fmt = secondaryDraws[i];
    if(fmt.vertexResourceId == ResourceId())
      continue;

    const MeshVertices *verts = secondaryMeshes[m++];
    SetMeshColour(dev, Vec4f(fmt.meshColor.x, fmt.meshColor.y, fmt.meshColor.z, 1.0f),
                  float(MeshDisplaySolid));
    drawMesh(verts, fmt.topology);
  }

  SetMeshConstants(dev, meshMVP, invProj, cfg.position.unproject, Vec2f(), &cfg);

  // solid shading
  if(cfg.visualisationMode != Visualisation::NoSolid)
  {
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_ZENABLE, depth);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
    SetMeshColour(dev, Vec4f(0.8f, 0.8f, 0.0f, 1.0f), MeshDisplayMode(cfg));
    drawMesh(mesh, cfg.position.topology);
  }

  // wireframe
  if(cfg.visualisationMode == Visualisation::NoSolid || cfg.wireframeDraw)
  {
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
    dev->SetRenderState(D3DRS_ZENABLE, depth);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    SetMeshColour(
        dev,
        Vec4f(cfg.position.meshColor.x, cfg.position.meshColor.y, cfg.position.meshColor.z, 1.0f),
        float(MeshDisplaySolid));
    drawMesh(mesh, cfg.position.topology);
  }

  // helpers, solid and without depth
  dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  SetMeshConstants(dev, helperMVP, invProj, false, Vec2f(), NULL);

  // axis markers
  if(!cfg.position.unproject)
  {
    const FloatVector axis[6] = {
        FloatVector(0.0f, 0.0f, 0.0f, 1.0f), FloatVector(1.0f, 0.0f, 0.0f, 1.0f),
        FloatVector(0.0f, 0.0f, 0.0f, 1.0f), FloatVector(0.0f, 1.0f, 0.0f, 1.0f),
        FloatVector(0.0f, 0.0f, 0.0f, 1.0f), FloatVector(0.0f, 0.0f, 1.0f, 1.0f),
    };
    for(int i = 0; i < 3; i++)
    {
      SetMeshColour(dev,
                    Vec4f(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f, 1.0f),
                    float(MeshDisplaySolid));
      DrawPositions(dev, D3DPT_LINELIST, 1, &axis[i * 2], 2);
    }
  }

  // the highlighted vertex, its primitive and the ones next to it
  if(cfg.highlightVert != ~0U)
  {
    FloatVector activeVertex;
    rdcarray<FloatVector> activePrim, adjacentPrimVertices, inactiveVertices;

    const bool lines = cfg.position.topology == Topology::LineList ||
                       cfg.position.topology == Topology::LineStrip ||
                       cfg.position.topology == Topology::LineList_Adj ||
                       cfg.position.topology == Topology::LineStrip_Adj;
    const uint32_t primSize = lines ? 2 : 3;
    const D3DPRIMITIVETYPE primType = lines ? D3DPT_LINELIST : D3DPT_TRIANGLELIST;

    if(m_HighlightCache.FetchHighlightPositions(cfg, activeVertex, activePrim, adjacentPrimVertices,
                                                inactiveVertices))
    {
      const Matrix4f &mvp = cfg.position.unproject ? meshMVP : helperMVP;
      SetMeshConstants(dev, mvp, invProj, cfg.position.unproject, Vec2f(), NULL);

      // the active primitive in red
      SetMeshColour(dev, Vec4f(1.0f, 0.0f, 0.0f, 1.0f), float(MeshDisplaySolid));
      if(activePrim.size() >= primSize)
        DrawPositions(dev, primType, 1, activePrim.data(), primSize);

      // adjacent primitives in green
      SetMeshColour(dev, Vec4f(0.0f, 1.0f, 0.0f, 1.0f), float(MeshDisplaySolid));
      if(adjacentPrimVertices.size() >= primSize && (adjacentPrimVertices.size() % primSize) == 0)
        DrawPositions(dev, primType, uint32_t(adjacentPrimVertices.size() / primSize),
                      adjacentPrimVertices.data(), (uint32_t)adjacentPrimVertices.size());

      // vertices as sprites
      const float scale = 800.0f / outH;
      const float asp = outW / outH;
      SetMeshConstants(dev, mvp, invProj, cfg.position.unproject, Vec2f(scale / asp, scale), NULL);

      // the active vertex in blue
      SetMeshColour(dev, Vec4f(0.0f, 0.0f, 1.0f, 1.0f), float(MeshDisplaySolid));
      FloatVector sprite[4] = {activeVertex, activeVertex, activeVertex, activeVertex};
      DrawPositions(dev, D3DPT_TRIANGLESTRIP, 2, sprite, 4);

      // other vertices in green
      SetMeshColour(dev, Vec4f(0.0f, 1.0f, 0.0f, 1.0f), float(MeshDisplaySolid));
      for(const FloatVector &v : inactiveVertices)
      {
        sprite[0] = sprite[1] = sprite[2] = sprite[3] = v;
        DrawPositions(dev, D3DPT_TRIANGLESTRIP, 2, sprite, 4);
      }

      SetMeshConstants(dev, helperMVP, invProj, false, Vec2f(), NULL);
    }
  }

  // bounding box
  if(cfg.showBBox)
  {
    const FloatVector a = cfg.minBounds, b = cfg.maxBounds;

    const FloatVector TLN(a.x, b.y, a.z, 1.0f), TRN(b.x, b.y, a.z, 1.0f);
    const FloatVector BLN(a.x, a.y, a.z, 1.0f), BRN(b.x, a.y, a.z, 1.0f);
    const FloatVector TLF(a.x, b.y, b.z, 1.0f), TRF(b.x, b.y, b.z, 1.0f);
    const FloatVector BLF(a.x, a.y, b.z, 1.0f), BRF(b.x, a.y, b.z, 1.0f);

    const FloatVector bbox[24] = {
        TLN, TRN, TRN, BRN, BRN, BLN, BLN, TLN, TLN, TLF, TRN, TRF,
        BLN, BLF, BRN, BRF, TLF, TRF, TRF, BRF, BRF, BLF, BLF, TLF,
    };

    // this one clips against the mesh
    dev->SetRenderState(D3DRS_ZENABLE, depth);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    SetMeshColour(dev, Vec4f(0.2f, 0.2f, 1.0f, 1.0f), float(MeshDisplaySolid));
    DrawPositions(dev, D3DPT_LINELIST, 12, bbox, 24);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
  }

  // the frustum the post-transform data was guessed to come from
  if(cfg.position.unproject)
  {
    const FloatVector TLN(-1.0f, 1.0f, 0.0f, 1.0f), TRN(1.0f, 1.0f, 0.0f, 1.0f);
    const FloatVector BLN(-1.0f, -1.0f, 0.0f, 1.0f), BRN(1.0f, -1.0f, 0.0f, 1.0f);
    const FloatVector TLF(-1.0f, 1.0f, 1.0f, 1.0f), TRF(1.0f, 1.0f, 1.0f, 1.0f);
    const FloatVector BLF(-1.0f, -1.0f, 1.0f, 1.0f), BRF(1.0f, -1.0f, 1.0f, 1.0f);

    const FloatVector frustum[24] = {
        TLN, TRN, TRN, BRN, BRN, BLN, BLN, TLN, TLN, TLF, TRN, TRF,
        BLN, BLF, BRN, BRF, TLF, TRF, TRF, BRF, BRF, BLF, BLF, TLF,
    };

    SetMeshConstants(dev, projMat.Mul(camMat.Mul(guessProjInv)), invProj, false, Vec2f(), NULL);
    SetMeshColour(dev, Vec4f(1.0f, 1.0f, 1.0f, 1.0f), float(MeshDisplaySolid));
    DrawPositions(dev, D3DPT_LINELIST, 12, frustum, 24);
  }

  dev->SetStreamSource(0, NULL, 0, 0);

  if(ownScene)
    dev->EndScene();

  m_pDevice->RestoreReplayDrawState(state);
}
