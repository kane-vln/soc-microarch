#include "probes.hpp"

#include "cuda_util.hpp"

#include <cuda_runtime.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kSmokeBytes = 256ull * 1024ull * 1024ull;

cudaError_t enable_peer_access_if_needed(int current, int peer) {
  CUDA_CHECK(cudaSetDevice(current));
  const cudaError_t status = cudaDeviceEnablePeerAccess(peer, 0);
  if (status == cudaErrorPeerAccessAlreadyEnabled) {
    (void)cudaGetLastError();
    return cudaSuccess;
  }
  return status;
}

Json failed_pair(int src, int dst, const std::string& reason) {
  Json pair = Json::object();
  pair["src"] = src;
  pair["dst"] = dst;
  pair["accessible"] = false;
  pair["status"] = "failed";
  pair["reason"] = reason;
  return pair;
}

Json copy_pair(int src, int dst, std::uint64_t bytes) {
  int can_access = 0;
  const cudaError_t access_status = cudaDeviceCanAccessPeer(&can_access, dst, src);
  if (access_status != cudaSuccess) {
    return failed_pair(src, dst, cudaGetErrorString(access_status));
  }
  if (!can_access) {
    return failed_pair(src, dst, "cudaDeviceCanAccessPeer returned false");
  }

  cudaError_t status = enable_peer_access_if_needed(dst, src);
  if (status != cudaSuccess) {
    return failed_pair(src, dst, cudaGetErrorString(status));
  }
  status = enable_peer_access_if_needed(src, dst);
  if (status != cudaSuccess) {
    return failed_pair(src, dst, cudaGetErrorString(status));
  }

  void* d_src = nullptr;
  void* d_dst = nullptr;
  CUDA_CHECK(cudaSetDevice(src));
  CUDA_CHECK(cudaMalloc(&d_src, bytes));
  CUDA_CHECK(cudaMemset(d_src, 0x5a, bytes));

  CUDA_CHECK(cudaSetDevice(dst));
  CUDA_CHECK(cudaMalloc(&d_dst, bytes));
  CUDA_CHECK(cudaMemset(d_dst, 0, bytes));

  cudaEvent_t start{};
  cudaEvent_t stop{};
  CUDA_CHECK(cudaEventCreate(&start));
  CUDA_CHECK(cudaEventCreate(&stop));

  CUDA_CHECK(cudaEventRecord(start));
  CUDA_CHECK(cudaMemcpyPeerAsync(d_dst, dst, d_src, src, bytes));
  CUDA_CHECK(cudaEventRecord(stop));
  CUDA_CHECK(cudaEventSynchronize(stop));

  float elapsed_ms = 0.0f;
  CUDA_CHECK(cudaEventElapsedTime(&elapsed_ms, start, stop));

  CUDA_CHECK(cudaEventDestroy(stop));
  CUDA_CHECK(cudaEventDestroy(start));
  CUDA_CHECK(cudaFree(d_dst));
  CUDA_CHECK(cudaSetDevice(src));
  CUDA_CHECK(cudaFree(d_src));

  const double seconds = static_cast<double>(elapsed_ms) / 1000.0;
  const double gb_per_second = seconds > 0.0 ? (static_cast<double>(bytes) / 1.0e9) / seconds : 0.0;

  Json pair = Json::object();
  pair["src"] = src;
  pair["dst"] = dst;
  pair["accessible"] = true;
  pair["status"] = "ok";
  pair["bytes"] = bytes;
  pair["elapsed_ms"] = static_cast<double>(elapsed_ms);
  pair["gb_per_second"] = gb_per_second;
  return pair;
}

std::vector<int> selected_gpus(const Options& options) {
  if (!options.gpus.empty()) {
    for (const int gpu : options.gpus) {
      require_gpu_id(gpu);
    }
    return options.gpus;
  }
  return default_visible_gpus();
}

}  // namespace

Json run_fabric_smoke_probe(const Options& options, std::vector<std::string>& warnings) {
  const auto gpus = selected_gpus(options);
  if (gpus.size() < 2) {
    warnings.push_back("fabric-smoke requires at least two visible GPUs");
  }

  Json gpu_list = Json::array();
  for (const int gpu : gpus) {
    gpu_list.push_back(gpu);
  }

  Json pairs = Json::array();
  for (const int src : gpus) {
    for (const int dst : gpus) {
      if (src == dst) {
        continue;
      }
      pairs.push_back(copy_pair(src, dst, kSmokeBytes));
    }
  }

  Json parameters = Json::object();
  parameters["probe"] = "fabric-smoke";
  parameters["gpus"] = gpu_list;
  parameters["mode"] = options.mode;
  parameters["bytes_per_copy"] = kSmokeBytes;
  parameters["copy_direction"] = "ordered GPU pairs";

  Json validation = Json::object();
  validation["topology_command"] = "nvidia-smi topo -m";
  validation["measurement"] = "cudaMemcpyPeerAsync timed with destination-device CUDA events";

  Json result = Json::object();
  result["parameters"] = parameters;
  result["raw_samples"] = pairs;
  result["summary"] = pairs;
  result["validation"] = validation;
  return result;
}
