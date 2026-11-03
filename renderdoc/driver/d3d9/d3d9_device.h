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

#include "common/threading.h"
#include "replay/replay_driver.h"
#include "serialise/rdcfile.h"
#include "d3d9_common.h"
#include "d3d9_manager.h"
#include "d3d9_resources.h"
#include "d3d9_state.h"

class D3D9DebugManager;
class D3D9Replay;
class WrappedD3DSurface9;
class WrappedD3DVolume9;
class WrappedD3DTexture9;
class WrappedD3DCubeTexture9;
class WrappedD3DVolumeTexture9;
class WrappedD3DStateBlock9;

class WrappedD3DDevice9 final : public IDirect3DDevice9Ex, public IFrameCapturer
{
public:
  WrappedD3DDevice9(IDirect3DDevice9 *device, WrappedD3D9 *wrappedD3D, HWND wnd,
                    const D3D9InitParams &params);
  // a device with no real device behind it, for structured export of captures
  explicit WrappedD3DDevice9(const D3D9InitParams &params);
  ~WrappedD3DDevice9();

  void LazyInit();

  IDirect3DDevice9 *GetReal() const { return m_device; }
  bool IsExDevice() const { return m_deviceEx != NULL; }
  D3D9ResourceManager *GetResourceManager() const { return m_ResourceManager; }
  D3D9DebugManager *GetDebugManager() const { return m_DebugManager; }

  // protects wrapper creation and the tracked bindings. Recursive, like all CriticalSections.
  Threading::CriticalSection &D3DLock() { return m_D3DLock; }

  // state block support. Capture copies the device's current bindings selected by mask, Apply
  // copies them back (into the state block being recorded, if any, like the runtime does).
  void CaptureBindings(D3D9ObjectBindings &dst, const D3D9BindingMask &mask);
  void ApplyBindings(const D3D9ObjectBindings &src, const D3D9BindingMask &mask);

  //////////////////////////////
  // capture

  CaptureState GetState() const { return m_State; }
  WriteSerialiser &GetScratchSerialiser() { return m_ScratchSerialiser; }

  // create the resource record for a new object, with its creation chunk written by serialise
  template <typename SerialiseFunc>
  void RecordCreation(WrappedD3D9Object *obj, D3D9Chunk chunk, SerialiseFunc serialise,
                      WrappedD3D9Object *parent = NULL);

  // records for objects created on first use by their containers
  void RecordSurfaceLevel(WrappedD3DTexture9 *tex, UINT level, WrappedD3DSurface9 *surf);
  void RecordCubeFace(WrappedD3DCubeTexture9 *cube, D3DCUBEMAP_FACES face, UINT level,
                      WrappedD3DSurface9 *surf);
  void RecordVolumeLevel(WrappedD3DVolumeTexture9 *tex, UINT level, WrappedD3DVolume9 *vol);
  void RecordBackBuffer(WrappedD3DSwapChain9 *swap, UINT index, WrappedD3DSurface9 *surf);

  // called when a wrapper dies. Records are kept until the end of a capture in progress, since
  // its chunks may still be needed.
  void ReleaseRecord(D3D9ResourceRecord *record);

  // calls on objects other than the device that are recorded into the frame
  void OnStateBlockCapture(IDirect3DStateBlock9 *sb);
  void OnStateBlockApply(IDirect3DStateBlock9 *sb);
  void OnQueryIssue(IDirect3DQuery9 *query, DWORD flags);
  void OnGenerateMipSubLevels(IDirect3DBaseTexture9 *tex);
  void OnSetAutoGenFilterType(IDirect3DBaseTexture9 *tex, D3DTEXTUREFILTERTYPE filter);
  void OnSetLOD(IDirect3DBaseTexture9 *tex, DWORD lod);
  void OnSwapChainPresent(IDirect3DSwapChain9 *swap, CONST RECT *pSourceRect, CONST RECT *pDestRect,
                          HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion, DWORD dwFlags);

  //////////////////////////////
  // resource contents, see d3d9_contents.h

  // reads an object's current contents, for its initial contents
  void ReadContents(WrappedD3D9Object *obj, D3D9InitialContentsData &contents);

  // called before anything writes to obj (or the object holding its contents) during a capture,
  // so that its initial contents are read first
  void PrepareForWrite(WrappedD3D9Object *obj);
  void PrepareForWrite(IUnknown *obj);
  void PrepareRenderTargetsForWrite();

  // the application released its last reference to obj
  void OnLastExternalRelease(WrappedD3D9Object *obj);

  // locks and dirty regions on obj's subresource face/level. region is NULL for all of it.
  void OnLock(WrappedD3D9Object *obj, UINT face, UINT level, const D3DBOX *region, DWORD flags);
  void OnLocked(WrappedD3D9Object *obj, UINT face, UINT level, const D3DBOX *region, DWORD flags,
                void *data, UINT rowPitch, UINT slicePitch);
  // records the unlock's data, returning whether it did. OnUnlocked is called after the real unlock.
  bool OnUnlock(WrappedD3D9Object *obj, UINT face, UINT level);
  void OnUnlocked(WrappedD3D9Object *obj);
  void OnAddDirtyRegion(WrappedD3D9Object *obj, UINT face, const D3DBOX *box);

  // writes that aren't locks, called after they succeed
  void OnReleaseDC(WrappedD3D9Object *surf);
  void OnSwapChainGetFrontBufferData(IDirect3DSwapChain9 *swap, IDirect3DSurface9 *pDestSurface);
  void OnGetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface);

  // keep the shadows of D3DPOOL_DEFAULT textures up to date, after a successful copy
  void UpdateShadowFromTexture(IDirect3DBaseTexture9 *pSourceTexture,
                               IDirect3DBaseTexture9 *pDestinationTexture);
  void UpdateShadowFromSurface(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect,
                               IDirect3DSurface9 *pDestinationSurface, CONST POINT *pDestPoint);

  // D3DPERF markers, which aren't tied to a device: every device that's capturing records them
  static void PerfMarker(D3D9Chunk chunk, DWORD col, const wchar_t *name);

  // called by swapchain and device presents, before the real present
  void HandlePresent(IDirect3DSwapChain9 *realSwapChain, HWND hDestWindowOverride);

  //////////////////////////////
  // structured export of captures

  void SetStructuredExport(uint64_t sectionVersion)
  {
    m_SectionVersion = sectionVersion;
    m_State = CaptureState::StructuredExport;
  }
  RDResult ReadLogInitialisation(RDCFile *rdc, bool storeStructuredBuffers);
  SDFile *GetStructuredFile() { return m_StructuredFile; }
  static rdcstr GetChunkName(uint32_t idx);

  //////////////////////////////
  // replay, see d3d9_replay_device.cpp

  // window is the hidden window the capture's swapchains present to
  void SetReplay(D3D9Replay *replay, HWND window)
  {
    m_Replay = replay;
    m_ReplayWindow = window;
  }
  D3D9Replay *GetReplay() const { return m_Replay; }
  void ReplayLog(uint32_t startEventID, uint32_t endEventID, ReplayLogType replayType);
  const ActionDescription *GetAction(uint32_t eventId);
  SDFile *DetachStructuredFile();
  // drops the references replay holds on the objects it created
  void ReleaseReplayObjects();
  // whether the real device stopped working
  RDResult FatalErrorCheck();

  // the ID the next wrapper created takes, so that wrappers created during replay have their
  // captured IDs. Consumed by WrappedD3D9Object's constructor, which otherwise gets a new one.
  ResourceId TakeWrapperID();

  // the texture a surface belongs to, or the surface itself if it isn't a texture level
  ResourceId SurfaceTextureID(IDirect3DSurface9 *surf);

  // writes a subresource's data to obj (which holds the contents, or is a texture level or volume
  // level whose face and level are 0 in sub)
  void WriteContents(WrappedD3D9Object *obj, const D3D9SubresourceData &sub);
  // sets the device to the state block's values and captures them into it
  void ApplyStateBlockContents(WrappedD3DStateBlock9 *sb, const D3D9StateBlockContents &contents);
  // reads one subresource of a texture, standalone surface or buffer, rows packed. face is the
  // cube face, for cube textures.
  rdcstr ReadSubresource(WrappedD3D9Object *obj, uint32_t face, uint32_t level,
                         D3D9SubresourceData &sub);

  //////////////////////////////
  // implement IFrameCapturer

  RDCDriver GetFrameCaptureDriver() { return RDCDriver::D3D9; }
  void StartFrameCapture(DeviceOwnedWindow devWnd);
  bool EndFrameCapture(DeviceOwnedWindow devWnd);
  bool DiscardFrameCapture(DeviceOwnedWindow devWnd);

  // annotations are not supported until D3D9 capture is implemented
  uint32_t SetObjectAnnotation(void *object, const char *key, RENDERDOC_AnnotationType valueType,
                               uint32_t valueVectorWidth, const RENDERDOC_AnnotationValue *value)
  {
    return 2;
  }
  uint32_t SetCommandAnnotation(void *queueOrCommandBuffer, const char *key,
                                RENDERDOC_AnnotationType valueType, uint32_t valueVectorWidth,
                                const RENDERDOC_AnnotationValue *value)
  {
    return 2;
  }

  //////////////////////////////
  // implement IUnknown

  ULONG STDMETHODCALLTYPE AddRef();
  ULONG STDMETHODCALLTYPE Release();
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject);

  // serialisation, see d3d9_device_serialise.cpp. Phase 2 only writes; replay comes in phase 3.
  template <typename SerialiserType>
  bool Serialise_SetRenderTarget(SerialiserType &ser, DWORD RenderTargetIndex,
                                 IDirect3DSurface9 *pRenderTarget);
  template <typename SerialiserType>
  bool Serialise_SetDepthStencilSurface(SerialiserType &ser, IDirect3DSurface9 *pNewZStencil);
  template <typename SerialiserType>
  bool Serialise_BeginScene(SerialiserType &ser);
  template <typename SerialiserType>
  bool Serialise_EndScene(SerialiserType &ser);
  template <typename SerialiserType>
  bool Serialise_Clear(SerialiserType &ser, DWORD Count, CONST D3DRECT *pRects, DWORD Flags,
                       D3DCOLOR Color, float Z, DWORD Stencil);
  template <typename SerialiserType>
  bool Serialise_SetTransform(SerialiserType &ser, D3DTRANSFORMSTATETYPE State,
                              CONST D3DMATRIX *pMatrix);
  template <typename SerialiserType>
  bool Serialise_MultiplyTransform(SerialiserType &ser, D3DTRANSFORMSTATETYPE State,
                                   CONST D3DMATRIX *pMatrix);
  template <typename SerialiserType>
  bool Serialise_SetViewport(SerialiserType &ser, CONST D3DVIEWPORT9 *pViewport);
  template <typename SerialiserType>
  bool Serialise_SetMaterial(SerialiserType &ser, CONST D3DMATERIAL9 *pMaterial);
  template <typename SerialiserType>
  bool Serialise_SetLight(SerialiserType &ser, DWORD Index, CONST D3DLIGHT9 *pLight);
  template <typename SerialiserType>
  bool Serialise_LightEnable(SerialiserType &ser, DWORD Index, BOOL Enable);
  template <typename SerialiserType>
  bool Serialise_SetClipPlane(SerialiserType &ser, DWORD Index, CONST float *pPlane);
  template <typename SerialiserType>
  bool Serialise_SetRenderState(SerialiserType &ser, D3DRENDERSTATETYPE State, DWORD Value);
  template <typename SerialiserType>
  bool Serialise_BeginStateBlock(SerialiserType &ser);
  template <typename SerialiserType>
  bool Serialise_SetClipStatus(SerialiserType &ser, CONST D3DCLIPSTATUS9 *pClipStatus);
  template <typename SerialiserType>
  bool Serialise_SetTexture(SerialiserType &ser, DWORD Stage, IDirect3DBaseTexture9 *pTexture);
  template <typename SerialiserType>
  bool Serialise_SetTextureStageState(SerialiserType &ser, DWORD Stage,
                                      D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
  template <typename SerialiserType>
  bool Serialise_SetSamplerState(SerialiserType &ser, DWORD Sampler, D3DSAMPLERSTATETYPE Type,
                                 DWORD Value);
  template <typename SerialiserType>
  bool Serialise_SetPaletteEntries(SerialiserType &ser, UINT PaletteNumber,
                                   CONST PALETTEENTRY *pEntries);
  template <typename SerialiserType>
  bool Serialise_SetCurrentTexturePalette(SerialiserType &ser, UINT PaletteNumber);
  template <typename SerialiserType>
  bool Serialise_SetScissorRect(SerialiserType &ser, CONST RECT *pRect);
  template <typename SerialiserType>
  bool Serialise_SetSoftwareVertexProcessing(SerialiserType &ser, BOOL bSoftware);
  template <typename SerialiserType>
  bool Serialise_SetNPatchMode(SerialiserType &ser, float nSegments);
  template <typename SerialiserType>
  bool Serialise_DrawPrimitive(SerialiserType &ser, D3DPRIMITIVETYPE PrimitiveType,
                               UINT StartVertex, UINT PrimitiveCount);
  template <typename SerialiserType>
  bool Serialise_DrawIndexedPrimitive(SerialiserType &ser, D3DPRIMITIVETYPE PrimitiveType,
                                      INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices,
                                      UINT startIndex, UINT primCount);
  template <typename SerialiserType>
  bool Serialise_ProcessVertices(SerialiserType &ser, UINT SrcStartIndex, UINT DestIndex,
                                 UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer,
                                 IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags);
  template <typename SerialiserType>
  bool Serialise_SetVertexDeclaration(SerialiserType &ser, IDirect3DVertexDeclaration9 *pDecl);
  template <typename SerialiserType>
  bool Serialise_SetFVF(SerialiserType &ser, DWORD FVF);
  template <typename SerialiserType>
  bool Serialise_SetVertexShader(SerialiserType &ser, IDirect3DVertexShader9 *pShader);
  template <typename SerialiserType>
  bool Serialise_SetVertexShaderConstantF(SerialiserType &ser, UINT StartRegister,
                                          CONST float *pConstantData, UINT Vector4fCount);
  template <typename SerialiserType>
  bool Serialise_SetVertexShaderConstantI(SerialiserType &ser, UINT StartRegister,
                                          CONST int *pConstantData, UINT Vector4iCount);
  template <typename SerialiserType>
  bool Serialise_SetVertexShaderConstantB(SerialiserType &ser, UINT StartRegister,
                                          CONST BOOL *pConstantData, UINT BoolCount);
  template <typename SerialiserType>
  bool Serialise_SetStreamSource(SerialiserType &ser, UINT StreamNumber,
                                 IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes,
                                 UINT Stride);
  template <typename SerialiserType>
  bool Serialise_SetStreamSourceFreq(SerialiserType &ser, UINT StreamNumber, UINT Setting);
  template <typename SerialiserType>
  bool Serialise_SetIndices(SerialiserType &ser, IDirect3DIndexBuffer9 *pIndexData);
  template <typename SerialiserType>
  bool Serialise_SetPixelShader(SerialiserType &ser, IDirect3DPixelShader9 *pShader);
  template <typename SerialiserType>
  bool Serialise_SetPixelShaderConstantF(SerialiserType &ser, UINT StartRegister,
                                         CONST float *pConstantData, UINT Vector4fCount);
  template <typename SerialiserType>
  bool Serialise_SetPixelShaderConstantI(SerialiserType &ser, UINT StartRegister,
                                         CONST int *pConstantData, UINT Vector4iCount);
  template <typename SerialiserType>
  bool Serialise_SetPixelShaderConstantB(SerialiserType &ser, UINT StartRegister,
                                         CONST BOOL *pConstantData, UINT BoolCount);
  template <typename SerialiserType>
  bool Serialise_DrawRectPatch(SerialiserType &ser, UINT Handle, CONST float *pNumSegs,
                               CONST D3DRECTPATCH_INFO *pRectPatchInfo);
  template <typename SerialiserType>
  bool Serialise_DrawTriPatch(SerialiserType &ser, UINT Handle, CONST float *pNumSegs,
                              CONST D3DTRIPATCH_INFO *pTriPatchInfo);
  template <typename SerialiserType>
  bool Serialise_DeletePatch(SerialiserType &ser, UINT Handle);
  template <typename SerialiserType>
  bool Serialise_UpdateSurface(SerialiserType &ser, IDirect3DSurface9 *pSourceSurface,
                               CONST RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface,
                               CONST POINT *pDestPoint);
  template <typename SerialiserType>
  bool Serialise_UpdateTexture(SerialiserType &ser, IDirect3DBaseTexture9 *pSourceTexture,
                               IDirect3DBaseTexture9 *pDestinationTexture);
  template <typename SerialiserType>
  bool Serialise_GetRenderTargetData(SerialiserType &ser, IDirect3DSurface9 *pRenderTarget,
                                     IDirect3DSurface9 *pDestSurface);
  template <typename SerialiserType>
  bool Serialise_StretchRect(SerialiserType &ser, IDirect3DSurface9 *pSourceSurface,
                             CONST RECT *pSourceRect, IDirect3DSurface9 *pDestSurface,
                             CONST RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter);
  template <typename SerialiserType>
  bool Serialise_ColorFill(SerialiserType &ser, IDirect3DSurface9 *pSurface, CONST RECT *pRect,
                           D3DCOLOR color);
  template <typename SerialiserType>
  bool Serialise_ComposeRects(SerialiserType &ser, IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst,
                              IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects,
                              IDirect3DVertexBuffer9 *pDstRectDescs, D3DCOMPOSERECTSOP Operation,
                              int Xoffset, int Yoffset);
  template <typename SerialiserType>
  bool Serialise_SetConvolutionMonoKernel(SerialiserType &ser, UINT width, UINT height, float *rows,
                                          float *columns);
  template <typename SerialiserType, typename Iface>
  bool Serialise_Unlock(SerialiserType &ser, Iface *pResource, DWORD Flags,
                        D3D9SubresourceData &Contents);
  template <typename SerialiserType>
  bool Serialise_Surface_ReleaseDC(SerialiserType &ser, IDirect3DSurface9 *pSurface,
                                   D3D9SubresourceData &Contents);
  template <typename SerialiserType>
  bool Serialise_GetFrontBufferData(SerialiserType &ser, UINT iSwapChain,
                                    IDirect3DSurface9 *pDestSurface, D3D9SubresourceData &Contents);
  template <typename SerialiserType>
  bool Serialise_SwapChain_GetFrontBufferData(SerialiserType &ser, IDirect3DSwapChain9 *pSwapChain,
                                              IDirect3DSurface9 *pDestSurface,
                                              D3D9SubresourceData &Contents);
  template <typename SerialiserType>
  bool Serialise_D3DPERF_BeginEvent(SerialiserType &ser, D3DCOLOR col, rdcstr Name);
  template <typename SerialiserType>
  bool Serialise_D3DPERF_EndEvent(SerialiserType &ser);
  template <typename SerialiserType>
  bool Serialise_D3DPERF_SetMarker(SerialiserType &ser, D3DCOLOR col, rdcstr Name);
  template <typename SerialiserType>
  bool Serialise_Texture_AddDirtyRect(SerialiserType &ser, IDirect3DTexture9 *pTexture,
                                      CONST RECT *pDirtyRect);
  template <typename SerialiserType>
  bool Serialise_CubeTexture_AddDirtyRect(SerialiserType &ser, IDirect3DCubeTexture9 *pCubeTexture,
                                          D3DCUBEMAP_FACES FaceType, CONST RECT *pDirtyRect);
  template <typename SerialiserType>
  bool Serialise_VolumeTexture_AddDirtyBox(SerialiserType &ser,
                                           IDirect3DVolumeTexture9 *pVolumeTexture,
                                           CONST D3DBOX *pDirtyBox);
  template <typename SerialiserType>
  bool Serialise_StateBlock_Capture(SerialiserType &ser, IDirect3DStateBlock9 *pStateBlock);
  template <typename SerialiserType>
  bool Serialise_StateBlock_Apply(SerialiserType &ser, IDirect3DStateBlock9 *pStateBlock);
  template <typename SerialiserType>
  bool Serialise_Query_Issue(SerialiserType &ser, IDirect3DQuery9 *pQuery, DWORD dwIssueFlags);
  template <typename SerialiserType>
  bool Serialise_BaseTexture_GenerateMipSubLevels(SerialiserType &ser,
                                                  IDirect3DBaseTexture9 *pTexture);
  template <typename SerialiserType>
  bool Serialise_BaseTexture_SetAutoGenFilterType(SerialiserType &ser,
                                                  IDirect3DBaseTexture9 *pTexture,
                                                  D3DTEXTUREFILTERTYPE FilterType);
  template <typename SerialiserType>
  bool Serialise_BaseTexture_SetLOD(SerialiserType &ser, IDirect3DBaseTexture9 *pTexture,
                                    DWORD LODNew);
  template <typename SerialiserType>
  bool Serialise_CreateTexture(SerialiserType &ser, UINT Width, UINT Height, UINT Levels,
                               DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                               IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle);
  template <typename SerialiserType>
  bool Serialise_CreateVolumeTexture(SerialiserType &ser, UINT Width, UINT Height, UINT Depth,
                                     UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                     IDirect3DVolumeTexture9 **ppVolumeTexture,
                                     HANDLE *pSharedHandle);
  template <typename SerialiserType>
  bool Serialise_CreateCubeTexture(SerialiserType &ser, UINT EdgeLength, UINT Levels, DWORD Usage,
                                   D3DFORMAT Format, D3DPOOL Pool,
                                   IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle);
  template <typename SerialiserType>
  bool Serialise_CreateVertexBuffer(SerialiserType &ser, UINT Length, DWORD Usage, DWORD FVF,
                                    D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer,
                                    HANDLE *pSharedHandle);
  template <typename SerialiserType>
  bool Serialise_CreateIndexBuffer(SerialiserType &ser, UINT Length, DWORD Usage, D3DFORMAT Format,
                                   D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer,
                                   HANDLE *pSharedHandle);
  template <typename SerialiserType>
  bool Serialise_CreateRenderTarget(SerialiserType &ser, UINT Width, UINT Height, D3DFORMAT Format,
                                    D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                    BOOL Lockable, IDirect3DSurface9 **ppSurface,
                                    HANDLE *pSharedHandle);
  template <typename SerialiserType>
  bool Serialise_CreateDepthStencilSurface(SerialiserType &ser, UINT Width, UINT Height,
                                           D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                           DWORD MultisampleQuality, BOOL Discard,
                                           IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle);
  template <typename SerialiserType>
  bool Serialise_CreateOffscreenPlainSurface(SerialiserType &ser, UINT Width, UINT Height,
                                             D3DFORMAT Format, D3DPOOL Pool,
                                             IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle);
  template <typename SerialiserType>
  bool Serialise_CreateRenderTargetEx(SerialiserType &ser, UINT Width, UINT Height, D3DFORMAT Format,
                                      D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                      BOOL Lockable, IDirect3DSurface9 **ppSurface,
                                      HANDLE *pSharedHandle, DWORD Usage);
  template <typename SerialiserType>
  bool Serialise_CreateOffscreenPlainSurfaceEx(SerialiserType &ser, UINT Width, UINT Height,
                                               D3DFORMAT Format, D3DPOOL Pool,
                                               IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle,
                                               DWORD Usage);
  template <typename SerialiserType>
  bool Serialise_CreateDepthStencilSurfaceEx(SerialiserType &ser, UINT Width, UINT Height,
                                             D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                             DWORD MultisampleQuality, BOOL Discard,
                                             IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle,
                                             DWORD Usage);
  template <typename SerialiserType>
  bool Serialise_CreateQuery(SerialiserType &ser, D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery);
  template <typename SerialiserType>
  bool Serialise_CreateStateBlock(SerialiserType &ser, D3DSTATEBLOCKTYPE Type,
                                  IDirect3DStateBlock9 **ppSB);
  template <typename SerialiserType>
  bool Serialise_EndStateBlock(SerialiserType &ser, IDirect3DStateBlock9 **ppSB);
  template <typename SerialiserType>
  bool Serialise_CreateAdditionalSwapChain(SerialiserType &ser,
                                           D3DPRESENT_PARAMETERS *pPresentationParameters,
                                           IDirect3DSwapChain9 **pSwapChain);
  template <typename SerialiserType>
  bool Serialise_Texture_GetSurfaceLevel(SerialiserType &ser, IDirect3DTexture9 *pTexture,
                                         UINT Level, IDirect3DSurface9 *_pSurfaceLevel);
  template <typename SerialiserType>
  bool Serialise_CubeTexture_GetCubeMapSurface(SerialiserType &ser,
                                               IDirect3DCubeTexture9 *pCubeTexture,
                                               D3DCUBEMAP_FACES FaceType, UINT Level,
                                               IDirect3DSurface9 *_pCubeMapSurface);
  template <typename SerialiserType>
  bool Serialise_VolumeTexture_GetVolumeLevel(SerialiserType &ser,
                                              IDirect3DVolumeTexture9 *pVolumeTexture, UINT Level,
                                              IDirect3DVolume9 *_pVolumeLevel);
  template <typename SerialiserType>
  bool Serialise_SwapChain_GetBackBuffer(SerialiserType &ser, IDirect3DSwapChain9 *pSwapChain,
                                         UINT iBackBuffer, IDirect3DSurface9 *_pBackBuffer);
  template <typename SerialiserType>
  bool Serialise_DeviceInitialisation(SerialiserType &ser);
  template <typename SerialiserType>
  bool Serialise_BeginCaptureFrame(SerialiserType &ser);
  template <typename SerialiserType>
  bool Serialise_CreateVertexDeclaration(SerialiserType &ser,
                                         CONST D3DVERTEXELEMENT9 *pVertexElements,
                                         IDirect3DVertexDeclaration9 **ppDecl);
  template <typename SerialiserType>
  bool Serialise_CreateVertexShader(SerialiserType &ser, CONST DWORD *pFunction,
                                    IDirect3DVertexShader9 **ppShader);
  template <typename SerialiserType>
  bool Serialise_CreatePixelShader(SerialiserType &ser, CONST DWORD *pFunction,
                                   IDirect3DPixelShader9 **ppShader);
  template <typename SerialiserType>
  bool Serialise_DrawPrimitiveUP(SerialiserType &ser, D3DPRIMITIVETYPE PrimitiveType,
                                 UINT PrimitiveCount, CONST void *pVertexStreamZeroData,
                                 UINT VertexStreamZeroStride);
  template <typename SerialiserType>
  bool Serialise_DrawIndexedPrimitiveUP(SerialiserType &ser, D3DPRIMITIVETYPE PrimitiveType,
                                        UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount,
                                        CONST void *pIndexData, D3DFORMAT IndexDataFormat,
                                        CONST void *pVertexStreamZeroData,
                                        UINT VertexStreamZeroStride);
  template <typename SerialiserType>
  bool Serialise_Present(SerialiserType &ser, CONST RECT *pSourceRect, CONST RECT *pDestRect,
                         HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion);
  template <typename SerialiserType>
  bool Serialise_PresentEx(SerialiserType &ser, CONST RECT *pSourceRect, CONST RECT *pDestRect,
                           HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion, DWORD dwFlags);
  template <typename SerialiserType>
  bool Serialise_SwapChain_Present(SerialiserType &ser, IDirect3DSwapChain9 *pSwapChain,
                                   CONST RECT *pSourceRect, CONST RECT *pDestRect,
                                   HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion,
                                   DWORD dwFlags);

  /*** IDirect3DDevice9 methods ***/
  virtual HRESULT __stdcall TestCooperativeLevel();
  virtual UINT __stdcall GetAvailableTextureMem();
  virtual HRESULT __stdcall EvictManagedResources();
  virtual HRESULT __stdcall GetDirect3D(IDirect3D9 **ppD3D9);
  virtual HRESULT __stdcall GetDeviceCaps(D3DCAPS9 *pCaps);
  virtual HRESULT __stdcall GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode);
  virtual HRESULT __stdcall GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters);
  virtual HRESULT __stdcall SetCursorProperties(UINT XHotSpot, UINT YHotSpot,
                                                IDirect3DSurface9 *pCursorBitmap);
  virtual void __stdcall SetCursorPosition(int X, int Y, DWORD Flags);
  virtual BOOL __stdcall ShowCursor(BOOL bShow);
  virtual HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters,
                                                      IDirect3DSwapChain9 **pSwapChain);
  virtual HRESULT __stdcall GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain);
  virtual UINT __stdcall GetNumberOfSwapChains();
  virtual HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS *pPresentationParameters);
  virtual HRESULT __stdcall Present(CONST RECT *pSourceRect, CONST RECT *pDestRect,
                                    HWND hDestWindow, CONST RGNDATA *pDirtyRegion);

  virtual HRESULT __stdcall GetBackBuffer(UINT iSwapChain, UINT iBackBuffer,
                                          D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer);
  virtual HRESULT __stdcall GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus);
  virtual HRESULT __stdcall SetDialogBoxMode(BOOL bEnableDialogs);
  virtual void __stdcall SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP *pRamp);
  virtual void __stdcall GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp);
  virtual HRESULT __stdcall CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
                                          D3DFORMAT Format, D3DPOOL Pool,
                                          IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle);
  virtual HRESULT __stdcall CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels,
                                                DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                                IDirect3DVolumeTexture9 **ppVolumeTexture,
                                                HANDLE *pSharedHandle);
  virtual HRESULT __stdcall CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage,
                                              D3DFORMAT Format, D3DPOOL Pool,
                                              IDirect3DCubeTexture9 **ppCubeTexture,
                                              HANDLE *pSharedHandle);
  virtual HRESULT __stdcall CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                               IDirect3DVertexBuffer9 **ppVertexBuffer,
                                               HANDLE *pSharedHandle);
  virtual HRESULT __stdcall CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format,
                                              D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer,
                                              HANDLE *pSharedHandle);
  virtual HRESULT __stdcall CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format,
                                               D3DMULTISAMPLE_TYPE MultiSample,
                                               DWORD MultisampleQuality, BOOL Lockable,
                                               IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle);
  virtual HRESULT __stdcall CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                      D3DMULTISAMPLE_TYPE MultiSample,
                                                      DWORD MultisampleQuality, BOOL Discard,
                                                      IDirect3DSurface9 **ppSurface,
                                                      HANDLE *pSharedHandle);
  virtual HRESULT __stdcall UpdateSurface(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect,
                                          IDirect3DSurface9 *pDestinationSurface,
                                          CONST POINT *pDestPoint);
  virtual HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture,
                                          IDirect3DBaseTexture9 *pDestinationTexture);
  virtual HRESULT __stdcall GetRenderTargetData(IDirect3DSurface9 *pRenderTarget,
                                                IDirect3DSurface9 *pDestSurface);
  virtual HRESULT __stdcall GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface);
  virtual HRESULT __stdcall StretchRect(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect,
                                        IDirect3DSurface9 *pDestSurface, CONST RECT *pDestRect,
                                        D3DTEXTUREFILTERTYPE Filter);
  virtual HRESULT __stdcall ColorFill(IDirect3DSurface9 *pSurface, CONST RECT *pRect, D3DCOLOR color);
  virtual HRESULT __stdcall CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                        D3DPOOL Pool, IDirect3DSurface9 **ppSurface,
                                                        HANDLE *pSharedHandle);
  virtual HRESULT __stdcall SetRenderTarget(DWORD RenderTargetIndex,
                                            IDirect3DSurface9 *pRenderTarget);
  virtual HRESULT __stdcall GetRenderTarget(DWORD RenderTargetIndex,
                                            IDirect3DSurface9 **ppRenderTarget);
  virtual HRESULT __stdcall SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil);
  virtual HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface);
  virtual HRESULT __stdcall BeginScene();
  virtual HRESULT __stdcall EndScene();
  virtual HRESULT __stdcall Clear(DWORD Count, CONST D3DRECT *pRects, DWORD Flags, D3DCOLOR Color,
                                  float Z, DWORD Stencil);
  virtual HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix);
  virtual HRESULT __stdcall GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix);
  virtual HRESULT __stdcall MultiplyTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX *);
  virtual HRESULT __stdcall SetViewport(CONST D3DVIEWPORT9 *pViewport);
  virtual HRESULT __stdcall GetViewport(D3DVIEWPORT9 *pViewport);
  virtual HRESULT __stdcall SetMaterial(CONST D3DMATERIAL9 *pMaterial);
  virtual HRESULT __stdcall GetMaterial(D3DMATERIAL9 *pMaterial);
  virtual HRESULT __stdcall SetLight(DWORD Index, CONST D3DLIGHT9 *);
  virtual HRESULT __stdcall GetLight(DWORD Index, D3DLIGHT9 *);
  virtual HRESULT __stdcall LightEnable(DWORD Index, BOOL Enable);
  virtual HRESULT __stdcall GetLightEnable(DWORD Index, BOOL *pEnable);
  virtual HRESULT __stdcall SetClipPlane(DWORD Index, CONST float *pPlane);
  virtual HRESULT __stdcall GetClipPlane(DWORD Index, float *pPlane);
  virtual HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE State, DWORD Value);
  virtual HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue);
  virtual HRESULT __stdcall CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB);
  virtual HRESULT __stdcall BeginStateBlock();
  virtual HRESULT __stdcall EndStateBlock(IDirect3DStateBlock9 **ppSB);
  virtual HRESULT __stdcall SetClipStatus(CONST D3DCLIPSTATUS9 *pClipStatus);
  virtual HRESULT __stdcall GetClipStatus(D3DCLIPSTATUS9 *pClipStatus);
  virtual HRESULT __stdcall GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture);
  virtual HRESULT __stdcall SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture);
  virtual HRESULT __stdcall GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                                                 DWORD *pValue);
  virtual HRESULT __stdcall SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                                                 DWORD Value);
  virtual HRESULT __stdcall GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue);
  virtual HRESULT __stdcall SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value);
  virtual HRESULT __stdcall ValidateDevice(DWORD *pNumPasses);
  virtual HRESULT __stdcall SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY *pEntries);
  virtual HRESULT __stdcall GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries);
  virtual HRESULT __stdcall SetCurrentTexturePalette(UINT PaletteNumber);
  virtual HRESULT __stdcall GetCurrentTexturePalette(UINT *PaletteNumber);
  virtual HRESULT __stdcall SetScissorRect(CONST RECT *pRect);
  virtual HRESULT __stdcall GetScissorRect(RECT *pRect);
  virtual HRESULT __stdcall SetSoftwareVertexProcessing(BOOL bSoftware);
  virtual BOOL __stdcall GetSoftwareVertexProcessing();
  virtual HRESULT __stdcall SetNPatchMode(float nSegments);
  virtual float __stdcall GetNPatchMode();
  virtual HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                          UINT PrimitiveCount);
  virtual HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT BaseVertexIndex,
                                                 UINT MinVertexIndex, UINT NumVertices,
                                                 UINT startIndex, UINT primCount);
  virtual HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                            CONST void *pVertexStreamZeroData,
                                            UINT VertexStreamZeroStride);
  virtual HRESULT __stdcall DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,
                                                   UINT MinVertexIndex, UINT NumVertices,
                                                   UINT PrimitiveCount, CONST void *pIndexData,
                                                   D3DFORMAT IndexDataFormat,
                                                   CONST void *pVertexStreamZeroData,
                                                   UINT VertexStreamZeroStride);
  virtual HRESULT __stdcall ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
                                            IDirect3DVertexBuffer9 *pDestBuffer,
                                            IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags);
  virtual HRESULT __stdcall CreateVertexDeclaration(CONST D3DVERTEXELEMENT9 *pVertexElements,
                                                    IDirect3DVertexDeclaration9 **ppDecl);
  virtual HRESULT __stdcall SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl);
  virtual HRESULT __stdcall GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl);
  virtual HRESULT __stdcall SetFVF(DWORD FVF);
  virtual HRESULT __stdcall GetFVF(DWORD *pFVF);
  virtual HRESULT __stdcall CreateVertexShader(CONST DWORD *pFunction,
                                               IDirect3DVertexShader9 **ppShader);
  virtual HRESULT __stdcall SetVertexShader(IDirect3DVertexShader9 *pShader);
  virtual HRESULT __stdcall GetVertexShader(IDirect3DVertexShader9 **ppShader);
  virtual HRESULT __stdcall SetVertexShaderConstantF(UINT StartRegister, CONST float *pConstantData,
                                                     UINT Vector4fCount);
  virtual HRESULT __stdcall GetVertexShaderConstantF(UINT StartRegister, float *pConstantData,
                                                     UINT Vector4fCount);
  virtual HRESULT __stdcall SetVertexShaderConstantI(UINT StartRegister, CONST int *pConstantData,
                                                     UINT Vector4iCount);
  virtual HRESULT __stdcall GetVertexShaderConstantI(UINT StartRegister, int *pConstantData,
                                                     UINT Vector4iCount);
  virtual HRESULT __stdcall SetVertexShaderConstantB(UINT StartRegister, CONST BOOL *pConstantData,
                                                     UINT BoolCount);
  virtual HRESULT __stdcall GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData,
                                                     UINT BoolCount);
  virtual HRESULT __stdcall SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData,
                                            UINT OffsetInBytes, UINT Stride);
  virtual HRESULT __stdcall GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData,
                                            UINT *pOffsetInBytes, UINT *pStride);
  virtual HRESULT __stdcall SetStreamSourceFreq(UINT StreamNumber, UINT Setting);
  virtual HRESULT __stdcall GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting);
  virtual HRESULT __stdcall SetIndices(IDirect3DIndexBuffer9 *pIndexData);
  virtual HRESULT __stdcall GetIndices(IDirect3DIndexBuffer9 **ppIndexData);
  virtual HRESULT __stdcall CreatePixelShader(CONST DWORD *pFunction,
                                              IDirect3DPixelShader9 **ppShader);
  virtual HRESULT __stdcall SetPixelShader(IDirect3DPixelShader9 *pShader);
  virtual HRESULT __stdcall GetPixelShader(IDirect3DPixelShader9 **ppShader);
  virtual HRESULT __stdcall SetPixelShaderConstantF(UINT StartRegister, CONST float *pConstantData,
                                                    UINT Vector4fCount);
  virtual HRESULT __stdcall GetPixelShaderConstantF(UINT StartRegister, float *pConstantData,
                                                    UINT Vector4fCount);
  virtual HRESULT __stdcall SetPixelShaderConstantI(UINT StartRegister, CONST int *pConstantData,
                                                    UINT Vector4iCount);
  virtual HRESULT __stdcall GetPixelShaderConstantI(UINT StartRegister, int *pConstantData,
                                                    UINT Vector4iCount);
  virtual HRESULT __stdcall SetPixelShaderConstantB(UINT StartRegister, CONST BOOL *pConstantData,
                                                    UINT BoolCount);
  virtual HRESULT __stdcall GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData,
                                                    UINT BoolCount);
  virtual HRESULT __stdcall DrawRectPatch(UINT Handle, CONST float *pNumSegs,
                                          CONST D3DRECTPATCH_INFO *pRectPatchInfo);
  virtual HRESULT __stdcall DrawTriPatch(UINT Handle, CONST float *pNumSegs,
                                         CONST D3DTRIPATCH_INFO *pTriPatchInfo);
  virtual HRESULT __stdcall DeletePatch(UINT Handle);
  virtual HRESULT __stdcall CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery);

  /*** IDirect3DDevice9Ex methods ***/
  virtual HRESULT __stdcall SetConvolutionMonoKernel(UINT width, UINT height, float *rows,
                                                     float *columns);
  virtual HRESULT __stdcall ComposeRects(IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst,
                                         IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects,
                                         IDirect3DVertexBuffer9 *pDstRectDescs,
                                         D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset);
  virtual HRESULT __stdcall PresentEx(CONST RECT *pSourceRect, CONST RECT *pDestRect,
                                      HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion,
                                      DWORD dwFlags);
  virtual HRESULT __stdcall GetGPUThreadPriority(INT *pPriority);
  virtual HRESULT __stdcall SetGPUThreadPriority(INT Priority);
  virtual HRESULT __stdcall WaitForVBlank(UINT iSwapChain);
  virtual HRESULT __stdcall CheckResourceResidency(IDirect3DResource9 **pResourceArray,
                                                   UINT32 NumResources);
  virtual HRESULT __stdcall SetMaximumFrameLatency(UINT MaxLatency);
  virtual HRESULT __stdcall GetMaximumFrameLatency(UINT *pMaxLatency);
  virtual HRESULT __stdcall CheckDeviceState(HWND hDestinationWindow);
  virtual HRESULT __stdcall CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format,
                                                 D3DMULTISAMPLE_TYPE MultiSample,
                                                 DWORD MultisampleQuality, BOOL Lockable,
                                                 IDirect3DSurface9 **ppSurface,
                                                 HANDLE *pSharedHandle, DWORD Usage);
  virtual HRESULT __stdcall CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format,
                                                          D3DPOOL Pool, IDirect3DSurface9 **ppSurface,
                                                          HANDLE *pSharedHandle, DWORD Usage);
  virtual HRESULT __stdcall CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format,
                                                        D3DMULTISAMPLE_TYPE MultiSample,
                                                        DWORD MultisampleQuality, BOOL Discard,
                                                        IDirect3DSurface9 **ppSurface,
                                                        HANDLE *pSharedHandle, DWORD Usage);
  virtual HRESULT __stdcall ResetEx(D3DPRESENT_PARAMETERS *pPresentationParameters,
                                    D3DDISPLAYMODEEX *pFullscreenDisplayMode);
  virtual HRESULT __stdcall GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX *pMode,
                                             D3DDISPLAYROTATION *pRotation);

private:
  bool ProcessChunk(ReadSerialiser &ser, D3D9Chunk chunk);

  //////////////////////////////
  // replay, see d3d9_replay_device.cpp

  bool ContextProcessChunk(ReadSerialiser &ser, D3D9Chunk chunk);
  RDResult ContextReplayLog(CaptureState readType, uint32_t startEventID, uint32_t endEventID,
                            bool partial);
  void AddEvent();
  void AddAction(const ActionDescription &a);
  const APIEvent &GetEvent(uint32_t eventId);
  void AddReplayResource(ResourceId id, ResourceType type, const char *prefix);
  void DerivedReplayResource(ResourceId parent, ResourceId child);

  // Creates an object during replay with its captured ID. create(pool, usage) calls the wrapped
  // creation function. D3D9Ex devices have no managed pool, so those objects go in the default
  // pool, and textures that can't be rendered to are made dynamic where possible so they can be
  // locked to write their contents.
  template <typename CreateFunc>
  HRESULT ReplayCreate(ResourceId id, D3DPOOL pool, DWORD usage, bool texture, CreateFunc create);
  bool CheckReplayCreate(HRESULT hr, IUnknown *obj, ResourceId id, const char *what);
  // children of other objects (texture levels, back buffers) created on first use
  template <typename GetFunc>
  void ReplayGetChild(ResourceId parent, ResourceId child, ResourceType type, const char *prefix,
                      GetFunc get);
  void PatchReplayPresentParams(D3DPRESENT_PARAMETERS &params);
  // draws must be in a scene, which the replayed range might have started outside of
  void ReplayBeginScene();
  void ReplayEndScene();
  void ApplyReplayDeviceState(const D3D9DeviceState &state);
  void AddDrawAction(ActionFlags flags, uint32_t numIndices, uint32_t indexOffset,
                     int32_t baseVertex, uint32_t vertexOffset);
  void AddCopyAction(ActionFlags flags, IUnknown *source, IUnknown *dest);
  ResourceId ContentsTextureID(IUnknown *obj);
  // the first back buffer of a swapchain, described as a resource even if the capture never used it
  ResourceId ReplayBackBufferID(IDirect3DSwapChain9 *swap);
  rdcstr WriteSurface(IDirect3DSurface9 *surf, const D3D9SubresourceData &sub);
  rdcstr WriteVolume(IDirect3DVolume9 *vol, const D3D9SubresourceData &sub);
  // writing depth stencils that can't be locked, see d3d9_depth.cpp
  rdcstr WriteDepthStencil(IDirect3DSurface9 *realDS, const D3D9SubresourceData &sub);
  bool InitDepthWrite(rdcstr &err);

  // state block contents, see d3d9_contents.h
  void ReadStateBlock(WrappedD3DStateBlock9 *sb, D3D9InitialContentsData &contents);
  // set the real device's object bindings to the ones we track, fvf being the FVF to set if the
  // vertex declaration came from one
  void RestoreRealBindings(DWORD fvf);
  void MarkStateBlockReferenced(WrappedD3DStateBlock9 *sb);

  // reading depth stencils that can't be locked, see d3d9_depth.cpp. note describes how the
  // contents are laid out, when they were read.
  rdcstr ReadDepthStencil(IDirect3DSurface9 *realDS, D3D9SubresourceData &sub, rdcstr &note);
  bool InitDepthReadback(rdcstr &err);

  // the shaders that draw for ReadDepthStencil, on the real device, created on first use
  struct DepthReadback
  {
    bool tried = false;
    // why they couldn't be created
    rdcstr error;
    IDirect3DVertexShader9 *vs = NULL;
    IDirect3DPixelShader9 *depthTest = NULL;
    IDirect3DPixelShader9 *stencilTest = NULL;
    IDirect3DPixelShader9 *combine = NULL;
    IDirect3DVertexDeclaration9 *decl = NULL;
    // for writing depth stencils back at replay
    bool triedWrite = false;
    rdcstr writeError;
    IDirect3DPixelShader9 *depthWrite = NULL;
    IDirect3DPixelShader9 *stencilWrite = NULL;

    void Release();
  } m_DepthReadback;

  // State calls are recorded into the frame, and also while a state block is being recorded: they
  // become part of the state block's creation, since that's what decides which states it covers.
  bool ShouldRecordState() const { return IsActiveCapturing(m_State) || m_RecordingRecord != NULL; }
  void AddStateChunk(Chunk *chunk);
  void FreeRecordingRecord();
  bool ProcessUnlockChunk(ReadSerialiser &ser, D3D9Chunk chunk);

  template <typename SerialiserType>
  bool Serialise_CaptureScope(SerialiserType &ser);

  // frame-reference objects used by the call being recorded
  void MarkReferenced(IUnknown *obj, FrameRefType refType);
  void MarkDrawReferenced();
  void MarkRenderTargetsReferenced();

  void ThumbnailFromSwapChain(IDirect3DSwapChain9 *realSwapChain, RenderDoc::FramePixels &fp);

  // the presentation parameters of each implicit swapchain, kept up to date across Reset
  rdcarray<D3DPRESENT_PARAMETERS> GetImplicitPresentParams();
  rdcarray<ResourceId> GetImplicitSwapChainIDs();

  // shared setup between the constructors
  void InitCapture();
  // a record with no chunks for objects created implicitly by the device
  void CreateOwnedRecord(WrappedD3D9Object *obj);
  void FlushDeferredRecordReleases();

  // a lost (or hung or removed) device drops everything it's asked to do, so a frame captured
  // while it's lost is useless. wnd is the window being presented to, for D3D9Ex devices.
  bool IsDeviceLost(HWND wnd);
  // Reset discards a capture in progress: the frame's objects in D3DPOOL_DEFAULT are gone. Returns
  // whether a capture was discarded, and whether it was application controlled.
  bool DiscardCaptureForReset(bool &appControlled);
  void AfterResetDiscard(bool appControlled, bool resetSucceeded);

  // draw the in-application overlay into a real swapchain's first back buffer
  void RenderOverlay(IDirect3DSwapChain9 *realSwapChain, DeviceOwnedWindow devWnd);

  // wrap the objects the device creates and owns: implicit swapchains and the automatic depth
  // stencil surface, then set up the initial render target bindings to match the real device.
  void InitDeviceOwnedObjects();
  void WrapAutoDepthStencil();
  // after a successful Reset/ResetEx: all state returns to defaults and device-owned objects may
  // have been recreated.
  void OnReset();

  // Set* calls for state-block state go to the state block being recorded if there is one,
  // otherwise to the device's bindings.
  D3D9ObjectBindings &BindingTarget() { return m_Recording ? *m_Recording : m_Bindings; }

  IDirect3DDevice9 *m_device;
  // no reference held, it's the same object as m_device
  IDirect3DDevice9Ex *m_deviceEx = NULL;

  WrappedD3D9 *m_D3D;
  D3D9DebugManager *m_DebugManager = NULL;
  D3D9ResourceManager *m_ResourceManager = NULL;
  CaptureState m_State;
  Threading::CriticalSection m_D3DLock;

  int32_t m_RefCount = 1;

  // device-owned objects, each holding an internal reference
  rdcarray<WrappedD3DSwapChain9 *> m_ImplicitSwapChains;
  WrappedD3DSurface9 *m_AutoDepthStencil = NULL;

  // current bindings (wrapped objects, each holding an internal reference)
  D3D9ObjectBindings m_Bindings;
  IDirect3DSurface9 *m_RenderTargets[D3D9_NUM_RENDER_TARGETS] = {};
  IDirect3DSurface9 *m_DepthStencil = NULL;

  // between BeginStateBlock and EndStateBlock
  D3D9ObjectBindings *m_Recording = NULL;
  D3D9BindingMask m_RecordingMask;
  // holds the BeginStateBlock chunk and the state calls made since, while capturing is possible.
  // EndStateBlock makes it a parent of the new state block's record, so its chunks come before the
  // EndStateBlock chunk.
  D3D9ResourceRecord *m_RecordingRecord = NULL;

  // the light and palette indices the application has used, protected by m_D3DLock
  D3D9IndexSet m_LightIndices;
  D3D9IndexSet m_PaletteNumbers;

  HWND m_Wnd = NULL;
  uint32_t m_FrameCounter = 0;

  // capture
  D3D9InitParams m_InitParams;
  uint64_t m_SectionVersion = D3D9InitParams::CurrentVersion;
  WriteSerialiser m_ScratchSerialiser;
  ResourceId m_DeviceResourceID;
  // holds the chunks for a frame being captured
  D3D9ResourceRecord *m_FrameRecord = NULL;
  rdcarray<D3D9ResourceRecord *> m_DeferredRecordReleases;
  rdcarray<FrameDescription> m_CapturedFrames;
  bool m_AppControlledCapture = false;
  // the window the capture in progress was started for
  DeviceOwnedWindow m_CaptureWindow;
  // the non-object state when the frame being captured started
  D3D9DeviceState m_FrameStartState;

  // structured export
  std::set<rdcstr> m_StringDB;
  SDFile *m_StructuredFile = NULL;
  SDFile *m_StoredStructuredData = NULL;

  // replay
  D3D9Replay *m_Replay = NULL;
  HWND m_ReplayWindow = NULL;
  ResourceId m_NextWrapperID;
  // the objects replay created, holding the reference the creation returned
  rdcarray<IUnknown *> m_ReplayObjects;
  // the capture's implicit swapchains, created as additional swapchains
  rdcarray<IDirect3DSwapChain9 *> m_ReplaySwapChains;
  bool m_ReplayInScene = false;
  RDResult m_FailedReplayResult = ResultCode::Succeeded;
  uint64_t m_TimeBase = 0;
  double m_TimeFrequency = 1.0;
  StreamReader *m_FrameReader = NULL;
  uint32_t m_CurEventID = 0;
  uint32_t m_CurActionID = 0;
  uint32_t m_FirstEventID = 0;
  uint32_t m_LastEventID = ~0U;
  uint64_t m_CurChunkOffset = 0;
  D3D9Chunk m_LastChunk = D3D9Chunk::Max;
  bool m_AddedAction = false;
  rdcarray<APIEvent> m_CurEvents;
  rdcarray<APIEvent> m_Events;
  ActionDescription m_ParentAction;
  rdcarray<ActionDescription *> m_ActionStack;
  rdcarray<ActionDescription *> m_Actions;
};

template <typename CreateFunc>
HRESULT WrappedD3DDevice9::ReplayCreate(ResourceId id, D3DPOOL pool, DWORD usage, bool texture,
                                        CreateFunc create)
{
  if(pool == D3DPOOL_MANAGED)
    pool = D3DPOOL_DEFAULT;

  HRESULT hr = E_FAIL;

  const DWORD notLockable =
      D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC | D3DUSAGE_AUTOGENMIPMAP;
  if(texture && pool == D3DPOOL_DEFAULT && (usage & notLockable) == 0)
  {
    m_NextWrapperID = id;
    hr = create(pool, usage | D3DUSAGE_DYNAMIC);
  }

  if(FAILED(hr))
  {
    m_NextWrapperID = id;
    hr = create(pool, usage);
  }

  m_NextWrapperID = ResourceId();
  return hr;
}

template <typename GetFunc>
void WrappedD3DDevice9::ReplayGetChild(ResourceId parent, ResourceId child, ResourceType type,
                                       const char *prefix, GetFunc get)
{
  m_NextWrapperID = child;
  IUnknown *obj = get();
  m_NextWrapperID = ResourceId();

  if(obj == NULL)
  {
    RDCERR("Couldn't get %s %s of %s", prefix, ToStr(child).c_str(), ToStr(parent).c_str());
    return;
  }

  WrappedD3D9Object *wrapped = GetWrappedObject(obj);
  if(wrapped && wrapped->GetResourceID() != child)
    RDCWARN("%s %s already existed as %s", prefix, ToStr(child).c_str(),
            ToStr(wrapped->GetResourceID()).c_str());

  // the container owns its children
  obj->Release();

  AddReplayResource(child, type, prefix);
  DerivedReplayResource(parent, child);
}

template <typename SerialiseFunc>
void WrappedD3DDevice9::RecordCreation(WrappedD3D9Object *obj, D3D9Chunk chunk,
                                       SerialiseFunc serialise, WrappedD3D9Object *parent)
{
  if(obj == NULL || !IsCaptureMode(m_State))
    return;

  SCOPED_LOCK(m_D3DLock);

  D3D9ResourceRecord *record = m_ResourceManager->AddResourceRecord(obj->GetResourceID());
  record->DataInSerialiser = false;
  record->Length = 0;

  if(parent && parent->GetResourceRecord())
    record->AddParent(parent->GetResourceRecord());

  {
    WriteSerialiser &ser = m_ScratchSerialiser;
    SCOPED_SERIALISE_CHUNK(chunk);
    serialise(ser);
    record->AddChunk(scope.Get());
  }

  obj->SetResourceRecord(record);

  // anything could happen to the contents before a capture, so they always need reading
  if(D3D9HasContents(obj))
    m_ResourceManager->MarkDirtyResource(obj->GetResourceID());
}

////////////////////////////////////////////////////////////////////////////
// WrappedD3D9

class WrappedD3D9 final : public IDirect3D9Ex
{
public:
  WrappedD3D9(IDirect3D9 *direct3D9);
  /*** IUnknown methods ***/
  virtual HRESULT __stdcall QueryInterface(REFIID riid, void **ppvObj);
  virtual ULONG __stdcall AddRef();
  virtual ULONG __stdcall Release();

  /*** IDirect3D9 methods ***/
  virtual HRESULT __stdcall RegisterSoftwareDevice(void *pInitializeFunction);
  virtual UINT __stdcall GetAdapterCount();
  virtual HRESULT __stdcall GetAdapterIdentifier(UINT Adapter, DWORD Flags,
                                                 D3DADAPTER_IDENTIFIER9 *pIdentifier);
  virtual UINT __stdcall GetAdapterModeCount(UINT Adapter, D3DFORMAT Format);
  virtual HRESULT __stdcall EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode,
                                             D3DDISPLAYMODE *pMode);
  virtual HRESULT __stdcall GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode);
  virtual HRESULT __stdcall CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat,
                                            D3DFORMAT BackBufferFormat, BOOL bWindowed);
  virtual HRESULT __stdcall CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType,
                                              D3DFORMAT AdapterFormat, DWORD Usage,
                                              D3DRESOURCETYPE RType, D3DFORMAT CheckFormat);
  virtual HRESULT __stdcall CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType,
                                                       D3DFORMAT SurfaceFormat, BOOL Windowed,
                                                       D3DMULTISAMPLE_TYPE MultiSampleType,
                                                       DWORD *pQualityLevels);
  virtual HRESULT __stdcall CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType,
                                                   D3DFORMAT AdapterFormat,
                                                   D3DFORMAT RenderTargetFormat,
                                                   D3DFORMAT DepthStencilFormat);
  virtual HRESULT __stdcall CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType,
                                                        D3DFORMAT SourceFormat,
                                                        D3DFORMAT TargetFormat);
  virtual HRESULT __stdcall GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps);
  virtual HMONITOR __stdcall GetAdapterMonitor(UINT Adapter);
  virtual HRESULT __stdcall CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                         DWORD BehaviorFlags,
                                         D3DPRESENT_PARAMETERS *pPresentationParameters,
                                         IDirect3DDevice9 **ppReturnedDeviceInterface);

  virtual UINT __stdcall GetAdapterModeCountEx(UINT Adapter, CONST D3DDISPLAYMODEFILTER *pFilter);
  virtual HRESULT __stdcall EnumAdapterModesEx(UINT Adapter, CONST D3DDISPLAYMODEFILTER *pFilter,
                                               UINT Mode, D3DDISPLAYMODEEX *pMode);
  virtual HRESULT __stdcall GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode,
                                                    D3DDISPLAYROTATION *pRotation);
  virtual HRESULT __stdcall CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                           DWORD BehaviorFlags,
                                           D3DPRESENT_PARAMETERS *pPresentationParameters,
                                           D3DDISPLAYMODEEX *pFullscreenDisplayMode,
                                           IDirect3DDevice9Ex **ppReturnedDeviceInterface);
  virtual HRESULT __stdcall GetAdapterLUID(UINT Adapter, LUID *pLUID);

private:
  IDirect3D9 *m_Direct3D;
  // no reference held, it's the same object as m_Direct3D. NULL if created with Direct3DCreate9
  IDirect3D9Ex *m_Direct3DEx = NULL;

  int32_t m_RefCount = 1;
};
