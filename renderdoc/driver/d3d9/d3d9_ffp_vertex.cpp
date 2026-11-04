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

#include "d3d9_ffp_vertex.h"
#include <math.h>
#include "common/common.h"
#include "common/formatting.h"

// What's emulated, in camera space (D3D9's, looking down +z):
// - position: the world matrices blended by D3DRS_VERTEXBLEND (1-3 weights from the vertex and
//   one more for the remainder, or 0WEIGHTS, or tweening between POSITION0 and POSITION1), with
//   D3DRS_INDEXEDVERTEXBLENDENABLE taking each matrix from the vertex's blend indices. Each
//   matrix is World[i] * View, the result goes through Projection.
// - normals: through the inverse transpose of each World[i] * View, blended the same way, and
//   normalized with D3DRS_NORMALIZENORMALS.
// - lighting: D3DRS_AMBIENT plus each enabled light's ambient, diffuse and specular (Blinn-Phong
//   half vector), attenuated by distance and the spot cone for point and spot lights, with the
//   material colours taken from the vertex's colours by the D3DRS_*MATERIALSOURCE states when
//   D3DRS_COLORVERTEX is on. Specular only with D3DRS_SPECULARENABLE, and only where N.L > 0.
//   The direction to the viewer is -position with D3DRS_LOCALVIEWER, otherwise (0, 0, -1).
//   Results are clamped to 0-1, and the diffuse alpha is the diffuse source's alpha.
// - texture coordinates: per stage, D3DTSS_TEXCOORDINDEX picks an input set or generates
//   camera space normals, positions, reflection vectors or sphere map coordinates, then
//   D3DTSS_TEXTURETRANSFORMFLAGS applies the stage's matrix to the coordinate padded with a 1
//   after its last component (so a 2D coordinate's translation is the matrix's third row). The
//   projected divide happens per pixel, so it doesn't change the vertex's output.
// - fog: with D3DRS_FOGENABLE and no table fog, the vertex fog factor from camera space |z| (or
//   the distance, with D3DRS_RANGEFOGENABLE), or the specular alpha without a vertex fog mode.
// - point size: the vertex's or D3DRS_POINTSIZE, scaled by distance with D3DRS_POINTSCALEENABLE,
//   clamped to D3DRS_POINTSIZE_MIN and _MAX.
// - pretransformed (POSITIONT, XYZRHW) vertices pass through, unlit and untransformed.

namespace D3D9Emu
{
namespace
{
// the direction to the viewer without D3DRS_LOCALVIEWER: the camera looks down +z
const float NonLocalViewer[3] = {0.0f, 0.0f, -1.0f};

float Saturate(float f)
{
  if(!(f > 0.0f))
    return 0.0f;
  return f > 1.0f ? 1.0f : f;
}

float Dot3(const float *a, const float *b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Normalize3(float *v)
{
  float l = sqrtf(Dot3(v, v));
  if(l > 0.0f)
  {
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
  }
}

// a direction through the upper 3x3 of m
void TransformDir(const float *d, const Matrix &m, float *out)
{
  for(int c = 0; c < 3; c++)
    out[c] = d[0] * m.m[c] + d[1] * m.m[4 + c] + d[2] * m.m[8 + c];
}

// a normal through the inverse transpose of m's upper 3x3: the cofactors divided by the
// determinant
void TransformNormal(const float *n, const Matrix &m, float *out)
{
  auto a = [&m](int r, int c) { return m.m[r * 4 + c]; };

  float cof[3][3];
  for(int r = 0; r < 3; r++)
  {
    for(int c = 0; c < 3; c++)
    {
      const int r0 = (r + 1) % 3, r1 = (r + 2) % 3;
      const int c0 = (c + 1) % 3, c1 = (c + 2) % 3;
      cof[r][c] = a(r0, c0) * a(r1, c1) - a(r0, c1) * a(r1, c0);
    }
  }

  const float det = a(0, 0) * cof[0][0] + a(0, 1) * cof[0][1] + a(0, 2) * cof[0][2];
  if(det == 0.0f)
  {
    out[0] = out[1] = out[2] = 0.0f;
    return;
  }

  for(int c = 0; c < 3; c++)
    out[c] = (n[0] * cof[0][c] + n[1] * cof[1][c] + n[2] * cof[2][c]) / det;
}

Vec4 Lerp(const Vec4 &a, const Vec4 &b, float t)
{
  Vec4 ret;
  for(int i = 0; i < 4; i++)
    ret[i] = a[i] * (1.0f - t) + b[i] * t;
  return ret;
}

// the reflection of the view direction about the normal, both in camera space
void Reflection(const FFPState &state, const float *viewPos, const float *n, float *r)
{
  float i[3] = {NonLocalViewer[0], NonLocalViewer[1], NonLocalViewer[2]};
  if(state.localViewer)
  {
    // from the camera to the vertex
    i[0] = viewPos[0];
    i[1] = viewPos[1];
    i[2] = viewPos[2];
    Normalize3(i);
  }
  else
  {
    i[0] = -i[0];
    i[1] = -i[1];
    i[2] = -i[2];
  }
  const float d = 2.0f * Dot3(n, i);
  for(int c = 0; c < 3; c++)
    r[c] = i[c] - d * n[c];
}

void AddSig(rdcarray<SigParameter> &sig, const char *name, uint32_t semanticIndex, uint32_t comps,
            ShaderBuiltin builtin)
{
  SigParameter p;
  p.semanticName = name;
  p.semanticIndex = uint16_t(semanticIndex);
  p.regIndex = (uint32_t)sig.size();
  p.systemValue = builtin;
  p.varType = VarType::Float;
  p.compCount = uint8_t(comps);
  p.regChannelMask = uint8_t((1 << comps) - 1);
  p.channelUsedMask = p.regChannelMask;
  sig.push_back(p);
}
};    // namespace

Vec4 Transform(const Vec4 &v, const Matrix &m)
{
  Vec4 ret;
  for(int c = 0; c < 4; c++)
    ret[c] = v[0] * m.m[c] + v[1] * m.m[4 + c] + v[2] * m.m[8 + c] + v[3] * m.m[12 + c];
  return ret;
}

Matrix Multiply(const Matrix &a, const Matrix &b)
{
  Matrix ret;
  for(int r = 0; r < 4; r++)
    for(int c = 0; c < 4; c++)
      ret.m[r * 4 + c] = a.m[r * 4 + 0] * b.m[0 + c] + a.m[r * 4 + 1] * b.m[4 + c] +
                         a.m[r * 4 + 2] * b.m[8 + c] + a.m[r * 4 + 3] * b.m[12 + c];
  return ret;
}

void FFPInputs::FromElements(const rdcarray<InputElement> &elements)
{
  *this = FFPInputs();

  for(const InputElement &el : elements)
  {
    if(el.method != 0 || DeclTypeSize(el.type) == 0)
      continue;

    switch(el.usage)
    {
      case D3D9BC::DeclUsage::Position:
        if(el.usageIndex == 0)
          position = true;
        else if(el.usageIndex == 1)
          position1 = true;
        break;
      case D3D9BC::DeclUsage::PositionT:
        if(el.usageIndex == 0)
          positionT = true;
        break;
      case D3D9BC::DeclUsage::Normal:
        if(el.usageIndex == 0)
          normal = true;
        else if(el.usageIndex == 1)
          normal1 = true;
        break;
      case D3D9BC::DeclUsage::Color:
        if(el.usageIndex == 0)
          diffuse = true;
        else if(el.usageIndex == 1)
          specular = true;
        break;
      case D3D9BC::DeclUsage::PSize:
        if(el.usageIndex == 0)
          psize = true;
        break;
      case D3D9BC::DeclUsage::BlendWeight:
        if(el.usageIndex == 0)
          blendWeights = DeclTypeComponents(el.type);
        break;
      case D3D9BC::DeclUsage::BlendIndices:
        if(el.usageIndex == 0)
        {
          blendIndices = true;
          blendIndicesNormalized = el.type == DeclType::D3DColor || el.type == DeclType::UByte4N;
        }
        break;
      case D3D9BC::DeclUsage::TexCoord:
        if(el.usageIndex < MaxTextureStages)
          texcoordDims[el.usageIndex] = DeclTypeComponents(el.type);
        break;
      default: break;
    }
  }
}

FFPOutputLayout FFPLayout(const FFPState &state, const FFPInputs &inputs)
{
  FFPOutputLayout ret;
  ret.pretransformed = inputs.positionT;
  ret.fog = state.fogEnable && state.fogTableMode == FogMode::None;
  ret.psize = state.points || inputs.psize;

  for(uint32_t i = 0; i < MaxTextureStages && i < state.outputStages; i++)
  {
    const uint32_t idx = state.texCoordIndex[i] & 0xFFFF;
    uint32_t gen = state.texCoordIndex[i] & 0xFFFF0000;
    // pretransformed vertices have no camera space to generate coordinates in
    if(ret.pretransformed)
      gen = TexGenPassthru;

    uint32_t dims = 0;
    switch(gen)
    {
      case TexGenPassthru: dims = idx < MaxTextureStages ? inputs.texcoordDims[idx] : 0; break;
      case TexGenCameraSpaceNormal:
      case TexGenCameraSpacePosition:
      case TexGenCameraSpaceReflection: dims = 3; break;
      case TexGenSphereMap: dims = 2; break;
      default: dims = 0; break;
    }

    const uint32_t count = state.textureTransformFlags[i] & 0xFF;
    if(dims > 0 && !ret.pretransformed && count >= 1 && count <= 4)
      dims = count;

    ret.texcoordDims[i] = dims;
  }

  return ret;
}

rdcarray<SigParameter> FFPSignature(const FFPOutputLayout &layout)
{
  rdcarray<SigParameter> ret;

  AddSig(ret, layout.pretransformed ? "POSITIONT" : "POSITION", 0, 4, ShaderBuiltin::Position);
  AddSig(ret, "COLOR", 0, 4, ShaderBuiltin::Undefined);
  AddSig(ret, "COLOR", 1, 4, ShaderBuiltin::Undefined);
  for(uint32_t i = 0; i < MaxTextureStages; i++)
    if(layout.texcoordDims[i])
      AddSig(ret, "TEXCOORD", i, layout.texcoordDims[i], ShaderBuiltin::Undefined);
  if(layout.fog)
    AddSig(ret, "FOG", 0, 1, ShaderBuiltin::Undefined);
  if(layout.psize)
    AddSig(ret, "PSIZE", 0, 1, ShaderBuiltin::PointSize);

  // named as the shader reflection names them: the index shown when it isn't 0 or the semantic
  // appears more than once
  for(SigParameter &a : ret)
  {
    a.needSemanticIndex = a.semanticIndex != 0;
    for(const SigParameter &b : ret)
      if(&a != &b && a.semanticName == b.semanticName)
        a.needSemanticIndex = true;
    a.semanticIdxName = a.semanticName;
    if(a.needSemanticIndex)
      a.semanticIdxName += StringFormat::Fmt("%u", uint32_t(a.semanticIndex));
  }

  return ret;
}

void FFPProcess(const FFPState &state, const FFPInputs &inputs, const FFPOutputLayout &layout,
                const FFPVertex &vertex, FFPOutput &out)
{
  out = FFPOutput();

  const Vec4 vertexDiffuse = inputs.diffuse ? vertex.diffuse : Vec4(1.0f, 1.0f, 1.0f, 1.0f);
  const Vec4 vertexSpecular = inputs.specular ? vertex.specular : Vec4();

  const float pointSize = inputs.psize ? vertex.psize : state.pointSize;

  if(layout.pretransformed)
  {
    out.position = vertex.position;
    out.diffuse = vertexDiffuse;
    out.specular = vertexSpecular;
    for(uint32_t i = 0; i < MaxTextureStages; i++)
    {
      const uint32_t idx = state.texCoordIndex[i] & 0xFFFF;
      if(layout.texcoordDims[i] && idx < MaxTextureStages)
        out.texcoords[i] = vertex.texcoords[idx];
    }
    out.fog = vertexSpecular[3];
    out.psize = RDCCLAMP(pointSize, state.pointSizeMin, state.pointSizeMax);
    return;
  }

  //////////////////////////////////////////////////////////////////////////
  // blending

  Vec4 pos = vertex.position;
  Vec4 nrm = vertex.normal;
  if(state.vertexBlend == VertexBlendTweening)
  {
    if(inputs.position1)
      pos = Lerp(vertex.position, vertex.position1, state.tweenFactor);
    if(inputs.normal1)
      nrm = Lerp(vertex.normal, vertex.normal1, state.tweenFactor);
  }

  uint32_t numMatrices = 1;
  float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  if(state.vertexBlend >= 1 && state.vertexBlend <= 3)
  {
    numMatrices = state.vertexBlend + 1;
    float sum = 0.0f;
    for(uint32_t i = 0; i < state.vertexBlend; i++)
    {
      weights[i] = i < inputs.blendWeights ? vertex.blendWeight[i] : 0.0f;
      sum += weights[i];
    }
    weights[state.vertexBlend] = 1.0f - sum;
  }

  const bool blending = state.vertexBlend == VertexBlend0Weights ||
                        (state.vertexBlend >= 1 && state.vertexBlend <= 3);

  float viewPos[4] = {}, viewNrm[3] = {};
  for(uint32_t i = 0; i < numMatrices; i++)
  {
    uint32_t idx = i;
    if(blending && state.indexedVertexBlend)
    {
      float f = vertex.blendIndices[i];
      if(inputs.blendIndicesNormalized)
        f *= 255.0f;
      f = floorf(f + 0.5f);
      idx = (f >= 0.0f && f < float(NumWorldMatrices)) ? uint32_t(f) : 0;
    }

    const Matrix worldView = Multiply(state.world[idx], state.view);
    const Vec4 p = Transform(pos, worldView);
    float n[3];
    TransformNormal(nrm.v, worldView, n);

    for(int c = 0; c < 4; c++)
      viewPos[c] += weights[i] * p[c];
    for(int c = 0; c < 3; c++)
      viewNrm[c] += weights[i] * n[c];
  }

  out.position = Transform(Vec4(viewPos[0], viewPos[1], viewPos[2], viewPos[3]), state.projection);

  // the camera space position lighting and fog use
  float v[3] = {viewPos[0], viewPos[1], viewPos[2]};
  if(viewPos[3] != 0.0f && viewPos[3] != 1.0f)
  {
    v[0] /= viewPos[3];
    v[1] /= viewPos[3];
    v[2] /= viewPos[3];
  }

  float n[3] = {viewNrm[0], viewNrm[1], viewNrm[2]};
  if(!inputs.normal)
    n[0] = n[1] = n[2] = 0.0f;
  if(state.normalizeNormals)
    Normalize3(n);

  //////////////////////////////////////////////////////////////////////////
  // lighting

  if(state.lighting)
  {
    auto source = [&](ColorSource s, const Vec4 &material) {
      if(state.colorVertex)
      {
        if(s == ColorSource::Color1 && inputs.diffuse)
          return vertex.diffuse;
        if(s == ColorSource::Color2 && inputs.specular)
          return vertex.specular;
      }
      return material;
    };

    const Vec4 cd = source(state.diffuseSource, state.material.diffuse);
    const Vec4 cs = source(state.specularSource, state.material.specular);
    const Vec4 ca = source(state.ambientSource, state.material.ambient);
    const Vec4 ce = source(state.emissiveSource, state.material.emissive);

    float ambient[3] = {state.ambient[0], state.ambient[1], state.ambient[2]};
    float diffuse[3] = {}, specular[3] = {};

    for(size_t l = 0; l < state.lights.size() && l < MaxFFPLights; l++)
    {
      const FFPLight &light = state.lights[l];

      float ldir[3];
      float atten = 1.0f, spot = 1.0f;

      if(light.type == LightType::Directional)
      {
        TransformDir(light.direction, state.view, ldir);
        Normalize3(ldir);
        for(int c = 0; c < 3; c++)
          ldir[c] = -ldir[c];
      }
      else
      {
        Vec4 lp = Transform(Vec4(light.position[0], light.position[1], light.position[2], 1.0f),
                            state.view);
        float d[3] = {lp[0] - v[0], lp[1] - v[1], lp[2] - v[2]};
        const float dist = sqrtf(Dot3(d, d));
        if(dist > light.range)
          continue;
        for(int c = 0; c < 3; c++)
          ldir[c] = dist > 0.0f ? d[c] / dist : 0.0f;

        const float denom =
            light.attenuation[0] + light.attenuation[1] * dist + light.attenuation[2] * dist * dist;
        atten = denom > 0.0f ? 1.0f / denom : 1.0f;

        if(light.type == LightType::Spot)
        {
          float axis[3];
          TransformDir(light.direction, state.view, axis);
          Normalize3(axis);
          // the cosine of the angle between the spot's axis and the light to the vertex
          const float rho = -Dot3(axis, ldir);
          const float cosTheta = cosf(light.theta * 0.5f);
          const float cosPhi = cosf(light.phi * 0.5f);
          if(rho > cosTheta)
            spot = 1.0f;
          else if(rho <= cosPhi)
            spot = 0.0f;
          else
            spot = powf((rho - cosPhi) / (cosTheta - cosPhi), light.falloff);
        }
      }

      const float scale = atten * spot;

      for(int c = 0; c < 3; c++)
        ambient[c] += light.ambient[c] * scale;

      const float ndotl = Dot3(n, ldir);
      if(ndotl > 0.0f)
      {
        for(int c = 0; c < 3; c++)
          diffuse[c] += light.diffuse[c] * ndotl * scale;

        if(state.specularEnable)
        {
          float h[3];
          if(state.localViewer)
          {
            float e[3] = {-v[0], -v[1], -v[2]};
            Normalize3(e);
            for(int c = 0; c < 3; c++)
              h[c] = e[c] + ldir[c];
          }
          else
          {
            for(int c = 0; c < 3; c++)
              h[c] = NonLocalViewer[c] + ldir[c];
          }
          Normalize3(h);
          const float ndoth = Dot3(n, h);
          if(ndoth > 0.0f)
          {
            const float p = powf(ndoth, state.material.power);
            for(int c = 0; c < 3; c++)
              specular[c] += light.specular[c] * p * scale;
          }
        }
      }
    }

    for(int c = 0; c < 3; c++)
    {
      out.diffuse[c] = Saturate(ce[c] + ca[c] * ambient[c] + cd[c] * diffuse[c]);
      out.specular[c] = Saturate(cs[c] * specular[c]);
    }
    out.diffuse[3] = Saturate(cd[3]);
    out.specular[3] = Saturate(cs[3]);
  }
  else
  {
    out.diffuse = vertexDiffuse;
    out.specular = vertexSpecular;
  }

  //////////////////////////////////////////////////////////////////////////
  // texture coordinates

  for(uint32_t i = 0; i < MaxTextureStages; i++)
  {
    if(layout.texcoordDims[i] == 0)
      continue;

    const uint32_t idx = state.texCoordIndex[i] & 0xFFFF;
    const uint32_t gen = state.texCoordIndex[i] & 0xFFFF0000;

    Vec4 tc;
    uint32_t dims = 3;
    switch(gen)
    {
      case TexGenCameraSpaceNormal: tc = Vec4(n[0], n[1], n[2], 1.0f); break;
      case TexGenCameraSpacePosition: tc = Vec4(v[0], v[1], v[2], 1.0f); break;
      case TexGenCameraSpaceReflection:
      {
        float r[3];
        Reflection(state, v, n, r);
        tc = Vec4(r[0], r[1], r[2], 1.0f);
        break;
      }
      case TexGenSphereMap:
      {
        float r[3];
        Reflection(state, v, n, r);
        float m = 2.0f * sqrtf(r[0] * r[0] + r[1] * r[1] + (r[2] + 1.0f) * (r[2] + 1.0f));
        if(m == 0.0f)
          m = 1.0f;
        tc = Vec4(r[0] / m + 0.5f, r[1] / m + 0.5f, 0.0f, 1.0f);
        dims = 2;
        break;
      }
      default:
        if(idx < MaxTextureStages)
        {
          tc = vertex.texcoords[idx];
          dims = inputs.texcoordDims[idx];
        }
        break;
    }

    const uint32_t count = state.textureTransformFlags[i] & 0xFF;
    if(count >= 1 && count <= 4)
    {
      // the coordinate gets a 1 after its last component, the rest 0
      Vec4 padded;
      for(uint32_t c = 0; c < 4; c++)
        padded[c] = c < dims ? tc[c] : (c == dims ? 1.0f : 0.0f);
      tc = Transform(padded, state.texture[i]);
    }

    out.texcoords[i] = tc;
  }

  //////////////////////////////////////////////////////////////////////////
  // fog

  if(layout.fog)
  {
    if(state.fogVertexMode == FogMode::None)
    {
      out.fog = out.specular[3];
    }
    else
    {
      const float d = state.rangeFog ? sqrtf(Dot3(v, v)) : fabsf(v[2]);
      float f = 1.0f;
      switch(state.fogVertexMode)
      {
        case FogMode::Linear:
          f = state.fogEnd != state.fogStart ? (state.fogEnd - d) / (state.fogEnd - state.fogStart)
                                             : (d < state.fogEnd ? 1.0f : 0.0f);
          break;
        case FogMode::Exp: f = expf(-d * state.fogDensity); break;
        case FogMode::Exp2:
        {
          const float e = d * state.fogDensity;
          f = expf(-e * e);
          break;
        }
        default: break;
      }
      out.fog = Saturate(f);
    }
  }

  //////////////////////////////////////////////////////////////////////////
  // point size

  if(layout.psize)
  {
    float size = pointSize;
    if(state.pointScaleEnable)
    {
      const float de = sqrtf(Dot3(v, v));
      const float denom =
          state.pointScale[0] + state.pointScale[1] * de + state.pointScale[2] * de * de;
      size = denom > 0.0f ? state.viewportHeight * pointSize * sqrtf(1.0f / denom) : 0.0f;
    }
    out.psize = RDCCLAMP(size, state.pointSizeMin, state.pointSizeMax);
  }
}

void FFPOutputValues(const FFPOutputLayout &layout, const FFPOutput &out, const SigParameter &sig,
                     float *dst)
{
  Vec4 val;
  if(sig.semanticName == "POSITION" || sig.semanticName == "POSITIONT")
    val = out.position;
  else if(sig.semanticName == "COLOR")
    val = sig.semanticIndex == 0 ? out.diffuse : out.specular;
  else if(sig.semanticName == "TEXCOORD" && sig.semanticIndex < MaxTextureStages)
    val = out.texcoords[sig.semanticIndex];
  else if(sig.semanticName == "FOG")
    val[0] = out.fog;
  else if(sig.semanticName == "PSIZE")
    val[0] = out.psize;

  for(uint32_t c = 0; c < sig.compCount && c < 4; c++)
    dst[c] = val[c];
}

};    // namespace D3D9Emu

#if ENABLED(ENABLE_UNIT_TESTS)

#include "catch/catch.hpp"

using namespace D3D9Emu;

namespace
{
Matrix Translate(float x, float y, float z)
{
  Matrix m;
  m.m[12] = x;
  m.m[13] = y;
  m.m[14] = z;
  return m;
}

Matrix Scale(float x, float y, float z)
{
  Matrix m;
  m.m[0] = x;
  m.m[5] = y;
  m.m[10] = z;
  return m;
}

void CheckFFPVec(const Vec4 &v, float x, float y, float z, float w, float eps = 1e-5f)
{
  INFO(StringFormat::Fmt("(%f, %f, %f, %f) vs (%f, %f, %f, %f)", v[0], v[1], v[2], v[3], x, y, z, w)
           .c_str());
  CHECK(fabsf(v[0] - x) <= eps * RDCMAX(1.0f, fabsf(x)));
  CHECK(fabsf(v[1] - y) <= eps * RDCMAX(1.0f, fabsf(y)));
  CHECK(fabsf(v[2] - z) <= eps * RDCMAX(1.0f, fabsf(z)));
  CHECK(fabsf(v[3] - w) <= eps * RDCMAX(1.0f, fabsf(w)));
}

struct FFPRun
{
  FFPState state;
  FFPInputs inputs;
  FFPVertex vertex;
  FFPOutputLayout layout;
  FFPOutput out;

  FFPRun()
  {
    state.lighting = false;
    inputs.position = true;
  }

  const FFPOutput &Run()
  {
    layout = FFPLayout(state, inputs);
    FFPProcess(state, inputs, layout, vertex, out);
    return out;
  }
};
};    // namespace

TEST_CASE("D3D9 fixed function vertex processing - transforms", "[d3d9][ffp]")
{
  FFPRun r;

  SECTION("world, view and projection")
  {
    r.state.world[0] = Translate(1.0f, 2.0f, 3.0f);
    r.state.view = Scale(2.0f, 2.0f, 1.0f);
    // a perspective projection: w = z
    Matrix proj;
    proj.m[10] = 1.0f;
    proj.m[11] = 1.0f;
    proj.m[14] = -0.5f;
    proj.m[15] = 0.0f;
    r.state.projection = proj;
    r.vertex.position = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
    // world (2, 3, 4), view (4, 6, 4), clip (4, 6, 3.5, 4)
    CheckFFPVec(r.Run().position, 4.0f, 6.0f, 3.5f, 4.0f);
    CHECK_FALSE(r.layout.pretransformed);
  }

  SECTION("pretransformed vertices pass through")
  {
    r.inputs.position = false;
    r.inputs.positionT = true;
    r.inputs.diffuse = true;
    r.inputs.texcoordDims[0] = 2;
    r.state.lighting = true;
    r.state.world[0] = Translate(1.0f, 2.0f, 3.0f);
    r.state.textureTransformFlags[0] = 2;
    r.state.texture[0] = Scale(5.0f, 5.0f, 5.0f);
    r.vertex.position = Vec4(100.0f, 200.0f, 0.5f, 0.25f);
    r.vertex.diffuse = Vec4(0.1f, 0.2f, 0.3f, 0.4f);
    r.vertex.texcoords[0] = Vec4(0.5f, 0.75f, 0.0f, 1.0f);
    r.Run();
    CHECK(r.layout.pretransformed);
    CheckFFPVec(r.out.position, 100.0f, 200.0f, 0.5f, 0.25f);
    CheckFFPVec(r.out.diffuse, 0.1f, 0.2f, 0.3f, 0.4f);
    CheckFFPVec(r.out.specular, 0.0f, 0.0f, 0.0f, 0.0f);
    CheckFFPVec(r.out.texcoords[0], 0.5f, 0.75f, 0.0f, 1.0f);
    CHECK(r.layout.texcoordDims[0] == 2);

    rdcarray<SigParameter> sig = FFPSignature(r.layout);
    REQUIRE(sig.size() == 4);
    CHECK(sig[0].semanticName == "POSITIONT");
    CHECK(sig[0].systemValue == ShaderBuiltin::Position);
  }

  SECTION("vertex blending")
  {
    r.state.world[0] = Translate(10.0f, 0.0f, 0.0f);
    r.state.world[1] = Translate(-10.0f, 0.0f, 0.0f);
    r.state.world[2] = Translate(0.0f, 100.0f, 0.0f);
    r.state.vertexBlend = 1;
    r.inputs.blendWeights = 1;
    r.vertex.position = Vec4(1.0f, 2.0f, 3.0f, 1.0f);
    r.vertex.blendWeight = Vec4(0.25f, 0.0f, 0.0f, 1.0f);
    // 0.25 * (p + 10) + 0.75 * (p - 10)
    CheckFFPVec(r.Run().position, -4.0f, 2.0f, 3.0f, 1.0f);

    // indexed: matrix 2 with weight 0.25, matrix 0 with the remaining 0.75
    r.state.indexedVertexBlend = true;
    r.inputs.blendIndices = true;
    r.vertex.blendIndices = Vec4(2.0f, 0.0f, 0.0f, 0.0f);
    CheckFFPVec(r.Run().position, 8.5f, 27.0f, 3.0f, 1.0f);

    // indices in a D3DCOLOR are scaled back to integers
    r.inputs.blendIndicesNormalized = true;
    r.vertex.blendIndices = Vec4(2.0f / 255.0f, 0.0f, 0.0f, 0.0f);
    CheckFFPVec(r.Run().position, 8.5f, 27.0f, 3.0f, 1.0f);

    // 0 weights: one matrix, by index
    r.state.vertexBlend = VertexBlend0Weights;
    CheckFFPVec(r.Run().position, 1.0f, 102.0f, 3.0f, 1.0f);

    // tweening between the two positions, with world 0
    r.state.vertexBlend = VertexBlendTweening;
    r.state.tweenFactor = 0.25f;
    r.inputs.position1 = true;
    r.vertex.position1 = Vec4(5.0f, 6.0f, 7.0f, 1.0f);
    CheckFFPVec(r.Run().position, 12.0f, 3.0f, 4.0f, 1.0f);
  }

  SECTION("normals use the inverse transpose")
  {
    r.inputs.normal = true;
    r.state.world[0] = Scale(2.0f, 1.0f, 1.0f);
    r.state.texCoordIndex[0] = TexGenCameraSpaceNormal;
    r.vertex.normal = Vec4(1.0f, 1.0f, 0.0f, 0.0f);
    CheckFFPVec(r.Run().texcoords[0], 0.5f, 1.0f, 0.0f, 1.0f);
    r.state.normalizeNormals = true;
    const float l = sqrtf(1.25f);
    CheckFFPVec(r.Run().texcoords[0], 0.5f / l, 1.0f / l, 0.0f, 1.0f);
  }
}

TEST_CASE("D3D9 fixed function vertex processing - lighting", "[d3d9][ffp]")
{
  FFPRun r;
  r.state.lighting = true;
  r.state.colorVertex = false;
  r.inputs.normal = true;
  r.state.material.diffuse = Vec4(0.5f, 0.5f, 0.5f, 0.25f);
  r.state.material.ambient = Vec4(0.2f, 0.2f, 0.2f, 1.0f);
  r.state.material.emissive = Vec4(0.0f, 0.0f, 0.1f, 1.0f);
  r.state.material.specular = Vec4(1.0f, 1.0f, 1.0f, 0.5f);
  r.state.material.power = 4.0f;
  r.state.ambient = Vec4(0.5f, 0.0f, 0.0f, 0.0f);
  // facing the camera
  r.vertex.position = Vec4(0.0f, 0.0f, 2.0f, 1.0f);
  r.vertex.normal = Vec4(0.0f, 0.0f, -1.0f, 0.0f);

  FFPLight light;
  light.diffuse = Vec4(1.0f, 0.5f, 0.0f, 1.0f);
  light.ambient = Vec4(0.0f, 0.5f, 0.0f, 0.0f);
  light.specular = Vec4(0.0f, 0.0f, 1.0f, 0.0f);

  SECTION("no lights: ambient and emissive")
  {
    CheckFFPVec(r.Run().diffuse, 0.1f, 0.0f, 0.1f, 0.25f);
    CheckFFPVec(r.out.specular, 0.0f, 0.0f, 0.0f, 0.5f);
  }

  SECTION("directional")
  {
    light.type = LightType::Directional;
    light.direction[2] = 1.0f;
    r.state.lights.push_back(light);
    // emissive + ambient * (global + light) + diffuse * light * N.L
    CheckFFPVec(r.Run().diffuse, 0.1f + 0.5f, 0.1f + 0.25f, 0.1f, 0.25f);

    // facing away
    r.vertex.normal = Vec4(0.0f, 0.0f, 1.0f, 0.0f);
    CheckFFPVec(r.Run().diffuse, 0.1f, 0.1f, 0.1f, 0.25f);

    // at 60 degrees
    r.vertex.normal = Vec4(sinf(1.0471976f), 0.0f, -0.5f, 0.0f);
    CheckFFPVec(r.Run().diffuse, 0.1f + 0.25f, 0.1f + 0.125f, 0.1f, 0.25f);

    // the view matrix rotates the light's direction too: turn everything 90 degrees about y
    Matrix rot;
    rot.m[0] = 0.0f;
    rot.m[2] = -1.0f;
    rot.m[8] = 1.0f;
    rot.m[10] = 0.0f;
    r.state.view = rot;
    r.vertex.normal = Vec4(0.0f, 0.0f, -1.0f, 0.0f);
    CheckFFPVec(r.Run().diffuse, 0.1f + 0.5f, 0.1f + 0.25f, 0.1f, 0.25f);
  }

  SECTION("point light attenuation and range")
  {
    light.type = LightType::Point;
    light.range = 10.0f;
    light.attenuation[0] = 1.0f;
    light.attenuation[2] = 0.25f;
    r.state.lights.push_back(light);
    // distance 2: 1 / (1 + 0.25 * 4) = 0.5
    CheckFFPVec(r.Run().diffuse, 0.1f + 0.25f, 0.05f + 0.125f, 0.1f, 0.25f);
    r.state.lights[0].range = 1.0f;
    CheckFFPVec(r.Run().diffuse, 0.1f, 0.0f, 0.1f, 0.25f);
  }

  SECTION("spot light cone")
  {
    light.type = LightType::Spot;
    light.range = 100.0f;
    light.attenuation[0] = 1.0f;
    light.direction[2] = 1.0f;
    light.theta = 0.5f;
    light.phi = 1.0f;
    light.falloff = 2.0f;
    light.ambient = Vec4();
    r.state.ambient = Vec4();
    r.state.material.emissive = Vec4();
    r.state.lights.push_back(light);

    // on the axis, inside theta
    CheckFFPVec(r.Run().diffuse, 0.5f, 0.25f, 0.0f, 0.25f);

    // 0.4 radians off the axis, between theta/2 and phi/2, with the normal facing the light
    const float a = 0.4f;
    r.vertex.position = Vec4(2.0f * sinf(a), 0.0f, 2.0f * cosf(a), 1.0f);
    r.vertex.normal = Vec4(-sinf(a), 0.0f, -cosf(a), 0.0f);
    const float spot = powf((cosf(a) - cosf(0.5f)) / (cosf(0.25f) - cosf(0.5f)), 2.0f);
    CheckFFPVec(r.Run().diffuse, 0.5f * spot, 0.25f * spot, 0.0f, 0.25f);

    // outside phi/2
    const float b = 0.6f;
    r.vertex.position = Vec4(2.0f * sinf(b), 0.0f, 2.0f * cosf(b), 1.0f);
    r.vertex.normal = Vec4(-sinf(b), 0.0f, -cosf(b), 0.0f);
    CheckFFPVec(r.Run().diffuse, 0.0f, 0.0f, 0.0f, 0.25f);
  }

  SECTION("specular")
  {
    light.type = LightType::Directional;
    light.direction[2] = 1.0f;
    r.state.lights.push_back(light);

    // off without D3DRS_SPECULARENABLE
    CheckFFPVec(r.Run().specular, 0.0f, 0.0f, 0.0f, 0.5f);

    r.state.specularEnable = true;
    r.state.localViewer = false;
    // the normal is 0.3 radians from both the light and the viewer: N.H = cos(0.3)
    const float a = 0.3f;
    r.vertex.normal = Vec4(sinf(a), 0.0f, -cosf(a), 0.0f);
    CheckFFPVec(r.Run().specular, 0.0f, 0.0f, powf(cosf(a), 4.0f), 0.5f);

    // the local viewer, from a vertex off to the side: H halves the light and the eye vectors
    r.state.localViewer = true;
    r.vertex.position = Vec4(2.0f, 0.0f, 2.0f, 1.0f);
    r.vertex.normal = Vec4(0.0f, 0.0f, -1.0f, 0.0f);
    float h[3] = {-0.70710678f, 0.0f, -0.70710678f - 1.0f};
    float hl = sqrtf(h[0] * h[0] + h[2] * h[2]);
    CheckFFPVec(r.Run().specular, 0.0f, 0.0f, powf(-h[2] / hl, 4.0f), 0.5f);
  }

  SECTION("material sources")
  {
    r.inputs.diffuse = true;
    r.inputs.specular = true;
    r.vertex.diffuse = Vec4(0.0f, 1.0f, 0.0f, 0.75f);
    r.vertex.specular = Vec4(0.0f, 0.0f, 1.0f, 1.0f);
    r.state.ambient = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
    r.state.material.emissive = Vec4();

    // without D3DRS_COLORVERTEX the material is used
    CheckFFPVec(r.Run().diffuse, 0.2f, 0.2f, 0.2f, 0.25f);

    // ambient from the vertex's specular, diffuse from its diffuse
    r.state.colorVertex = true;
    r.state.ambientSource = ColorSource::Color2;
    CheckFFPVec(r.Run().diffuse, 0.0f, 0.0f, 1.0f, 0.75f);

    // a source the vertex doesn't have falls back to the material
    r.inputs.specular = false;
    CheckFFPVec(r.Run().diffuse, 0.2f, 0.2f, 0.2f, 0.75f);

    // lighting off: the vertex's colours as they are
    r.state.lighting = false;
    r.inputs.specular = true;
    CheckFFPVec(r.Run().diffuse, 0.0f, 1.0f, 0.0f, 0.75f);
    CheckFFPVec(r.out.specular, 0.0f, 0.0f, 1.0f, 1.0f);
  }
}

TEST_CASE("D3D9 fixed function vertex processing - texture coordinates", "[d3d9][ffp]")
{
  FFPRun r;
  r.inputs.normal = true;
  r.inputs.texcoordDims[0] = 2;
  r.inputs.texcoordDims[1] = 3;
  r.vertex.position = Vec4(1.0f, 2.0f, 3.0f, 1.0f);
  r.vertex.texcoords[0] = Vec4(0.25f, 0.5f, 0.0f, 1.0f);
  r.vertex.texcoords[1] = Vec4(1.0f, 2.0f, 3.0f, 1.0f);

  SECTION("passthrough and the layout")
  {
    r.Run();
    CHECK(r.layout.texcoordDims[0] == 2);
    CHECK(r.layout.texcoordDims[1] == 3);
    CHECK(r.layout.texcoordDims[2] == 0);
    CheckFFPVec(r.out.texcoords[0], 0.25f, 0.5f, 0.0f, 1.0f);

    // a stage can read another set
    r.state.texCoordIndex[2] = 0;
    r.Run();
    CHECK(r.layout.texcoordDims[2] == 2);
    CheckFFPVec(r.out.texcoords[2], 0.25f, 0.5f, 0.0f, 1.0f);
  }

  SECTION("generated coordinates")
  {
    r.state.view = Translate(0.0f, 0.0f, 1.0f);
    r.state.texCoordIndex[0] = TexGenCameraSpacePosition;
    r.Run();
    CHECK(r.layout.texcoordDims[0] == 3);
    CheckFFPVec(r.out.texcoords[0], 1.0f, 2.0f, 4.0f, 1.0f);

    // the reflection of the view direction (0, 0, 1) about the normal, without the local viewer
    r.state.localViewer = false;
    r.state.texCoordIndex[0] = TexGenCameraSpaceReflection;
    r.vertex.normal = Vec4(0.70710678f, 0.0f, -0.70710678f, 0.0f);
    CheckFFPVec(r.Run().texcoords[0], 1.0f, 0.0f, 0.0f, 1.0f);

    // sphere map: r / (2 * |r + (0, 0, 1)|) + 0.5
    r.state.texCoordIndex[0] = TexGenSphereMap;
    r.Run();
    CHECK(r.layout.texcoordDims[0] == 2);
    CheckFFPVec(r.out.texcoords[0], 1.0f / (2.0f * sqrtf(2.0f)) + 0.5f, 0.5f, 0.0f, 1.0f);

    // the local viewer reflects the direction to the vertex instead
    r.state.localViewer = true;
    r.state.view = Matrix();
    r.vertex.position = Vec4(0.0f, 0.0f, 5.0f, 1.0f);
    r.state.texCoordIndex[0] = TexGenCameraSpaceReflection;
    CheckFFPVec(r.Run().texcoords[0], 1.0f, 0.0f, 0.0f, 1.0f);
  }

  SECTION("texture transforms")
  {
    // a 2D coordinate is (u, v, 1, 0), so the third row translates it
    Matrix m = Scale(2.0f, 2.0f, 1.0f);
    m.m[8] = 0.1f;
    m.m[9] = 0.2f;
    r.state.texture[0] = m;
    r.state.textureTransformFlags[0] = 2;
    r.Run();
    CHECK(r.layout.texcoordDims[0] == 2);
    CheckFFPVec(r.out.texcoords[0], 0.6f, 1.2f, 1.0f, 0.0f);

    // projected: the vertex's output doesn't change, the last component is the divisor
    r.state.textureTransformFlags[0] = 3 | TextureTransformProjected;
    r.Run();
    CHECK(r.layout.texcoordDims[0] == 3);
    CheckFFPVec(r.out.texcoords[0], 0.6f, 1.2f, 1.0f, 0.0f);

    // a 3D coordinate is (u, v, w, 1)
    r.state.texture[1] = Translate(10.0f, 20.0f, 30.0f);
    r.state.textureTransformFlags[1] = 4;
    r.Run();
    CHECK(r.layout.texcoordDims[1] == 4);
    CheckFFPVec(r.out.texcoords[1], 11.0f, 22.0f, 33.0f, 1.0f);
  }
}

TEST_CASE("D3D9 fixed function vertex processing - fog and point size", "[d3d9][ffp]")
{
  FFPRun r;
  r.vertex.position = Vec4(3.0f, 0.0f, 4.0f, 1.0f);

  SECTION("fog")
  {
    r.Run();
    CHECK_FALSE(r.layout.fog);

    r.state.fogEnable = true;
    r.state.fogVertexMode = FogMode::Linear;
    r.state.fogStart = 2.0f;
    r.state.fogEnd = 6.0f;
    r.Run();
    CHECK(r.layout.fog);
    // camera space z
    CHECK(fabsf(r.out.fog - 0.5f) < 1e-6f);
    // the distance
    r.state.rangeFog = true;
    CHECK(fabsf(r.Run().fog - 0.25f) < 1e-6f);
    r.state.rangeFog = false;

    r.state.fogVertexMode = FogMode::Exp;
    r.state.fogDensity = 0.25f;
    CHECK(fabsf(r.Run().fog - expf(-1.0f)) < 1e-6f);
    r.state.fogVertexMode = FogMode::Exp2;
    CHECK(fabsf(r.Run().fog - expf(-1.0f)) < 1e-6f);

    // without a vertex fog mode it's the specular alpha
    r.state.fogVertexMode = FogMode::None;
    r.inputs.specular = true;
    r.vertex.specular = Vec4(0.0f, 0.0f, 0.0f, 0.375f);
    CHECK(r.Run().fog == 0.375f);

    // table fog has no vertex output
    r.state.fogTableMode = FogMode::Linear;
    r.Run();
    CHECK_FALSE(r.layout.fog);
  }

  SECTION("point size")
  {
    r.Run();
    CHECK_FALSE(r.layout.psize);
    r.state.points = true;
    r.state.pointSize = 4.0f;
    r.Run();
    CHECK(r.layout.psize);
    CHECK(r.out.psize == 4.0f);

    // the vertex's size, clamped
    r.inputs.psize = true;
    r.vertex.psize = 100.0f;
    CHECK(r.Run().psize == 64.0f);

    // scaled: H * size * sqrt(1 / (A + B * d + C * d^2)), d = 5
    r.vertex.psize = 2.0f;
    r.state.pointScaleEnable = true;
    r.state.pointScale[0] = 0.0f;
    r.state.pointScale[1] = 0.0f;
    r.state.pointScale[2] = 1.0f;
    r.state.viewportHeight = 100.0f;
    CHECK(fabsf(r.Run().psize - 40.0f) < 1e-4f);
  }
}

TEST_CASE("D3D9 fixed function output signature and draws", "[d3d9][ffp]")
{
  FFPOutputLayout layout;
  layout.texcoordDims[0] = 2;
  layout.texcoordDims[3] = 3;
  layout.fog = true;
  layout.psize = true;
  rdcarray<SigParameter> sig = FFPSignature(layout);
  REQUIRE(sig.size() == 7);
  CHECK(sig[0].semanticIdxName == "POSITION");
  CHECK(sig[0].systemValue == ShaderBuiltin::Position);
  CHECK(sig[0].compCount == 4);
  CHECK(sig[1].semanticIdxName == "COLOR0");
  CHECK(sig[2].semanticIdxName == "COLOR1");
  CHECK(sig[3].semanticIdxName == "TEXCOORD0");
  CHECK(sig[3].compCount == 2);
  CHECK(sig[4].semanticIdxName == "TEXCOORD3");
  CHECK(sig[4].compCount == 3);
  CHECK(sig[5].semanticIdxName == "FOG");
  CHECK(sig[5].compCount == 1);
  CHECK(sig[6].semanticIdxName == "PSIZE");
  CHECK(sig[6].systemValue == ShaderBuiltin::PointSize);
  for(size_t i = 0; i < sig.size(); i++)
    CHECK(sig[i].regIndex == i);

  // a draw through RunPostVS, from an FVF-like XYZ | DIFFUSE | TEX1 layout
  FFPState state;
  state.lighting = false;
  state.world[0] = Translate(0.0f, 0.0f, 1.0f);

  DrawInputs draw;
  InputElement pos;
  pos.type = DeclType::Float3;
  InputElement col;
  col.offset = 12;
  col.type = DeclType::D3DColor;
  col.usage = D3D9BC::DeclUsage::Color;
  InputElement tex;
  tex.offset = 16;
  tex.type = DeclType::Float2;
  tex.usage = D3D9BC::DeclUsage::TexCoord;
  draw.elements = {pos, col, tex};
  draw.streams.resize(1);
  draw.streams[0].stride = 24;
  for(uint32_t i = 0; i < 3; i++)
  {
    float p[3] = {float(i), 0.0f, 0.0f};
    uint32_t c = 0xFF0000FF;
    float t[2] = {0.5f, float(i)};
    draw.streams[0].data.append((const byte *)p, sizeof(p));
    draw.streams[0].data.append((const byte *)&c, sizeof(c));
    draw.streams[0].data.append((const byte *)t, sizeof(t));
  }
  draw.numVertices = 3;

  FFPInputs inputs;
  inputs.FromElements(draw.elements);
  CHECK(inputs.position);
  CHECK(inputs.diffuse);
  CHECK(inputs.texcoordDims[0] == 2);

  rdcarray<SigParameter> drawSig = FFPSignature(FFPLayout(state, inputs));
  REQUIRE(drawSig.size() == 4);

  PostVSResult res;
  RunPostVS(draw, 0, NULL, VSConstants(), NULL, &state, drawSig, res);
  INFO(res.status.c_str());
  REQUIRE(res.status.empty());
  CHECK(res.clipSpace);
  CHECK(res.stride == (4 + 4 + 4 + 2) * 4);
  const float *f = (const float *)res.vertexData.data() + res.stride / 4 * 2;
  CheckFFPVec(Vec4(f[0], f[1], f[2], f[3]), 2.0f, 0.0f, 1.0f, 1.0f);
  CheckFFPVec(Vec4(f[4], f[5], f[6], f[7]), 0.0f, 0.0f, 1.0f, 1.0f);
  CheckFFPVec(Vec4(f[8], f[9], f[10], f[11]), 0.0f, 0.0f, 0.0f, 0.0f);
  CHECK(f[12] == 0.5f);
  CHECK(f[13] == 2.0f);
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)
