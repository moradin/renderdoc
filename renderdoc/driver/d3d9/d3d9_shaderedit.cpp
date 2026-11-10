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

#include "driver/dx/official/d3dcompiler.h"
#include "driver/shaders/dxbc/dxbc_compile.h"
#include "driver/shaders/dxbc/dxbc_container.h"
#include "d3d9_device.h"
#include "d3d9_replay.h"
#include "d3d9_resources.h"

// Shader editing
// --------------
// Edited shaders replace the application's, as on D3D11: BuildTargetShader creates a shader through
// the wrapped device, so it has a ResourceId, and ReplaceResource has the resource manager return
// it in place of the original. Every way a shader gets bound on replay looks it up by ID there:
// replayed Set*Shader calls, the bindings when the frame starts (BeginCaptureFrame), and state
// blocks' initial contents. State blocks created or captured during the frame record what replay
// bound, which is the replacement.
//
// Shaders can be:
// * HLSL, compiled by d3dcompiler_47 for the profile given with /T, or vs_3_0 / ps_3_0. It
//   compiles vs_1_1 to vs_3_0 and ps_2_0 to ps_3_0, but not ps_1_x.
// * D3D9 assembly, assembled by d3dcompiler_47's D3DAssemble, which takes every shader model
//   including ps_1_x. RenderDoc's disassembly is in fxc's format, so it assembles back to the same
//   instructions; the constant table, which is only comments in the disassembly, is lost.
// * D3D9 bytecode, as given.
// The result has to parse completely, as the runtime reads it up to its end token with no length.

typedef HRESULT(WINAPI *pD3DAssemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                      const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                      UINT Flags, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs);

bool D3D9Replay::BuildShaderBytecode(ShaderEncoding encoding, const bytebuf &source,
                                     const rdcstr &entry, const ShaderCompileFlags &flags,
                                     const rdcstr &profile, const rdcarray<rdcstr> &includeDirs,
                                     ShaderStage stage, rdcarray<DWORD> &bytecode, rdcstr &errors)
{
  bytecode.clear();

  if(stage != ShaderStage::Vertex && stage != ShaderStage::Pixel)
  {
    errors = "D3D9 only has vertex and pixel shaders";
    return false;
  }

  const char *stageName = stage == ShaderStage::Vertex ? "vertex" : "pixel";

  if(encoding == ShaderEncoding::HLSL || encoding == ShaderEncoding::D3D9Asm)
  {
    HMODULE d3dcompiler = GetD3DCompiler();
    if(d3dcompiler == NULL)
    {
      errors = "Couldn't load d3dcompiler_47.dll";
      return false;
    }

    EmbeddedD3DIncluder includer(includeDirs, {});

    ID3DBlob *code = NULL, *compileErrors = NULL;
    HRESULT hr = E_FAIL;

    if(encoding == ShaderEncoding::HLSL)
    {
      rdcstr target = DXBC::GetProfile(flags);
      if(target.empty())
        target = profile;

      if(!target.beginsWith(stage == ShaderStage::Vertex ? "vs_" : "ps_"))
      {
        errors = StringFormat::Fmt("%s isn't a %s shader profile", target.c_str(), stageName);
        return false;
      }

      pD3DCompile compile = (pD3DCompile)GetProcAddress(d3dcompiler, "D3DCompile");
      if(compile == NULL)
      {
        errors = "Couldn't load D3DCompile from d3dcompiler_47.dll";
        return false;
      }

      hr = compile(source.data(), source.size(), "main.hlsl", NULL, &includer,
                   entry.empty() ? "main" : entry.c_str(), target.c_str(), DXBC::DecodeFlags(flags),
                   0, &code, &compileErrors);
    }
    else
    {
      pD3DAssemble assemble = (pD3DAssemble)GetProcAddress(d3dcompiler, "D3DAssemble");
      if(assemble == NULL)
      {
        errors = "Couldn't load D3DAssemble from d3dcompiler_47.dll";
        return false;
      }

      hr = assemble(source.data(), source.size(), "main.asm", NULL, &includer, 0, &code,
                    &compileErrors);
    }

    if(compileErrors)
      errors.assign(
          (const char *)compileErrors->GetBufferPointer(),
          strnlen((const char *)compileErrors->GetBufferPointer(), compileErrors->GetBufferSize()));

    if(SUCCEEDED(hr) && code)
    {
      bytecode.resize(code->GetBufferSize() / sizeof(DWORD));
      memcpy(bytecode.data(), code->GetBufferPointer(), bytecode.byteSize());
    }
    else if(errors.empty())
    {
      errors = StringFormat::Fmt("Couldn't %s the shader: %s",
                                 encoding == ShaderEncoding::HLSL ? "compile" : "assemble",
                                 ToStr(hr).c_str());
    }

    SAFE_RELEASE(code);
    SAFE_RELEASE(compileErrors);

    if(bytecode.empty())
      return false;
  }
  else if(encoding == ShaderEncoding::D3D9Bytecode)
  {
    if(source.size() % sizeof(DWORD) != 0)
    {
      errors = "D3D9 shader bytecode is a whole number of DWORDs";
      return false;
    }
    bytecode.resize(source.size() / sizeof(DWORD));
    memcpy(bytecode.data(), source.data(), bytecode.byteSize());
  }
  else
  {
    errors = StringFormat::Fmt("D3D9 shaders can't be built from %s", ToStr(encoding).c_str());
    return false;
  }

  D3D9BC::Program prog;
  D3D9BC::ParseProgram((const uint32_t *)bytecode.data(), bytecode.size(), prog);

  rdcstr problem;
  if(!prog.validVersion)
    problem = "It isn't D3D9 shader bytecode, it has no shader version token";
  else if((prog.version.type == D3D9BC::ShaderType::Vertex) != (stage == ShaderStage::Vertex))
    problem = StringFormat::Fmt("It's a %s shader, not a %s shader",
                                D3D9BC::VersionString(prog.version).c_str(), stageName);
  else if(!prog.error.empty())
    // the runtime reads up to the end token, so bytecode that doesn't parse isn't passed on
    problem = "The bytecode doesn't parse: " + prog.error;

  if(!problem.empty())
  {
    if(!errors.empty() && errors.back() != '\n')
      errors += "\n";
    errors += problem;
    bytecode.clear();
    return false;
  }

  return true;
}

void D3D9Replay::BuildTargetShader(ShaderEncoding sourceEncoding, const bytebuf &source,
                                   const rdcstr &entry, const ShaderCompileFlags &compileFlags,
                                   ShaderStage type, ResourceId &id, rdcstr &errors)
{
  id = ResourceId();

  // HLSL without a profile is shader model 3. vs_3_0 and ps_3_0 only work with each other, so for
  // shaders paired with an older model the profile should be given.
  rdcarray<DWORD> bytecode;
  if(!BuildShaderBytecode(sourceEncoding, source, entry, compileFlags,
                          type == ShaderStage::Vertex ? "vs_3_0" : "ps_3_0", {}, type, bytecode,
                          errors))
    return;

  IUnknown *shader = NULL;
  HRESULT hr = E_FAIL;
  if(type == ShaderStage::Vertex)
  {
    IDirect3DVertexShader9 *vs = NULL;
    hr = m_pDevice->CreateVertexShader(bytecode.data(), &vs);
    shader = vs;
  }
  else
  {
    IDirect3DPixelShader9 *ps = NULL;
    hr = m_pDevice->CreatePixelShader(bytecode.data(), &ps);
    shader = ps;
  }

  if(FAILED(hr) || shader == NULL)
  {
    if(!errors.empty() && errors.back() != '\n')
      errors += "\n";
    errors += StringFormat::Fmt("Couldn't create the shader: %s", ToStr(hr).c_str());
    SAFE_RELEASE(shader);
    return;
  }

  id = GetResID(shader);
  AddShader(id, (const uint32_t *)bytecode.data(), bytecode.size());
  m_TargetShaders[id] = shader;
}

rdcarray<ShaderEncoding> D3D9Replay::GetTargetShaderEncodings()
{
  return {ShaderEncoding::D3D9Bytecode, ShaderEncoding::HLSL, ShaderEncoding::D3D9Asm};
}

void D3D9Replay::ReplaceResource(ResourceId from, ResourceId to)
{
  // only a shader for a shader of the same stage: the replacement is bound with the original's
  // Set*Shader, which can't take anything else
  auto fromIt = m_Shaders.find(from);
  auto toIt = m_Shaders.find(to);
  if(fromIt == m_Shaders.end() || toIt == m_Shaders.end() ||
     fromIt->second.GetStage() != toIt->second.GetStage())
  {
    RDCWARN("Only a D3D9 shader can replace another, of the same stage: %s can't replace %s",
            ToStr(to).c_str(), ToStr(from).c_str());
    return;
  }

  m_pDevice->GetResourceManager()->ReplaceResource(from, to);
  ClearPostVSCache();
}

void D3D9Replay::RemoveReplacement(ResourceId id)
{
  m_pDevice->GetResourceManager()->RemoveReplacement(id);
  ClearPostVSCache();
}

void D3D9Replay::FreeTargetResource(ResourceId id)
{
  auto it = m_TargetShaders.find(id);
  if(it == m_TargetShaders.end())
    return;

  // its reflection in m_Shaders stays, the replay controller may still point to it
  IUnknown *shader = it->second;
  m_TargetShaders.erase(it);

  // a replacement that's gone would bind nothing, so it's removed. The shader itself is only
  // released after the next replay, which no longer draws with it: releasing it straight after a
  // replay that drew with it made NVIDIA's driver crash in the next replay's first draw (reading
  // freed memory). Releasing it after a replay without it, as RemoveReplacement then
  // FreeTargetResource does, didn't. See d3d9_step4.md.
  D3D9ResourceManager *rm = m_pDevice->GetResourceManager();
  ResourceId original = rm->GetUnreplacedID(id);
  if(original != id)
  {
    rm->RemoveReplacement(original);
    ClearPostVSCache();
    m_FreedTargetShaders.push_back(shader);
    return;
  }

  SAFE_RELEASE(shader);
}

void D3D9Replay::ReleaseFreedTargetShaders()
{
  for(IUnknown *shader : m_FreedTargetShaders)
    SAFE_RELEASE(shader);
  m_FreedTargetShaders.clear();
}

void D3D9Replay::ReleaseTargetShaders()
{
  while(!m_TargetShaders.empty())
    FreeTargetResource(m_TargetShaders.begin()->first);
  ReleaseFreedTargetShaders();
}
