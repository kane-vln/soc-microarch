#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct Options {
  std::string probe = "all";
  int gpu = 0;
  std::vector<int> gpus;
  std::string mode = "smoke";
  int iterations = 0;
  std::vector<std::uint64_t> mem_sizes;
  std::uint64_t bytes = 0;
  std::string output = "results/latest.json";
  bool help = false;
};

Options parse_options(int argc, char** argv);
std::string usage();
std::vector<int> parse_gpu_csv(const std::string& csv);
std::vector<std::uint64_t> parse_u64_csv(const std::string& csv, const std::string& flag);
bool is_valid_probe(const std::string& probe);
bool is_valid_mode(const std::string& mode);
