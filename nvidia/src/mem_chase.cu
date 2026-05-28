#include "probes.hpp"

#include "cuda_util.hpp"

#include <cuda_runtime.h>

#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace {

constexpr int kSmokeIterations = 1000000;

__global__ void pointer_chase_kernel(const std::uint32_t* next, std::uint64_t* cycles,
                                     std::uint32_t* sink, int iterations) {
  std::uint32_t index = 0;
  const std::uint64_t start = clock64();
#pragma unroll 1
  for (int i = 0; i < iterations; ++i) {
    index = next[index];
  }
  const std::uint64_t stop = clock64();
  cycles[0] = stop - start;
  sink[0] = index;
}

std::uint32_t coprime_stride(std::uint32_t elements) {
  std::uint32_t stride = elements > 2 ? (elements / 2u) - 1u : 1u;
  if (stride == 0) {
    stride = 1;
  }
  while (std::gcd(stride, elements) != 1u) {
    --stride;
  }
  return stride;
}

Json run_size(std::uint64_t bytes, int iterations) {
  const auto elements = static_cast<std::uint32_t>(bytes / sizeof(std::uint32_t));
  const std::uint32_t stride = coprime_stride(elements);

  std::vector<std::uint32_t> host_next(elements);
  for (std::uint32_t i = 0; i < elements; ++i) {
    host_next[i] = (i + stride) % elements;
  }

  std::uint32_t* d_next = nullptr;
  std::uint64_t* d_cycles = nullptr;
  std::uint32_t* d_sink = nullptr;
  CUDA_CHECK(cudaMalloc(&d_next, bytes));
  CUDA_CHECK(cudaMalloc(&d_cycles, sizeof(std::uint64_t)));
  CUDA_CHECK(cudaMalloc(&d_sink, sizeof(std::uint32_t)));
  CUDA_CHECK(cudaMemcpy(d_next, host_next.data(), bytes, cudaMemcpyHostToDevice));

  pointer_chase_kernel<<<1, 1>>>(d_next, d_cycles, d_sink, iterations);
  CUDA_CHECK(cudaGetLastError());
  CUDA_CHECK(cudaDeviceSynchronize());

  pointer_chase_kernel<<<1, 1>>>(d_next, d_cycles, d_sink, iterations);
  CUDA_CHECK(cudaGetLastError());
  CUDA_CHECK(cudaDeviceSynchronize());

  std::uint64_t cycles = 0;
  std::uint32_t sink = 0;
  CUDA_CHECK(cudaMemcpy(&cycles, d_cycles, sizeof(cycles), cudaMemcpyDeviceToHost));
  CUDA_CHECK(cudaMemcpy(&sink, d_sink, sizeof(sink), cudaMemcpyDeviceToHost));

  CUDA_CHECK(cudaFree(d_sink));
  CUDA_CHECK(cudaFree(d_cycles));
  CUDA_CHECK(cudaFree(d_next));

  Json sample = Json::object();
  sample["bytes"] = bytes;
  sample["elements"] = static_cast<std::uint64_t>(elements);
  sample["stride"] = static_cast<std::uint64_t>(stride);
  sample["iterations"] = iterations;
  sample["cycles"] = cycles;
  sample["cycles_per_load"] = static_cast<double>(cycles) / iterations;
  sample["sink"] = static_cast<std::uint64_t>(sink);
  return sample;
}

}  // namespace

Json run_mem_chase_probe(const Options& options, std::vector<std::string>& warnings) {
  (void)warnings;
  require_gpu_id(options.gpu);
  CUDA_CHECK(cudaSetDevice(options.gpu));

  const int iterations = options.iterations > 0 ? options.iterations : kSmokeIterations;
  const std::vector<std::uint64_t> default_sizes = {
      4ull * 1024ull,
      32ull * 1024ull,
      256ull * 1024ull,
      1ull * 1024ull * 1024ull,
      4ull * 1024ull * 1024ull,
      8ull * 1024ull * 1024ull,
      32ull * 1024ull * 1024ull,
      128ull * 1024ull * 1024ull,
      256ull * 1024ull * 1024ull,
  };
  const std::vector<std::uint64_t>& sizes =
      options.mem_sizes.empty() ? default_sizes : options.mem_sizes;

  Json samples = Json::array();
  Json summary = Json::object();
  for (const auto bytes : sizes) {
    Json sample = run_size(bytes, iterations);
    summary[std::to_string(bytes)] = sample;
    samples.push_back(sample);
  }

  Json parameters = Json::object();
  parameters["probe"] = "mem-chase";
  parameters["gpu"] = options.gpu;
  parameters["mode"] = options.mode;
  parameters["iterations"] = iterations;
  parameters["timing"] = "%clock64";
  Json working_sets = Json::array();
  for (const auto bytes : sizes) {
    working_sets.push_back(bytes);
  }
  parameters["working_set_bytes"] = working_sets;

  Json validation = Json::object();
  validation["access_pattern"] = "single-thread dependent pointer ring";
  validation["cache_boundary_note"] =
      "V2 smoke sizes sweep small cache, intermediate cache, L2-scale, and HBM-scale regions";

  Json result = Json::object();
  result["parameters"] = parameters;
  result["raw_samples"] = samples;
  result["summary"] = summary;
  result["validation"] = validation;
  return result;
}
