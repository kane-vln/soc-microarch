#include "probes.hpp"

#include "cuda_util.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace {

constexpr int kSmokeIterations = 1000000;
constexpr int kSamples = 5;

__global__ void bench_add_s32(std::uint64_t* cycles, std::uint32_t* sink, int iterations) {
  std::uint32_t x = static_cast<std::uint32_t>(threadIdx.x + blockIdx.x + 1);
  const std::uint64_t start = clock64();
#pragma unroll 1
  for (int i = 0; i < iterations; ++i) {
    asm volatile("add.s32 %0, %0, 1;" : "+r"(x));
  }
  const std::uint64_t stop = clock64();
  cycles[0] = stop - start;
  sink[0] = x;
}

__global__ void bench_add_u64(std::uint64_t* cycles, std::uint64_t* sink, int iterations) {
  std::uint64_t x = static_cast<std::uint64_t>(threadIdx.x + blockIdx.x + 1);
  const std::uint64_t start = clock64();
#pragma unroll 1
  for (int i = 0; i < iterations; ++i) {
    asm volatile("add.u64 %0, %0, 1;" : "+l"(x));
  }
  const std::uint64_t stop = clock64();
  cycles[0] = stop - start;
  sink[0] = x;
}

__global__ void bench_fma_f32(std::uint64_t* cycles, float* sink, int iterations) {
  float x = 1.0f + static_cast<float>(threadIdx.x + blockIdx.x) * 0.000001f;
  const float a = 1.000001f + static_cast<float>(threadIdx.x) * 0.0000001f;
  const float b = 0.000001f + static_cast<float>(blockIdx.x) * 0.0000001f;
  const std::uint64_t start = clock64();
#pragma unroll 1
  for (int i = 0; i < iterations; ++i) {
    asm volatile("fma.rn.f32 %0, %0, %1, %2;" : "+f"(x) : "f"(a), "f"(b));
  }
  const std::uint64_t stop = clock64();
  cycles[0] = stop - start;
  sink[0] = x;
}

__global__ void bench_loop_overhead(std::uint64_t* cycles, std::uint32_t* sink, int iterations) {
  std::uint32_t x = static_cast<std::uint32_t>(threadIdx.x + blockIdx.x);
  const std::uint64_t start = clock64();
#pragma unroll 1
  for (int i = 0; i < iterations; ++i) {
    asm volatile("" : "+r"(x));
  }
  const std::uint64_t stop = clock64();
  cycles[0] = stop - start;
  sink[0] = x;
}

struct InstructionSummary {
  std::string instruction;
  std::vector<std::uint64_t> cycles;
};

template <typename SinkT, typename Kernel>
InstructionSummary run_instruction(const std::string& instruction, Kernel kernel, int iterations) {
  std::uint64_t* d_cycles = nullptr;
  SinkT* d_sink = nullptr;
  CUDA_CHECK(cudaMalloc(&d_cycles, sizeof(std::uint64_t)));
  CUDA_CHECK(cudaMalloc(&d_sink, sizeof(SinkT)));

  kernel<<<1, 1>>>(d_cycles, d_sink, iterations);
  CUDA_CHECK(cudaGetLastError());
  CUDA_CHECK(cudaDeviceSynchronize());

  InstructionSummary summary;
  summary.instruction = instruction;
  summary.cycles.reserve(kSamples);
  for (int sample = 0; sample < kSamples; ++sample) {
    kernel<<<1, 1>>>(d_cycles, d_sink, iterations);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    std::uint64_t host_cycles = 0;
    CUDA_CHECK(cudaMemcpy(&host_cycles, d_cycles, sizeof(host_cycles), cudaMemcpyDeviceToHost));
    summary.cycles.push_back(host_cycles);
  }

  CUDA_CHECK(cudaFree(d_sink));
  CUDA_CHECK(cudaFree(d_cycles));
  return summary;
}

std::vector<std::uint64_t> run_loop_overhead(int iterations) {
  std::uint64_t* d_cycles = nullptr;
  std::uint32_t* d_sink = nullptr;
  CUDA_CHECK(cudaMalloc(&d_cycles, sizeof(std::uint64_t)));
  CUDA_CHECK(cudaMalloc(&d_sink, sizeof(std::uint32_t)));

  bench_loop_overhead<<<1, 1>>>(d_cycles, d_sink, iterations);
  CUDA_CHECK(cudaGetLastError());
  CUDA_CHECK(cudaDeviceSynchronize());

  std::vector<std::uint64_t> cycles;
  cycles.reserve(kSamples);
  for (int sample = 0; sample < kSamples; ++sample) {
    bench_loop_overhead<<<1, 1>>>(d_cycles, d_sink, iterations);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    std::uint64_t host_cycles = 0;
    CUDA_CHECK(cudaMemcpy(&host_cycles, d_cycles, sizeof(host_cycles), cudaMemcpyDeviceToHost));
    cycles.push_back(host_cycles);
  }

  CUDA_CHECK(cudaFree(d_sink));
  CUDA_CHECK(cudaFree(d_cycles));
  return cycles;
}

std::uint64_t corrected_cycles(std::uint64_t raw, std::uint64_t overhead) {
  return raw > overhead ? raw - overhead : 0;
}

Json raw_samples_json(const std::vector<InstructionSummary>& summaries,
                      const std::vector<std::uint64_t>& overhead_cycles, int iterations) {
  Json samples = Json::array();
  for (std::size_t i = 0; i < overhead_cycles.size(); ++i) {
    Json sample = Json::object();
    sample["instruction"] = "loop-overhead";
    sample["sample"] = static_cast<int>(i);
    sample["cycles"] = static_cast<std::uint64_t>(overhead_cycles[i]);
    sample["cycles_per_iteration"] = static_cast<double>(overhead_cycles[i]) / iterations;
    samples.push_back(sample);
  }
  for (const auto& summary : summaries) {
    for (std::size_t i = 0; i < summary.cycles.size(); ++i) {
      const std::uint64_t overhead = overhead_cycles[i % overhead_cycles.size()];
      const std::uint64_t corrected = corrected_cycles(summary.cycles[i], overhead);
      Json sample = Json::object();
      sample["instruction"] = summary.instruction;
      sample["sample"] = static_cast<int>(i);
      sample["cycles"] = static_cast<std::uint64_t>(summary.cycles[i]);
      sample["overhead_cycles"] = static_cast<std::uint64_t>(overhead);
      sample["corrected_cycles"] = static_cast<std::uint64_t>(corrected);
      sample["cycles_per_op"] = static_cast<double>(summary.cycles[i]) / iterations;
      sample["corrected_cycles_per_op"] = static_cast<double>(corrected) / iterations;
      samples.push_back(sample);
    }
  }
  return samples;
}

Json summary_json(const std::vector<InstructionSummary>& summaries,
                  const std::vector<std::uint64_t>& overhead_cycles, int iterations) {
  Json summary_object = Json::object();
  const auto overhead_total =
      std::accumulate(overhead_cycles.begin(), overhead_cycles.end(), std::uint64_t{0});
  const double overhead_avg = static_cast<double>(overhead_total) / overhead_cycles.size();

  Json overhead = Json::object();
  overhead["instruction"] = "loop-overhead";
  overhead["avg_cycles"] = overhead_avg;
  overhead["avg_cycles_per_iteration"] = overhead_avg / iterations;
  summary_object["loop_overhead"] = overhead;

  for (const auto& summary : summaries) {
    const auto [min_it, max_it] = std::minmax_element(summary.cycles.begin(), summary.cycles.end());
    const auto total = std::accumulate(summary.cycles.begin(), summary.cycles.end(), std::uint64_t{0});
    const double average = static_cast<double>(total) / summary.cycles.size();
    std::vector<std::uint64_t> corrected;
    corrected.reserve(summary.cycles.size());
    for (std::size_t i = 0; i < summary.cycles.size(); ++i) {
      corrected.push_back(corrected_cycles(summary.cycles[i], overhead_cycles[i % overhead_cycles.size()]));
    }
    const auto [min_corrected_it, max_corrected_it] =
        std::minmax_element(corrected.begin(), corrected.end());
    const auto corrected_total =
        std::accumulate(corrected.begin(), corrected.end(), std::uint64_t{0});
    const double corrected_average = static_cast<double>(corrected_total) / corrected.size();

    Json item = Json::object();
    item["instruction"] = summary.instruction;
    item["min_cycles"] = static_cast<std::uint64_t>(*min_it);
    item["max_cycles"] = static_cast<std::uint64_t>(*max_it);
    item["avg_cycles"] = average;
    item["min_cycles_per_op"] = static_cast<double>(*min_it) / iterations;
    item["avg_cycles_per_op"] = average / iterations;
    item["min_corrected_cycles"] = static_cast<std::uint64_t>(*min_corrected_it);
    item["max_corrected_cycles"] = static_cast<std::uint64_t>(*max_corrected_it);
    item["avg_corrected_cycles"] = corrected_average;
    item["min_corrected_cycles_per_op"] = static_cast<double>(*min_corrected_it) / iterations;
    item["avg_corrected_cycles_per_op"] = corrected_average / iterations;

    std::string key = summary.instruction;
    std::replace(key.begin(), key.end(), '.', '_');
    summary_object[key] = item;
  }
  return summary_object;
}

}  // namespace

Json run_ptx_latency_probe(const Options& options, std::vector<std::string>& warnings) {
  (void)warnings;
  require_gpu_id(options.gpu);
  CUDA_CHECK(cudaSetDevice(options.gpu));

  const int iterations = options.iterations > 0 ? options.iterations : kSmokeIterations;
  const auto overhead_cycles = run_loop_overhead(iterations);
  std::vector<InstructionSummary> summaries;
  summaries.push_back(run_instruction<std::uint32_t>("add.s32", bench_add_s32, iterations));
  summaries.push_back(run_instruction<std::uint64_t>("add.u64", bench_add_u64, iterations));
  summaries.push_back(run_instruction<float>("fma.rn.f32", bench_fma_f32, iterations));

  Json parameters = Json::object();
  parameters["probe"] = "ptx-latency";
  parameters["gpu"] = options.gpu;
  parameters["mode"] = options.mode;
  parameters["iterations"] = iterations;
  parameters["samples"] = kSamples;
  parameters["timing"] = "%clock64";
  parameters["dependency_mode"] = "single-thread dependent chain";
  parameters["correction"] = "per-sample loop-overhead subtraction";

  Json validation = Json::object();
  validation["sass_command"] = "cuobjdump --dump-sass build/nvidia_gb200_probe";
  validation["expected_ptx_ops"] = "add.s32, add.u64, fma.rn.f32";
  validation["sass_snippet_command"] =
      "tools/extract_sass_snippets.sh results/gb200_v2_smoke.sass results/gb200_v2_sass_snippets.md";

  Json result = Json::object();
  result["parameters"] = parameters;
  result["raw_samples"] = raw_samples_json(summaries, overhead_cycles, iterations);
  result["summary"] = summary_json(summaries, overhead_cycles, iterations);
  result["validation"] = validation;
  return result;
}
