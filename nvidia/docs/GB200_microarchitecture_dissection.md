# GB200 Microarchitecture Dissection

This note maps the Ampere microbenchmarking methodology from arXiv:2208.11174 onto NVIDIA GB200 / Blackwell. Treat GB200 as a stack, not a single chip: a Grace Blackwell Superchip contains one Grace CPU and two Blackwell GPUs connected by NVLink-C2C; GB200 NVL72 scales that into 36 Grace CPUs and 72 Blackwell GPUs over a fifth-generation NVLink fabric.

## Source Spine

- Abdelkhalik et al., "Demystifying the Nvidia Ampere Architecture through Microbenchmarking and Instruction-level Analysis", arXiv:2208.11174: https://arxiv.org/pdf/2208.11174
- Luo et al., "Benchmarking and Dissecting the Nvidia Hopper GPU Architecture", arXiv:2402.13499: https://arxiv.org/abs/2402.13499
- Luo et al., "Dissecting the NVIDIA Hopper Architecture through Microbenchmarking and Multiple Level Analysis", arXiv:2501.12084: https://arxiv.org/abs/2501.12084
- Jarmusch, Graddon, and Chandrasekaran, "Dissecting the NVIDIA Blackwell Architecture with Microbenchmarks", arXiv:2507.10789: https://arxiv.org/abs/2507.10789
- NVIDIA GB200 NVL72 product/spec page: https://www.nvidia.com/en-gb/data-center/gb200-nvl72/
- NVIDIA GB200 NVL multi-node tuning guide: https://docs.nvidia.com/multi-node-nvlink-systems/multi-node-tuning-guide/overview.html
- NVIDIA Blackwell tuning guide: https://docs.nvidia.com/cuda/blackwell-tuning-guide/index.html
- NVIDIA PTX ISA 9.x documentation for TensorCore fifth-generation `tcgen05` instructions: https://docs.nvidia.com/cuda/archive/13.0.0/parallel-thread-execution/contents.html

## What Changed From Ampere To GB200

The Ampere paper dissects one A100 by isolating PTX instructions, verifying PTX-to-SASS lowering, measuring memory hierarchy latencies, and characterizing WMMA tensor core instructions. The Hopper papers are the bridge generation: they cover L2/global-memory behavior, FP8 tensor cores, asynchronous WGMMA, DPX, distributed shared memory, and TMA. The 2025 Blackwell microbenchmark paper is the closest direct Blackwell-era companion: it studies memory hierarchy, SM execution pipelines, SM sub-core units, cache behavior, scheduling details, power efficiency, and fifth-generation tensor cores with FP4/FP6 support. GB200 needs that same discipline, but the target surface is wider:

- Scalar and vector pipelines: measure dependent latency and independent throughput for INT32, FP32, FP64, BF16/FP16 conversions, predicates, barriers, atomics, and mixed INT/FP issue.
- Memory hierarchy: measure register pressure, L1/shared carveout behavior, L2 residency, HBM3e bandwidth/latency, and cache bypass operators. Blackwell keeps the unified L1/texture/shared-memory model and exposes a much larger GB200 L2 cache.
- Tensor core path: Ampere used WMMA/HMMA/IMMA/DMMA. Blackwell adds fifth-generation tensor core operations, low-precision FP4/FP6/NVFP4 paths, block scaling, and `tcgen05` tensor-memory workflows.
- Tensor Memory: Blackwell introduces a tensor-core-facing memory space that must be benchmarked separately from registers, shared memory, L1, L2, and HBM.
- Decompression: GB200 includes dedicated decompression support for formats such as LZ4, Snappy, and Deflate, so compressed-memory pipelines become first-class microbenchmarks.
- System fabric: GB200 is not just an SM study. Grace CPU memory, NVLink-C2C, GPU-GPU NVLink, NVLink Switch, and rack topology affect real model behavior.

Reference roles:

- Ampere paper: baseline measurement contract for instruction latency, throughput, PTX/SASS mapping, and memory hierarchy sweeps.
- Hopper papers: transition references for FP8, WGMMA, TMA, DPX, DSM, and multi-level evaluation from microbenchmarks to applications. The 2025 Hopper paper should be treated as an expanded/alternate Hopper reference rather than a fully independent line of evidence because arXiv notes substantial text overlap with arXiv:2402.13499.
- Blackwell paper: direct B200/Blackwell-era microbenchmark reference for memory hierarchy, execution pipelines, scheduling, power, and FP4/FP6 tensor-core behavior.

## Public GB200 Anchors

- GB200 Grace Blackwell Superchip: 1 Grace CPU plus 2 Blackwell GPUs.
- NVLink-C2C: 900 GB/s bidirectional coherent CPU-GPU bandwidth per Superchip path.
- GB200 NVL72: 36 Grace CPUs plus 72 Blackwell GPUs in a rack-scale liquid-cooled NVLink domain.
- NVL72 fabric: 130 TB/s aggregate NVLink bandwidth; fifth-generation NVLink gives 1.8 TB/s bidirectional bandwidth per GPU in the NVL72 design.
- GB200 Superchip specs listed by NVIDIA include 372 GB HBM3e at 16 TB/s, 3.6 TB/s NVLink bandwidth, 72 Arm Neoverse V2 CPU cores, and up to 480 GB LPDDR5X.
- GB200 NVL72 specs listed by NVIDIA include 13.4 TB HBM3e at 576 TB/s, 17 TB LPDDR5X at 14 TB/s, and 2,592 Arm Neoverse V2 CPU cores.

## Dissection Plan

1. Baseline identity
   - Record exact GPU name, compute capability, CUDA driver/toolkit, clocks, power caps, MIG state, persistence mode, and NVLink topology.
   - Dump `nvidia-smi topo -m`, `nvidia-smi nvlink --status`, `deviceQuery`, and `cuobjdump`/`nvdisasm` versions.

2. PTX-to-SASS map
   - Use one PTX kernel per instruction family.
   - Time with `%clock64`, not host timers.
   - Emit both dependent chains for true latency and independent chains for issue/throughput.
   - Verify generated SASS with `nvdisasm` or `cuobjdump --dump-sass`; reject measurements when the compiler adds barriers, folds instructions, or changes the intended data path.

3. Memory hierarchy
   - Pointer-chase global memory with cache-bypass and cache-control variants.
   - Sweep working-set sizes across L1/shared, L2, and HBM.
   - Separate load latency, store latency, bandwidth, atomics, coalesced access, strided access, and random access.
   - Repeat under different shared-memory carveouts.

4. Tensor core and TMEM
   - Start with CUDA/CUTLASS/Triton-generated kernels, then reduce to PTX once the instruction shapes are known.
   - Characterize FP64, TF32, FP32, FP16, BF16, FP8, FP6, FP4, and NVFP4 where software support exposes them.
   - Track instruction family, tile shape, issue granularity, CTA-pair use, accumulator location, TMEM allocation, TMEM load/store/copy latency, and barrier/wait costs.

5. Grace-C2C and unified memory behavior
   - Measure CPU-to-GPU, GPU-to-CPU, and GPU access to CPU memory over NVLink-C2C.
   - Compare pinned memory, managed memory, explicit copies, and direct mapped access.
   - Record latency distribution, not only peak bandwidth.

6. NVLink/NVL72 behavior
   - Benchmark peer copy, all-reduce, all-gather, reduce-scatter, and all-to-all at 2, 4, 8, 18, 36, and 72 GPU scales when hardware is available.
   - Compare locality-aware placements against random placements using the documented 36x2 and 72x1 configurations.
   - Correlate NCCL topology choices with NVLink switch paths.

7. Application probes
   - GEMM: dense and sparse, multiple precisions, scale-factor layouts.
   - Attention: FlashAttention-style tiled kernels stressing TMEM, TMA, shared memory, and L2.
   - MoE: expert-parallel all-to-all plus low-precision GEMM.
   - Data analytics: decompression plus join/aggregation-style memory-bound kernels.

## First Microbenchmark Skeleton

For each target instruction or subsystem, use this evidence row:

| Field | What to record |
| --- | --- |
| Target | PTX op, SASS op, CUDA intrinsic, or library kernel |
| Scope | SM, GPU, Superchip, tray, rack |
| Dependency mode | dependent latency / independent throughput / mixed |
| Clocking | `%clock64`, event timer, CUPTI, Nsight Compute metric |
| Working set | bytes, tile shape, stride, compression format |
| Placement | local HBM, remote GPU HBM, Grace LPDDR5X, peer via NVLink |
| Result | cycles, GB/s, TFLOP/s/PFLOP/s, latency percentiles |
| Validation | SASS dump, profiler counters, repeated-run variance |

## Questions To Answer

- Is GB200 scalar instruction latency materially different from Hopper, or are the big gains concentrated in tensor cores, memory bandwidth, and fabric?
- Does the dual-die Blackwell GPU expose measurable locality effects through L2/HBM partitioning or cross-die paths?
- How much does TMEM reduce shared-memory pressure in real GEMM/attention kernels?
- Which precision paths are software-mature enough to reach theoretical throughput: FP8, FP6, FP4, NVFP4?
- Where does Grace LPDDR5X behave like a useful extension of GPU memory, and where is it too latent?
- At what collective sizes does NVL72's 72-GPU NVLink domain stop looking like a single large accelerator?

## Near-Term Next Step

The first tranche has been implemented as a CUDA microbenchmark suite. See `GB200_v1_benchmark_results.md` for the runnable commands, JSON/SASS artifact locations, and first 4xGB200 smoke results.

- `env_probe`: topology, clocks, CUDA versions, device properties.
- `ptx_latency`: dependent and independent PTX chains with SASS dumps.
- `mem_chase`: L1/L2/HBM pointer-chase and bandwidth sweeps.
- `fabric_probe`: P2P and NCCL collectives across the visible NVLink domain.

V1 implements environment identity, dependent PTX latency, global-memory pointer chase, and local 4-GPU P2P copy smoke tests. Core V2 adds loop-overhead-corrected PTX fields, richer memory sweeps, a local HBM bandwidth probe, and SASS snippet extraction; see `GB200_v2_benchmark_results.md`. Later tranches should add deeper cache/TLB isolation, NCCL collectives, decompression, and TMEM/`tcgen05`.

The Ampere paper's most reusable idea is not any specific A100 number; it is the measurement contract: isolate one mechanism, verify actual SASS, control dependencies, sweep enough sizes to cross hierarchy boundaries, and record both latency and throughput.
