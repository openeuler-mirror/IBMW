// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// EXT-030: record service handlers validate their input, report accurate
// error codes and never let an exception escape into the RPC layer.

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "engine/executor/executor_access.h"
#include "engine/executor/thread_pool_executor.h"
#include "record_playback_ext/service.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

namespace fs = std::filesystem;
using namespace ibmw::protocols::record_playback_extension;

constexpr std::string_view kExecutorName = "record-service-test";

class TestService : public RecordPlaybackServiceImpl {
 public:
  // Runs func_name through its registered service function; reports whether
  // the response callback ran.
  template <typename Req, typename Rsp>
  bool Call(std::string_view func_name, const Req& req, Rsp& rsp) {
    bool replied = false;
    for (auto& wrapper : service_func_wrapper_vec_) {
      if (wrapper.func_name != func_name) continue;
      ibmw::rpc::ServiceCallback callback([&replied](uint32_t) { replied = true; });
      wrapper.service_func(nullptr, &req, &rsp, callback.NativeHandle());
    }
    return replied;
  }
};

class RecordServiceTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("ibmw_record_service_test_" + std::to_string(::getpid()));
    fs::remove_all(root_);
    fs::create_directories(root_);
    executor_.Initialize(std::string(kExecutorName), YAML::Load("{thread_num: 1}"));
    executor_.Start();
    access_ = std::make_unique<runtime::core::executor::ExecutorAccess>(&executor_);
  }

  void TearDown() override {
    for (auto& [name, action] : actions_) action->Shutdown();
    executor_.Shutdown();
    fs::remove_all(root_);
  }

  ibmw::executor::ExecutorHandle Handle() const {
    return ibmw::executor::ExecutorHandle(access_->NativeHandle());
  }

  RecordAction& AddAction(std::string_view mode, bool start) {
    auto action = std::make_unique<RecordAction>();
    action->RegisterGetExecutorFunc([this](std::string_view name) {
      return name == kExecutorName ? Handle() : ibmw::executor::ExecutorHandle();
    });
    action->RegisterGetTypeSupportFunc([](std::string_view) { return ibmw::util::TypeSupportRef(); });
    action->RegisterGetSchemaSupportFunc(
        [](std::string_view, std::string_view) -> std::optional<ibmw::util::SchemaDescriptor> {
          return std::nullopt;
        });
    YAML::Node options = YAML::Load("{name: act, topic_meta_list: []}");
    options["mode"] = std::string(mode);
    options["executor"] = std::string(kExecutorName);
    options["bag_path"] = root_.string();
    action->Initialize(options);
    action->InitExecutor(Handle());
    if (start) action->Start();
    auto& ref = *action;
    actions_.emplace("act", std::move(action));
    service_.SetRecordActionMap(&actions_);
    return ref;
  }

  UpdateRecordActionRsp UpdateTopic(std::string topic_name, std::optional<double> sample_freq) {
    UpdateRecordActionReq req{.action_name = "act"};
    req.topic_metas.push_back(ProtoTopicMeta{
        .topic_name = std::move(topic_name), .msg_type = "t", .sample_freq = sample_freq});
    UpdateRecordActionRsp rsp;
    EXPECT_TRUE(service_.Call("UpdateRecordAction", req, rsp));
    return rsp;
  }

  fs::path root_;
  runtime::core::executor::ThreadPoolExecutor executor_;
  std::unique_ptr<runtime::core::executor::ExecutorAccess> access_;
  std::unordered_map<std::string_view, std::unique_ptr<RecordAction>> actions_;
  TestService service_;
};

TEST(RecordActionSampleFreqTest, Validation) {
  EXPECT_TRUE(RecordAction::IsValidSampleFreq(0));
  EXPECT_TRUE(RecordAction::IsValidSampleFreq(10));
  EXPECT_TRUE(RecordAction::IsValidSampleFreq(RecordAction::kMinSampleFreq));
  EXPECT_FALSE(RecordAction::IsValidSampleFreq(-1));
  EXPECT_FALSE(RecordAction::IsValidSampleFreq(1e-300));
  EXPECT_FALSE(RecordAction::IsValidSampleFreq(std::nan("")));
  EXPECT_FALSE(RecordAction::IsValidSampleFreq(std::numeric_limits<double>::infinity()));
  EXPECT_EQ(RecordAction::SampleIntervalNs(0), 0u);
  EXPECT_EQ(RecordAction::SampleIntervalNs(10), 100000000u);
}

// EXT-020: publisher metadata from the peer is parsed without throwing.
TEST(RecordActionPubMetaTest, MalformedValuesAreRejected) {
  EXPECT_EQ(RecordAction::ParsePubMeta(""), 0u);
  EXPECT_EQ(RecordAction::ParsePubMeta("1700000000123456789"), 1700000000123456789u);
  EXPECT_EQ(RecordAction::ParsePubMeta("18446744073709551615"), UINT64_MAX);
  for (std::string_view bad : {"abc", "12x", "-1", " 1", "18446744073709551616"}) {
    EXPECT_FALSE(RecordAction::ParsePubMeta(bad)) << bad;
  }
}

TEST_F(RecordServiceTest, InvalidSampleFreqIsRejected) {
  AddAction("signal", true);
  for (double freq : {-1.0, 1e-300, std::nan(""), std::numeric_limits<double>::infinity()}) {
    EXPECT_EQ(UpdateTopic("/missing", freq).common_rsp.msg, "INVALID_ARGUMENT") << freq;
  }
}

TEST_F(RecordServiceTest, UnknownTopicIsReported) {
  AddAction("signal", true);
  EXPECT_EQ(UpdateTopic("/missing", std::nullopt).common_rsp.msg, "TOPIC_NOT_FOUND");
  // Invalid UTF-8 in the request is logged without throwing.
  EXPECT_EQ(UpdateTopic("\xff\xfe", 1.0).common_rsp.msg, "TOPIC_NOT_FOUND");

  UpdateRecordActionReq req{.action_name = "act", .record_enabled = false};
  UpdateRecordActionRsp rsp;
  EXPECT_TRUE(service_.Call("UpdateRecordAction", req, rsp));
  EXPECT_EQ(rsp.common_rsp.code, 0u);
}

TEST_F(RecordServiceTest, ActionThatIsNotRunningIsReported) {
  AddAction("signal", false);
  EXPECT_EQ(UpdateTopic("/missing", std::nullopt).common_rsp.msg, "ACTION_NOT_RUNNING");
}

TEST_F(RecordServiceTest, HandlerExceptionStillReplies) {
  AddAction("signal", false);  // StartSignalRecord requires the Start state.
  StartRecordRsp rsp{.filefolder = "partial"};  // stands in for a half-filled answer
  EXPECT_TRUE(service_.Call("StartRecord", StartRecordReq{.action_name = "act"}, rsp));
  EXPECT_EQ(rsp.common_rsp.msg, "INTERNAL_ERROR");
  EXPECT_EQ(rsp.filefolder, "");
}

TEST_F(RecordServiceTest, OversizedMetadataIsRejected) {
  AddAction("signal", true);
  CommonRsp rsp;
  UpdateMetadataReq big_value{.action_name = "act"};
  big_value.kv_pairs["k"] = std::string(RecordPlaybackServiceImpl::kMaxMetadataValueSize + 1, 'a');
  EXPECT_TRUE(service_.Call("UpdateMetadata", big_value, rsp));
  EXPECT_EQ(rsp.msg, "INVALID_ARGUMENT");

  UpdateMetadataReq big_key{.action_name = "act"};
  big_key.kv_pairs[std::string(RecordPlaybackServiceImpl::kMaxMetadataKeySize + 1, 'k')] = "v";
  rsp = {};
  EXPECT_TRUE(service_.Call("UpdateMetadata", big_key, rsp));
  EXPECT_EQ(rsp.msg, "INVALID_ARGUMENT");

  UpdateMetadataReq many{.action_name = "act"};
  for (size_t i = 0; i <= RecordPlaybackServiceImpl::kMaxMetadataPairs; ++i) {
    many.kv_pairs["k" + std::to_string(i)] = "v";
  }
  rsp = {};
  EXPECT_TRUE(service_.Call("UpdateMetadata", many, rsp));
  EXPECT_EQ(rsp.msg, "INVALID_ARGUMENT");

  many.kv_pairs.erase("k0");
  rsp = {};
  EXPECT_TRUE(service_.Call("UpdateMetadata", many, rsp));
  EXPECT_EQ(rsp.code, 0u);
}

TEST_F(RecordServiceTest, ExtraAttributeCountIsBounded) {
  auto& action = AddAction("imd", true);
  std::unordered_map<std::string, std::string> kv_pairs;
  for (size_t i = 0; i < RecordAction::kMaxExtraAttributes + 100; ++i) {
    kv_pairs["k" + std::to_string(i)] = "v";
  }
  action.UpdateMetadata(std::move(kv_pairs));
  // Existing keys can still be updated at the limit.
  action.UpdateMetadata({{"k0", "updated"}});
  // Waits for the queued updates: the action executor is single threaded.
  EXPECT_EQ(action.UpdateTopicMetaRecord({}, std::nullopt), RecordAction::UpdateResult::kOk);
  action.Shutdown();

  for (const auto& bag : fs::directory_iterator(root_)) {
    const auto node = YAML::LoadFile((bag.path() / "metadata.yaml").string());
    const auto attributes = node["ibmw_bagfile_information"]["extra_attributes"];
    ASSERT_TRUE(attributes.IsMap());
    EXPECT_EQ(attributes.size(), RecordAction::kMaxExtraAttributes);
    EXPECT_EQ(attributes["k0"].as<std::string>(), "updated");
    return;
  }
  FAIL() << "no bag folder was created";
}

// EXT-027: metadata updated before the bag folder exists is kept in memory
// and must not leave a ".tmp" file in the working directory.
TEST_F(RecordServiceTest, MetadataBeforeBagOpenWritesNoFile) {
  const auto cwd = root_ / "cwd";
  fs::create_directories(cwd);
  const auto previous_cwd = fs::current_path();
  fs::current_path(cwd);
  auto& action = AddAction("signal", true);
  action.UpdateMetadata({{"k", "v"}});
  EXPECT_EQ(action.UpdateTopicMetaRecord({}, std::nullopt), RecordAction::UpdateResult::kOk);
  action.Shutdown();
  fs::current_path(previous_cwd);
  EXPECT_TRUE(fs::is_empty(cwd));
}

// EXT-027: metadata.yaml is replaced by a rename, never rewritten in place,
// and a failed write keeps the previous file.
TEST_F(RecordServiceTest, MetadataIsReplacedAtomically) {
  auto& action = AddAction("imd", true);
  const auto wait = [&action] {  // the action executor is single threaded
    EXPECT_EQ(action.UpdateTopicMetaRecord({}, std::nullopt), RecordAction::UpdateResult::kOk);
  };
  wait();
  fs::path metadata;
  for (const auto& bag : fs::directory_iterator(root_)) metadata = bag.path() / "metadata.yaml";
  ASSERT_TRUE(fs::exists(metadata));
  const auto inode = [&metadata] {
    struct stat st {};
    EXPECT_EQ(::stat(metadata.c_str(), &st), 0);
    return st.st_ino;
  };
  const auto read = [&metadata] {
    std::ifstream in(metadata);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };

  const auto before = inode();
  action.UpdateMetadata({{"k", "v1"}});
  wait();
  EXPECT_NE(inode(), before);
  EXPECT_NE(read().find("v1"), std::string::npos);

  // The temporary file can not be created: the old content stays.
  auto tmp = metadata;
  tmp += ".tmp";
  ASSERT_TRUE(fs::create_directory(tmp));
  const auto old_content = read();
  action.UpdateMetadata({{"k", "v2"}});
  wait();
  EXPECT_EQ(read(), old_content);
  fs::remove(tmp);
  action.Shutdown();
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
