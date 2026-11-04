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

// Fixed function vertex processing on the CPU, for the mesh viewer's post-transform data
// (phase 3e), following the formulas in the D3D9 documentation ("Mathematics of Lighting",
// "Geometry Blending", "Texture Coordinate Processing", "Vertex Fog", "Point Sprites"). No
// Windows headers: the replay fills FFPState from the device (d3d9_postvs.cpp).

#include "d3d9_vs_emulate.h"

namespace D3D9Emu
{
// a D3DMATRIX: m[row * 4 + column], row vectors (v' = v * M), translation in the last row
struct Matrix
{
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

Vec4 Transform(const Vec4 &v, const Matrix &m);
Matrix Multiply(const Matrix &a, const Matrix &b);

// D3DLIGHTTYPE
enum class LightType : uint32_t
{
  Point = 1,
  Spot = 2,
  Directional = 3,
};

struct FFPLight
{
  LightType type = LightType::Directional;
  Vec4 diffuse, specular, ambient;
  float position[3] = {};
  float direction[3] = {0.0f, 0.0f, 1.0f};
  float range = 0.0f;
  float falloff = 0.0f;
  float attenuation[3] = {};
  float theta = 0.0f;
  float phi = 0.0f;
};

struct FFPMaterial
{
  Vec4 diffuse, ambient, specular, emissive;
  float power = 0.0f;
};

// D3DMATERIALCOLORSOURCE
enum class ColorSource : uint32_t
{
  Material = 0,
  Color1 = 1,
  Color2 = 2,
};

// D3DFOGMODE
enum class FogMode : uint32_t
{
  None = 0,
  Exp = 1,
  Exp2 = 2,
  Linear = 3,
};

// D3DVERTEXBLENDFLAGS
enum : uint32_t
{
  VertexBlendDisable = 0,
  VertexBlendTweening = 255,
  VertexBlend0Weights = 256,
};

// D3DTSS_TCI_* and D3DTTFF_PROJECTED
enum : uint32_t
{
  TexGenPassthru = 0x00000,
  TexGenCameraSpaceNormal = 0x10000,
  TexGenCameraSpacePosition = 0x20000,
  TexGenCameraSpaceReflection = 0x30000,
  TexGenSphereMap = 0x40000,
  TextureTransformProjected = 256,
};

enum
{
  MaxFFPLights = 8,
  MaxTextureStages = 8,
  NumWorldMatrices = 256,
};

// the device state fixed function vertex processing uses
struct FFPState
{
  // D3DTS_WORLDMATRIX(0-255), D3DTS_VIEW, D3DTS_PROJECTION, D3DTS_TEXTURE0-7
  Matrix world[NumWorldMatrices];
  Matrix view, projection;
  Matrix texture[MaxTextureStages];

  bool lighting = true;
  bool normalizeNormals = false;
  bool localViewer = true;
  bool colorVertex = true;
  bool specularEnable = false;
  ColorSource diffuseSource = ColorSource::Color1;
  ColorSource specularSource = ColorSource::Color2;
  ColorSource ambientSource = ColorSource::Material;
  ColorSource emissiveSource = ColorSource::Material;
  // D3DRS_AMBIENT
  Vec4 ambient;
  FFPMaterial material;
  // the enabled lights, at most 8
  rdcarray<FFPLight> lights;

  // D3DRS_VERTEXBLEND
  uint32_t vertexBlend = VertexBlendDisable;
  bool indexedVertexBlend = false;
  float tweenFactor = 0.0f;

  bool fogEnable = false;
  FogMode fogVertexMode = FogMode::None;
  FogMode fogTableMode = FogMode::None;
  bool rangeFog = false;
  float fogStart = 0.0f;
  float fogEnd = 1.0f;
  float fogDensity = 1.0f;

  float pointSize = 1.0f;
  float pointSizeMin = 1.0f;
  float pointSizeMax = 64.0f;
  bool pointScaleEnable = false;
  float pointScale[3] = {1.0f, 0.0f, 0.0f};
  // the viewport's height in pixels, for point scaling
  float viewportHeight = 1.0f;
  // whether the primitives are points, which always get a point size
  bool points = false;

  // D3DTSS_TEXCOORDINDEX and D3DTSS_TEXTURETRANSFORMFLAGS per stage
  uint32_t texCoordIndex[MaxTextureStages] = {0, 1, 2, 3, 4, 5, 6, 7};
  uint32_t textureTransformFlags[MaxTextureStages] = {};
  // how many stages output a texture coordinate. With no pixel shader it's the stages before the
  // first one whose D3DTSS_COLOROP is D3DTOP_DISABLE, as ProcessVertices does.
  uint32_t outputStages = MaxTextureStages;
};

// what the vertex declaration gives the fixed function pipeline. It reads the elements with
// usage index 0, POSITION1 and NORMAL1 for tweening, and TEXCOORD0-7.
struct FFPInputs
{
  bool position = false;
  bool positionT = false;
  bool position1 = false;
  bool normal = false;
  bool normal1 = false;
  bool diffuse = false;
  bool specular = false;
  bool psize = false;
  uint32_t blendWeights = 0;
  bool blendIndices = false;
  // blend indices in a D3DCOLOR or normalized type, read back as integers by * 255
  bool blendIndicesNormalized = false;
  // each texture coordinate set's number of components, 0 if it isn't there
  uint32_t texcoordDims[MaxTextureStages] = {};

  // fills these from the elements' usages
  void FromElements(const rdcarray<InputElement> &elements);
};

// one vertex's inputs
struct FFPVertex
{
  Vec4 position = Vec4(0, 0, 0, 1);
  Vec4 position1 = Vec4(0, 0, 0, 1);
  Vec4 normal, normal1;
  Vec4 diffuse = Vec4(1, 1, 1, 1);
  Vec4 specular;
  Vec4 blendWeight, blendIndices;
  float psize = 1.0f;
  Vec4 texcoords[MaxTextureStages];
};

// what the fixed function pipeline outputs for a draw: which outputs there are and their sizes
struct FFPOutputLayout
{
  bool pretransformed = false;
  bool fog = false;
  bool psize = false;
  // each stage's output texture coordinate's number of components, 0 if it has none
  uint32_t texcoordDims[MaxTextureStages] = {};
};

FFPOutputLayout FFPLayout(const FFPState &state, const FFPInputs &inputs);

// the output signature for a layout: POSITION (POSITIONT for pretransformed vertices), COLOR0,
// COLOR1, TEXCOORDn for the stages that have one, FOG, PSIZE. regIndex numbers the elements.
rdcarray<SigParameter> FFPSignature(const FFPOutputLayout &layout);

struct FFPOutput
{
  // clip space, or the pretransformed position as it was given
  Vec4 position;
  Vec4 diffuse, specular;
  Vec4 texcoords[MaxTextureStages];
  float fog = 0.0f;
  float psize = 0.0f;
};

void FFPProcess(const FFPState &state, const FFPInputs &inputs, const FFPOutputLayout &layout,
                const FFPVertex &vertex, FFPOutput &out);

// the values of an output (by regIndex, as FFPSignature numbers them), compCount of them
void FFPOutputValues(const FFPOutputLayout &layout, const FFPOutput &out, const SigParameter &sig,
                     float *dst);

};    // namespace D3D9Emu
