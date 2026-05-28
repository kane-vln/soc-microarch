#pragma once

#include "cli.hpp"
#include "json.hpp"

#include <string>
#include <vector>

Json collect_devices();
Json run_env_probe(const Options& options, std::vector<std::string>& warnings);
Json run_ptx_latency_probe(const Options& options, std::vector<std::string>& warnings);
Json run_mem_chase_probe(const Options& options, std::vector<std::string>& warnings);
Json run_hbm_bandwidth_probe(const Options& options, std::vector<std::string>& warnings);
Json run_fabric_smoke_probe(const Options& options, std::vector<std::string>& warnings);
