#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <hip/hip_runtime.h>
#include "utils.h"
#include "reference.h"

__global__ void car (
    const float *__restrict__ img,
    const float *__restrict__ kernels,
    const float *__restrict__ offsets_h,
    const float *__restrict__ offsets_v,
          float *__restrict__ output,
    const params p,
    const int offset_unit,
    const int padding,
    const size_t n)
{
  int global_idx = blockDim.x * blockIdx.x + threadIdx.x;
  if(global_idx >= n) return;

  const int dim_b = p.output_dim_b;
  const int dim_c = p.output_dim_c;
  const int dim_h = p.output_dim_h;
  const int dim_w = p.output_dim_w;
  const int kernels_size = p.kernel_size;
  const int img_w = p.image_w;
  const int img_h = p.image_h;

  const int idb = (global_idx / (dim_c * dim_h * dim_w)) % dim_b;
  const int idc = (global_idx / (dim_h * dim_w)) % dim_c;
  const int idy = (global_idx / dim_w) % dim_h;
  const int idx = global_idx % dim_w;

  const int k_size = (int)sqrtf(float(kernels_size));
  const int w = img_w - 2 * padding;
  const int h = img_h - 2 * padding;

  float result = 0;
  for(int k_y = 0; k_y < k_size; ++k_y)
  {
    for(int k_x = 0; k_x < k_size; ++k_x)
    {
      const float offset_h = offsets_h(idb,k_size * k_y + k_x,idy,idx) * offset_unit;
      const float offset_v = offsets_v(idb,k_size * k_y + k_x,idy,idx) * offset_unit;

      const float p_x = static_cast<float>(idx + 0.5f) / dim_w * w + k_x + offset_h - 0.5f;
      const float p_y = static_cast<float>(idy + 0.5f) / dim_h * h + k_y + offset_v - 0.5f;
      const float alpha = p_x - floorf(p_x);
      const float beta = p_y - floorf(p_y);

      const int xL = max(min(int(floorf(p_x)), w + 2 * padding - 1), 0);
      const int xR = max(min(xL + 1, w + 2 * padding - 1), 0);
      const int yT = max(min(int(floorf(p_y)), h + 2 * padding - 1), 0);
      const int yB = max(min(yT + 1, h + 2 * padding - 1), 0);

      float val = (1.f - alpha) * (1.f - beta) * img(idb,idc,yT,xL);
      val += alpha * (1.f - beta) * img(idb,idc,yT,xR);
      val += (1.f - alpha) * beta * img(idb,idc,yB,xL);
      val += alpha * beta * img(idb,idc,yB,xR);
      result += val * kernels(idb,k_size * k_y + k_x,idy,idx);
    }
  }
  output(idb,idc,idy,idx) = result;
}

int main() {
  params p = {128, 3, 480, 640, 9, 1024, 1024};
  const int dim_b = p.output_dim_b;
  const int dim_c = p.output_dim_c;
  const int dim_h = p.output_dim_h;
  const int dim_w = p.output_dim_w;
  const int kernels_size = p.kernel_size;
  const int img_w = p.image_w;
  const int img_h = p.image_h;

  const int padding = 1;

  size_t image_size = dim_b * dim_c * (img_w + padding) * (img_h + padding);
  size_t offset_size = dim_b * kernels_size * dim_w * dim_h;
  size_t kernel_size = dim_b * kernels_size * dim_w * dim_h;
  size_t output_size = dim_b * dim_c * dim_w * dim_h;

  size_t image_size_byte = sizeof(float) * dim_b * dim_c * (img_w + padding) * (img_h + padding);
  size_t offset_size_byte = sizeof(float) * dim_b * kernels_size * dim_w * dim_h;
  size_t kernel_size_byte = sizeof(float) * dim_b * kernels_size * dim_w * dim_h;
  size_t output_size_byte = sizeof(float) * dim_b * dim_c * dim_w * dim_h;

  float *img = (float*) malloc (image_size_byte);
  float *offsets_h = (float*) malloc (offset_size_byte);
  float *offsets_v = (float*) malloc (offset_size_byte);
  float *kernel = (float*) malloc (kernel_size_byte);
  float *output = (float*) malloc (output_size_byte);
  float *output_ref = (float*) malloc (output_size_byte);

  unsigned long long seed = 123;
  for (size_t i = 0; i < image_size; i++) img[i] = (unsigned char)(256*LCG_random_double(&seed));
  for (size_t i = 0; i < kernel_size; i++) kernel[i] = (unsigned char)(256*LCG_random_double(&seed));
  for (size_t i = 0; i < offset_size; i++) {
    offsets_h[i] = LCG_random_double(&seed);
    offsets_v[i] = LCG_random_double(&seed);
  }

  float *d_img, *d_offsets_h, *d_offsets_v, *d_kernel, *d_output;
  hipMalloc((void**)&d_img, image_size_byte);
  hipMemcpy(d_img, img, image_size_byte, hipMemcpyHostToDevice);

  hipMalloc((void**)&d_offsets_h, offset_size_byte);
  hipMemcpy(d_offsets_h, offsets_h, offset_size_byte, hipMemcpyHostToDevice);

  hipMalloc((void**)&d_offsets_v, offset_size_byte);
  hipMemcpy(d_offsets_v, offsets_v, offset_size_byte, hipMemcpyHostToDevice);

  hipMalloc((void**)&d_kernel, kernel_size_byte);
  hipMemcpy(d_kernel, kernel, kernel_size_byte, hipMemcpyHostToDevice);

  hipMalloc((void**)&d_output, output_size_byte);

  dim3 grid ((output_size + 255) / 256);
  dim3 block (256);

  for (int i = 0; i < 100; i++) {
    hipLaunchKernelGGL(car, grid, block, 0, 0, 
        d_img,
        d_kernel,
        d_offsets_h,
        d_offsets_v,
        d_output,
        p,
        1, // offset_unit,
        padding,
        output_size);
  }

  reference (img, kernel, offsets_h, offsets_v, output_ref, p, 1, padding);

  hipMemcpy(output, d_output, output_size_byte, hipMemcpyDeviceToHost);

  float rmse = 0;
  for (size_t i = 0; i < output_size; i++)
    rmse += (output_ref[i] - output[i]) * (output_ref[i] - output[i]);
  printf("RMSE: %f\n", sqrtf(rmse/output_size));

  hipFree(d_img);
  hipFree(d_offsets_h);
  hipFree(d_offsets_v);
  hipFree(d_kernel);
  hipFree(d_output);

  free(img);
  free(offsets_h);
  free(offsets_v);
  free(kernel);
  free(output);
  free(output_ref);
  return 0;
}

