// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "perf_parser.h"

#include <stdint.h>
#include <sys/capability.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/logging.h"
#include "compat/test.h"
#include "compat/thread.h"
#include "dso_test_utils.h"
#include "perf_buildid.h"
#include "perf_data_utils.h"
#include "perf_reader.h"
#include "perf_serializer.h"
#include "perf_test_files.h"
#include "scoped_temp_path.h"
#include "test_perf_data.h"
#include "test_utils.h"

namespace quipper {

using SampleEvent = PerfDataProto_SampleEvent;
using SampleInfo = PerfDataProto_SampleInfo;
using PerfEvent = PerfDataProto_PerfEvent;

namespace {

void CheckChronologicalOrderOfEvents(const PerfReader &reader) {
  if (reader.events().empty()) return;
  const auto &events = reader.events();
  uint64_t prev_time = GetTimeFromPerfEvent(events.Get(0));
  for (int i = 1; i < events.size(); ++i) {
    uint64_t new_time = GetTimeFromPerfEvent(events.Get(i));
    CHECK_LE(prev_time, new_time);
    prev_time = new_time;
  }
}

void CheckNoDuplicates(const std::vector<std::string> &list) {
  std::set<std::string> list_as_set(list.begin(), list.end());
  if (list.size() != list_as_set.size())
    ADD_FAILURE() << "Given list has at least one duplicate";
}

void CreateFilenameToBuildIDMap(
    const std::vector<std::string> &filenames, unsigned int seed,
    std::map<std::string, std::string> *filenames_to_build_ids) {
  srand(seed);
  // Only use every other filename, so that half the filenames are unused.
  for (size_t i = 0; i < filenames.size(); i += 2) {
    u8 build_id[kBuildIDArraySize];
    for (size_t j = 0; j < kBuildIDArraySize; ++j) build_id[j] = rand_r(&seed);

    (*filenames_to_build_ids)[filenames[i]] =
        RawDataToHexString(build_id, kBuildIDArraySize);
  }
}

// Given a PerfReader that has already consumed an input perf data file, inject
// new build IDs for the MMAP'd files in the perf data and check that they have
// been correctly injected.
void CheckFilenameAndBuildIDMethods(PerfReader *reader,
                                    const std::string &output_perf_data_prefix,
                                    unsigned int seed) {
  // Check filenames.
  std::vector<std::string> filenames;
  reader->GetFilenames(&filenames);

  ASSERT_FALSE(filenames.empty());
  CheckNoDuplicates(filenames);

  std::set<std::string> filename_set;
  reader->GetFilenamesAsSet(&filename_set);

  // Make sure all MMAP filenames are in the set.
  for (const auto &event : reader->events()) {
    if (event.header().type() == PERF_RECORD_MMAP) {
      EXPECT_TRUE(filename_set.find(event.mmap_event().filename()) !=
                  filename_set.end())
          << event.mmap_event().filename()
          << " is not present in the filename set";
    }
  }

  std::map<std::string, std::string> expected_map;
  reader->GetFilenamesToBuildIDs(&expected_map);

  // Inject some made up build ids.
  std::map<std::string, std::string> filenames_to_build_ids;
  CreateFilenameToBuildIDMap(filenames, seed, &filenames_to_build_ids);
  ASSERT_TRUE(reader->InjectBuildIDs(filenames_to_build_ids));

  // Reader should now correctly populate the filenames to build ids map.
  std::map<std::string, std::string>::const_iterator it;
  for (it = filenames_to_build_ids.begin(); it != filenames_to_build_ids.end();
       ++it) {
    expected_map[it->first] = it->second;
  }
  std::map<std::string, std::string> reader_map;
  reader->GetFilenamesToBuildIDs(&reader_map);
  ASSERT_EQ(expected_map, reader_map);

  std::string output_perf_data1 = output_perf_data_prefix + ".parse.inject.out";
  ASSERT_TRUE(reader->WriteFile(output_perf_data1));

  // Perf should find the same build ids.
  std::map<std::string, std::string> perf_build_id_map;
  ASSERT_TRUE(GetPerfBuildIDMap(output_perf_data1, &perf_build_id_map));
  ASSERT_EQ(expected_map, perf_build_id_map);

  std::map<std::string, std::string> build_id_localizer;
  // Only localize the first half of the files which have build ids.
  for (size_t j = 0; j < filenames.size() / 2; ++j) {
    std::string old_filename = filenames[j];
    if (expected_map.find(old_filename) == expected_map.end()) continue;
    std::string build_id = expected_map[old_filename];

    std::string new_filename = old_filename + ".local";
    filenames[j] = new_filename;
    build_id_localizer[build_id] = new_filename;
    expected_map[new_filename] = build_id;
    expected_map.erase(old_filename);
  }
  reader->Localize(build_id_localizer);

  // Filenames should be the same.
  std::vector<std::string> new_filenames;
  reader->GetFilenames(&new_filenames);
  std::sort(filenames.begin(), filenames.end());
  ASSERT_EQ(filenames, new_filenames);

  // Build ids should be updated.
  reader_map.clear();
  reader->GetFilenamesToBuildIDs(&reader_map);
  ASSERT_EQ(expected_map, reader_map);

  std::string output_perf_data2 =
      output_perf_data_prefix + ".parse.localize.out";
  ASSERT_TRUE(reader->WriteFile(output_perf_data2));

  perf_build_id_map.clear();
  ASSERT_TRUE(GetPerfBuildIDMap(output_perf_data2, &perf_build_id_map));
  EXPECT_EQ(expected_map, perf_build_id_map);

  std::map<std::string, std::string> filename_localizer;
  // Only localize every third filename.
  for (size_t j = 0; j < filenames.size(); j += 3) {
    std::string old_filename = filenames[j];
    std::string new_filename = old_filename + ".local2";
    filenames[j] = new_filename;
    filename_localizer[old_filename] = new_filename;

    if (expected_map.find(old_filename) != expected_map.end()) {
      std::string build_id = expected_map[old_filename];
      expected_map[new_filename] = build_id;
      expected_map.erase(old_filename);
    }
  }
  reader->LocalizeUsingFilenames(filename_localizer);

  // Filenames should be the same.
  new_filenames.clear();
  reader->GetFilenames(&new_filenames);
  std::sort(filenames.begin(), filenames.end());
  EXPECT_EQ(filenames, new_filenames);

  // Build ids should be updated.
  reader_map.clear();
  reader->GetFilenamesToBuildIDs(&reader_map);
  EXPECT_EQ(expected_map, reader_map);

  std::string output_perf_data3 =
      output_perf_data_prefix + ".parse.localize2.out";
  ASSERT_TRUE(reader->WriteFile(output_perf_data3));

  perf_build_id_map.clear();
  ASSERT_TRUE(GetPerfBuildIDMap(output_perf_data3, &perf_build_id_map));
  EXPECT_EQ(expected_map, perf_build_id_map);
}

void CopyActualEvents(const std::vector<ParsedEvent> &events,
                      PerfDataProto *out) {
  for (const auto &ev : events) {
    if (ev.event_ptr == nullptr) {
      continue;
    }
    *out->add_events() = *ev.event_ptr;
  }
}

// Given a PerfReader that has already consumed an input perf data file using
// address remapping, check that the MMAP entries corresponding to the kernel
// have zero file offsets.
void CheckKernelHasZeroPgoff(const PerfReader &reader) {
  for (const auto &event : reader.events()) {
    if ((event.header().type() == PERF_RECORD_MMAP ||
         event.header().type() == PERF_RECORD_MMAP2) &&
        event.mmap_event().filename().find("kernel.kallsyms") !=
            std::string::npos) {
      EXPECT_EQ(0, event.mmap_event().pgoff())
          << "remapped kernel offset must be zero";
    }
  }
}

}  // namespace

TEST(PerfParserTest, TestDSOAndOffsetConstructor) {
  // DSOAndOffset contains a pointer to a dso info struct. Make sure this is
  // initialized in a way such that DSOAndOffset::dso_name() executes without
  // segfault and returns an empty string.
  ParsedEvent::DSOAndOffset dso_and_offset;
  EXPECT_TRUE(dso_and_offset.dso_name().empty());
}

class PerfDataFiles : public ::testing::TestWithParam<const char *> {};
class PerfPipedDataFiles : public ::testing::TestWithParam<const char *> {};

TEST_P(PerfDataFiles, NormalPerfData) {
  ScopedTempDir output_dir;
  ASSERT_FALSE(output_dir.path().empty());
  std::string output_path = output_dir.path();

  int seed = 0;
  std::string test_file = GetParam();
  std::string input_perf_data = GetTestInputFilePath(test_file);
  LOG(INFO) << "Testing " << input_perf_data;

  PerfReader reader;
  ASSERT_TRUE(reader.ReadFile(input_perf_data));

  // Test the PerfReader stage of the processing before continuing.
  std::string pr_output_perf_data = output_path + test_file + ".pr.out";
  std::string pr_baseline_filename = test_file + ".io.out";
  ASSERT_TRUE(reader.WriteFile(pr_output_perf_data));
  std::string difference;
  EXPECT_TRUE(CheckPerfDataAgainstBaseline(pr_output_perf_data,
                                           pr_baseline_filename, &difference))
      << difference;

  // Run it through PerfParser.
  PerfParserOptions options = GetTestOptions();
  options.sort_events_by_time = true;
  PerfParser parser(&reader, options);
  ASSERT_TRUE(parser.ParseRawEvents());

  CHECK_GT(parser.parsed_events().size(), 0U);
  CheckChronologicalOrderOfEvents(reader);

  // Check perf event stats.
  const PerfEventStats &stats = parser.stats();
  EXPECT_GT(stats.num_sample_events, 0U);
  EXPECT_GT(stats.num_mmap_events, 0U);
  EXPECT_GT(stats.num_sample_events_mapped, 0U);
  EXPECT_FALSE(stats.did_remap);

  std::string parsed_perf_data = output_path + test_file + ".parse.out";
  ASSERT_TRUE(reader.WriteFile(parsed_perf_data));

  EXPECT_TRUE(CheckPerfDataAgainstBaseline(parsed_perf_data, "", &difference))
      << difference;
  EXPECT_TRUE(ComparePerfBuildIDLists(input_perf_data, parsed_perf_data));

  // Run the event parsing again, this time with remapping.
  options = PerfParserOptions();
  options.do_remap = true;
  parser.set_options(options);
  ASSERT_TRUE(parser.ParseRawEvents());

  // Check perf event stats.
  EXPECT_GT(stats.num_sample_events, 0U);
  EXPECT_GT(stats.num_mmap_events, 0U);
  EXPECT_GT(stats.num_sample_events_mapped, 0U);
  EXPECT_TRUE(stats.did_remap);

  // Kernel MMAP entries should have zero file offsets when remapped.
  CheckKernelHasZeroPgoff(reader);

  // Remapped addresses should not match the original addresses.
  std::string remapped_perf_data = output_path + test_file + ".parse.remap.out";
  ASSERT_TRUE(reader.WriteFile(remapped_perf_data));
  EXPECT_TRUE(CheckPerfDataAgainstBaseline(remapped_perf_data, "", &difference))
      << difference;

  // Remapping again should produce the same addresses. Disable mappings
  // combining on second remapping, because the first remapping step made all
  // mapping ranges contiguous.
  LOG(INFO) << "Reading in remapped data: " << remapped_perf_data;
  PerfReader remap_reader;
  ASSERT_TRUE(remap_reader.ReadFile(remapped_perf_data));
  options.combine_mappings = false;

  PerfParser remap_parser(&remap_reader, options);
  ASSERT_TRUE(remap_parser.ParseRawEvents());

  const PerfEventStats &remap_stats = remap_parser.stats();
  EXPECT_GT(remap_stats.num_sample_events, 0U);
  EXPECT_GT(remap_stats.num_mmap_events, 0U);
  EXPECT_GT(remap_stats.num_sample_events_mapped, 0U);
  EXPECT_TRUE(remap_stats.did_remap);

  ASSERT_EQ(stats.num_sample_events, remap_stats.num_sample_events);
  ASSERT_EQ(stats.num_mmap_events, remap_stats.num_mmap_events);
  ASSERT_EQ(stats.num_sample_events_mapped,
            remap_stats.num_sample_events_mapped);

  std::string remapped_perf_data2 =
      output_path + test_file + ".parse.remap2.out";
  ASSERT_TRUE(remap_reader.WriteFile(remapped_perf_data2));
  EXPECT_TRUE(
      CheckPerfDataAgainstBaseline(remapped_perf_data2, "", &difference))
      << difference;

  // No need to call CheckPerfDataAgainstBaseline again. Just compare
  // ParsedEvents.
  const auto &parser_events = parser.parsed_events();
  const auto &remap_parser_events = remap_parser.parsed_events();
  EXPECT_EQ(parser_events.size(), remap_parser_events.size());
  EXPECT_TRUE(std::equal(parser_events.begin(), parser_events.end(),
                         remap_parser_events.begin()));
  EXPECT_TRUE(ComparePerfBuildIDLists(remapped_perf_data, remapped_perf_data2));

  // This must be called when |reader| is no longer going to be used, as it
  // modifies the contents of |reader|.
  CheckFilenameAndBuildIDMethods(&reader, output_path + test_file, seed);
  ++seed;
}

TEST_P(PerfPipedDataFiles, PipedModePerfData) {
  ScopedTempDir output_dir;
  ASSERT_FALSE(output_dir.path().empty());
  std::string output_path = output_dir.path();

  int seed = 0;
  const std::string test_file = GetParam();
  std::string input_perf_data = GetTestInputFilePath(test_file);
  LOG(INFO) << "Testing " << input_perf_data;
  std::string output_perf_data = output_path + test_file + ".pr.out";

  PerfReader reader;
  ASSERT_TRUE(reader.ReadFile(input_perf_data));

  // Check results from the PerfReader stage.
  ASSERT_TRUE(reader.WriteFile(output_perf_data));
  std::string difference;
  EXPECT_TRUE(CheckPerfDataAgainstBaseline(output_perf_data, "", &difference))
      << difference;

  PerfParserOptions options = GetTestOptions();
  options.do_remap = true;
  options.sort_events_by_time = true;
  PerfParser parser(&reader, options);
  ASSERT_TRUE(parser.ParseRawEvents());

  EXPECT_GT(parser.stats().num_sample_events, 0U);
  EXPECT_GT(parser.stats().num_mmap_events, 0U);
  EXPECT_GT(parser.stats().num_sample_events_mapped, 0U);
  EXPECT_TRUE(parser.stats().did_remap);

  // Kernel MMAP entries should have zero file offsets when remapped.
  CheckKernelHasZeroPgoff(reader);

  // This must be called when |reader| is no longer going to be used, as it
  // modifies the contents of |reader|.
  CheckFilenameAndBuildIDMethods(&reader, output_path + test_file, seed);
  ++seed;
}

INSTANTIATE_TEST_SUITE_P(
    PerfParserTest, PerfDataFiles,
    ::testing::ValuesIn(perf_test_files::GetPerfDataFiles()));
INSTANTIATE_TEST_SUITE_P(
    PerfParserTest, PerfPipedDataFiles,
    ::testing::ValuesIn(perf_test_files::GetPerfPipedDataFiles()));

TEST(PerfParserTest, MapsSampleEventIp) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP
  testing::ExampleMmapEvent(1001, 0x1c1000, 0x1000, 0, "/usr/lib/foo.so",
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 0
  // becomes: 0x0000, 0x1000, 0
  testing::ExampleMmapEvent(1001, 0x1c3000, 0x2000, 0x2000, "/usr/lib/bar.so",
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 1
  // becomes: 0x1000, 0x2000, 0x2000

  // PERF_RECORD_MMAP2
  testing::ExampleMmap2Event(1002, 0x2c1000, 0x2000, 0, "/usr/lib/baz.so",
                             testing::SampleInfo().Tid(1002))
      .WriteTo(&input);  // 2
  // becomes: 0x0000, 0x2000, 0
  testing::ExampleMmap2Event(1002, 0x2c3000, 0x1000, 0x3000, "/usr/lib/xyz.so",
                             testing::SampleInfo().Tid(1002))
      .WriteTo(&input);  // 3
  // becomes: 0x1000, 0x1000, 0x3000

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001))
      .WriteTo(&input);  // 4
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c100a).Tid(1001))
      .WriteTo(&input);  // 5
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c3fff).Tid(1001))
      .WriteTo(&input);  // 6
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c2bad).Tid(1001))
      .WriteTo(&input);  // 7 (not mapped)
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000002c100a).Tid(1002))
      .WriteTo(&input);  // 8
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000002c5bad).Tid(1002))
      .WriteTo(&input);  // 9 (not mapped)
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000002c300b).Tid(1002))
      .WriteTo(&input);  // 10

  // not mapped yet:
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000002c400b).Tid(1002))
      .WriteTo(&input);  // 11
  testing::ExampleMmap2Event(1002, 0x2c4000, 0x1000, 0, "/usr/lib/new.so",
                             testing::SampleInfo().Tid(1002))
      .WriteTo(&input);  // 12
  // becomes: 0x2000, 0x1000, 0
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000002c400b).Tid(1002))
      .WriteTo(&input);  // 13

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(5, parser.stats().num_mmap_events);
  EXPECT_EQ(9, parser.stats().num_sample_events);
  EXPECT_EQ(6, parser.stats().num_sample_events_mapped);
  EXPECT_EQ(0, parser.stats().num_data_sample_events);
  EXPECT_EQ(0, parser.stats().num_data_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(14, events.size());

  // MMAPs

  EXPECT_EQ(PERF_RECORD_MMAP, events[0].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/foo.so", events[0].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x0000, events[0].event_ptr->mmap_event().start());
  EXPECT_EQ(0x1000, events[0].event_ptr->mmap_event().len());
  EXPECT_EQ(0, events[0].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_MMAP, events[1].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/bar.so", events[1].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x1000, events[1].event_ptr->mmap_event().start());
  EXPECT_EQ(0x2000, events[1].event_ptr->mmap_event().len());
  EXPECT_EQ(0x2000, events[1].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_MMAP2, events[2].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/baz.so", events[2].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x0000, events[2].event_ptr->mmap_event().start());
  EXPECT_EQ(0x2000, events[2].event_ptr->mmap_event().len());
  EXPECT_EQ(0, events[2].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_MMAP2, events[3].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/xyz.so", events[3].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x2000, events[3].event_ptr->mmap_event().start());
  EXPECT_EQ(0x1000, events[3].event_ptr->mmap_event().len());
  EXPECT_EQ(0x3000, events[3].event_ptr->mmap_event().pgoff());

  // SAMPLEs

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[4].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/foo.so", events[4].dso_and_offset.dso_name());
  EXPECT_EQ(0x0, events[4].dso_and_offset.offset());
  EXPECT_EQ(0x0, events[4].event_ptr->sample_event().ip());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[5].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/foo.so", events[5].dso_and_offset.dso_name());
  EXPECT_EQ(0xa, events[5].dso_and_offset.offset());
  EXPECT_EQ(0xa, events[5].event_ptr->sample_event().ip());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[6].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/bar.so", events[6].dso_and_offset.dso_name());
  EXPECT_EQ(0x2fff, events[6].dso_and_offset.offset());
  EXPECT_EQ(0x1fff, events[6].event_ptr->sample_event().ip());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[7].event_ptr->header().type());
  EXPECT_EQ(0x80000000001c2bad, events[7].event_ptr->sample_event().ip());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[8].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/baz.so", events[8].dso_and_offset.dso_name());
  EXPECT_EQ(0xa, events[8].dso_and_offset.offset());
  EXPECT_EQ(0xa, events[8].event_ptr->sample_event().ip());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[9].event_ptr->header().type());
  EXPECT_EQ(0x80000000002c5bad, events[9].event_ptr->sample_event().ip());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[10].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/xyz.so", events[10].dso_and_offset.dso_name());
  EXPECT_EQ(0x300b, events[10].dso_and_offset.offset());
  EXPECT_EQ(0x200b, events[10].event_ptr->sample_event().ip());

  // not mapped yet:
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[11].event_ptr->header().type());
  EXPECT_EQ(0x80000000002c400b, events[11].event_ptr->sample_event().ip());

  EXPECT_EQ(PERF_RECORD_MMAP2, events[12].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/new.so", events[12].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x3000, events[12].event_ptr->mmap_event().start());
  EXPECT_EQ(0x1000, events[12].event_ptr->mmap_event().len());
  EXPECT_EQ(0, events[12].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[13].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/new.so", events[13].dso_and_offset.dso_name());
  EXPECT_EQ(0xb, events[13].dso_and_offset.offset());
  EXPECT_EQ(0x300b, events[13].event_ptr->sample_event().ip());
}

TEST(PerfParserTest, MapsSampleEventAddr) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(
      PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR,
      true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP
  testing::ExampleMmapEvent(1001, 0x1c1000, 0x1000, 0, "/usr/lib/foo.so",
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 0
  // becomes: 0x0000, 0x1000, 0
  testing::ExampleMmapEvent(1001, 0x1c3000, 0x2000, 0x2000, "/usr/lib/bar.so",
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 1
  // becomes: 0x1000, 0x2000, 0x2000
  testing::ExampleMmapEvent(1001, 0x1c6000, 0x1000, 0x4000, "/usr/lib/bar.so",
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 2
  // becomes: 0x3000, 0x1000, 0x4000

  // PERF_RECORD_MMAP2
  testing::ExampleMmap2Event(1002, 0x2c1000, 0x2000, 0, "/usr/lib/baz.so",
                             testing::SampleInfo().Tid(1002))
      .WriteTo(&input);  // 3
  // becomes: 0x0000, 0x2000, 0
  testing::ExampleMmap2Event(1002, 0x2c4000, 0x2000, 0x2000, "/usr/lib/baz.so",
                             testing::SampleInfo().Tid(1002))
      .WriteTo(&input);  // 4
  // becomes: 0x2000, 0x2000, 0x2000
  testing::ExampleMmap2Event(1002, 0x2d1000, 0x1000, 0x3000, "/usr/lib/xyz.so",
                             testing::SampleInfo().Tid(1002))
      .WriteTo(&input);  // 5
  // becomes: 0x4000, 0x1000, 0x3000

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(testing::SampleInfo()
                                      .Ip(0x00000000001c1000)
                                      .Tid(1001)
                                      .Addr(0x00000000001c6100))
      .WriteTo(&input);  // 6
  testing::ExamplePerfSampleEvent(testing::SampleInfo()
                                      .Ip(0x00000000001c3fff)
                                      .Tid(1001)
                                      .Addr(0x00000000001c6820))
      .WriteTo(&input);  // 7
  testing::ExamplePerfSampleEvent(testing::SampleInfo()
                                      .Ip(0x00000000001c3b00)
                                      .Tid(1001)
                                      .Addr(0x00000000001c7bad))
      .WriteTo(&input);  // 8 (addr not mapped)
  testing::ExamplePerfSampleEvent(testing::SampleInfo()
                                      .Ip(0x00000000002c100a)
                                      .Tid(1002)
                                      .Addr(0x00000000002c4100))
      .WriteTo(&input);  // 9
  testing::ExamplePerfSampleEvent(testing::SampleInfo()
                                      .Ip(0x00000000002c2800)
                                      .Tid(1002)
                                      .Addr(0x00000000002c6bad))
      .WriteTo(&input);  // 10 (addr not mapped)
  // Zero data addresses are not counted as samples with data.
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000002d100b).Tid(1002).Addr(0))
      .WriteTo(&input);  // 11

  // not mapped yet:
  testing::ExamplePerfSampleEvent(testing::SampleInfo()
                                      .Ip(0x00000000002d1200)
                                      .Tid(1002)
                                      .Addr(0x00000000002d3300))
      .WriteTo(&input);  // 12
  testing::ExampleMmap2Event(1002, 0x2d3000, 0x1000, 0x4000, "/usr/lib/xyz.so",
                             testing::SampleInfo().Tid(1002))
      .WriteTo(&input);  // 13
  // becomes: 0x5000, 0x1000, 0x4000
  testing::ExamplePerfSampleEvent(testing::SampleInfo()
                                      .Ip(0x00000000002d1200)
                                      .Tid(1002)
                                      .Addr(0x00000000002d3300))
      .WriteTo(&input);  // 14

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(7, parser.stats().num_mmap_events);
  EXPECT_EQ(8, parser.stats().num_sample_events);
  EXPECT_EQ(8, parser.stats().num_sample_events_mapped);
  EXPECT_EQ(7, parser.stats().num_data_sample_events);
  EXPECT_EQ(4, parser.stats().num_data_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(15, events.size());

  // MMAPs

  EXPECT_EQ(PERF_RECORD_MMAP, events[0].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/foo.so", events[0].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x0000, events[0].event_ptr->mmap_event().start());
  EXPECT_EQ(0x1000, events[0].event_ptr->mmap_event().len());
  EXPECT_EQ(0, events[0].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_MMAP, events[1].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/bar.so", events[1].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x1000, events[1].event_ptr->mmap_event().start());
  EXPECT_EQ(0x2000, events[1].event_ptr->mmap_event().len());
  EXPECT_EQ(0x2000, events[1].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_MMAP, events[2].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/bar.so", events[2].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x3000, events[2].event_ptr->mmap_event().start());
  EXPECT_EQ(0x1000, events[2].event_ptr->mmap_event().len());
  EXPECT_EQ(0x4000, events[2].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_MMAP2, events[3].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/baz.so", events[3].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x0000, events[3].event_ptr->mmap_event().start());
  EXPECT_EQ(0x2000, events[3].event_ptr->mmap_event().len());
  EXPECT_EQ(0, events[3].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_MMAP2, events[4].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/baz.so", events[4].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x2000, events[4].event_ptr->mmap_event().start());
  EXPECT_EQ(0x2000, events[4].event_ptr->mmap_event().len());
  EXPECT_EQ(0x2000, events[4].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_MMAP2, events[5].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/xyz.so", events[5].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x4000, events[5].event_ptr->mmap_event().start());
  EXPECT_EQ(0x1000, events[5].event_ptr->mmap_event().len());
  EXPECT_EQ(0x3000, events[5].event_ptr->mmap_event().pgoff());

  // SAMPLEs

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[6].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/foo.so", events[6].dso_and_offset.dso_name());
  EXPECT_EQ(0x0, events[6].dso_and_offset.offset());
  EXPECT_EQ(0x0, events[6].event_ptr->sample_event().ip());
  EXPECT_EQ("/usr/lib/bar.so", events[6].data_dso_and_offset.dso_name());
  EXPECT_EQ(0x4100, events[6].data_dso_and_offset.offset());
  EXPECT_EQ(0x3100, events[6].event_ptr->sample_event().addr());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[7].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/bar.so", events[7].dso_and_offset.dso_name());
  EXPECT_EQ(0x2fff, events[7].dso_and_offset.offset());
  EXPECT_EQ(0x1fff, events[7].event_ptr->sample_event().ip());
  EXPECT_EQ("/usr/lib/bar.so", events[7].data_dso_and_offset.dso_name());
  EXPECT_EQ(0x4820, events[7].data_dso_and_offset.offset());
  EXPECT_EQ(0x3820, events[7].event_ptr->sample_event().addr());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[8].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/bar.so", events[8].dso_and_offset.dso_name());
  EXPECT_EQ(0x2b00, events[8].dso_and_offset.offset());
  EXPECT_EQ(0x1b00, events[8].event_ptr->sample_event().ip());
  // addr field not mapped.
  EXPECT_EQ(0x80000000001c7bad, events[8].event_ptr->sample_event().addr());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[9].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/baz.so", events[9].dso_and_offset.dso_name());
  EXPECT_EQ(0xa, events[9].dso_and_offset.offset());
  EXPECT_EQ(0xa, events[9].event_ptr->sample_event().ip());
  EXPECT_EQ("/usr/lib/baz.so", events[9].data_dso_and_offset.dso_name());
  EXPECT_EQ(0x2100, events[9].data_dso_and_offset.offset());
  EXPECT_EQ(0x2100, events[9].event_ptr->sample_event().addr());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[10].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/baz.so", events[10].dso_and_offset.dso_name());
  EXPECT_EQ(0x1800, events[10].dso_and_offset.offset());
  EXPECT_EQ(0x1800, events[10].event_ptr->sample_event().ip());
  // addr field not mapped.
  EXPECT_EQ(0x80000000002c6bad, events[10].event_ptr->sample_event().addr());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[11].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/xyz.so", events[11].dso_and_offset.dso_name());
  EXPECT_EQ(0x300b, events[11].dso_and_offset.offset());
  EXPECT_EQ(0x400b, events[11].event_ptr->sample_event().ip());
  EXPECT_EQ(0, events[11].event_ptr->sample_event().addr());

  // not mapped yet:
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[12].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/xyz.so", events[12].dso_and_offset.dso_name());
  EXPECT_EQ(0x3200, events[12].dso_and_offset.offset());
  EXPECT_EQ(0x4200, events[12].event_ptr->sample_event().ip());
  EXPECT_EQ(0x80000000002d3300, events[12].event_ptr->sample_event().addr());

  EXPECT_EQ(PERF_RECORD_MMAP2, events[13].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/xyz.so", events[13].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x5000, events[13].event_ptr->mmap_event().start());
  EXPECT_EQ(0x1000, events[13].event_ptr->mmap_event().len());
  EXPECT_EQ(0x4000, events[13].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[14].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/xyz.so", events[14].dso_and_offset.dso_name());
  EXPECT_EQ(0x3200, events[14].dso_and_offset.offset());
  EXPECT_EQ(0x4200, events[14].event_ptr->sample_event().ip());
  EXPECT_EQ("/usr/lib/xyz.so", events[14].data_dso_and_offset.dso_name());
  EXPECT_EQ(0x4300, events[14].data_dso_and_offset.offset());
  EXPECT_EQ(0x5300, events[14].event_ptr->sample_event().addr());
}

TEST(PerfParserTest, ProfileWithNoKernelMappingHasCorrectOffsets) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP
  testing::ExampleMmapEvent(1001, 0x1c0000, 0x10000, 0x20000,
                            "/bin/some_binary",
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 0
  testing::ExampleMmapEvent(1001, 0x200000, 0x1000, 0x2000, "/usr/lib/bar.so",
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 1

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001))
      .WriteTo(&input);  // 2
  // dso_and_offset.offset should be 0x21000

  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x0000000000200800).Tid(1001))
      .WriteTo(&input);  // 3
  // dso_and_offset.offset should be 0x2800

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(2, parser.stats().num_mmap_events);
  EXPECT_EQ(2, parser.stats().num_sample_events);
  EXPECT_EQ(2, parser.stats().num_sample_events_mapped);
  EXPECT_EQ(0, parser.stats().num_data_sample_events);
  EXPECT_EQ(0, parser.stats().num_data_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(4, events.size());

  // MMAPs

  EXPECT_EQ(PERF_RECORD_MMAP, events[0].event_ptr->header().type());
  EXPECT_EQ("/bin/some_binary", events[0].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x20000, events[0].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_MMAP, events[1].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/bar.so", events[1].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x2000, events[1].event_ptr->mmap_event().pgoff());

  // SAMPLESs

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[2].event_ptr->header().type());
  EXPECT_EQ("/bin/some_binary", events[2].dso_and_offset.dso_name());
  EXPECT_EQ(0x21000, events[2].dso_and_offset.offset());

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[3].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/bar.so", events[3].dso_and_offset.dso_name());
  EXPECT_EQ(0x2800, events[3].dso_and_offset.offset());
}

TEST(PerfParserTest, ContextSwitchEvents) {
  std::stringstream input;
  // PERF_RECORD_SWITCH
  testing::ExampleContextSwitchEvent context_switch_event_1(
      true, testing::SampleInfo().Tid(1001));  // 0
  testing::ExampleContextSwitchEvent context_switch_event_2(
      false, testing::SampleInfo().Tid(1001));  // 1

  // PERF_RECORD_SWITCH_CPU_WIDE
  testing::ExampleContextSwitchEvent context_switch_event_3(
      true, 5656, 5656, testing::SampleInfo().Tid(1001));  // 2
  testing::ExampleContextSwitchEvent context_switch_event_4(
      false, 1001, 1001, testing::SampleInfo().Tid(5656));  // 3

  size_t data_size =
      context_switch_event_1.GetSize() + context_switch_event_2.GetSize() +
      context_switch_event_3.GetSize() + context_switch_event_4.GetSize();

  // header
  testing::ExamplePerfDataFileHeader file_header(0);
  file_header.WithAttrCount(1).WithDataSize(data_size).WriteTo(&input);

  // attrs
  ASSERT_EQ(file_header.header().attrs.offset, static_cast<u64>(input.tellp()));
  testing::ExamplePerfFileAttr_Hardware(PERF_SAMPLE_TID, true /*sample_id_all*/)
      .WriteTo(&input);

  // data
  ASSERT_EQ(file_header.header().data.offset, static_cast<u64>(input.tellp()));
  context_switch_event_1.WriteTo(&input);
  context_switch_event_2.WriteTo(&input);
  context_switch_event_3.WriteTo(&input);
  context_switch_event_4.WriteTo(&input);
  ASSERT_EQ(file_header.header().data.offset + data_size,
            static_cast<u64>(input.tellp()));

  // no metadata

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(4, events.size());

  EXPECT_EQ(PERF_RECORD_SWITCH, events[0].event_ptr->header().type());
  EXPECT_EQ(PERF_RECORD_MISC_SWITCH_OUT, events[0].event_ptr->header().misc());
  EXPECT_EQ(1001,
            events[0].event_ptr->context_switch_event().sample_info().tid());

  EXPECT_EQ(PERF_RECORD_SWITCH, events[1].event_ptr->header().type());
  EXPECT_EQ(0, events[1].event_ptr->header().misc());
  EXPECT_EQ(1001,
            events[0].event_ptr->context_switch_event().sample_info().tid());

  EXPECT_EQ(PERF_RECORD_SWITCH_CPU_WIDE, events[2].event_ptr->header().type());
  EXPECT_EQ(PERF_RECORD_MISC_SWITCH_OUT, events[2].event_ptr->header().misc());
  EXPECT_EQ(5656, events[2].event_ptr->context_switch_event().next_prev_pid());
  EXPECT_EQ(5656, events[2].event_ptr->context_switch_event().next_prev_tid());
  EXPECT_EQ(1001,
            events[2].event_ptr->context_switch_event().sample_info().pid());
  EXPECT_EQ(1001,
            events[2].event_ptr->context_switch_event().sample_info().tid());

  EXPECT_EQ(PERF_RECORD_SWITCH_CPU_WIDE, events[3].event_ptr->header().type());
  EXPECT_EQ(0, events[3].event_ptr->header().misc());
  EXPECT_EQ(1001, events[3].event_ptr->context_switch_event().next_prev_pid());
  EXPECT_EQ(1001, events[3].event_ptr->context_switch_event().next_prev_tid());
  EXPECT_EQ(5656,
            events[3].event_ptr->context_switch_event().sample_info().pid());
  EXPECT_EQ(5656,
            events[3].event_ptr->context_switch_event().sample_info().tid());
}

TEST(PerfParserTest, PipedContextSwitchEvents) {
  std::stringstream input;
  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data
  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_SWITCH
  testing::ExampleContextSwitchEvent(true, testing::SampleInfo().Tid(1001))
      .WriteTo(&input);
  testing::ExampleContextSwitchEvent(false, testing::SampleInfo().Tid(1001))
      .WriteTo(&input);

  // PERF_RECORD_SWITCH_CPU_WIDE
  testing::ExampleContextSwitchEvent(true, 5656, 5656,
                                     testing::SampleInfo().Tid(1001))
      .WriteTo(&input);
  testing::ExampleContextSwitchEvent(false, 1001, 1001,
                                     testing::SampleInfo().Tid(5656))
      .WriteTo(&input);

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(4, events.size());

  EXPECT_EQ(PERF_RECORD_SWITCH, events[0].event_ptr->header().type());
  EXPECT_EQ(PERF_RECORD_MISC_SWITCH_OUT, events[0].event_ptr->header().misc());
  EXPECT_EQ(1001,
            events[0].event_ptr->context_switch_event().sample_info().pid());
  EXPECT_EQ(1001,
            events[0].event_ptr->context_switch_event().sample_info().tid());

  EXPECT_EQ(PERF_RECORD_SWITCH, events[1].event_ptr->header().type());
  EXPECT_EQ(0, events[1].event_ptr->header().misc());
  EXPECT_EQ(1001,
            events[0].event_ptr->context_switch_event().sample_info().pid());
  EXPECT_EQ(1001,
            events[0].event_ptr->context_switch_event().sample_info().tid());

  EXPECT_EQ(PERF_RECORD_SWITCH_CPU_WIDE, events[2].event_ptr->header().type());
  EXPECT_EQ(PERF_RECORD_MISC_SWITCH_OUT, events[2].event_ptr->header().misc());
  EXPECT_EQ(5656, events[2].event_ptr->context_switch_event().next_prev_pid());
  EXPECT_EQ(5656, events[2].event_ptr->context_switch_event().next_prev_tid());
  EXPECT_EQ(1001,
            events[2].event_ptr->context_switch_event().sample_info().pid());
  EXPECT_EQ(1001,
            events[2].event_ptr->context_switch_event().sample_info().tid());

  EXPECT_EQ(PERF_RECORD_SWITCH_CPU_WIDE, events[3].event_ptr->header().type());
  EXPECT_EQ(0, events[3].event_ptr->header().misc());
  EXPECT_EQ(1001, events[3].event_ptr->context_switch_event().next_prev_pid());
  EXPECT_EQ(1001, events[3].event_ptr->context_switch_event().next_prev_tid());
  EXPECT_EQ(5656,
            events[3].event_ptr->context_switch_event().sample_info().pid());
  EXPECT_EQ(5656,
            events[3].event_ptr->context_switch_event().sample_info().tid());
}

TEST(PerfParserTest, NamespacesEvents) {
  std::stringstream input;
  std::vector<struct perf_ns_link_info> link_infos;
  struct perf_ns_link_info link_info1 = {
      .dev = 1234,
      .ino = 5678,
  };
  struct perf_ns_link_info link_info2 = {
      .dev = 223344,
      .ino = 556677,
  };

  link_infos.push_back(link_info1);
  link_infos.push_back(link_info2);

  // PERF_RECORD_NAMESPACES
  testing::ExampleNamespacesEvent namespaces_event(
      5656, 5656, link_infos, testing::SampleInfo().Tid(1001));

  size_t data_size = namespaces_event.GetSize();

  // header
  testing::ExamplePerfDataFileHeader file_header(0);
  file_header.WithAttrCount(1).WithDataSize(data_size).WriteTo(&input);

  // attrs
  ASSERT_EQ(file_header.header().attrs.offset, static_cast<u64>(input.tellp()));
  testing::ExamplePerfFileAttr_Hardware(PERF_SAMPLE_TID, true /*sample_id_all*/)
      .WriteTo(&input);

  // data
  ASSERT_EQ(file_header.header().data.offset, static_cast<u64>(input.tellp()));
  namespaces_event.WriteTo(&input);
  ASSERT_EQ(file_header.header().data.offset + data_size,
            static_cast<u64>(input.tellp()));

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_NAMESPACES, events[0].event_ptr->header().type());
  EXPECT_EQ(5656, events[0].event_ptr->namespaces_event().pid());
  EXPECT_EQ(5656, events[0].event_ptr->namespaces_event().tid());
  EXPECT_EQ(2, events[0].event_ptr->namespaces_event().link_info_size());
  EXPECT_EQ(1234, events[0].event_ptr->namespaces_event().link_info(0).dev());
  EXPECT_EQ(5678, events[0].event_ptr->namespaces_event().link_info(0).ino());
  EXPECT_EQ(223344, events[0].event_ptr->namespaces_event().link_info(1).dev());
  EXPECT_EQ(556677, events[0].event_ptr->namespaces_event().link_info(1).ino());
}

TEST(PerfParserTest, PipedNamespacesEvents) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data
  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  std::vector<struct perf_ns_link_info> link_infos;
  struct perf_ns_link_info link_info1 = {
      .dev = 1234,
      .ino = 5678,
  };
  struct perf_ns_link_info link_info2 = {
      .dev = 223344,
      .ino = 556677,
  };

  link_infos.push_back(link_info1);
  link_infos.push_back(link_info2);

  // PERF_RECORD_NAMESPACES
  testing::ExampleNamespacesEvent(5656, 5656, link_infos,
                                  testing::SampleInfo().Tid(1001))
      .WriteTo(&input);

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_NAMESPACES, events[0].event_ptr->header().type());
  EXPECT_EQ(5656, events[0].event_ptr->namespaces_event().pid());
  EXPECT_EQ(5656, events[0].event_ptr->namespaces_event().tid());
  EXPECT_EQ(2, events[0].event_ptr->namespaces_event().link_info_size());
  EXPECT_EQ(1234, events[0].event_ptr->namespaces_event().link_info(0).dev());
  EXPECT_EQ(5678, events[0].event_ptr->namespaces_event().link_info(0).ino());
  EXPECT_EQ(223344, events[0].event_ptr->namespaces_event().link_info(1).dev());
  EXPECT_EQ(556677, events[0].event_ptr->namespaces_event().link_info(1).ino());
}

TEST(PerfParserTest, AuxtraceInfoEvents) {
  std::stringstream input;
  std::vector<u64> priv;

  priv.push_back(67548);
  priv.push_back(320945);

  // PERF_RECORD_AUXTRACE_INFO
  testing::ExampleAuxtraceInfoEvent auxtrace_info_event(1, priv);

  size_t data_size = auxtrace_info_event.GetSize();

  // header
  testing::ExamplePerfDataFileHeader file_header(0);
  file_header.WithAttrCount(1).WithDataSize(data_size).WriteTo(&input);

  // attrs
  ASSERT_EQ(file_header.header().attrs.offset, static_cast<u64>(input.tellp()));
  testing::ExamplePerfFileAttr_Hardware(PERF_SAMPLE_TID, /*sample_id_all=*/true)
      .WriteTo(&input);

  // data
  ASSERT_EQ(file_header.header().data.offset, static_cast<u64>(input.tellp()));
  auxtrace_info_event.WriteTo(&input);
  ASSERT_EQ(file_header.header().data.offset + data_size,
            static_cast<u64>(input.tellp()));

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_AUXTRACE_INFO, events[0].event_ptr->header().type());
  EXPECT_EQ(1, events[0].event_ptr->auxtrace_info_event().type());
  EXPECT_EQ(2, events[0]
                   .event_ptr->auxtrace_info_event()
                   .unparsed_binary_blob_priv_data_size());
  EXPECT_EQ(
      67548,
      events[0].event_ptr->auxtrace_info_event().unparsed_binary_blob_priv_data(
          0));
  EXPECT_EQ(
      320945,
      events[0].event_ptr->auxtrace_info_event().unparsed_binary_blob_priv_data(
          1));
}

TEST(PerfParserTest, PipedAuxtraceInfoEvents) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data
  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_TID,
                                              /*sample_id_all=*/true)
      .WriteTo(&input);

  std::vector<u64> priv;

  priv.push_back(67548);
  priv.push_back(320945);

  // PERF_RECORD_AUXTRACE_INFO
  testing::ExampleAuxtraceInfoEvent(1, priv).WriteTo(&input);

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_AUXTRACE_INFO, events[0].event_ptr->header().type());
  EXPECT_EQ(1, events[0].event_ptr->auxtrace_info_event().type());
  EXPECT_EQ(2, events[0]
                   .event_ptr->auxtrace_info_event()
                   .unparsed_binary_blob_priv_data_size());
  EXPECT_EQ(
      67548,
      events[0].event_ptr->auxtrace_info_event().unparsed_binary_blob_priv_data(
          0));
  EXPECT_EQ(
      320945,
      events[0].event_ptr->auxtrace_info_event().unparsed_binary_blob_priv_data(
          1));
}

TEST(PerfSerializerTest, AuxtraceErrorEvents) {
  std::stringstream input;
  std::string auxtrace_error_msg = "AUXTRACE ERROR MESSAGE.";

  // PERF_RECORD_AUXTRACE_INFO
  testing::ExampleAuxtraceErrorEvent auxtrace_error_event(
      1, 9876, 10, 4365, 4365, 86758, auxtrace_error_msg);
  size_t data_size = auxtrace_error_event.GetSize();

  // header
  testing::ExamplePerfDataFileHeader file_header(0);
  file_header.WithAttrCount(1).WithDataSize(data_size).WriteTo(&input);

  // attrs
  ASSERT_EQ(file_header.header().attrs.offset, static_cast<u64>(input.tellp()));
  testing::ExamplePerfFileAttr_Hardware(PERF_SAMPLE_TID, /*sample_id_all=*/true)
      .WriteTo(&input);

  // data
  ASSERT_EQ(file_header.header().data.offset, static_cast<u64>(input.tellp()));
  auxtrace_error_event.WriteTo(&input);
  ASSERT_EQ(file_header.header().data.offset + data_size,
            static_cast<u64>(input.tellp()));

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_AUXTRACE_ERROR, events[0].event_ptr->header().type());
  EXPECT_EQ(1, events[0].event_ptr->auxtrace_error_event().type());
  EXPECT_EQ(9876, events[0].event_ptr->auxtrace_error_event().code());
  EXPECT_EQ(10, events[0].event_ptr->auxtrace_error_event().cpu());
  EXPECT_EQ(4365, events[0].event_ptr->auxtrace_error_event().pid());
  EXPECT_EQ(4365, events[0].event_ptr->auxtrace_error_event().tid());
  EXPECT_EQ(86758, events[0].event_ptr->auxtrace_error_event().ip());
  EXPECT_EQ(auxtrace_error_msg,
            events[0].event_ptr->auxtrace_error_event().msg());
  EXPECT_EQ(Md5Prefix(auxtrace_error_msg),
            events[0].event_ptr->auxtrace_error_event().msg_md5_prefix());
}

TEST(PerfSerializerTest, PipedAuxtraceErrorEvents) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data
  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_TID,
                                              /*sample_id_all=*/true)
      .WriteTo(&input);

  std::string auxtrace_error_msg = "AUXTRACE ERROR MESSAGE.";

  // PERF_RECORD_AUXTRACE_ERROR
  testing::ExampleAuxtraceErrorEvent(1, 9876, 10, 4365, 4365, 86758,
                                     auxtrace_error_msg)
      .WriteTo(&input);

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_AUXTRACE_ERROR, events[0].event_ptr->header().type());
  EXPECT_EQ(1, events[0].event_ptr->auxtrace_error_event().type());
  EXPECT_EQ(9876, events[0].event_ptr->auxtrace_error_event().code());
  EXPECT_EQ(10, events[0].event_ptr->auxtrace_error_event().cpu());
  EXPECT_EQ(4365, events[0].event_ptr->auxtrace_error_event().pid());
  EXPECT_EQ(4365, events[0].event_ptr->auxtrace_error_event().tid());
  EXPECT_EQ(86758, events[0].event_ptr->auxtrace_error_event().ip());
  EXPECT_EQ(auxtrace_error_msg,
            events[0].event_ptr->auxtrace_error_event().msg());
  EXPECT_EQ(Md5Prefix(auxtrace_error_msg),
            events[0].event_ptr->auxtrace_error_event().msg_md5_prefix());
}

TEST(PerfSerializerTest, ThreadMapEvents) {
  std::stringstream input;

  // PERF_RECORD_THREAD_MAP
  testing::ExampleThreadMapEvent thread_map_event;
  thread_map_event.WithEntry(1234, "comm1").WithEntry(223344, "comm2");
  size_t data_size = thread_map_event.GetSize();

  // header
  testing::ExamplePerfDataFileHeader file_header(0);
  file_header.WithAttrCount(1).WithDataSize(data_size).WriteTo(&input);

  // attrs
  ASSERT_EQ(file_header.header().attrs.offset, static_cast<u64>(input.tellp()));
  testing::ExamplePerfFileAttr_Hardware(PERF_SAMPLE_TID, /*sample_id_all=*/true)
      .WriteTo(&input);

  // data
  ASSERT_EQ(file_header.header().data.offset, static_cast<u64>(input.tellp()));
  thread_map_event.WriteTo(&input);
  ASSERT_EQ(file_header.header().data.offset + data_size,
            static_cast<u64>(input.tellp()));

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_THREAD_MAP, events[0].event_ptr->header().type());
  EXPECT_EQ(2, events[0].event_ptr->thread_map_event().entries_size());
  EXPECT_EQ(1234, events[0].event_ptr->thread_map_event().entries(0).pid());
  EXPECT_EQ("comm1", events[0].event_ptr->thread_map_event().entries(0).comm());
  EXPECT_EQ(
      Md5Prefix("comm1"),
      events[0].event_ptr->thread_map_event().entries(0).comm_md5_prefix());
  EXPECT_EQ(223344, events[0].event_ptr->thread_map_event().entries(1).pid());
  EXPECT_EQ("comm2", events[0].event_ptr->thread_map_event().entries(1).comm());
  EXPECT_EQ(
      Md5Prefix("comm2"),
      events[0].event_ptr->thread_map_event().entries(1).comm_md5_prefix());
}

TEST(PerfSerializerTest, PipedThreadMapEvents) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data
  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_TID,
                                              /*sample_id_all=*/true)
      .WriteTo(&input);

  // PERF_RECORD_THREAD_MAP
  testing::ExampleThreadMapEvent()
      .WithEntry(1234, "comm1")
      .WithEntry(223344, "comm2")
      .WriteTo(&input);

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_THREAD_MAP, events[0].event_ptr->header().type());
  EXPECT_EQ(2, events[0].event_ptr->thread_map_event().entries_size());
  EXPECT_EQ(1234, events[0].event_ptr->thread_map_event().entries(0).pid());
  EXPECT_EQ("comm1", events[0].event_ptr->thread_map_event().entries(0).comm());
  EXPECT_EQ(
      Md5Prefix("comm1"),
      events[0].event_ptr->thread_map_event().entries(0).comm_md5_prefix());
  EXPECT_EQ(223344, events[0].event_ptr->thread_map_event().entries(1).pid());
  EXPECT_EQ("comm2", events[0].event_ptr->thread_map_event().entries(1).comm());
  EXPECT_EQ(
      Md5Prefix("comm2"),
      events[0].event_ptr->thread_map_event().entries(1).comm_md5_prefix());
}

TEST(PerfSerializerTest, StatConfigEvents) {
  std::stringstream input;
  std::vector<struct stat_config_event_entry> data;
  struct stat_config_event_entry entry1 = {
      .tag = 0,
      .val = 3,
  };
  struct stat_config_event_entry entry2 = {
      .tag = 1,
      .val = 20030,
  };
  struct stat_config_event_entry entry3 = {
      .tag = 2,
      .val = 1,
  };

  data.push_back(entry1);
  data.push_back(entry2);
  data.push_back(entry3);

  // PERF_RECORD_STAT_CONFIG
  testing::ExampleStatConfigEvent stat_config_event(data);
  size_t data_size = stat_config_event.GetSize();

  // header
  testing::ExamplePerfDataFileHeader file_header(0);
  file_header.WithAttrCount(1).WithDataSize(data_size).WriteTo(&input);

  // attrs
  ASSERT_EQ(file_header.header().attrs.offset, static_cast<u64>(input.tellp()));
  testing::ExamplePerfFileAttr_Hardware(PERF_SAMPLE_TID, /*sample_id_all=*/true)
      .WriteTo(&input);

  // data
  ASSERT_EQ(file_header.header().data.offset, static_cast<u64>(input.tellp()));
  stat_config_event.WriteTo(&input);
  ASSERT_EQ(file_header.header().data.offset + data_size,
            static_cast<u64>(input.tellp()));

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_STAT_CONFIG, events[0].event_ptr->header().type());
  EXPECT_EQ(3, events[0].event_ptr->stat_config_event().data_size());
  EXPECT_EQ(0, events[0].event_ptr->stat_config_event().data(0).tag());
  EXPECT_EQ(3, events[0].event_ptr->stat_config_event().data(0).val());
  EXPECT_EQ(1, events[0].event_ptr->stat_config_event().data(1).tag());
  EXPECT_EQ(20030, events[0].event_ptr->stat_config_event().data(1).val());
  EXPECT_EQ(2, events[0].event_ptr->stat_config_event().data(2).tag());
  EXPECT_EQ(1, events[0].event_ptr->stat_config_event().data(2).val());
}

TEST(PerfSerializerTest, PipedStatConfigEvents) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data
  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_TID,
                                              /*sample_id_all=*/true)
      .WriteTo(&input);

  std::vector<struct stat_config_event_entry> data;
  struct stat_config_event_entry entry1 = {
      .tag = 0,
      .val = 3,
  };
  struct stat_config_event_entry entry2 = {
      .tag = 1,
      .val = 20030,
  };
  struct stat_config_event_entry entry3 = {
      .tag = 2,
      .val = 1,
  };

  data.push_back(entry1);
  data.push_back(entry2);
  data.push_back(entry3);

  // PERF_RECORD_STAT_CONFIG
  testing::ExampleStatConfigEvent(data).WriteTo(&input);

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_STAT_CONFIG, events[0].event_ptr->header().type());
  EXPECT_EQ(3, events[0].event_ptr->stat_config_event().data_size());
  EXPECT_EQ(0, events[0].event_ptr->stat_config_event().data(0).tag());
  EXPECT_EQ(3, events[0].event_ptr->stat_config_event().data(0).val());
  EXPECT_EQ(1, events[0].event_ptr->stat_config_event().data(1).tag());
  EXPECT_EQ(20030, events[0].event_ptr->stat_config_event().data(1).val());
  EXPECT_EQ(2, events[0].event_ptr->stat_config_event().data(2).tag());
  EXPECT_EQ(1, events[0].event_ptr->stat_config_event().data(2).val());
}

TEST(PerfSerializerTest, StatEvents) {
  std::stringstream input;

  // PERF_RECORD_STAT
  testing::ExampleStatEvent stat_event(10, 2, 3454, 8967584, 123423, 123056);
  size_t data_size = stat_event.GetSize();

  // header
  testing::ExamplePerfDataFileHeader file_header(0);
  file_header.WithAttrCount(1).WithDataSize(data_size).WriteTo(&input);

  // attrs
  ASSERT_EQ(file_header.header().attrs.offset, static_cast<u64>(input.tellp()));
  testing::ExamplePerfFileAttr_Hardware(PERF_SAMPLE_TID, /*sample_id_all=*/true)
      .WriteTo(&input);

  // data
  ASSERT_EQ(file_header.header().data.offset, static_cast<u64>(input.tellp()));
  stat_event.WriteTo(&input);
  ASSERT_EQ(file_header.header().data.offset + data_size,
            static_cast<u64>(input.tellp()));

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_STAT, events[0].event_ptr->header().type());
  EXPECT_EQ(10, events[0].event_ptr->stat_event().id());
  EXPECT_EQ(2, events[0].event_ptr->stat_event().cpu());
  EXPECT_EQ(3454, events[0].event_ptr->stat_event().thread());
  EXPECT_EQ(8967584, events[0].event_ptr->stat_event().value());
  EXPECT_EQ(123423, events[0].event_ptr->stat_event().enabled());
  EXPECT_EQ(123056, events[0].event_ptr->stat_event().running());
}

TEST(PerfSerializerTest, PipedStatEvents) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data
  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_TID,
                                              /*sample_id_all=*/true)
      .WriteTo(&input);

  // PERF_RECORD_STAT
  testing::ExampleStatEvent(10, 2, 3454, 8967584, 123423, 123056)
      .WriteTo(&input);

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_STAT, events[0].event_ptr->header().type());
  EXPECT_EQ(10, events[0].event_ptr->stat_event().id());
  EXPECT_EQ(2, events[0].event_ptr->stat_event().cpu());
  EXPECT_EQ(3454, events[0].event_ptr->stat_event().thread());
  EXPECT_EQ(8967584, events[0].event_ptr->stat_event().value());
  EXPECT_EQ(123423, events[0].event_ptr->stat_event().enabled());
  EXPECT_EQ(123056, events[0].event_ptr->stat_event().running());
}

TEST(PerfSerializerTest, StatRoundEvents) {
  std::stringstream input;

  // PERF_RECORD_STAT_ROUND
  testing::ExampleStatRoundEvent stat_round_event(PERF_STAT_ROUND_TYPE__FINAL,
                                                  134256);
  size_t data_size = stat_round_event.GetSize();

  // header
  testing::ExamplePerfDataFileHeader file_header(0);
  file_header.WithAttrCount(1).WithDataSize(data_size).WriteTo(&input);

  // attrs
  ASSERT_EQ(file_header.header().attrs.offset, static_cast<u64>(input.tellp()));
  testing::ExamplePerfFileAttr_Hardware(PERF_SAMPLE_TID, /*sample_id_all=*/true)
      .WriteTo(&input);

  // data
  ASSERT_EQ(file_header.header().data.offset, static_cast<u64>(input.tellp()));
  stat_round_event.WriteTo(&input);
  ASSERT_EQ(file_header.header().data.offset + data_size,
            static_cast<u64>(input.tellp()));

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_STAT_ROUND, events[0].event_ptr->header().type());
  EXPECT_EQ(PERF_STAT_ROUND_TYPE__FINAL,
            events[0].event_ptr->stat_round_event().type());
  EXPECT_EQ(134256, events[0].event_ptr->stat_round_event().time());
}

TEST(PerfSerializerTest, PipedStatRoundEvents) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data
  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_TID,
                                              /*sample_id_all=*/true)
      .WriteTo(&input);

  // PERF_RECORD_STAT_ROUND
  testing::ExampleStatRoundEvent(PERF_STAT_ROUND_TYPE__FINAL, 134256)
      .WriteTo(&input);

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_STAT_ROUND, events[0].event_ptr->header().type());
  EXPECT_EQ(PERF_STAT_ROUND_TYPE__FINAL,
            events[0].event_ptr->stat_round_event().type());
  EXPECT_EQ(134256, events[0].event_ptr->stat_round_event().time());
}

TEST(PerfParserTest, TimeConvEvents) {
  std::stringstream input;

  // PERF_RECORD_TIME_CONV
  testing::ExampleTimeConvEventSmall time_conv_event(5656, 4, 234321);

  size_t data_size = time_conv_event.GetSize();

  // header
  testing::ExamplePerfDataFileHeader file_header(0);
  file_header.WithAttrCount(1).WithDataSize(data_size).WriteTo(&input);

  // attrs
  ASSERT_EQ(file_header.header().attrs.offset, static_cast<u64>(input.tellp()));
  testing::ExamplePerfFileAttr_Hardware(PERF_SAMPLE_TID, true /*sample_id_all*/)
      .WriteTo(&input);

  // data
  ASSERT_EQ(file_header.header().data.offset, static_cast<u64>(input.tellp()));
  time_conv_event.WriteTo(&input);
  ASSERT_EQ(file_header.header().data.offset + data_size,
            static_cast<u64>(input.tellp()));

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_TIME_CONV, events[0].event_ptr->header().type());
  EXPECT_EQ(5656, events[0].event_ptr->time_conv_event().time_shift());
  EXPECT_EQ(4, events[0].event_ptr->time_conv_event().time_mult());
  EXPECT_EQ(234321, events[0].event_ptr->time_conv_event().time_zero());
  EXPECT_FALSE(events[0].event_ptr->time_conv_event().has_time_cycles());
  EXPECT_FALSE(events[0].event_ptr->time_conv_event().has_time_mask());
  EXPECT_FALSE(events[0].event_ptr->time_conv_event().has_cap_user_time_zero());
  EXPECT_FALSE(
      events[0].event_ptr->time_conv_event().has_cap_user_time_short());
}

// Kernel 5.10 added new fields to the time_conv event.
TEST(PerfParserTest, TimeConvEventsLarge) {
  std::stringstream input;

  // PERF_RECORD_TIME_CONV
  testing::ExampleTimeConvEvent time_conv_event(5656, 4, 234321, 9876, 1234,
                                                true, false);

  size_t data_size = time_conv_event.GetSize();

  // header
  testing::ExamplePerfDataFileHeader file_header(0);
  file_header.WithAttrCount(1).WithDataSize(data_size).WriteTo(&input);

  // attrs
  ASSERT_EQ(file_header.header().attrs.offset, static_cast<u64>(input.tellp()));
  testing::ExamplePerfFileAttr_Hardware(PERF_SAMPLE_TID, true /*sample_id_all*/)
      .WriteTo(&input);

  // data
  ASSERT_EQ(file_header.header().data.offset, static_cast<u64>(input.tellp()));
  time_conv_event.WriteTo(&input);
  ASSERT_EQ(file_header.header().data.offset + data_size,
            static_cast<u64>(input.tellp()));

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_TIME_CONV, events[0].event_ptr->header().type());
  EXPECT_EQ(5656, events[0].event_ptr->time_conv_event().time_shift());
  EXPECT_EQ(4, events[0].event_ptr->time_conv_event().time_mult());
  EXPECT_EQ(234321, events[0].event_ptr->time_conv_event().time_zero());
  EXPECT_EQ(9876, events[0].event_ptr->time_conv_event().time_cycles());
  EXPECT_EQ(1234, events[0].event_ptr->time_conv_event().time_mask());
  EXPECT_EQ(true, events[0].event_ptr->time_conv_event().cap_user_time_zero());
  EXPECT_EQ(false,
            events[0].event_ptr->time_conv_event().cap_user_time_short());
}

TEST(PerfParserTest, PipedTimeConvEvents) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data
  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_TIME_CONV
  testing::ExampleTimeConvEventSmall(5656, 4, 234321).WriteTo(&input);

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_TIME_CONV, events[0].event_ptr->header().type());
  EXPECT_EQ(5656, events[0].event_ptr->time_conv_event().time_shift());
  EXPECT_EQ(4, events[0].event_ptr->time_conv_event().time_mult());
  EXPECT_EQ(234321, events[0].event_ptr->time_conv_event().time_zero());
  EXPECT_FALSE(events[0].event_ptr->time_conv_event().has_time_cycles());
  EXPECT_FALSE(events[0].event_ptr->time_conv_event().has_time_mask());
  EXPECT_FALSE(events[0].event_ptr->time_conv_event().has_cap_user_time_zero());
  EXPECT_FALSE(
      events[0].event_ptr->time_conv_event().has_cap_user_time_short());
}

TEST(PerfParserTest, PipedTimeConvEventsLarge) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data
  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_TIME_CONV
  testing::ExampleTimeConvEvent(5656, 4, 234321, 9876, 1234, true, false)
      .WriteTo(&input);

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_TIME_CONV, events[0].event_ptr->header().type());
  EXPECT_EQ(5656, events[0].event_ptr->time_conv_event().time_shift());
  EXPECT_EQ(4, events[0].event_ptr->time_conv_event().time_mult());
  EXPECT_EQ(234321, events[0].event_ptr->time_conv_event().time_zero());
  EXPECT_EQ(9876, events[0].event_ptr->time_conv_event().time_cycles());
  EXPECT_EQ(1234, events[0].event_ptr->time_conv_event().time_mask());
  EXPECT_EQ(true, events[0].event_ptr->time_conv_event().cap_user_time_zero());
  EXPECT_EQ(false,
            events[0].event_ptr->time_conv_event().cap_user_time_short());
}

TEST(PerfParserTest, CgroupEvents) {
  std::stringstream input;

  // PERF_RECORD_CGROUP
  testing::ExampleCgroupEvent cgroup_event(567, "group1",
                                           testing::SampleInfo().Tid(1001));

  size_t data_size = cgroup_event.GetSize();

  // header
  testing::ExamplePerfDataFileHeader file_header(0);
  file_header.WithAttrCount(1).WithDataSize(data_size).WriteTo(&input);

  // attrs
  ASSERT_EQ(file_header.header().attrs.offset, static_cast<u64>(input.tellp()));
  testing::ExamplePerfFileAttr_Hardware(PERF_SAMPLE_TID, true /*sample_id_all*/)
      .WriteTo(&input);

  // data
  ASSERT_EQ(file_header.header().data.offset, static_cast<u64>(input.tellp()));
  cgroup_event.WriteTo(&input);
  ASSERT_EQ(file_header.header().data.offset + data_size,
            static_cast<u64>(input.tellp()));

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_CGROUP, events[0].event_ptr->header().type());
  EXPECT_EQ(567, events[0].event_ptr->cgroup_event().id());
  EXPECT_STREQ("group1", events[0].event_ptr->cgroup_event().path().c_str());
}

TEST(PerfParserTest, PipedCgroupEvents) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data
  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_CGROUP
  testing::ExampleCgroupEvent(5678, "group2", testing::SampleInfo().Tid(1001))
      .WriteTo(&input);

  //
  // Parse input.
  //
  PerfReader reader;
  ASSERT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(1, events.size());

  EXPECT_EQ(PERF_RECORD_CGROUP, events[0].event_ptr->header().type());
  EXPECT_EQ(5678, events[0].event_ptr->cgroup_event().id());
  EXPECT_STREQ("group2", events[0].event_ptr->cgroup_event().path().c_str());
}

TEST(PerfParserTest, DsoInfoHasBuildId) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP
  testing::ExampleMmapEvent(1001, 0x1c1000, 0x1000, 0, "/usr/lib/foo.so",
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 0
  // becomes: 0x0000, 0x1000, 0
  testing::ExampleMmapEvent(1001, 0x1c3000, 0x2000, 0x2000, "/usr/lib/bar.so",
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 1
  // becomes: 0x1000, 0x2000, 0

  // PERF_RECORD_HEADER_BUILDID                                // N/A
  std::string build_id_filename("/usr/lib/foo.so\0", 2 * sizeof(u64));
  ASSERT_EQ(0, build_id_filename.size() % sizeof(u64)) << "Sanity check";
  const size_t event_size =
      sizeof(struct build_id_event) + build_id_filename.size();
  const struct build_id_event event = {
      .header =
          {
              .type = PERF_RECORD_HEADER_BUILD_ID,
              .misc = 0,
              .size = static_cast<u16>(event_size),
          },
      .pid = -1,
      .build_id = {0xde, 0xad, 0xf0, 0x0d},
  };
  input.write(reinterpret_cast<const char *>(&event), sizeof(event));
  input.write(build_id_filename.data(), build_id_filename.size());

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001))
      .WriteTo(&input);  // 2
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c300a).Tid(1001))
      .WriteTo(&input);  // 3

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(2, parser.stats().num_mmap_events);
  EXPECT_EQ(2, parser.stats().num_sample_events);
  EXPECT_EQ(2, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(4, events.size());

  EXPECT_EQ("/usr/lib/foo.so", events[2].dso_and_offset.dso_name());
  EXPECT_EQ("deadf00d00000000000000000000000000000000",
            events[2].dso_and_offset.build_id());
  EXPECT_EQ("/usr/lib/bar.so", events[3].dso_and_offset.dso_name());
  EXPECT_EQ("", events[3].dso_and_offset.build_id());
}

// Check the process has a Linux capability. See libcap(3) and capabilities(7).
bool HaveCapability(cap_value_t capability) {
  cap_t capabilities = cap_get_proc();
  cap_flag_value_t value;
  CHECK_EQ(cap_get_flag(capabilities, capability, CAP_EFFECTIVE, &value), 0);
  cap_free(capabilities);
  return value == CAP_SET;
}

class RunInMountNamespaceThread : public quipper::Thread {
 public:
  explicit RunInMountNamespaceThread(std::string tmpdir, std::string mntdir)
      : quipper::Thread("MntNamespace"),
        tmpdir_(std::move(tmpdir)),
        mntdir_(std::move(mntdir)) {}

  void Start() override {
    quipper::Thread::Start();
    ready.Wait();
  }

  void Join() override {
    exit.Notify();
    quipper::Thread::Join();
  }

 private:
  void Run() override {
    CHECK_EQ(unshare(CLONE_NEWNS), 0);
    CHECK_EQ(mount(tmpdir_.c_str(), mntdir_.c_str(), nullptr, MS_BIND, nullptr),
             0);
    ready.Notify();
    exit.Wait();
  }

  Notification ready;
  Notification exit;
  std::string tmpdir_;
  std::string mntdir_;
};

// Root task <pid>/<pid> Filesystem:
//   "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
//   "/tmp/quipper_mnt.../file_in_namespace"  (Doesn't exist)
// Container task <pid>/<tid> Filesystem:
//   "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
// * "/tmp/quipper_mnt.../file_in_namespace"  buildid: deadbeef  ino: X
// <path> = marked with *
// MMAP2: <pid+10>/<tid+1>, <path>, ino: X
// MMAP2: <pid>/<tid>, <path>, ino: X
// Reject (doesn't exist): /proc/<tid+10>/root/<path>
// Reject (doesn't exist): /proc/<pid+1>/root/<path>
// Accept:                 /proc/<tid>/root/<path>
// (Not tried):            /proc/<pid>/root/<path>
// (Not tried): /<path>
// Expected buildid for <path>: "deadbeef"
TEST(PerfParserTest, ReadsBuildidsInMountNamespace) {
  if (!HaveCapability(CAP_SYS_ADMIN)) return;  // Skip test.
  ScopedTempDir tmpdir("/tmp/quipper_tmp.");
  ScopedTempDir mntdir("/tmp/quipper_mnt.");
  RunInMountNamespaceThread thread(tmpdir.path(), mntdir.path());
  thread.Start();
  const pid_t pid = getpid();
  const pid_t tid = thread.tid();

  const std::string tmpfile = tmpdir.path() + "file_in_namespace";
  const std::string tmpfile_in_ns = mntdir.path() + "file_in_namespace";
  InitializeLibelf();
  testing::WriteElfWithBuildid(tmpfile, ".note.gnu.build-id",
                               "\xde\xad\xbe\xef");
  struct stat tmp_stat;
  ASSERT_NE(stat(tmpfile_in_ns.c_str(), &tmp_stat), 0);
  ASSERT_EQ(stat(tmpfile.c_str(), &tmp_stat), 0);

  // Create perf.data
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP2
  // - mmap from a process and thread that doesn't exist
  testing::ExampleMmap2Event(pid, tid, 0x1c1000, 0x1000, 0, tmpfile_in_ns,
                             testing::SampleInfo().Tid(pid + 10, tid + 1))
      .WithDeviceInfo(major(tmp_stat.st_dev), minor(tmp_stat.st_dev),
                      tmp_stat.st_ino)
      .WriteTo(&input);  // 0
  // - mmap from a running thread
  testing::ExampleMmap2Event(pid, tid, 0x1c2000, 0x1000, 0, tmpfile_in_ns,
                             testing::SampleInfo().Tid(pid, tid))
      .WithDeviceInfo(major(tmp_stat.st_dev), minor(tmp_stat.st_dev),
                      tmp_stat.st_ino)
      .WriteTo(&input);  // 1

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(pid, tid))
      .WriteTo(&input);  // 2

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.read_missing_buildids = true;
  options.sample_mapping_percentage_threshold = 0;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(2, parser.stats().num_mmap_events);
  EXPECT_EQ(1, parser.stats().num_sample_events);
  EXPECT_EQ(1, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(3, events.size());

  EXPECT_EQ(tmpfile_in_ns, events[2].dso_and_offset.dso_name());
  EXPECT_EQ("deadbeef", events[2].dso_and_offset.build_id());

  thread.Join();
}

class RunInMountNamespaceProcess {
 public:
  RunInMountNamespaceProcess(std::string tmpdir, std::string mntdir)
      : pid_(0), tmpdir_(std::move(tmpdir)), mntdir_(std::move(mntdir)) {}

  void Start() {
    int pipe_fd[2];
    int nonce = 0;
    CHECK_EQ(pipe(pipe_fd), 0) << "pipe: " << strerror(errno);

    pid_ = fork();
    CHECK_NE(-1, pid_) << "fork: " << strerror(errno);
    if (pid_ == 0) {  // child
      close(pipe_fd[0]);
      CHECK_EQ(unshare(CLONE_NEWNS), 0);
      CHECK_EQ(
          mount(tmpdir_.c_str(), mntdir_.c_str(), nullptr, MS_BIND, nullptr),
          0);
      // Tell parent mounting is done.
      CHECK_EQ(write(pipe_fd[1], &nonce, sizeof(nonce)),
               static_cast<ssize_t>(sizeof(nonce)));
      close(pipe_fd[1]);
      pause();  // Wait to be killed. (So morbid.)
      std::_Exit(-1);
    }
    close(pipe_fd[1]);

    // Wait for child to tell us it has mounted the dir.
    ssize_t sz = read(pipe_fd[0], &nonce, sizeof(nonce));
    CHECK_EQ(sz, static_cast<ssize_t>(sizeof(nonce)))
        << "read: " << strerror(errno);
    close(pipe_fd[0]);
  }

  void Exit() {
    kill(pid_, SIGTERM);
    waitpid(pid_, nullptr, 0);
  }

  pid_t pid() { return pid_; }

 private:
  pid_t pid_;
  std::string tmpdir_;
  std::string mntdir_;
};

// Root task <pid>/<pid> Filesystem:
//   "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
// * "/tmp/quipper_mnt.../file_in_namespace"  buildid: baadf00d  ino: Y
// Container task <pid2>/<pid2> Filesystem:
//   "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
// * "/tmp/quipper_mnt.../file_in_namespace"  buildid: deadbeef  ino: X
// <path> = marked with *
// MMAP2: <pid2>/<pid2+1>, <path>, ino: X
// Reject (doesn't exist): /proc/<pid2+1>/root/<path>
// Accept:                 /proc/<pid2>/root/<path>
// (Not tried): /<path>
// Expected buildid for <path>: "deadbeef"
TEST(PerfParserTest, ReadsBuildidsInMountNamespace_TriesOwningProcess) {
  if (!HaveCapability(CAP_SYS_ADMIN)) return;  // Skip test.
  ScopedTempDir tmpdir("/tmp/quipper_tmp.");
  ScopedTempDir mntdir("/tmp/quipper_mnt.");
  RunInMountNamespaceProcess process(tmpdir.path(), mntdir.path());
  process.Start();

  // Pretend we launched a thread in the other process, it mapped the file,
  // and then exited. Let's make up a tid for it that's not likely to exist.
  const pid_t pid = process.pid();
  const pid_t tid = pid + 1;

  const std::string tmpfile = tmpdir.path() + "file_in_namespace";
  const std::string tmpfile_in_ns = mntdir.path() + "file_in_namespace";
  InitializeLibelf();
  testing::WriteElfWithBuildid(tmpfile, ".note.gnu.build-id",
                               "\xde\xad\xbe\xef");
  // It's a trap! If "baadf00d" is seen, we read the wrong file.
  testing::WriteElfWithBuildid(tmpfile_in_ns, ".note.gnu.build-id",
                               "\xba\xad\xf0\x0d");
  struct stat tmp_stat;
  ASSERT_EQ(stat(tmpfile.c_str(), &tmp_stat), 0);

  // Create perf.data
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP2
  testing::ExampleMmap2Event(pid, tid, 0x1c1000, 0x1000, 0, tmpfile_in_ns,
                             testing::SampleInfo().Tid(pid, tid))
      .WithDeviceInfo(major(tmp_stat.st_dev), minor(tmp_stat.st_dev),
                      tmp_stat.st_ino)
      .WriteTo(&input);  // 0

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(pid, tid))
      .WriteTo(&input);  // 1

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.read_missing_buildids = true;
  options.sample_mapping_percentage_threshold = 0;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(1, parser.stats().num_mmap_events);
  EXPECT_EQ(1, parser.stats().num_sample_events);
  EXPECT_EQ(1, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(2, events.size());

  EXPECT_EQ(tmpfile_in_ns, events[1].dso_and_offset.dso_name());
  EXPECT_EQ("deadbeef", events[1].dso_and_offset.build_id());

  process.Exit();
}

// Root task <pid>/<pid> Filesystem:
// * "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
//   "/tmp/quipper_mnt.../file_in_namespace"  buildid: baadf00d  ino: Y
// Container task <pid>/<tid> Filesystem:
// * "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
//   "/tmp/quipper_mnt.../file_in_namespace"  buildid: deadbeef  ino: X
// <path> = marked with *
// MMAP2: <pid+10>/<tid+1>, <path>, ino: X
// Reject (doesn't exist): /proc/<tid+1>/root/<path>
// Reject (doesn't exist): /proc/<pid+10>/root/<path>
// Accept (same inode): /<path>
// Expected buildid for <path>: "deadbeef"
TEST(PerfParserTest, ReadsBuildidsInMountNamespace_TriesRootFs) {
  if (!HaveCapability(CAP_SYS_ADMIN)) return;  // Skip test.
  ScopedTempDir tmpdir("/tmp/quipper_tmp.");
  ScopedTempDir mntdir("/tmp/quipper_mnt.");
  RunInMountNamespaceThread thread(tmpdir.path(), mntdir.path());
  thread.Start();
  const pid_t pid = getpid();
  const pid_t tid = thread.tid();

  const std::string tmpfile = tmpdir.path() + "file_in_namespace";
  const std::string tmpfile_in_ns = mntdir.path() + "file_in_namespace";
  InitializeLibelf();
  testing::WriteElfWithBuildid(tmpfile, ".note.gnu.build-id",
                               "\xde\xad\xbe\xef");
  // It's a trap! If "baadf00d" is seen, we read the wrong file.
  testing::WriteElfWithBuildid(tmpfile_in_ns, ".note.gnu.build-id",
                               "\xba\xad\xf0\x0d");
  struct stat tmp_stat;
  ASSERT_EQ(stat(tmpfile.c_str(), &tmp_stat), 0);

  // Create perf.data
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP2
  // - Process doesn't exist, but file exists in our own namespace.
  testing::ExampleMmap2Event(pid, pid, 0x1c1000, 0x1000, 0, tmpfile,
                             testing::SampleInfo().Tid(pid + 10, tid + 1))
      .WithDeviceInfo(major(tmp_stat.st_dev), minor(tmp_stat.st_dev),
                      tmp_stat.st_ino)
      .WriteTo(&input);  // 0

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(pid))
      .WriteTo(&input);  // 1

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.read_missing_buildids = true;
  options.sample_mapping_percentage_threshold = 0;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(1, parser.stats().num_mmap_events);
  EXPECT_EQ(1, parser.stats().num_sample_events);
  EXPECT_EQ(1, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(2, events.size());

  EXPECT_EQ(tmpfile, events[1].dso_and_offset.dso_name());
  // Finds file in root FS.
  EXPECT_EQ("deadbeef", events[1].dso_and_offset.build_id());

  thread.Join();
}

// Root task <pid>/<pid> Filesystem:
//   "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
//   "/tmp/quipper_mnt.../file_in_namespace"  buildid: baadf00d  ino: Y
// Container task <pid>/<tid> Filesystem:
//   "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
// * "/tmp/quipper_mnt.../file_in_namespace"  buildid: deadbeef  ino: X
// <path> = marked with *
// MMAP2: <pid>/<tid>, <path>, ino: X+1
// Reject (wrong inode): /proc/<tid>/root/<path>
// Reject (wrong inode): /proc/<pid>/root/<path>
// Reject (wrong inode): /<path>
// Expected buildid for <path>: ""
TEST(PerfParserTest, ReadsBuildidsInMountNamespace_TriesRootFsRejectsInode) {
  if (!HaveCapability(CAP_SYS_ADMIN)) return;  // Skip test.
  ScopedTempDir tmpdir("/tmp/quipper_tmp.");
  ScopedTempDir mntdir("/tmp/quipper_mnt.");
  RunInMountNamespaceThread thread(tmpdir.path(), mntdir.path());
  thread.Start();
  const pid_t pid = getpid();
  const pid_t tid = thread.tid();

  const std::string tmpfile = tmpdir.path() + "file_in_namespace";
  const std::string tmpfile_in_ns = mntdir.path() + "file_in_namespace";
  InitializeLibelf();
  testing::WriteElfWithBuildid(tmpfile, ".note.gnu.build-id",
                               "\xde\xad\xbe\xef");
  // It's a trap! If "baadf00d" is seen, we read the wrong file.
  testing::WriteElfWithBuildid(tmpfile_in_ns, ".note.gnu.build-id",
                               "\xba\xad\xf0\x0d");
  struct stat tmp_stat;
  struct stat tmp_in_ns_stat;
  ASSERT_EQ(stat(tmpfile.c_str(), &tmp_stat), 0);
  ASSERT_EQ(stat(tmpfile_in_ns.c_str(), &tmp_in_ns_stat), 0);
  // inodes are often issued sequentially, so go backwards rather than forwards.
  const ino_t bad_ino = tmp_stat.st_ino - 1;
  ASSERT_NE(bad_ino, tmp_in_ns_stat.st_ino);

  // Create perf.data
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP2
  // - Process doesn't exist, but file exists in our own namespace.
  testing::ExampleMmap2Event(pid, pid, 0x1c1000, 0x1000, 0, tmpfile_in_ns,
                             testing::SampleInfo().Tid(pid, tid))
      .WithDeviceInfo(major(tmp_stat.st_dev), minor(tmp_stat.st_dev), bad_ino)
      .WriteTo(&input);  // 0

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(pid))
      .WriteTo(&input);  // 1

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.read_missing_buildids = true;
  options.sample_mapping_percentage_threshold = 0;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(1, parser.stats().num_mmap_events);
  EXPECT_EQ(1, parser.stats().num_sample_events);
  EXPECT_EQ(1, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(2, events.size());

  EXPECT_EQ(tmpfile_in_ns, events[1].dso_and_offset.dso_name());
  // Wrong inode, so rejects all candidates.
  EXPECT_EQ("", events[1].dso_and_offset.build_id());

  thread.Join();
}

// Root task <pid>/<pid> Filesystem:
//   "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
// * "/tmp/quipper_mnt.../file_in_namespace"  buildid: baadf00d  ino: Y
// Container task <pid>/<tid> Filesystem:
//   "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
// * "/tmp/quipper_mnt.../file_in_namespace"  buildid: deadbeef  ino: X
// <path> = marked with *
// MMAP: <pid+10>/<tid+1>, <path>, ino: Not available
// Reject (not found): /proc/<tid+1>/root/<path>
// Reject (not found): /proc/<pid+10>/root/<path>
// Accept (falsely): /<path>
// Expected buildid for <path>: "baadf00d" (even though incorrect)
TEST(PerfParserTest, ReadsBuildidsInMountNamespace_TriesRootFsNoInodeToReject) {
  if (!HaveCapability(CAP_SYS_ADMIN)) return;  // Skip test.
  ScopedTempDir tmpdir("/tmp/quipper_tmp.");
  ScopedTempDir mntdir("/tmp/quipper_mnt.");
  RunInMountNamespaceThread thread(tmpdir.path(), mntdir.path());
  thread.Start();
  const pid_t pid = getpid();
  const pid_t tid = thread.tid();

  const std::string tmpfile = tmpdir.path() + "file_in_namespace";
  const std::string tmpfile_in_ns = mntdir.path() + "file_in_namespace";
  InitializeLibelf();
  testing::WriteElfWithBuildid(tmpfile, ".note.gnu.build-id",
                               "\xde\xad\xf0\x0d");
  // It's a trap! If "baadf00d" is seen, we read the wrong file.
  testing::WriteElfWithBuildid(tmpfile_in_ns, ".note.gnu.build-id",
                               "\xba\xad\xf0\x0d");
  struct stat tmp_stat;
  ASSERT_EQ(stat(tmpfile.c_str(), &tmp_stat), 0);

  // Create perf.data
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP
  // - Process & thread don't exist, but file exists in our own namespace.
  testing::ExampleMmapEvent(pid, 0x1c1000, 0x1000, 0, tmpfile_in_ns,
                            testing::SampleInfo().Tid(pid + 10, tid + 1))
      .WriteTo(&input);  // 0

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(pid))
      .WriteTo(&input);  // 1

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.read_missing_buildids = true;
  options.sample_mapping_percentage_threshold = 0;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(1, parser.stats().num_mmap_events);
  EXPECT_EQ(1, parser.stats().num_sample_events);
  EXPECT_EQ(1, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(2, events.size());

  EXPECT_EQ(tmpfile_in_ns, events[1].dso_and_offset.dso_name());
  // We'll read the wrong file b/c we couldn't reject based on inode:
  EXPECT_EQ("baadf00d", events[1].dso_and_offset.build_id());

  thread.Join();
}

// Root task <pid>/<pid> Filesystem:
//   "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
// * "/tmp/quipper_mnt.../file_in_namespace"  buildid: baadf00d  ino: Y
// Container task <pid>/<tid> Filesystem:
//   "/tmp/quipper_tmp.../file_in_namespace"  buildid: deadbeef  ino: X
// * "/tmp/quipper_mnt.../file_in_namespace"  buildid: deadbeef  ino: X
// <path> = marked with *
// MMAP2(0): <pid>/<tid>, <path>, <maj+1>/<min>,  ino: X
// MMAP2(1): <pid>/<tid>, <path>, <maj>/<min+1>,  ino: X
// MMAP2(2): <pid>/<tid>, <path>, <maj>/<min>,    ino: X+1
// SAMPLE(3): <pid>/<tid>, addr in MMAP2(0)
// SAMPLE(4): <pid>/<tid>, addr in MMAP2(1)
// SAMPLE(5): <pid>/<tid>, addr in MMAP2(2)
// Expected buildid for <path>: ""
//
// with multiple device/ino numbers. This is really a shortcoming of perf--
// it can only associate a buildid with a path. If the same path exists in
// multiple containers but refers to different files (device/inode), then
// it's hard to know what to do. Similarly, PerfParser associates a DSO name
// (path) with a single DSOInfo and device/inode info therein, although it
// tracks all threads the DSO name was seen in. This test is set up such that
// all MMAPs should be rejected, but in truth PerfParser only compares the
// device/inode of the file against the device/inode of one of the MMAPs.
// A better thing to do might be to track a
// map<tuple<maj,min,ino,path>, DSOInfo>, but even so, it will be impossible
// to store unambiguously in perf.data.
TEST(PerfParserTest, ReadsBuildidsInMountNamespace_DifferentDevOrIno) {
  if (!HaveCapability(CAP_SYS_ADMIN)) return;  // Skip test.
  ScopedTempDir tmpdir("/tmp/quipper_tmp.");
  ScopedTempDir mntdir("/tmp/quipper_mnt.");
  RunInMountNamespaceThread thread(tmpdir.path(), mntdir.path());
  thread.Start();
  const pid_t pid = getpid();
  const pid_t tid = thread.tid();

  const std::string tmpfile = tmpdir.path() + "file_in_namespace";
  const std::string tmpfile_in_ns = mntdir.path() + "file_in_namespace";
  InitializeLibelf();
  testing::WriteElfWithBuildid(tmpfile, ".note.gnu.build-id",
                               "\xde\xad\xf0\x0d");
  // It's a trap! If "baadf00d" is seen, we read the wrong file.
  testing::WriteElfWithBuildid(tmpfile_in_ns, ".note.gnu.build-id",
                               "\xba\xad\xf0\x0d");
  struct stat tmp_stat;
  struct stat tmp_in_ns_stat;
  ASSERT_EQ(stat(tmpfile.c_str(), &tmp_stat), 0);
  ASSERT_EQ(stat(tmpfile_in_ns.c_str(), &tmp_in_ns_stat), 0);
  // inodes are often issued sequentially, so go backwards rather than forwards.
  const ino_t bad_ino = tmp_stat.st_ino - 1;
  ASSERT_NE(bad_ino, tmp_in_ns_stat.st_ino);

  // Create perf.data
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP2
  // - Wrong major number
  testing::ExampleMmap2Event(pid, tid, 0x1c1000, 0x1000, 0, tmpfile_in_ns,
                             testing::SampleInfo().Tid(pid, tid))
      .WithDeviceInfo(major(tmp_stat.st_dev) + 1, minor(tmp_stat.st_dev),
                      tmp_stat.st_ino)
      .WriteTo(&input);  // 0
  // - Wrong minor number
  testing::ExampleMmap2Event(pid, tid, 0x1c2000, 0x1000, 0, tmpfile_in_ns,
                             testing::SampleInfo().Tid(pid, tid))
      .WithDeviceInfo(major(tmp_stat.st_dev), minor(tmp_stat.st_dev) + 1,
                      tmp_stat.st_ino)
      .WriteTo(&input);  // 1
  // - Wrong inode number
  testing::ExampleMmap2Event(pid, tid, 0x1c3000, 0x1000, 0, tmpfile_in_ns,
                             testing::SampleInfo().Tid(pid, tid))
      .WithDeviceInfo(major(tmp_stat.st_dev), minor(tmp_stat.st_dev),
                      bad_ino)
      .WriteTo(&input);  // 2

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(pid, tid))
      .WriteTo(&input);  // 3
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c2000).Tid(pid, tid))
      .WriteTo(&input);  // 4
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c3000).Tid(pid, tid))
      .WriteTo(&input);  // 5

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.read_missing_buildids = true;
  options.sample_mapping_percentage_threshold = 0;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(3, parser.stats().num_mmap_events);
  EXPECT_EQ(3, parser.stats().num_sample_events);
  EXPECT_EQ(3, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(6, events.size());

  // Buildid should not be found for any of the samples.
  for (int i : {3, 4, 5}) {
    EXPECT_EQ(tmpfile_in_ns, events[i].dso_and_offset.dso_name());
    EXPECT_EQ("", events[i].dso_and_offset.build_id());
  }

  thread.Join();
}

TEST(PerfParserTest, OverwriteBuildidIfAlreadyKnown) {
  ScopedTempDir tmpdir("/tmp/quipper_tmp.");
  const std::string known_file = tmpdir.path() + "buildid_already_known";
  const std::string known_file_to_overwrite =
      tmpdir.path() + "buildid_already_known_overwrite";
  const std::string unknown_file = tmpdir.path() + "buildid_not_known";
  InitializeLibelf();
  testing::WriteElfWithBuildid(known_file_to_overwrite, ".note.gnu.build-id",
                               "\xf0\x01\x57\xea");
  testing::WriteElfWithBuildid(unknown_file, ".note.gnu.build-id",
                               "\xc0\x01\xd0\x0d");

  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP
  testing::ExampleMmapEvent(1001, 0x1c1000, 0x1000, 0, known_file,
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 0
  // becomes: 0x0000, 0x1000, 0
  testing::ExampleMmapEvent(1001, 0x1c2000, 0x2000, 0, known_file_to_overwrite,
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 1
  // becomes: 0x1000, 0x2000, 0
  testing::ExampleMmapEvent(1001, 0x1c4000, 0x2000, 0x2000, unknown_file,
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 2
  // becomes: 0x3000, 0x2000, 0x2000

  // PERF_RECORD_HEADER_BUILDID                                // N/A
  {
    std::string build_id_filename(known_file);
    build_id_filename.resize(Align<u64>(known_file.size()));  // null-pad
    const size_t event_size =
        sizeof(struct build_id_event) + build_id_filename.size();
    const struct build_id_event event = {
        .header =
            {
                .type = PERF_RECORD_HEADER_BUILD_ID,
                .misc = 0,
                .size = static_cast<u16>(event_size),
            },
        .pid = -1,
        .build_id = {0xde, 0xad, 0xbe, 0xef},
    };
    input.write(reinterpret_cast<const char *>(&event), sizeof(event));
    input.write(build_id_filename.data(), build_id_filename.size());
  }

  // PERF_RECORD_HEADER_BUILDID                                // N/A
  {
    std::string build_id_filename(known_file_to_overwrite);
    // null-pad
    build_id_filename.resize(Align<u64>(known_file_to_overwrite.size()));
    const size_t event_size =
        sizeof(struct build_id_event) + build_id_filename.size();
    const struct build_id_event event = {
        .header =
            {
                .type = PERF_RECORD_HEADER_BUILD_ID,
                .misc = 0,
                .size = static_cast<u16>(event_size),
            },
        .pid = -1,
        .build_id = {0xca, 0xfe, 0xba, 0xbe},
    };
    input.write(reinterpret_cast<const char *>(&event), sizeof(event));
    input.write(build_id_filename.data(), build_id_filename.size());
  }

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c100a).Tid(1001))
      .WriteTo(&input);  // 3
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c300b).Tid(1001))
      .WriteTo(&input);  // 4
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c400c).Tid(1001))
      .WriteTo(&input);  // 5

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.read_missing_buildids = true;
  options.sample_mapping_percentage_threshold = 0;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(3, parser.stats().num_mmap_events);
  EXPECT_EQ(3, parser.stats().num_sample_events);
  EXPECT_EQ(3, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(6, events.size());

  EXPECT_EQ(known_file, events[3].dso_and_offset.dso_name());
  EXPECT_EQ("deadbeef00000000000000000000000000000000",
            events[3].dso_and_offset.build_id());
  EXPECT_EQ(known_file_to_overwrite, events[4].dso_and_offset.dso_name());
  EXPECT_EQ("f00157ea", events[4].dso_and_offset.build_id());
  EXPECT_EQ(unknown_file, events[5].dso_and_offset.dso_name());
  EXPECT_EQ("c001d00d", events[5].dso_and_offset.build_id());
}

TEST(PerfParserTest, OnlyReadsBuildidIfSampled) {
  ScopedTempDir tmpdir("/tmp/quipper_tmp.");
  const std::string unknown_file = tmpdir.path() + "buildid_not_known";
  InitializeLibelf();
  testing::WriteElfWithBuildid(unknown_file, ".note.gnu.build-id",
                               "\xc0\x01\xd0\x0d");

  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP
  testing::ExampleMmapEvent(1001, 0x1c1000, 0x1000, 0, unknown_file,
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 0
  // becomes: 0x0000, 0x1000, 0

  // PERF_RECORD_SAMPLE
  // - Sample outside mmap
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c300a).Tid(1001))
      .WriteTo(&input);  // 1

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.read_missing_buildids = true;
  options.sample_mapping_percentage_threshold = 0;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(1, parser.stats().num_mmap_events);
  EXPECT_EQ(1, parser.stats().num_sample_events);
  EXPECT_EQ(0, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(2, events.size());

  EXPECT_EQ("", events[1].dso_and_offset.dso_name());
  EXPECT_EQ("", events[1].dso_and_offset.build_id());

  std::map<std::string, std::string> filenames_to_build_ids;
  reader.GetFilenamesToBuildIDs(&filenames_to_build_ids);
  auto it = filenames_to_build_ids.find(unknown_file);
  EXPECT_EQ(filenames_to_build_ids.end(), it) << it->first << " " << it->second;
}

TEST(PerfParserTest, HandlesFinishedRoundEventsAndSortsByTime) {
  // For now at least, we are ignoring PERF_RECORD_FINISHED_ROUND events.

  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(
      PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME,
      true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP
  testing::ExampleMmapEvent(1001, 0x1c1000, 0x1000, 0, "/usr/lib/foo.so",
                            testing::SampleInfo().Tid(1001).Time(12300010))
      .WriteTo(&input);
  // becomes: 0x0000, 0x1000, 0

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001).Time(12300020))
      .WriteTo(&input);  // 1
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001).Time(12300030))
      .WriteTo(&input);  // 2
  // PERF_RECORD_FINISHED_ROUND
  testing::FinishedRoundEvent().WriteTo(&input);  // N/A

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001).Time(12300050))
      .WriteTo(&input);  // 3
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001).Time(12300040))
      .WriteTo(&input);  // 4
  // PERF_RECORD_FINISHED_ROUND
  testing::FinishedRoundEvent().WriteTo(&input);  // N/A

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.sort_events_by_time = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(1, parser.stats().num_mmap_events);
  EXPECT_EQ(4, parser.stats().num_sample_events);
  EXPECT_EQ(4, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(5, events.size());

  EXPECT_EQ(PERF_RECORD_MMAP, events[0].event_ptr->header().type());
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[1].event_ptr->header().type());
  EXPECT_EQ(12300020, events[1].event_ptr->sample_event().sample_time_ns());
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[2].event_ptr->header().type());
  EXPECT_EQ(12300030, events[2].event_ptr->sample_event().sample_time_ns());
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[3].event_ptr->header().type());
  EXPECT_EQ(12300040, events[3].event_ptr->sample_event().sample_time_ns());
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[4].event_ptr->header().type());
  EXPECT_EQ(12300050, events[4].event_ptr->sample_event().sample_time_ns());
}

TEST(PerfParserTest, MmapCoversEntireAddressSpace) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP, a kernel mapping that covers the whole space.
  const uint32_t kKernelMmapPid = UINT32_MAX;
  testing::ExampleMmapEvent(kKernelMmapPid, 0, UINT64_MAX, 0,
                            "[kernel.kallsyms]_text",
                            testing::SampleInfo().Tid(kKernelMmapPid, 0))
      .WriteTo(&input);

  // PERF_RECORD_MMAP, a shared object library.
  testing::ExampleMmapEvent(1234, 0x7f008e000000, 0x2000000, 0,
                            "/usr/lib/libfoo.so",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);

  // PERF_RECORD_SAMPLE, within library.
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x7f008e123456).Tid(1234, 1235))
      .WriteTo(&input);
  // PERF_RECORD_SAMPLE, within kernel.
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x8000819e).Tid(1234, 1235))
      .WriteTo(&input);
  // PERF_RECORD_SAMPLE, within library.
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x7f008fdeadbe).Tid(1234, 1235))
      .WriteTo(&input);
  // PERF_RECORD_SAMPLE, within kernel.
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0xffffffff8100cafe).Tid(1234, 1235))
      .WriteTo(&input);

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(2, parser.stats().num_mmap_events);
  EXPECT_EQ(4, parser.stats().num_sample_events);
  EXPECT_EQ(4, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(6, events.size());

  EXPECT_EQ(PERF_RECORD_MMAP, events[0].event_ptr->header().type());
  EXPECT_EQ("[kernel.kallsyms]_text",
            events[0].event_ptr->mmap_event().filename());
  EXPECT_EQ(PERF_RECORD_MMAP, events[1].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/libfoo.so", events[1].event_ptr->mmap_event().filename());

  // Sample from library.
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[2].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/libfoo.so", events[2].dso_and_offset.dso_name());
  EXPECT_EQ(0x123456, events[2].dso_and_offset.offset());

  // Sample from kernel.
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[3].event_ptr->header().type());
  EXPECT_EQ("[kernel.kallsyms]_text", events[3].dso_and_offset.dso_name());
  EXPECT_EQ(0x8000819e, events[3].dso_and_offset.offset());

  // Sample from library.
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[4].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/libfoo.so", events[4].dso_and_offset.dso_name());
  EXPECT_EQ(0x1deadbe, events[4].dso_and_offset.offset());

  // Sample from kernel.
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[5].event_ptr->header().type());
  EXPECT_EQ("[kernel.kallsyms]_text", events[5].dso_and_offset.dso_name());
  EXPECT_EQ(0xffffffff8100cafe, events[5].dso_and_offset.offset());
}

TEST(PerfParserTest, HugePagesMappings) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP, a normal mapping.
  testing::ExampleMmapEvent(1234, 0x40000000, 0x18000, 0, "/usr/lib/libfoo.so",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);

  // PERF_RECORD_SAMPLE, within library.
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x40000100).Tid(1234, 1235))
      .WriteTo(&input);

  // Split Chrome mapping #1, with a huge pages section in the middle.
  testing::ExampleMmapEvent(1234, 0x40018000, 0x1e8000, 0,
                            "/opt/google/chrome/chrome",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);
  testing::ExampleMmapEvent(1234, 0x40200000, 0x1c00000, 0, "//anon",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);
  testing::ExampleMmapEvent(1234, 0x41e00000, 0x4000000, 0x1de8000,
                            "/opt/google/chrome/chrome",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);

  // Split Chrome mapping #2, starting with a huge pages section (no preceding
  // normal mapping).
  testing::ExampleMmapEvent(2345, 0x45e00000, 0x1e00000, 0, "//anon",
                            testing::SampleInfo().Tid(2345, 2346))
      .WriteTo(&input);
  testing::ExampleMmapEvent(2345, 0x47c00000, 0x4000000, 0x1e00000,
                            "/opt/google/chrome/chrome",
                            testing::SampleInfo().Tid(2345, 2346))
      .WriteTo(&input);

  // PERF_RECORD_SAMPLE, within Chrome #1 (before huge pages mapping).
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x40018300).Tid(1234, 1235))
      .WriteTo(&input);
  // PERF_RECORD_SAMPLE, within Chrome #1 (within huge pages mapping).
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x40020400).Tid(1234, 1235))
      .WriteTo(&input);
  // PERF_RECORD_SAMPLE, within Chrome #1 (after huge pages mapping).
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x41e20500).Tid(1234, 1235))
      .WriteTo(&input);

  // PERF_RECORD_SAMPLE, within library.
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x40000700).Tid(1234, 1235))
      .WriteTo(&input);

  // PERF_RECORD_SAMPLE, within Chrome #2 (within huge pages mapping).
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x45e01300).Tid(2345, 2346))
      .WriteTo(&input);
  // PERF_RECORD_SAMPLE, within Chrome #2 (after huge pages mapping).
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x45e02f00).Tid(2345, 2346))
      .WriteTo(&input);

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.deduce_huge_page_mappings = true;
  options.combine_mappings = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(3, parser.stats().num_mmap_events);
  EXPECT_EQ(7, parser.stats().num_sample_events);
  EXPECT_EQ(7, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent> &events = parser.parsed_events();
  ASSERT_EQ(10, events.size());

  EXPECT_EQ(PERF_RECORD_MMAP, events[0].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/libfoo.so", events[0].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x40000000, events[0].event_ptr->mmap_event().start());
  EXPECT_EQ(0x18000, events[0].event_ptr->mmap_event().len());
  EXPECT_EQ(0x0, events[0].event_ptr->mmap_event().pgoff());

  // Sample from library.
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[1].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/libfoo.so", events[1].dso_and_offset.dso_name());
  EXPECT_EQ(0x100, events[1].dso_and_offset.offset());

  // The split Chrome mappings should have been combined.
  EXPECT_EQ(PERF_RECORD_MMAP, events[2].event_ptr->header().type());
  EXPECT_EQ("/opt/google/chrome/chrome",
            events[2].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x40018000, events[2].event_ptr->mmap_event().start());
  EXPECT_EQ(0x5de8000, events[2].event_ptr->mmap_event().len());
  EXPECT_EQ(0x0, events[2].event_ptr->mmap_event().pgoff());

  EXPECT_EQ(PERF_RECORD_MMAP, events[3].event_ptr->header().type());
  EXPECT_EQ("/opt/google/chrome/chrome",
            events[3].event_ptr->mmap_event().filename());
  EXPECT_EQ(0x45e00000, events[3].event_ptr->mmap_event().start());
  EXPECT_EQ(0x5e00000, events[3].event_ptr->mmap_event().len());
  EXPECT_EQ(0x0, events[3].event_ptr->mmap_event().pgoff());

  // Sample from Chrome (before huge pages mapping).
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[4].event_ptr->header().type());
  EXPECT_EQ("/opt/google/chrome/chrome", events[4].dso_and_offset.dso_name());
  EXPECT_EQ(0x300, events[4].dso_and_offset.offset());

  // Sample from Chrome (within huge pages mapping).
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[5].event_ptr->header().type());
  EXPECT_EQ("/opt/google/chrome/chrome", events[5].dso_and_offset.dso_name());
  EXPECT_EQ(0x8400, events[5].dso_and_offset.offset());

  // Sample from Chrome (after huge pages mapping).
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[6].event_ptr->header().type());
  EXPECT_EQ("/opt/google/chrome/chrome", events[6].dso_and_offset.dso_name());
  EXPECT_EQ(0x1e08500, events[6].dso_and_offset.offset());

  // Sample from library.
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[7].event_ptr->header().type());
  EXPECT_EQ("/usr/lib/libfoo.so", events[7].dso_and_offset.dso_name());
  EXPECT_EQ(0x700, events[7].dso_and_offset.offset());

  // Sample from Chrome #2 (within huge pages mapping).
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[8].event_ptr->header().type());
  EXPECT_EQ("/opt/google/chrome/chrome", events[8].dso_and_offset.dso_name());
  EXPECT_EQ(0x1300, events[8].dso_and_offset.offset());

  // Sample from Chrome #2 (after huge pages mapping).
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[9].event_ptr->header().type());
  EXPECT_EQ("/opt/google/chrome/chrome", events[9].dso_and_offset.dso_name());
  EXPECT_EQ(0x2f00, events[9].dso_and_offset.offset());
}

TEST(PerfParserTest, Regression62446346_Perf3_12_0_14) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // Perf infers the filename is "file", but at offset 0 for
  // hugepage-backed, anonymous mappings.
  //
  // vaddr start   - vaddr end     vaddr-size    elf-offset
  // [0x55bd43879000-0x55bd45000000) (0x 1787000) @ 0x       0]: file
  // [0x55bd45000000-0x55bd58c00000) (0x13c00000) @ 0x 1787000]: file
  // [0x55bd58c00000-0x55bd59800000) (0x  c00000) @ 0x15387000]: file
  // [0x55bd59800000-0x55bd59a00000) (0x  200000) @ 0x15f87000]: file
  // [0x55bd59a00000-0x55bd5b600000) (0x 1c00000) @ 0x16187000]: file
  // [0x55bd5b600000-0x55bd5b800000) (0x  200000) @ 0x17d87000]: file
  // [0x55bd5b800000-0x55bd5bcb5000) (0x  4b5000) @ 0x17f87000]: file

  testing::ExampleMmapEvent(1234, 0x55bd43879000, 0x1787000, 0, "file",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);
  testing::ExampleMmapEvent(1234, 0x55bd45000000, 0x13c00000, 0x1787000, "file",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);
  testing::ExampleMmapEvent(1234, 0x55bd58c00000, 0xc00000, 0x15387000, "file",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);
  testing::ExampleMmapEvent(1234, 0x55bd59800000, 0x200000, 0x15f87000, "file",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);
  testing::ExampleMmapEvent(1234, 0x55bd59a00000, 0x1c00000, 0x16187000, "file",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);
  testing::ExampleMmapEvent(1234, 0x55bd5b600000, 0x200000, 0x17d87000, "file",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);
  testing::ExampleMmapEvent(1234, 0x55bd5b800000, 0x4b5000, 0x17f87000, "file",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.deduce_huge_page_mappings = true;
  options.combine_mappings = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(1, parser.stats().num_mmap_events);
  EXPECT_EQ(0, parser.stats().num_sample_events);
  EXPECT_EQ(0, parser.stats().num_sample_events_mapped);

  PerfDataProto expected;
  {
    auto *ev = expected.add_events();
    ev->mutable_header()->set_type(PERF_RECORD_MMAP);
    ev->mutable_mmap_event()->set_filename("file");
    ev->mutable_mmap_event()->set_start(0x55bd43879000);
    ev->mutable_mmap_event()->set_len(0x55bd5bcb5000 - 0x55bd43879000);
    ev->mutable_mmap_event()->set_pgoff(0x0);
  }

  PerfDataProto actual;
  CopyActualEvents(parser.parsed_events(), &actual);
  std::string difference;
  EXPECT_TRUE(PartiallyEqualsProto(actual, expected, &difference))
      << difference;
  EXPECT_EQ(1, parser.parsed_events().size());
}

TEST(PerfParserTest, DiscontiguousMappings) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP | PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // vaddr start   - vaddr end     vaddr-size    elf-offset
  // [0x7f489000-0x80200000) (0xd77000)   @ 0]:          file
  // [0x80200000-0x80400000) (0x200000)   @ 0]:          file
  // [0x80400000-0x80474000) (0x47000)    @ 0x1a00000]:  file
  testing::ExampleMmapEvent(1234, 0x7f489000, 0xd77000, 0, "file",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);
  testing::ExampleMmapEvent(1234, 0x80200000, 0x200000, 0, "file",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);
  testing::ExampleMmapEvent(1234, 0x80400000, 0x47000, 0x1a00000, "file",
                            testing::SampleInfo().Tid(1234, 1234))
      .WriteTo(&input);

  //
  // Parse input.
  //

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.deduce_huge_page_mappings = true;
  options.combine_mappings = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());

  EXPECT_EQ(3, parser.stats().num_mmap_events);
  EXPECT_EQ(0, parser.stats().num_sample_events);
  EXPECT_EQ(0, parser.stats().num_sample_events_mapped);

  // The first two mappings should not combine, since we cannot know if the
  // middle one follows the first, or preceeds the last in the binary.
  PerfDataProto expected;
  {
    auto *ev = expected.add_events();
    ev->mutable_header()->set_type(PERF_RECORD_MMAP);
    ev->mutable_mmap_event()->set_filename("file");
    ev->mutable_mmap_event()->set_start(0x7f489000);
    ev->mutable_mmap_event()->set_len(0xd77000);
    ev->mutable_mmap_event()->set_pgoff(0x0);
  }
  {
    auto *ev = expected.add_events();
    ev->mutable_header()->set_type(PERF_RECORD_MMAP);
    ev->mutable_mmap_event()->set_filename("file");
    ev->mutable_mmap_event()->set_start(0x80200000);
    ev->mutable_mmap_event()->set_len(0x200000);
    ev->mutable_mmap_event()->set_pgoff(0x0);
  }
  {
    auto *ev = expected.add_events();
    ev->mutable_header()->set_type(PERF_RECORD_MMAP);
    ev->mutable_mmap_event()->set_filename("file");
    ev->mutable_mmap_event()->set_start(0x80400000);
    ev->mutable_mmap_event()->set_len(0x47000);
    ev->mutable_mmap_event()->set_pgoff(0x1a00000);
  }

  PerfDataProto actual;
  CopyActualEvents(parser.parsed_events(), &actual);

  std::string difference;
  EXPECT_TRUE(PartiallyEqualsProto(actual, expected, &difference))
      << difference;
  EXPECT_EQ(3, parser.parsed_events().size());
}

TEST(PerfParserTest, BranchStackEntries) {
  std::stringstream input;

  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);
  testing::ExamplePerfEventAttrEvent_Hardware(
      PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_BRANCH_STACK,
      true /*sample_id_all*/)
      .WriteTo(&input);
  uint64_t base_addr = 0x1c1000;
  testing::ExampleMmapEvent(1001, base_addr, 0x1000, 0, "/usr/lib/foo.so",
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);  // 0
  testing::ExampleMmapEvent(1001, 0x1c3000, 0x2000, 0x2000, "/usr/lib/bar.so",
                            testing::SampleInfo().Tid(1001))
      .WriteTo(&input);

  uint16_t expected_cycles1 = 0xa001;
  uint16_t expected_cycles2 = 0x0002;
  uint64_t flags1 = 1 << 1                    // target predicted
                    | expected_cycles1 << 4;  // cycles
  uint64_t flags2 = 1                         // target mispredicted
                    | expected_cycles2 << 4;  // cycles

  uint64_t expected_offset1_from = 0x0;
  uint64_t expected_offset1_to = 0x8;
  uint64_t expected_offset2_from = 0x2;
  uint64_t expected_offset2_to = 0x12;

  testing::ExamplePerfSampleEvent(
      quipper::testing::SampleInfo()
          .Ip(0x1212)
          .Tid(1001)
          .BranchStack_nr(2)
          .BranchStack_lbr(base_addr + expected_offset1_from,
                           base_addr + expected_offset1_to, flags1)
          .BranchStack_lbr(base_addr + expected_offset2_from,
                           base_addr + expected_offset2_to, flags2))
      .WriteTo(&input);

  PerfReader reader;
  EXPECT_TRUE(reader.ReadFromString(input.str()));

  PerfParserOptions options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(&reader, options);
  EXPECT_TRUE(parser.ParseRawEvents());
  ASSERT_EQ(3, parser.parsed_events().size());
  ASSERT_EQ(2, parser.parsed_events()[2].branch_stack.size());

  auto branch1 = parser.parsed_events()[2].branch_stack[0];
  EXPECT_EQ(expected_offset1_from, branch1.from.offset());
  EXPECT_EQ("/usr/lib/foo.so", branch1.from.dso_name());
  EXPECT_EQ(expected_offset1_to, branch1.to.offset());
  EXPECT_EQ("/usr/lib/foo.so", branch1.to.dso_name());
  EXPECT_FALSE(branch1.mispredicted);
  EXPECT_TRUE(branch1.predicted);
  EXPECT_EQ(expected_cycles1, branch1.cycles);

  auto branch2 = parser.parsed_events()[2].branch_stack[1];
  EXPECT_EQ(expected_offset2_from, branch2.from.offset());
  EXPECT_EQ("/usr/lib/foo.so", branch2.from.dso_name());
  EXPECT_EQ(expected_offset2_to, branch2.to.offset());
  EXPECT_EQ("/usr/lib/foo.so", branch2.to.dso_name());
  EXPECT_TRUE(branch2.mispredicted);
  EXPECT_FALSE(branch2.predicted);
  EXPECT_EQ(expected_cycles2, branch2.cycles);
}
}  // namespace quipper
