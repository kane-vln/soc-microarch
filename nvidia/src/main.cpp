#include "cli.hpp"
#include "cuda_util.hpp"
#include "probes.hpp"

#include <cuda_runtime.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string iso_timestamp_utc() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&time, &tm);

  char buffer[32]{};
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

std::string hostname() {
  char buffer[256]{};
  if (gethostname(buffer, sizeof(buffer) - 1) != 0) {
    return "unknown";
  }
  return buffer;
}

std::string env_value(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? "" : value;
}

Json warnings_json(const std::vector<std::string>& warnings) {
  Json array = Json::array();
  for (const auto& warning : warnings) {
    array.push_back(warning);
  }
  return array;
}

Json run_metadata(const Options& options) {
  Json run = Json::object();
  run["timestamp_utc"] = iso_timestamp_utc();
  run["hostname"] = hostname();
  run["selected_probe"] = options.probe;
  run["mode"] = options.mode;
  run["output"] = options.output;
  run["slurm_job_id"] = env_value("SLURM_JOB_ID");
  run["slurm_job_nodelist"] = env_value("SLURM_JOB_NODELIST");
  run["slurm_step_id"] = env_value("SLURM_STEP_ID");
  return run;
}

void write_json_file(const std::string& path, const Json& root) {
  const std::filesystem::path output_path(path);
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path());
  }

  std::ofstream out(output_path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  out << root.dump(2) << "\n";
}

bool should_run(const Options& options, const std::string& probe) {
  return options.probe == "all" || options.probe == probe;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    if (options.help) {
      std::cout << usage();
      return 0;
    }

    std::vector<std::string> warnings;
    Json probes = Json::object();
    if (should_run(options, "env")) {
      probes["env"] = run_env_probe(options, warnings);
    }
    if (should_run(options, "ptx-latency")) {
      probes["ptx-latency"] = run_ptx_latency_probe(options, warnings);
    }
    if (should_run(options, "mem-chase")) {
      probes["mem-chase"] = run_mem_chase_probe(options, warnings);
    }
    if (should_run(options, "hbm-bandwidth")) {
      probes["hbm-bandwidth"] = run_hbm_bandwidth_probe(options, warnings);
    }
    if (should_run(options, "fabric-smoke")) {
      probes["fabric-smoke"] = run_fabric_smoke_probe(options, warnings);
    }

    Json root = Json::object();
    root["run"] = run_metadata(options);
    root["devices"] = collect_devices();
    root["probes"] = probes;
    root["warnings"] = warnings_json(warnings);

    write_json_file(options.output, root);
    std::cout << "wrote " << options.output << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
