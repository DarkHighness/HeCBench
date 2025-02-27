// Copyright (c) 2019-2020, NVIDIA CORPORATION.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "common.h"

///////////////////////////////////////////////////////////////////////////////
//                                SOSFILT                                    //
///////////////////////////////////////////////////////////////////////////////
#define MAX_THREADS 256
#define THREADS 32
#define sos_width  6   // https://www.mathworks.com/help/signal/ref/sosfilt.html 

// Forward declarations
template <typename T>
class sosfilter;

template <typename T>
void filtering (queue &q, const int n_signals, const int n_samples, const int n_sections, const int zi_width)
{
  // the number of second-order sections must be less than max threads per block
  assert(MAX_THREADS >= n_sections);

  // The number of samples must be greater than or equal to the number of sections
  assert(n_samples >= n_sections);

  // randomize input data
  srand(2);

  const int blocks = n_signals;

  // Second-order section digital filter
  const int sos_size = n_sections * sos_width ;

  T* sos = (T*) malloc (sizeof(T) * sos_size);
  for (int i = 0; i < n_sections; i++)
    for (int j = 0; j < sos_width; j++)
      sos[i*sos_width+j] = (T)1 ; // for test 

  // initial  conditions
  const int z_size = (n_sections + 1) * blocks * zi_width;
  T* zi = (T*) malloc (sizeof(T) * z_size);
  for (int i = 0; i < z_size; i++) zi[i] = (T)1; // for test

  // input signals
  const int x_size = n_signals * n_samples;
  T* x = (T*) malloc (sizeof(T) * x_size);
  for (int i = 0; i < n_signals; i++) 
    for (int j = 0; j < n_samples; j++) 
      x[i*n_samples+j] = (T)std::sin(2*3.14*(i+1+j));


  { // sycl scope

  buffer<T, 1> d_sos (sos, sos_size);
  buffer<T, 1> d_zi (zi, z_size);
  buffer<T, 1> d_x (x, x_size);
  range<2> gws (blocks, THREADS);
  range<2> lws (1, THREADS);
  const int out_size = n_sections;
  const int shared_mem_size = (out_size + z_size + sos_size);

  for (int n = 0; n < 100; n++) {
    q.submit([&] (handler &cgh) {
      auto zi = d_zi.template get_access<sycl_read>(cgh);
      auto sos = d_sos.template get_access<sycl_read>(cgh);
      auto x_in = d_x.template get_access<sycl_read_write>(cgh);
      accessor<T, 1, sycl_read_write, access::target::local> s_out (shared_mem_size, cgh);
      cgh.parallel_for<class sosfilter<T>>(nd_range<2>(gws, lws), [=] (nd_item<2> item) {

        T *s_zi =  &s_out[n_sections] ;
        T *s_sos = &s_zi[n_sections * zi_width] ;

        const int tx = static_cast<int>( item.get_local_id(1) ) ;
        const int ty = static_cast<int>( item.get_global_id(0) ) ;

        // Reset shared memory
        s_out[tx] = 0;

        // Load zi
        for ( int i = 0; i < zi_width; i++ ) {
          s_zi[tx * zi_width + i] = zi[ty * n_sections * zi_width + tx * zi_width + i];
        }

        // Load SOS
        #pragma unroll 
        for ( int i = 0; i < sos_width; i++ ) {
          s_sos[tx * sos_width + i] = sos[tx * sos_width + i];
        }

        item.barrier(access::fence_space::local_space);

        const int load_size = n_sections - 1 ;
        const int unload_size = n_samples - load_size ;

        T temp;
        T x_n;

        if ( ty < n_signals ) {
          // Loading phase
          for ( int n = 0; n < load_size; n++ ) {
            if ( tx == 0 ) {
              x_n = x_in[ty * n_samples + n];
            } else {
              x_n = s_out[tx - 1];
            }

            // Use direct II transposed structure
            temp = s_sos[tx * sos_width + 0] * x_n + s_zi[tx * zi_width + 0];

            s_zi[tx * zi_width + 0] =
              s_sos[tx * sos_width + 1] * x_n - s_sos[tx * sos_width + 4] * temp + s_zi[tx * zi_width + 1];

            s_zi[tx * zi_width + 1] = s_sos[tx * sos_width + 2] * x_n - s_sos[tx * sos_width + 5] * temp;

            s_out[tx] = temp;

            item.barrier(access::fence_space::local_space);
          }

          // Processing phase
          for ( int n = load_size; n < n_samples; n++ ) {
            if ( tx == 0 ) {
              x_n = x_in[ty * n_samples + n];
            } else {
              x_n = s_out[tx - 1];
            }

            // Use direct II transposed structure
            temp = s_sos[tx * sos_width + 0] * x_n + s_zi[tx * zi_width + 0];

            s_zi[tx * zi_width + 0] =
              s_sos[tx * sos_width + 1] * x_n - s_sos[tx * sos_width + 4] * temp + s_zi[tx * zi_width + 1];

            s_zi[tx * zi_width + 1] = s_sos[tx * sos_width + 2] * x_n - s_sos[tx * sos_width + 5] * temp;

            if ( tx < load_size ) {
              s_out[tx] = temp;
            } else {
              x_in[ty * n_samples + ( n - load_size )] = temp;
            }

            item.barrier(access::fence_space::local_space);
          }

          // Unloading phase
          for ( int n = 0; n < n_sections; n++ ) {
            // retire threads that are less than n
            if ( tx > n ) {
              x_n = s_out[tx - 1];

              // Use direct II transposed structure
              temp = s_sos[tx * sos_width + 0] * x_n + s_zi[tx * zi_width + 0];

              s_zi[tx * zi_width + 0] =
                s_sos[tx * sos_width + 1] * x_n - s_sos[tx * sos_width + 4] * temp + s_zi[tx * zi_width + 1];

              s_zi[tx * zi_width + 1] = s_sos[tx * sos_width + 2] * x_n - s_sos[tx * sos_width + 5] * temp;

              if ( tx < load_size ) {
                s_out[tx] = temp;
              } else {
                x_in[ty * n_samples + ( n + unload_size )] = temp;
              }
            }
            item.barrier(access::fence_space::local_space);
          }
        }
      });
    });
  }
  q.wait();

  }

#ifdef DEBUG
  for (int i = 0; i < n_signals; i++) { 
    for (int j = 0; j < n_samples; j++) 
      printf("%.2f ", x[i*n_samples+j]);
    printf("\n");
  }
#endif

  free(x);
  free(sos);
  free(zi);
}

int main(int argc, char** argv) {

  const int numSections = THREADS; 

#ifdef DEBUG
  const int numSignals = 2; 
  const int numSamples = THREADS+1;
#else
  // failed to launch the double-precision kernel when numSignals = 16 on a P100 GPU
  const int numSignals = 8;  
  const int numSamples = 100000;
#endif

#ifdef USE_GPU
  gpu_selector dev_sel;
#else
  cpu_selector dev_sel;
#endif
  queue q(dev_sel);

  const int zi_width = 2;
  filtering<float> (q, numSignals, numSamples, numSections, zi_width);
  filtering<double> (q, numSignals, numSamples, numSections, zi_width);
  return 0;
}
