# meshoptimizer 0.23 (https://github.com/zeux/meshoptimizer, tag v0.23, 3e9d1ff; MIT, LICENSE.md): the simplifier and
# the vertex cache optimizer only, for the BAR Droid branch's model simplification (IModelParser.cpp, BARDROID_SIM)
ADD_LIBRARY(meshoptimizer STATIC EXCLUDE_FROM_ALL allocator.cpp simplifier.cpp vcacheoptimizer.cpp)
SET_TARGET_PROPERTIES(meshoptimizer PROPERTIES COMPILE_FLAGS "${PIC_FLAG}")
