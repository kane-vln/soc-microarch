#include "probe_helpers.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

struct options {
  bool smoke;
  bool csv;
  uint32_t samples;
  uint64_t blocks;
};

struct bench_summary {
  const char *group;
  const char *name;
  const char *detail;
  uint64_t ops;
  struct probe_stats ticks;
  double ns_per_op;
};

typedef uint64_t (*kernel_fn)(uint64_t blocks);

static volatile uint64_t g_sink_u64;
static volatile uintptr_t g_sink_ptr;

static double ticks_to_ns(uint64_t ticks) {
  return probe_cycles_to_ns(ticks, (double)probe_read_cntfrq());
}

static uint64_t round_down_u64(uint64_t value, uint64_t multiple) {
  return multiple == 0 ? value : value - (value % multiple);
}

static void usage(const char *argv0) {
  printf("Usage: %s [--smoke] [--csv] [--samples N] [--blocks N]\n", argv0);
  printf("\n");
  printf("Exploratory Apple Silicon CPU-core probes. CNTVCT_EL0 is a fixed-rate\n");
  printf("timer, so reported per-op values are nanoseconds/op, not core cycles/op.\n");
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
  opts->samples = 9;
  opts->blocks = 1000000;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--smoke") == 0) {
      opts->smoke = true;
      opts->samples = 3;
      opts->blocks = 100000;
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
  opts->blocks = round_down_u64(opts->blocks, 8);
  if (opts->blocks == 0) {
    opts->blocks = 8;
  }
  return true;
}

static void print_text_header(const char *title, bool csv) {
  if (!csv) {
    printf("\n== %s ==\n", title);
  }
}

static void print_summary(const struct bench_summary *summary, bool csv) {
  if (csv) {
    printf("%s,%s,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%.6f,%.6f\n",
           summary->group, summary->name, summary->detail, summary->ops,
           summary->ticks.min, summary->ticks.median, summary->ticks.max,
           summary->ticks.mean, summary->ns_per_op);
    return;
  }

  printf("  %-18s %-12s ops=%-12" PRIu64 " median_ticks=%-10" PRIu64
         " ns/op=%.6f\n",
         summary->name, summary->detail, summary->ops, summary->ticks.median,
         summary->ns_per_op);
}

static void print_sysctl_string(const char *label, const char *name, bool csv) {
  char value[256];
  size_t len = sizeof(value);
  if (sysctlbyname(name, value, &len, NULL, 0) != 0) {
    if (csv) {
      printf("host,%s,unavailable\n", label);
    } else {
      printf("  %-30s unavailable (%s)\n", label, strerror(errno));
    }
    return;
  }
  value[sizeof(value) - 1] = '\0';
  if (csv) {
    printf("host,%s,%s\n", label, value);
  } else {
    printf("  %-30s %s\n", label, value);
  }
}

static void print_sysctl_number(const char *label, const char *name, bool csv) {
  uint64_t raw = 0;
  size_t len = sizeof(raw);
  if (sysctlbyname(name, &raw, &len, NULL, 0) != 0) {
    if (csv) {
      printf("host,%s,unavailable\n", label);
    } else {
      printf("  %-30s unavailable (%s)\n", label, strerror(errno));
    }
    return;
  }

  uint64_t value = raw;
  if (len == sizeof(uint32_t)) {
    value = *(uint32_t *)&raw;
  }
  if (csv) {
    printf("host,%s,%" PRIu64 "\n", label, value);
  } else {
    printf("  %-30s %" PRIu64 "\n", label, value);
  }
}

static void run_host_census(bool csv) {
  print_text_header("Host", csv);
  print_sysctl_string("brand", "machdep.cpu.brand_string", csv);
  print_sysctl_number("logical_cpus", "hw.ncpu", csv);
  print_sysctl_number("p_physical_cpus", "hw.perflevel0.physicalcpu", csv);
  print_sysctl_number("e_physical_cpus", "hw.perflevel1.physicalcpu", csv);
  print_sysctl_number("p_l1i_bytes", "hw.perflevel0.l1icachesize", csv);
  print_sysctl_number("p_l1d_bytes", "hw.perflevel0.l1dcachesize", csv);
  print_sysctl_number("e_l1i_bytes", "hw.perflevel1.l1icachesize", csv);
  print_sysctl_number("e_l1d_bytes", "hw.perflevel1.l1dcachesize", csv);
  print_sysctl_number("feat_sme", "hw.optional.arm.FEAT_SME", csv);
}

static void sleep_for_ns(long ns) {
  struct timespec req;
  req.tv_sec = ns / 1000000000L;
  req.tv_nsec = ns % 1000000000L;
  while (nanosleep(&req, &req) != 0 && errno == EINTR) {
  }
}

static void run_timer_calibration(bool csv) {
  const long sleep_ns = 20000000L;
  uint64_t cntfrq = probe_read_cntfrq();
  uint64_t cnt_a = probe_read_cntvct();
  uint64_t mach_a = probe_read_mach_time();
  sleep_for_ns(sleep_ns);
  uint64_t cnt_b = probe_read_cntvct();
  uint64_t mach_b = probe_read_mach_time();
  uint64_t cnt_delta = cnt_b - cnt_a;
  uint64_t mach_delta = mach_b - mach_a;

  print_text_header("Timer Calibration", csv);
  if (csv) {
    printf("timer,cntfrq_hz,%" PRIu64 "\n", cntfrq);
    printf("timer,cntvct_delta_ticks,%" PRIu64 "\n", cnt_delta);
    printf("timer,cntvct_delta_ns,%.3f\n", ticks_to_ns(cnt_delta));
    printf("timer,mach_delta_ticks,%" PRIu64 "\n", mach_delta);
    printf("timer,mach_delta_ns,%.3f\n", probe_mach_ticks_to_ns(mach_delta));
    return;
  }

  printf("  CNTFRQ_EL0                  %" PRIu64 " Hz\n", cntfrq);
  printf("  CNTVCT over 20 ms sleep     %" PRIu64 " ticks, %.3f ns\n",
         cnt_delta, ticks_to_ns(cnt_delta));
  printf("  mach_absolute_time delta    %" PRIu64 " ticks, %.3f ns\n",
         mach_delta, probe_mach_ticks_to_ns(mach_delta));
}

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

static uint64_t kernel_ind_add(uint64_t blocks) {
  uint64_t a0 = 1, a1 = 2, a2 = 3, a3 = 4;
  uint64_t a4 = 5, a5 = 6, a6 = 7, a7 = 8;
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

static uint64_t kernel_ind_mul(uint64_t blocks) {
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

static uint64_t kernel_ind_fadd(uint64_t blocks) {
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

static uint64_t kernel_ind_fmul(uint64_t blocks) {
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

static struct bench_summary measure_kernel(const char *group, const char *name,
                                           const char *detail, kernel_fn fn,
                                           uint64_t blocks,
                                           uint64_t ops_per_block,
                                           uint32_t samples) {
  uint64_t *ticks = (uint64_t *)calloc(samples, sizeof(*ticks));
  if (ticks == NULL) {
    fprintf(stderr, "failed to allocate samples\n");
    exit(2);
  }

  g_sink_u64 ^= fn(blocks / 8 + 1);
  for (uint32_t i = 0; i < samples; i++) {
    uint64_t start = probe_read_cntvct();
    g_sink_u64 ^= fn(blocks);
    uint64_t end = probe_read_cntvct();
    ticks[i] = end - start;
  }

  struct bench_summary summary;
  summary.group = group;
  summary.name = name;
  summary.detail = detail;
  summary.ops = blocks * ops_per_block;
  summary.ticks = probe_stats_from(ticks, samples);
  summary.ns_per_op = ticks_to_ns(summary.ticks.median) / (double)summary.ops;
  free(ticks);
  return summary;
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

static void *make_pointer_ring(size_t bytes, size_t stride) {
  void *buffer = NULL;
  if (posix_memalign(&buffer, 16384, bytes) != 0) {
    return NULL;
  }
  memset(buffer, 0, bytes);

  size_t nodes = bytes / stride;
  size_t mask = nodes - 1;
  for (size_t i = 0; i < nodes; i++) {
    size_t current = (i * 97U) & mask;
    size_t next = ((i + 1U) * 97U) & mask;
    void **slot = (void **)((char *)buffer + current * stride);
    *slot = (char *)buffer + next * stride;
  }
  return buffer;
}

static struct bench_summary measure_pointer_chase(const char *group,
                                                  const char *detail,
                                                  size_t bytes,
                                                  uint64_t steps,
                                                  uint32_t samples) {
  uint64_t *ticks = (uint64_t *)calloc(samples, sizeof(*ticks));
  void *ring = make_pointer_ring(bytes, 64);
  if (ticks == NULL || ring == NULL) {
    free(ticks);
    free(ring);
    fprintf(stderr, "failed to allocate pointer ring for %zu bytes\n", bytes);
    exit(2);
  }

  g_sink_ptr ^= pointer_chase_steps(ring, 4096);
  for (uint32_t i = 0; i < samples; i++) {
    uint64_t start = probe_read_cntvct();
    g_sink_ptr ^= pointer_chase_steps(ring, steps);
    uint64_t end = probe_read_cntvct();
    ticks[i] = end - start;
  }

  struct bench_summary summary;
  summary.group = group;
  summary.name = "ptr_chase";
  summary.detail = detail;
  summary.ops = steps;
  summary.ticks = probe_stats_from(ticks, samples);
  summary.ns_per_op = ticks_to_ns(summary.ticks.median) / (double)steps;
  free(ticks);
  free(ring);
  return summary;
}

static void run_chain_probes(const struct options *opts) {
  print_text_header("Dependent Latency Chains", opts->csv);
  struct bench_summary dep_add = measure_kernel(
      "latency", "dep_add", "8x", kernel_dep_add, opts->blocks, 8,
      opts->samples);
  struct bench_summary dep_mul = measure_kernel(
      "latency", "dep_mul", "8x", kernel_dep_mul, opts->blocks, 8,
      opts->samples);
  struct bench_summary dep_fadd = measure_kernel(
      "latency", "dep_fadd", "8x", kernel_dep_fadd, opts->blocks, 8,
      opts->samples);
  struct bench_summary dep_fmul = measure_kernel(
      "latency", "dep_fmul", "8x", kernel_dep_fmul, opts->blocks, 8,
      opts->samples);
  print_summary(&dep_add, opts->csv);
  print_summary(&dep_mul, opts->csv);
  print_summary(&dep_fadd, opts->csv);
  print_summary(&dep_fmul, opts->csv);
}

static void run_throughput_probes(const struct options *opts) {
  print_text_header("Independent Throughput Loops", opts->csv);
  struct bench_summary ind_add = measure_kernel(
      "throughput", "ind_add", "8-way", kernel_ind_add, opts->blocks, 8,
      opts->samples);
  struct bench_summary ind_mul = measure_kernel(
      "throughput", "ind_mul", "4-way", kernel_ind_mul, opts->blocks, 4,
      opts->samples);
  struct bench_summary ind_fadd = measure_kernel(
      "throughput", "ind_fadd", "4-way", kernel_ind_fadd, opts->blocks, 4,
      opts->samples);
  struct bench_summary ind_fmul = measure_kernel(
      "throughput", "ind_fmul", "4-way", kernel_ind_fmul, opts->blocks, 4,
      opts->samples);
  print_summary(&ind_add, opts->csv);
  print_summary(&ind_mul, opts->csv);
  print_summary(&ind_fadd, opts->csv);
  print_summary(&ind_fmul, opts->csv);
}

struct qos_task {
  qos_class_t qos;
  const char *label;
  uint64_t blocks;
  uint32_t samples;
  struct bench_summary summary;
  int set_qos_result;
};

static void *qos_thread_main(void *arg) {
  struct qos_task *task = (struct qos_task *)arg;
  task->set_qos_result = pthread_set_qos_class_self_np(task->qos, 0);
  task->summary = measure_kernel("qos", "dep_add", task->label, kernel_dep_add,
                                 task->blocks, 8, task->samples);
  return NULL;
}

static void run_qos_probes(const struct options *opts) {
  struct qos_task tasks[] = {
      {QOS_CLASS_USER_INTERACTIVE, "user_interactive", opts->blocks,
       opts->samples, {0}, 0},
      {QOS_CLASS_BACKGROUND, "background", opts->blocks, opts->samples, {0}, 0},
  };

  print_text_header("QoS Core-Class Hint Probe", opts->csv);
  for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); i++) {
    pthread_t thread;
    int rc = pthread_create(&thread, NULL, qos_thread_main, &tasks[i]);
    if (rc != 0) {
      fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
      exit(2);
    }
    rc = pthread_join(thread, NULL);
    if (rc != 0) {
      fprintf(stderr, "pthread_join failed: %s\n", strerror(rc));
      exit(2);
    }
    if (tasks[i].set_qos_result != 0 && !opts->csv) {
      printf("  %-18s qos_set_failed=%s\n", tasks[i].label,
             strerror(tasks[i].set_qos_result));
    }
    print_summary(&tasks[i].summary, opts->csv);
  }
}

static void format_size(char *out, size_t out_len, size_t bytes) {
  if (bytes >= 1024U * 1024U) {
    snprintf(out, out_len, "%zuM", bytes / (1024U * 1024U));
  } else {
    snprintf(out, out_len, "%zuK", bytes / 1024U);
  }
}

static void run_cache_sweep(const struct options *opts) {
  static const size_t full_sizes[] = {
      4U * 1024U,       8U * 1024U,       16U * 1024U,
      32U * 1024U,      64U * 1024U,      128U * 1024U,
      256U * 1024U,     512U * 1024U,     1024U * 1024U,
      2U * 1024U * 1024U, 4U * 1024U * 1024U, 8U * 1024U * 1024U,
      16U * 1024U * 1024U};
  static const size_t smoke_sizes[] = {4U * 1024U, 64U * 1024U,
                                       128U * 1024U, 256U * 1024U,
                                       1024U * 1024U};
  const size_t *sizes = opts->smoke ? smoke_sizes : full_sizes;
  size_t count = opts->smoke ? sizeof(smoke_sizes) / sizeof(smoke_sizes[0])
                             : sizeof(full_sizes) / sizeof(full_sizes[0]);

  print_text_header("Pointer-Chase Cache Sweep", opts->csv);
  for (size_t i = 0; i < count; i++) {
    size_t bytes = sizes[i];
    size_t lines = bytes / 64U;
    uint64_t steps = opts->smoke ? 32768U : 262144U;
    if ((uint64_t)lines * 16U > steps) {
      steps = (uint64_t)lines * 16U;
    }
    char detail[32];
    format_size(detail, sizeof(detail), bytes);
    struct bench_summary summary =
        measure_pointer_chase("cache", detail, bytes, steps, opts->samples);
    print_summary(&summary, opts->csv);
  }
}

int main(int argc, char **argv) {
  struct options opts;
  if (!parse_options(argc, argv, &opts)) {
    usage(argv[0]);
    return 2;
  }

  if (opts.csv) {
    printf("section,name,detail,ops,min_ticks,median_ticks,max_ticks,mean_ticks,"
           "median_ns_per_op\n");
  }

  run_host_census(opts.csv);
  run_timer_calibration(opts.csv);
  run_qos_probes(&opts);
  run_chain_probes(&opts);
  run_throughput_probes(&opts);
  run_cache_sweep(&opts);

  if (!opts.csv) {
    printf("\nSinks: u64=%" PRIu64 " ptr=%" PRIuPTR "\n", g_sink_u64,
           g_sink_ptr);
  }
  return 0;
}
