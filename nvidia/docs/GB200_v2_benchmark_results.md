# GB200 Core V2 Benchmark Results

Core V2 extends the V1 harness without jumping yet to TMEM/`tcgen05` or NCCL collectives. The purpose is to improve the measurement contract: expose loop-overhead-corrected latency fields, sweep more memory working-set sizes, add a local HBM bandwidth probe, and extract SASS snippets into a small inspectable artifact.

## Build And Run

```bash
cd /home/huik/shared/codebase/GPU-Micro-Arch/soc-microarch/nvidia
cmake -S . -B build -G Ninja -DCMAKE_CUDA_ARCHITECTURES=100
cmake --build build
ctest --test-dir build --output-on-failure
./build/nvidia_gb200_probe --probe all --gpus 0,1,2,3 --mode smoke --output results/gb200_v2_smoke.json
cuobjdump --dump-sass build/nvidia_gb200_probe > results/gb200_v2_smoke.sass
tools/extract_sass_snippets.sh results/gb200_v2_smoke.sass results/gb200_v2_sass_snippets.md
```

The raw run artifacts are ignored by git:

- `results/gb200_v2_smoke.json`
- `results/gb200_v2_smoke.sass`
- `results/gb200_v2_sass_snippets.md`

## Run Provenance

| Field | Value |
| --- | --- |
| SLURM job | `1321673` |
| Node | `nvl72D161-T17` |
| Timestamp UTC | `2026-05-28T13:36:44Z` |
| Visible GPUs | `4` |
| Probe keys | `env`, `fabric-smoke`, `hbm-bandwidth`, `mem-chase`, `ptx-latency` |
| Warnings | `0` |

Device summary:

| GPU | Name | CC | Global memory bytes | SM count | L2 bytes |
| ---: | --- | ---: | ---: | ---: | ---: |
| 0 | NVIDIA GB200 | 10.0 | 197897748480 | 152 | 135528448 |
| 1 | NVIDIA GB200 | 10.0 | 197897748480 | 152 | 135528448 |
| 2 | NVIDIA GB200 | 10.0 | 197897748480 | 152 | 135528448 |
| 3 | NVIDIA GB200 | 10.0 | 197897748480 | 152 | 135528448 |

## Core V2 Additions

`ptx-latency` now emits raw and corrected cycle fields. The correction subtracts a per-sample empty-loop measurement from each dependent instruction-chain sample. In this run, loop overhead measured only `2` cycles total for one million iterations, so corrected and raw results are effectively identical. Treat that as a limitation of the current overhead kernel rather than proof that loop control is free.

`mem-chase` now sweeps nine smoke working sets instead of four: `4 KiB`, `32 KiB`, `256 KiB`, `1 MiB`, `4 MiB`, `8 MiB`, `32 MiB`, `128 MiB`, and `256 MiB`. The CLI also accepts `--mem-sizes <csv>` for custom byte-sized sweeps.

`hbm-bandwidth` is a new local GPU bandwidth probe. It runs warmed-up grid-stride CUDA kernels over a `float4` buffer and reports read, write, and copy bandwidth. Copy reports both payload GB/s and estimated DRAM GB/s where read+write traffic is counted.

`tools/extract_sass_snippets.sh` converts a full `cuobjdump --dump-sass` file into a compact markdown snippet file for the instruction families this tranche cares about: `IADD3`, `FFMA`, `LDG`, and `STG`.

## Results

PTX dependent-chain timing:

| Instruction | Raw avg cycles/op | Corrected avg cycles/op | Corrected min cycles/op |
| --- | ---: | ---: | ---: |
| `add.s32` | 23.0002866 | 23.0002846 | 23.000086 |
| `add.u64` | 23.0001586 | 23.0001566 | 23.000086 |
| `fma.rn.f32` | 23.0002858 | 23.0002838 | 23.000087 |

Memory pointer-chase timing:

| Working set bytes | Cycles/load |
| ---: | ---: |
| 4096 | 38.034210 |
| 32768 | 38.269281 |
| 262144 | 81.145604 |
| 1048576 | 103.388356 |
| 4194304 | 103.382239 |
| 8388608 | 103.387881 |
| 33554432 | 103.399605 |
| 134217728 | 103.391092 |
| 268435456 | 103.388017 |

HBM bandwidth smoke timing:

| Operation | Payload bytes | Elapsed ms | Payload GB/s | Estimated DRAM GB/s |
| --- | ---: | ---: | ---: | ---: |
| read | 536870912 | 0.0846080035 | 6345.391568 | 6345.391568 |
| write | 536870912 | 0.08041600138 | 6676.170200 | 6676.170200 |
| copy | 536870912 | 0.1604479998 | 3346.074196 | 6692.148391 |

Peer-copy smoke timing:

| Source | Destination | GB/s |
| ---: | ---: | ---: |
| 0 | 1 | 720.7946583 |
| 0 | 2 | 721.8490511 |
| 0 | 3 | 721.7869835 |
| 1 | 0 | 711.9246322 |
| 1 | 2 | 721.9111873 |
| 1 | 3 | 738.1089206 |
| 2 | 0 | 735.9719435 |
| 2 | 1 | 738.1089206 |
| 2 | 3 | 744.2647563 |
| 3 | 0 | 736.0364749 |
| 3 | 1 | 738.1089206 |
| 3 | 2 | 744.3968172 |

Peer-copy aggregate:

| Metric | Value |
| --- | ---: |
| Ordered pairs | 12 |
| Min GB/s | 711.9246322 |
| Max GB/s | 744.3968172 |
| Average GB/s | 731.105272175 |

## SASS Snippets

`results/gb200_v2_sass_snippets.md` contains compact snippets from the full SASS dump. The V2 run includes:

- `IADD3`: integer add/control-path signatures.
- `FFMA`: FP32 fused multiply-add signatures.
- `LDG`: global-load signatures for pointer-chase/read paths.
- `STG`: global-store signatures for write/copy/sink paths.

This is better than the V1 manual grep, but it is still pattern-based. A later pass should map snippets back to named kernels and include line-ranged snippets per probe in JSON.

## Interpretation

The richer memory sweep shows three coarse regimes in this run: around `38` cycles/load for tiny working sets, around `81` cycles/load at `256 KiB`, and around `103` cycles/load from `1 MiB` through `256 MiB`. That is a useful smoke-level hierarchy signal, but not yet a cache-capacity proof because the access pattern, replacement state, TLB behavior, and compiler-generated load form still need deeper isolation.

The HBM bandwidth probe reports roughly `6.3-6.7 TB/s` for read/write kernels and about `3.35 TB/s` payload copy bandwidth, or `6.69 TB/s` if counting copy as read plus write DRAM traffic. These are kernel-level smoke measurements, not peak-HBM claims; they should be compared against Nsight Compute counters in a later pass.

The peer-copy probe remained healthy across all 12 ordered pairs, with average payload bandwidth around `731 GB/s` for a single 256 MiB peer copy at a time.

## Next Tranche

After Core V2, the next highest-value step is a Blackwell-specific tensor tranche:

- Add a compile-gated `tcgen05`/TMEM probe only after choosing exact PTX shapes from CUDA 13 PTX docs.
- Add CUTLASS or minimal CUDA tensor-core paths for FP8 first, then FP6/FP4/NVFP4 when toolchain support is clear.
- Add NCCL collectives separately from single-copy fabric smoke, because those results answer different questions.
