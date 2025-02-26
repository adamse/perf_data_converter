// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "perf_serializer.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>

#include <algorithm>  // for std::copy

#include "base/logging.h"
#include "binary_data_utils.h"
#include "compat/proto.h"
#include "kernel/perf_event.h"
#include "perf_buildid.h"
#include "perf_data_structures.h"
#include "perf_data_utils.h"
#include "perf_parser.h"
#include "perf_reader.h"
#include "string_utils.h"

namespace quipper {

namespace {

constexpr int kHexCharsPerByte = 2;

}  // namespace

PerfSerializer::PerfSerializer() {}

PerfSerializer::~PerfSerializer() {}

bool PerfSerializer::IsSupportedKernelEventType(uint32_t type) {
  switch (type) {
    case PERF_RECORD_MMAP:
    case PERF_RECORD_LOST:
    case PERF_RECORD_COMM:
    case PERF_RECORD_EXIT:
    case PERF_RECORD_THROTTLE:
    case PERF_RECORD_UNTHROTTLE:
    case PERF_RECORD_FORK:
    case PERF_RECORD_SAMPLE:
    case PERF_RECORD_MMAP2:
    case PERF_RECORD_AUX:
    case PERF_RECORD_ITRACE_START:
    case PERF_RECORD_LOST_SAMPLES:
    case PERF_RECORD_SWITCH:
    case PERF_RECORD_SWITCH_CPU_WIDE:
    case PERF_RECORD_NAMESPACES:
    case PERF_RECORD_CGROUP:
      return true;
  }
  return false;
}

bool PerfSerializer::IsSupportedUserEventType(uint32_t type) {
  switch (type) {
    case PERF_RECORD_FINISHED_ROUND:
    case PERF_RECORD_AUXTRACE_INFO:
    case PERF_RECORD_AUXTRACE:
    case PERF_RECORD_AUXTRACE_ERROR:
    case PERF_RECORD_THREAD_MAP:
    case PERF_RECORD_STAT_CONFIG:
    case PERF_RECORD_STAT:
    case PERF_RECORD_STAT_ROUND:
    case PERF_RECORD_TIME_CONV:
      return true;
  }
  return false;
}

bool PerfSerializer::IsSupportedHeaderEventType(uint32_t type) {
  switch (type) {
    case PERF_RECORD_HEADER_ATTR:
    case PERF_RECORD_HEADER_EVENT_TYPE:
    case PERF_RECORD_HEADER_TRACING_DATA:
    case PERF_RECORD_HEADER_BUILD_ID:
    case PERF_RECORD_HEADER_FEATURE:
      return true;
  }
  return false;
}

bool PerfSerializer::ContainsSampleInfo(uint32_t type) const {
  if (type == PERF_RECORD_SAMPLE) {
    return true;
  }
  if (!SampleInfoReader::IsSupportedEventType(type)) {
    return false;
  }
  // Do non-PERF_RECORD_SAMPLE events have a sample_id? Reflects the value of
  // sample_id_all in the first attr, which should be consistent across all
  // attrs.
  return !sample_info_reader_map_.empty() &&
         sample_info_reader_map_.begin()->second->event_attr().sample_id_all;
}

size_t PerfSerializer::GetEventSize(
    const PerfDataProto_PerfEvent& event) const {
  u32 type = event.header().type();
  size_t event_data_size = GetEventDataSize(event);
  if (event_data_size == 0) {
    LOG(ERROR) << "Couldn't get event data size for event "
               << GetEventName(type);
    return 0L;
  }

  if (!ContainsSampleInfo(type)) {
    // No sample info data available for the event so return the event data
    // size.
    return event_data_size;
  }

  perf_sample sample_info = {};
  if (event.header().type() == PERF_RECORD_SAMPLE) {
    GetPerfSampleInfo(event.sample_event(), &sample_info);
  } else {
    const PerfDataProto_SampleInfo* sample_info_proto =
        GetSampleInfoForEvent(event);
    if (sample_info_proto == nullptr) {
      LOG(ERROR) << "Sample info not available for event: "
                 << GetEventName(event.header().type());
      return 0L;
    }
    GetPerfSampleInfo(*sample_info_proto, &sample_info);
  }

  const SampleInfoReader* reader = GetSampleInfoReaderForId(sample_info.id);
  if (reader == nullptr) {
    LOG(ERROR) << "No sample info reader available for event "
               << GetEventName(event.header().type()) << ", sample id "
               << sample_info.id << ", from proto "
               << GetSampleIdFromPerfEvent(event);
    return 0L;
  }

  return event_data_size +
         reader->GetPerfSampleDataSize(sample_info, event.header().type());
}

bool PerfSerializer::SerializePerfFileAttr(
    const PerfFileAttr& perf_file_attr,
    PerfDataProto_PerfFileAttr* perf_file_attr_proto) const {
  if (!SerializePerfEventAttr(perf_file_attr.attr,
                              perf_file_attr_proto->mutable_attr())) {
    return false;
  }

  for (size_t i = 0; i < perf_file_attr.ids.size(); i++)
    perf_file_attr_proto->add_ids(perf_file_attr.ids[i]);
  return true;
}

bool PerfSerializer::DeserializePerfFileAttr(
    const PerfDataProto_PerfFileAttr& perf_file_attr_proto,
    PerfFileAttr* perf_file_attr) const {
  if (!DeserializePerfEventAttr(perf_file_attr_proto.attr(),
                                &perf_file_attr->attr)) {
    return false;
  }

  for (int i = 0; i < perf_file_attr_proto.ids_size(); i++)
    perf_file_attr->ids.push_back(perf_file_attr_proto.ids(i));
  return true;
}

bool PerfSerializer::SerializePerfEventAttr(
    const perf_event_attr& perf_event_attr,
    PerfDataProto_PerfEventAttr* perf_event_attr_proto) const {
#define S(x) perf_event_attr_proto->set_##x(perf_event_attr.x)
  S(type);
  S(size);
  S(config);
  if (perf_event_attr_proto->freq())
    S(sample_freq);
  else
    S(sample_period);
  S(sample_type);
  S(read_format);
  S(disabled);
  S(inherit);
  S(pinned);
  S(exclusive);
  S(exclude_user);
  S(exclude_kernel);
  S(exclude_hv);
  S(exclude_idle);
  S(mmap);
  S(comm);
  S(freq);
  S(inherit_stat);
  S(enable_on_exec);
  S(task);
  S(watermark);
  S(precise_ip);
  S(mmap_data);
  S(sample_id_all);
  S(exclude_host);
  S(exclude_guest);
  S(exclude_callchain_kernel);
  S(exclude_callchain_user);
  S(mmap2);
  S(comm_exec);
  S(use_clockid);
  S(context_switch);
  S(write_backward);
  S(namespaces);
  S(cgroup);
  if (perf_event_attr_proto->watermark())
    S(wakeup_watermark);
  else
    S(wakeup_events);
  S(bp_type);
  S(bp_addr);
  S(bp_len);
  S(branch_sample_type);
  S(sample_regs_user);
  S(sample_stack_user);
#undef S
  return true;
}

bool PerfSerializer::DeserializePerfEventAttr(
    const PerfDataProto_PerfEventAttr& perf_event_attr_proto,
    perf_event_attr* perf_event_attr) const {
  memset(perf_event_attr, 0, sizeof(*perf_event_attr));
#define S(x) perf_event_attr->x = perf_event_attr_proto.x()
  S(type);
  S(size);
  S(config);
  if (perf_event_attr->freq)
    S(sample_freq);
  else
    S(sample_period);
  S(sample_type);
  S(read_format);
  S(disabled);
  S(inherit);
  S(pinned);
  S(exclusive);
  S(exclude_user);
  S(exclude_kernel);
  S(exclude_hv);
  S(exclude_idle);
  S(mmap);
  S(comm);
  S(freq);
  S(inherit_stat);
  S(enable_on_exec);
  S(task);
  S(watermark);
  S(precise_ip);
  S(mmap_data);
  S(sample_id_all);
  S(exclude_host);
  S(exclude_guest);
  S(exclude_callchain_kernel);
  S(exclude_callchain_user);
  S(mmap2);
  S(comm_exec);
  S(use_clockid);
  S(context_switch);
  S(write_backward);
  S(namespaces);
  S(cgroup);
  if (perf_event_attr->watermark)
    S(wakeup_watermark);
  else
    S(wakeup_events);
  S(bp_type);
  S(bp_addr);
  S(bp_len);
  S(branch_sample_type);
  S(sample_regs_user);
  S(sample_stack_user);
#undef S
  return true;
}

bool PerfSerializer::SerializePerfEventType(
    const PerfFileAttr& event_attr,
    quipper::PerfDataProto_PerfEventType* event_type_proto) const {
  event_type_proto->set_id(event_attr.attr.config);
  event_type_proto->set_name(event_attr.name);
  event_type_proto->set_name_md5_prefix(Md5Prefix(event_attr.name));
  return true;
}

bool PerfSerializer::DeserializePerfEventType(
    const quipper::PerfDataProto_PerfEventType& event_type_proto,
    PerfFileAttr* event_attr) const {
  // Attr should have already been deserialized.
  if (event_attr->attr.config != event_type_proto.id()) {
    LOG(ERROR) << "Event type ID " << event_type_proto.id()
               << " does not match attr.config " << event_attr->attr.config
               << ". Not deserializing event name!";
    return false;
  }
  event_attr->name = event_type_proto.name();
  return true;
}

bool PerfSerializer::SerializeEvent(
    const malloced_unique_ptr<event_t>& event_ptr,
    PerfDataProto_PerfEvent* event_proto) const {
  const event_t& event = *event_ptr;

  if (!SerializeEventHeader(event.header, event_proto->mutable_header()))
    return false;

  if (event.header.type >= PERF_RECORD_USER_TYPE_START) {
    if (!SerializeUserEvent(event, event_proto)) {
      return false;
    }
  } else if (!SerializeKernelEvent(event, event_proto)) {
    return false;
  }

  event_proto->set_timestamp(GetTimeFromPerfEvent(*event_proto));
  return true;
}

bool PerfSerializer::SerializeKernelEvent(
    const event_t& event, PerfDataProto_PerfEvent* event_proto) const {
  switch (event.header.type) {
    case PERF_RECORD_SAMPLE:
      return SerializeSampleEvent(event, event_proto->mutable_sample_event());
    case PERF_RECORD_MMAP:
      return SerializeMMapEvent(event, event_proto->mutable_mmap_event());
    case PERF_RECORD_MMAP2:
      return SerializeMMap2Event(event, event_proto->mutable_mmap_event());
    case PERF_RECORD_COMM:
      return SerializeCommEvent(event, event_proto->mutable_comm_event());
    case PERF_RECORD_EXIT:
      return SerializeForkExitEvent(event, event_proto->mutable_exit_event());
    case PERF_RECORD_FORK:
      return SerializeForkExitEvent(event, event_proto->mutable_fork_event());
    case PERF_RECORD_LOST:
      return SerializeLostEvent(event, event_proto->mutable_lost_event());
    case PERF_RECORD_THROTTLE:
    case PERF_RECORD_UNTHROTTLE:
      return SerializeThrottleEvent(event,
                                    event_proto->mutable_throttle_event());
    case PERF_RECORD_AUX:
      return SerializeAuxEvent(event, event_proto->mutable_aux_event());
    case PERF_RECORD_ITRACE_START:
      return SerializeItraceStartEvent(
          event, event_proto->mutable_itrace_start_event());
    case PERF_RECORD_LOST_SAMPLES:
      return SerializeLostSamplesEvent(
          event, event_proto->mutable_lost_samples_event());
    case PERF_RECORD_SWITCH:
    case PERF_RECORD_SWITCH_CPU_WIDE:
      return SerializeContextSwitchEvent(
          event, event_proto->mutable_context_switch_event());
    case PERF_RECORD_NAMESPACES:
      return SerializeNamespacesEvent(event,
                                      event_proto->mutable_namespaces_event());
    case PERF_RECORD_CGROUP:
      return SerializeCgroupEvent(event, event_proto->mutable_cgroup_event());
    default:
      LOG(ERROR) << "Unsupported event " << GetEventName(event.header.type);
  }
  return true;
}

bool PerfSerializer::SerializeUserEvent(
    const event_t& event, PerfDataProto_PerfEvent* event_proto) const {
  switch (event.header.type) {
    case PERF_RECORD_FINISHED_ROUND:
      // A PERF_RECORD_FINISHED_ROUND event contains only header data so skip
      // serializing event data.
      return true;
    case PERF_RECORD_AUXTRACE_INFO:
      return SerializeAuxtraceInfoEvent(
          event, event_proto->mutable_auxtrace_info_event());
    case PERF_RECORD_AUXTRACE:
      return SerializeAuxtraceEvent(event,
                                    event_proto->mutable_auxtrace_event());
    case PERF_RECORD_AUXTRACE_ERROR:
      return SerializeAuxtraceErrorEvent(
          event, event_proto->mutable_auxtrace_error_event());
    case PERF_RECORD_THREAD_MAP:
      return SerializeThreadMapEvent(event,
                                     event_proto->mutable_thread_map_event());
    case PERF_RECORD_STAT_CONFIG:
      return SerializeStatConfigEvent(event,
                                      event_proto->mutable_stat_config_event());
    case PERF_RECORD_STAT:
      return SerializeStatEvent(event, event_proto->mutable_stat_event());
    case PERF_RECORD_STAT_ROUND:
      return SerializeStatRoundEvent(event,
                                     event_proto->mutable_stat_round_event());
    case PERF_RECORD_TIME_CONV:
      return SerializeTimeConvEvent(event,
                                    event_proto->mutable_time_conv_event());
    default:
      LOG(ERROR) << "Unsupported event " << GetEventName(event.header.type);
  }
  return true;
}

bool PerfSerializer::DeserializeEvent(
    const PerfDataProto_PerfEvent& event_proto,
    malloced_unique_ptr<event_t>* event_ptr) const {
  event_ptr->reset(CallocMemoryForEvent(event_proto.header().size()));
  event_t* event = event_ptr->get();

  if (!DeserializeEventHeader(event_proto.header(), &event->header))
    return false;

  bool event_deserialized = false;
  if (event_proto.header().type() >= PERF_RECORD_USER_TYPE_START) {
    event_deserialized = DeserializeUserEvent(event_proto, event);
  } else {
    event_deserialized = DeserializeKernelEvent(event_proto, event);
  }

  if (!event_deserialized) {
    LOG(ERROR) << "Could not deserialize event "
               << GetEventName(event_proto.header().type());
    return false;
  }

  return true;
}

bool PerfSerializer::DeserializeKernelEvent(
    const PerfDataProto_PerfEvent& event_proto, event_t* event) const {
  switch (event_proto.header().type()) {
    case PERF_RECORD_SAMPLE:
      return DeserializeSampleEvent(event_proto.sample_event(), event);
    case PERF_RECORD_MMAP:
      return DeserializeMMapEvent(event_proto.mmap_event(), event);
    case PERF_RECORD_MMAP2:
      return DeserializeMMap2Event(event_proto.mmap_event(), event);
    case PERF_RECORD_COMM: {
      // Sometimes the command string will be modified.  e.g. if the original
      // comm string is not recoverable from the Md5sum prefix, then use the
      // latter as a replacement comm string.  However, if the original was
      // < 8 bytes (fit into |sizeof(uint64_t)|), then the size is no longer
      // correct. This section checks for the size difference and updates the
      // size in the header.
      size_t size = GetEventSize(event_proto);
      if (size == 0) {
        LOG(ERROR) << "Couldn't get event size for PERF_RECORD_COMM event";
        return false;
      }
      event->header.size = size;
      return DeserializeCommEvent(event_proto.comm_event(), event);
    }
    case PERF_RECORD_EXIT:
      return (event_proto.has_exit_event() &&
              DeserializeForkExitEvent(event_proto.exit_event(), event)) ||
             (event_proto.has_fork_event() &&
              DeserializeForkExitEvent(event_proto.fork_event(), event));
    // Some older protobufs use the |fork_event| field to store exit
    // events.
    case PERF_RECORD_FORK:
      return DeserializeForkExitEvent(event_proto.fork_event(), event);
    case PERF_RECORD_LOST:
      return DeserializeLostEvent(event_proto.lost_event(), event);
    case PERF_RECORD_THROTTLE:
    case PERF_RECORD_UNTHROTTLE:
      return DeserializeThrottleEvent(event_proto.throttle_event(), event);
    case PERF_RECORD_AUX:
      return DeserializeAuxEvent(event_proto.aux_event(), event);
    case PERF_RECORD_ITRACE_START:
      return DeserializeItraceStartEvent(event_proto.itrace_start_event(),
                                         event);
    case PERF_RECORD_LOST_SAMPLES:
      return DeserializeLostSamplesEvent(event_proto.lost_samples_event(),
                                         event);
    case PERF_RECORD_SWITCH:
    case PERF_RECORD_SWITCH_CPU_WIDE:
      return DeserializeContextSwitchEvent(event_proto.context_switch_event(),
                                           event);
    case PERF_RECORD_NAMESPACES:
      return DeserializeNamespacesEvent(event_proto.namespaces_event(), event);
    case PERF_RECORD_CGROUP:
      return DeserializeCgroupEvent(event_proto.cgroup_event(), event);
      break;
  }
  return false;
}

bool PerfSerializer::DeserializeUserEvent(
    const PerfDataProto_PerfEvent& event_proto, event_t* event) const {
  switch (event_proto.header().type()) {
    case PERF_RECORD_AUXTRACE_INFO:
      return DeserializeAuxtraceInfoEvent(event_proto.auxtrace_info_event(),
                                          event);
    case PERF_RECORD_AUXTRACE:
      return DeserializeAuxtraceEvent(event_proto.auxtrace_event(), event);
    case PERF_RECORD_AUXTRACE_ERROR:
      return DeserializeAuxtraceErrorEvent(event_proto.auxtrace_error_event(),
                                           event);
    case PERF_RECORD_THREAD_MAP:
      return DeserializeThreadMapEvent(event_proto.thread_map_event(), event);
    case PERF_RECORD_STAT_CONFIG:
      return DeserializeStatConfigEvent(event_proto.stat_config_event(), event);
    case PERF_RECORD_STAT:
      return DeserializeStatEvent(event_proto.stat_event(), event);
    case PERF_RECORD_STAT_ROUND:
      return DeserializeStatRoundEvent(event_proto.stat_round_event(), event);
    case PERF_RECORD_TIME_CONV:
      return DeserializeTimeConvEvent(event_proto.time_conv_event(), event);
    default:
      // User type events are marked as deserialized because they don't
      // have non-header data in perf.data proto.
      if (event_proto.header().type() >= PERF_RECORD_HEADER_MAX) {
        return false;
      }
  }
  return true;
}

bool PerfSerializer::SerializeEventHeader(
    const perf_event_header& header,
    PerfDataProto_EventHeader* header_proto) const {
  header_proto->set_type(header.type);
  header_proto->set_misc(header.misc);
  header_proto->set_size(header.size);
  return true;
}

bool PerfSerializer::DeserializeEventHeader(
    const PerfDataProto_EventHeader& header_proto,
    perf_event_header* header) const {
  header->type = header_proto.type();
  header->misc = header_proto.misc();
  header->size = header_proto.size();
  return true;
}

bool PerfSerializer::SerializeSampleEvent(
    const event_t& event, PerfDataProto_SampleEvent* sample) const {
  perf_sample sample_info;
  uint64_t sample_type = 0;
  if (!ReadPerfSampleInfoAndType(event, &sample_info, &sample_type))
    return false;

  if (sample_type & PERF_SAMPLE_IP) sample->set_ip(sample_info.ip);
  if (sample_type & PERF_SAMPLE_TID) {
    sample->set_pid(sample_info.pid);
    sample->set_tid(sample_info.tid);
  }
  if (sample_type & PERF_SAMPLE_TIME)
    sample->set_sample_time_ns(sample_info.time);
  if (sample_type & PERF_SAMPLE_ADDR) sample->set_addr(sample_info.addr);
  if ((sample_type & PERF_SAMPLE_ID) || (sample_type & PERF_SAMPLE_IDENTIFIER))
    sample->set_id(sample_info.id);
  if (sample_type & PERF_SAMPLE_STREAM_ID)
    sample->set_stream_id(sample_info.stream_id);
  if (sample_type & PERF_SAMPLE_CPU) sample->set_cpu(sample_info.cpu);
  if (sample_type & PERF_SAMPLE_PERIOD) sample->set_period(sample_info.period);
  if (sample_type & PERF_SAMPLE_RAW) {
    // See raw and raw_size comments in perf_data.proto
    sample->set_raw(sample_info.raw_data, sample_info.raw_size);
    sample->set_raw_size(sample_info.raw_size);
  }
  if (sample_type & PERF_SAMPLE_READ) {
    const SampleInfoReader* reader = GetSampleInfoReaderForEvent(event);
    if (reader) {
      PerfDataProto_ReadInfo* read_info = sample->mutable_read_info();
      if (reader->event_attr().read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
        read_info->set_time_enabled(sample_info.read.time_enabled);
      if (reader->event_attr().read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
        read_info->set_time_running(sample_info.read.time_running);
      if (reader->event_attr().read_format & PERF_FORMAT_GROUP) {
        for (size_t i = 0; i < sample_info.read.group.nr; i++) {
          auto read_value = read_info->add_read_value();
          read_value->set_value(sample_info.read.group.values[i].value);
          if (reader->event_attr().read_format & PERF_FORMAT_ID)
            read_value->set_id(sample_info.read.group.values[i].id);
        }
      } else {
        auto read_value = read_info->add_read_value();
        read_value->set_value(sample_info.read.one.value);
        if (reader->event_attr().read_format & PERF_FORMAT_ID)
          read_value->set_id(sample_info.read.one.id);
      }
    }
  }
  if (sample_type & PERF_SAMPLE_CALLCHAIN) {
    sample->mutable_callchain()->Reserve(sample_info.callchain->nr);
    for (size_t i = 0; i < sample_info.callchain->nr; ++i)
      sample->add_callchain(sample_info.callchain->ips[i]);
  }
  if (sample_type & PERF_SAMPLE_BRANCH_STACK) {
    sample->set_no_hw_idx(sample_info.no_hw_idx);
    sample->set_branch_stack_hw_idx(sample_info.branch_stack->hw_idx);
    for (size_t i = 0; i < sample_info.branch_stack->nr; ++i) {
      sample->add_branch_stack();
      const struct branch_entry& entry = sample_info.branch_stack->entries[i];
      sample->mutable_branch_stack(i)->set_from_ip(entry.from);
      sample->mutable_branch_stack(i)->set_to_ip(entry.to);
      sample->mutable_branch_stack(i)->set_mispredicted(entry.flags.mispred);
      // Support for mispred, predicted is optional. In case it
      // is not supported mispred = predicted = 0. So, set predicted
      // separately.
      sample->mutable_branch_stack(i)->set_predicted(entry.flags.predicted);
      sample->mutable_branch_stack(i)->set_in_transaction(entry.flags.in_tx);
      sample->mutable_branch_stack(i)->set_abort(entry.flags.abort);
      sample->mutable_branch_stack(i)->set_cycles(entry.flags.cycles);
      sample->mutable_branch_stack(i)->set_type(entry.flags.type);
      sample->mutable_branch_stack(i)->set_spec(entry.flags.spec);
      if (entry.flags.reserved != 0) {
        LOG(WARNING) << "Ignoring branch stack entry reserved bits: "
                     << entry.flags.reserved;
      }
    }
  }

  if (sample_type & PERF_SAMPLE_WEIGHT)
    sample->set_weight(sample_info.weight.full);
  if (sample_type & PERF_SAMPLE_DATA_SRC)
    sample->set_data_src(sample_info.data_src);
  if (sample_type & PERF_SAMPLE_TRANSACTION)
    sample->set_transaction(sample_info.transaction);
  if (sample_type & PERF_SAMPLE_PHYS_ADDR)
    sample->set_physical_addr(sample_info.physical_addr);
  if (sample_type & PERF_SAMPLE_CGROUP) sample->set_cgroup(sample_info.cgroup);
  if (sample_type & PERF_SAMPLE_DATA_PAGE_SIZE)
    sample->set_data_page_size(sample_info.data_page_size);
  if (sample_type & PERF_SAMPLE_CODE_PAGE_SIZE)
    sample->set_code_page_size(sample_info.code_page_size);
  if (sample_type & PERF_SAMPLE_WEIGHT_STRUCT) {
    sample->mutable_weight_struct()->set_var1_dw(sample_info.weight.var1_dw);
    sample->mutable_weight_struct()->set_var2_w(sample_info.weight.var2_w);
    sample->mutable_weight_struct()->set_var3_w(sample_info.weight.var3_w);
  }

  return true;
}

bool PerfSerializer::DeserializeSampleEvent(
    const PerfDataProto_SampleEvent& sample, event_t* event) const {
  perf_sample sample_info = {};
  GetPerfSampleInfo(sample, &sample_info);

  const SampleInfoReader* writer = GetSampleInfoReaderForId(sample_info.id);
  CHECK(writer) << " failed for event " << event->header.type
                << ", sample id: " << sample_info.id;

  return writer->WritePerfSampleInfo(sample_info, event);
}

bool PerfSerializer::SerializeMMapEvent(const event_t& event,
                                        PerfDataProto_MMapEvent* sample) const {
  const struct mmap_event& mmap = event.mmap;
  sample->set_pid(mmap.pid);
  sample->set_tid(mmap.tid);
  sample->set_start(mmap.start);
  sample->set_len(mmap.len);
  sample->set_pgoff(mmap.pgoff);
  sample->set_filename(mmap.filename);
  sample->set_filename_md5_prefix(Md5Prefix(mmap.filename));
  std::string root_path = RootPath(mmap.filename);
  if (!root_path.empty()) {
    sample->set_root_path(root_path);
  }
  sample->set_root_path_md5_prefix(Md5Prefix(root_path));

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeMMapEvent(const PerfDataProto_MMapEvent& sample,
                                          event_t* event) const {
  struct mmap_event& mmap = event->mmap;
  mmap.pid = sample.pid();
  mmap.tid = sample.tid();
  mmap.start = sample.start();
  mmap.len = sample.len();
  mmap.pgoff = sample.pgoff();
  snprintf(mmap.filename, PATH_MAX, "%s", sample.filename().c_str());

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeMMap2Event(
    const event_t& event, PerfDataProto_MMapEvent* sample) const {
  const struct mmap2_event& mmap = event.mmap2;
  sample->set_pid(mmap.pid);
  sample->set_tid(mmap.tid);
  sample->set_start(mmap.start);
  sample->set_len(mmap.len);
  sample->set_pgoff(mmap.pgoff);
  if (event.header.misc & PERF_RECORD_MISC_MMAP_BUILD_ID) {
    if (mmap.build_id_size > sizeof(mmap.build_id)) {
      LOG(ERROR)
          << "Invalid build_id_size, possibly data corruption: build_id_size = "
          << mmap.build_id_size;
      return false;
    }
    sample->set_build_id(RawDataToHexString(mmap.build_id, mmap.build_id_size));
  } else {
    sample->set_maj(mmap.maj);
    sample->set_min(mmap.min);
    sample->set_ino(mmap.ino);
    sample->set_ino_generation(mmap.ino_generation);
  }
  sample->set_prot(mmap.prot);
  sample->set_flags(mmap.flags);
  sample->set_filename(mmap.filename);
  sample->set_filename_md5_prefix(Md5Prefix(mmap.filename));
  std::string root_path = RootPath(mmap.filename);
  if (!root_path.empty()) {
    sample->set_root_path(root_path);
  }
  sample->set_root_path_md5_prefix(Md5Prefix(root_path));

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeMMap2Event(
    const PerfDataProto_MMapEvent& sample, event_t* event) const {
  struct mmap2_event& mmap = event->mmap2;
  mmap.pid = sample.pid();
  mmap.tid = sample.tid();
  mmap.start = sample.start();
  mmap.len = sample.len();
  mmap.pgoff = sample.pgoff();
  if (sample.has_build_id()) {
    uint8_t bytes[kBuildIDArraySize];
    if (sample.build_id().size() % kHexCharsPerByte == 0 &&
        sample.build_id().size() / kHexCharsPerByte <= sizeof(bytes) &&
        HexStringToRawData(sample.build_id(), bytes, sizeof(bytes))) {
      mmap.build_id_size = sample.build_id().size() / kHexCharsPerByte;
      memset(mmap.build_id, 0, sizeof(mmap.build_id));
      memcpy(mmap.build_id, bytes, mmap.build_id_size);
    } else {
      LOG(ERROR) << "Failed to convert build_id=" << sample.build_id()
                 << " (size=" << sample.build_id().size() << ") into bytes.";
      return false;
    }
  } else {
    mmap.maj = sample.maj();
    mmap.min = sample.min();
    mmap.ino = sample.ino();
    mmap.ino_generation = sample.ino_generation();
  }
  mmap.prot = sample.prot();
  mmap.flags = sample.flags();
  snprintf(mmap.filename, PATH_MAX, "%s", sample.filename().c_str());

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeCommEvent(const event_t& event,
                                        PerfDataProto_CommEvent* sample) const {
  const struct comm_event& comm = event.comm;
  sample->set_pid(comm.pid);
  sample->set_tid(comm.tid);
  sample->set_comm(comm.comm);
  sample->set_comm_md5_prefix(Md5Prefix(comm.comm));

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeCommEvent(const PerfDataProto_CommEvent& sample,
                                          event_t* event) const {
  struct comm_event& comm = event->comm;
  comm.pid = sample.pid();
  comm.tid = sample.tid();
  snprintf(comm.comm, sizeof(comm.comm), "%s", sample.comm().c_str());

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeForkExitEvent(
    const event_t& event, PerfDataProto_ForkEvent* sample) const {
  const struct fork_event& fork = event.fork;
  sample->set_pid(fork.pid);
  sample->set_ppid(fork.ppid);
  sample->set_tid(fork.tid);
  sample->set_ptid(fork.ptid);
  sample->set_fork_time_ns(fork.time);

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeForkExitEvent(
    const PerfDataProto_ForkEvent& sample, event_t* event) const {
  struct fork_event& fork = event->fork;
  fork.pid = sample.pid();
  fork.ppid = sample.ppid();
  fork.tid = sample.tid();
  fork.ptid = sample.ptid();
  fork.time = sample.fork_time_ns();

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeLostEvent(const event_t& event,
                                        PerfDataProto_LostEvent* sample) const {
  const struct lost_event& lost = event.lost;
  sample->set_id(lost.id);
  sample->set_lost(lost.lost);

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeLostEvent(const PerfDataProto_LostEvent& sample,
                                          event_t* event) const {
  struct lost_event& lost = event->lost;
  lost.id = sample.id();
  lost.lost = sample.lost();

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeThrottleEvent(
    const event_t& event, PerfDataProto_ThrottleEvent* sample) const {
  const struct throttle_event& throttle = event.throttle;
  sample->set_time_ns(throttle.time);
  sample->set_id(throttle.id);
  sample->set_stream_id(throttle.stream_id);

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeThrottleEvent(
    const PerfDataProto_ThrottleEvent& sample, event_t* event) const {
  struct throttle_event& throttle = event->throttle;
  throttle.time = sample.time_ns();
  throttle.id = sample.id();
  throttle.stream_id = sample.stream_id();

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeAuxEvent(const event_t& event,
                                       PerfDataProto_AuxEvent* sample) const {
  const struct aux_event& aux = event.aux;
  sample->set_aux_offset(aux.aux_offset);
  sample->set_aux_size(aux.aux_size);
  sample->set_is_truncated(aux.flags & PERF_AUX_FLAG_TRUNCATED ? true : false);
  sample->set_is_overwrite(aux.flags & PERF_AUX_FLAG_OVERWRITE ? true : false);
  sample->set_is_partial(aux.flags & PERF_AUX_FLAG_PARTIAL ? true : false);
  if (aux.flags & ~(PERF_AUX_FLAG_TRUNCATED | PERF_AUX_FLAG_OVERWRITE |
                    PERF_AUX_FLAG_PARTIAL)) {
    LOG(WARNING) << "Ignoring unknown PERF_RECORD_AUX flag: " << aux.flags;
  }

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeAuxEvent(const PerfDataProto_AuxEvent& sample,
                                         event_t* event) const {
  struct aux_event& aux = event->aux;
  aux.aux_offset = sample.aux_offset();
  aux.aux_size = sample.aux_size();
  aux.flags |= sample.is_truncated() ? PERF_AUX_FLAG_TRUNCATED : 0;
  aux.flags |= sample.is_overwrite() ? PERF_AUX_FLAG_OVERWRITE : 0;
  aux.flags |= sample.is_partial() ? PERF_AUX_FLAG_PARTIAL : 0;

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeItraceStartEvent(
    const event_t& event, PerfDataProto_ItraceStartEvent* sample) const {
  const struct itrace_start_event& it_start = event.itrace_start;
  sample->set_pid(it_start.pid);
  sample->set_tid(it_start.tid);

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeItraceStartEvent(
    const PerfDataProto_ItraceStartEvent& sample, event_t* event) const {
  struct itrace_start_event& it_start = event->itrace_start;
  it_start.pid = sample.pid();
  it_start.tid = sample.tid();

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeLostSamplesEvent(
    const event_t& event, PerfDataProto_LostSamplesEvent* sample) const {
  const struct lost_samples_event& lost_samples = event.lost_samples;
  sample->set_num_lost(lost_samples.lost);

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeLostSamplesEvent(
    const PerfDataProto_LostSamplesEvent& sample, event_t* event) const {
  struct lost_samples_event& lost_samples = event->lost_samples;
  lost_samples.lost = sample.num_lost();

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeContextSwitchEvent(
    const event_t& event, PerfDataProto_ContextSwitchEvent* sample) const {
  const struct context_switch_event& context_switch = event.context_switch;
  sample->set_is_out(context_switch.header.misc & PERF_RECORD_MISC_SWITCH_OUT);
  if (context_switch.header.type == PERF_RECORD_SWITCH_CPU_WIDE) {
    sample->set_next_prev_pid(context_switch.next_prev_pid);
    sample->set_next_prev_tid(context_switch.next_prev_tid);
  }
  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeContextSwitchEvent(
    const PerfDataProto_ContextSwitchEvent& sample, event_t* event) const {
  struct context_switch_event& context_switch = event->context_switch;
  if (event->header.type == PERF_RECORD_SWITCH_CPU_WIDE) {
    context_switch.next_prev_pid = sample.next_prev_pid();
    context_switch.next_prev_tid = sample.next_prev_tid();
  }
  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeNamespacesEvent(
    const event_t& event, PerfDataProto_NamespacesEvent* sample) const {
  const struct namespaces_event& namespaces = event.namespaces;
  sample->set_pid(namespaces.pid);
  sample->set_tid(namespaces.tid);
  for (u64 i = 0; i < namespaces.nr_namespaces; ++i) {
    sample->add_link_info();
    sample->mutable_link_info(i)->set_dev(namespaces.link_info[i].dev);
    sample->mutable_link_info(i)->set_ino(namespaces.link_info[i].ino);
  }
  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeNamespacesEvent(
    const PerfDataProto_NamespacesEvent& sample, event_t* event) const {
  struct namespaces_event& namespaces = event->namespaces;
  namespaces.pid = sample.pid();
  namespaces.tid = sample.tid();
  namespaces.nr_namespaces = sample.link_info_size();
  for (u64 i = 0; i < namespaces.nr_namespaces; ++i) {
    namespaces.link_info[i].dev = sample.link_info(i).dev();
    namespaces.link_info[i].ino = sample.link_info(i).ino();
  }
  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeCgroupEvent(
    const event_t& event, PerfDataProto_CgroupEvent* sample) const {
  const struct cgroup_event& cgroup = event.cgroup;
  sample->set_id(cgroup.id);
  sample->set_path(cgroup.path);
  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeCgroupEvent(
    const PerfDataProto_CgroupEvent& sample, event_t* event) const {
  struct cgroup_event& cgroup = event->cgroup;
  cgroup.id = sample.id();
  snprintf(cgroup.path, PATH_MAX, "%s", sample.path().c_str());
  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeSampleInfo(
    const event_t& event, PerfDataProto_SampleInfo* sample) const {
  if (!ContainsSampleInfo(event.header.type)) return true;

  perf_sample sample_info;
  uint64_t sample_type = 0;
  if (!ReadPerfSampleInfoAndType(event, &sample_info, &sample_type))
    return false;

  if (sample_type & PERF_SAMPLE_TID) {
    sample->set_pid(sample_info.pid);
    sample->set_tid(sample_info.tid);
  }
  if (sample_type & PERF_SAMPLE_TIME)
    sample->set_sample_time_ns(sample_info.time);
  if ((sample_type & PERF_SAMPLE_ID) || (sample_type & PERF_SAMPLE_IDENTIFIER))
    sample->set_id(sample_info.id);
  if (sample_type & PERF_SAMPLE_CPU) sample->set_cpu(sample_info.cpu);
  if (sample_type & PERF_SAMPLE_STREAM_ID)
    sample->set_stream_id(sample_info.stream_id);
  return true;
}

bool PerfSerializer::DeserializeSampleInfo(
    const PerfDataProto_SampleInfo& sample, event_t* event) const {
  if (!ContainsSampleInfo(event->header.type)) return true;

  perf_sample sample_info = {};
  GetPerfSampleInfo(sample, &sample_info);

  const SampleInfoReader* writer = GetSampleInfoReaderForId(sample_info.id);
  CHECK(writer) << " failed for event " << event->header.type
                << ", sample id: " << sample_info.id;

  return writer->WritePerfSampleInfo(sample_info, event);
}

bool PerfSerializer::SerializeTracingMetadata(const char* from, size_t size,
                                              PerfDataProto* to) const {
  if (size == 0) {
    return true;
  }
  PerfDataProto_PerfTracingMetadata* data = to->mutable_tracing_data();
  data->set_tracing_data(from, size);

  return true;
}

bool PerfSerializer::DeserializeTracingMetadata(const PerfDataProto& from,
                                                std::vector<char>* to) const {
  if (!from.has_tracing_data()) {
    to->clear();
    return true;
  }

  const PerfDataProto_PerfTracingMetadata& data = from.tracing_data();
  to->assign(data.tracing_data().begin(), data.tracing_data().end());
  return true;
}

bool PerfSerializer::SerializeBuildIDEvent(
    const malloced_unique_ptr<build_id_event>& from,
    PerfDataProto_PerfBuildID* to) const {
  to->set_misc(from->header.misc);
  to->set_pid(from->pid);
  to->set_filename(from->filename);
  to->set_filename_md5_prefix(Md5Prefix(from->filename));
  if (from->header.misc & PERF_RECORD_MISC_BUILD_ID_SIZE)
    to->set_size(from->size);

  // Trim out trailing zeroes from the build ID.
  std::string build_id = RawDataToHexString(from->build_id, kBuildIDArraySize);
  TrimZeroesFromBuildIDString(&build_id);

  uint8_t build_id_bytes[kBuildIDArraySize];
  if (!HexStringToRawData(build_id, build_id_bytes, sizeof(build_id_bytes)))
    return false;

  // Used to convert build IDs (and possibly other hashes) between raw data
  // format and as string of hex digits.
  to->set_build_id_hash(build_id_bytes, build_id.size() / kHexCharsPerByte);

  return true;
}

bool PerfSerializer::DeserializeBuildIDEvent(
    const PerfDataProto_PerfBuildID& from,
    malloced_unique_ptr<build_id_event>* to) const {
  const std::string& filename = from.filename();
  size_t size =
      sizeof(build_id_event) + GetUint64AlignedStringLength(filename.size());

  malloced_unique_ptr<build_id_event>& event = *to;
  event.reset(CallocMemoryForBuildID(size));
  event->header.type = PERF_RECORD_HEADER_BUILD_ID;
  event->header.size = size;
  event->header.misc = from.misc();
  event->pid = from.pid();
  memcpy(event->build_id, from.build_id_hash().c_str(),
         from.build_id_hash().size());
  if (event->header.misc & PERF_RECORD_MISC_BUILD_ID_SIZE)
    event->size = from.size();

  if (from.has_filename() && !filename.empty()) {
    CHECK_GT(
        snprintf(event->filename, filename.size() + 1, "%s", filename.c_str()),
        0);
  }
  return true;
}

bool PerfSerializer::SerializeAuxtraceInfoEvent(
    const event_t& event, PerfDataProto_AuxtraceInfoEvent* sample) const {
  const struct auxtrace_info_event& auxtrace_info = event.auxtrace_info;
  u64 priv_size =
      (event.header.size - sizeof(struct auxtrace_info_event)) / sizeof(u64);
  sample->set_type(auxtrace_info.type);
  if (auxtrace_info.reserved__ != 0) {
    LOG(WARNING) << "PERF_RECORD_AUXTRACE_INFO's auxtrace_info_event.reserved__"
                    " contains a non-zero value: "
                 << auxtrace_info.reserved__
                 << ". This"
                    " record's format has changed.";
  }
  for (u64 i = 0; i < priv_size; ++i) {
    sample->add_unparsed_binary_blob_priv_data(auxtrace_info.priv[i]);
  }
  return true;
}

bool PerfSerializer::DeserializeAuxtraceInfoEvent(
    const PerfDataProto_AuxtraceInfoEvent& sample, event_t* event) const {
  struct auxtrace_info_event& auxtrace_info = event->auxtrace_info;
  auxtrace_info.type = sample.type();
  for (int i = 0; i < sample.unparsed_binary_blob_priv_data_size(); ++i) {
    auxtrace_info.priv[i] = sample.unparsed_binary_blob_priv_data(i);
  }
  return true;
}

bool PerfSerializer::SerializeAuxtraceEvent(
    const event_t& event, PerfDataProto_AuxtraceEvent* sample) const {
  const struct auxtrace_event& auxtrace = event.auxtrace;
  sample->set_size(auxtrace.size);
  sample->set_offset(auxtrace.offset);
  sample->set_reference(auxtrace.reference);
  sample->set_idx(auxtrace.idx);
  sample->set_tid(auxtrace.tid);
  sample->set_cpu(auxtrace.cpu);

  return true;
}

bool PerfSerializer::SerializeAuxtraceEventTraceData(
    const char* from, size_t size, PerfDataProto_AuxtraceEvent* to) const {
  if (size == 0) {
    return true;
  }
  to->set_trace_data(from, size);

  return true;
}

bool PerfSerializer::DeserializeAuxtraceEvent(
    const PerfDataProto_AuxtraceEvent& sample, event_t* event) const {
  struct auxtrace_event& auxtrace = event->auxtrace;
  auxtrace.size = sample.size();
  auxtrace.offset = sample.offset();
  auxtrace.reference = sample.reference();
  auxtrace.idx = sample.idx();
  auxtrace.tid = sample.tid();
  auxtrace.cpu = sample.cpu();

  return true;
}

bool PerfSerializer::DeserializeAuxtraceEventTraceData(
    const PerfDataProto_AuxtraceEvent& from, std::vector<char>* to) const {
  to->assign(from.trace_data().begin(), from.trace_data().end());
  return true;
}

bool PerfSerializer::SerializeAuxtraceErrorEvent(
    const event_t& event, PerfDataProto_AuxtraceErrorEvent* sample) const {
  const struct auxtrace_error_event& auxtrace_error = event.auxtrace_error;
  sample->set_type(auxtrace_error.type);
  sample->set_code(auxtrace_error.code);
  sample->set_cpu(auxtrace_error.cpu);
  sample->set_pid(auxtrace_error.pid);
  sample->set_tid(auxtrace_error.tid);
  sample->set_ip(auxtrace_error.ip);
  if (auxtrace_error.reserved__ != 0) {
    LOG(WARNING)
        << "PERF_RECORD_AUXTRACE_ERROR's auxtrace_error_event.reserved__ "
        << "contains a non-zero value: " << auxtrace_error.reserved__ << ". "
        << "This record's format has changed.";
  }
  size_t len =
      strnlen(auxtrace_error.msg,
              std::min<size_t>(MAX_AUXTRACE_ERROR_MSG,
                               (auxtrace_error.header.size -
                                offsetof(struct auxtrace_error_event, msg))));
  sample->set_msg(auxtrace_error.msg, len);
  sample->set_msg_md5_prefix(Md5Prefix(sample->msg()));
  return true;
}

bool PerfSerializer::DeserializeAuxtraceErrorEvent(
    const PerfDataProto_AuxtraceErrorEvent& sample, event_t* event) const {
  struct auxtrace_error_event& auxtrace_error = event->auxtrace_error;
  auxtrace_error.type = sample.type();
  auxtrace_error.code = sample.code();
  auxtrace_error.cpu = sample.cpu();
  auxtrace_error.pid = sample.pid();
  auxtrace_error.tid = sample.tid();
  auxtrace_error.ip = sample.ip();
  snprintf(auxtrace_error.msg, MAX_AUXTRACE_ERROR_MSG, "%s",
           sample.msg().c_str());
  return true;
}

bool PerfSerializer::SerializeThreadMapEvent(
    const event_t& event, PerfDataProto_ThreadMapEvent* sample) const {
  const struct thread_map_event& thread_map = event.thread_map;
  for (u64 i = 0; i < thread_map.nr; ++i) {
    auto entry = sample->add_entries();
    entry->set_pid(thread_map.entries[i].pid);
    entry->set_comm(thread_map.entries[i].comm);
    entry->set_comm_md5_prefix(Md5Prefix(thread_map.entries[i].comm));
  }
  return true;
}

bool PerfSerializer::DeserializeThreadMapEvent(
    const PerfDataProto_ThreadMapEvent& sample, event_t* event) const {
  struct thread_map_event& thread_map = event->thread_map;
  thread_map.nr = sample.entries_size();
  for (u64 i = 0; i < thread_map.nr; ++i) {
    thread_map.entries[i].pid = sample.entries(i).pid();
    snprintf(thread_map.entries[i].comm, sizeof(thread_map.entries[i].comm),
             "%s", sample.entries(i).comm().c_str());
  }
  return true;
}

bool PerfSerializer::SerializeStatConfigEvent(
    const event_t& event, PerfDataProto_StatConfigEvent* sample) const {
  const struct stat_config_event& stat_config = event.stat_config;
  for (u64 i = 0; i < stat_config.nr; ++i) {
    auto data = sample->add_data();
    if (stat_config.data[i].tag >= PERF_STAT_CONFIG_TERM__MAX) {
      LOG(WARNING) << "Unknown stat_config_event_entry.tag, got "
                   << stat_config.data[i].tag;
    } else if (stat_config.data[i].tag == PERF_STAT_CONFIG_TERM__AGGR_MODE &&
               stat_config.data[i].val >= aggr_mode::AGGR_MAX) {
      LOG(WARNING) << "Unknown perf_stat_config.aggr_mode, got "
                   << stat_config.data[i].val;
    }

    data->set_tag(stat_config.data[i].tag);
    data->set_val(stat_config.data[i].val);
  }
  return true;
}

bool PerfSerializer::DeserializeStatConfigEvent(
    const PerfDataProto_StatConfigEvent& sample, event_t* event) const {
  struct stat_config_event& stat_config = event->stat_config;
  stat_config.nr = sample.data_size();
  for (u64 i = 0; i < stat_config.nr; ++i) {
    stat_config.data[i].tag = sample.data(i).tag();
    stat_config.data[i].val = sample.data(i).val();
  }
  return true;
}

bool PerfSerializer::SerializeStatEvent(const event_t& event,
                                        PerfDataProto_StatEvent* sample) const {
  const struct stat_event& stat = event.stat;
  sample->set_id(stat.id);
  sample->set_cpu(stat.cpu);
  sample->set_thread(stat.thread);
  sample->set_value(stat.val);
  sample->set_enabled(stat.ena);
  sample->set_running(stat.run);
  return true;
}

bool PerfSerializer::DeserializeStatEvent(const PerfDataProto_StatEvent& sample,
                                          event_t* event) const {
  struct stat_event& stat = event->stat;
  stat.id = sample.id();
  stat.cpu = sample.cpu();
  stat.thread = sample.thread();
  stat.val = sample.value();
  stat.ena = sample.enabled();
  stat.run = sample.running();
  return true;
}

bool PerfSerializer::SerializeStatRoundEvent(
    const event_t& event, PerfDataProto_StatRoundEvent* sample) const {
  const struct stat_round_event& stat_round = event.stat_round;
  sample->set_type(stat_round.type);
  sample->set_time(stat_round.time);
  return true;
}

bool PerfSerializer::DeserializeStatRoundEvent(
    const PerfDataProto_StatRoundEvent& sample, event_t* event) const {
  struct stat_round_event& stat_round = event->stat_round;
  stat_round.type = sample.type();
  stat_round.time = sample.time();
  return true;
}

bool PerfSerializer::SerializeTimeConvEvent(
    const event_t& event, PerfDataProto_TimeConvEvent* sample) const {
  const struct time_conv_event& time_conv = event.time_conv;
  sample->set_time_shift(time_conv.time_shift);
  sample->set_time_mult(time_conv.time_mult);
  sample->set_time_zero(time_conv.time_zero);
  // Large time_conv struct.
  if (time_conv.header.size == sizeof(struct time_conv_event)) {
    sample->set_time_cycles(time_conv.time_cycles);
    sample->set_time_mask(time_conv.time_mask);
    sample->set_cap_user_time_zero(time_conv.cap_user_time_zero != 0);
    sample->set_cap_user_time_short(time_conv.cap_user_time_short != 0);
  }
  return true;
}

bool PerfSerializer::DeserializeTimeConvEvent(
    const PerfDataProto_TimeConvEvent& sample, event_t* event) const {
  struct time_conv_event& time_conv = event->time_conv;
  time_conv.time_shift = sample.time_shift();
  time_conv.time_mult = sample.time_mult();
  time_conv.time_zero = sample.time_zero();
  if (sample.has_time_cycles()) {
    time_conv.time_cycles = sample.time_cycles();
    time_conv.time_mask = sample.time_mask();
    time_conv.cap_user_time_zero = sample.cap_user_time_zero();
    time_conv.cap_user_time_short = sample.cap_user_time_short();
  }
  return true;
}

bool PerfSerializer::SerializeSingleUint32Metadata(
    const PerfUint32Metadata& metadata,
    PerfDataProto_PerfUint32Metadata* proto_metadata) const {
  proto_metadata->set_type(metadata.type);
  for (size_t i = 0; i < metadata.data.size(); ++i)
    proto_metadata->add_data(metadata.data[i]);
  return true;
}

bool PerfSerializer::DeserializeSingleUint32Metadata(
    const PerfDataProto_PerfUint32Metadata& proto_metadata,
    PerfUint32Metadata* metadata) const {
  metadata->type = proto_metadata.type();
  for (int i = 0; i < proto_metadata.data_size(); ++i)
    metadata->data.push_back(proto_metadata.data(i));
  return true;
}

bool PerfSerializer::SerializeSingleUint64Metadata(
    const PerfUint64Metadata& metadata,
    PerfDataProto_PerfUint64Metadata* proto_metadata) const {
  proto_metadata->set_type(metadata.type);
  for (size_t i = 0; i < metadata.data.size(); ++i)
    proto_metadata->add_data(metadata.data[i]);
  return true;
}

bool PerfSerializer::DeserializeSingleUint64Metadata(
    const PerfDataProto_PerfUint64Metadata& proto_metadata,
    PerfUint64Metadata* metadata) const {
  metadata->type = proto_metadata.type();
  for (int i = 0; i < proto_metadata.data_size(); ++i)
    metadata->data.push_back(proto_metadata.data(i));
  return true;
}

bool PerfSerializer::SerializeCPUTopologyMetadata(
    const PerfCPUTopologyMetadata& metadata,
    PerfDataProto_PerfCPUTopologyMetadata* proto_metadata) const {
  for (const std::string& core_name : metadata.core_siblings) {
    proto_metadata->add_core_siblings(core_name);
    proto_metadata->add_core_siblings_md5_prefix(Md5Prefix(core_name));
  }
  for (const std::string& thread_name : metadata.thread_siblings) {
    proto_metadata->add_thread_siblings(thread_name);
    proto_metadata->add_thread_siblings_md5_prefix(Md5Prefix(thread_name));
  }
  for (PerfCPU cpu : metadata.available_cpus) {
    PerfDataProto_PerfCPUTopologyMetadata_CPU* proto_cpu =
        proto_metadata->add_available_cpus();
    proto_cpu->set_core_id(cpu.core_id);
    proto_cpu->set_socket_id(cpu.socket_id);
  }
  return true;
}

bool PerfSerializer::DeserializeCPUTopologyMetadata(
    const PerfDataProto_PerfCPUTopologyMetadata& proto_metadata,
    PerfCPUTopologyMetadata* metadata) const {
  metadata->core_siblings.clear();
  metadata->core_siblings.reserve(proto_metadata.core_siblings().size());
  std::copy(proto_metadata.core_siblings().begin(),
            proto_metadata.core_siblings().end(),
            std::back_inserter(metadata->core_siblings));

  metadata->thread_siblings.clear();
  metadata->thread_siblings.reserve(proto_metadata.thread_siblings().size());
  std::copy(proto_metadata.thread_siblings().begin(),
            proto_metadata.thread_siblings().end(),
            std::back_inserter(metadata->thread_siblings));

  metadata->available_cpus.clear();
  for (const PerfDataProto_PerfCPUTopologyMetadata_CPU& proto_cpu :
       proto_metadata.available_cpus()) {
    PerfCPU cpu;
    cpu.core_id = proto_cpu.core_id();
    cpu.socket_id = proto_cpu.socket_id();
    metadata->available_cpus.push_back(cpu);
  }
  return true;
}

bool PerfSerializer::SerializeNodeTopologyMetadata(
    const PerfNodeTopologyMetadata& metadata,
    PerfDataProto_PerfNodeTopologyMetadata* proto_metadata) const {
  proto_metadata->set_id(metadata.id);
  proto_metadata->set_total_memory(metadata.total_memory);
  proto_metadata->set_free_memory(metadata.free_memory);
  proto_metadata->set_cpu_list(metadata.cpu_list);
  proto_metadata->set_cpu_list_md5_prefix(Md5Prefix(metadata.cpu_list));
  return true;
}

bool PerfSerializer::DeserializeNodeTopologyMetadata(
    const PerfDataProto_PerfNodeTopologyMetadata& proto_metadata,
    PerfNodeTopologyMetadata* metadata) const {
  metadata->id = proto_metadata.id();
  metadata->total_memory = proto_metadata.total_memory();
  metadata->free_memory = proto_metadata.free_memory();
  metadata->cpu_list = proto_metadata.cpu_list();
  return true;
}

bool PerfSerializer::SerializePMUMappingsMetadata(
    const PerfPMUMappingsMetadata& metadata,
    PerfDataProto_PerfPMUMappingsMetadata* proto_metadata) const {
  proto_metadata->set_type(metadata.type);
  proto_metadata->set_name(metadata.name);
  proto_metadata->set_name_md5_prefix(Md5Prefix(metadata.name));
  return true;
}

bool PerfSerializer::DeserializePMUMappingsMetadata(
    const PerfDataProto_PerfPMUMappingsMetadata& proto_metadata,
    PerfPMUMappingsMetadata* metadata) const {
  metadata->type = proto_metadata.type();
  metadata->name = proto_metadata.name();
  return true;
}

bool PerfSerializer::SerializeGroupDescMetadata(
    const PerfGroupDescMetadata& metadata,
    PerfDataProto_PerfGroupDescMetadata* proto_metadata) const {
  proto_metadata->set_name(metadata.name);
  proto_metadata->set_name_md5_prefix(Md5Prefix(metadata.name));
  proto_metadata->set_leader_idx(metadata.leader_idx);
  proto_metadata->set_num_members(metadata.num_members);
  return true;
}

bool PerfSerializer::DeserializeGroupDescMetadata(
    const PerfDataProto_PerfGroupDescMetadata& proto_metadata,
    PerfGroupDescMetadata* metadata) const {
  metadata->name = proto_metadata.name();
  metadata->leader_idx = proto_metadata.leader_idx();
  metadata->num_members = proto_metadata.num_members();
  return true;
}

// static
void PerfSerializer::SerializeParserStats(const PerfEventStats& stats,
                                          PerfDataProto* perf_data_proto) {
  PerfDataProto_PerfEventStats* stats_pb = perf_data_proto->mutable_stats();
  stats_pb->set_num_sample_events(stats.num_sample_events);
  stats_pb->set_num_mmap_events(stats.num_mmap_events);
  stats_pb->set_num_fork_events(stats.num_fork_events);
  stats_pb->set_num_exit_events(stats.num_exit_events);
  stats_pb->set_did_remap(stats.did_remap);
  stats_pb->set_num_sample_events_mapped(stats.num_sample_events_mapped);
}

// static
void PerfSerializer::DeserializeParserStats(
    const PerfDataProto& perf_data_proto, PerfEventStats* stats) {
  const PerfDataProto_PerfEventStats& stats_pb = perf_data_proto.stats();
  stats->num_sample_events = stats_pb.num_sample_events();
  stats->num_mmap_events = stats_pb.num_mmap_events();
  stats->num_fork_events = stats_pb.num_fork_events();
  stats->num_exit_events = stats_pb.num_exit_events();
  stats->did_remap = stats_pb.did_remap();
  stats->num_sample_events_mapped = stats_pb.num_sample_events_mapped();
}

bool PerfSerializer::CreateSampleInfoReader(const PerfFileAttr& attr,
                                            bool read_cross_endian) {
  for (const auto& id :
       (attr.ids.empty() ? std::initializer_list<u64>({0}) : attr.ids)) {
    sample_info_reader_map_[id].reset(
        new SampleInfoReader(attr.attr, read_cross_endian));
  }
  return UpdateEventIdPositions(attr.attr);
}

bool PerfSerializer::UpdateEventIdPositions(
    const struct perf_event_attr& attr) {
  const u64 sample_type = attr.sample_type;
  ssize_t new_sample_event_id_pos = EventIdPosition::NotPresent;
  ssize_t new_other_event_id_pos = EventIdPosition::NotPresent;
  if (sample_type & PERF_SAMPLE_IDENTIFIER) {
    new_sample_event_id_pos = 0;
    new_other_event_id_pos = 1;
  } else if (sample_type & PERF_SAMPLE_ID) {
    // Increment for IP, TID, TIME, ADDR
    new_sample_event_id_pos = 0;
    if (sample_type & PERF_SAMPLE_IP) new_sample_event_id_pos++;
    if (sample_type & PERF_SAMPLE_TID) new_sample_event_id_pos++;
    if (sample_type & PERF_SAMPLE_TIME) new_sample_event_id_pos++;
    if (sample_type & PERF_SAMPLE_ADDR) new_sample_event_id_pos++;

    // Increment for CPU, STREAM_ID
    new_other_event_id_pos = 1;
    if (sample_type & PERF_SAMPLE_CPU) new_other_event_id_pos++;
    if (sample_type & PERF_SAMPLE_STREAM_ID) new_other_event_id_pos++;
  }

  if (sample_event_id_pos_ == EventIdPosition::Uninitialized) {
    sample_event_id_pos_ = new_sample_event_id_pos;
  } else if (new_sample_event_id_pos != sample_event_id_pos_) {
    LOG(ERROR)
        << "Event IDs must be in a consistent positition. Event ID"
        << " position from sample events linked to current attr "
        << new_sample_event_id_pos
        << " doesn't match the position from sample events linked to previous"
        << " attrs " << sample_event_id_pos_;
    return false;
  }

  if (other_event_id_pos_ == EventIdPosition::Uninitialized) {
    other_event_id_pos_ = new_other_event_id_pos;
  } else if (new_other_event_id_pos != other_event_id_pos_) {
    LOG(ERROR) << "Event IDs must be in a consistent positition. Event ID"
               << " position from non-sample events linked to current attr "
               << new_other_event_id_pos
               << " doesn't match the position from non-sample events linked to"
               << " previous attrs " << other_event_id_pos_;
    return false;
  }
  return true;
}

const SampleInfoReader* PerfSerializer::GetSampleInfoReaderForEvent(
    const event_t& event) const {
  // Where is the event id?
  ssize_t event_id_pos = EventIdPosition::Uninitialized;
  if (event.header.type == PERF_RECORD_SAMPLE) {
    event_id_pos = sample_event_id_pos_;
  } else if (ContainsSampleInfo(event.header.type)) {
    event_id_pos = other_event_id_pos_;
  } else {
    event_id_pos = EventIdPosition::NotPresent;
  }

  // What is the event id?
  u64 event_id;
  switch (event_id_pos) {
    case EventIdPosition::Uninitialized:
      LOG(FATAL) << "Position of event id was not initialized!";
      return nullptr;
    case EventIdPosition::NotPresent:
      event_id = 0;
      break;
    default:
      size_t offset = GetEventDataSize(event);
      if (offset == 0) {
        LOG(ERROR) << "Couldn't get event data size for event "
                   << GetEventName(event.header.type);
        return nullptr;
      }
      if (event.header.size <= offset) {
        LOG(ERROR) << "Sample info offset " << offset << " past event size "
                   << event.header.size << " for event "
                   << GetEventName(event.header.type);
        return nullptr;
      }
      // Directly access the sample info data array beginning at the sample info
      // data offset.
      const u64* array =
          reinterpret_cast<const u64*>(&event) + (offset / sizeof(u64));
      // Find the length of the sample info array.
      ssize_t array_size = (event.header.size - offset) / sizeof(u64);
      if (event.header.type == PERF_RECORD_SAMPLE) {
        if (array_size <= event_id_pos) {
          LOG(ERROR) << "Sample info array of size " << array_size
                     << " doesn't contain event id at the position "
                     << event_id_pos << " for event "
                     << GetEventName(event.header.type);
          return nullptr;
        }
        event_id = array[event_id_pos];
      } else {
        // The event id is at the end of the sample info data array, and
        // event_id_pos (aka other_event_id_pos_) counts from the end.
        // This check doesn't guarantee that the correct field is read to get
        // the event id as the |event_id_pos| is relative to the end of the
        // sample info data array. This check just ensures that there is enough
        // data available to read the value at |array_size - event_id_pos|.
        if (array_size < event_id_pos) {
          LOG(ERROR) << "Sample info array of size " << array_size
                     << " is not big enough to hold event id at the position "
                     << event_id_pos << " from the end of the array for event "
                     << GetEventName(event.header.type);
          return nullptr;
        }
        event_id = array[array_size - event_id_pos];
      }
      break;
  }
  return GetSampleInfoReaderForId(event_id);
}

const SampleInfoReader* PerfSerializer::GetSampleInfoReaderForId(
    uint64_t id) const {
  if (id) {
    auto iter = sample_info_reader_map_.find(id);
    if (iter == sample_info_reader_map_.end()) return nullptr;
    return iter->second.get();
  }

  if (sample_info_reader_map_.empty()) return nullptr;
  return sample_info_reader_map_.begin()->second.get();
}

bool PerfSerializer::ReadPerfSampleInfoAndType(const event_t& event,
                                               perf_sample* sample_info,
                                               uint64_t* sample_type) const {
  const SampleInfoReader* reader = GetSampleInfoReaderForEvent(event);
  if (!reader) {
    LOG(ERROR) << "No SampleInfoReader available";
    return false;
  }

  if (!reader->ReadPerfSampleInfo(event, sample_info)) return false;
  *sample_type = reader->event_attr().sample_type;
  return true;
}

void PerfSerializer::GetPerfSampleInfo(const PerfDataProto_SampleInfo& sample,
                                       perf_sample* sample_info) const {
  if (sample.has_pid()) {
    CHECK(sample.has_tid()) << "Cannot have PID without TID.";
    sample_info->pid = sample.pid();
    sample_info->tid = sample.tid();
  }
  if (sample.has_sample_time_ns()) sample_info->time = sample.sample_time_ns();
  if (sample.has_id()) sample_info->id = sample.id();
  if (sample.has_cpu()) sample_info->cpu = sample.cpu();
  if (sample.has_stream_id()) sample_info->stream_id = sample.stream_id();
}

void PerfSerializer::GetPerfSampleInfo(const PerfDataProto_SampleEvent& sample,
                                       perf_sample* sample_info) const {
  if (sample.has_ip()) sample_info->ip = sample.ip();
  if (sample.has_pid()) {
    CHECK(sample.has_tid()) << "Cannot have PID without TID.";
    sample_info->pid = sample.pid();
    sample_info->tid = sample.tid();
  }
  if (sample.has_sample_time_ns()) sample_info->time = sample.sample_time_ns();
  if (sample.has_addr()) sample_info->addr = sample.addr();
  if (sample.has_id()) sample_info->id = sample.id();
  if (sample.has_stream_id()) sample_info->stream_id = sample.stream_id();
  if (sample.has_cpu()) sample_info->cpu = sample.cpu();
  if (sample.has_period()) sample_info->period = sample.period();
  if (sample.has_read_info()) {
    const SampleInfoReader* reader = GetSampleInfoReaderForId(sample_info->id);
    if (reader) {
      const PerfDataProto_ReadInfo& read_info = sample.read_info();
      if (reader->event_attr().read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
        sample_info->read.time_enabled = read_info.time_enabled();
      if (reader->event_attr().read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
        sample_info->read.time_running = read_info.time_running();
      if (reader->event_attr().read_format & PERF_FORMAT_GROUP) {
        sample_info->read.group.nr = read_info.read_value_size();
        sample_info->read.group.values =
            new sample_read_value[read_info.read_value_size()];
        for (size_t i = 0; i < sample_info->read.group.nr; i++) {
          sample_info->read.group.values[i].value =
              read_info.read_value(i).value();
          sample_info->read.group.values[i].id = read_info.read_value(i).id();
        }
      } else if (read_info.read_value_size() == 1) {
        sample_info->read.one.value = read_info.read_value(0).value();
        sample_info->read.one.id = read_info.read_value(0).id();
      } else {
        LOG(ERROR) << "Expected read_value array size of 1 but got "
                   << read_info.read_value_size();
      }
    }
  }
  if (sample.callchain_size() > 0) {
    uint64_t callchain_size = sample.callchain_size();
    sample_info->callchain = reinterpret_cast<struct ip_callchain*>(
        new uint64_t[callchain_size + 1]);
    sample_info->callchain->nr = callchain_size;
    for (size_t i = 0; i < callchain_size; ++i)
      sample_info->callchain->ips[i] = sample.callchain(i);
  }
  // See raw and raw_size comments in perf_data.proto
  if (!sample.raw().empty()) {
    sample_info->raw_size = sample.raw().size();
    sample_info->raw_data = new uint8_t[sample.raw().size()];
    sample.raw().copy(reinterpret_cast<char*>(sample_info->raw_data),
                      sample_info->raw_size);
  } else if (sample.raw_size() > 0) {
    sample_info->raw_size = sample.raw_size();
    sample_info->raw_data = new uint8_t[sample.raw_size()];
    memset(sample_info->raw_data, 0, sample.raw_size());
  }
  // Older protos w/o branch_stack_hw_idx field shall have no_hw_idx set.
  // So the perf profiles don't change.
  sample_info->no_hw_idx = true;
  if (sample.has_no_hw_idx()) sample_info->no_hw_idx = sample.no_hw_idx();
  if (sample.branch_stack_size() > 0) {
    uint64_t branch_stack_size = sample.branch_stack_size();
    sample_info->branch_stack = reinterpret_cast<struct branch_stack*>(
        new uint8_t[sizeof(uint64_t) + sizeof(uint64_t) +
                    branch_stack_size * sizeof(struct branch_entry)]);
    sample_info->branch_stack->nr = branch_stack_size;
    if (sample.has_branch_stack_hw_idx())
      sample_info->branch_stack->hw_idx = sample.branch_stack_hw_idx();
    for (size_t i = 0; i < branch_stack_size; ++i) {
      struct branch_entry& entry = sample_info->branch_stack->entries[i];
      memset(&entry, 0, sizeof(entry));
      entry.from = sample.branch_stack(i).from_ip();
      entry.to = sample.branch_stack(i).to_ip();
      entry.flags.mispred = sample.branch_stack(i).mispredicted();
      entry.flags.predicted = sample.branch_stack(i).predicted();
      entry.flags.in_tx = sample.branch_stack(i).in_transaction();
      entry.flags.abort = sample.branch_stack(i).abort();
      entry.flags.cycles = sample.branch_stack(i).cycles();
      entry.flags.type = sample.branch_stack(i).type();
      entry.flags.spec = sample.branch_stack(i).spec();
    }
  }

  if (sample.has_weight()) sample_info->weight.full = sample.weight();
  if (sample.has_weight_struct()) {
    sample_info->weight.var1_dw = sample.weight_struct().var1_dw();
    sample_info->weight.var2_w = sample.weight_struct().var2_w();
    sample_info->weight.var3_w = sample.weight_struct().var3_w();
  }
  if (sample.has_data_src()) sample_info->data_src = sample.data_src();
  if (sample.has_transaction()) sample_info->transaction = sample.transaction();
  if (sample.has_physical_addr())
    sample_info->physical_addr = sample.physical_addr();
  if (sample.has_cgroup()) sample_info->cgroup = sample.cgroup();
  if (sample.has_data_page_size())
    sample_info->data_page_size = sample.data_page_size();
  if (sample.has_code_page_size())
    sample_info->code_page_size = sample.code_page_size();
}

}  // namespace quipper
