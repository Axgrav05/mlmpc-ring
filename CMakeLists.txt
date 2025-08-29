cmake_minimum_required(VERSION 3.16)

project(mlmpmc_ring
  VERSION 0.1.0
  LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option for CUDA demos (we’ll keep it OFF while we finish CPU)
option(ENABLE_CUDA "Build CUDA demos" OFF)

# Header-only interface lib for our ring headers
add_library(ring INTERFACE)
target_include_directories(ring INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ring INTERFACE cxx_std_20)

# Executables
add_executable(ring_main src/main.cpp)
target_link_libraries(ring_main PRIVATE ring)

add_executable(test_correctness tests/test_correctness.cpp)
target_link_libraries(test_correctness PRIVATE ring)

add_executable(bench_throughput benchmarks/bench_throughput.cpp)
target_link_libraries(bench_throughput PRIVATE ring)

add_executable(cpu_demo demos/cpu_demo.cpp)
target_link_libraries(cpu_demo PRIVATE ring)

# Optional CUDA demo target
if(ENABLE_CUDA)
  enable_language(CUDA)
  add_executable(gpu_demo demos/gpu_demo.cu)
  # RTX 4060 (Ada Lovelace) is compute capability 8.9
  set_target_properties(gpu_demo PROPERTIES CUDA_ARCHITECTURES "89")
endif()

if (MSVC)
  add_compile_options(
    /O2 /DNDEBUG /EHsc 
    /permissive-
  )
  add_link_options(/LTCG)
  add_compile_options(/Ot /fp:fast)
endif()
