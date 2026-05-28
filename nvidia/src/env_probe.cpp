#include "probes.hpp"

#include "cuda_util.hpp"

#include <cuda_runtime.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <string>

namespace {

std::string cuda_version_string(int version) {
  const int major = version / 1000;
  const int minor = (version % 1000) / 10;
  return std::to_string(major) + "." + std::to_string(minor);
}

std::string hostname() {
  std::array<char, 256> buffer{};
  if (gethostname(buffer.data(), buffer.size() - 1) != 0) {
    return "unknown";
  }
  return buffer.data();
}

int device_attribute(int gpu, cudaDeviceAttr attribute) {
  int value = -1;
  const cudaError_t status = cudaDeviceGetAttribute(&value, attribute, gpu);
  if (status != cudaSuccess) {
    (void)cudaGetLastError();
    return -1;
  }
  return value;
}

Json device_json(int gpu) {
  cudaDeviceProp prop{};
  CUDA_CHECK(cudaGetDeviceProperties(&prop, gpu));

  char pci_bus_id[64]{};
  CUDA_CHECK(cudaDeviceGetPCIBusId(pci_bus_id, sizeof(pci_bus_id), gpu));

  Json device = Json::object();
  device["index"] = gpu;
  device["name"] = prop.name;
  device["pci_bus_id"] = pci_bus_id;
  device["compute_capability_major"] = prop.major;
  device["compute_capability_minor"] = prop.minor;
  device["total_global_mem_bytes"] = static_cast<std::uint64_t>(prop.totalGlobalMem);
  device["multi_processor_count"] = device_attribute(gpu, cudaDevAttrMultiProcessorCount);
  device["clock_rate_khz"] = device_attribute(gpu, cudaDevAttrClockRate);
  device["memory_clock_rate_khz"] = device_attribute(gpu, cudaDevAttrMemoryClockRate);
  device["memory_bus_width_bits"] = device_attribute(gpu, cudaDevAttrGlobalMemoryBusWidth);
  device["l2_cache_size_bytes"] = device_attribute(gpu, cudaDevAttrL2CacheSize);
  device["shared_mem_per_block_bytes"] = device_attribute(gpu, cudaDevAttrMaxSharedMemoryPerBlock);
  device["shared_mem_per_multiprocessor_bytes"] =
      device_attribute(gpu, cudaDevAttrMaxSharedMemoryPerMultiprocessor);
  device["regs_per_block"] = device_attribute(gpu, cudaDevAttrMaxRegistersPerBlock);
  device["warp_size"] = device_attribute(gpu, cudaDevAttrWarpSize);
  device["async_engine_count"] = device_attribute(gpu, cudaDevAttrAsyncEngineCount);
  device["can_map_host_memory"] =
      static_cast<bool>(device_attribute(gpu, cudaDevAttrCanMapHostMemory));
  device["unified_addressing"] =
      static_cast<bool>(device_attribute(gpu, cudaDevAttrUnifiedAddressing));
  return device;
}

}  // namespace

Json collect_devices() {
  Json devices = Json::array();
  const int count = visible_device_count();
  for (int gpu = 0; gpu < count; ++gpu) {
    devices.push_back(device_json(gpu));
  }
  return devices;
}

Json run_env_probe(const Options& options, std::vector<std::string>& warnings) {
  (void)options;

  int driver_version = 0;
  int runtime_version = 0;
  CUDA_CHECK(cudaDriverGetVersion(&driver_version));
  CUDA_CHECK(cudaRuntimeGetVersion(&runtime_version));

  const int count = visible_device_count();
  Json matrix = Json::array();
  for (int src = 0; src < count; ++src) {
    Json row = Json::array();
    for (int dst = 0; dst < count; ++dst) {
      int can_access = src == dst ? 1 : 0;
      if (src != dst) {
        const cudaError_t status = cudaDeviceCanAccessPeer(&can_access, src, dst);
        if (status != cudaSuccess) {
          warnings.push_back("cudaDeviceCanAccessPeer failed for " + std::to_string(src) + "->" +
                             std::to_string(dst) + ": " + cudaGetErrorString(status));
          can_access = 0;
        }
      }
      Json cell = Json::object();
      cell["src"] = src;
      cell["dst"] = dst;
      cell["can_access"] = static_cast<bool>(can_access);
      row.push_back(cell);
    }
    matrix.push_back(row);
  }

  Json validation = Json::object();
  validation["device_query"] = "CUDA runtime device properties via cudaGetDeviceProperties";
  validation["topology_command"] = "nvidia-smi topo -m";
  validation["sass_command"] = "cuobjdump --dump-sass build/nvidia_gb200_probe";

  Json parameters = Json::object();
  parameters["probe"] = "env";

  Json result = Json::object();
  result["parameters"] = parameters;
  result["hostname"] = hostname();
  result["cuda_driver_version"] = driver_version;
  result["cuda_driver_version_string"] = cuda_version_string(driver_version);
  result["cuda_runtime_version"] = runtime_version;
  result["cuda_runtime_version_string"] = cuda_version_string(runtime_version);
  result["visible_device_count"] = count;
  result["p2p_access_matrix"] = matrix;
  result["validation"] = validation;
  return result;
}
