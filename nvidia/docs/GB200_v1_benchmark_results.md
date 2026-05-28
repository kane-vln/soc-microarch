# GB200 V1 Benchmark Results

This document records the first runnable GB200 dissection tranche. The goal is a reproducible baseline, not a final architectural characterization. The suite is intentionally small: identify the environment, verify basic SASS-visible instruction timing, measure dependent global-memory pointer-chase latency over a few working sets, and check peer-copy bandwidth across the four visible GPUs.

## Build And Run

The benchmark lives under `nvidia/` and builds with CUDA C++20, CMake, Ninja, and CTest.

```bash
cd /home/huik/shared/codebase/GPU-Micro-Arch/soc-microarch/nvidia
cmake -S . -B build -G Ninja -DCMAKE_CUDA_ARCHITECTURES=100
cmake --build build
ctest --test-dir build --output-on-failure
./build/nvidia_gb200_probe --probe all --gpus 0,1,2,3 --mode smoke --output results/gb200_smoke.json
cuobjdump --dump-sass build/nvidia_gb200_probe > results/gb200_smoke.sass
```

The result files are intentionally untracked and ignored by git:

- `results/gb200_smoke.json`: machine-readable probe output.
- `results/gb200_smoke.sass`: SASS dump used to verify generated instruction families.

## Run Provenance

Final smoke run:

| Field | Value |
| --- | --- |
| SLURM job | `1321607` |
| Node | `nvl72D202-T08` |
| Timestamp UTC | `2026-05-28T13:19:09Z` |
| CUDA driver/runtime | `13.0` / `13.0` |
| Visible GPUs | `4` |
| Warnings | `0` |

Each visible GPU reported:

| GPU | Name | Compute capability | Global memory bytes | SM count | L2 bytes |
| --- | --- | --- | --- | --- | --- |
| 0 | NVIDIA GB200 | 10.0 | 197897748480 | 152 | 135528448 |
| 1 | NVIDIA GB200 | 10.0 | 197897748480 | 152 | 135528448 |
| 2 | NVIDIA GB200 | 10.0 | 197897748480 | 152 | 135528448 |
| 3 | NVIDIA GB200 | 10.0 | 197897748480 | 152 | 135528448 |

## Probe Semantics

`env` records CUDA runtime identity, device properties, and the CUDA P2P access matrix. This is the provenance anchor for every later measurement.

`ptx-latency` launches one CUDA block with one active thread and times dependent instruction chains using `%clock64`. V1 covers `add.s32`, `add.u64`, and `fma.rn.f32`, with five samples and one million dependent operations per sample. The reported cycles/op include loop-control cost, so they should be treated as stable smoke baselines rather than deconvolved single-instruction latencies.

`mem-chase` builds a dependent pointer ring in global memory and times one million dependent loads using `%clock64`. The smoke working sets are `4 KiB`, `256 KiB`, `8 MiB`, and `256 MiB`. This reveals broad cache/memory regime changes, but it is not yet a full cache-line, associativity, replacement, or TLB study.

`fabric-smoke` enables CUDA peer access where available and runs `cudaMemcpyPeerAsync` for every ordered GPU pair. Each transfer copies `256 MiB` and is timed with CUDA events. This is a bandwidth smoke check for the local 4-GPU NVLink domain, not an NCCL collective benchmark.

## Results

PTX dependent-chain timing:

| Instruction | Avg cycles | Min cycles/op | Avg cycles/op |
| --- | ---: | ---: | ---: |
| `add.s32` | 23000581 | 23.000360 | 23.000581 |
| `add.u64` | 23000282 | 23.000094 | 23.000282 |
| `fma.rn.f32` | 23000154.2 | 23.000094 | 23.000154 |

Memory pointer-chase timing:

| Working set | Cycles/load |
| ---: | ---: |
| 4096 bytes | 38.033958 |
| 262144 bytes | 80.316275 |
| 8388608 bytes | 101.966965 |
| 268435456 bytes | 102.004211 |

Peer-copy smoke timing:

| Source | Destination | GB/s |
| ---: | ---: | ---: |
| 0 | 1 | 727.1048181 |
| 0 | 2 | 744.5289865 |
| 0 | 3 | 740.1930701 |
| 1 | 0 | 742.4203832 |
| 1 | 2 | 742.4203832 |
| 1 | 3 | 733.8472449 |
| 2 | 0 | 740.7159477 |
| 2 | 1 | 742.2890226 |
| 2 | 3 | 740.1930701 |
| 3 | 0 | 721.8490511 |
| 3 | 1 | 721.8490511 |
| 3 | 2 | 738.3037958 |

Aggregate peer-copy smoke summary:

| Metric | Value |
| --- | ---: |
| Ordered pairs | 12 |
| Min GB/s | 721.8490511 |
| Max GB/s | 744.5289865 |
| Average GB/s | 736.3095687 |

## SASS Evidence

`cuobjdump --dump-sass` was run on the executable. The SASS dump contains the expected low-level instruction families for the V1 probes:

- `IADD3` for integer add paths.
- `FFMA` for the FP32 fused multiply-add path.
- `LDG` for global loads in the pointer-chase path.

This check is deliberately lightweight. For deeper latency work, the next pass should isolate loop overhead, label exact kernel symbols in the SASS dump, and add a parser that records the matching SASS snippets directly in the JSON.

## Interpretation

The V1 numbers show that the plumbing is working: four GB200 GPUs are visible, CUDA reports compute capability 10.0, the suite can build and run inside the target container, SASS can be dumped, all P2P directions are accessible, and the probes produce positive timing data.

The PTX-latency result around 23 cycles/op is a baseline for this exact dependent-loop harness, not yet a pure instruction latency. The memory-chase results show a clear jump from tiny working set to larger working sets, with the 8 MiB and 256 MiB smoke cases close together in this run. The peer-copy results are tightly clustered around the low-to-mid 700 GB/s range per ordered copy, which is consistent with a healthy local NVLink peer path for this smoke benchmark.

## V2 Follow-Up

The next tranche should add:

- Loop-overhead subtraction and per-kernel SASS snippet extraction.
- More memory sizes, stride variants, cache operators, and TLB-sensitive access patterns.
- Local HBM bandwidth kernels, not only pointer-chase latency.
- NCCL collectives across 2, 4, and larger NVL72 placements.
- TMEM/`tcgen05`, WGMMA, FP8/FP6/FP4/NVFP4, and decompression probes after the V1 harness remains stable.
