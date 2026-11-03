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

#include <map>
#include "replay/replay_driver.h"
#include "d3d9_common.h"
#include "d3d9_shader_reflect.h"

class WrappedD3DDevice9;

// The replay side of the driver. It owns the replay device (a WrappedD3DDevice9 replaying the
// capture, see d3d9_replay_device.cpp), and answers the replay controller's questions about it.
//
// Where things are:
// - loading, replaying to any event, the event list, resource descriptions, and fetching texture
//   and buffer data: here and d3d9_replay_device.cpp (phase 3a)
// - the texture viewer, pixel queries and mesh viewer inputs: d3d9_rendertexture.cpp,
//   d3d9_pixel.cpp and d3d9_rendermesh.cpp (phase 3b)
// - the pipeline state and descriptors: d3d9_pipestate.cpp (phase 3c)
// - shader reflection, disassembly and constants: d3d9_shaders.cpp (phase 3d)
// Mesh output and overlays aren't implemented yet (phase 3e).
class D3D9Replay final : public IReplayDriver
{
public:
  D3D9Replay(WrappedD3DDevice9 *device, IDirect3D9Ex *d3d, HWND hiddenWindow);

  ResourceDescription &GetResourceDesc(ResourceId id);
  FrameRecord &WriteFrameRecord() { return m_FrameRecord; }

  // keeps a shader's bytecode as it's created at replay, for reflection and disassembly
  // (d3d9_shaders.cpp)
  void AddShader(ResourceId id, const uint32_t *tokens, size_t numTokens);

  void Shutdown();

  APIProperties GetAPIProperties();

  rdcarray<ResourceDescription> GetResources();

  rdcarray<DescriptorStoreDescription> GetDescriptorStores();

  rdcarray<BufferDescription> GetBuffers();
  BufferDescription GetBuffer(ResourceId id);

  rdcarray<TextureDescription> GetTextures();
  TextureDescription GetTexture(ResourceId id);

  rdcarray<DebugMessage> GetDebugMessages();

  rdcarray<ShaderEntryPoint> GetShaderEntryPoints(ResourceId shader);
  const ShaderReflection *GetShader(ResourceId pipeline, ResourceId shader, ShaderEntryPoint entry);

  rdcarray<rdcstr> GetDisassemblyTargets(bool withPipeline);
  rdcstr DisassembleShader(ResourceId pipeline, const ShaderReflection *refl, const rdcstr &target);

  rdcarray<EventUsage> GetUsage(ResourceId id);

  void SetPipelineStates(D3D11Pipe::State *d3d11, D3D12Pipe::State *d3d12, GLPipe::State *gl,
                         VKPipe::State *vk, D3D9Pipe::State *d3d9);
  void SavePipelineState(uint32_t eventId);
  rdcarray<Descriptor> GetDescriptors(ResourceId descriptorStore,
                                      const rdcarray<DescriptorRange> &ranges);
  rdcarray<SamplerDescriptor> GetSamplerDescriptors(ResourceId descriptorStore,
                                                    const rdcarray<DescriptorRange> &ranges);
  rdcarray<DescriptorAccess> GetDescriptorAccess(uint32_t eventId);
  rdcarray<DescriptorLogicalLocation> GetDescriptorLocations(ResourceId descriptorStore,
                                                             const rdcarray<DescriptorRange> &ranges);

  FrameRecord GetFrameRecord() { return m_FrameRecord; }

  RDResult ReadLogInitialisation(RDCFile *rdc, bool storeStructuredBuffers);
  void ReplayLog(uint32_t endEventID, ReplayLogType replayType);
  SDFile *GetStructuredFile();

  rdcarray<uint32_t> GetPassEvents(uint32_t eventId);

  void InitPostVSBuffers(uint32_t eventId);
  void InitPostVSBuffers(const rdcarray<uint32_t> &passEvents);

  MeshFormat GetPostVSBuffers(uint32_t eventId, uint32_t instID, uint32_t viewID,
                              MeshDataStage stage);

  void GetBufferData(ResourceId buff, uint64_t offset, uint64_t len, bytebuf &retData);
  void GetTextureData(ResourceId tex, const Subresource &sub, const GetTextureDataParams &params,
                      bytebuf &data);

  void BuildTargetShader(ShaderEncoding sourceEncoding, const bytebuf &source, const rdcstr &entry,
                         const ShaderCompileFlags &compileFlags, ShaderStage type, ResourceId &id,
                         rdcstr &errors);
  rdcarray<ShaderEncoding> GetTargetShaderEncodings();
  void ReplaceResource(ResourceId from, ResourceId to);
  void RemoveReplacement(ResourceId id);
  void FreeTargetResource(ResourceId id);
  void ClearReplayCache();
  void ReloadShaderDebugInformation();

  rdcarray<GPUCounter> EnumerateCounters();
  CounterDescription DescribeCounter(GPUCounter counterID);
  rdcarray<CounterResult> FetchCounters(const rdcarray<GPUCounter> &counterID);

  void FillCBufferVariables(ResourceId pipeline, ResourceId shader, ShaderStage stage,
                            rdcstr entryPoint, uint32_t cbufSlot, rdcarray<ShaderVariable> &outvars,
                            const bytebuf &data);

  rdcarray<PixelModification> PixelHistory(rdcarray<EventUsage> events, ResourceId target, uint32_t x,
                                           uint32_t y, const Subresource &sub, CompType typeCast);
  ShaderDebugTrace *DebugVertex(uint32_t eventId, uint32_t vertid, uint32_t instid, uint32_t idx,
                                uint32_t view);
  ShaderDebugTrace *DebugPixel(uint32_t eventId, uint32_t x, uint32_t y,
                               const DebugPixelInputs &inputs);
  ShaderDebugTrace *DebugThread(uint32_t eventId, const rdcfixedarray<uint32_t, 3> &groupid,
                                const rdcfixedarray<uint32_t, 3> &threadid);
  ShaderDebugTrace *DebugMeshThread(uint32_t eventId, const rdcfixedarray<uint32_t, 3> &groupid,
                                    const rdcfixedarray<uint32_t, 3> &threadid);
  rdcarray<ShaderDebugState> ContinueDebug(ShaderDebugger *debugger);
  void FreeDebugger(ShaderDebugger *debugger);

  ResourceId RenderOverlay(ResourceId texid, FloatVector clearCol, DebugOverlay overlay,
                           uint32_t eventId, const rdcarray<uint32_t> &passEvents);

  bool IsRenderOutput(ResourceId id);

  void FileChanged() {}
  RDResult FatalErrorCheck();

  bool NeedRemapForFetch(const ResourceFormat &format) { return false; }

  DriverInformation GetDriverInfo() { return m_DriverInfo; }

  rdcarray<GPUDevice> GetAvailableGPUs() { return {}; }

  // IReplayDriver
  bool IsRemoteProxy() { return false; }

  IReplayDriver *MakeDummyDriver();

  rdcarray<WindowingSystem> GetSupportedWindowSystems() { return {WindowingSystem::Win32}; }

  AMDRGPControl *GetRGPControl() { return NULL; }

  uint64_t MakeOutputWindow(WindowingData window, bool depth);
  void DestroyOutputWindow(uint64_t id);
  bool CheckResizeOutputWindow(uint64_t id);
  void GetOutputWindowDimensions(uint64_t id, int32_t &w, int32_t &h);
  void GetOutputWindowData(uint64_t id, bytebuf &retData);
  void ClearOutputWindowColor(uint64_t id, FloatVector col);
  void ClearOutputWindowDepth(uint64_t id, float depth, uint8_t stencil);
  void BindOutputWindow(uint64_t id, bool depth);
  bool IsOutputWindowVisible(uint64_t id);
  void FlipOutputWindow(uint64_t id);

  bool GetMinMax(ResourceId texid, const Subresource &sub, CompType typeCast, float *minval,
                 float *maxval);
  bool GetHistogram(ResourceId texid, const Subresource &sub, CompType typeCast, float minval,
                    float maxval, const rdcfixedarray<bool, 4> &channels,
                    rdcarray<uint32_t> &histogram);
  void PickPixel(ResourceId texture, uint32_t x, uint32_t y, const Subresource &sub,
                 CompType typeCast, float pixel[4]);

  ResourceId CreateProxyTexture(const TextureDescription &templateTex) { return ResourceId(); }
  void SetProxyTextureData(ResourceId texid, const Subresource &sub, byte *data, size_t dataSize) {}
  bool IsTextureSupported(const TextureDescription &tex) { return true; }

  ResourceId CreateProxyBuffer(const BufferDescription &templateBuf) { return ResourceId(); }
  void SetProxyBufferData(ResourceId bufid, byte *data, size_t dataSize) {}

  void RenderMesh(uint32_t eventId, const rdcarray<MeshFormat> &secondaryDraws,
                  const MeshDisplay &cfg);
  bool RenderTexture(TextureDisplay cfg);

  void SetCustomShaderIncludes(const rdcarray<rdcstr> &directories) {}
  void BuildCustomShader(ShaderEncoding sourceEncoding, const bytebuf &source, const rdcstr &entry,
                         const ShaderCompileFlags &compileFlags, ShaderStage type, ResourceId &id,
                         rdcstr &errors);
  rdcarray<ShaderEncoding> GetCustomShaderEncodings() { return {ShaderEncoding::HLSL}; }
  rdcarray<ShaderSourcePrefix> GetCustomShaderSourcePrefixes() { return {}; }
  ResourceId ApplyCustomShader(TextureDisplay &display) { return ResourceId(); }
  void FreeCustomShader(ResourceId id) {}

  void RenderCheckerboard(FloatVector dark, FloatVector light);

  void RenderHighlightBox(float w, float h, float scale);

  uint32_t PickVertex(uint32_t eventId, int32_t width, int32_t height, const MeshDisplay &cfg,
                      uint32_t x, uint32_t y)
  {
    return ~0U;
  }

private:
  ~D3D9Replay() = default;

  struct OutputWindow
  {
    HWND wnd = NULL;
    bool depth = false;
    int32_t width = 0;
    int32_t height = 0;
    // on the real device, not wrapped
    IDirect3DSwapChain9 *swap = NULL;
    IDirect3DSurface9 *backBuffer = NULL;
    IDirect3DSurface9 *depthStencil = NULL;

    void Release();
  };

  bool CreateOutputSwapChain(OutputWindow &outw);

  //////////////////////////////
  // Texture viewer, pixel queries and mesh viewer (phase 3b): d3d9_rendertexture.cpp,
  // d3d9_pixel.cpp and d3d9_rendermesh.cpp. Everything draws on the real device, and saves and
  // restores the replayed state around it.

  // A subresource's contents decoded to floats, in the channels the format is described with.
  // Used for pixel queries, and for displaying what can't be sampled directly.
  struct DecodedKey
  {
    ResourceId id;
    uint32_t mip = 0;
    // the cube face, or the slice of a volume
    uint32_t slice = 0;
    uint32_t sample = 0;

    bool operator<(const DecodedKey &o) const
    {
      if(id != o.id)
        return id < o.id;
      if(mip != o.mip)
        return mip < o.mip;
      if(slice != o.slice)
        return slice < o.slice;
      return sample < o.sample;
    }
  };

  struct DecodedSubresource
  {
    // why it couldn't be decoded, when texels is empty
    rdcstr error;
    uint32_t width = 0;
    uint32_t height = 0;
    rdcarray<FloatVector> texels;
    // an A32B32G32R32F copy on the real device, made the first time it's displayed
    IDirect3DTexture9 *upload = NULL;
  };

  // what the display shader samples, and how the sampled channels map to the described format's
  struct DisplaySource
  {
    // on the real device, not owned
    IDirect3DBaseTexture9 *tex = NULL;
    // 0: 2D, 1: cube, 2: volume
    uint32_t type = 0;
    float lod = 0.0f;
    // the cube face, or the volume's w coordinate
    float slice = 0.0f;
    bool filterable = false;
    // rows of the matrix applied to the sampled value, then add
    Vec4f swizzle[4];
    Vec4f add;
  };

  struct OutputTarget
  {
    IDirect3DSurface9 *rt = NULL;
    IDirect3DSurface9 *ds = NULL;
    int32_t width = 0;
    int32_t height = 0;
  };

  // the shaders we draw with, on the real device, created on first use
  struct DebugRender
  {
    bool tried = false;
    bool ok = false;
    // whether the output windows' back buffers can be written with sRGB conversion
    bool srgbWrite = false;
    IDirect3DVertexShader9 *texDisplayVS = NULL;
    IDirect3DPixelShader9 *texDisplayPS = NULL;
    IDirect3DPixelShader9 *checkerPS = NULL;
    IDirect3DVertexDeclaration9 *quadDecl = NULL;
    IDirect3DVertexShader9 *meshVS = NULL;
    IDirect3DPixelShader9 *meshPS = NULL;
    IDirect3DVertexDeclaration9 *meshDecl = NULL;

    void Release();
  } m_Debug;

  bool InitDebugRender();
  // the bound output window's targets, or false if there isn't one
  bool GetOutputTarget(OutputTarget &target);
  // what to sample to display a subresource. Without allowDecoded only native sampling is used.
  bool GetDisplaySource(ResourceId id, const Subresource &sub, bool allowDecoded, DisplaySource &src);
  // draws the texture display quad into the bound target. posScale is the quad's top left
  // corner (xy) and size (zw) in clip space.
  void DrawTextureQuad(const TextureDisplay &cfg, const DisplaySource &src, const Vec4f &posScale,
                       int32_t targetWidth, int32_t targetHeight, bool blend, bool srgbWrite);
  DecodedSubresource &GetDecoded(ResourceId id, const Subresource &sub);
  // reads a subresource back through the display shader, for what can't be read directly
  bool ReadDecodedFromGPU(ResourceId id, const Subresource &sub, DecodedSubresource &decoded);
  // cached data is only valid for the event it was read at
  void ClearReplayCaches();
  void ReleaseDebugRender();

  std::map<DecodedKey, DecodedSubresource> m_Decoded;
  // render target surfaces are copied into textures to sample them
  std::map<ResourceId, IDirect3DTexture9 *> m_SurfaceCopies;
  // D3DUSAGE_QUERY_FILTER results by format
  std::map<DWORD, bool> m_Filterable;

  uint64_t m_CurrentOutputWindow = 0;

  // a mesh viewer draw's vertices: positions decoded, indices expanded
  struct MeshVertices
  {
    IDirect3DVertexBuffer9 *vb = NULL;
    uint32_t numVerts = 0;
  };
  std::map<uint64_t, MeshVertices> m_MeshCache;
  HighlightCache m_HighlightCache;

  const MeshVertices *GetMeshVertices(uint32_t eventId, const MeshFormat &pos,
                                      const MeshFormat *secondary, uint32_t curInstance);

  // pipeline state, see d3d9_pipestate.cpp
  void CreatePipelineStateResources();
  int ConstantsBufferIndex(ResourceId id) const;
  BufferDescription GetConstantsBuffer(ResourceId id);
  void GetConstantsData(ResourceId id, uint64_t offset, uint64_t len, bytebuf &retData);
  Descriptor MakeSurfaceDescriptor(IDirect3DSurface9 *surf);
  Topology GetEventTopology(uint32_t eventId);

  D3D9Pipe::State *m_D3D9PipelineState = NULL;
  // the virtual descriptor store the bindings are presented in
  ResourceId m_DescriptorStore;
  // pseudo buffers for the shader constants: VS float, int, bool, then PS float, int, bool. Their
  // contents are read from the device when they're fetched.
  ResourceId m_ConstantsBuffers[6];
  uint32_t m_ConstantCounts[6] = {};

  WrappedD3DDevice9 *m_pDevice = NULL;
  IDirect3D9Ex *m_D3D = NULL;
  HWND m_HiddenWindow = NULL;

  DriverInformation m_DriverInfo = {};

  rdcarray<ResourceDescription> m_Resources;
  std::map<ResourceId, size_t> m_ResourceIdx;

  FrameRecord m_FrameRecord;

  std::map<uint64_t, OutputWindow> m_OutputWindows;
  uint64_t m_OutputWindowID = 1;

  // every vertex and pixel shader, by ID. The replay controller holds on to pointers to their
  // reflection, which a map keeps where they are.
  std::map<ResourceId, D3D9BC::ShaderInfo> m_Shaders;
};
