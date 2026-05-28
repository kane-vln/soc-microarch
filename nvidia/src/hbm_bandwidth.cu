#include "probes.hpp"

#include "cuda_util.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace {

constexpr std::uint64_t kSmokeBytes = 512ull * 1024ull * 1024ull;
constexpr int kThreads = 256;
constexpr int kMaxBlocks = 8192;

__global__ void write_kernel(float4* dst, std::size_t elements, float seed) {
  const std::size_t tid = blockIdx.x * blockDim.x + threadIdx.x;
  const std::size_t stride = gridDim.x * blockDim.x;
  const float4 value = make_float4(seed, seed + 1.0f, seed + 2.0f, seed + 3.0f);
  for (std::size_t i = tid; i < elements; i += stride) {
    dst[i] = value;
  }
}

__global__ void copy_kernel(const float4* src, float4* dst, std::size_t elements) {
  const std::size_t tid = blockIdx.x * blockDim.x + threadIdx.x;
  const std::size_t stride = gridDim.x * blockDim.x;
  for (std::size_t i = tid; i < elements; i += stride) {
    dst[i] = src[i];
  }
}

__global__ void read_kernel(const float4* src, float* partial, std::size_t elements) {
  const std::size_t tid = blockIdx.x * blockDim.x + threadIdx.x;
  const std::size_t stride = gridDim.x * blockDim.x;
  float sum = 0.0f;
  for (std::size_t i = tid; i < elements; i += stride) {
    const float4 value = src[i];
    sum += value.x + value.y + value.z + value.w;
  }
  partial[tid] = sum;
}

template <typename Kernel>
float time_kernel(Kernel launch) {
  cudaEvent_t start{};
  cudaEvent_t stop{};
  CUDA_CHECK(cudaEventCreate(&start));
  CUDA_CHECK(cudaEventCreate(&stop));
  launch();
  CUDA_CHECK(cudaGetLastError());
  CUDA_CHECK(cudaDeviceSynchronize());
  CUDA_CHECK(cudaEventRecord(start));
  launch();
  CUDA_CHECK(cudaGetLastError());
  CUDA_CHECK(cudaEventRecord(stop));
  CUDA_CHECK(cudaEventSynchronize(stop));
  float elapsed_ms = 0.0f;
  CUDA_CHECK(cudaEventElapsedTime(&elapsed_ms, start, stop));
  CUDA_CHECK(cudaEventDestroy(stop));
  CUDA_CHECK(cudaEventDestroy(start));
  return elapsed_ms;
}

Json bandwidth_item(const std::string& operation, std::uint64_t payload_bytes,
                    std::uint64_t estimated_dram_bytes, float elapsed_ms) {
  const double seconds = static_cast<double>(elapsed_ms) / 1000.0;
  Json item = Json::object();
  item["operation"] = operation;
  item["bytes"] = payload_bytes;
  item["estimated_dram_bytes"] = estimated_dram_bytes;
  item["elapsed_ms"] = static_cast<double>(elapsed_ms);
  item["gb_per_second"] = seconds > 0.0 ? (static_cast<double>(payload_bytes) / 1.0e9) / seconds : 0.0;
  item["estimated_dram_gb_per_second"] =
      seconds > 0.0 ? (static_cast<double>(estimated_dram_bytes) / 1.0e9) / seconds : 0.0;
  return item;
}

}  // namespace

Json run_hbm_bandwidth_probe(const Options& options, std::vector<std::string>& warnings) {
  (void)warnings;
  require_gpu_id(options.gpu);
  CUDA_CHECK(cudaSetDevice(options.gpu));

  const std::uint64_t requested_bytes = options.bytes > 0 ? options.bytes : kSmokeBytes;
  const std::size_t elements =
      std::max<std::size_t>(1, static_cast<std::size_t>(requested_bytes / sizeof(float4)));
  const std::uint64_t payload_bytes = static_cast<std::uint64_t>(elements * sizeof(float4));
  const int blocks =
      std::min<int>(kMaxBlocks, static_cast<int>((elements + kThreads - 1) / kThreads));
  const int partial_elements = blocks * kThreads;

  float4* d_src = nullptr;
  float4* d_dst = nullptr;
  float* d_partial = nullptr;
  CUDA_CHECK(cudaMalloc(&d_src, payload_bytes));
  CUDA_CHECK(cudaMalloc(&d_dst, payload_bytes));
  CUDA_CHECK(cudaMalloc(&d_partial, static_cast<std::size_t>(partial_elements) * sizeof(float)));

  const float write_ms = time_kernel([&] { write_kernel<<<blocks, kThreads>>>(d_dst, elements, 1.0f); });
  const float copy_ms = time_kernel([&] { copy_kernel<<<blocks, kThreads>>>(d_dst, d_src, elements); });
  const float read_ms = time_kernel([&] { read_kernel<<<blocks, kThreads>>>(d_src, d_partial, elements); });

  CUDA_CHECK(cudaFree(d_partial));
  CUDA_CHECK(cudaFree(d_dst));
  CUDA_CHECK(cudaFree(d_src));

  Json read = bandwidth_item("read", payload_bytes, payload_bytes, read_ms);
  Json write = bandwidth_item("write", payload_bytes, payload_bytes, write_ms);
  Json copy = bandwidth_item("copy", payload_bytes, payload_bytes * 2, copy_ms);

  Json raw_samples = Json::array();
  raw_samples.push_back(read);
  raw_samples.push_back(write);
  raw_samples.push_back(copy);

  Json summary = Json::object();
  summary["read"] = read;
  summary["write"] = write;
  summary["copy"] = copy;

  Json parameters = Json::object();
  parameters["probe"] = "hbm-bandwidth";
  parameters["gpu"] = options.gpu;
  parameters["mode"] = options.mode;
  parameters["bytes"] = payload_bytes;
  parameters["threads_per_block"] = kThreads;
  parameters["blocks"] = blocks;
  parameters["vector_type"] = "float4";

  Json validation = Json::object();
  validation["measurement"] = "CUDA events around one warmed-up grid-stride kernel launch";
  validation["copy_note"] =
      "copy.gb_per_second reports payload bytes/s; estimated_dram_gb_per_second counts read+write bytes";

  Json result = Json::object();
  result["parameters"] = parameters;
  result["raw_samples"] = raw_samples;
  result["summary"] = summary;
  result["validation"] = validation;
  return result;
}
