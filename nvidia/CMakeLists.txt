cmake_minimum_required(VERSION 3.26)

project(nvidia_gb200_microbench LANGUAGES CXX CUDA)

if(NOT CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 100)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(CUDAToolkit REQUIRED)

add_executable(nvidia_gb200_probe
  src/main.cpp
  src/cli.cpp
  src/json.cpp
  src/env_probe.cpp
  src/fabric_smoke.cpp
  src/hbm_bandwidth.cu
  src/mem_chase.cu
  src/ptx_latency.cu
)

target_include_directories(nvidia_gb200_probe PRIVATE src)
target_link_libraries(nvidia_gb200_probe PRIVATE CUDA::cudart)
target_compile_options(nvidia_gb200_probe PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  $<$<COMPILE_LANGUAGE:CUDA>:--expt-relaxed-constexpr>
)

enable_testing()

add_test(
  NAME cli_contract
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli_contract.sh $<TARGET_FILE:nvidia_gb200_probe>
)

add_test(
  NAME env_json_contract
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_env_json_contract.sh $<TARGET_FILE:nvidia_gb200_probe>
)

add_test(
  NAME hbm_bandwidth_json_contract
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_hbm_bandwidth_json_contract.sh $<TARGET_FILE:nvidia_gb200_probe>
)

add_test(
  NAME sass_snippet_helper
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sass_snippet_helper.sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_sass_snippets.sh
)
