# NVIDIA Microarchitecture

This directory contains NVIDIA-focused microarchitecture notes, benchmarks, and tests.

- `docs/`: research notes and architecture dissection documents.
- `tests/`: unit tests for NVIDIA microbenchmark tooling.

## GB200 V1 Probe

Build and run inside a GB200 CUDA container:

```bash
cmake -S . -B build -G Ninja -DCMAKE_CUDA_ARCHITECTURES=100
cmake --build build
ctest --test-dir build --output-on-failure
./build/nvidia_gb200_probe --probe all --gpus 0,1,2,3 --mode smoke --output results/gb200_smoke.json
```

See `docs/GB200_v1_benchmark_results.md` for probe semantics, commands, and the first recorded 4xGB200 smoke results.

Core V2 adds loop-overhead-corrected PTX fields, richer memory sweeps, local HBM bandwidth, and SASS snippet extraction. See `docs/GB200_v2_benchmark_results.md`.
