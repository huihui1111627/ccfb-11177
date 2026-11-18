# Query Executor with runtime code generation (JIT)
CXX ?= clang++
CC  ?= cc

CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter
LDFLAGS  ?=
LDLIBS   ?= -lpthread

# macOS needs -ldl? no, dlopen is in libSystem. Linux needs -ldl.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  LDLIBS += -ldl
endif

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:.cpp=.o)
DEP := $(SRC:.cpp=.d)

qex: $(OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEP)

clean:
	rm -f src/*.o src/*.d qex

.PHONY: clean
