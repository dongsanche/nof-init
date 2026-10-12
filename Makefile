CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g -I./src
LDLIBS   ?= -libverbs

# library modules (add new .cpp here as they are created)
LIB_SRC := src/verbs.cpp src/cm.cpp

APPS := m0_selfpair m1_cm nof_init unit_test robust_test stress_init lifecycle_test keepalive_test multi_queue_test bench abort_shutdown_test reconnect_test

all: $(APPS)

%: test/%.cpp $(LIB_SRC) $(wildcard src/*.hpp)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIB_SRC) $(LDLIBS)

clean:
	rm -f $(APPS)

.PHONY: all clean
