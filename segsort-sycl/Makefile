#===============================================================================
# User Options
#===============================================================================

# Compiler can be set below, or via environment variable
CC        = clang++
OPTIMIZE  = yes
DEBUG     = no

GPU       = yes
CUDA      = no
CUDA_ARCH = sm_70
HIP       = no
HIP_ARCH  = gfx908
GCC_TOOLCHAIN = "/auto/software/gcc/x86_64/gcc-9.1.0/"

#===============================================================================
# Program name & source code list
#===============================================================================

program = main

source = main.cpp

obj = $(source:.cpp=.o)

headers = \
	src/bb_bin.dp.hpp \
	src/bb_comput_common.dp.hpp \
	src/bb_comput_l_keys.dp.hpp \
	src/bb_comput_l.dp.hpp \
	src/bb_comput_s_keys.dp.hpp \
	src/bb_comput_s.dp.hpp \
	src/bb_exch_keys.dp.hpp \
	src/bb_exch.dp.hpp \
	src/bb_segsort_common.dp.hpp \
	src/bb_segsort_keys.dp.hpp \
	src/bb_segsort.dp.hpp

#===============================================================================
# Sets Flags
#===============================================================================

# Standard Flags
CFLAGS := -std=c++17 -Wall -I../include -fsycl

# Linker Flags
LDFLAGS = 

ifeq ($(CUDA), yes)
  CFLAGS += -fsycl-targets=nvptx64-nvidia-cuda-sycldevice \
            -Xsycl-target-backend --cuda-gpu-arch=$(CUDA_ARCH) \
            --gcc-toolchain=$(GCC_TOOLCHAIN)
endif

ifeq ($(HIP), yes)
  CFLAGS += -fsycl-targets=amdgcn-amd-amdhsa \
	    -Xsycl-target-backend --offload-arch=$(HIP_ARCH) 
endif

# Debug Flags
ifeq ($(DEBUG),yes)
  CFLAGS  += -g -DDEBUG
  LDFLAGS += -g
endif

# Optimization Flags
ifeq ($(OPTIMIZE),yes)
  CFLAGS += -O3
endif

ifeq ($(GPU),yes)
  CFLAGS +=-DUSE_GPU
endif
#===============================================================================
# Targets to Build
#===============================================================================

$(program): $(obj)
	$(CC) $(CFLAGS) $(obj) -o $@ $(LDFLAGS)

%.o: %.cpp $(headers) 
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(program) $(obj)

run: $(program)
	./$(program)
