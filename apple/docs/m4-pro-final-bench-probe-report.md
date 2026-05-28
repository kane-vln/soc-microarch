# Apple M4 Pro Final Bench/Probe Report

Date: 2026-05-28

This directory now contains a three-layer CPU-core microarchitecture probe suite
for the local Apple M4 Pro MacBook Pro:

| Layer | Binary | Purpose |
|---|---|---|
| Quick | `build/cpu_probes` | Fast sanity probes: host, timer, QoS, latency/throughput, L1D pointer chase |
| Deep | `build/deep_cpu_probes` | Broad baseline: scalar int/FP, NEON, load/store, frontend proxies, cache/TLB, MLP, thread scaling |
| Advanced | `build/advanced_cpu_probes` | Second-layer probes: PMU capability, branch patterns, RAS depth, indirect target fanout, random memory, bandwidth, sustained QoS |

Raw result files:

- `results/m4-pro-deep-2026-05-28.csv` has 81 rows.
- `results/m4-pro-advanced-2026-05-28.csv` has 65 rows.

## Completion Status

The user-mode benchmark/probe suite is complete for the current repository
scope. It covers:

- host/timer census
- measurement hygiene and baseline correction
- scalar integer latency and throughput proxies
- scalar FP64 and NEON FP32 proxies
- L1 load/store and store-forwarding proxies
- aligned, unaligned, and split-line load proxies
- cache pointer-chase working-set sweep
- 16 KiB page-stride TLB pressure sweep
- frontend NOP, branch, call/return, branch predictability, RAS depth, and indirect-target proxies
- randomized load/store streams
- sequential read/write/memcpy bandwidth
- memory-level parallelism from 1 to 8 independent streams
- QoS and sustained thread scaling
- PMU/kperf capability detection

Two things are intentionally recorded as limitations rather than faked:

1. Exact P-core/E-core pinning is not guaranteed by public macOS APIs. QoS is a
   scheduler hint, not a core selector.
2. The private `kperf` framework and `kpc_get_counter_count` symbol are visible
   on this machine, but this harness does not configure Apple-private PMU event
   tables. The suite records PMU availability; it does not claim retired-cycle,
   retired-instruction, cache-miss, or TLB-miss counters.

## Reproduction Commands

```sh
make -C soc-microarch/apple clean
make -C soc-microarch/apple test
make -C soc-microarch/apple run-smoke
make -C soc-microarch/apple run-deep-smoke
make -C soc-microarch/apple run-advanced-smoke

./soc-microarch/apple/build/deep_cpu_probes --csv --samples 9 --blocks 800000 \
  > soc-microarch/apple/results/m4-pro-deep-2026-05-28.csv

./soc-microarch/apple/build/advanced_cpu_probes --csv --samples 7 --iters 200000 \
  > soc-microarch/apple/results/m4-pro-advanced-2026-05-28.csv
```

## Host Facts

The measured local system reports:

- Apple M4 Pro
- 14 logical CPUs
- 10 performance cores and 4 efficiency cores
- P-core L1I/L1D: 196,608 B / 131,072 B
- E-core L1I/L1D: 131,072 B / 65,536 B
- 16 KiB system page size
- `CNTFRQ_EL0 = 1,000,000,000 Hz`

Apple’s public spec for this M4 Pro class lists a 14-core CPU with 10
performance cores, 4 efficiency cores, and 273 GB/s memory bandwidth:
<https://support.apple.com/en-euro/121554>

## Most Important Findings

### 1. L1D Boundary

The cache pointer-chase sweep is the cleanest structural result. In the deep
run, 64-byte pointer chasing stayed flat through 128 KiB at about `0.665 ns/load`
and jumped at 256 KiB to about `4.02 ns/load`.

That matches the exposed P-core L1D size of 128 KiB and gives us a useful
sanity check that the pointer-chase method is observing real cache structure.

### 2. Scalar Core Behavior

From `m4-pro-deep-2026-05-28.csv`:

| Probe | Shape | ns/op |
|---|---|---:|
| integer `add` | dependent chain | 0.2005 |
| integer `mul` | dependent chain | 0.6848 |
| integer `umulh` | dependent chain | 0.6319 |
| FP64 `fadd` | dependent chain | 0.4494 |
| FP64 `fmul` | dependent chain | 0.6859 |
| independent integer add | 8 chains | 0.0104 |
| independent FP64 add | 4 chains | 0.0696 |

The dependent/independent gap is large, which is exactly what we expect from a
wide out-of-order core: dependency chains expose latency; independent chains
allow backend throughput.

### 3. Load/Store Proxies

From the deep run:

| Probe | ns/op |
|---|---:|
| independent L1 load | 0.0865 |
| independent L1 store | 0.0831 |
| store-to-load forwarding, same address | 0.9973 |
| repeated aligned load | 0.0462 |
| repeated unaligned +1 load | 0.0464 |
| repeated split-line load | 0.0462 |

The simple repeated unaligned/split-line cases did not show a strong penalty.
The advanced random-stream probes are more realistic for address-generation and
cache-pressure behavior.

### 4. Page/TLB Pressure

The deep page-stride pointer chase uses one pointer per 16 KiB page.

| Pages | Footprint | ns/load |
|---:|---:|---:|
| 512 | 8 MiB | 7.10 |
| 1024 | 16 MiB | 20.22 |
| 2048 | 32 MiB | 38.37 |
| 4096 | 64 MiB | 44.89 |

The large transition between 512 and 1024 pages suggests a major translation or
cache-residency boundary around that region. This is not a pure TLB result,
because cache footprint changes at the same time.

### 5. Branch Predictability

From the advanced run:

| Pattern | ns/branch |
|---|---:|
| all taken | 0.322 |
| alternating | 0.433 |
| pseudo-random | 1.000 |

The pseudo-random branch pattern is roughly 3x slower than the all-taken pattern
in this run. The alternating row had high variation, so it should be rerun if we
want to tune it into a branch-predictor-specific test.

### 6. Return Address Stack Proxy

Recursive call depth stayed relatively cheap through depth 48, then jumped
sharply at depth 64:

| Depth | ns/op |
|---:|---:|
| 32 | 0.488 |
| 48 | 0.483 |
| 64 | 4.647 |

This is a proxy rather than a formal RAS-size proof, because recursion also
changes stack behavior and call footprint. Still, the cliff at depth 64 is
interesting and worth a dedicated generated-call-chain follow-up.

### 7. Indirect Target / Instruction Footprint Proxy

The advanced suite calls sets of 64-byte-aligned tiny functions through function
pointers:

| Fanout | ns/call |
|---:|---:|
| 1 target | 0.680 |
| 2 targets | 0.722 |
| 4 targets | 1.846 |
| 8 targets | 1.812 |
| 16 targets | 1.854 |
| 32 targets | 1.943 |
| 64 targets | 1.812 |

The transition from 2 to 4 targets is the main jump. This mixes indirect target
prediction, BTB behavior, call overhead, and instruction footprint, so it should
be read as a frontend stress proxy.

### 8. Randomized Load/Store Streams

Selected advanced rows:

| Working set | Random load GiB/s | Random store GiB/s | Random RMW GiB/s |
|---|---:|---:|---:|
| 64 KiB | 48.37 | 16.05 | 55.45 |
| 128 KiB | 36.66 | 15.82 | 40.62 |
| 256 KiB | 23.80 | 14.02 | 27.85 |
| 1 MiB | 38.19 | 16.03 | 41.64 |
| 4 MiB | 14.66 | 9.24 | 16.00 |
| 16 MiB | 10.65 | 9.55 | 13.22 |

The 16 MiB random-load row had high variation, but the broad shape is clear:
small, cache-resident random loads are much faster than multi-MiB random loads.

### 9. Sequential Bandwidth

Selected advanced rows:

| Size | Read GiB/s | Write GiB/s | memcpy traffic GiB/s |
|---|---:|---:|---:|
| 8 MiB | 109.27 | 69.93 | 145.86 |
| 64 MiB | 86.56 | 77.01 | 124.86 |
| 256 MiB | 85.99 | 76.75 | 116.37 |

This is CPU-side sequential bandwidth from simple user-mode loops and libc
`memcpy`. It does not saturate Apple’s full published SoC memory bandwidth,
which requires more parallelism and/or GPU/ANE traffic.

### 10. Memory-Level Parallelism

Advanced independent pointer rings:

| Streams | ns/load |
|---:|---:|
| 1 | 1.547 |
| 2 | 0.786 |
| 4 | 0.670 |
| 8 | 0.969 |

Going from 1 to 4 independent streams improves effective ns/load, showing useful
memory-level parallelism. The 8-stream result regresses in this run, suggesting
pressure or saturation somewhere in the core or memory subsystem.

### 11. Sustained QoS Scaling

Aggregate `thr_add8` throughput by QoS:

| Threads | User-interactive ns/op | Background ns/op |
|---:|---:|---:|
| 1 | 0.0432 | 0.0547 |
| 2 | 0.0241 | 0.0405 |
| 4 | 0.0117 | 0.0276 |
| 8 | 0.00614 | 0.0235 |
| 10 | 0.00506 | 0.0239 |
| 14 | 0.00678 | 0.0210 |

The high-QoS workload scales much better and peaks around the 10-thread region
in this run, which is consistent with the 10 P-core count. The 14-thread row
adds E cores and scheduler complexity, so it is not simply “more is faster.”

## What The Suite Does Not Pretend To Know

The suite does not claim exact:

- ROB size
- physical register file size
- scheduler queue depth
- load queue/store queue size
- exact BTB level capacities
- exact RAS entry count
- exact P-core/E-core placement for each row
- PMU-backed cycle, IPC, miss, or retired-instruction counts

It contains probes that stress those mechanisms and expose cliffs or trends.
Turning those trends into exact capacities requires generated code families and
PMU counters. This report names that boundary instead of overclaiming.

## Recommended Future Extensions

If we go beyond user-mode probing, the highest-value next steps are:

1. Add a privileged PMU runner using Apple `kperf/kpc` event tables or
   Instruments exports.
2. Generate dedicated BTB/RAS/ROB source families instead of using mixed
   frontend proxies.
3. Add a core-placement harness that records scheduler placement with
   `powermetrics` or scheduler trace data.
4. Extend memory bandwidth to multi-process and GPU/Metal participants to
   approach the full unified-memory fabric limit.
