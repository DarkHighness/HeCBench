/*
 * Copyright 1993-2015 NVIDIA Corporation.  All rights reserved.
 *
 * Please refer to the NVIDIA end user license agreement (EULA) associated
 * with this source code for terms and conditions that govern your use of
 * this software. Any use, reproduction, disclosure, or distribution of
 * this software and related documentation outside the terms of the EULA
 * is strictly prohibited.
 *
 */

/* A simple program demonstrating trivial use of global memory atomic
 * device functions (atomic*() functions).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <hip/hip_runtime.h>
#include "kernel.h"
#include "reference.h"

template <class T>
void testcase(const int repeat)
{
  unsigned int len = 1 << 27;
  unsigned int numThreads = 256;
  unsigned int numBlocks = (len + numThreads - 1) / numThreads;
  unsigned int numData = 7;
  unsigned int memSize = sizeof(T) * numData;
  T gpuData[] = {0, 0, (T)-256, 256, 255, 0, 255};

  // allocate device memory for result
  T *dOData;
  hipMalloc((void **) &dOData, memSize);

  for (int i = 0; i < repeat; i++) {
    // copy host memory to device to initialize to zero
    hipMemcpy(dOData, gpuData, memSize, hipMemcpyHostToDevice);

    // execute the kernel
    hipLaunchKernelGGL(HIP_KERNEL_NAME(testKernel<T>), numBlocks, numThreads, 0, 0, dOData);
  }

  //Copy result from device to host
  hipMemcpy(gpuData, dOData, memSize, hipMemcpyDeviceToHost);

  computeGold<T>(gpuData, numThreads * numBlocks);

  hipFree(dOData);
}

int main(int argc, char **argv)
{
  if (argc != 2) {
    printf("Usage: %s <repeat>\n", argv[0]);
    return 1;
  }

  const int repeat = atoi(argv[1]);
  testcase<int>(repeat);
  testcase<unsigned int>(repeat);
  return 0;
}
