// Copyright (c) 2016 Nicolas Weber and Sandra C. Amend / GCC / TU-Darmstadt. All rights reserved. 
// Use of this source code is governed by the BSD 3-Clause license that can be
// found in the LICENSE file.
#include <cmath>
#include <cstdint>
#include "common.h"

#define THREADS 128
#define WSIZE 32
#define TSIZE (THREADS / WSIZE)

#define TX item.get_local_id(1)
#define PX (item.get_group(1) * TSIZE + (TX / WSIZE))
#define PY item.get_group(0)
#define WTHREAD  (TX % WSIZE)

//-------------------------------------------------------------------
// SHARED
//-------------------------------------------------------------------
struct Params {
  uint32_t oWidth;
  uint32_t oHeight;
  uint32_t iWidth;
  uint32_t iHeight;
  float pWidth;
  float pHeight;
  float lambda;
};

inline
void normalize(float4& var) {
  var.x() /= var.w();
  var.y() /= var.w();
  var.z() /= var.w();
  var.w() = 1.f;
}

void add(float4& output, const uchar3& color, const float factor) {
  output.x() += (float)color.x() * factor;  
  output.y() += (float)color.y() * factor;  
  output.z() += (float)color.z() * factor;  
  output.w() += factor;
}

inline
float lambda(const Params p, const float dist) {
  if(p.lambda == 0.f)
    return 1.f;
  else if(p.lambda == 1.f)
    return dist;
  return sycl::pow(dist, p.lambda);
}

struct Local {
  float sx, ex, sy, ey;
  uint32_t sxr, syr, exr, eyr, xCount, yCount, pixelCount;

  inline Local(nd_item<2> &item, const Params& p) {
    sx      = sycl::fmax( PX    * p.pWidth, 0.f);
    ex      = sycl::fmin((PX+1) * p.pWidth, (float)p.iWidth);
    sy      = sycl::fmax( PY    * p.pHeight, 0.f);
    ey      = sycl::fmin((PY+1) * p.pHeight, (float)p.iHeight);

    sxr      = (uint32_t)sycl::floor(sx);
    syr      = (uint32_t)sycl::floor(sy);
    exr      = (uint32_t)sycl::ceil(ex);
    eyr      = (uint32_t)sycl::ceil(ey);
    xCount    = exr - sxr;
    yCount    = eyr - syr;
    pixelCount  = xCount * yCount;
  }
};

inline
float contribution(const Local& l, float f, const uint32_t x, const uint32_t y) {
  if(x < l.sx)    f *= 1.f - (l.sx - x);
  if((x+1.f) > l.ex)  f *= 1.f - ((x+1.f) - l.ex);
  if(y < l.sy)    f *= 1.f - (l.sy - y);
  if((y+1.f) > l.ey)  f *= 1.f - ((y+1.f) - l.ey);
  return f;
}

// https://devblogs.nvidia.com/parallelforall/faster-parallel-reductions-kepler/
inline
float4 __shfl_down(nd_item<2> &item, const float4 var, const uint32_t srcLane)
{
  float4 output;
  auto sg = item.get_sub_group();
  output.x() = shift_group_left(sg, var.x(), srcLane);
  output.y() = shift_group_left(sg, var.y(), srcLane);
  output.z() = shift_group_left(sg, var.z(), srcLane);
  output.w() = shift_group_left(sg, var.w(), srcLane);
  return output;
}

inline
void reduce(nd_item<2> &item, float4& value) {
  value += __shfl_down(item, value, 16);
  value += __shfl_down(item, value, 8);
  value += __shfl_down(item, value, 4);
  value += __shfl_down(item, value, 2);
  value += __shfl_down(item, value, 1);
}

inline
float distance(const float4& avg, const uchar3& color) {
  const float x = avg.x() - color.x();
  const float y = avg.y() - color.y();
  const float z = avg.z() - color.z();
  return sycl::sqrt(x * x + y * y + z * z) / 441.6729559f; // L2-Norm / sqrt(255^2 * 3)
}

void kernelGuidance(nd_item<2> &item,
                    const uchar3* __restrict input,
                    uchar3* __restrict patches, const Params p)
{
  if(PX >= p.oWidth || PY >= p.oHeight) return;

  // init
  const Local l(item, p);
  float4 color = (float4)(0);

  // iterate pixels
  for(uint32_t i = WTHREAD; i < l.pixelCount; i += WSIZE) {
    const uint32_t x = l.sxr + (i % l.xCount);
    const uint32_t y = l.syr + (i / l.xCount);

    float f = contribution(l, 1.f, x, y);  

    const uchar3& pixel = input[x + y * p.iWidth];
    color += {pixel.x() * f, pixel.y() * f, pixel.z() * f, f};
  }

  // reduce warps
  reduce(item, color);

  // store results
  if((TX % 32) == 0) {
    normalize(color);
    patches[PX + PY * p.oWidth] = uchar3(color.x(), color.y(), color.z());
  }
}

inline
float4 calcAverage(nd_item<2> &item, const Params& p, 
                   const uchar3* __restrict patches) {
  const float corner = 1.0;
  const float edge   = 2.0;
  const float center = 4.0;

  // calculate average color
  float4 avg = (float4)(0);

  // TOP
  if(PY > 0) {
    if(PX > 0) 
      add(avg, patches[(PX - 1) + (PY - 1) * p.oWidth], corner);

    add(avg, patches[(PX) + (PY - 1) * p.oWidth], edge);

    if((PX+1) < p.oWidth)
      add(avg, patches[(PX + 1) + (PY - 1) * p.oWidth], corner);
  }

  // LEFT
  if(PX > 0) 
    add(avg, patches[(PX - 1) + (PY) * p.oWidth], edge);

  // CENTER
  add(avg, patches[(PX) + (PY) * p.oWidth], center);

  // RIGHT
  if((PX+1) < p.oWidth)
    add(avg, patches[(PX + 1) + (PY) * p.oWidth], edge);

  // BOTTOM
  if((PY+1) < p.oHeight) {
    if(PX > 0) 
      add(avg, patches[(PX - 1) + (PY + 1) * p.oWidth], corner);

    add(avg, patches[(PX) + (PY + 1) * p.oWidth], edge);

    if((PX+1) < p.oWidth)
      add(avg, patches[(PX + 1) + (PY + 1) * p.oWidth], corner);
  }

  normalize(avg);

  return avg;
}

void kernelDownsampling(nd_item<2> &item,
                        const uchar3* __restrict input,
                        const uchar3* __restrict patches,
                        const Params p,
                              uchar3* __restrict output)
{
  if(PX >= p.oWidth || PY >= p.oHeight) return;

  // init
  const Local l(item, p);
  const float4 avg = calcAverage(item, p, patches);

  float4 color = (float4)(0);

  // iterate pixels
  for(uint32_t i = WTHREAD; i < l.pixelCount; i += WSIZE) {
    const uint32_t x = l.sxr + (i % l.xCount);
    const uint32_t y = l.syr + (i / l.xCount);

    const uchar3& pixel = input[x + y * p.iWidth];
    float f = distance(avg, pixel);

    f = lambda(p, f);
    f = contribution(l, f, x, y);

    add(color, pixel, f);
  }

  // reduce warp
  reduce(item, color);

  if(WTHREAD == 0) {
    uchar3& ref = output[PX + PY * p.oWidth];

    if(color.w() == 0.0f)
      ref = uchar3((unsigned char)avg.x(), (unsigned char)avg.y(), (unsigned char)avg.z());
    else {
      normalize(color);
      ref = uchar3((unsigned char)color.x(), (unsigned char)color.y(), (unsigned char)color.z());
    }
  }
}

void run(const Params& p, const uchar3* hInput, uchar3* hOutput) {
  const size_t sInput    = p.iWidth * p.iHeight;
  const size_t sOutput  = p.oWidth * p.oHeight;
  const size_t sGuidance  = p.oWidth * p.oHeight;

#ifdef USE_GPU
  gpu_selector dev_sel;
#else
  cpu_selector dev_sel;
#endif
  queue q(dev_sel);

  buffer<uchar3, 1> dInput (hInput, sInput);
  buffer<uchar3, 1> dOutput (hOutput, sOutput);
  buffer<uchar3, 1> dGuidance (sGuidance);

  range<2> lws (1, THREADS);
  range<2> gws (p.oHeight, (uint32_t)std::ceil(p.oWidth / (float)TSIZE) * THREADS);

  for (int i = 0; i < 100; i++) {
    q.submit([&] (handler &cgh) {
      auto in = dInput.get_access<sycl_read>(cgh);
      auto patch = dGuidance.get_access<sycl_write>(cgh);
      cgh.parallel_for<class guidance>(nd_range<2>(gws, lws), [=] (nd_item<2> item) [[intel::reqd_sub_group_size(32)]] {
        kernelGuidance (item, in.get_pointer(), patch.get_pointer(), p);
      });
    });

    q.submit([&] (handler &cgh) {
      auto in = dInput.get_access<sycl_read>(cgh);
      auto patch = dGuidance.get_access<sycl_read>(cgh);
      auto out = dOutput.get_access<sycl_discard_write>(cgh);
      cgh.parallel_for<class downsample>(nd_range<2>(gws, lws), [=] (nd_item<2> item) [[intel::reqd_sub_group_size(32)]] {
        kernelDownsampling (item, in.get_pointer(), patch.get_pointer(), p, out.get_pointer());
      });
    });
  }
}
