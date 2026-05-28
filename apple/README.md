# Apple M4 Pro CPU-Core Exploratory Probes

Small, self-contained probes for first-pass Apple Silicon CPU-core
microarchitecture exploration. These are deliberately exploratory: the numbers
are useful for forming hypotheses, not yet a polished latency/throughput table.

## Build

```sh
make -C soc-microarch/apple test
make -C soc-microarch/apple
```

## Run

Quick smoke run:

```sh
make -C soc-microarch/apple run-smoke
```

Direct runs:

```sh
./soc-microarch/apple/build/cpu_probes
./soc-microarch/apple/build/cpu_probes --smoke --csv
./soc-microarch/apple/build/cpu_probes --samples 15 --blocks 2000000
./soc-microarch/apple/build/deep_cpu_probes --smoke
./soc-microarch/apple/build/deep_cpu_probes --csv --samples 9 --blocks 800000
./soc-microarch/apple/build/advanced_cpu_probes --smoke
./soc-microarch/apple/build/advanced_cpu_probes --csv --samples 7 --iters 200000
```

## What It Probes

- Host census via `sysctlbyname`
- `CNTVCT_EL0`, `CNTFRQ_EL0`, and `mach_absolute_time` calibration
- QoS-hinted dependent-add runs for rough P-core vs E-core placement signal
- Dependent integer and FP chains: `add`, `mul`, `fadd`, `fmul`
- Independent integer and FP loops for rough backend throughput signal
- Pointer-chase cache sweep around L1D capacity boundaries

The deeper binary, `deep_cpu_probes`, adds:

- empty-loop baseline correction and raw coefficient-of-variation reporting
- scalar integer latency/throughput proxies
- scalar FP64 and NEON FP32 vector probes
- L1 load/store, store-to-load forwarding, and alignment probes
- frontend proxies for NOP streams, branches, and call/return
- cache and 16 KiB page-stride TLB pointer chasing
- independent pointer-chase streams for memory-level parallelism
- QoS and multi-thread scaling probes

The final advanced binary, `advanced_cpu_probes`, adds:

- PMU/kperf capability detection
- branch predictability patterns
- return-address-stack depth proxy
- indirect target / instruction-footprint proxy
- randomized load/store streams
- sequential read/write/memcpy bandwidth
- 1/2/4/8-stream memory-level parallelism
- sustained QoS thread-scaling comparison

## Interpretation Notes

- `CNTVCT_EL0` is a fixed-rate timer on this system, not a core-cycle counter.
  Results are reported as `ns/op`, not architectural cycles.
- QoS is only a scheduler hint. It does not guarantee a specific core class.
- Thermal state, power mode, background activity, and frequency changes matter.
- The inline assembly loops still include loop overhead. Later versions should
  add empty-loop subtraction, larger unrolls, and PMU collection where possible.
- Pointer chasing uses one pointer per 64-byte cache line and a pseudo-random
  permutation to reduce prefetch usefulness.

## Current Results

- Raw CSV: `results/m4-pro-deep-2026-05-28.csv`
- Advanced CSV: `results/m4-pro-advanced-2026-05-28.csv`
- Deep-dive write-up: `docs/m4-pro-cpu-core-deep-dive.md`
- Final consolidated report: `docs/m4-pro-final-bench-probe-report.md`

## Files

- `src/probe_helpers.[ch]`: timing and deterministic statistics helpers
- `src/cpu_probes.c`: exploratory probe harness and kernels
- `tests/test_probe_helpers.c`: helper-unit smoke tests
