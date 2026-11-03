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

#include "d3d9_debug.h"
#include "os/os_specific.h"
#include "stb/stb_truetype.h"
#include "strings/string_utils.h"

D3D9DebugManager::D3D9DebugManager(WrappedD3DDevice9 *wrapper)
    : m_fvf(D3DFVF_XYZ | D3DFVF_TEX1), m_Device(wrapper->GetReal()), m_IsEx(wrapper->IsExDevice())
{
  InitFontRendering();
}

D3D9DebugManager::~D3D9DebugManager()
{
  ShutdownFontRendering();
}

bool D3D9DebugManager::InitFontRendering()
{
  HRESULT hr = S_OK;

  int width = FONT_TEX_WIDTH;
  int height = FONT_TEX_HEIGHT;

  rdcstr font = GetEmbeddedResource(sourcecodepro_ttf);
  byte *ttfdata = (byte *)font.c_str();

  const int firstChar = int(' ') + 1;
  const int lastChar = 127;
  const int numChars = lastChar - firstChar;

  byte *buf = new byte[width * height];

  const float pixelHeight = 20.0f;

  stbtt_BakeFontBitmap(ttfdata, 0, pixelHeight, buf, width, height, firstChar, numChars,
                       m_Font.charData);

  stbtt_fontinfo f = {0};
  stbtt_InitFont(&f, ttfdata, 0);

  int ascent = 0;
  stbtt_GetFontVMetrics(&f, &ascent, NULL, NULL);

  m_Font.maxHeight = float(ascent) * stbtt_ScaleForPixelHeight(&f, pixelHeight);

  IDirect3DTexture9 *fontTex = NULL;

  // A D3DPOOL_DEFAULT texture would make the application's Reset fail on non-Ex devices, so use the
  // managed pool there. Ex devices don't support the managed pool, but don't need it either.
  hr = m_Device->CreateTexture(width, height, 1, m_IsEx ? D3DUSAGE_DYNAMIC : 0, D3DFMT_A8R8G8B8,
                               m_IsEx ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &fontTex, NULL);

  if(FAILED(hr) || fontTex == NULL)
  {
    RDCERR("Failed to create font texture HRESULT: %s", ToStr(hr).c_str());
    delete[] buf;
    return false;
  }

  D3DLOCKED_RECT lockedRegion;
  hr = fontTex->LockRect(0, &lockedRegion, NULL, m_IsEx ? D3DLOCK_DISCARD : 0);

  if(FAILED(hr))
  {
    RDCERR("Failed to lock font texture HRESULT: %s", ToStr(hr).c_str());
  }
  else
  {
    BYTE *texBase = (BYTE *)lockedRegion.pBits;

    for(int y = 0; y < height; y++)
    {
      byte *curRow = (texBase + (y * lockedRegion.Pitch));

      for(int x = 0; x < width; x++)
      {
        curRow[x * 4 + 0] = buf[(y * width) + x];
        curRow[x * 4 + 1] = buf[(y * width) + x];
        curRow[x * 4 + 2] = buf[(y * width) + x];
        curRow[x * 4 + 3] = buf[(y * width) + x];
      }
    }

    hr = fontTex->UnlockRect(0);
    if(hr != S_OK)
    {
      RDCERR("Failed to unlock font texture HRESULT: %s", ToStr(hr).c_str());
    }
  }

  m_Font.Tex = fontTex;

  delete[] buf;

  return true;
}

void D3D9DebugManager::ShutdownFontRendering()
{
  SAFE_RELEASE(m_Font.Tex);
}

void D3D9DebugManager::SetOutputWindow(HWND w)
{
  // RECT rect;
  // GetClientRect(w, &rect);
  // m_supersamplingX = float(m_width) / float(rect.right - rect.left);
  // m_supersamplingY = float(m_height) / float(rect.bottom - rect.top);
}

namespace
{
struct RenderStateValue
{
  D3DRENDERSTATETYPE state;
  DWORD value;
};

struct SamplerStateValue
{
  D3DSAMPLERSTATETYPE state;
  DWORD value;
};

struct TextureStageStateValue
{
  DWORD stage;
  D3DTEXTURESTAGESTATETYPE state;
  DWORD value;
};

// every state the text rendering sets. RenderText saves each one first and restores it after, so
// nothing else about the application's state is touched. In particular no state block is used:
// applying a D3DSBT_ALL state block has been seen to copy shader constants c0-c127 over c128-c255
// (natively, on NVIDIA), which corrupted any application using the upper constants every frame.
const RenderStateValue overlayRenderStates[] = {
    {D3DRS_ZENABLE, D3DZB_FALSE},
    {D3DRS_ZWRITEENABLE, FALSE},
    {D3DRS_LIGHTING, FALSE},
    {D3DRS_STENCILENABLE, FALSE},
    {D3DRS_CLIPPLANEENABLE, FALSE},
    {D3DRS_ALPHATESTENABLE, FALSE},
    {D3DRS_CLIPPING, FALSE},
    {D3DRS_SCISSORTESTENABLE, FALSE},
    {D3DRS_FOGENABLE, FALSE},
    {D3DRS_SEPARATEALPHABLENDENABLE, FALSE},
    {D3DRS_COLORWRITEENABLE, 0x0000000F},
    {D3DRS_CULLMODE, D3DCULL_NONE},
    {D3DRS_FILLMODE, D3DFILL_SOLID},
    {D3DRS_WRAP0, 0},
    {D3DRS_BLENDOP, D3DBLENDOP_ADD},
    {D3DRS_VERTEXBLEND, D3DVBF_DISABLE},
    {D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE},
    {D3DRS_SRGBWRITEENABLE, FALSE},
    {D3DRS_ALPHABLENDENABLE, FALSE},
    {D3DRS_SRCBLEND, D3DBLEND_SRCALPHA},
    {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA},
};

const SamplerStateValue overlaySamplerStates[] = {
    {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
    {D3DSAMP_MINFILTER, D3DTEXF_LINEAR},   {D3DSAMP_MAGFILTER, D3DTEXF_LINEAR},
    {D3DSAMP_MIPFILTER, D3DTEXF_LINEAR},   {D3DSAMP_SRGBTEXTURE, 0},
};

const TextureStageStateValue overlayTextureStageStates[] = {
    {0, D3DTSS_COLOROP, D3DTOP_MODULATE},
    {0, D3DTSS_COLORARG1, D3DTA_TEXTURE},
    {0, D3DTSS_COLORARG2, D3DTA_CURRENT},
    {0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1},
    {0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE},
    {0, D3DTSS_ALPHAARG2, D3DTA_CURRENT},
    {0, D3DTSS_TEXCOORDINDEX, 0},
    {0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE},
    {0, D3DTSS_COLORARG0, D3DTA_CURRENT},
    {0, D3DTSS_ALPHAARG0, D3DTA_CURRENT},
    {0, D3DTSS_RESULTARG, D3DTA_CURRENT},
    {0, D3DTSS_CONSTANT, 0xffffffff},
    // only stage 0 is used
    {1, D3DTSS_COLOROP, D3DTOP_DISABLE},
    {1, D3DTSS_ALPHAOP, D3DTOP_DISABLE},
};

const D3DTRANSFORMSTATETYPE overlayTransforms[] = {D3DTS_PROJECTION, D3DTS_WORLD, D3DTS_VIEW};

const DWORD overlayTextureStages = 8;
}

// the state RenderText changes, read back from the device before it's changed
struct D3D9DebugManager::SavedState
{
  DWORD renderStates[ARRAY_COUNT(overlayRenderStates)] = {};
  DWORD samplerStates[ARRAY_COUNT(overlaySamplerStates)] = {};
  DWORD textureStageStates[ARRAY_COUNT(overlayTextureStageStates)] = {};
  D3DMATRIX transforms[ARRAY_COUNT(overlayTransforms)] = {};
  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  IDirect3DVertexDeclaration9 *decl = NULL;
  DWORD fvf = 0;
  IDirect3DBaseTexture9 *textures[overlayTextureStages] = {};
  // DrawPrimitiveUP unbinds stream 0
  IDirect3DVertexBuffer9 *stream0 = NULL;
  UINT stream0Offset = 0;
  UINT stream0Stride = 0;
};

void D3D9DebugManager::SaveState(SavedState &saved)
{
  for(size_t i = 0; i < ARRAY_COUNT(overlayRenderStates); i++)
    m_Device->GetRenderState(overlayRenderStates[i].state, &saved.renderStates[i]);
  for(size_t i = 0; i < ARRAY_COUNT(overlaySamplerStates); i++)
    m_Device->GetSamplerState(0, overlaySamplerStates[i].state, &saved.samplerStates[i]);
  for(size_t i = 0; i < ARRAY_COUNT(overlayTextureStageStates); i++)
    m_Device->GetTextureStageState(overlayTextureStageStates[i].stage,
                                   overlayTextureStageStates[i].state, &saved.textureStageStates[i]);
  for(size_t i = 0; i < ARRAY_COUNT(overlayTransforms); i++)
    m_Device->GetTransform(overlayTransforms[i], &saved.transforms[i]);

  m_Device->GetVertexShader(&saved.vs);
  m_Device->GetPixelShader(&saved.ps);
  m_Device->GetVertexDeclaration(&saved.decl);
  m_Device->GetFVF(&saved.fvf);
  for(DWORD stage = 0; stage < overlayTextureStages; stage++)
    m_Device->GetTexture(stage, &saved.textures[stage]);
  m_Device->GetStreamSource(0, &saved.stream0, &saved.stream0Offset, &saved.stream0Stride);
}

void D3D9DebugManager::RestoreState(SavedState &saved)
{
  for(size_t i = 0; i < ARRAY_COUNT(overlayRenderStates); i++)
    m_Device->SetRenderState(overlayRenderStates[i].state, saved.renderStates[i]);
  for(size_t i = 0; i < ARRAY_COUNT(overlaySamplerStates); i++)
    m_Device->SetSamplerState(0, overlaySamplerStates[i].state, saved.samplerStates[i]);
  for(size_t i = 0; i < ARRAY_COUNT(overlayTextureStageStates); i++)
    m_Device->SetTextureStageState(overlayTextureStageStates[i].stage,
                                   overlayTextureStageStates[i].state, saved.textureStageStates[i]);
  for(size_t i = 0; i < ARRAY_COUNT(overlayTransforms); i++)
    m_Device->SetTransform(overlayTransforms[i], &saved.transforms[i]);

  m_Device->SetVertexShader(saved.vs);
  m_Device->SetPixelShader(saved.ps);

  // the declaration covers both cases. If the application set an FVF, setting it again afterwards
  // keeps GetFVF returning it.
  m_Device->SetVertexDeclaration(saved.decl);
  if(saved.fvf != 0)
    m_Device->SetFVF(saved.fvf);

  for(DWORD stage = 0; stage < overlayTextureStages; stage++)
    m_Device->SetTexture(stage, saved.textures[stage]);
  m_Device->SetStreamSource(0, saved.stream0, saved.stream0Offset, saved.stream0Stride);

  SAFE_RELEASE(saved.vs);
  SAFE_RELEASE(saved.ps);
  SAFE_RELEASE(saved.decl);
  for(DWORD stage = 0; stage < overlayTextureStages; stage++)
    SAFE_RELEASE(saved.textures[stage]);
  SAFE_RELEASE(saved.stream0);
}

void D3D9DebugManager::RenderText(float x, float y, const rdcstr &text)
{
  if(m_Font.Tex == NULL)
    return;

  SavedState saved;
  SaveState(saved);

  for(size_t i = 0; i < ARRAY_COUNT(overlayRenderStates); i++)
    m_Device->SetRenderState(overlayRenderStates[i].state, overlayRenderStates[i].value);
  for(size_t i = 0; i < ARRAY_COUNT(overlaySamplerStates); i++)
    m_Device->SetSamplerState(0, overlaySamplerStates[i].state, overlaySamplerStates[i].value);
  for(size_t i = 0; i < ARRAY_COUNT(overlayTextureStageStates); i++)
    m_Device->SetTextureStageState(overlayTextureStageStates[i].stage,
                                   overlayTextureStageStates[i].state,
                                   overlayTextureStageStates[i].value);

  // enable fixed function pipeline
  m_Device->SetVertexShader(NULL);
  m_Device->SetPixelShader(NULL);

  m_Device->SetFVF(m_fvf);
  m_Device->SetTexture(0, m_Font.Tex);
  for(DWORD stage = 1; stage < overlayTextureStages; stage++)
    m_Device->SetTexture(stage, NULL);

  rdcarray<rdcstr> lines;
  split(text, lines, '\n');

  for(const rdcstr &line : lines)
  {
    RenderTextInternal(x, y, line);
    y += 1.0f;
  }

  RestoreState(saved);
}

void D3D9DebugManager::RenderTextInternal(float x, float y, const rdcstr &text)
{
  if(text.empty())
    return;

  RDCASSERT(text.size() < FONT_MAX_CHARS);

  // transforms
  float width = (float)m_width;
  float height = (float)m_height;
  float nearPlane = 0.001f;
  float farPlane = 1.f;
  D3DMATRIX identity = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
                        0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
  D3DMATRIX ortho = {2.f / width,
                     0.f,
                     0.f,
                     0.f,
                     0.f,
                     -(2.f / height),
                     0.f,
                     0.f,
                     0.f,
                     0.f,
                     1.f / (farPlane - nearPlane),
                     0.f,
                     0.f,
                     0.f,
                     nearPlane / (nearPlane - farPlane),
                     1.f};

  m_Device->SetTransform(D3DTS_PROJECTION, &ortho);
  m_Device->SetTransform(D3DTS_WORLD, &identity);
  m_Device->SetTransform(D3DTS_VIEW, &identity);

  struct Vertex
  {
    float pos[3];
    float uv[2];

    Vertex() {}
    Vertex(float posx, float posy, float posz, float texu, float texv)
    {
      pos[0] = posx;
      pos[1] = posy;
      pos[2] = posz;
      uv[0] = texu;
      uv[1] = texv;
    }
  };

  struct Quad
  {
    Vertex vertices[6];

    Quad() {}
    Quad(float x0, float y0, float z, float s0, float t0, float x1, float y1, float s1, float t1)
    {
      vertices[0] = Vertex(x0, y0, z, s0, t0);
      vertices[1] = Vertex(x1, y0, z, s1, t0);
      vertices[2] = Vertex(x0, y1, z, s0, t1);
      vertices[3] = Vertex(x1, y0, z, s1, t0);
      vertices[4] = Vertex(x1, y1, z, s1, t1);
      vertices[5] = Vertex(x0, y1, z, s0, t1);
    }
  };

  Quad *quads = NULL;
  Quad background;
  UINT triangleCount = 0;
  {
    UINT quadCount = (UINT)text.size();    // calculate string length

    triangleCount = quadCount * 2;
    // create text VB
    quads = new Quad[quadCount];

    float textStartingPositionX = (-width / 2.f) + (x * m_Font.charData[0].xadvance);
    float textStartingPositionY = (-height / 2.f) + ((y + 1.f) * m_Font.maxHeight);

    float textPositionX = textStartingPositionX;
    float textPositionY = textStartingPositionY;

    for(UINT i = 0; i < quadCount; ++i)
    {
      char glyphIndex = text[i] - (' ' + 1);
      if(glyphIndex < 0)
      {
        float currentX = textPositionX;
        textPositionX += m_Font.charData[0].xadvance;
        quads[i] = Quad(currentX, textPositionY - m_Font.maxHeight, 0.5f, 0.f, 0.f, textPositionX,
                        textPositionY, 0.f, 0.f);
      }
      else
      {
        stbtt_aligned_quad quad;
        stbtt_GetBakedQuad(m_Font.charData, 256, 128, glyphIndex, &textPositionX, &textPositionY,
                           &quad, 0);
        quads[i] = Quad(quad.x0, quad.y0, 0.5f, quad.s0, quad.t0, quad.x1, quad.y1, quad.s1, quad.t1);
      }
    }

    background = Quad(textStartingPositionX, textStartingPositionY - m_Font.maxHeight, 0.6f, 0.f,
                      0.f, textPositionX, textPositionY + 3.f, 0.f, 0.f);
  }

  {
    m_Device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_Device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, &background, sizeof(Vertex));

    m_Device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_Device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, triangleCount, &quads[0], sizeof(Vertex));
    delete[] quads;
  }
}
