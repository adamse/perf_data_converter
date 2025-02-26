// Copied from kernel sources. See COPYING for license details.

#ifndef PERF_DATA_CONVERTER_SRC_QUIPPER_KERNEL_PERF_INTERNALS_H_
#define PERF_DATA_CONVERTER_SRC_QUIPPER_KERNEL_PERF_INTERNALS_H_

#include <linux/limits.h>
#include <stddef.h>     // For NULL
#include <sys/types.h>  // For pid_t

#include "kernel/perf_event.h"

namespace quipper {
// The first 64 bits of the perf header, used as a perf data file ID tag.
const uint64_t kPerfMagic = 0x32454c4946524550LL;  // "PERFILE2" little-endian

// These data structures have been copied from the kernel. See files under
// tools/perf/util.

//
// From tools/perf/util/header.h
//

enum {
  HEADER_RESERVED = 0, /* always cleared */
  HEADER_FIRST_FEATURE = 1,
  HEADER_TRACING_DATA = 1,
  HEADER_BUILD_ID,

  HEADER_HOSTNAME,
  HEADER_OSRELEASE,
  HEADER_VERSION,
  HEADER_ARCH,
  HEADER_NRCPUS,
  HEADER_CPUDESC,
  HEADER_CPUID,
  HEADER_TOTAL_MEM,
  HEADER_CMDLINE,
  HEADER_EVENT_DESC,
  HEADER_CPU_TOPOLOGY,
  HEADER_NUMA_TOPOLOGY,
  HEADER_BRANCH_STACK,
  HEADER_PMU_MAPPINGS,
  HEADER_GROUP_DESC,
  HEADER_LAST_FEATURE,
  HEADER_FEAT_BITS = 256,
};

struct perf_file_section {
  u64 offset;
  u64 size;
};

struct perf_file_attr {
  struct perf_event_attr attr;
  struct perf_file_section ids;
};

#define BITS_PER_BYTE 8
#define DIV_ROUND_UP(n, d) (((n) + (d)-1) / (d))
#define BITS_TO_LONGS(nr) \
  DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))  

#define DECLARE_BITMAP(name, bits) \
  unsigned long name[BITS_TO_LONGS(bits)]  

struct perf_file_header {
  u64 magic;
  u64 size;
  u64 attr_size;
  struct perf_file_section attrs;
  struct perf_file_section data;
  struct perf_file_section event_types;
  DECLARE_BITMAP(adds_features, HEADER_FEAT_BITS);
};

#undef BITS_PER_BYTE
#undef DIV_ROUND_UP
#undef BITS_TO_LONGS
#undef DECLARE_BITMAP

struct perf_pipe_file_header {
  u64 magic;
  u64 size;
};

//
// From tools/perf/util/event.h
//

struct mmap_event {
  struct perf_event_header header;
  u32 pid, tid;
  u64 start;
  u64 len;
  u64 pgoff;
  char filename[PATH_MAX];
};

const u8 kMaxBuildIdSize = 20;

// It needs the "C" linkage to keep the compatibility with C unnamed
// struct/union fields.
extern "C" {
struct mmap2_event {
  struct perf_event_header header;
  u32 pid, tid;
  u64 start;
  u64 len;
  u64 pgoff;
  union {
    struct {
      u32 maj;
      u32 min;
      u64 ino;
      u64 ino_generation;
    };
    struct {
      u8 build_id_size;
      u8 __reserved1;
      u16 __reserved2;
      u8 build_id[kMaxBuildIdSize];
    };
  };
  u32 prot;
  u32 flags;
  char filename[PATH_MAX];
};
}

// The max size is 16 for comm name in Linux perf. However, to support comm name
// from Android simpleperf that is longer than 16, the max size is increased.
// We still cap the max size of comm name in thread_map_event at 16 characters.
const u16 kMaxCommSize = PATH_MAX;
const u16 kMaxThreadCommSize = 16;

struct comm_event {
  struct perf_event_header header;
  u32 pid, tid;
  char comm[kMaxCommSize];
};

struct namespaces_event {
  struct perf_event_header header;
  u32 pid, tid;
  u64 nr_namespaces;
  struct perf_ns_link_info link_info[];
};

struct cgroup_event {
  struct perf_event_header header;
  u64 id;
  char path[PATH_MAX];
};

struct fork_event {
  struct perf_event_header header;
  u32 pid, ppid;
  u32 tid, ptid;
  u64 time;
};

struct lost_event {
  struct perf_event_header header;
  u64 id;
  u64 lost;
};

struct lost_samples_event {
  struct perf_event_header header;
  u64 lost;
};

/*
 * PERF_FORMAT_ENABLED | PERF_FORMAT_RUNNING | PERF_FORMAT_ID
 */
struct read_event {
  struct perf_event_header header;
  u32 pid, tid;
  u64 value;
  u64 time_enabled;
  u64 time_running;
  u64 id;
};

struct throttle_event {
  struct perf_event_header header;
  u64 time;
  u64 id;
  u64 stream_id;
};

/*
// The below macro is not used in quipper. It is commented out for future
// reference.

#define PERF_SAMPLE_MASK                                                 \
    (PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME |       \
     PERF_SAMPLE_ADDR | PERF_SAMPLE_ID | PERF_SAMPLE_STREAM_ID | \
     PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_IDENTIFIER)
*/

/* perf sample has 16 bits size limit */
// #define PERF_SAMPLE_MAX_SIZE (1 << 16)

struct sample_event {
  struct perf_event_header header;
  u64 array[];
};

#if 0
// PERF_REGS_MAX is arch-dependent, so this is not a useful struct as-is.
struct regs_dump {
  u64 abi;
  u64 mask;
  u64 *regs;

  /* Cached values/mask filled by first register access. */
  u64 cache_regs[PERF_REGS_MAX];
  u64 cache_mask;
};
#endif

struct regs_dump {
  u64 abi;
  u64 regs[0];
};

struct stack_dump {
  u64  size;
  void *data;
  u64  *dyn_size;

  stack_dump() : size(0), data(nullptr), dyn_size(nullptr) {}
  ~stack_dump() { delete[] reinterpret_cast<char *>(data); ; delete dyn_size; }
};

struct sample_read_value {
  u64 value;
  u64 id;
};

struct sample_read {
  u64 time_enabled;
  u64 time_running;
  struct {
    struct {
      u64 nr;
      struct sample_read_value *values;
    } group;
    struct sample_read_value one;
  };
  sample_read() : one({}) {
    group.nr = 0;
    group.values = nullptr;
  }
  ~sample_read() { delete[] group.values; }
};

struct ip_callchain {
  u64 nr;
  u64 ips[0];
};

struct branch_flags {
  u64 mispred : 1;
  u64 predicted : 1;
  u64 in_tx : 1;
  u64 abort : 1;
  u64 cycles : 16;
  u64 type : 4;
  u64 spec : 2;
  u64 reserved : 38;
};

struct branch_entry {
  u64 from;
  u64 to;
  struct branch_flags flags;
};

struct branch_stack {
  u64 nr;
  u64 hw_idx;
  struct branch_entry entries[0];
};

union perf_sample_weight {
  u64 full;
  struct {
    u32 var1_dw;
    u16 var2_w;
    u16 var3_w;
  };
};

// All the possible fields of a perf sample.  This is not an actual data
// structure found in raw perf data, as each field may or may not be present
// in the data.
struct perf_sample {
  u64 ip;
  u32 pid, tid;
  u64 time;
  u64 addr;
  u64 id;
  u64 stream_id;
  u64 period;
  perf_sample_weight weight;
  u64 transaction;
  u32 cpu;
  u32 raw_size;
  u64 data_src;
  u32 flags;
  u16 insn_len;
  bool no_hw_idx; /* No hw_idx collected in branch_stack */
  void *raw_data;
  struct ip_callchain *callchain;
  struct branch_stack *branch_stack;
  struct regs_dump    *user_regs;
  struct stack_dump   user_stack;
  struct sample_read read;
  u64 physical_addr;
  u64 cgroup;
  u64 data_page_size;
  u64 code_page_size;

  perf_sample()
      : ip(0),
        pid(0),
        tid(0),
        time(0),
        addr(0),
        id(0),
        stream_id(0),
        period(0),
        weight(perf_sample_weight{}),
        transaction(0),
        cpu(0),
        raw_size(0),
        data_src(0),
        flags(0),
        insn_len(0),
        raw_data(nullptr),
        callchain(nullptr),
        branch_stack(nullptr),
        user_regs(nullptr),
        user_stack({}),
        read({}),
        physical_addr(0),
        cgroup(0),
        data_page_size(0),
        code_page_size(0) {}
  ~perf_sample() {
    delete[] callchain;
    delete[] branch_stack;
    delete[] user_regs;
    delete[] reinterpret_cast<char *>(raw_data);
  }
};

// If this is changed, kBuildIDArraySize in perf_reader.h must also be changed.
#define BUILD_ID_SIZE 20

// The flag marks buildid events with size.
#define PERF_RECORD_MISC_BUILD_ID_SIZE (1 << 15)

// Synced with perf_record_header_build_id at
// tools/lib/perf/include/perf/event.h.
struct build_id_event {
  struct perf_event_header header;
  pid_t pid;
  u8 build_id[BUILD_ID_SIZE];
  u8 size;
  // Padding for 8-byte alignment.
  u8 reserved1__;
  u16 reserved2__;
  char filename[];
};

#undef BUILD_ID_SIZE

enum perf_user_event_type {
  /* above any possible kernel type */
  PERF_RECORD_USER_TYPE_START = 64,
  PERF_RECORD_HEADER_ATTR = 64,
  PERF_RECORD_HEADER_EVENT_TYPE = 65, /* depreceated */
  PERF_RECORD_HEADER_TRACING_DATA = 66,
  PERF_RECORD_HEADER_BUILD_ID = 67,
  PERF_RECORD_FINISHED_ROUND = 68,
  PERF_RECORD_ID_INDEX = 69,
  PERF_RECORD_AUXTRACE_INFO = 70,
  PERF_RECORD_AUXTRACE = 71,
  PERF_RECORD_AUXTRACE_ERROR = 72,
  PERF_RECORD_THREAD_MAP = 73,
  PERF_RECORD_CPU_MAP = 74,
  PERF_RECORD_STAT_CONFIG = 75,
  PERF_RECORD_STAT = 76,
  PERF_RECORD_STAT_ROUND = 77,
  PERF_RECORD_EVENT_UPDATE = 78,
  PERF_RECORD_TIME_CONV = 79,
  PERF_RECORD_HEADER_FEATURE = 80,
  PERF_RECORD_HEADER_MAX = 81,
};

enum auxtrace_error_type {
  PERF_AUXTRACE_ERROR_ITRACE = 1,
  PERF_AUXTRACE_ERROR_MAX
};

struct attr_event {
  struct perf_event_header header;
  struct perf_event_attr attr;
  u64 id[];
};

#define MAX_EVENT_NAME 64

struct perf_trace_event_type {
  u64 event_id;
  char name[MAX_EVENT_NAME];
};

#undef MAX_EVENT_NAME

struct event_type_event {
  struct perf_event_header header;
  struct perf_trace_event_type event_type;
};

struct tracing_data_event {
  struct perf_event_header header;
  u32 size;
};

struct auxtrace_info_event {
  struct perf_event_header header;
  u32 type;
  u32 reserved__; /* For alignment */
  u64 priv[];
};

struct auxtrace_event {
  struct perf_event_header header;
  u64 size;
  u64 offset;
  u64 reference;
  u32 idx;
  u32 tid;
  u32 cpu;
  u32 reserved__; /* For alignment */
};

const u16 MAX_AUXTRACE_ERROR_MSG = 64;

struct auxtrace_error_event {
  struct perf_event_header header;
  u32 type;
  u32 code;
  u32 cpu;
  u32 pid;
  u32 tid;
  u32 reserved__; /* For alignment */
  u64 ip;
  char msg[MAX_AUXTRACE_ERROR_MSG];
};

struct aux_event {
  struct perf_event_header header;
  u64 aux_offset;
  u64 aux_size;
  u64 flags;
};

struct itrace_start_event {
  struct perf_event_header header;
  u32 pid, tid;
};

struct context_switch_event {
  struct perf_event_header header;
  u32 next_prev_pid;
  u32 next_prev_tid;
};

struct thread_map_event_entry {
  u64 pid;
  char comm[kMaxThreadCommSize];
};

struct thread_map_event {
  struct perf_event_header header;
  u64 nr;
  struct thread_map_event_entry entries[];
};

enum aggr_mode {
  AGGR_NONE,
  AGGR_GLOBAL,
  AGGR_SOCKET,
  AGGR_CORE,
  AGGR_THREAD,
  AGGR_UNSET,
  AGGR_MAX,
};

// Enum values correspond to fields in perf's perf_stat_config struct.
enum {
  PERF_STAT_CONFIG_TERM__AGGR_MODE = 0,
  PERF_STAT_CONFIG_TERM__INTERVAL = 1,
  PERF_STAT_CONFIG_TERM__SCALE = 2,
  PERF_STAT_CONFIG_TERM__MAX = 3,
};

struct stat_config_event_entry {
  u64 tag;
  u64 val;
};

struct stat_config_event {
  struct perf_event_header header;
  u64 nr;
  struct stat_config_event_entry data[];
};

struct stat_event {
  struct perf_event_header header;

  u64 id;
  u32 cpu;
  u32 thread;

  union {
    struct {
      u64 val;
      u64 ena;
      u64 run;
    };
    u64 values[3];
  };
};

enum {
  PERF_STAT_ROUND_TYPE__INTERVAL = 0,
  PERF_STAT_ROUND_TYPE__FINAL = 1,
};

struct stat_round_event {
  struct perf_event_header header;
  u64 type;
  u64 time;
};

struct time_conv_event {
  struct perf_event_header header;
  u64 time_shift;
  u64 time_mult;
  u64 time_zero;
  // New members introduced in kernel 5.10
  u64 time_cycles;
  u64 time_mask;
  u8 cap_user_time_zero;
  u8 cap_user_time_short;
  u8 reserved[6];
};

struct feature_event {
  struct perf_event_header header;
  u64 feat_id;
  char data[];
};

union perf_event {
  struct perf_event_header header;
  struct mmap_event mmap;
  struct mmap2_event mmap2;
  struct comm_event comm;
  struct namespaces_event namespaces;
  struct cgroup_event cgroup;
  struct fork_event fork;
  struct lost_event lost;
  struct lost_samples_event lost_samples;
  struct read_event read;
  struct throttle_event throttle;
  struct sample_event sample;
  struct attr_event attr;
  struct event_type_event event_type;
  struct tracing_data_event tracing_data;
  struct build_id_event build_id;
  struct auxtrace_info_event auxtrace_info;
  struct auxtrace_event auxtrace;
  struct auxtrace_error_event auxtrace_error;
  struct aux_event aux;
  struct itrace_start_event itrace_start;
  struct context_switch_event context_switch;
  struct thread_map_event thread_map;
  struct stat_config_event stat_config;
  struct stat_event stat;
  struct stat_round_event stat_round;
  struct time_conv_event time_conv;
  struct feature_event feat;
};

typedef perf_event event_t;

}  // namespace quipper

#endif /*PERF_DATA_CONVERTER_SRC_QUIPPER_KERNEL_PERF_INTERNALS_H_*/
