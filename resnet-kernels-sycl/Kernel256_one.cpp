#include "util.h"

const char inputName256one[] = "data/input_one_14_1024.bin";
const char weightName256one[] = "data/weight_one_1024.bin";
const char bnBias_myKernel_Name256one[] = "data/bnBias_myKernel_one_1024.bin";
const char bnScale_myKernel_Name256one[] = "data/bnScale_myKernel_one_1024.bin";

#define __syncthreads() item.barrier(access::fence_space::local_space)

void kernel_1024_one_256(
  nd_item<2> &item,
        float *__restrict shared_,
  const float *__restrict A,
  const float *__restrict B,
  const float *__restrict bnBias,
  const float *__restrict bnScale,
        float *__restrict C) 
{
  int tile = item.get_group(1), 
      in_channel = item.get_local_id(1),
      line = item.get_local_id(0);
  int ind = line*256 + in_channel;

  float *__restrict weights = shared_ + 1024*4,
        *__restrict output = weights + 256*16,
        *__restrict input = shared_;
  float *__restrict bias = output + 4*256,
        *__restrict scale = bias + 256;

  for (int i = 0; i < 4; i++)
    input[ind + i*1024] = A[tile*4096 + i*1024 + ind];
  bias[in_channel] = bnBias[in_channel];
  scale[in_channel] = bnScale[in_channel];
  output[ind] = 0.0f;
  __syncthreads();

  for (int k = 0; k < 1024; k += 16) {
    const float *B_start = B + k*256;
    for (int i = 0; i < 4; i++)
      weights[ind + i*1024] = B_start[i*1024 + ind];
    __syncthreads();

    const float *A_start = input + k;
    for (int p = 0; p < 16; p++) {
      output[ind] += A_start[line*1024 + p] * weights[in_channel + p*256];
    }
    __syncthreads();
  }

  float *C_start = C + tile*1024, res = scale[in_channel] * output[ind] + bias[in_channel];
  C_start[ind] = res > 0 ? res : 0;
}

void kernel_256_one_1024(
  nd_item<2> &item,
        float *__restrict shared_,
  const float *__restrict A,
  const float *__restrict B,
  const float *__restrict bnBias,
  const float *__restrict bnScale,
        float *__restrict C) 
{
  int tile = item.get_group(1), part = item.get_group(0),
      in_channel = item.get_local_id(1), line = item.get_local_id(0);
  int ind = line*256 + in_channel;

  float *weights = shared_ + 256*4, *output = weights + 256*32, *input = shared_;
  float *bias = output + 4*256, *scale = bias + 256;

  input[ind] = A[tile * 1024 + ind];
  bias[in_channel] = bnBias[part*256 + in_channel];
  scale[in_channel] = bnScale[part*256+ in_channel];
  output[ind] = 0.0f;
  __syncthreads();

  for (int k = 0; k < 256; k += 32) {
    for (int i = 0; i < 8; i++)
      weights[ind + 1024*i] = B[(k + i*4 + line)*1024 + part*256 + in_channel];
    __syncthreads();

    float *A_start = input + k;
    for (int p = 0; p < 32; p++) {
      output[ind] += A_start[line*256 + p] * weights[in_channel + p*256];
    }
    __syncthreads();
  }

  float *C_start = C + tile*4096 + part*256;
  C_start[line * 1024 + in_channel] = scale[in_channel] * output[ind] + bias[in_channel];
}

int kernel_256_1_in(queue &q) {
  float *input = get_parameter(inputName256one, 14*14*1024);
  float *weight = get_parameter(weightName256one, 256*1024);
  float *bnBias_myKernel = get_parameter(bnBias_myKernel_Name256one, 256);
  float *bnScale_myKernel = get_parameter(bnScale_myKernel_Name256one, 256);

  int nInput = 14*14*1024, nOutput = 14*14*256, nWeights = 256*1024;
  float result[nOutput];

  uint64_t nT1 = 0, nT2 = 0;

  nT1 = getTimeMicroseconds64();

  buffer<float, 1> input_(input, nInput);
  buffer<float, 1> output_(nOutput);
  buffer<float, 1> weight_(weight, nWeights);
  buffer<float, 1> bnBias_(bnBias_myKernel, 256);
  buffer<float, 1> bnScale_(bnScale_myKernel, 256);

  range<2> gws (4, 256*49);
  range<2> lws (4, 256);

  q.submit([&] (handler &cgh) {
    auto i = input_.get_access<sycl_read>(cgh);
    auto w = weight_.get_access<sycl_read>(cgh);
    auto b = bnBias_.get_access<sycl_read>(cgh);
    auto s = bnScale_.get_access<sycl_read>(cgh);
    auto o = output_.get_access<sycl_discard_write>(cgh);
    accessor<float, 1, sycl_read_write, access::target::local>
      sm (4*1024 + 16*256 + 4*256 + 2*256, cgh);
    cgh.parallel_for<class k1024>(nd_range<2>(gws, lws), [=] (nd_item<2> item) {
      kernel_1024_one_256 (item, sm.get_pointer(), i.get_pointer(),
        w.get_pointer(), b.get_pointer(), s.get_pointer(), o.get_pointer());
    });
  });

  q.submit([&] (handler &cgh) {
    auto acc = output_.get_access<sycl_read>(cgh);
    cgh.copy(acc, result);
  }).wait();

  nT2 = getTimeMicroseconds64();

  #ifdef DEBUG
  double s = 0;
  for (int i = 0; i < nOutput; i++) {
    s += result[i];
  }
  printf("Check sum: %lf\n", s);
  #endif

  free(input);
  free(weight);
  free(bnBias_myKernel);
  free(bnScale_myKernel);

  return ((nT2-nT1) << 16);
}

int kernel_256_1_out(queue &q) {
  float *input = get_parameter(inputName256one, 14*14*256);
  float *weight = get_parameter(weightName256one, 256*1024);
  float *bnBias_myKernel = get_parameter(bnBias_myKernel_Name256one, 1024);
  float *bnScale_myKernel = get_parameter(bnScale_myKernel_Name256one, 1024);

  int nInput = 14*14*256, nOutput = 14*14*1024, nWeights = 256*1024;
  float result[nOutput];

  uint64_t nT1 = 0, nT2 = 0;

  nT1 = getTimeMicroseconds64();

  buffer<float, 1> input_(input, nInput);
  buffer<float, 1> output_(nOutput);
  buffer<float, 1> weight_(weight, nWeights);
  buffer<float, 1> bnBias_(bnBias_myKernel, 1024);
  buffer<float, 1> bnScale_(bnScale_myKernel, 1024);

  range<2> gws (4*4, 256*49);
  range<2> lws (4, 256);

  q.submit([&] (handler &cgh) {
    auto i = input_.get_access<sycl_read>(cgh);
    auto w = weight_.get_access<sycl_read>(cgh);
    auto b = bnBias_.get_access<sycl_read>(cgh);
    auto s = bnScale_.get_access<sycl_read>(cgh);
    auto o = output_.get_access<sycl_discard_write>(cgh);
    accessor<float, 1, sycl_read_write, access::target::local>
      sm (4*256 + 32*256 + 4*256 + 2*256, cgh);
    cgh.parallel_for<class k256_1024>(nd_range<2>(gws, lws), [=] (nd_item<2> item) {
      kernel_256_one_1024 (item, sm.get_pointer(), i.get_pointer(),
        w.get_pointer(), b.get_pointer(), s.get_pointer(), o.get_pointer());
    });
  }).wait();

  nT2 = getTimeMicroseconds64();

  q.submit([&] (handler &cgh) {
    auto acc = output_.get_access<sycl_read>(cgh);
    cgh.copy(acc, result);
  }).wait();

  #ifdef DEBUG
  double s = 0;
  for (int i = 0; i < nOutput; i++) {
    s += result[i];
  }
  printf("Check sum: %lf\n", s);
  #endif

  free(bnBias_myKernel);
  free(bnScale_myKernel);
  free(input);
  free(weight);

  return ((nT2-nT1) << 16);
}
