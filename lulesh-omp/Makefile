#===============================================================================
# User Options
#===============================================================================

# Compiler can be set below, or via environment variable
CC        = icpc
OPTIMIZE  = yes
DEBUG     = no
DEVICE    = gpu
VERIFY    = no


#===============================================================================
# Program name & source code list
#===============================================================================

program = lulesh

source = lulesh.cc \
        lulesh-viz.cc \
        lulesh-util.cc \
        lulesh-init.cc

obj = $(source:.cc=.o)

#===============================================================================
# Sets Flags
#===============================================================================

# Standard Flags
CFLAGS := -std=c++14 -Wall

# Linker Flags
LDFLAGS = 

# Debug Flags
ifeq ($(DEBUG),yes)
  CFLAGS += -g
  LDFLAGS  += -g
endif

# Verify kernel results
ifeq ($(VERIFY),yes)
  CFLAGS += -DVERIFY
endif

# Optimization Flags
ifeq ($(OPTIMIZE),yes)
  CFLAGS += -O3
endif

ifeq ($(DEVICE),gpu)
  CFLAGS +=-qnextgen -fiopenmp -fopenmp-targets=spir64 -D__STRICT_ANSI__
else
  CFLAGS +=-qopenmp
endif
#===============================================================================
# Targets to Build
#===============================================================================

$(program): $(obj)
	$(CC) $(CFLAGS) $(obj) -o $@ $(LDFLAGS)

%.o: %.cc lulesh.h 
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(program) $(obj)

test: $(program)
	./$(program) -i 1 -s 32 -r 11 -b 1 -c 1

run: $(program)
	./$(program) -i 100 -s 128 -r 11 -b 1 -c 1

