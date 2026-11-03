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
#include "d3d9_manager.h"
#include "d3d9_resources.h"

bool D3D9InitParams::IsSupportedVersion(uint64_t ver)
{
  return ver == CurrentVersion;
}

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3D9InitParams &el)
{
  SERIALISE_MEMBER(Adapter);
  SERIALISE_MEMBER(DeviceType);
  SERIALISE_MEMBER_TYPED(uint32_t, BehaviorFlags);
  SERIALISE_MEMBER(Ex);
  SERIALISE_MEMBER(AdapterIdentifier);
}

INSTANTIATE_SERIALISE_TYPE(D3D9InitParams);

// interfaces are serialised as the ResourceId of the wrapper
template <class SerialiserType, class Interface>
void DoSerialiseViaResourceId(SerialiserType &ser, Interface *&el)
{
  D3D9ResourceManager *rm = (D3D9ResourceManager *)ser.GetUserData();

  ResourceId id;

  if(ser.IsWriting() && rm)
    id = GetResID(el);

  DoSerialise(ser, id);

  if(ser.IsReading())
  {
    if(id != ResourceId() && rm && rm->HasResource(id))
      el = (Interface *)rm->GetResource(id);
    else
      el = NULL;
  }
}

#undef SERIALISE_INTERFACE
#define SERIALISE_INTERFACE(iface)                  \
  template <class SerialiserType>                   \
  void DoSerialise(SerialiserType &ser, iface *&el) \
  {                                                 \
    DoSerialiseViaResourceId(ser, el);              \
  }                                                 \
  INSTANTIATE_SERIALISE_TYPE(iface *);

SERIALISE_D3D9_INTERFACES();

// window handles are meaningless outside the captured process. Serialise them for information but
// read them back as NULL.
template <class SerialiserType>
static void SerialiseHWND(SerialiserType &ser, const rdcliteral &name, HWND &wnd)
{
  uint64_t handle = ser.IsWriting() ? (uint64_t)(uintptr_t)wnd : 0;
  ser.Serialise(name, handle);
  if(ser.IsReading())
    wnd = NULL;
}

// fixed-size char arrays as strings
template <class SerialiserType, size_t N>
static void SerialiseCharArray(SerialiserType &ser, const rdcliteral &name, char (&str)[N])
{
  rdcstr s;
  if(ser.IsWriting())
    s = rdcstr(str, strnlen(str, N));
  ser.Serialise(name, s);
  if(ser.IsReading())
  {
    RDCEraseEl(str);
    memcpy(str, s.c_str(), RDCMIN(s.size(), N - 1));
  }
}

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DPRESENT_PARAMETERS &el)
{
  SERIALISE_MEMBER(BackBufferWidth).Important();
  SERIALISE_MEMBER(BackBufferHeight).Important();
  SERIALISE_MEMBER(BackBufferFormat).Important();
  SERIALISE_MEMBER(BackBufferCount);
  SERIALISE_MEMBER(MultiSampleType);
  SERIALISE_MEMBER_TYPED(uint32_t, MultiSampleQuality);
  SERIALISE_MEMBER(SwapEffect);
  SerialiseHWND(ser, "hDeviceWindow"_lit, el.hDeviceWindow);
  SERIALISE_MEMBER_TYPED(int32_t, Windowed);
  SERIALISE_MEMBER_TYPED(int32_t, EnableAutoDepthStencil);
  SERIALISE_MEMBER(AutoDepthStencilFormat);
  SERIALISE_MEMBER_TYPED(uint32_t, Flags);
  SERIALISE_MEMBER(FullScreen_RefreshRateInHz);
  SERIALISE_MEMBER(PresentationInterval);
}

INSTANTIATE_SERIALISE_TYPE(D3DPRESENT_PARAMETERS);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DDISPLAYMODEEX &el)
{
  SERIALISE_MEMBER(Size);
  SERIALISE_MEMBER(Width);
  SERIALISE_MEMBER(Height);
  SERIALISE_MEMBER(RefreshRate);
  SERIALISE_MEMBER(Format);
  SERIALISE_MEMBER(ScanLineOrdering);
}

INSTANTIATE_SERIALISE_TYPE(D3DDISPLAYMODEEX);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DADAPTER_IDENTIFIER9 &el)
{
  SerialiseCharArray(ser, "Driver"_lit, el.Driver);
  SerialiseCharArray(ser, "Description"_lit, el.Description);
  SerialiseCharArray(ser, "DeviceName"_lit, el.DeviceName);
  SERIALISE_MEMBER_TYPED(int64_t, DriverVersion);
  SERIALISE_MEMBER_TYPED(uint32_t, VendorId);
  SERIALISE_MEMBER_TYPED(uint32_t, DeviceId);
  SERIALISE_MEMBER_TYPED(uint32_t, SubSysId);
  SERIALISE_MEMBER_TYPED(uint32_t, Revision);
  SERIALISE_MEMBER(DeviceIdentifier);
  SERIALISE_MEMBER_TYPED(uint32_t, WHQLLevel);
}

INSTANTIATE_SERIALISE_TYPE(D3DADAPTER_IDENTIFIER9);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DVIEWPORT9 &el)
{
  SERIALISE_MEMBER_TYPED(uint32_t, X);
  SERIALISE_MEMBER_TYPED(uint32_t, Y);
  SERIALISE_MEMBER_TYPED(uint32_t, Width);
  SERIALISE_MEMBER_TYPED(uint32_t, Height);
  SERIALISE_MEMBER(MinZ);
  SERIALISE_MEMBER(MaxZ);
}

INSTANTIATE_SERIALISE_TYPE(D3DVIEWPORT9);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DMATRIX &el)
{
  // serialise the union as a flat array of 16 floats, row-major (_11, _12, ...)
  float(&m)[16] = *(float(*)[16]) & el.m[0][0];
  ser.Serialise("m"_lit, m);
}

INSTANTIATE_SERIALISE_TYPE(D3DMATRIX);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DCOLORVALUE &el)
{
  SERIALISE_MEMBER(r);
  SERIALISE_MEMBER(g);
  SERIALISE_MEMBER(b);
  SERIALISE_MEMBER(a);
}

INSTANTIATE_SERIALISE_TYPE(D3DCOLORVALUE);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DVECTOR &el)
{
  SERIALISE_MEMBER(x);
  SERIALISE_MEMBER(y);
  SERIALISE_MEMBER(z);
}

INSTANTIATE_SERIALISE_TYPE(D3DVECTOR);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DMATERIAL9 &el)
{
  SERIALISE_MEMBER(Diffuse);
  SERIALISE_MEMBER(Ambient);
  SERIALISE_MEMBER(Specular);
  SERIALISE_MEMBER(Emissive);
  SERIALISE_MEMBER(Power);
}

INSTANTIATE_SERIALISE_TYPE(D3DMATERIAL9);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DLIGHT9 &el)
{
  SERIALISE_MEMBER(Type).Important();
  SERIALISE_MEMBER(Diffuse);
  SERIALISE_MEMBER(Specular);
  SERIALISE_MEMBER(Ambient);
  SERIALISE_MEMBER(Position);
  SERIALISE_MEMBER(Direction);
  SERIALISE_MEMBER(Range);
  SERIALISE_MEMBER(Falloff);
  SERIALISE_MEMBER(Attenuation0);
  SERIALISE_MEMBER(Attenuation1);
  SERIALISE_MEMBER(Attenuation2);
  SERIALISE_MEMBER(Theta);
  SERIALISE_MEMBER(Phi);
}

INSTANTIATE_SERIALISE_TYPE(D3DLIGHT9);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DCLIPSTATUS9 &el)
{
  SERIALISE_MEMBER_TYPED(uint32_t, ClipUnion);
  SERIALISE_MEMBER_TYPED(uint32_t, ClipIntersection);
}

INSTANTIATE_SERIALISE_TYPE(D3DCLIPSTATUS9);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DVERTEXELEMENT9 &el)
{
  SERIALISE_MEMBER(Stream);
  SERIALISE_MEMBER(Offset);
  // these are BYTEs holding D3DDECLTYPE, D3DDECLMETHOD and D3DDECLUSAGE
  SERIALISE_MEMBER(Type);
  SERIALISE_MEMBER(Method);
  SERIALISE_MEMBER(Usage);
  SERIALISE_MEMBER(UsageIndex);
}

INSTANTIATE_SERIALISE_TYPE(D3DVERTEXELEMENT9);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DRECT &el)
{
  SERIALISE_MEMBER_TYPED(int32_t, x1);
  SERIALISE_MEMBER_TYPED(int32_t, y1);
  SERIALISE_MEMBER_TYPED(int32_t, x2);
  SERIALISE_MEMBER_TYPED(int32_t, y2);
}

INSTANTIATE_SERIALISE_TYPE(D3DRECT);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DBOX &el)
{
  SERIALISE_MEMBER(Left);
  SERIALISE_MEMBER(Top);
  SERIALISE_MEMBER(Right);
  SERIALISE_MEMBER(Bottom);
  SERIALISE_MEMBER(Front);
  SERIALISE_MEMBER(Back);
}

INSTANTIATE_SERIALISE_TYPE(D3DBOX);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DRECTPATCH_INFO &el)
{
  SERIALISE_MEMBER(StartVertexOffsetWidth);
  SERIALISE_MEMBER(StartVertexOffsetHeight);
  SERIALISE_MEMBER(Width);
  SERIALISE_MEMBER(Height);
  SERIALISE_MEMBER(Stride);
  SERIALISE_MEMBER(Basis);
  SERIALISE_MEMBER(Degree);
}

INSTANTIATE_SERIALISE_TYPE(D3DRECTPATCH_INFO);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3DTRIPATCH_INFO &el)
{
  SERIALISE_MEMBER(StartVertexOffset);
  SERIALISE_MEMBER(NumVertices);
  SERIALISE_MEMBER(Basis);
  SERIALISE_MEMBER(Degree);
}

INSTANTIATE_SERIALISE_TYPE(D3DTRIPATCH_INFO);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, PALETTEENTRY &el)
{
  SERIALISE_MEMBER(peRed);
  SERIALISE_MEMBER(peGreen);
  SERIALISE_MEMBER(peBlue);
  SERIALISE_MEMBER(peFlags);
}

INSTANTIATE_SERIALISE_TYPE(PALETTEENTRY);

// RECT is serialised by the shared DXGI code

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, POINT &el)
{
  SERIALISE_MEMBER_TYPED(int32_t, x);
  SERIALISE_MEMBER_TYPED(int32_t, y);
}

INSTANTIATE_SERIALISE_TYPE(POINT);
