#pragma once

#include <cuda_runtime.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

inline void cuda_check(cudaError_t status, const char* expression, const char* file, int line) {
  if (status != cudaSuccess) {
    std::ostringstream message;
    message << file << ":" << line << " CUDA call failed: " << expression << ": "
            << cudaGetErrorString(status);
    throw std::runtime_error(message.str());
  }
}

#define CUDA_CHECK(expr) cuda_check((expr), #expr, __FILE__, __LINE__)

inline int visible_device_count() {
  int count = 0;
  CUDA_CHECK(cudaGetDeviceCount(&count));
  return count;
}

inline std::vector<int> default_visible_gpus() {
  const int count = visible_device_count();
  std::vector<int> gpus;
  gpus.reserve(static_cast<std::size_t>(count));
  for (int gpu = 0; gpu < count; ++gpu) {
    gpus.push_back(gpu);
  }
  return gpus;
}

inline void require_gpu_id(int gpu) {
  const int count = visible_device_count();
  if (gpu < 0 || gpu >= count) {
    std::ostringstream message;
    message << "GPU id " << gpu << " is outside visible range [0, " << count << ")";
    throw std::runtime_error(message.str());
  }
}
