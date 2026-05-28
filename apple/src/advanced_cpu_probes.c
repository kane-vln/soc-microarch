#include "probe_helpers.h"

#include <dlfcn.h>
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

struct options {
  bool smoke;
  bool csv;
  uint32_t samples;
  uint64_t iters;
};

struct sample_result {
  const char *section;
  const char *name;
  const char *variant;
  const char *note;
  uint64_t ops;
  uint64_t bytes;
  struct probe_stats ticks;
  double ns_per_op;
  double gib_per_s;
};

typedef uint64_t (*kernel_fn)(void *ctx, uint64_t iters);

static volatile uint64_t g_sink_u64;
static volatile uintptr_t g_sink_ptr;

static double ticks_to_ns(uint64_t ticks) {
  return probe_cycles_to_ns(ticks, (double)probe_read_cntfrq());
}

static void usage(const char *argv0) {
  printf("Usage: %s [--smoke] [--csv] [--samples N] [--iters N]\n", argv0);
  printf("\n");
  printf("Advanced Apple Silicon CPU probes: frontend predictors, RAS depth,\n");
  printf("randomized load/store, bandwidth, MLP, QoS saturation, and PMU checks.\n");
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
  opts->iters = 200000;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--smoke") == 0) {
      opts->smoke = true;
      opts->samples = 3;
      opts->iters = 40000;
    } else if (strcmp(argv[i], "--csv") == 0) {
      opts->csv = true;
    } else if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
      uint64_t value = 0;
      if (!parse_u64(argv[++i], &value) || value == 0 || value > 1000) {
        fprintf(stderr, "invalid --samples value\n");
        return false;
      }
      opts->samples = (uint32_t)value;
    } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
      uint64_t value = 0;
      if (!parse_u64(argv[++i], &value) || value == 0) {
        fprintf(stderr, "invalid --iters value\n");
        return false;
      }
      opts->iters = value;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(argv[0]);
      exit(0);
    } else {
      fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return false;
    }
  }
  return true;
}

static void print_header(bool csv) {
  if (csv) {
    printf("section,name,variant,value,unit,ops,bytes,median_ticks,ns_per_op,"
           "gib_per_s,cv_percent,note\n");
  }
}

static void print_section(const char *name, bool csv) {
  if (!csv) {
    printf("\n== %s ==\n", name);
  }
}

static void print_fact(const char *section, const char *name,
                       const char *variant, const char *value,
                       const char *unit, const char *note, bool csv) {
  if (csv) {
    printf("%s,%s,%s,%s,%s,0,0,0,0,0,0,%s\n", section, name, variant, value,
           unit, note == NULL ? "" : note);
  } else {
    printf("  %-18s %-20s %-18s %-8s %s\n", name, variant, value, unit,
           note == NULL ? "" : note);
  }
}

static struct sample_result measure_kernel(const char *section, const char *name,
                                           const char *variant,
                                           const char *note, kernel_fn fn,
                                           void *ctx, uint64_t iters,
                                           uint64_t ops_per_iter,
                                           uint64_t bytes_per_iter,
                                           uint32_t samples) {
  uint64_t *ticks = (uint64_t *)calloc(samples, sizeof(*ticks));
  if (ticks == NULL) {
    fprintf(stderr, "failed to allocate sample buffer\n");
    exit(2);
  }

  g_sink_u64 ^= fn(ctx, iters / 16 + 1);
  for (uint32_t i = 0; i < samples; i++) {
    uint64_t start = probe_read_cntvct();
    g_sink_u64 ^= fn(ctx, iters);
    uint64_t end = probe_read_cntvct();
    ticks[i] = end - start;
  }

  struct sample_result result;
  result.section = section;
  result.name = name;
  result.variant = variant;
  result.note = note;
  result.ops = iters * ops_per_iter;
  result.bytes = iters * bytes_per_iter;
  result.ticks = probe_stats_from(ticks, samples);
  result.ns_per_op =
      result.ops == 0 ? 0.0 : ticks_to_ns(result.ticks.median) / (double)result.ops;
  result.gib_per_s =
      result.bytes == 0 ? 0.0
                        : probe_gib_per_second(result.bytes,
                                               ticks_to_ns(result.ticks.median));
  free(ticks);
  return result;
}

static void print_result(const struct sample_result *result, bool csv) {
  if (csv) {
    printf("%s,%s,%s,,ns,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.9f,%.6f,"
           "%.6f,%s\n",
           result->section, result->name, result->variant, result->ops,
           result->bytes, result->ticks.median, result->ns_per_op,
           result->gib_per_s, result->ticks.cv_percent,
           result->note == NULL ? "" : result->note);
    return;
  }
  printf("  %-18s %-20s ops=%-12" PRIu64 " bytes=%-12" PRIu64
         " ns/op=%-11.6f GiB/s=%-10.3f cv=%6.2f%% %s\n",
         result->name, result->variant, result->ops, result->bytes,
         result->ns_per_op, result->gib_per_s, result->ticks.cv_percent,
         result->note == NULL ? "" : result->note);
}

static uint64_t next_pow2_u64(uint64_t value) {
  if (value <= 1) {
    return 1;
  }
  value--;
  for (unsigned shift = 1; shift < 64; shift <<= 1) {
    value |= value >> shift;
  }
  return value + 1;
}

static void fill_random_u8(uint8_t *data, size_t count, uint32_t seed) {
  uint32_t x = seed;
  for (size_t i = 0; i < count; i++) {
    x = x * 1664525U + 1013904223U;
    data[i] = (uint8_t)(x >> 24);
  }
}

static void fill_permutation(uint32_t *idx, size_t count) {
  size_t mask = count - 1U;
  size_t step = count > 65536U ? 65537U : 8191U;
  if ((step & 1U) == 0) {
    step++;
  }
  for (size_t i = 0; i < count; i++) {
    idx[i] = (uint32_t)((i * step) & mask);
  }
}

static void run_host_and_pmu(bool csv) {
  print_section("PMU And Host Capability", csv);
  char value[64];
  snprintf(value, sizeof(value), "%" PRIu64, probe_read_cntfrq());
  print_fact("capability", "cntfrq", "cntfrq_el0", value, "Hz",
             "fixed timer, not core cycles", csv);

  void *kperf = dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf",
                       RTLD_LAZY);
  if (kperf == NULL) {
    kperf = dlopen(
        "/System/Library/PrivateFrameworks/kperf.framework/Versions/A/kperf",
        RTLD_LAZY);
  }
  if (kperf == NULL) {
    print_fact("capability", "kperf", "dlopen", "unavailable", "",
               "PMU counters not collected by this user-mode harness", csv);
  } else {
    void *sym = dlsym(kperf, "kpc_get_counter_count");
    print_fact("capability", "kperf", "dlopen", "available", "",
               sym == NULL ? "framework present but kpc symbol not visible"
                           : "framework and kpc symbol visible",
               csv);
    dlclose(kperf);
  }
}

struct branch_ctx {
  uint8_t *pattern;
  uint64_t mask;
};

static uint64_t kernel_branch_pattern(void *raw, uint64_t iters) {
  struct branch_ctx *ctx = (struct branch_ctx *)raw;
  uint64_t acc = 1;
  for (uint64_t i = 0; i < iters; i++) {
    uint32_t flag = (uint32_t)ctx->pattern[i & ctx->mask];
    __asm__ volatile("cmp %w[flag], #0\n\t"
                     "b.eq 1f\n\t"
                     "add %[acc], %[acc], #3\n\t"
                     "b 2f\n"
                     "1:\n\t"
                     "eor %[acc], %[acc], %[i]\n"
                     "2:"
                     : [acc] "+r"(acc)
                     : [flag] "r"(flag), [i] "r"(i)
                     : "cc");
  }
  return acc;
}

static void run_branch_predictability(const struct options *opts) {
  const size_t count = 4096;
  uint8_t *all_taken = (uint8_t *)calloc(count, 1);
  uint8_t *alternating = (uint8_t *)calloc(count, 1);
  uint8_t *randomish = (uint8_t *)calloc(count, 1);
  if (all_taken == NULL || alternating == NULL || randomish == NULL) {
    fprintf(stderr, "failed to allocate branch patterns\n");
    exit(2);
  }
  for (size_t i = 0; i < count; i++) {
    all_taken[i] = 1;
    alternating[i] = (uint8_t)(i & 1U);
  }
  fill_random_u8(randomish, count, 0x12345678U);
  for (size_t i = 0; i < count; i++) {
    randomish[i] &= 1U;
  }

  struct branch_ctx contexts[] = {
      {all_taken, count - 1U},
      {alternating, count - 1U},
      {randomish, count - 1U},
  };
  const char *variants[] = {"all_taken", "alternating", "lfsr_random"};

  print_section("Branch Predictability", opts->csv);
  for (size_t i = 0; i < sizeof(contexts) / sizeof(contexts[0]); i++) {
    struct sample_result r =
        measure_kernel("frontend_branch", "data_branch", variants[i],
                       "inline asm branch over memory pattern",
                       kernel_branch_pattern, &contexts[i], opts->iters, 1, 0,
                       opts->samples);
    print_result(&r, opts->csv);
  }

  free(all_taken);
  free(alternating);
  free(randomish);
}

__attribute__((noinline)) static uint64_t ras_recurse(uint64_t depth,
                                                      uint64_t x) {
  __asm__ volatile("" : "+r"(x) : : "memory");
  if (depth == 0) {
    return x + 1;
  }
  return ras_recurse(depth - 1, x + 1) + 1;
}

static uint64_t kernel_ras_depth(void *raw, uint64_t iters) {
  const uint64_t depth = *(const uint64_t *)raw;
  uint64_t sum = 0;
  for (uint64_t i = 0; i < iters; i++) {
    sum += ras_recurse(depth, i);
  }
  return sum;
}

static void run_ras_depth(const struct options *opts) {
  static const uint64_t full_depths[] = {1, 2, 4, 8, 16, 24, 32, 48, 64};
  static const uint64_t smoke_depths[] = {1, 8, 32};
  const uint64_t *depths = opts->smoke ? smoke_depths : full_depths;
  size_t count = opts->smoke ? sizeof(smoke_depths) / sizeof(smoke_depths[0])
                             : sizeof(full_depths) / sizeof(full_depths[0]);

  print_section("Return Address Stack Depth Proxy", opts->csv);
  for (size_t i = 0; i < count; i++) {
    char variant[32];
    snprintf(variant, sizeof(variant), "depth_%" PRIu64, depths[i]);
    uint64_t iters = opts->smoke ? opts->iters / 8 : opts->iters / 4;
    if (iters < 1000) {
      iters = 1000;
    }
    uint64_t depth = depths[i];
    struct sample_result r =
        measure_kernel("frontend_ras", "recursive_call", variant,
                       "watch for non-linear jumps as depth grows",
                       kernel_ras_depth, &depth, iters, depth + 1U, 0,
                       opts->samples);
    print_result(&r, opts->csv);
  }
}

#define TINY_BODY(N)                                                            \
  __attribute__((noinline, aligned(64))) static uint64_t tiny_##N(uint64_t x) { \
    __asm__ volatile("nop\n\t"                                                  \
                     "nop\n\t"                                                  \
                     "nop\n\t"                                                  \
                     "nop\n\t"                                                  \
                     "nop\n\t"                                                  \
                     "nop\n\t"                                                  \
                     "nop\n\t"                                                  \
                     "nop"                                                      \
                     : "+r"(x));                                                \
    return x + (uint64_t)(N + 1);                                                \
  }

TINY_BODY(0)
TINY_BODY(1)
TINY_BODY(2)
TINY_BODY(3)
TINY_BODY(4)
TINY_BODY(5)
TINY_BODY(6)
TINY_BODY(7)
TINY_BODY(8)
TINY_BODY(9)
TINY_BODY(10)
TINY_BODY(11)
TINY_BODY(12)
TINY_BODY(13)
TINY_BODY(14)
TINY_BODY(15)
TINY_BODY(16)
TINY_BODY(17)
TINY_BODY(18)
TINY_BODY(19)
TINY_BODY(20)
TINY_BODY(21)
TINY_BODY(22)
TINY_BODY(23)
TINY_BODY(24)
TINY_BODY(25)
TINY_BODY(26)
TINY_BODY(27)
TINY_BODY(28)
TINY_BODY(29)
TINY_BODY(30)
TINY_BODY(31)
TINY_BODY(32)
TINY_BODY(33)
TINY_BODY(34)
TINY_BODY(35)
TINY_BODY(36)
TINY_BODY(37)
TINY_BODY(38)
TINY_BODY(39)
TINY_BODY(40)
TINY_BODY(41)
TINY_BODY(42)
TINY_BODY(43)
TINY_BODY(44)
TINY_BODY(45)
TINY_BODY(46)
TINY_BODY(47)
TINY_BODY(48)
TINY_BODY(49)
TINY_BODY(50)
TINY_BODY(51)
TINY_BODY(52)
TINY_BODY(53)
TINY_BODY(54)
TINY_BODY(55)
TINY_BODY(56)
TINY_BODY(57)
TINY_BODY(58)
TINY_BODY(59)
TINY_BODY(60)
TINY_BODY(61)
TINY_BODY(62)
TINY_BODY(63)

typedef uint64_t (*tiny_fn)(uint64_t);

static tiny_fn tiny_functions[] = {
    tiny_0,  tiny_1,  tiny_2,  tiny_3,  tiny_4,  tiny_5,  tiny_6,  tiny_7,
    tiny_8,  tiny_9,  tiny_10, tiny_11, tiny_12, tiny_13, tiny_14, tiny_15,
    tiny_16, tiny_17, tiny_18, tiny_19, tiny_20, tiny_21, tiny_22, tiny_23,
    tiny_24, tiny_25, tiny_26, tiny_27, tiny_28, tiny_29, tiny_30, tiny_31,
    tiny_32, tiny_33, tiny_34, tiny_35, tiny_36, tiny_37, tiny_38, tiny_39,
    tiny_40, tiny_41, tiny_42, tiny_43, tiny_44, tiny_45, tiny_46, tiny_47,
    tiny_48, tiny_49, tiny_50, tiny_51, tiny_52, tiny_53, tiny_54, tiny_55,
    tiny_56, tiny_57, tiny_58, tiny_59, tiny_60, tiny_61, tiny_62, tiny_63};

struct fanout_ctx {
  tiny_fn *functions;
  uint64_t mask;
};

static uint64_t kernel_indirect_fanout(void *raw, uint64_t iters) {
  struct fanout_ctx *ctx = (struct fanout_ctx *)raw;
  uint64_t x = 1;
  for (uint64_t i = 0; i < iters; i++) {
    x = ctx->functions[(i * 17U) & ctx->mask](x);
  }
  return x;
}

static void run_instruction_footprint(const struct options *opts) {
  static const uint64_t full_counts[] = {1, 2, 4, 8, 16, 32, 64};
  static const uint64_t smoke_counts[] = {1, 8, 64};
  const uint64_t *counts = opts->smoke ? smoke_counts : full_counts;
  size_t count_len = opts->smoke ? sizeof(smoke_counts) / sizeof(smoke_counts[0])
                                 : sizeof(full_counts) / sizeof(full_counts[0]);

  print_section("Instruction Footprint And Indirect Target Proxy", opts->csv);
  for (size_t i = 0; i < count_len; i++) {
    char variant[32];
    snprintf(variant, sizeof(variant), "%" PRIu64 "_tiny_functions", counts[i]);
    struct fanout_ctx ctx = {tiny_functions, counts[i] - 1U};
    struct sample_result r = measure_kernel(
        "frontend_icache", "indirect_fanout", variant,
        "64-byte aligned tiny functions; mixes I-cache, BTB, and indirect target prediction",
        kernel_indirect_fanout, &ctx, opts->iters, 1, 0, opts->samples);
    print_result(&r, opts->csv);
  }
}

struct random_mem_ctx {
  uint64_t *data;
  uint32_t *idx;
  uint64_t mask;
};

static struct random_mem_ctx make_random_mem(size_t bytes) {
  struct random_mem_ctx ctx;
  size_t count = (size_t)next_pow2_u64((uint64_t)(bytes / sizeof(uint64_t)));
  if (count < 1024U) {
    count = 1024U;
  }
  ctx.data = NULL;
  ctx.idx = NULL;
  ctx.mask = (uint64_t)(count - 1U);
  if (posix_memalign((void **)&ctx.data, 16384, count * sizeof(*ctx.data)) != 0 ||
      posix_memalign((void **)&ctx.idx, 16384, count * sizeof(*ctx.idx)) != 0) {
    fprintf(stderr, "failed to allocate random memory context\n");
    exit(2);
  }
  for (size_t i = 0; i < count; i++) {
    ctx.data[i] = (uint64_t)i * 11400714819323198485ULL;
  }
  fill_permutation(ctx.idx, count);
  return ctx;
}

static void free_random_mem(struct random_mem_ctx *ctx) {
  free(ctx->data);
  free(ctx->idx);
  ctx->data = NULL;
  ctx->idx = NULL;
  ctx->mask = 0;
}

static uint64_t kernel_random_load(void *raw, uint64_t iters) {
  struct random_mem_ctx *ctx = (struct random_mem_ctx *)raw;
  uint64_t sum = 0;
  for (uint64_t i = 0; i < iters; i++) {
    sum += ctx->data[ctx->idx[i & ctx->mask]];
  }
  return sum;
}

static uint64_t kernel_random_store(void *raw, uint64_t iters) {
  struct random_mem_ctx *ctx = (struct random_mem_ctx *)raw;
  uint64_t value = 0x123456789abcdef0ULL;
  for (uint64_t i = 0; i < iters; i++) {
    uint64_t pos = ctx->idx[i & ctx->mask];
    ctx->data[pos] = value + i;
  }
  return value + iters;
}

static uint64_t kernel_random_rmw(void *raw, uint64_t iters) {
  struct random_mem_ctx *ctx = (struct random_mem_ctx *)raw;
  uint64_t sum = 0;
  for (uint64_t i = 0; i < iters; i++) {
    uint64_t pos = ctx->idx[i & ctx->mask];
    uint64_t value = ctx->data[pos] + i;
    ctx->data[pos] = value;
    sum += value;
  }
  return sum;
}

static void run_random_load_store(const struct options *opts) {
  static const size_t full_sizes[] = {64U * 1024U, 128U * 1024U,
                                      256U * 1024U, 1024U * 1024U,
                                      4U * 1024U * 1024U,
                                      16U * 1024U * 1024U};
  static const size_t smoke_sizes[] = {64U * 1024U, 256U * 1024U,
                                       4U * 1024U * 1024U};
  const size_t *sizes = opts->smoke ? smoke_sizes : full_sizes;
  size_t count = opts->smoke ? sizeof(smoke_sizes) / sizeof(smoke_sizes[0])
                             : sizeof(full_sizes) / sizeof(full_sizes[0]);

  print_section("Randomized Load Store Streams", opts->csv);
  for (size_t i = 0; i < count; i++) {
    char variant[32];
    probe_format_bytes(variant, sizeof(variant), sizes[i]);
    struct random_mem_ctx ctx = make_random_mem(sizes[i]);
    struct sample_result load = measure_kernel(
        "memory_random", "random_load64", variant,
        "permuted 64-bit addresses", kernel_random_load, &ctx, opts->iters, 1,
        sizeof(uint64_t), opts->samples);
    struct sample_result store = measure_kernel(
        "memory_random", "random_store64", variant,
        "permuted 64-bit addresses", kernel_random_store, &ctx, opts->iters, 1,
        sizeof(uint64_t), opts->samples);
    struct sample_result rmw = measure_kernel(
        "memory_random", "random_rmw64", variant,
        "load plus store at permuted address", kernel_random_rmw, &ctx,
        opts->iters, 2, 2U * sizeof(uint64_t), opts->samples);
    print_result(&load, opts->csv);
    print_result(&store, opts->csv);
    print_result(&rmw, opts->csv);
    free_random_mem(&ctx);
  }
}

struct stream_ctx {
  uint8_t *src;
  uint8_t *dst;
  size_t bytes;
};

static struct stream_ctx make_stream_ctx(size_t bytes) {
  struct stream_ctx ctx;
  ctx.src = NULL;
  ctx.dst = NULL;
  ctx.bytes = bytes;
  if (posix_memalign((void **)&ctx.src, 16384, bytes) != 0 ||
      posix_memalign((void **)&ctx.dst, 16384, bytes) != 0) {
    fprintf(stderr, "failed to allocate stream context\n");
    exit(2);
  }
  fill_random_u8(ctx.src, bytes, 0xa5a5a5a5U);
  memset(ctx.dst, 0, bytes);
  return ctx;
}

static void free_stream_ctx(struct stream_ctx *ctx) {
  free(ctx->src);
  free(ctx->dst);
  ctx->src = NULL;
  ctx->dst = NULL;
}

static uint64_t kernel_stream_read(void *raw, uint64_t iters) {
  struct stream_ctx *ctx = (struct stream_ctx *)raw;
  uint64_t sum = 0;
  const uint64_t *src = (const uint64_t *)ctx->src;
  size_t count = ctx->bytes / sizeof(uint64_t);
  for (uint64_t r = 0; r < iters; r++) {
    for (size_t i = 0; i < count; i += 8U) {
      sum += src[i] + src[i + 1U] + src[i + 2U] + src[i + 3U];
      sum += src[i + 4U] + src[i + 5U] + src[i + 6U] + src[i + 7U];
    }
  }
  return sum;
}

static uint64_t kernel_stream_write(void *raw, uint64_t iters) {
  struct stream_ctx *ctx = (struct stream_ctx *)raw;
  uint64_t *dst = (uint64_t *)ctx->dst;
  size_t count = ctx->bytes / sizeof(uint64_t);
  uint64_t value = 0xfeedfacecafebeefULL;
  for (uint64_t r = 0; r < iters; r++) {
    for (size_t i = 0; i < count; i += 8U) {
      dst[i] = value + i;
      dst[i + 1U] = value + i + 1U;
      dst[i + 2U] = value + i + 2U;
      dst[i + 3U] = value + i + 3U;
      dst[i + 4U] = value + i + 4U;
      dst[i + 5U] = value + i + 5U;
      dst[i + 6U] = value + i + 6U;
      dst[i + 7U] = value + i + 7U;
    }
    value += 17U;
  }
  return value;
}

static uint64_t kernel_stream_copy(void *raw, uint64_t iters) {
  struct stream_ctx *ctx = (struct stream_ctx *)raw;
  uint64_t sum = 0;
  for (uint64_t r = 0; r < iters; r++) {
    memcpy(ctx->dst, ctx->src, ctx->bytes);
    sum += ctx->dst[(r * 4099U) % ctx->bytes];
  }
  return sum;
}

static void run_stream_bandwidth(const struct options *opts) {
  static const size_t full_sizes[] = {8U * 1024U * 1024U, 64U * 1024U * 1024U,
                                      256U * 1024U * 1024U};
  static const size_t smoke_sizes[] = {8U * 1024U * 1024U,
                                       64U * 1024U * 1024U};
  const size_t *sizes = opts->smoke ? smoke_sizes : full_sizes;
  size_t count = opts->smoke ? sizeof(smoke_sizes) / sizeof(smoke_sizes[0])
                             : sizeof(full_sizes) / sizeof(full_sizes[0]);

  print_section("Sequential Bandwidth", opts->csv);
  for (size_t i = 0; i < count; i++) {
    char variant[32];
    probe_format_bytes(variant, sizeof(variant), sizes[i]);
    struct stream_ctx ctx = make_stream_ctx(sizes[i]);
    uint64_t reps = opts->smoke ? 2U : 4U;
    struct sample_result read = measure_kernel(
        "memory_bandwidth", "stream_read", variant,
        "sequential uint64 read sum", kernel_stream_read, &ctx, reps,
        sizes[i] / sizeof(uint64_t), sizes[i], opts->samples);
    struct sample_result write = measure_kernel(
        "memory_bandwidth", "stream_write", variant,
        "sequential uint64 stores", kernel_stream_write, &ctx, reps,
        sizes[i] / sizeof(uint64_t), sizes[i], opts->samples);
    struct sample_result copy = measure_kernel(
        "memory_bandwidth", "memcpy", variant,
        "bytes count includes read plus write traffic", kernel_stream_copy, &ctx,
        reps, sizes[i], 2U * sizes[i], opts->samples);
    print_result(&read, opts->csv);
    print_result(&write, opts->csv);
    print_result(&copy, opts->csv);
    free_stream_ctx(&ctx);
  }
}

static void *make_pointer_ring(size_t nodes, size_t stride) {
  void *buffer = NULL;
  if ((nodes & (nodes - 1U)) != 0 || nodes == 0) {
    return NULL;
  }
  if (posix_memalign(&buffer, 16384, nodes * stride) != 0) {
    return NULL;
  }
  memset(buffer, 0, nodes * stride);
  size_t mask = nodes - 1U;
  for (size_t i = 0; i < nodes; i++) {
    size_t current = (i * 131071U) & mask;
    size_t next = ((i + 1U) * 131071U) & mask;
    void **slot = (void **)((char *)buffer + current * stride);
    *slot = (char *)buffer + next * stride;
  }
  return buffer;
}

struct mlp_ctx {
  void *rings[8];
  uint32_t streams;
};

static uintptr_t chase_mlp(struct mlp_ctx *ctx, uint64_t iters) {
  void *p0 = ctx->rings[0], *p1 = ctx->rings[1], *p2 = ctx->rings[2],
       *p3 = ctx->rings[3], *p4 = ctx->rings[4], *p5 = ctx->rings[5],
       *p6 = ctx->rings[6], *p7 = ctx->rings[7];
  for (uint64_t i = 0; i < iters; i++) {
    if (ctx->streams == 1) {
      __asm__ volatile("ldr %0, [%0]" : "+r"(p0) : : "memory");
    } else if (ctx->streams == 2) {
      __asm__ volatile("ldr %0, [%0]\n\t"
                       "ldr %1, [%1]"
                       : "+r"(p0), "+r"(p1)
                       :
                       : "memory");
    } else if (ctx->streams == 4) {
      __asm__ volatile("ldr %0, [%0]\n\t"
                       "ldr %1, [%1]\n\t"
                       "ldr %2, [%2]\n\t"
                       "ldr %3, [%3]"
                       : "+r"(p0), "+r"(p1), "+r"(p2), "+r"(p3)
                       :
                       : "memory");
    } else {
      __asm__ volatile("ldr %0, [%0]\n\t"
                       "ldr %1, [%1]\n\t"
                       "ldr %2, [%2]\n\t"
                       "ldr %3, [%3]\n\t"
                       "ldr %4, [%4]\n\t"
                       "ldr %5, [%5]\n\t"
                       "ldr %6, [%6]\n\t"
                       "ldr %7, [%7]"
                       : "+r"(p0), "+r"(p1), "+r"(p2), "+r"(p3), "+r"(p4),
                         "+r"(p5), "+r"(p6), "+r"(p7)
                       :
                       : "memory");
    }
  }
  return (uintptr_t)p0 ^ (uintptr_t)p1 ^ (uintptr_t)p2 ^ (uintptr_t)p3 ^
         (uintptr_t)p4 ^ (uintptr_t)p5 ^ (uintptr_t)p6 ^ (uintptr_t)p7;
}

static uint64_t kernel_mlp(void *raw, uint64_t iters) {
  struct mlp_ctx *ctx = (struct mlp_ctx *)raw;
  return chase_mlp(ctx, iters);
}

static void run_mlp_scaling(const struct options *opts) {
  static const uint32_t streams[] = {1, 2, 4, 8};
  print_section("Memory-Level Parallelism Scaling", opts->csv);
  for (size_t i = 0; i < sizeof(streams) / sizeof(streams[0]); i++) {
    struct mlp_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.streams = streams[i];
    for (uint32_t s = 0; s < streams[i]; s++) {
      ctx.rings[s] = make_pointer_ring(16384, 64);
      if (ctx.rings[s] == NULL) {
        fprintf(stderr, "failed to allocate MLP ring\n");
        exit(2);
      }
    }
    char variant[32];
    snprintf(variant, sizeof(variant), "%u_streams", streams[i]);
    uint64_t ops_per_iter = streams[i];
    struct sample_result r = measure_kernel(
        "memory_mlp", "pointer_chase", variant,
        "independent 1 MiB pointer rings", kernel_mlp, &ctx,
        opts->smoke ? 32768U : 131072U, ops_per_iter, 0, opts->samples);
    print_result(&r, opts->csv);
    for (uint32_t s = 0; s < streams[i]; s++) {
      free(ctx.rings[s]);
    }
  }
}

static uint64_t kernel_int_add8(void *raw, uint64_t iters) {
  (void)raw;
  uint64_t a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5, a5 = 6, a6 = 7, a7 = 8;
  for (uint64_t i = 0; i < iters; i++) {
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

struct scale_task {
  atomic_int *start;
  qos_class_t qos;
  uint64_t iters;
  uint64_t result;
};

static void *scale_main(void *raw) {
  struct scale_task *task = (struct scale_task *)raw;
  (void)pthread_set_qos_class_self_np(task->qos, 0);
  while (atomic_load_explicit(task->start, memory_order_acquire) == 0) {
  }
  task->result = kernel_int_add8(NULL, task->iters);
  return NULL;
}

static void run_sustained_thread_scaling(const struct options *opts) {
  static const int full_counts[] = {1, 2, 4, 8, 10, 14};
  static const int smoke_counts[] = {1, 4, 14};
  const int *counts = opts->smoke ? smoke_counts : full_counts;
  size_t count_len = opts->smoke ? sizeof(smoke_counts) / sizeof(smoke_counts[0])
                                 : sizeof(full_counts) / sizeof(full_counts[0]);
  qos_class_t qoses[] = {QOS_CLASS_USER_INTERACTIVE, QOS_CLASS_BACKGROUND};
  const char *qos_names[] = {"user_interactive", "background"};

  print_section("Sustained Thread Scaling By QoS", opts->csv);
  for (size_t q = 0; q < sizeof(qoses) / sizeof(qoses[0]); q++) {
    for (size_t c = 0; c < count_len; c++) {
      int n = counts[c];
      pthread_t *threads = (pthread_t *)calloc((size_t)n, sizeof(*threads));
      struct scale_task *tasks =
          (struct scale_task *)calloc((size_t)n, sizeof(*tasks));
      atomic_int start;
      atomic_init(&start, 0);
      if (threads == NULL || tasks == NULL) {
        fprintf(stderr, "failed to allocate scale tasks\n");
        exit(2);
      }
      uint64_t per_thread_iters = opts->smoke ? opts->iters : opts->iters * 4U;
      for (int i = 0; i < n; i++) {
        tasks[i].start = &start;
        tasks[i].qos = qoses[q];
        tasks[i].iters = per_thread_iters;
        int rc = pthread_create(&threads[i], NULL, scale_main, &tasks[i]);
        if (rc != 0) {
          fprintf(stderr, "pthread_create: %s\n", strerror(rc));
          exit(2);
        }
      }
      struct timespec wait_time = {0, 1000000L};
      nanosleep(&wait_time, NULL);
      uint64_t start_tick = probe_read_cntvct();
      atomic_store_explicit(&start, 1, memory_order_release);
      for (int i = 0; i < n; i++) {
        int rc = pthread_join(threads[i], NULL);
        if (rc != 0) {
          fprintf(stderr, "pthread_join: %s\n", strerror(rc));
          exit(2);
        }
        g_sink_u64 ^= tasks[i].result;
      }
      uint64_t elapsed = probe_read_cntvct() - start_tick;
      char variant[48];
      snprintf(variant, sizeof(variant), "%s_%d_threads", qos_names[q], n);
      struct sample_result r;
      r.section = "core_scaling";
      r.name = "thr_add8";
      r.variant = variant;
      r.note = "aggregate wall-time throughput";
      r.ops = (uint64_t)n * per_thread_iters * 8U;
      r.bytes = 0;
      r.ticks = (struct probe_stats){elapsed, elapsed, elapsed, (double)elapsed,
                                     0.0, 0.0};
      r.ns_per_op = ticks_to_ns(elapsed) / (double)r.ops;
      r.gib_per_s = 0.0;
      print_result(&r, opts->csv);
      free(threads);
      free(tasks);
    }
  }
}

int main(int argc, char **argv) {
  struct options opts;
  if (!parse_options(argc, argv, &opts)) {
    usage(argv[0]);
    return 2;
  }

  print_header(opts.csv);
  run_host_and_pmu(opts.csv);
  run_branch_predictability(&opts);
  run_ras_depth(&opts);
  run_instruction_footprint(&opts);
  run_random_load_store(&opts);
  run_stream_bandwidth(&opts);
  run_mlp_scaling(&opts);
  run_sustained_thread_scaling(&opts);

  if (!opts.csv) {
    printf("\nSinks: u64=%" PRIu64 " ptr=%" PRIuPTR "\n", g_sink_u64,
           g_sink_ptr);
  }
  return 0;
}
