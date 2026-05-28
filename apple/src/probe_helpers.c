#include "probe_helpers.h"

#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t probe_read_cntvct(void) {
  uint64_t value = 0;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
  return value;
}

uint64_t probe_read_cntfrq(void) {
  uint64_t value = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
  return value;
}

uint64_t probe_read_mach_time(void) { return mach_absolute_time(); }

double probe_mach_ticks_to_ns(uint64_t ticks) {
  mach_timebase_info_data_t info;
  if (mach_timebase_info(&info) != KERN_SUCCESS || info.denom == 0) {
    return 0.0;
  }
  return (double)ticks * (double)info.numer / (double)info.denom;
}

double probe_cycles_to_ns(uint64_t cycles, double frequency_hz) {
  if (frequency_hz <= 0.0) {
    return 0.0;
  }
  return (double)cycles * 1000000000.0 / frequency_hz;
}

double probe_gib_per_second(uint64_t bytes, double nanoseconds) {
  if (nanoseconds <= 0.0) {
    return 0.0;
  }
  return ((double)bytes / nanoseconds) * (1000000000.0 / 1073741824.0);
}

uint64_t probe_corrected_ticks(uint64_t raw_ticks, uint64_t baseline_ticks) {
  if (raw_ticks <= baseline_ticks) {
    return 1;
  }
  return raw_ticks - baseline_ticks;
}

void probe_format_bytes(char *out, size_t out_len, size_t bytes) {
  if (out == NULL || out_len == 0) {
    return;
  }

  const double value = (double)bytes;
  if (bytes >= 1024U * 1024U * 1024U) {
    snprintf(out, out_len, "%.1f GiB", value / (1024.0 * 1024.0 * 1024.0));
  } else if (bytes >= 1024U * 1024U) {
    snprintf(out, out_len, "%.1f MiB", value / (1024.0 * 1024.0));
  } else if (bytes >= 1024U) {
    snprintf(out, out_len, "%.1f KiB", value / 1024.0);
  } else {
    snprintf(out, out_len, "%zu B", bytes);
  }
}

static int compare_u64(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

struct probe_stats probe_stats_from(const uint64_t *samples, size_t count) {
  struct probe_stats stats = {0, 0, 0, 0.0, 0.0, 0.0};
  if (samples == NULL || count == 0) {
    return stats;
  }

  uint64_t *sorted = (uint64_t *)malloc(count * sizeof(*sorted));
  if (sorted == NULL) {
    return stats;
  }
  memcpy(sorted, samples, count * sizeof(*sorted));
  qsort(sorted, count, sizeof(*sorted), compare_u64);

  long double total = 0.0;
  for (size_t i = 0; i < count; i++) {
    total += (long double)samples[i];
  }

  stats.min = sorted[0];
  stats.median = sorted[count / 2];
  stats.max = sorted[count - 1];
  stats.mean = (double)(total / (long double)count);

  long double variance_sum = 0.0;
  for (size_t i = 0; i < count; i++) {
    const long double delta = (long double)samples[i] - (long double)stats.mean;
    variance_sum += delta * delta;
  }
  stats.stddev = sqrt((double)(variance_sum / (long double)count));
  if (stats.mean != 0.0) {
    stats.cv_percent = stats.stddev * 100.0 / stats.mean;
  }

  free(sorted);
  return stats;
}
