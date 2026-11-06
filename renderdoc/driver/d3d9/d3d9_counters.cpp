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

#include "d3d9_device.h"
#include "d3d9_replay.h"

// GPU counters
// ------------
// Two of D3D9's queries map onto RenderDoc's generic counters: timestamps (with TIMESTAMPDISJOINT
// and TIMESTAMPFREQ) for EventGPUDuration, and occlusion for SamplesPassed. D3D9 has no pipeline
// statistics query like D3D11's (vertices, primitives, invocations). The nearest, VERTEXSTATS and
// the *TIMINGS queries, are optional, and NVIDIA's current driver supports none of them, so they
// aren't offered.
//
// As on D3D11, each action is measured on its own: the frame is replayed up to the action without
// it, then the action alone is replayed between a pair of timestamps and inside an occlusion query.
// Every action that isn't a marker is measured, so clears, copies and Present get a duration too.
// Each partial replay ends the scene it started (WrappedD3DDevice9::ReplayLog), so the after
// timestamp includes the action's EndScene, which on current drivers doesn't wait for the GPU.
//
// Queries are made on the real device, so they aren't in the capture's resources. Afterwards the
// frame is replayed to the event the replay controller was last at, so the texture viewer and the
// rest see the same state as before.

namespace
{
enum class D3D9Counters
{
  Duration = 0x1,
  Samples = 0x2,
};

struct D3D9ActionQueries
{
  uint32_t eventId = 0;
  IDirect3DQuery9 *before = NULL;
  IDirect3DQuery9 *after = NULL;
  IDirect3DQuery9 *occlusion = NULL;
};

bool QuerySupported(IDirect3DDevice9 *dev, D3DQUERYTYPE type)
{
  // a NULL query pointer only asks whether the type is supported
  return dev && SUCCEEDED(dev->CreateQuery(type, NULL));
}

// waits for a query's result. Returns false if it doesn't arrive, e.g. when the device is lost.
bool GetQueryData(IDirect3DQuery9 *query, void *data, DWORD size)
{
  if(!query)
    return false;

  // about 10 seconds, far longer than any single action takes
  for(uint32_t i = 0; i < 1000000; i++)
  {
    HRESULT hr = query->GetData(data, size, D3DGETDATA_FLUSH);
    if(hr == S_OK)
      return true;
    if(hr != S_FALSE)
      return false;
    if(i > 1000)
      Threading::Sleep(0);
  }

  return false;
}

void CollectActions(const rdcarray<ActionDescription> &actions, rdcarray<uint32_t> &eventIds)
{
  for(const ActionDescription &a : actions)
  {
    CollectActions(a.children, eventIds);

    if(a.events.empty() ||
       (a.flags & (ActionFlags::PushMarker | ActionFlags::SetMarker | ActionFlags::PopMarker)))
      continue;

    eventIds.push_back(a.eventId);
  }
}
};

uint32_t D3D9Replay::SupportedCounters()
{
  if(!m_CountersChecked)
  {
    m_CountersChecked = true;
    m_SupportedCounters = 0;

    IDirect3DDevice9 *dev = m_pDevice->GetReal();
    if(QuerySupported(dev, D3DQUERYTYPE_TIMESTAMP) &&
       QuerySupported(dev, D3DQUERYTYPE_TIMESTAMPDISJOINT) &&
       QuerySupported(dev, D3DQUERYTYPE_TIMESTAMPFREQ))
      m_SupportedCounters |= (uint32_t)D3D9Counters::Duration;
    if(QuerySupported(dev, D3DQUERYTYPE_OCCLUSION))
      m_SupportedCounters |= (uint32_t)D3D9Counters::Samples;

    if(!(m_SupportedCounters & (uint32_t)D3D9Counters::Duration))
      RDCWARN("The GPU doesn't support D3D9 timestamp queries, so there's no GPU duration counter");
    if(!(m_SupportedCounters & (uint32_t)D3D9Counters::Samples))
      RDCWARN(
          "The GPU doesn't support D3D9 occlusion queries, so there's no samples passed counter");
  }

  return m_SupportedCounters;
}

rdcarray<GPUCounter> D3D9Replay::EnumerateCounters()
{
  rdcarray<GPUCounter> ret;

  const uint32_t supported = SupportedCounters();
  if(supported & (uint32_t)D3D9Counters::Duration)
    ret.push_back(GPUCounter::EventGPUDuration);
  if(supported & (uint32_t)D3D9Counters::Samples)
    ret.push_back(GPUCounter::SamplesPassed);

  return ret;
}

CounterDescription D3D9Replay::DescribeCounter(GPUCounter counterID)
{
  CounterDescription desc = {};
  desc.counter = counterID;

  // 9B1E6E2A-3C4D-4F6B-8A1D-5E2C7B9D0F31
  desc.uuid.words[0] = 0x9B1E6E2A;
  desc.uuid.words[1] = 0x3C4D4F6B;
  desc.uuid.words[2] = 0x8A1D5E2C;
  desc.uuid.words[3] = 0x7B9D0F31 ^ (uint32_t)counterID;

  desc.category = "D3D9 Built-in";

  switch(counterID)
  {
    case GPUCounter::EventGPUDuration:
      desc.name = "GPU Duration";
      desc.description =
          "Time taken for this event on the GPU, as measured by delta between two GPU timestamps.";
      desc.resultByteWidth = 8;
      desc.resultType = CompType::Float;
      desc.unit = CounterUnit::Seconds;
      break;
    case GPUCounter::SamplesPassed:
      desc.name = "Samples Passed";
      desc.description = "Number of samples that passed depth/stencil test.";
      desc.resultByteWidth = 8;
      desc.resultType = CompType::UInt;
      desc.unit = CounterUnit::Absolute;
      break;
    default:
      desc.name = "Unknown";
      desc.description = "Unknown counter ID";
      desc.resultByteWidth = 0;
      desc.resultType = CompType::Typeless;
      desc.unit = CounterUnit::Absolute;
      break;
  }

  return desc;
}

rdcarray<CounterResult> D3D9Replay::FetchCounters(const rdcarray<GPUCounter> &counters)
{
  rdcarray<CounterResult> ret;

  if(counters.empty())
  {
    RDCERR("No counters specified to FetchCounters");
    return ret;
  }

  SCOPED_TIMER("Fetch Counters, counters to fetch %u", counters.size());

  const uint32_t supported = SupportedCounters();

  bool wantDuration = false, wantSamples = false;
  for(GPUCounter c : counters)
  {
    if(c == GPUCounter::EventGPUDuration && (supported & (uint32_t)D3D9Counters::Duration))
      wantDuration = true;
    else if(c == GPUCounter::SamplesPassed && (supported & (uint32_t)D3D9Counters::Samples))
      wantSamples = true;
    else
      RDCWARN("Counter %s isn't supported on D3D9", ToStr(c).c_str());
  }

  if(!wantDuration && !wantSamples)
    return ret;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  rdcarray<uint32_t> eventIds;
  CollectActions(m_FrameRecord.actionList, eventIds);

  rdcarray<D3D9ActionQueries> queries;
  queries.resize(eventIds.size());

  IDirect3DQuery9 *disjoint = NULL;
  IDirect3DQuery9 *freq = NULL;

  if(wantDuration)
  {
    dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint);
    dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &freq);
    if(!disjoint || !freq)
    {
      RDCERR("Couldn't create the timestamp disjoint or frequency queries");
      wantDuration = false;
    }
  }

  if(disjoint)
    disjoint->Issue(D3DISSUE_BEGIN);

  uint32_t eventStart = 0;
  for(size_t i = 0; i < eventIds.size(); i++)
  {
    D3D9ActionQueries &q = queries[i];
    q.eventId = eventIds[i];

    if(wantDuration)
    {
      dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q.before);
      dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q.after);
    }
    if(wantSamples)
      dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q.occlusion);

    m_pDevice->ReplayLog(eventStart, q.eventId, eReplay_WithoutDraw);

    if(q.occlusion)
      q.occlusion->Issue(D3DISSUE_BEGIN);
    if(q.before && q.after)
      q.before->Issue(D3DISSUE_END);

    m_pDevice->ReplayLog(eventStart, q.eventId, eReplay_OnlyDraw);

    if(q.before && q.after)
      q.after->Issue(D3DISSUE_END);
    if(q.occlusion)
      q.occlusion->Issue(D3DISSUE_END);

    eventStart = q.eventId + 1;
  }

  if(disjoint)
    disjoint->Issue(D3DISSUE_END);
  if(freq)
    freq->Issue(D3DISSUE_END);

  double ticksPerSecond = 0.0;
  if(wantDuration)
  {
    BOOL isDisjoint = FALSE;
    UINT64 frequency = 0;
    if(!GetQueryData(disjoint, &isDisjoint, sizeof(isDisjoint)) ||
       !GetQueryData(freq, &frequency, sizeof(frequency)) || frequency == 0)
    {
      RDCERR("Couldn't read the timestamp frequency");
    }
    else
    {
      // a disjoint range (the clock changed, e.g. power management) makes the durations
      // unreliable, but they're still the best there is
      if(isDisjoint)
        RDCWARN("The timestamps were disjoint, the GPU durations may be wrong");
      ticksPerSecond = double(frequency);
    }
  }

  for(D3D9ActionQueries &q : queries)
  {
    for(GPUCounter c : counters)
    {
      if(c == GPUCounter::EventGPUDuration && wantDuration)
      {
        UINT64 a = 0, b = 0;
        double duration = -1.0;
        if(ticksPerSecond > 0.0 && GetQueryData(q.before, &a, sizeof(a)) &&
           GetQueryData(q.after, &b, sizeof(b)))
          duration = b >= a ? double(b - a) / ticksPerSecond : 0.0;
        ret.push_back(CounterResult(q.eventId, GPUCounter::EventGPUDuration, duration));
      }
      else if(c == GPUCounter::SamplesPassed && wantSamples)
      {
        DWORD samples = 0;
        uint64_t value = 0xFFFFFFFFFFFFFFFFULL;
        if(GetQueryData(q.occlusion, &samples, sizeof(samples)))
          value = samples;
        ret.push_back(CounterResult(q.eventId, GPUCounter::SamplesPassed, value));
      }
    }

    SAFE_RELEASE(q.before);
    SAFE_RELEASE(q.after);
    SAFE_RELEASE(q.occlusion);
  }

  SAFE_RELEASE(disjoint);
  SAFE_RELEASE(freq);

  // put the replay back where the replay controller left it
  m_pDevice->ReplayLog(0, m_ReplayedEvent, eReplay_Full);

  return ret;
}
