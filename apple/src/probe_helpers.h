#ifndef PROBE_HELPERS_H
#define PROBE_HELPERS_H

#include <stddef.h>
#include <stdint.h>

struct probe_stats {
  uint64_t min;
  uint64_t median;
  uint64_t max;
  double mean;
  double stddev;
  double cv_percent;
};

uint64_t probe_read_cntvct(void);
uint64_t probe_read_cntfrq(void);
uint64_t probe_read_mach_time(void);
double probe_mach_ticks_to_ns(uint64_t ticks);
double probe_cycles_to_ns(uint64_t cycles, double frequency_hz);
double probe_gib_per_second(uint64_t bytes, double nanoseconds);
uint64_t probe_corrected_ticks(uint64_t raw_ticks, uint64_t baseline_ticks);
void probe_format_bytes(char *out, size_t out_len, size_t bytes);
struct probe_stats probe_stats_from(const uint64_t *samples, size_t count);

#endif
