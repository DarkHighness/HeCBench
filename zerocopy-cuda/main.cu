/* Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of NVIDIA CORPORATION nor the names of its
    contributors may be used to endorse or promote products derived
    from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <math.h>
#include <cuda.h>

long long get_time() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000000) + tv.tv_usec;
}

/* Add two vectors on the GPU */
__global__ void vectorAddGPU(float *__restrict__ a,
                             float *__restrict__ b,
                             float *__restrict__ c,
                             int N) 
{
  int idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx < N) {
    c[idx] = a[idx] + b[idx];
  }
}

// Macro to aligned up to the memory size in question
#define MEMORY_ALIGNMENT 4096
#define ALIGN_UP(x, size) (((size_t)x + (size - 1)) & (~(size - 1)))

void eval (bool bPinGenericMemory) {
  int n, nelem;
  unsigned int flags;
  size_t bytes;
  float *a, *b, *c;           // Pinned memory allocated on the CPU
  float *a_UA, *b_UA, *c_UA;  // Non-4K Aligned Pinned memory on the CPU
  float *d_a, *d_b, *d_c;     // Device pointers for mapped memory
  float errorNorm, refNorm, ref, diff;


#if defined(__APPLE__) || defined(MACOSX)
    bPinGenericMemory = false;
    printf("Warning: Generic Pinning of System Paged memory is not support on MacOS\n");
#endif

  if (bPinGenericMemory) {
    printf("> Using Generic System Paged Memory (malloc)\n");
  } else {
    printf("> Using Host Allocated (cudaHostAlloc)\n");
  }

  /* Allocate mapped CPU memory. */

  for (nelem = 1024*1024; nelem <= (1024*1024*128); nelem = nelem*2) {
    bytes = nelem * sizeof(float);

    auto start = get_time();

    if (bPinGenericMemory) {
      // Allocate generic memory with malloc() and pin it later 
      // instead of using cudaHostAlloc()
      a_UA = (float *)malloc(bytes + MEMORY_ALIGNMENT);
      b_UA = (float *)malloc(bytes + MEMORY_ALIGNMENT);
      c_UA = (float *)malloc(bytes + MEMORY_ALIGNMENT);

      // We need to ensure memory is aligned to 4K (so we will need to padd memory
      // accordingly)
      a = (float *)ALIGN_UP(a_UA, MEMORY_ALIGNMENT);
      b = (float *)ALIGN_UP(b_UA, MEMORY_ALIGNMENT);
      c = (float *)ALIGN_UP(c_UA, MEMORY_ALIGNMENT);

      cudaHostRegister(a, bytes, cudaHostRegisterMapped);
      cudaHostRegister(b, bytes, cudaHostRegisterMapped);
      cudaHostRegister(c, bytes, cudaHostRegisterMapped);
    } else {
      flags = cudaHostAllocMapped;
      cudaHostAlloc((void **)&a, bytes, flags);
      cudaHostAlloc((void **)&b, bytes, flags);
      cudaHostAlloc((void **)&c, bytes, flags);
    }

    /* Initialize the vectors. */
    for (n = 0; n < nelem; n++) {
      a[n] = rand() / (float)RAND_MAX;
      b[n] = rand() / (float)RAND_MAX;
    }

    /* Get the device pointers for the pinned CPU memory mapped into the GPU
       memory space. */
    cudaHostGetDevicePointer((void **)&d_a, (void *)a, 0);
    cudaHostGetDevicePointer((void **)&d_b, (void *)b, 0);
    cudaHostGetDevicePointer((void **)&d_c, (void *)c, 0);

    /* Call the GPU kernel using the pointers residing in CPU mapped memory.
     */
    dim3 block(256);
    dim3 grid((unsigned int)ceil(nelem / (float)block.x));
    vectorAddGPU<<<grid, block>>>(d_a, d_b, d_c, nelem);
    cudaDeviceSynchronize();

    /* Compare the results */

    errorNorm = 0.f;
    refNorm = 0.f;

    for (n = 0; n < nelem; n++) {
      ref = a[n] + b[n];
      diff = c[n] - ref;
      errorNorm += diff * diff;
      refNorm += ref * ref;
    }

    errorNorm = (float)sqrt((double)errorNorm);
    refNorm = (float)sqrt((double)refNorm);

    printf("%s ", (errorNorm / refNorm < 1.e-6f) ? "SUCCESS" : "FAILURE");

    /* Memory clean up */


    if (bPinGenericMemory) {
      cudaHostUnregister(a);
      cudaHostUnregister(b);
      cudaHostUnregister(c);
      free(a_UA);
      free(b_UA);
      free(c_UA);
    } else {
      cudaFreeHost(a);
      cudaFreeHost(b);
      cudaFreeHost(c);
    }

    auto end = get_time();
    printf("Total elapsed time: %.2f s\n", (end - start) / 1e6f);
  }
}

int main(int argc, char **argv) {
  bool bPinGenericMemory;

  bPinGenericMemory = false;
  eval(bPinGenericMemory); 

  bPinGenericMemory = true;
  eval(bPinGenericMemory); 
  return 0;
}
