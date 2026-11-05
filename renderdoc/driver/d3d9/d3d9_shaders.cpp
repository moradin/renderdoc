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

// The replay driver's shader functions: reflection, disassembly and constants. The work is done by
// the platform-neutral D3D9BC code (d3d9_bytecode.h, d3d9_shader_reflect.h), this keeps each
// shader's bytecode and answers the replay controller with it.

#include "common/formatting.h"
#include "d3d9_replay.h"
#include "d3d9_shader_reflect.h"

static const char *const D3D9DisassemblyTarget = "D3D9 Bytecode";

void D3D9Replay::AddShader(ResourceId id, const uint32_t *tokens, size_t numTokens)
{
  // shaders created during the frame are created again each time it's replayed. Keep the first,
  // the replay controller may be holding on to its reflection.
  if(m_Shaders.find(id) != m_Shaders.end())
    return;

  D3D9BC::ShaderInfo &info = m_Shaders[id];
  info.Init(id, tokens, numTokens);

  if(!info.GetProgram().error.empty())
    RDCWARN("Couldn't fully parse shader %s: %s", ToStr(id).c_str(), info.GetProgram().error.c_str());
  if(!info.GetConstantTable().error.empty())
    RDCWARN("Couldn't fully read shader %s's constant table: %s", ToStr(id).c_str(),
            info.GetConstantTable().error.c_str());
}

rdcarray<ShaderEntryPoint> D3D9Replay::GetShaderEntryPoints(ResourceId shader)
{
  auto it = m_Shaders.find(shader);
  if(it == m_Shaders.end())
    return {};

  // D3D9 bytecode has no entry point name, every shader has the one
  return {{"main", it->second.GetStage()}};
}

const ShaderReflection *D3D9Replay::GetShader(ResourceId pipeline, ResourceId shader,
                                              ShaderEntryPoint entry)
{
  auto it = m_Shaders.find(shader);
  if(it == m_Shaders.end())
    return NULL;

  return &it->second.GetReflection();
}

rdcarray<rdcstr> D3D9Replay::GetDisassemblyTargets(bool withPipeline)
{
  return {D3D9DisassemblyTarget};
}

rdcstr D3D9Replay::DisassembleShader(ResourceId pipeline, const ShaderReflection *refl,
                                     const rdcstr &target)
{
  if(refl == NULL)
    return "; Invalid Shader Specified";

  if(!target.empty() && target != D3D9DisassemblyTarget)
    return StringFormat::Fmt("; Invalid disassembly target %s", target.c_str());

  auto it = m_Shaders.find(refl->resourceId);
  if(it != m_Shaders.end())
    return it->second.GetDisassembly();

  // not one of ours, but if it's D3D9 bytecode it can still be disassembled
  if(refl->encoding != ShaderEncoding::D3D9Bytecode)
    return "; Not a D3D9 shader";

  D3D9BC::ShaderInfo info;
  info.Init(refl->resourceId, (const uint32_t *)refl->rawBytes.data(),
            refl->rawBytes.size() / sizeof(uint32_t));
  return info.GetDisassembly();
}

void D3D9Replay::FillCBufferVariables(ResourceId pipeline, ResourceId shader, ShaderStage stage,
                                      rdcstr entryPoint, uint32_t cbufSlot,
                                      rdcarray<ShaderVariable> &outvars, const bytebuf &data)
{
  auto it = m_Shaders.find(shader);
  if(it == m_Shaders.end())
  {
    outvars.clear();
    return;
  }

  const D3D9BC::ShaderInfo &info = it->second;
  const rdcarray<ConstantBlock> &blocks = info.GetReflection().constantBlocks;

  // D3D9's constants aren't in a buffer (the blocks aren't bufferBacked), the pseudo buffers just
  // present the device's register files. With no data, read the register file from the device
  // ourselves, as GL does for bare uniforms. The replay controller passes no data when
  // GetCBufferVariableContents is given a length of 0, which for D3D11 means a binding with an
  // empty range, but D3D9 can't bind a part of its constants, so here it means all of them.
  if(data.empty() && cbufSlot < blocks.size())
  {
    ResourceId buf = GetConstantsBufferID(info.GetStage(), blocks[cbufSlot].fixedBindNumber);
    if(buf != ResourceId())
    {
      bytebuf registers;
      GetConstantsData(buf, 0, 0, registers);
      info.FillConstants(cbufSlot, registers, outvars);
      return;
    }
  }

  info.FillConstants(cbufSlot, data, outvars);
}
