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

#include "d3d9_common.h"
#include "d3d9_device.h"

// disabled until capture is actually supported, so we can actually tell programmatically that the
// API is unsupported. E.g. when we detect the API and let the user know, but disable a capture
// button.
#if 0

ResultDetails D3D9_CreateReplayDevice(RDCFile *rdc, IReplayDriver **driver)
{
  RDCERR("D3D9 captures are not currently supported");
  return ResultCode::APIUnsupported;
}

static DriverRegistration D3D9DriverRegistration(RDCDriver::D3D9, &D3D9_CreateReplayDevice);

#endif

// Structured export (e.g. renderdoccmd convert, or the python API's GetStructuredData) only needs
// to read the capture, so it works before replay does.
RDResult D3D9_ProcessStructured(RDCFile *rdc, SDFile &output)
{
  int sectionIdx = rdc->SectionIndex(SectionType::FrameCapture);

  if(sectionIdx < 0)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "File does not contain captured API data");

  uint64_t version = rdc->GetSectionProperties(sectionIdx).version;

  if(!D3D9InitParams::IsSupportedVersion(version))
    RETURN_ERROR_RESULT(ResultCode::APIIncompatibleVersion,
                        "D3D9 capture is incompatible version %llu, newest supported by this "
                        "build of RenderDoc is %llu",
                        version, D3D9InitParams::CurrentVersion);

  WrappedD3DDevice9 device((D3D9InitParams()));

  device.SetStructuredExport(version);
  RDResult result = device.ReadLogInitialisation(rdc, true);

  if(result == ResultCode::Succeeded)
    device.GetStructuredFile()->Swap(output);

  return result;
}

static StructuredProcessRegistration D3D9ProcessRegistration(RDCDriver::D3D9,
                                                             &D3D9_ProcessStructured);
