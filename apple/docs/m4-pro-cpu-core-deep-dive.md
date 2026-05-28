# Apple M4 Pro CPU-Core Deep Dive

Date: 2026-05-28

Target machine:

- MacBook Pro `Mac16,7`
- Apple M4 Pro
- 14 logical CPUs: 10 performance cores and 4 efficiency cores
- 48 GB unified memory
- macOS/Darwin kernel: `25.5.0`

External anchors:

- Apple lists this M4 Pro configuration as a 14-core CPU with 10 performance
  cores, 4 efficiency cores, and 273 GB/s memory bandwidth:
  <https://support.apple.com/en-euro/121554>
- Apple Silicon CPU Optimization Guide:
  <https://developer.apple.com/documentation/apple-silicon/cpu-optimization-guide>
- Public M4 P-core microarchitecture diagram collection:
  <https://jia.je/cpu/m4_pcore.html>
- The benchmark style is inspired by microbenchmarking papers such as
  <https://arxiv.org/pdf/2208.11174>, adapted here from GPU methodology to
  AArch64 CPU loops.

Raw result file:

- `soc-microarch/apple/results/m4-pro-deep-2026-05-28.csv`

Command used:

```sh
./soc-microarch/apple/build/deep_cpu_probes --csv --samples 9 --blocks 800000 \
  > soc-microarch/apple/results/m4-pro-deep-2026-05-28.csv
```

## Methodology

The suite uses short AArch64 inline-assembly kernels wrapped by a C harness.
Each kernel is run repeatedly, timed with `CNTVCT_EL0`, summarized by median,
and reported as nanoseconds per operation. For loop-shaped kernels, the harness
also measures an empty loop with the same block count and subtracts the median
empty-loop time.

Important measurement facts:

- `CNTFRQ_EL0` reports `1,000,000,000 Hz` on this system, so one timer tick is
  one nanosecond.
- `CNTVCT_EL0` is a fixed-rate architectural timer. It is not the CPU core clock.
  These results are `ns/op`, not true core cycles/op.
- The coefficient of variation in the CSV is computed from raw samples. Treat
  rows with high CV as unstable and rerun them before drawing hard conclusions.
- macOS does not provide normal users a stable public API for pinning a thread
  to one exact P core or E core. QoS class is only a scheduler hint.
- Baseline subtraction is least reliable when the measured body is close to
  empty-loop cost. The NOP rows are therefore useful as a sanity check, not as
  a real decode-width result.

## Host And Timer Census

| Item | Result |
|---|---:|
| Brand | Apple M4 Pro |
| Logical CPUs | 14 |
| P physical CPUs | 10 |
| E physical CPUs | 4 |
| P L1I | 196,608 B |
| P L1D | 131,072 B |
| E L1I | 131,072 B |
| E L1D | 65,536 B |
| Page size | 16,384 B |
| SME advertised by sysctl | yes |
| CNTVCT sleep calibration | ~25.012 ms for requested 20 ms sleep |
| Mach sleep calibration | ~25.013 ms for requested 20 ms sleep |

The sleep calibration overshoots because `nanosleep` wakeup is scheduler-bound.
The important part is that `CNTVCT_EL0` and `mach_absolute_time` agree closely.

## Measurement Hygiene

| Probe | Median | Interpretation |
|---|---:|---|
| Empty loop | 0.249 ns/block | Baseline loop/control cost |
| NOP stream after subtraction | ~0 | The NOP body is below reliable baseline subtraction resolution |

The empty loop is not free. Any sub-nanosecond body must be interpreted together
with raw and baseline ticks in the CSV.

## Core-Class QoS Hints

| QoS | Dependent add ns/op |
|---|---:|
| user interactive | 0.2008 |
| user initiated | 0.2006 |
| utility | 0.2013 |
| background | 0.2005 |

In this full run all QoS classes landed on similarly fast cores. That means the
system likely scheduled these short single-threaded kernels on P cores despite
the lower QoS hints. Earlier smoke runs showed larger QoS differences, so core
placement remains a variable to control in later work.

## Scalar Integer Results

| Probe | Shape | ns/op |
|---|---|---:|
| `add` | 8-op dependency chain | 0.2005 |
| `sub` | 8-op dependency chain | 0.1999 |
| logic mix | 8-op dependency chain | 0.2005 |
| shift mix | 8-op dependency chain | 0.2005 |
| `mul` | 8-op dependency chain | 0.6848 |
| `madd` | 4-op dependency chain | 0.8650 |
| `umulh` | 4-op dependency chain | 0.6319 |
| `udiv` | 2 divides plus adds | 1.8481 |
| independent `add` | 8 independent chains | 0.0104 |
| independent `mul` | 4 independent chains | 0.1161 |

Interpretation:

- Simple integer ALU chains are extremely fast in wall-clock terms, around
  `0.20 ns/op` in this run.
- Multiplication and high-half multiplication are much slower than simple ALU
  operations, as expected.
- Independent chains are much faster per operation than dependency chains. That
  confirms the core can expose far more throughput when dependencies do not
  serialize the backend.
- The `udiv` number is a proxy only. The kernel includes adds to keep the input
  changing, so it is not pure divide latency.

## FP And NEON Results

| Probe | Shape | ns/op |
|---|---|---:|
| FP64 `fadd` | 8-op dependency chain | 0.4494 |
| FP64 `fmul` | 8-op dependency chain | 0.6859 |
| FP64 `fmadd` | 4-op dependency chain | 0.6467 |
| FP64 independent `fadd` | 4 independent chains | 0.0696 |
| FP64 independent `fmul` | 4 independent chains | 0.1277 |
| NEON FP32x4 `fadd` | normalized per FP32 lane-op | 0.1041 |
| NEON FP32x4 `fmla` | normalized per FP32 lane-op | 0.1591 |

Interpretation:

- FP64 dependent add is slower than simple integer add, while FP64 multiply is
  close to integer multiply in wall-clock ns/op.
- Independent FP loops show much higher throughput than dependent chains.
- NEON rows are normalized per scalar lane operation. They are useful for
  comparing vectorized code paths, not for deriving exact vector pipe count yet.

## Load/Store Unit Proxies

| Probe | Variant | ns/op |
|---|---|---:|
| L1 load | 8 independent loads | 0.0865 |
| L1 store | 8 independent stores | 0.0831 |
| store-to-load forwarding | same address | 0.9973 |
| aligned repeated load | aligned | 0.0462 |
| unaligned repeated load | +1 byte | 0.0464 |
| split-line repeated load | crosses 64-byte line | 0.0462 |

Interpretation:

- Independent L1 loads and stores are very fast in aggregate. These are
  throughput proxies, not load-to-use latency numbers.
- Store-to-load forwarding is much more expensive than independent load/store
  streams, which is expected because it forces a dependency through the store
  queue and forwarding path.
- The simple unaligned and split-line repeated-load rows did not show a strong
  penalty in this run. Because they repeatedly touch the same hot address, they
  should be followed by a randomized address-stream version before concluding
  that split-line accesses are always cheap.

## Cache Pointer-Chase Sweep

| Working set | ns/load |
|---|---:|
| 4 KiB | 0.665 |
| 8 KiB | 0.665 |
| 16 KiB | 0.665 |
| 32 KiB | 0.665 |
| 64 KiB | 0.665 |
| 128 KiB | 0.665 |
| 256 KiB | 4.023 |
| 512 KiB | 5.270 |
| 1 MiB | 5.761 |
| 2 MiB | 5.942 |
| 4 MiB | 7.537 |
| 8 MiB | 7.867 |
| 16 MiB | 18.760 |
| 32 MiB | 115.641 |

Interpretation:

- The flat region through 128 KiB is a strong sanity check against the exposed
  P-core L1D size of 128 KiB.
- The jump at 256 KiB marks the first clear transition out of private L1D.
- The 4-8 MiB region remains much cheaper than the 16-32 MiB region, suggesting
  service from an on-chip cache level for at least part of that range.
- The 16 MiB row has high raw CV, so it is likely near a scheduling, cluster,
  cache, or power-state boundary. It should be rerun several times before being
  used to infer a precise L2/SLC capacity.
- The 32 MiB row is dramatically slower and is consistent with leaving the
  closest on-chip cache residency path for this pointer-chase pattern.

## TLB Pointer-Chase Sweep

The system page size is 16 KiB. This sweep places one pointer per page.

| Pages | Footprint | ns/load |
|---:|---:|---:|
| 16 | 256 KiB | 5.598 |
| 32 | 512 KiB | 4.671 |
| 64 | 1 MiB | 6.197 |
| 128 | 2 MiB | 6.265 |
| 256 | 4 MiB | 7.095 |
| 512 | 8 MiB | 7.104 |
| 1024 | 16 MiB | 20.224 |
| 2048 | 32 MiB | 38.366 |
| 4096 | 64 MiB | 44.888 |

Interpretation:

- The major TLB-related transition appears between 512 and 1024 pages, i.e.
  between 8 MiB and 16 MiB of 16 KiB pages.
- This is not a pure TLB measurement because the footprint also changes cache
  residency. Still, the page-stride pattern makes address-translation pressure
  much stronger than the 64-byte cache sweep.

## Frontend Proxies

| Probe | Variant | ns/op |
|---|---|---:|
| NOP stream | 8 NOPs | effectively below baseline resolution |
| unconditional branch | 4 branches | 0.1662 |
| conditional not-taken branch | 4 branches | 0.0819 |
| call/return | 4 calls | 0.4432 |

Interpretation:

- The NOP row cannot be used to infer decode width because empty-loop
  subtraction wipes it out.
- Taken branches are visibly more expensive than the not-taken branch proxy.
- Call/return is costlier still, as expected. A real return-address-stack probe
  should vary call depth and detect the depth where returns stop predicting
  cleanly.

## OoO And Memory-Level Parallelism

| Probe | Variant | ns/op |
|---|---|---:|
| integer add | 1 dependency chain | 0.1950 |
| integer add | 8 independent chains | 0.0102 |
| pointer chase | 1 independent stream | 5.591 |
| pointer chase | 2 independent streams | 3.026 |
| pointer chase | 4 independent streams | 1.546 |

Interpretation:

- Independent integer chains show the expected gap between latency-bound and
  throughput-bound execution.
- Multiple independent pointer-chase streams reduce effective ns/load, showing
  that the core and memory system can overlap several outstanding misses when
  the dependency graph allows it.
- This is a memory-level parallelism proxy, not a direct count of load queue,
  miss status holding register, or reorder-buffer entries.

## Thread Scaling

| Threads | Aggregate ns/op |
|---:|---:|
| 1 | 0.0441 |
| 2 | 0.0236 |
| 4 | 0.0198 |
| 8 | 0.0075 |
| 10 | 0.0081 |
| 14 | 0.0067 |

Interpretation:

- The benchmark reports aggregate throughput: total operations divided by wall
  time. It is expected to improve as more cores participate.
- The 10- and 14-thread rows include interactions with the P/E scheduler and
  possibly both P-core clusters. They are useful for coarse scaling, not exact
  per-core characterization.

## Current Conclusions

1. The measurement harness is usable: timer calibration is consistent, helper
   tests pass, CSV output is stable, and the cache sweep rediscovers the 128 KiB
   P-core L1D boundary.
2. Simple dependent integer operations are around `0.20 ns/op` in this run;
   dependent integer multiply is around `0.68 ns/op`.
3. FP64 dependent add/multiply are around `0.45 ns/op` and `0.69 ns/op`.
4. Independent integer, FP, and load/store loops expose much higher throughput
   than dependency chains, as expected for a wide OoO core.
5. Pointer chasing strongly suggests private L1D residency through 128 KiB,
   then a transition at 256 KiB, then further cache/TLB transitions around
   16-32 MiB depending on access pattern.
6. Page-stride pointer chasing shows a major transition between 512 and 1024
   16 KiB pages.

## What Still Needs More Rigor

- Core placement: add stronger P-core/E-core separation by running sustained
  background workloads, sampling `powermetrics`, or using lower-level thread
  placement tools if available.
- PMU counters: collect retired instruction, cycle, branch, cache miss, and TLB
  miss counters through Apple kperf/kpc or Instruments if permissions allow.
- Frontend: add generated instruction-footprint sweeps, BTB capacity tests,
  indirect branch predictor tests, and return-address-stack depth tests.
- Load/store: add randomized unaligned streams, explicit load-to-use dependency
  chains, store-set conflict tests, and more independent miss streams.
- OoO window: add chain-count sweeps with controlled dependency distance to
  estimate scheduler/ROB pressure rather than only showing throughput relief.

## Reproduction Commands

```sh
make -C soc-microarch/apple clean
make -C soc-microarch/apple test
make -C soc-microarch/apple run-smoke
make -C soc-microarch/apple run-deep-smoke
./soc-microarch/apple/build/deep_cpu_probes --csv --samples 9 --blocks 800000 \
  > soc-microarch/apple/results/m4-pro-deep-2026-05-28.csv
```
