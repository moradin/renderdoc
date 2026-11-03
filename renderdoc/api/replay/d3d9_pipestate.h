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

#include "common_pipestate.h"

// NOTE: Remember that python sees namespaces flattened to a prefix - i.e. D3D9Pipe::Shader is
// renamed to D3D9Shader, so these types must be referenced in the documentation

namespace D3D9Pipe
{
DOCUMENT(R"(
D3D9VertexElement()
D3D9VertexElement(other: D3D9VertexElement)

Describes a single element of a D3D9 vertex declaration. When the vertex declaration came from an
FVF code, the elements are the ones that FVF code describes.
)");
struct VertexElement
{
  DOCUMENT("");
  VertexElement() = default;
  VertexElement(const VertexElement &) = default;
  VertexElement &operator=(const VertexElement &) = default;

  bool operator==(const VertexElement &o) const
  {
    return stream == o.stream && byteOffset == o.byteOffset && format == o.format &&
           declType == o.declType && method == o.method && semanticName == o.semanticName &&
           semanticIndex == o.semanticIndex;
  }
  bool operator<(const VertexElement &o) const
  {
    if(!(stream == o.stream))
      return stream < o.stream;
    if(!(byteOffset == o.byteOffset))
      return byteOffset < o.byteOffset;
    if(!(format == o.format))
      return format < o.format;
    if(!(declType == o.declType))
      return declType < o.declType;
    if(!(method == o.method))
      return method < o.method;
    if(!(semanticName == o.semanticName))
      return semanticName < o.semanticName;
    if(!(semanticIndex == o.semanticIndex))
      return semanticIndex < o.semanticIndex;
    return false;
  }

  DOCUMENT(R"(The stream the element's data is read from.

:type: int
)");
  uint32_t stream = 0;

  DOCUMENT(R"(The byte offset of the element from the start of each vertex in the stream.

:type: int
)");
  uint32_t byteOffset = 0;

  DOCUMENT(R"(The format describing how the element's data is stored.

Integer formats that D3D9 converts to floats without normalising them, such as ``SHORT2``, are
described with their integer format.

:type: ResourceFormat
)");
  ResourceFormat format;

  DOCUMENT(R"(The element's D3DDECLTYPE, without the ``D3DDECLTYPE_`` prefix, e.g. ``FLOAT3``.

:type: str
)");
  rdcstr declType;

  DOCUMENT(R"(The element's D3DDECLMETHOD, without the ``D3DDECLMETHOD_`` prefix, e.g. ``DEFAULT``.

:type: str
)");
  rdcstr method;

  DOCUMENT(R"(The element's D3DDECLUSAGE, without the ``D3DDECLUSAGE_`` prefix, e.g. ``POSITION``.

:type: str
)");
  rdcstr semanticName;

  DOCUMENT(R"(The element's usage index.

:type: int
)");
  uint32_t semanticIndex = 0;
};

DOCUMENT(R"(
D3D9StreamSource()
D3D9StreamSource(other: D3D9StreamSource)

Describes a vertex buffer bound to one of the D3D9 vertex streams.
)");
struct StreamSource
{
  DOCUMENT("");
  StreamSource() = default;
  StreamSource(const StreamSource &) = default;
  StreamSource &operator=(const StreamSource &) = default;

  bool operator==(const StreamSource &o) const
  {
    return resourceId == o.resourceId && byteOffset == o.byteOffset && byteStride == o.byteStride &&
           frequency == o.frequency && indexedData == o.indexedData &&
           instanceData == o.instanceData;
  }
  bool operator<(const StreamSource &o) const
  {
    if(!(resourceId == o.resourceId))
      return resourceId < o.resourceId;
    if(!(byteOffset == o.byteOffset))
      return byteOffset < o.byteOffset;
    if(!(byteStride == o.byteStride))
      return byteStride < o.byteStride;
    if(!(frequency == o.frequency))
      return frequency < o.frequency;
    if(!(indexedData == o.indexedData))
      return indexedData < o.indexedData;
    if(!(instanceData == o.instanceData))
      return instanceData < o.instanceData;
    return false;
  }

  DOCUMENT(R"(The :class:`ResourceId` of the vertex buffer.

:type: ResourceId
)");
  ResourceId resourceId;

  DOCUMENT(R"(The byte offset from the start of the buffer to the first vertex.

:type: int
)");
  uint32_t byteOffset = 0;

  DOCUMENT(R"(The byte stride between vertices.

:type: int
)");
  uint32_t byteStride = 0;

  DOCUMENT(R"(The stream's frequency, as set with ``SetStreamSourceFreq`` without the
:data:`indexedData` and :data:`instanceData` flags. For indexed data it's the number of instances,
for instance data the number of instances that use each element.

:type: int
)");
  uint32_t frequency = 1;

  DOCUMENT(R"(``True`` if the stream holds indexed (per-vertex) data and the draw is instanced
(``D3DSTREAMSOURCE_INDEXEDDATA``).

:type: bool
)");
  bool indexedData = false;

  DOCUMENT(R"(``True`` if the stream holds per-instance data (``D3DSTREAMSOURCE_INSTANCEDATA``).

:type: bool
)");
  bool instanceData = false;
};

DOCUMENT(R"(
D3D9IndexBuffer()
D3D9IndexBuffer(other: D3D9IndexBuffer)

Describes the D3D9 index buffer.
)");
struct IndexBuffer
{
  DOCUMENT("");
  IndexBuffer() = default;
  IndexBuffer(const IndexBuffer &) = default;
  IndexBuffer &operator=(const IndexBuffer &) = default;

  DOCUMENT(R"(The :class:`ResourceId` of the index buffer.

:type: ResourceId
)");
  ResourceId resourceId;

  DOCUMENT(R"(The number of bytes for each index in the index buffer, 2 or 4. D3D9 index buffers
have no offset, draws start at an index instead.

:type: int
)");
  uint32_t byteStride = 0;
};

DOCUMENT(R"(
D3D9InputAssembly()
D3D9InputAssembly(other: D3D9InputAssembly)

Describes the D3D9 vertex input state.
)");
struct InputAssembly
{
  DOCUMENT("");
  InputAssembly() = default;
  InputAssembly(const InputAssembly &) = default;
  InputAssembly &operator=(const InputAssembly &) = default;

  DOCUMENT(R"(The elements of the current vertex declaration.

:type: List[D3D9VertexElement]
)");
  rdcarray<VertexElement> vertexElements;

  DOCUMENT(R"(The :class:`ResourceId` of the vertex declaration, or a null ID when it came from an
FVF code (see :data:`fvf`) or there is none.

:type: ResourceId
)");
  ResourceId declarationId;

  DOCUMENT(R"(The FVF code, when the vertex declaration came from ``SetFVF``, or 0.

:type: int
)");
  uint32_t fvf = 0;

  DOCUMENT(R"(The vertex streams, one for each of the 16 streams.

:type: List[D3D9StreamSource]
)");
  rdcarray<StreamSource> streams;

  DOCUMENT(R"(The index buffer.

:type: D3D9IndexBuffer
)");
  IndexBuffer indexBuffer;

  DOCUMENT(R"(The primitive topology of the current draw, or :data:`Topology.Unknown` when the
current event isn't a draw.

:type: Topology
)");
  Topology topology = Topology::Unknown;
};

DOCUMENT(R"(
D3D9Shader()
D3D9Shader(other: D3D9Shader)

Describes a D3D9 shader stage, and where its constants can be read from.

A D3D9 shader's constants aren't stored in buffers. RenderDoc presents the stage's constant
registers as three read-only buffers holding the whole register file, starting at register 0: one
for the float constants (``c#``, a ``float4`` per register), one for the integer constants
(``i#``, an ``int4`` per register) and one for the boolean constants (``b#``, a ``uint32_t`` per
register holding 0 or 1). They can be read with :meth:`ReplayController.GetBufferData`, and passed
to :meth:`ReplayController.GetCBufferVariableContents` for a constant block of the shader's
reflection that uses that register set.
)");
struct Shader
{
  DOCUMENT("");
  Shader() = default;
  Shader(const Shader &) = default;
  Shader &operator=(const Shader &) = default;

  DOCUMENT(R"(The :class:`ResourceId` of the shader object, or a null ID when the stage uses the
fixed function pipeline.

:type: ResourceId
)");
  ResourceId resourceId;

  DOCUMENT(R"(The reflection data for this shader, if it's available.

:type: ShaderReflection
)");
  const ShaderReflection *reflection = NULL;

  DOCUMENT(R"(A :class:`ShaderStage` identifying which stage this shader is bound to.

:type: ShaderStage
)");
  ShaderStage stage = ShaderStage::Vertex;

  DOCUMENT(R"(The shader's version as a shader model name, e.g. ``vs_3_0``, or an empty string
when the stage uses the fixed function pipeline.

:type: str
)");
  rdcstr shaderModel;

  DOCUMENT(R"(The :class:`ResourceId` of the buffer holding the stage's float constant registers.

:type: ResourceId
)");
  ResourceId floatConstantsBuffer;

  DOCUMENT(R"(The number of float constant registers the device has for this stage.

:type: int
)");
  uint32_t floatConstantCount = 0;

  DOCUMENT(R"(The :class:`ResourceId` of the buffer holding the stage's integer constant
registers.

:type: ResourceId
)");
  ResourceId intConstantsBuffer;

  DOCUMENT(R"(The number of integer constant registers the device has for this stage.

:type: int
)");
  uint32_t intConstantCount = 0;

  DOCUMENT(R"(The :class:`ResourceId` of the buffer holding the stage's boolean constant
registers.

:type: ResourceId
)");
  ResourceId boolConstantsBuffer;

  DOCUMENT(R"(The number of boolean constant registers the device has for this stage.

:type: int
)");
  uint32_t boolConstantCount = 0;
};

DOCUMENT(R"(
D3D9Sampler()
D3D9Sampler(other: D3D9Sampler)

Describes a D3D9 sampler: the texture bound to it and its sampler states.
)");
struct Sampler
{
  DOCUMENT("");
  Sampler() = default;
  Sampler(const Sampler &) = default;
  Sampler &operator=(const Sampler &) = default;

  bool operator==(const Sampler &o) const
  {
    return samplerIndex == o.samplerIndex && resourceId == o.resourceId && addressU == o.addressU &&
           addressV == o.addressV && addressW == o.addressW && borderColor == o.borderColor &&
           magFilter == o.magFilter && minFilter == o.minFilter && mipFilter == o.mipFilter &&
           mipLODBias == o.mipLODBias && maxMipLevel == o.maxMipLevel &&
           maxAnisotropy == o.maxAnisotropy && srgbTexture == o.srgbTexture &&
           elementIndex == o.elementIndex && dmapOffset == o.dmapOffset;
  }
  bool operator<(const Sampler &o) const
  {
    if(!(samplerIndex == o.samplerIndex))
      return samplerIndex < o.samplerIndex;
    if(!(resourceId == o.resourceId))
      return resourceId < o.resourceId;
    return false;
  }

  DOCUMENT(R"(The sampler as the API numbers it: 0 to 15 for the pixel samplers, 256 for the
displacement map sampler (``D3DDMAPSAMPLER``), 257 to 260 for the vertex texture samplers
(``D3DVERTEXTEXTURESAMPLER0`` to ``3``).

:type: int
)");
  uint32_t samplerIndex = 0;

  DOCUMENT(R"(The :class:`ResourceId` of the texture bound to the sampler.

:type: ResourceId
)");
  ResourceId resourceId;

  DOCUMENT(R"(The addressing mode for the U texture coordinate.

:type: AddressMode
)");
  AddressMode addressU = AddressMode::Wrap;

  DOCUMENT(R"(The addressing mode for the V texture coordinate.

:type: AddressMode
)");
  AddressMode addressV = AddressMode::Wrap;

  DOCUMENT(R"(The addressing mode for the W texture coordinate.

:type: AddressMode
)");
  AddressMode addressW = AddressMode::Wrap;

  DOCUMENT(R"(The border color, as RGBA.

:type: FloatVector
)");
  FloatVector borderColor;

  DOCUMENT(R"(The filter used when magnifying.

:type: FilterMode
)");
  FilterMode magFilter = FilterMode::Point;

  DOCUMENT(R"(The filter used when minifying.

:type: FilterMode
)");
  FilterMode minFilter = FilterMode::Point;

  DOCUMENT(R"(The filter used between mips, :data:`FilterMode.NoFilter` if mips aren't used.

:type: FilterMode
)");
  FilterMode mipFilter = FilterMode::NoFilter;

  DOCUMENT(R"(The bias added to the mip level.

:type: float
)");
  float mipLODBias = 0.0f;

  DOCUMENT(R"(The most detailed mip level that can be used (``D3DSAMP_MAXMIPLEVEL``).

:type: int
)");
  uint32_t maxMipLevel = 0;

  DOCUMENT(R"(The maximum anisotropy.

:type: int
)");
  uint32_t maxAnisotropy = 1;

  DOCUMENT(R"(``True`` if the texture is read as sRGB and converted to linear
(``D3DSAMP_SRGBTEXTURE``).

:type: bool
)");
  bool srgbTexture = false;

  DOCUMENT(R"(The element of a multi-element texture to use (``D3DSAMP_ELEMENTINDEX``).

:type: int
)");
  uint32_t elementIndex = 0;

  DOCUMENT(R"(The vertex offset in the displacement map (``D3DSAMP_DMAPOFFSET``).

:type: int
)");
  uint32_t dmapOffset = 0;
};

DOCUMENT(R"(
D3D9TextureStage()
D3D9TextureStage(other: D3D9TextureStage)

Describes one of the D3D9 fixed function texture stages. The operations and arguments are given
by their D3D9 names without the prefix, e.g. ``MODULATE`` or ``TEXTURE``, with any modifiers
after a ``|``.
)");
struct TextureStage
{
  DOCUMENT("");
  TextureStage() = default;
  TextureStage(const TextureStage &) = default;
  TextureStage &operator=(const TextureStage &) = default;

  bool operator==(const TextureStage &o) const
  {
    return stage == o.stage && colorOp == o.colorOp && colorArg0 == o.colorArg0 &&
           colorArg1 == o.colorArg1 && colorArg2 == o.colorArg2 && alphaOp == o.alphaOp &&
           alphaArg0 == o.alphaArg0 && alphaArg1 == o.alphaArg1 && alphaArg2 == o.alphaArg2 &&
           resultArg == o.resultArg && texCoordIndex == o.texCoordIndex &&
           texCoordGeneration == o.texCoordGeneration &&
           textureTransformFlags == o.textureTransformFlags && bumpEnvMatrix == o.bumpEnvMatrix &&
           bumpEnvLScale == o.bumpEnvLScale && bumpEnvLOffset == o.bumpEnvLOffset &&
           constant == o.constant;
  }
  bool operator<(const TextureStage &o) const
  {
    if(!(stage == o.stage))
      return stage < o.stage;
    return false;
  }

  DOCUMENT(R"(The index of the texture stage, 0 to 7.

:type: int
)");
  uint32_t stage = 0;

  DOCUMENT(R"(The color operation (``D3DTSS_COLOROP``).

:type: str
)");
  rdcstr colorOp;

  DOCUMENT(R"(The third color argument, used by some operations (``D3DTSS_COLORARG0``).

:type: str
)");
  rdcstr colorArg0;

  DOCUMENT(R"(The first color argument (``D3DTSS_COLORARG1``).

:type: str
)");
  rdcstr colorArg1;

  DOCUMENT(R"(The second color argument (``D3DTSS_COLORARG2``).

:type: str
)");
  rdcstr colorArg2;

  DOCUMENT(R"(The alpha operation (``D3DTSS_ALPHAOP``).

:type: str
)");
  rdcstr alphaOp;

  DOCUMENT(R"(The third alpha argument, used by some operations (``D3DTSS_ALPHAARG0``).

:type: str
)");
  rdcstr alphaArg0;

  DOCUMENT(R"(The first alpha argument (``D3DTSS_ALPHAARG1``).

:type: str
)");
  rdcstr alphaArg1;

  DOCUMENT(R"(The second alpha argument (``D3DTSS_ALPHAARG2``).

:type: str
)");
  rdcstr alphaArg2;

  DOCUMENT(R"(Where the stage's result goes, ``CURRENT`` or ``TEMP`` (``D3DTSS_RESULTARG``).

:type: str
)");
  rdcstr resultArg;

  DOCUMENT(R"(The texture coordinate set the stage uses (the low bits of ``D3DTSS_TEXCOORDINDEX``).

:type: int
)");
  uint32_t texCoordIndex = 0;

  DOCUMENT(R"(How texture coordinates are generated, ``PASSTHRU`` when they're read from the
vertex (the ``D3DTSS_TCI_`` flag of ``D3DTSS_TEXCOORDINDEX``).

:type: str
)");
  rdcstr texCoordGeneration;

  DOCUMENT(R"(How texture coordinates are transformed (``D3DTSS_TEXTURETRANSFORMFLAGS``).

:type: str
)");
  rdcstr textureTransformFlags;

  DOCUMENT(R"(The bump mapping matrix: 00, 01, 10 and 11 (``D3DTSS_BUMPENVMAT00`` to ``11``).

:type: Tuple[float,float,float,float]
)");
  rdcfixedarray<float, 4> bumpEnvMatrix = {0.0f, 0.0f, 0.0f, 0.0f};

  DOCUMENT(R"(The bump mapping luminance scale (``D3DTSS_BUMPENVLSCALE``).

:type: float
)");
  float bumpEnvLScale = 0.0f;

  DOCUMENT(R"(The bump mapping luminance offset (``D3DTSS_BUMPENVLOFFSET``).

:type: float
)");
  float bumpEnvLOffset = 0.0f;

  DOCUMENT(R"(The stage's constant color, as RGBA (``D3DTSS_CONSTANT``).

:type: FloatVector
)");
  FloatVector constant;
};

DOCUMENT(R"(
D3D9Transform()
D3D9Transform(other: D3D9Transform)

Describes one of the D3D9 fixed function transforms.
)");
struct Transform
{
  DOCUMENT("");
  Transform() = default;
  Transform(const Transform &) = default;
  Transform &operator=(const Transform &) = default;

  bool operator==(const Transform &o) const { return name == o.name && matrix == o.matrix; }
  bool operator<(const Transform &o) const
  {
    if(!(name == o.name))
      return name < o.name;
    return false;
  }

  DOCUMENT(R"(The transform's name, e.g. ``VIEW``, ``PROJECTION``, ``TEXTURE0``, or ``WORLD``,
``WORLDMATRIX(1)``... for the world matrices.

:type: str
)");
  rdcstr name;

  DOCUMENT(R"(The matrix, row by row as D3DMATRIX stores it.

:type: Tuple[float,...]
)");
  rdcfixedarray<float, 16> matrix = {
      1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
  };
};

DOCUMENT(R"(
D3D9Light()
D3D9Light(other: D3D9Light)

Describes one of the D3D9 fixed function lights.
)");
struct Light
{
  DOCUMENT("");
  Light() = default;
  Light(const Light &) = default;
  Light &operator=(const Light &) = default;

  bool operator==(const Light &o) const
  {
    return index == o.index && enabled == o.enabled && type == o.type && diffuse == o.diffuse &&
           specular == o.specular && ambient == o.ambient && position == o.position &&
           direction == o.direction && range == o.range && falloff == o.falloff &&
           attenuation0 == o.attenuation0 && attenuation1 == o.attenuation1 &&
           attenuation2 == o.attenuation2 && theta == o.theta && phi == o.phi;
  }
  bool operator<(const Light &o) const
  {
    if(!(index == o.index))
      return index < o.index;
    return false;
  }

  DOCUMENT(R"(The light's index.

:type: int
)");
  uint32_t index = 0;

  DOCUMENT(R"(``True`` if the light is enabled.

:type: bool
)");
  bool enabled = false;

  DOCUMENT(R"(The light's type: ``POINT``, ``SPOT`` or ``DIRECTIONAL``.

:type: str
)");
  rdcstr type;

  DOCUMENT(R"(The diffuse color, as RGBA.

:type: FloatVector
)");
  FloatVector diffuse;

  DOCUMENT(R"(The specular color, as RGBA.

:type: FloatVector
)");
  FloatVector specular;

  DOCUMENT(R"(The ambient color, as RGBA.

:type: FloatVector
)");
  FloatVector ambient;

  DOCUMENT(R"(The position in world space, with w set to 1.

:type: FloatVector
)");
  FloatVector position;

  DOCUMENT(R"(The direction in world space, with w set to 0.

:type: FloatVector
)");
  FloatVector direction;

  DOCUMENT(R"(The distance beyond which the light has no effect.

:type: float
)");
  float range = 0.0f;

  DOCUMENT(R"(The falloff between a spotlight's inner and outer cones.

:type: float
)");
  float falloff = 0.0f;

  DOCUMENT(R"(The constant attenuation factor.

:type: float
)");
  float attenuation0 = 0.0f;

  DOCUMENT(R"(The linear attenuation factor.

:type: float
)");
  float attenuation1 = 0.0f;

  DOCUMENT(R"(The quadratic attenuation factor.

:type: float
)");
  float attenuation2 = 0.0f;

  DOCUMENT(R"(The angle of a spotlight's inner cone, in radians.

:type: float
)");
  float theta = 0.0f;

  DOCUMENT(R"(The angle of a spotlight's outer cone, in radians.

:type: float
)");
  float phi = 0.0f;
};

DOCUMENT(R"(
D3D9Material()
D3D9Material(other: D3D9Material)

Describes the D3D9 fixed function material.
)");
struct Material
{
  DOCUMENT("");
  Material() = default;
  Material(const Material &) = default;
  Material &operator=(const Material &) = default;

  DOCUMENT(R"(The diffuse color, as RGBA.

:type: FloatVector
)");
  FloatVector diffuse;

  DOCUMENT(R"(The ambient color, as RGBA.

:type: FloatVector
)");
  FloatVector ambient;

  DOCUMENT(R"(The specular color, as RGBA.

:type: FloatVector
)");
  FloatVector specular;

  DOCUMENT(R"(The emissive color, as RGBA.

:type: FloatVector
)");
  FloatVector emissive;

  DOCUMENT(R"(The specular power.

:type: float
)");
  float power = 0.0f;
};

DOCUMENT(R"(
D3D9Fog()
D3D9Fog(other: D3D9Fog)

Describes the D3D9 fixed function fog.
)");
struct Fog
{
  DOCUMENT("");
  Fog() = default;
  Fog(const Fog &) = default;
  Fog &operator=(const Fog &) = default;

  DOCUMENT(R"(``True`` if fog is enabled (``D3DRS_FOGENABLE``).

:type: bool
)");
  bool enabled = false;

  DOCUMENT(R"(The fog color, as RGBA.

:type: FloatVector
)");
  FloatVector color;

  DOCUMENT(R"(The pixel fog mode: ``NONE``, ``EXP``, ``EXP2`` or ``LINEAR``
(``D3DRS_FOGTABLEMODE``).

:type: str
)");
  rdcstr tableMode;

  DOCUMENT(R"(The vertex fog mode: ``NONE``, ``EXP``, ``EXP2`` or ``LINEAR``
(``D3DRS_FOGVERTEXMODE``).

:type: str
)");
  rdcstr vertexMode;

  DOCUMENT(R"(The depth at which linear fog starts.

:type: float
)");
  float start = 0.0f;

  DOCUMENT(R"(The depth at which linear fog ends.

:type: float
)");
  float end = 1.0f;

  DOCUMENT(R"(The density of exponential fog.

:type: float
)");
  float density = 1.0f;

  DOCUMENT(R"(``True`` if vertex fog is based on range rather than depth (``D3DRS_RANGEFOGENABLE``).

:type: bool
)");
  bool rangeFog = false;
};

DOCUMENT(R"(
D3D9ClipPlane()
D3D9ClipPlane(other: D3D9ClipPlane)

Describes one of the D3D9 user clip planes.
)");
struct ClipPlane
{
  DOCUMENT("");
  ClipPlane() = default;
  ClipPlane(const ClipPlane &) = default;
  ClipPlane &operator=(const ClipPlane &) = default;

  bool operator==(const ClipPlane &o) const { return enabled == o.enabled && plane == o.plane; }
  bool operator<(const ClipPlane &o) const
  {
    if(!(enabled == o.enabled))
      return enabled < o.enabled;
    return false;
  }

  DOCUMENT(R"(``True`` if the plane is enabled (in ``D3DRS_CLIPPLANEENABLE``).

:type: bool
)");
  bool enabled = false;

  DOCUMENT(R"(The plane's coefficients. With shaders they're in clip space, with the fixed function
pipeline in world space.

:type: FloatVector
)");
  FloatVector plane;
};

DOCUMENT(R"(
D3D9FixedFunction()
D3D9FixedFunction(other: D3D9FixedFunction)

Describes the D3D9 fixed function state. The vertex processing part is used when no vertex shader
is bound, and the texture stages when no pixel shader is bound. Fog and clip planes apply to both.
)");
struct FixedFunction
{
  DOCUMENT("");
  FixedFunction() = default;
  FixedFunction(const FixedFunction &) = default;
  FixedFunction &operator=(const FixedFunction &) = default;

  DOCUMENT(R"(The 8 texture stages.

:type: List[D3D9TextureStage]
)");
  rdcarray<TextureStage> textureStages;

  DOCUMENT(R"(The view, projection and texture transforms, and the world matrices. World
matrices other than ``WORLD`` are only listed when they aren't the identity.

:type: List[D3D9Transform]
)");
  rdcarray<Transform> transforms;

  DOCUMENT(R"(The lights that have been set or enabled.

:type: List[D3D9Light]
)");
  rdcarray<Light> lights;

  DOCUMENT(R"(The material.

:type: D3D9Material
)");
  Material material;

  DOCUMENT(R"(``True`` if lighting is enabled (``D3DRS_LIGHTING``).

:type: bool
)");
  bool lighting = false;

  DOCUMENT(R"(The ambient light color, as RGBA (``D3DRS_AMBIENT``).

:type: FloatVector
)");
  FloatVector ambient;

  DOCUMENT(R"(``True`` if specular highlights are enabled (``D3DRS_SPECULARENABLE``).

:type: bool
)");
  bool specularEnable = false;

  DOCUMENT(R"(``True`` if vertex normals are normalised (``D3DRS_NORMALIZENORMALS``).

:type: bool
)");
  bool normalizeNormals = false;

  DOCUMENT(R"(``True`` if specular highlights are camera-relative (``D3DRS_LOCALVIEWER``).

:type: bool
)");
  bool localViewer = false;

  DOCUMENT(R"(``True`` if vertex colors are used for lighting (``D3DRS_COLORVERTEX``).

:type: bool
)");
  bool colorVertex = false;

  DOCUMENT(R"(Where the diffuse color comes from: ``MATERIAL``, ``COLOR1`` or ``COLOR2``
(``D3DRS_DIFFUSEMATERIALSOURCE``).

:type: str
)");
  rdcstr diffuseMaterialSource;

  DOCUMENT(R"(Where the ambient color comes from (``D3DRS_AMBIENTMATERIALSOURCE``).

:type: str
)");
  rdcstr ambientMaterialSource;

  DOCUMENT(R"(Where the specular color comes from (``D3DRS_SPECULARMATERIALSOURCE``).

:type: str
)");
  rdcstr specularMaterialSource;

  DOCUMENT(R"(Where the emissive color comes from (``D3DRS_EMISSIVEMATERIALSOURCE``).

:type: str
)");
  rdcstr emissiveMaterialSource;

  DOCUMENT(R"(The vertex blending mode, e.g. ``DISABLE`` or ``2WEIGHTS`` (``D3DRS_VERTEXBLEND``).

:type: str
)");
  rdcstr vertexBlend;

  DOCUMENT(R"(``True`` if indexed vertex blending is enabled
(``D3DRS_INDEXEDVERTEXBLENDENABLE``).

:type: bool
)");
  bool indexedVertexBlend = false;

  DOCUMENT(R"(The shading mode: ``FLAT``, ``GOURAUD`` or ``PHONG`` (``D3DRS_SHADEMODE``).

:type: str
)");
  rdcstr shadeMode;

  DOCUMENT(R"(The texture factor color, as RGBA (``D3DRS_TEXTUREFACTOR``).

:type: FloatVector
)");
  FloatVector textureFactor;

  DOCUMENT(R"(The fog state.

:type: D3D9Fog
)");
  Fog fog;

  DOCUMENT(R"(The 6 user clip planes.

:type: List[D3D9ClipPlane]
)");
  rdcarray<ClipPlane> clipPlanes;
};

DOCUMENT(R"(
D3D9Rasterizer()
D3D9Rasterizer(other: D3D9Rasterizer)

Describes the D3D9 rasterizer state.
)");
struct Rasterizer
{
  DOCUMENT("");
  Rasterizer() = default;
  Rasterizer(const Rasterizer &) = default;
  Rasterizer &operator=(const Rasterizer &) = default;

  DOCUMENT(R"(The polygon fill mode.

:type: FillMode
)");
  FillMode fillMode = FillMode::Solid;

  DOCUMENT(R"(The polygon culling mode. D3D9 front faces are clockwise, so ``D3DCULL_CCW`` is
:data:`CullMode.Back` and ``D3DCULL_CW`` is :data:`CullMode.Front`.

:type: CullMode
)");
  CullMode cullMode = CullMode::Back;

  DOCUMENT(R"(The viewport.

:type: Viewport
)");
  Viewport viewport;

  DOCUMENT(R"(The scissor rectangle, enabled with ``D3DRS_SCISSORTESTENABLE``.

:type: Scissor
)");
  Scissor scissor;

  DOCUMENT(R"(The constant depth bias (``D3DRS_DEPTHBIAS``), in depth buffer units.

:type: float
)");
  float depthBias = 0.0f;

  DOCUMENT(R"(The slope scaled depth bias (``D3DRS_SLOPESCALEDEPTHBIAS``).

:type: float
)");
  float slopeScaledDepthBias = 0.0f;

  DOCUMENT(R"(``True`` if multisample antialiasing is enabled (``D3DRS_MULTISAMPLEANTIALIAS``).

:type: bool
)");
  bool multisampleEnable = true;

  DOCUMENT(R"(The sample mask (``D3DRS_MULTISAMPLEMASK``).

:type: int
)");
  uint32_t multisampleMask = ~0U;

  DOCUMENT(R"(``True`` if lines are antialiased (``D3DRS_ANTIALIASEDLINEENABLE``).

:type: bool
)");
  bool antialiasedLines = false;

  DOCUMENT(R"(``True`` if primitives are clipped (``D3DRS_CLIPPING``).

:type: bool
)");
  bool clipping = true;
};

DOCUMENT(R"(
D3D9DepthStencilState()
D3D9DepthStencilState(other: D3D9DepthStencilState)

Describes the D3D9 depth and stencil test state.
)");
struct DepthStencilState
{
  DOCUMENT("");
  DepthStencilState() = default;
  DepthStencilState(const DepthStencilState &) = default;
  DepthStencilState &operator=(const DepthStencilState &) = default;

  DOCUMENT(R"(``True`` if depth testing is enabled (``D3DRS_ZENABLE``).

:type: bool
)");
  bool depthEnable = false;

  DOCUMENT(R"(``True`` if w-buffering is used instead of z-buffering (``D3DZB_USEW``).

:type: bool
)");
  bool wBuffer = false;

  DOCUMENT(R"(``True`` if depth writes are enabled (``D3DRS_ZWRITEENABLE``).

:type: bool
)");
  bool depthWrites = false;

  DOCUMENT(R"(The depth comparison function.

:type: CompareFunction
)");
  CompareFunction depthFunction = CompareFunction::LessEqual;

  DOCUMENT(R"(``True`` if stencil operations are enabled.

:type: bool
)");
  bool stencilEnable = false;

  DOCUMENT(R"(``True`` if two-sided stencil is enabled (``D3DRS_TWOSIDEDSTENCILMODE``), so that
counter-clockwise primitives use :data:`backFace`.

:type: bool
)");
  bool twoSidedStencil = false;

  DOCUMENT(R"(The stencil state for clockwise (front facing) primitives, and for every primitive
when :data:`twoSidedStencil` is ``False``.

:type: StencilFace
)");
  StencilFace frontFace;

  DOCUMENT(R"(The stencil state for counter-clockwise primitives, from the ``D3DRS_CCW_STENCIL``
states. D3D9 has a single reference value and masks for both faces.

:type: StencilFace
)");
  StencilFace backFace;
};

DOCUMENT(R"(
D3D9AlphaTest()
D3D9AlphaTest(other: D3D9AlphaTest)

Describes the D3D9 alpha test.
)");
struct AlphaTest
{
  DOCUMENT("");
  AlphaTest() = default;
  AlphaTest(const AlphaTest &) = default;
  AlphaTest &operator=(const AlphaTest &) = default;

  DOCUMENT(R"(``True`` if the alpha test is enabled (``D3DRS_ALPHATESTENABLE``).

:type: bool
)");
  bool enabled = false;

  DOCUMENT(R"(The function that compares the pixel's alpha against :data:`reference`.

:type: CompareFunction
)");
  CompareFunction function = CompareFunction::AlwaysTrue;

  DOCUMENT(R"(The reference value, 0 to 255 (``D3DRS_ALPHAREF``).

:type: int
)");
  uint32_t reference = 0;
};

DOCUMENT(R"(
D3D9BlendState()
D3D9BlendState(other: D3D9BlendState)

Describes the D3D9 blend state.
)");
struct BlendState
{
  DOCUMENT("");
  BlendState() = default;
  BlendState(const BlendState &) = default;
  BlendState &operator=(const BlendState &) = default;

  DOCUMENT(R"(The blending for each of the 4 render targets. D3D9 has one blend equation for every
target, so they only differ in their write masks (``D3DRS_COLORWRITEENABLE`` to
``D3DRS_COLORWRITEENABLE3``). When :data:`separateAlphaBlend` is ``False`` the alpha blend is the
same as the color blend.

:type: List[ColorBlend]
)");
  rdcarray<ColorBlend> blends;

  DOCUMENT(R"(``True`` if alpha has its own blend equation (``D3DRS_SEPARATEALPHABLENDENABLE``).

:type: bool
)");
  bool separateAlphaBlend = false;

  DOCUMENT(R"(The constant blend factor, as RGBA (``D3DRS_BLENDFACTOR``).

:type: Tuple[float,float,float,float]
)");
  rdcfixedarray<float, 4> blendFactor = {1.0f, 1.0f, 1.0f, 1.0f};
};

DOCUMENT(R"(
D3D9OutputMerger()
D3D9OutputMerger(other: D3D9OutputMerger)

Describes the D3D9 output merger state.
)");
struct OutputMerger
{
  DOCUMENT("");
  OutputMerger() = default;
  OutputMerger(const OutputMerger &) = default;
  OutputMerger &operator=(const OutputMerger &) = default;

  DOCUMENT(R"(The depth and stencil state.

:type: D3D9DepthStencilState
)");
  DepthStencilState depthStencilState;

  DOCUMENT(R"(The blend state.

:type: D3D9BlendState
)");
  BlendState blendState;

  DOCUMENT(R"(The alpha test.

:type: D3D9AlphaTest
)");
  AlphaTest alphaTest;

  DOCUMENT(R"(``True`` if writes to the render targets are converted to sRGB
(``D3DRS_SRGBWRITEENABLE``).

:type: bool
)");
  bool srgbWrite = false;

  DOCUMENT(R"(``True`` if dithering is enabled (``D3DRS_DITHERENABLE``).

:type: bool
)");
  bool dither = false;

  DOCUMENT(R"(The 4 render targets. A surface that's a level or face of a texture is given as that
texture, with the mip and slice set.

:type: List[Descriptor]
)");
  rdcarray<Descriptor> renderTargets;

  DOCUMENT(R"(The depth stencil surface.

:type: Descriptor
)");
  Descriptor depthTarget;
};

DOCUMENT(R"(
D3D9RenderState()
D3D9RenderState(other: D3D9RenderState)

A single D3D9 render state and its raw value.
)");
struct RenderState
{
  DOCUMENT("");
  RenderState() = default;
  RenderState(const RenderState &) = default;
  RenderState &operator=(const RenderState &) = default;

  bool operator==(const RenderState &o) const { return name == o.name && value == o.value; }
  bool operator<(const RenderState &o) const
  {
    if(!(name == o.name))
      return name < o.name;
    if(!(value == o.value))
      return value < o.value;
    return false;
  }

  DOCUMENT(R"(The render state's name, e.g. ``D3DRS_ZENABLE``.

:type: str
)");
  rdcstr name;

  DOCUMENT(R"(The render state's value, as the DWORD the device holds. Float states hold the bits
of the float.

:type: int
)");
  uint32_t value = 0;
};

DOCUMENT(R"(
The full current D3D9 pipeline state.
)");
struct State
{
#if !defined(RENDERDOC_EXPORTS)
  // disallow creation/copy of this object externally
  State() = delete;
  State(const State &) = delete;
#endif

  DOCUMENT(R"(The vertex input state.

:type: D3D9InputAssembly
)");
  InputAssembly inputAssembly;

  DOCUMENT(R"(The vertex shader stage.

:type: D3D9Shader
)");
  Shader vertexShader;

  DOCUMENT(R"(The pixel shader stage.

:type: D3D9Shader
)");
  Shader pixelShader;

  DOCUMENT(R"(The 16 pixel samplers.

:type: List[D3D9Sampler]
)");
  rdcarray<Sampler> pixelSamplers;

  DOCUMENT(R"(The 4 vertex texture samplers. A shader model 3 vertex shader's sampler ``s0`` is
the first.

:type: List[D3D9Sampler]
)");
  rdcarray<Sampler> vertexSamplers;

  DOCUMENT(R"(The displacement map sampler (``D3DDMAPSAMPLER``).

:type: D3D9Sampler
)");
  Sampler displacementMapSampler;

  DOCUMENT(R"(The fixed function state.

:type: D3D9FixedFunction
)");
  FixedFunction fixedFunction;

  DOCUMENT(R"(The rasterizer state.

:type: D3D9Rasterizer
)");
  Rasterizer rasterizer;

  DOCUMENT(R"(The output merger state.

:type: D3D9OutputMerger
)");
  OutputMerger outputMerger;

  DOCUMENT(R"(Every render state the device has, with its raw value.

:type: List[D3D9RenderState]
)");
  rdcarray<RenderState> renderStates;

  DOCUMENT(R"(The virtual descriptor storage.

:type: ResourceId
)");
  ResourceId descriptorStore;

  DOCUMENT(R"(The number of descriptors in the virtual descriptor storage.

:type: int
)");
  uint32_t descriptorCount = 0;

  DOCUMENT(R"(The byte size of a descriptor in the virtual descriptor storage.

:type: int
)");
  uint32_t descriptorByteSize = 0;
};

};    // namespace D3D9Pipe

DECLARE_REFLECTION_STRUCT(D3D9Pipe::VertexElement);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::StreamSource);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::IndexBuffer);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::InputAssembly);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::Shader);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::Sampler);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::TextureStage);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::Transform);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::Light);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::Material);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::Fog);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::ClipPlane);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::FixedFunction);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::Rasterizer);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::DepthStencilState);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::AlphaTest);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::BlendState);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::OutputMerger);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::RenderState);
DECLARE_REFLECTION_STRUCT(D3D9Pipe::State);
