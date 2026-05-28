#include "../src/probe_helpers.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int expect_int(const char *name, uint64_t got, uint64_t want) {
  if (got == want) {
    return 0;
  }
  fprintf(stderr, "%s: got %llu, want %llu\n", name,
          (unsigned long long)got, (unsigned long long)want);
  return 1;
}

static int expect_double(const char *name, double got, double want,
                         double tolerance) {
  if (fabs(got - want) <= tolerance) {
    return 0;
  }
  fprintf(stderr, "%s: got %.12f, want %.12f\n", name, got, want);
  return 1;
}

int main(void) {
  int failures = 0;
  uint64_t samples[] = {9, 2, 7, 7, 1};
  struct probe_stats stats = probe_stats_from(samples, 5);

  failures += expect_int("min", stats.min, 1);
  failures += expect_int("median", stats.median, 7);
  failures += expect_int("max", stats.max, 9);
  failures += expect_double("mean", stats.mean, 5.2, 0.000001);
  failures += expect_double("stddev", stats.stddev, 3.124099870, 0.000001);
  failures += expect_double("cv_percent", stats.cv_percent, 60.07884365, 0.000001);
  failures += expect_double("cycles_to_ns", probe_cycles_to_ns(3200, 3200000000.0),
                            1000.0, 0.000001);
  failures += expect_double("gib_per_second",
                            probe_gib_per_second(1073741824ULL, 1000000000.0),
                            1.0, 0.000001);
  failures += expect_int("corrected_ticks", probe_corrected_ticks(99, 42), 57);
  failures += expect_int("corrected_ticks_floor", probe_corrected_ticks(42, 99), 1);

  char formatted[32];
  probe_format_bytes(formatted, sizeof(formatted), 1536);
  if (strcmp(formatted, "1.5 KiB") != 0) {
    fprintf(stderr, "format 1536: got %s, want 1.5 KiB\n", formatted);
    failures++;
  }
  probe_format_bytes(formatted, sizeof(formatted), 1048576);
  if (strcmp(formatted, "1.0 MiB") != 0) {
    fprintf(stderr, "format 1048576: got %s, want 1.0 MiB\n", formatted);
    failures++;
  }

  uint64_t monotonic_a = probe_read_cntvct();
  uint64_t monotonic_b = probe_read_cntvct();
  if (monotonic_b < monotonic_a) {
    fprintf(stderr, "cntvct regressed: %llu -> %llu\n",
            (unsigned long long)monotonic_a, (unsigned long long)monotonic_b);
    failures++;
  }

  return failures == 0 ? 0 : 1;
}
