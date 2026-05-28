#include "probe_helpers.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

struct options {
  bool smoke;
  bool csv;
  uint32_t samples;
  uint64_t blocks;
};

struct measurement {
  const char *section;
  const char *name;
  const char *variant;
  uint64_t ops;
  struct probe_stats raw;
  struct probe_stats baseline;
  uint64_t corrected_ticks;
  double ns_per_op;
};

typedef uint64_t (*kernel_fn)(uint64_t blocks);

static volatile uint64_t g_sink_u64;
static volatile uintptr_t g_sink_ptr;
static uint8_t *g_mem;
static size_t g_mem_bytes;

static uint64_t cntfrq(void) { return probe_read_cntfrq(); }

static double ticks_to_ns(uint64_t ticks) {
  return probe_cycles_to_ns(ticks, (double)cntfrq());
}

static void usage(const char *argv0) {
  printf("Usage: %s [--smoke] [--csv] [--samples N] [--blocks N]\n", argv0);
  printf("\n");
  printf("Deep exploratory Apple Silicon CPU-core probes. Results use the fixed\n");
  printf("CNTVCT_EL0 timer and are reported as ns/op, not core cycles/op.\n");
}

static bool parse_u64(const char *text, uint64_t *out) {
  char *end = NULL;
  errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  *out = (uint64_t)value;
  return true;
}

static bool parse_options(int argc, char **argv, struct options *opts) {
  opts->smoke = false;
  opts->csv = false;
  opts->samples = 7;
  opts->blocks = 400000;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--smoke") == 0) {
      opts->smoke = true;
      opts->samples = 3;
      opts->blocks = 50000;
    } else if (strcmp(argv[i], "--csv") == 0) {
      opts->csv = true;
    } else if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
      uint64_t value = 0;
      if (!parse_u64(argv[++i], &value) || value == 0 || value > 1000) {
        fprintf(stderr, "invalid --samples value\n");
        return false;
      }
      opts->samples = (uint32_t)value;
    } else if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) {
      uint64_t value = 0;
      if (!parse_u64(argv[++i], &value) || value == 0) {
        fprintf(stderr, "invalid --blocks value\n");
        return false;
      }
      opts->blocks = value;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(argv[0]);
      exit(0);
    } else {
      fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return false;
    }
  }

  if (opts->blocks < 8) {
    opts->blocks = 8;
  }
  opts->blocks -= opts->blocks % 8;
  return true;
}

static void sleep_for_ns(long ns) {
  struct timespec req;
  req.tv_sec = ns / 1000000000L;
  req.tv_nsec = ns % 1000000000L;
  while (nanosleep(&req, &req) != 0 && errno == EINTR) {
  }
}

static void ensure_memory(size_t bytes) {
  if (g_mem_bytes >= bytes) {
    return;
  }
  free(g_mem);
  g_mem = NULL;
  g_mem_bytes = 0;
  if (posix_memalign((void **)&g_mem, 16384, bytes) != 0) {
    fprintf(stderr, "failed to allocate %zu bytes\n", bytes);
    exit(2);
  }
  for (size_t i = 0; i < bytes; i++) {
    g_mem[i] = (uint8_t)(i * 131U + 17U);
  }
  g_mem_bytes = bytes;
}

static void print_header(bool csv) {
  if (csv) {
    printf("section,name,variant,value,unit,ops,raw_median_ticks,"
           "baseline_median_ticks,corrected_median_ticks,ns_per_op,"
           "raw_cv_percent\n");
  }
}

static void print_fact(const char *section, const char *name,
                       const char *variant, const char *value,
                       const char *unit, bool csv) {
  if (csv) {
    printf("%s,%s,%s,%s,%s,0,0,0,0,0,0\n", section, name, variant, value,
           unit);
  } else {
    printf("  %-18s %-20s %s %s\n", name, variant, value, unit);
  }
}

static void print_section(const char *name, bool csv) {
  if (!csv) {
    printf("\n== %s ==\n", name);
  }
}

static void print_sysctl_string(const char *name, const char *sysctl_name,
                                bool csv) {
  char value[256];
  size_t len = sizeof(value);
  if (sysctlbyname(sysctl_name, value, &len, NULL, 0) != 0) {
    print_fact("host", name, "sysctl", "unavailable", strerror(errno), csv);
    return;
  }
  value[sizeof(value) - 1] = '\0';
  print_fact("host", name, "sysctl", value, "", csv);
}

static void print_sysctl_number(const char *name, const char *sysctl_name,
                                const char *unit, bool csv) {
  uint64_t raw = 0;
  size_t len = sizeof(raw);
  char value[64];
  if (sysctlbyname(sysctl_name, &raw, &len, NULL, 0) != 0) {
    print_fact("host", name, "sysctl", "unavailable", strerror(errno), csv);
    return;
  }
  uint64_t number = raw;
  if (len == sizeof(uint32_t)) {
    number = *(uint32_t *)&raw;
  }
  snprintf(value, sizeof(value), "%" PRIu64, number);
  print_fact("host", name, "sysctl", value, unit, csv);
}

static void run_host(bool csv) {
  char page_size[64];
  print_section("Host Census", csv);
  print_sysctl_string("brand", "machdep.cpu.brand_string", csv);
  print_sysctl_number("logical_cpus", "hw.ncpu", "count", csv);
  print_sysctl_number("p_physical_cpus", "hw.perflevel0.physicalcpu", "count",
                      csv);
  print_sysctl_number("e_physical_cpus", "hw.perflevel1.physicalcpu", "count",
                      csv);
  print_sysctl_number("p_l1i", "hw.perflevel0.l1icachesize", "bytes", csv);
  print_sysctl_number("p_l1d", "hw.perflevel0.l1dcachesize", "bytes", csv);
  print_sysctl_number("e_l1i", "hw.perflevel1.l1icachesize", "bytes", csv);
  print_sysctl_number("e_l1d", "hw.perflevel1.l1dcachesize", "bytes", csv);
  print_sysctl_number("feat_sme", "hw.optional.arm.FEAT_SME", "bool", csv);
  snprintf(page_size, sizeof(page_size), "%d", getpagesize());
  print_fact("host", "page_size", "getpagesize", page_size, "bytes", csv);
}

static void run_timer(bool csv) {
  char value[64];
  print_section("Timer Calibration", csv);
  snprintf(value, sizeof(value), "%" PRIu64, cntfrq());
  print_fact("timer", "cntfrq", "cntfrq_el0", value, "Hz", csv);

  uint64_t cnt_a = probe_read_cntvct();
  uint64_t mach_a = probe_read_mach_time();
  sleep_for_ns(20000000L);
  uint64_t cnt_b = probe_read_cntvct();
  uint64_t mach_b = probe_read_mach_time();
  snprintf(value, sizeof(value), "%.3f", ticks_to_ns(cnt_b - cnt_a));
  print_fact("timer", "cntvct_sleep", "20ms", value, "ns", csv);
  snprintf(value, sizeof(value), "%.3f", probe_mach_ticks_to_ns(mach_b - mach_a));
  print_fact("timer", "mach_sleep", "20ms", value, "ns", csv);
}

static uint64_t kernel_empty(uint64_t blocks) {
  uint64_t x = blocks;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("" : "+r"(x) : : "memory");
  }
  return x;
}

static uint64_t kernel_nop8(uint64_t blocks) {
  uint64_t x = blocks;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("nop\n\t"
                     "nop\n\t"
                     "nop\n\t"
                     "nop\n\t"
                     "nop\n\t"
                     "nop\n\t"
                     "nop\n\t"
                     "nop"
                     : "+r"(x));
  }
  return x;
}

static struct measurement measure_kernel(const char *section, const char *name,
                                         const char *variant, kernel_fn fn,
                                         uint64_t blocks,
                                         uint64_t ops_per_block,
                                         uint32_t samples,
                                         bool subtract_baseline) {
  uint64_t *raw = (uint64_t *)calloc(samples, sizeof(*raw));
  uint64_t *baseline = (uint64_t *)calloc(samples, sizeof(*baseline));
  if (raw == NULL || baseline == NULL) {
    fprintf(stderr, "failed to allocate sample buffers\n");
    exit(2);
  }

  g_sink_u64 ^= fn(blocks / 8 + 1);
  for (uint32_t i = 0; i < samples; i++) {
    uint64_t start = probe_read_cntvct();
    g_sink_u64 ^= fn(blocks);
    uint64_t end = probe_read_cntvct();
    raw[i] = end - start;

    if (subtract_baseline) {
      start = probe_read_cntvct();
      g_sink_u64 ^= kernel_empty(blocks);
      end = probe_read_cntvct();
      baseline[i] = end - start;
    }
  }

  struct measurement m;
  m.section = section;
  m.name = name;
  m.variant = variant;
  m.ops = blocks * ops_per_block;
  m.raw = probe_stats_from(raw, samples);
  m.baseline = probe_stats_from(baseline, samples);
  m.corrected_ticks =
      probe_corrected_ticks(m.raw.median, subtract_baseline ? m.baseline.median : 0);
  m.ns_per_op = ticks_to_ns(m.corrected_ticks) / (double)m.ops;

  free(raw);
  free(baseline);
  return m;
}

static void print_measurement(const struct measurement *m, bool csv) {
  if (csv) {
    printf("%s,%s,%s,,ns,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%.9f,%.6f\n",
           m->section, m->name, m->variant, m->ops, m->raw.median,
           m->baseline.median, m->corrected_ticks, m->ns_per_op,
           m->raw.cv_percent);
    return;
  }
  printf("  %-18s %-18s ops=%-12" PRIu64 " raw=%-10" PRIu64
         " base=%-9" PRIu64 " ns/op=%-11.6f cv=%6.3f%%\n",
         m->name, m->variant, m->ops, m->raw.median, m->baseline.median,
         m->ns_per_op, m->raw.cv_percent);
}

#define PRINT_KERNEL(section, name, variant, fn, blocks, ops, samples, base, csv) \
  do {                                                                           \
    struct measurement m__ =                                                     \
        measure_kernel(section, name, variant, fn, blocks, ops, samples, base);  \
    print_measurement(&m__, csv);                                                \
  } while (0)

static uint64_t kernel_dep_add(uint64_t blocks) {
  uint64_t x = 1;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("add %0, %0, #1\n\t"
                     "add %0, %0, #1\n\t"
                     "add %0, %0, #1\n\t"
                     "add %0, %0, #1\n\t"
                     "add %0, %0, #1\n\t"
                     "add %0, %0, #1\n\t"
                     "add %0, %0, #1\n\t"
                     "add %0, %0, #1"
                     : "+r"(x));
  }
  return x;
}

static uint64_t kernel_dep_sub(uint64_t blocks) {
  uint64_t x = blocks + 12345;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("sub %0, %0, #1\n\t"
                     "sub %0, %0, #1\n\t"
                     "sub %0, %0, #1\n\t"
                     "sub %0, %0, #1\n\t"
                     "sub %0, %0, #1\n\t"
                     "sub %0, %0, #1\n\t"
                     "sub %0, %0, #1\n\t"
                     "sub %0, %0, #1"
                     : "+r"(x));
  }
  return x;
}

static uint64_t kernel_dep_logic(uint64_t blocks) {
  uint64_t x = 0x0123456789abcdefULL;
  const uint64_t mask = 0xf0f0f0f0f0f0f0f0ULL;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("eor %0, %0, %1\n\t"
                     "and %0, %0, %1\n\t"
                     "orr %0, %0, %1\n\t"
                     "eor %0, %0, %1\n\t"
                     "and %0, %0, %1\n\t"
                     "orr %0, %0, %1\n\t"
                     "eor %0, %0, %1\n\t"
                     "orr %0, %0, %1"
                     : "+r"(x)
                     : "r"(mask));
  }
  return x;
}

static uint64_t kernel_dep_shift(uint64_t blocks) {
  uint64_t x = 0x123456789abcdef0ULL;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("lsl %0, %0, #1\n\t"
                     "lsr %0, %0, #1\n\t"
                     "lsl %0, %0, #1\n\t"
                     "lsr %0, %0, #1\n\t"
                     "lsl %0, %0, #1\n\t"
                     "lsr %0, %0, #1\n\t"
                     "lsl %0, %0, #1\n\t"
                     "lsr %0, %0, #1"
                     : "+r"(x));
  }
  return x;
}

static uint64_t kernel_dep_mul(uint64_t blocks) {
  uint64_t x = 3;
  const uint64_t c = 6364136223846793005ULL;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("mul %0, %0, %1\n\t"
                     "mul %0, %0, %1\n\t"
                     "mul %0, %0, %1\n\t"
                     "mul %0, %0, %1\n\t"
                     "mul %0, %0, %1\n\t"
                     "mul %0, %0, %1\n\t"
                     "mul %0, %0, %1\n\t"
                     "mul %0, %0, %1"
                     : "+r"(x)
                     : "r"(c));
  }
  return x;
}

static uint64_t kernel_dep_madd(uint64_t blocks) {
  uint64_t x = 3;
  const uint64_t c = 17;
  const uint64_t a = 23;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("madd %0, %0, %1, %2\n\t"
                     "madd %0, %0, %1, %2\n\t"
                     "madd %0, %0, %1, %2\n\t"
                     "madd %0, %0, %1, %2"
                     : "+r"(x)
                     : "r"(c), "r"(a));
  }
  return x;
}

static uint64_t kernel_dep_umulh(uint64_t blocks) {
  uint64_t x = 0x123456789abcdef0ULL;
  const uint64_t c = 0xfedcba9876543211ULL;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("umulh %0, %0, %1\n\t"
                     "umulh %0, %0, %1\n\t"
                     "umulh %0, %0, %1\n\t"
                     "umulh %0, %0, %1"
                     : "+r"(x)
                     : "r"(c));
  }
  return x;
}

static uint64_t kernel_dep_udiv(uint64_t blocks) {
  uint64_t x = 0xfedcba9876543211ULL;
  const uint64_t d = 7;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("add %0, %0, #97\n\t"
                     "udiv %0, %0, %1\n\t"
                     "add %0, %0, #97\n\t"
                     "udiv %0, %0, %1"
                     : "+r"(x)
                     : "r"(d));
  }
  return x;
}

static uint64_t kernel_thr_add8(uint64_t blocks) {
  uint64_t a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5, a5 = 6, a6 = 7, a7 = 8;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("add %0, %0, #1\n\t"
                     "add %1, %1, #1\n\t"
                     "add %2, %2, #1\n\t"
                     "add %3, %3, #1\n\t"
                     "add %4, %4, #1\n\t"
                     "add %5, %5, #1\n\t"
                     "add %6, %6, #1\n\t"
                     "add %7, %7, #1"
                     : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4),
                       "+r"(a5), "+r"(a6), "+r"(a7));
  }
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}

static uint64_t kernel_thr_mul4(uint64_t blocks) {
  uint64_t a0 = 3, a1 = 5, a2 = 7, a3 = 11;
  const uint64_t c = 2862933555777941757ULL;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("mul %0, %0, %4\n\t"
                     "mul %1, %1, %4\n\t"
                     "mul %2, %2, %4\n\t"
                     "mul %3, %3, %4"
                     : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3)
                     : "r"(c));
  }
  return a0 + a1 + a2 + a3;
}

static uint64_t bits_from_double(double value) {
  uint64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static uint64_t kernel_dep_fadd(uint64_t blocks) {
  double x = 1.0;
  const double c = 1.0000000001;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("fadd %d0, %d0, %d1\n\t"
                     "fadd %d0, %d0, %d1\n\t"
                     "fadd %d0, %d0, %d1\n\t"
                     "fadd %d0, %d0, %d1\n\t"
                     "fadd %d0, %d0, %d1\n\t"
                     "fadd %d0, %d0, %d1\n\t"
                     "fadd %d0, %d0, %d1\n\t"
                     "fadd %d0, %d0, %d1"
                     : "+w"(x)
                     : "w"(c));
  }
  return bits_from_double(x);
}

static uint64_t kernel_dep_fmul(uint64_t blocks) {
  double x = 1.0000001;
  const double c = 1.0000000001;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("fmul %d0, %d0, %d1\n\t"
                     "fmul %d0, %d0, %d1\n\t"
                     "fmul %d0, %d0, %d1\n\t"
                     "fmul %d0, %d0, %d1\n\t"
                     "fmul %d0, %d0, %d1\n\t"
                     "fmul %d0, %d0, %d1\n\t"
                     "fmul %d0, %d0, %d1\n\t"
                     "fmul %d0, %d0, %d1"
                     : "+w"(x)
                     : "w"(c));
  }
  return bits_from_double(x);
}

static uint64_t kernel_dep_fmadd(uint64_t blocks) {
  double x = 1.000001;
  const double c = 1.0000000001;
  const double a = 0.0000001;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("fmadd %d0, %d0, %d1, %d2\n\t"
                     "fmadd %d0, %d0, %d1, %d2\n\t"
                     "fmadd %d0, %d0, %d1, %d2\n\t"
                     "fmadd %d0, %d0, %d1, %d2"
                     : "+w"(x)
                     : "w"(c), "w"(a));
  }
  return bits_from_double(x);
}

static uint64_t kernel_thr_fadd4(uint64_t blocks) {
  double a0 = 1.0, a1 = 2.0, a2 = 3.0, a3 = 4.0;
  const double c = 1.0000000001;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("fadd %d0, %d0, %d4\n\t"
                     "fadd %d1, %d1, %d4\n\t"
                     "fadd %d2, %d2, %d4\n\t"
                     "fadd %d3, %d3, %d4"
                     : "+w"(a0), "+w"(a1), "+w"(a2), "+w"(a3)
                     : "w"(c));
  }
  return bits_from_double(a0 + a1 + a2 + a3);
}

static uint64_t kernel_thr_fmul4(uint64_t blocks) {
  double a0 = 1.000001, a1 = 1.000002, a2 = 1.000003, a3 = 1.000004;
  const double c = 1.0000000001;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("fmul %d0, %d0, %d4\n\t"
                     "fmul %d1, %d1, %d4\n\t"
                     "fmul %d2, %d2, %d4\n\t"
                     "fmul %d3, %d3, %d4"
                     : "+w"(a0), "+w"(a1), "+w"(a2), "+w"(a3)
                     : "w"(c));
  }
  return bits_from_double(a0 + a1 + a2 + a3);
}

typedef float v4f32 __attribute__((vector_size(16)));

static uint64_t bits_from_v4f32(v4f32 value) {
  uint64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static uint64_t kernel_neon_fadd(uint64_t blocks) {
  v4f32 x = {1.0f, 2.0f, 3.0f, 4.0f};
  const v4f32 c = {0.125f, 0.25f, 0.5f, 1.0f};
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("fadd %0.4s, %0.4s, %1.4s\n\t"
                     "fadd %0.4s, %0.4s, %1.4s\n\t"
                     "fadd %0.4s, %0.4s, %1.4s\n\t"
                     "fadd %0.4s, %0.4s, %1.4s"
                     : "+w"(x)
                     : "w"(c));
  }
  return bits_from_v4f32(x);
}

static uint64_t kernel_neon_fmla(uint64_t blocks) {
  v4f32 x = {1.0f, 2.0f, 3.0f, 4.0f};
  const v4f32 c = {1.0001f, 1.0002f, 1.0003f, 1.0004f};
  const v4f32 a = {0.125f, 0.25f, 0.5f, 1.0f};
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("fmla %0.4s, %1.4s, %2.4s\n\t"
                     "fmla %0.4s, %1.4s, %2.4s\n\t"
                     "fmla %0.4s, %1.4s, %2.4s\n\t"
                     "fmla %0.4s, %1.4s, %2.4s"
                     : "+w"(x)
                     : "w"(c), "w"(a));
  }
  return bits_from_v4f32(x);
}

static uint64_t kernel_l1_load_ind8(uint64_t blocks) {
  ensure_memory(4096);
  uint8_t *base = g_mem;
  uint64_t total = 0;
  for (uint64_t i = 0; i < blocks; i++) {
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    __asm__ volatile("ldr %0, [%8, #0]\n\t"
                     "ldr %1, [%8, #64]\n\t"
                     "ldr %2, [%8, #128]\n\t"
                     "ldr %3, [%8, #192]\n\t"
                     "ldr %4, [%8, #256]\n\t"
                     "ldr %5, [%8, #320]\n\t"
                     "ldr %6, [%8, #384]\n\t"
                     "ldr %7, [%8, #448]"
                     : "=&r"(x0), "=&r"(x1), "=&r"(x2), "=&r"(x3),
                       "=&r"(x4), "=&r"(x5), "=&r"(x6), "=&r"(x7)
                     : "r"(base)
                     : "memory");
    total += x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7;
  }
  return total;
}

static uint64_t kernel_l1_store8(uint64_t blocks) {
  ensure_memory(4096);
  uint8_t *base = g_mem;
  uint64_t x = 0xfeedfacecafebeefULL;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("str %1, [%0, #0]\n\t"
                     "str %1, [%0, #64]\n\t"
                     "str %1, [%0, #128]\n\t"
                     "str %1, [%0, #192]\n\t"
                     "str %1, [%0, #256]\n\t"
                     "str %1, [%0, #320]\n\t"
                     "str %1, [%0, #384]\n\t"
                     "str %1, [%0, #448]"
                     :
                     : "r"(base), "r"(x)
                     : "memory");
    x += i + 1;
  }
  return x;
}

static uint64_t kernel_store_load_forward(uint64_t blocks) {
  ensure_memory(4096);
  uint8_t *base = g_mem;
  uint64_t x = 1;
  uint64_t y = 0;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("str %1, [%2]\n\t"
                     "ldr %0, [%2]\n\t"
                     "str %0, [%2]\n\t"
                     "ldr %1, [%2]"
                     : "=&r"(y), "+r"(x)
                     : "r"(base)
                     : "memory");
  }
  return x ^ y;
}

static uint64_t kernel_load_aligned(uint64_t blocks) {
  ensure_memory(4096);
  uint8_t *base = g_mem + 64;
  uint64_t x = 0;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]"
                     : "=&r"(x)
                     : "r"(base)
                     : "memory");
  }
  return x;
}

static uint64_t kernel_load_unaligned(uint64_t blocks) {
  ensure_memory(4096);
  uint8_t *base = g_mem + 65;
  uint64_t x = 0;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]"
                     : "=&r"(x)
                     : "r"(base)
                     : "memory");
  }
  return x;
}

static uint64_t kernel_load_split_line(uint64_t blocks) {
  ensure_memory(4096);
  uint8_t *base = g_mem + 124;
  uint64_t x = 0;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]\n\t"
                     "ldr %0, [%1]"
                     : "=&r"(x)
                     : "r"(base)
                     : "memory");
  }
  return x;
}

static uintptr_t pointer_chase_steps(void *start, uint64_t steps) {
  void *p = start;
  uint64_t blocks = steps / 8;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("ldr %0, [%0]\n\t"
                     "ldr %0, [%0]\n\t"
                     "ldr %0, [%0]\n\t"
                     "ldr %0, [%0]\n\t"
                     "ldr %0, [%0]\n\t"
                     "ldr %0, [%0]\n\t"
                     "ldr %0, [%0]\n\t"
                     "ldr %0, [%0]"
                     : "+r"(p)
                     :
                     : "memory");
  }
  for (uint64_t i = blocks * 8; i < steps; i++) {
    __asm__ volatile("ldr %0, [%0]" : "+r"(p) : : "memory");
  }
  return (uintptr_t)p;
}

static void *make_pointer_ring(size_t nodes, size_t stride) {
  const size_t bytes = nodes * stride;
  void *buffer = NULL;
  if (nodes == 0 || (nodes & (nodes - 1U)) != 0) {
    return NULL;
  }
  if (posix_memalign(&buffer, 16384, bytes) != 0) {
    return NULL;
  }
  memset(buffer, 0, bytes);
  size_t mask = nodes - 1U;
  for (size_t i = 0; i < nodes; i++) {
    size_t current = (i * 97U) & mask;
    size_t next = ((i + 1U) * 97U) & mask;
    void **slot = (void **)((char *)buffer + current * stride);
    *slot = (char *)buffer + next * stride;
  }
  return buffer;
}

static struct measurement measure_pointer_ring(const char *section,
                                               const char *name,
                                               const char *variant,
                                               size_t nodes, size_t stride,
                                               uint64_t steps,
                                               uint32_t samples) {
  void *ring = make_pointer_ring(nodes, stride);
  uint64_t *raw = (uint64_t *)calloc(samples, sizeof(*raw));
  if (ring == NULL || raw == NULL) {
    fprintf(stderr, "failed to allocate pointer ring\n");
    exit(2);
  }
  g_sink_ptr ^= pointer_chase_steps(ring, 4096);
  for (uint32_t i = 0; i < samples; i++) {
    uint64_t start = probe_read_cntvct();
    g_sink_ptr ^= pointer_chase_steps(ring, steps);
    uint64_t end = probe_read_cntvct();
    raw[i] = end - start;
  }
  struct measurement m;
  m.section = section;
  m.name = name;
  m.variant = variant;
  m.ops = steps;
  m.raw = probe_stats_from(raw, samples);
  m.baseline = (struct probe_stats){0, 0, 0, 0.0, 0.0, 0.0};
  m.corrected_ticks = m.raw.median;
  m.ns_per_op = ticks_to_ns(m.corrected_ticks) / (double)m.ops;
  free(raw);
  free(ring);
  return m;
}

static uint64_t mlp_chase(void **rings, uint32_t streams, uint64_t steps) {
  void *p0 = rings[0], *p1 = rings[1], *p2 = rings[2], *p3 = rings[3];
  uint64_t blocks = steps / 4;
  for (uint64_t i = 0; i < blocks; i++) {
    if (streams == 1) {
      __asm__ volatile("ldr %0, [%0]\n\t"
                       "ldr %0, [%0]\n\t"
                       "ldr %0, [%0]\n\t"
                       "ldr %0, [%0]"
                       : "+r"(p0)
                       :
                       : "memory");
    } else if (streams == 2) {
      __asm__ volatile("ldr %0, [%0]\n\t"
                       "ldr %1, [%1]\n\t"
                       "ldr %0, [%0]\n\t"
                       "ldr %1, [%1]"
                       : "+r"(p0), "+r"(p1)
                       :
                       : "memory");
    } else {
      __asm__ volatile("ldr %0, [%0]\n\t"
                       "ldr %1, [%1]\n\t"
                       "ldr %2, [%2]\n\t"
                       "ldr %3, [%3]"
                       : "+r"(p0), "+r"(p1), "+r"(p2), "+r"(p3)
                       :
                       : "memory");
    }
  }
  return (uintptr_t)p0 ^ (uintptr_t)p1 ^ (uintptr_t)p2 ^ (uintptr_t)p3;
}

static struct measurement measure_mlp(uint32_t streams, uint64_t steps,
                                      uint32_t samples) {
  void *rings[4] = {NULL, NULL, NULL, NULL};
  uint64_t *raw = (uint64_t *)calloc(samples, sizeof(*raw));
  for (uint32_t i = 0; i < streams; i++) {
    rings[i] = make_pointer_ring(4096, 64);
    if (rings[i] == NULL) {
      fprintf(stderr, "failed to allocate MLP ring\n");
      exit(2);
    }
  }
  char *variant = (char *)calloc(16, 1);
  if (raw == NULL || variant == NULL) {
    fprintf(stderr, "failed to allocate MLP samples\n");
    exit(2);
  }
  snprintf(variant, 16, "%u_streams", streams);
  g_sink_ptr ^= mlp_chase(rings, streams, 4096);
  for (uint32_t i = 0; i < samples; i++) {
    uint64_t start = probe_read_cntvct();
    g_sink_ptr ^= mlp_chase(rings, streams, steps);
    uint64_t end = probe_read_cntvct();
    raw[i] = end - start;
  }
  struct measurement m;
  m.section = "ooo_memory";
  m.name = "independent_chase";
  m.variant = variant;
  m.ops = steps;
  m.raw = probe_stats_from(raw, samples);
  m.baseline = (struct probe_stats){0, 0, 0, 0.0, 0.0, 0.0};
  m.corrected_ticks = m.raw.median;
  m.ns_per_op = ticks_to_ns(m.corrected_ticks) / (double)steps;
  for (uint32_t i = 0; i < streams; i++) {
    free(rings[i]);
  }
  free(raw);
  return m;
}

static uint64_t kernel_branch_taken(uint64_t blocks) {
  uint64_t x = blocks;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("b 1f\n\t"
                     "1: add %0, %0, #1\n\t"
                     "b 2f\n\t"
                     "2: add %0, %0, #1\n\t"
                     "b 3f\n\t"
                     "3: add %0, %0, #1\n\t"
                     "b 4f\n\t"
                     "4: add %0, %0, #1"
                     : "+r"(x));
  }
  return x;
}

static uint64_t kernel_branch_cond_not_taken(uint64_t blocks) {
  uint64_t x = 1;
  const uint64_t y = 2;
  for (uint64_t i = 0; i < blocks; i++) {
    __asm__ volatile("cmp %0, %1\n\t"
                     "b.eq 1f\n\t"
                     "1: cmp %0, %1\n\t"
                     "b.eq 2f\n\t"
                     "2: cmp %0, %1\n\t"
                     "b.eq 3f\n\t"
                     "3: cmp %0, %1\n\t"
                     "b.eq 4f\n\t"
                     "4: add %0, %0, #1\n\t"
                     "sub %0, %0, #1"
                     : "+r"(x)
                     : "r"(y)
                     : "cc");
  }
  return x;
}

__attribute__((noinline)) static uint64_t tiny_call(uint64_t x) {
  __asm__ volatile("" : "+r"(x));
  return x + 1;
}

static uint64_t kernel_call_ret(uint64_t blocks) {
  uint64_t x = 1;
  for (uint64_t i = 0; i < blocks; i++) {
    x = tiny_call(x);
    x = tiny_call(x);
    x = tiny_call(x);
    x = tiny_call(x);
  }
  return x;
}

static void run_hygiene(const struct options *opts) {
  print_section("Measurement Hygiene", opts->csv);
  PRINT_KERNEL("hygiene", "empty_loop", "loop_only", kernel_empty,
               opts->blocks, 1, opts->samples, false, opts->csv);
  PRINT_KERNEL("hygiene", "nop", "8_per_block", kernel_nop8, opts->blocks, 8,
               opts->samples, true, opts->csv);
}

static void run_instruction_matrix(const struct options *opts) {
  uint64_t div_blocks = opts->blocks / 16;
  if (div_blocks < 1024) {
    div_blocks = 1024;
  }
  print_section("Scalar Integer Instruction Matrix", opts->csv);
  PRINT_KERNEL("int_scalar", "dep_add", "8_chain", kernel_dep_add, opts->blocks,
               8, opts->samples, true, opts->csv);
  PRINT_KERNEL("int_scalar", "dep_sub", "8_chain", kernel_dep_sub, opts->blocks,
               8, opts->samples, true, opts->csv);
  PRINT_KERNEL("int_scalar", "dep_logic", "8_mixed", kernel_dep_logic,
               opts->blocks, 8, opts->samples, true, opts->csv);
  PRINT_KERNEL("int_scalar", "dep_shift", "8_mixed", kernel_dep_shift,
               opts->blocks, 8, opts->samples, true, opts->csv);
  PRINT_KERNEL("int_scalar", "dep_mul", "8_chain", kernel_dep_mul, opts->blocks,
               8, opts->samples, true, opts->csv);
  PRINT_KERNEL("int_scalar", "dep_madd", "4_chain", kernel_dep_madd,
               opts->blocks, 4, opts->samples, true, opts->csv);
  PRINT_KERNEL("int_scalar", "dep_umulh", "4_chain", kernel_dep_umulh,
               opts->blocks, 4, opts->samples, true, opts->csv);
  PRINT_KERNEL("int_scalar", "dep_udiv", "2_chain", kernel_dep_udiv,
               div_blocks, 2, opts->samples, true, opts->csv);
  PRINT_KERNEL("int_scalar", "thr_add", "8_independent", kernel_thr_add8,
               opts->blocks, 8, opts->samples, true, opts->csv);
  PRINT_KERNEL("int_scalar", "thr_mul", "4_independent", kernel_thr_mul4,
               opts->blocks, 4, opts->samples, true, opts->csv);

  print_section("FP And NEON Instruction Matrix", opts->csv);
  PRINT_KERNEL("fp_scalar", "dep_fadd64", "8_chain", kernel_dep_fadd,
               opts->blocks, 8, opts->samples, true, opts->csv);
  PRINT_KERNEL("fp_scalar", "dep_fmul64", "8_chain", kernel_dep_fmul,
               opts->blocks, 8, opts->samples, true, opts->csv);
  PRINT_KERNEL("fp_scalar", "dep_fmadd64", "4_chain", kernel_dep_fmadd,
               opts->blocks, 4, opts->samples, true, opts->csv);
  PRINT_KERNEL("fp_scalar", "thr_fadd64", "4_independent", kernel_thr_fadd4,
               opts->blocks, 4, opts->samples, true, opts->csv);
  PRINT_KERNEL("fp_scalar", "thr_fmul64", "4_independent", kernel_thr_fmul4,
               opts->blocks, 4, opts->samples, true, opts->csv);
  PRINT_KERNEL("neon", "fadd32x4", "4_chain", kernel_neon_fadd, opts->blocks,
               16, opts->samples, true, opts->csv);
  PRINT_KERNEL("neon", "fmla32x4", "4_chain", kernel_neon_fmla, opts->blocks,
               16, opts->samples, true, opts->csv);
}

static void run_load_store(const struct options *opts) {
  print_section("Load Store Unit Proxies", opts->csv);
  PRINT_KERNEL("load_store", "l1_load", "8_independent", kernel_l1_load_ind8,
               opts->blocks, 8, opts->samples, true, opts->csv);
  PRINT_KERNEL("load_store", "l1_store", "8_independent", kernel_l1_store8,
               opts->blocks, 8, opts->samples, true, opts->csv);
  PRINT_KERNEL("load_store", "store_load", "forward_same_addr",
               kernel_store_load_forward, opts->blocks, 2, opts->samples, true,
               opts->csv);
  PRINT_KERNEL("load_store", "load_align", "aligned", kernel_load_aligned,
               opts->blocks, 8, opts->samples, true, opts->csv);
  PRINT_KERNEL("load_store", "load_align", "unaligned+1",
               kernel_load_unaligned, opts->blocks, 8, opts->samples, true,
               opts->csv);
  PRINT_KERNEL("load_store", "load_align", "split_line",
               kernel_load_split_line, opts->blocks, 8, opts->samples, true,
               opts->csv);
}

static void run_frontend(const struct options *opts) {
  print_section("Frontend Proxies", opts->csv);
  PRINT_KERNEL("frontend", "nop_stream", "8_nops", kernel_nop8, opts->blocks,
               8, opts->samples, true, opts->csv);
  PRINT_KERNEL("frontend", "branch", "uncond_taken4", kernel_branch_taken,
               opts->blocks, 4, opts->samples, true, opts->csv);
  PRINT_KERNEL("frontend", "branch", "cond_not_taken4",
               kernel_branch_cond_not_taken, opts->blocks, 4, opts->samples,
               true, opts->csv);
  PRINT_KERNEL("frontend", "call_ret", "4_calls", kernel_call_ret,
               opts->blocks / 4, 4, opts->samples, true, opts->csv);
}

static void run_cache_tlb(const struct options *opts) {
  static const size_t cache_sizes_full[] = {
      4U * 1024U,       8U * 1024U,       16U * 1024U,
      32U * 1024U,      64U * 1024U,      128U * 1024U,
      256U * 1024U,     512U * 1024U,     1024U * 1024U,
      2U * 1024U * 1024U, 4U * 1024U * 1024U, 8U * 1024U * 1024U,
      16U * 1024U * 1024U, 32U * 1024U * 1024U};
  static const size_t cache_sizes_smoke[] = {4U * 1024U, 64U * 1024U,
                                             128U * 1024U, 256U * 1024U,
                                             1024U * 1024U};
  const size_t *sizes = opts->smoke ? cache_sizes_smoke : cache_sizes_full;
  size_t count = opts->smoke ? sizeof(cache_sizes_smoke) / sizeof(sizes[0])
                             : sizeof(cache_sizes_full) / sizeof(sizes[0]);

  print_section("Cache Pointer Chase", opts->csv);
  for (size_t i = 0; i < count; i++) {
    char variant[32];
    probe_format_bytes(variant, sizeof(variant), sizes[i]);
    uint64_t steps = opts->smoke ? 32768U : 262144U;
    size_t nodes = sizes[i] / 64U;
    if (steps < nodes * 16U) {
      steps = nodes * 16U;
    }
    struct measurement m = measure_pointer_ring("cache", "ptr_chase_64B",
                                                strdup(variant), nodes, 64,
                                                steps, opts->samples);
    print_measurement(&m, opts->csv);
    free((void *)m.variant);
  }

  print_section("TLB Pointer Chase", opts->csv);
  const size_t page = (size_t)getpagesize();
  static const size_t pages_full[] = {16, 32, 64, 128, 256, 512, 1024, 2048,
                                      4096};
  static const size_t pages_smoke[] = {16, 64, 256, 1024};
  const size_t *pages = opts->smoke ? pages_smoke : pages_full;
  size_t page_count = opts->smoke ? sizeof(pages_smoke) / sizeof(pages[0])
                                  : sizeof(pages_full) / sizeof(pages[0]);
  for (size_t i = 0; i < page_count; i++) {
    char variant[32];
    snprintf(variant, sizeof(variant), "%zu_pages", pages[i]);
    uint64_t steps = opts->smoke ? 32768U : 131072U;
    if (steps < pages[i] * 16U) {
      steps = pages[i] * 16U;
    }
    struct measurement m = measure_pointer_ring("tlb", "ptr_chase_page",
                                                strdup(variant), pages[i], page,
                                                steps, opts->samples);
    print_measurement(&m, opts->csv);
    free((void *)m.variant);
  }
}

static void run_ooo_memory(const struct options *opts) {
  uint64_t steps = opts->smoke ? 32768U : 131072U;
  print_section("OoO And Memory-Level Parallelism Proxies", opts->csv);
  PRINT_KERNEL("ooo_integer", "ind_add", "1_chain", kernel_dep_add,
               opts->blocks, 8, opts->samples, true, opts->csv);
  PRINT_KERNEL("ooo_integer", "ind_add", "8_chains", kernel_thr_add8,
               opts->blocks, 8, opts->samples, true, opts->csv);
  struct measurement mlp1 = measure_mlp(1, steps, opts->samples);
  struct measurement mlp2 = measure_mlp(2, steps, opts->samples);
  struct measurement mlp4 = measure_mlp(4, steps, opts->samples);
  print_measurement(&mlp1, opts->csv);
  print_measurement(&mlp2, opts->csv);
  print_measurement(&mlp4, opts->csv);
  free((void *)mlp1.variant);
  free((void *)mlp2.variant);
  free((void *)mlp4.variant);
}

struct qos_task {
  qos_class_t qos;
  const char *label;
  uint64_t blocks;
  uint32_t samples;
  struct measurement m;
};

static void *qos_main(void *arg) {
  struct qos_task *task = (struct qos_task *)arg;
  (void)pthread_set_qos_class_self_np(task->qos, 0);
  task->m = measure_kernel("core_class", "qos_dep_add", task->label,
                           kernel_dep_add, task->blocks, 8, task->samples, true);
  return NULL;
}

static void run_qos(const struct options *opts) {
  struct qos_task tasks[] = {
      {QOS_CLASS_USER_INTERACTIVE, "user_interactive", opts->blocks,
       opts->samples, {0}},
      {QOS_CLASS_USER_INITIATED, "user_initiated", opts->blocks, opts->samples,
       {0}},
      {QOS_CLASS_UTILITY, "utility", opts->blocks, opts->samples, {0}},
      {QOS_CLASS_BACKGROUND, "background", opts->blocks, opts->samples, {0}},
  };
  print_section("Core-Class QoS Hints", opts->csv);
  for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); i++) {
    pthread_t thread;
    int rc = pthread_create(&thread, NULL, qos_main, &tasks[i]);
    if (rc != 0) {
      fprintf(stderr, "pthread_create: %s\n", strerror(rc));
      exit(2);
    }
    rc = pthread_join(thread, NULL);
    if (rc != 0) {
      fprintf(stderr, "pthread_join: %s\n", strerror(rc));
      exit(2);
    }
    print_measurement(&tasks[i].m, opts->csv);
  }
}

struct scale_task {
  atomic_int *start;
  uint64_t blocks;
  uint64_t result;
};

static void *scale_main(void *arg) {
  struct scale_task *task = (struct scale_task *)arg;
  (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  while (atomic_load_explicit(task->start, memory_order_acquire) == 0) {
  }
  task->result = kernel_thr_add8(task->blocks);
  return NULL;
}

static void run_thread_scaling(const struct options *opts) {
  static const int full_counts[] = {1, 2, 4, 8, 10, 14};
  static const int smoke_counts[] = {1, 4};
  const int *counts = opts->smoke ? smoke_counts : full_counts;
  size_t count_len = opts->smoke ? sizeof(smoke_counts) / sizeof(counts[0])
                                 : sizeof(full_counts) / sizeof(counts[0]);
  print_section("Thread Scaling", opts->csv);
  for (size_t ci = 0; ci < count_len; ci++) {
    int threads = counts[ci];
    pthread_t *ids = (pthread_t *)calloc((size_t)threads, sizeof(*ids));
    struct scale_task *tasks =
        (struct scale_task *)calloc((size_t)threads, sizeof(*tasks));
    atomic_int start;
    atomic_init(&start, 0);
    if (ids == NULL || tasks == NULL) {
      fprintf(stderr, "failed to allocate thread tasks\n");
      exit(2);
    }
    uint64_t per_thread_blocks = opts->blocks / 2;
    for (int i = 0; i < threads; i++) {
      tasks[i].start = &start;
      tasks[i].blocks = per_thread_blocks;
      int rc = pthread_create(&ids[i], NULL, scale_main, &tasks[i]);
      if (rc != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(rc));
        exit(2);
      }
    }
    sleep_for_ns(1000000L);
    uint64_t start_tick = probe_read_cntvct();
    atomic_store_explicit(&start, 1, memory_order_release);
    for (int i = 0; i < threads; i++) {
      int rc = pthread_join(ids[i], NULL);
      if (rc != 0) {
        fprintf(stderr, "pthread_join: %s\n", strerror(rc));
        exit(2);
      }
      g_sink_u64 ^= tasks[i].result;
    }
    uint64_t elapsed = probe_read_cntvct() - start_tick;
    uint64_t ops = (uint64_t)threads * per_thread_blocks * 8U;
    struct measurement m;
    char *variant = (char *)calloc(24, 1);
    if (variant == NULL) {
      fprintf(stderr, "failed to allocate variant\n");
      exit(2);
    }
    snprintf(variant, 24, "%d_threads", threads);
    m.section = "thread_scaling";
    m.name = "thr_add8";
    m.variant = variant;
    m.ops = ops;
    m.raw = (struct probe_stats){elapsed, elapsed, elapsed, (double)elapsed, 0.0,
                                 0.0};
    m.baseline = (struct probe_stats){0, 0, 0, 0.0, 0.0, 0.0};
    m.corrected_ticks = elapsed;
    m.ns_per_op = ticks_to_ns(elapsed) / (double)ops;
    print_measurement(&m, opts->csv);
    free((void *)m.variant);
    free(ids);
    free(tasks);
  }
}

int main(int argc, char **argv) {
  struct options opts;
  if (!parse_options(argc, argv, &opts)) {
    usage(argv[0]);
    return 2;
  }

  print_header(opts.csv);
  run_host(opts.csv);
  run_timer(opts.csv);
  run_hygiene(&opts);
  run_qos(&opts);
  run_instruction_matrix(&opts);
  run_load_store(&opts);
  run_frontend(&opts);
  run_cache_tlb(&opts);
  run_ooo_memory(&opts);
  run_thread_scaling(&opts);

  if (!opts.csv) {
    printf("\nSinks: u64=%" PRIu64 " ptr=%" PRIuPTR "\n", g_sink_u64,
           g_sink_ptr);
  }
  free(g_mem);
  return 0;
}
