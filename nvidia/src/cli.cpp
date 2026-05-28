#include "cli.hpp"

#include <charconv>
#include <stdexcept>
#include <string_view>

namespace {

int parse_int(std::string_view value, const std::string& flag) {
  int parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    throw std::runtime_error("invalid integer for " + flag + ": " + std::string(value));
  }
  return parsed;
}

std::uint64_t parse_u64(std::string_view value, const std::string& flag) {
  std::uint64_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed == 0) {
    throw std::runtime_error("invalid positive integer for " + flag + ": " + std::string(value));
  }
  return parsed;
}

std::string require_value(int& index, int argc, char** argv, const std::string& flag) {
  if (index + 1 >= argc) {
    throw std::runtime_error("missing value for " + flag);
  }
  ++index;
  return argv[index];
}

}  // namespace

bool is_valid_probe(const std::string& probe) {
  return probe == "env" || probe == "ptx-latency" || probe == "mem-chase" ||
         probe == "hbm-bandwidth" || probe == "fabric-smoke" || probe == "all";
}

bool is_valid_mode(const std::string& mode) { return mode == "smoke" || mode == "custom"; }

std::vector<int> parse_gpu_csv(const std::string& csv) {
  std::vector<int> gpus;
  std::size_t start = 0;
  while (start <= csv.size()) {
    const auto comma = csv.find(',', start);
    const auto end = comma == std::string::npos ? csv.size() : comma;
    const auto token = std::string_view(csv).substr(start, end - start);
    if (token.empty()) {
      throw std::runtime_error("empty GPU id in --gpus list");
    }
    const int gpu = parse_int(token, "--gpus");
    if (gpu < 0) {
      throw std::runtime_error("negative GPU id in --gpus list");
    }
    gpus.push_back(gpu);
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return gpus;
}

std::vector<std::uint64_t> parse_u64_csv(const std::string& csv, const std::string& flag) {
  std::vector<std::uint64_t> values;
  std::size_t start = 0;
  while (start <= csv.size()) {
    const auto comma = csv.find(',', start);
    const auto end = comma == std::string::npos ? csv.size() : comma;
    const auto token = std::string_view(csv).substr(start, end - start);
    if (token.empty()) {
      throw std::runtime_error("empty value in " + flag + " list");
    }
    values.push_back(parse_u64(token, flag));
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return values;
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--probe") {
      options.probe = require_value(i, argc, argv, arg);
    } else if (arg == "--gpu") {
      options.gpu = parse_int(require_value(i, argc, argv, arg), arg);
      if (options.gpu < 0) {
        throw std::runtime_error("--gpu must be non-negative");
      }
    } else if (arg == "--gpus") {
      options.gpus = parse_gpu_csv(require_value(i, argc, argv, arg));
    } else if (arg == "--mode") {
      options.mode = require_value(i, argc, argv, arg);
    } else if (arg == "--iterations") {
      options.iterations = parse_int(require_value(i, argc, argv, arg), arg);
      if (options.iterations < 0) {
        throw std::runtime_error("--iterations must be non-negative");
      }
    } else if (arg == "--mem-sizes") {
      options.mem_sizes = parse_u64_csv(require_value(i, argc, argv, arg), arg);
    } else if (arg == "--bytes") {
      options.bytes = parse_u64(require_value(i, argc, argv, arg), arg);
    } else if (arg == "--output") {
      options.output = require_value(i, argc, argv, arg);
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }

  if (!is_valid_probe(options.probe)) {
    throw std::runtime_error("unknown probe: " + options.probe);
  }
  if (!is_valid_mode(options.mode)) {
    throw std::runtime_error("unknown mode: " + options.mode);
  }
  if (options.output.empty()) {
    throw std::runtime_error("--output must not be empty");
  }
  return options;
}

std::string usage() {
  return R"(nvidia_gb200_probe

Usage:
  nvidia_gb200_probe [options]

Options:
  --probe env|ptx-latency|mem-chase|hbm-bandwidth|fabric-smoke|all
  --gpu <id>
  --gpus <csv>
  --mode smoke|custom
  --iterations <n>
  --mem-sizes <csv>
  --bytes <n>
  --output <path>
  --help

Defaults:
  --probe all
  --gpu 0
  --mode smoke
  --bytes 536870912
  --output results/latest.json
)";
}
