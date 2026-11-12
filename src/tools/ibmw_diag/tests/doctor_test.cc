// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli/doctor/log_check.h"
#include "cli/doctor/module_check.h"
#include "cli/doctor/qos_check.h"
#include "gtest/gtest.h"

namespace ibmw::diag {
namespace {
namespace doctor = cli::doctor;
using Status = doctor::CheckStatus;
using Result = extension::ModuleLifecycleResult;

discovery::DiscoverySnapshot Pair() {
  discovery::DiscoveredEndpoint writer;
  writer.source = {.backend = discovery::DiscoveryBackend::kDds, .domain = "7"};
  writer.participant_id = "writer-process";
  writer.endpoint_id = "writer";
  writer.topic_name = "/camera";
  writer.type_name = "Image";
  writer.qos.policies = {{"reliability", "reliable"}, {"durability", "volatile"},
                         {"deadline", "infinite"}, {"liveliness", "automatic"},
                         {"liveliness_lease_duration", "infinite"}, {"history", "keep_last"},
                         {"depth", "10"}, {"ownership", "shared"}, {"partition", "[]"},
                         {"disable_positive_acks", "false"}, {"data_representation", "[]"},
                         {"dds_profile", "fastdds3"}, {"topic_kind", "unkeyed"}};
  auto reader = writer;
  reader.participant_id = "reader-process";
  reader.endpoint_id = "reader";
  reader.role = discovery::EndpointRole::kSubscriber;
  return {.endpoints = {writer, reader},
          .sources = {{.source = writer.source, .status = discovery::DiscoveryStatus::kReady}}};
}

Status Policy(const discovery::DiscoverySnapshot& snapshot, const std::string& name) {
  const auto checks = doctor::CheckQos(snapshot);
  const auto it = std::find_if(checks.begin(), checks.end(), [&](const auto& check) {
    return check.policy == name && !check.publisher.empty() && !check.subscriber.empty();
  });
  if (it == checks.end()) {
    ADD_FAILURE() << "Missing pair check for " << name;
    return Status::kUnknown;
  }
  return it->status;
}

TEST(DiagDoctorTopic, PresenceAndQosCompatibilityRemainSeparateQuestions) {
  auto snapshot = Pair();
  snapshot.endpoints[0].qos.policies["reliability"] = "best_effort";
  const auto topics = doctor::CheckTopics(snapshot);
  ASSERT_EQ(topics.size(), 1u);
  EXPECT_EQ(topics.front().status, Status::kOk);
  EXPECT_EQ(Policy(snapshot, "reliability"), Status::kWarning);
}

TEST(DiagDoctorTopic, DuplicateEndpointsDoNotInflatePublisherCounts) {
  auto snapshot = Pair();
  snapshot.endpoints.push_back(snapshot.endpoints.front());
  const auto checks = doctor::CheckTopics(snapshot);
  ASSERT_EQ(checks.size(), 1u);
  EXPECT_EQ(checks[0].publishers, 1u);
  EXPECT_EQ(checks[0].subscribers, 1u);
}

TEST(DiagDoctorTopic, MissingSubscriberWarnsOnlyAfterSuccessfulDiscovery) {
  auto snapshot = Pair();
  snapshot.endpoints.pop_back();
  EXPECT_EQ(doctor::CheckTopics(snapshot).front().status, Status::kWarning);
  for (auto status : {discovery::DiscoveryStatus::kDiscovering,
                      discovery::DiscoveryStatus::kUnavailable, discovery::DiscoveryStatus::kError}) {
    snapshot.sources[0].status = status;
    const auto checks = doctor::CheckTopics(snapshot);
    ASSERT_EQ(checks.size(), 1u);
    EXPECT_EQ(checks[0].status, Status::kUnknown);
  }
}

TEST(DiagDoctorTopic, HiddenEndpointsAreOptInAndUnknownTypesDoNotProveAbsence) {
  auto snapshot = Pair();
  snapshot.endpoints[0].hidden = true;
  EXPECT_EQ(doctor::CheckTopics(snapshot).front().status, Status::kSkipped);
  EXPECT_EQ(doctor::CheckTopics(snapshot, true).front().status, Status::kOk);
  snapshot = Pair();
  snapshot.endpoints[1].type_name.clear();
  const auto checks = doctor::CheckTopics(snapshot);
  EXPECT_TRUE(std::any_of(checks.begin(), checks.end(), [](const auto& c) { return c.status == Status::kUnknown; }));
  EXPECT_FALSE(std::any_of(checks.begin(), checks.end(), [](const auto& c) { return c.status == Status::kWarning; }));
}

TEST(DiagDoctorTopic, SameTopicInAnotherDomainCannotSupplyTheMissingSubscriber) {
  auto snapshot = Pair();
  snapshot.endpoints[1].source.domain = "8";
  snapshot.sources.push_back({.source = snapshot.endpoints[1].source, .status = discovery::DiscoveryStatus::kReady});
  const auto checks = doctor::CheckTopics(snapshot);
  EXPECT_EQ(std::count_if(checks.begin(), checks.end(), [](const auto& c) { return c.status == Status::kWarning; }), 1);
}

discovery::DiscoverySnapshot IdleRpcServer() {
  auto snapshot = Pair();
  snapshot.endpoints[0].topic_name = "ibmw_rpc/rr/diag/service";
  snapshot.endpoints[0].type_name = "Reply";
  snapshot.endpoints[1].topic_name = "ibmw_rpc/rq/diag/service";
  snapshot.endpoints[1].type_name = "Request";
  snapshot.endpoints[1].participant_id = snapshot.endpoints[0].participant_id;
  return snapshot;
}

Status ReplyStatus(const discovery::DiscoverySnapshot& snapshot, bool hidden = false) {
  for (const auto& group : doctor::CheckTopics(snapshot, hidden))
    if (group.topic == "ibmw_rpc/rr/diag/service") return group.status;
  ADD_FAILURE() << "RPC reply group missing";
  return Status::kUnknown;
}

TEST(DiagDoctorTopic, IdleRpcReplyRequiresItsOwnRequestReader) {
  auto snapshot = IdleRpcServer();
  EXPECT_EQ(ReplyStatus(snapshot), Status::kSkipped);
  snapshot.endpoints[1].participant_id = "another-server";
  EXPECT_EQ(ReplyStatus(snapshot), Status::kWarning);
  snapshot = IdleRpcServer();
  snapshot.endpoints[1].source.domain = "another-domain";
  EXPECT_EQ(ReplyStatus(snapshot), Status::kWarning);
  snapshot = IdleRpcServer();
  snapshot.endpoints[1].topic_name += "/another-service";
  EXPECT_EQ(ReplyStatus(snapshot), Status::kWarning);
  snapshot = IdleRpcServer();
  snapshot.endpoints[0].participant_id.clear();
  snapshot.endpoints[1].participant_id.clear();
  EXPECT_EQ(ReplyStatus(snapshot), Status::kWarning);
}

TEST(DiagDoctorTopic, HealthyRpcServerCannotHideAnOrphanReplyPublisher) {
  auto snapshot = IdleRpcServer();
  auto orphan = snapshot.endpoints[0];
  orphan.participant_id = "orphan";
  orphan.endpoint_id = "orphan-reply";
  snapshot.endpoints.push_back(orphan);
  EXPECT_EQ(ReplyStatus(snapshot), Status::kWarning);
  auto request = snapshot.endpoints[1];
  request.participant_id = orphan.participant_id;
  request.endpoint_id = "orphan-request";
  snapshot.endpoints.push_back(request);
  EXPECT_EQ(ReplyStatus(snapshot), Status::kSkipped);
}

TEST(DiagDoctorTopic, RpcRuleRetainsVisibilityAndBackendBoundaries) {
  auto snapshot = IdleRpcServer();
  snapshot.endpoints[1].hidden = true;
  EXPECT_EQ(ReplyStatus(snapshot), Status::kWarning);
  EXPECT_EQ(ReplyStatus(snapshot, true), Status::kSkipped);
  snapshot = IdleRpcServer();
  snapshot.sources[0].source.backend = discovery::DiscoveryBackend::kRos2;
  for (auto& endpoint : snapshot.endpoints) endpoint.source = snapshot.sources[0].source;
  EXPECT_EQ(ReplyStatus(snapshot), Status::kWarning);
  snapshot = IdleRpcServer();
  for (auto& endpoint : snapshot.endpoints) {
    endpoint.wire_topic_name = endpoint.topic_name;
    endpoint.topic_name.clear();
  }
  EXPECT_EQ(ReplyStatus(snapshot), Status::kSkipped);
}

TEST(DiagDoctorTopic, ActiveRpcReplyAndOrdinaryOrphansKeepNormalRules) {
  auto snapshot = IdleRpcServer();
  auto client = snapshot.endpoints[0];
  client.role = discovery::EndpointRole::kSubscriber;
  client.participant_id = "client";
  client.endpoint_id = "reply-reader";
  snapshot.endpoints.push_back(client);
  EXPECT_EQ(ReplyStatus(snapshot), Status::kOk);
  auto orphan = Pair().endpoints[0];
  orphan.endpoint_id = "ordinary-writer";
  snapshot.endpoints.push_back(orphan);
  const auto groups = doctor::CheckTopics(snapshot);
  EXPECT_EQ(std::count_if(groups.begin(), groups.end(), [](const auto& group) {
    return group.topic == "/camera" && group.status == Status::kWarning;
  }), 1);
}

TEST(DiagDoctorQos, ReliabilityAndDurabilityUseOfferedRequestedDirection) {
  for (const auto* policy : {"reliability", "durability"}) {
    const std::string low = std::string(policy) == "reliability" ? "best_effort" : "volatile";
    const std::string high = std::string(policy) == "reliability" ? "reliable" : "transient_local";
    for (int offered = 0; offered < 2; ++offered) {
      for (int requested = 0; requested < 2; ++requested) {
        auto snapshot = Pair();
        snapshot.endpoints[0].qos.policies[policy] = offered ? high : low;
        snapshot.endpoints[1].qos.policies[policy] = requested ? high : low;
        EXPECT_EQ(Policy(snapshot, policy), offered >= requested ? Status::kOk : Status::kWarning);
      }
    }
  }
}

TEST(DiagDoctorQos, DeadlineComparisonRetainsNanosecondPrecision) {
  auto snapshot = Pair();
  snapshot.endpoints[0].qos.policies["deadline"] = "1 s 1 ns";
  snapshot.endpoints[1].qos.policies["deadline"] = "1 s 0 ns";
  EXPECT_EQ(Policy(snapshot, "deadline"), Status::kWarning);
  std::swap(snapshot.endpoints[0].qos.policies["deadline"], snapshot.endpoints[1].qos.policies["deadline"]);
  EXPECT_EQ(Policy(snapshot, "deadline"), Status::kOk);
  snapshot.endpoints[0].qos.policies["deadline"] = "0 s 1000000000 ns";
  EXPECT_EQ(Policy(snapshot, "deadline"), Status::kUnknown);
}

TEST(DiagDoctorQos, MissingOrUnresolvedPoliciesNeverPass) {
  for (const auto* value : {"unknown", "system_default", "", "invalid"}) {
    auto snapshot = Pair();
    snapshot.endpoints[0].qos.policies["reliability"] = value;
    EXPECT_EQ(Policy(snapshot, "reliability"), Status::kUnknown);
  }
  auto snapshot = Pair();
  snapshot.endpoints[1].qos.policies.erase("durability");
  EXPECT_EQ(Policy(snapshot, "durability"), Status::kUnknown);
}

TEST(DiagDoctorQos, PartitionsAndOwnershipCanPreventMatching) {
  auto snapshot = Pair();
  snapshot.endpoints[0].qos.policies["partition"] = R"(["robot/*"])";
  snapshot.endpoints[1].qos.policies["partition"] = R"(["robot/camera"])";
  EXPECT_EQ(Policy(snapshot, "partition"), Status::kOk);
  snapshot.endpoints[1].qos.policies["partition"] = R"(["other/camera"])";
  EXPECT_EQ(Policy(snapshot, "partition"), Status::kWarning);
  snapshot.endpoints[1].qos.policies["ownership"] = "exclusive";
  EXPECT_EQ(Policy(snapshot, "ownership"), Status::kWarning);
}

TEST(DiagDoctorQos, AGoodReaderDoesNotHideAnIncompatibleReader) {
  auto snapshot = Pair();
  auto incompatible = snapshot.endpoints[1];
  incompatible.endpoint_id = "durable-reader";
  incompatible.qos.policies["durability"] = "transient_local";
  snapshot.endpoints.push_back(incompatible);
  snapshot.endpoints.push_back(incompatible);
  const auto checks = doctor::CheckQos(snapshot);
  EXPECT_EQ(std::count_if(checks.begin(), checks.end(), [](const auto& c) {
    return c.policy == "durability" && c.status == Status::kWarning;
  }), 1);
}

TEST(DiagDoctorLog, ComparisonRequiresAnExplicitBaseline) {
  EXPECT_EQ(doctor::CheckLogLevels({{"camera", "DEBUG"}}, {}).front().status, Status::kSkipped);
  EXPECT_EQ(doctor::CheckLogLevels({{"camera", "debug"}}, {{"camera", "DeBuG"}}).front().status, Status::kOk);
  EXPECT_EQ(doctor::CheckLogLevels({{"camera", "DEBUG"}}, {{"camera", "INFO"}}).front().status, Status::kWarning);
}

TEST(DiagDoctorLog, MissingInvalidAndOutOfScopeEvidenceIsUnknown) {
  EXPECT_EQ(doctor::CheckLogLevels({}, {{"camera", "INFO"}}).front().status, Status::kUnknown);
  EXPECT_EQ(doctor::CheckLogLevels({{"camera", "BAD"}}, {{"camera", "INFO"}}).front().status, Status::kUnknown);
  const auto checks = doctor::CheckLogLevels({{"extra", "INFO"}}, {}, {"camera"});
  ASSERT_EQ(checks.size(), 2u);
  for (const auto& check : checks) EXPECT_EQ(check.status, Status::kUnknown);
}

extension::ModuleLifecycleStatus Managed(Result initialize, Result start) {
  return {.module_name = "camera", .kind = extension::ModuleLifecycleKind::kManaged,
          .initialize = initialize, .start = start, .updated_at_unix_ms = 123};
}

TEST(DiagDoctorModule, RecordedSuccessDoesNotMutateTheInput) {
  std::vector<extension::ModuleLifecycleStatus> snapshot{Managed(Result::kSucceeded, Result::kSucceeded)};
  const auto before = snapshot;
  const auto checks = doctor::CheckModuleLifecycle(snapshot);
  ASSERT_EQ(checks.size(), 2u);
  EXPECT_EQ(checks[0].status, Status::kOk);
  EXPECT_EQ(checks[1].status, Status::kOk);
  EXPECT_EQ(checks[0].updated_at_unix_ms, 123u);
  EXPECT_EQ(snapshot, before);
}

TEST(DiagDoctorModule, FailureReasonBelongsOnlyToTheFailedPhase) {
  auto item = Managed(Result::kSucceeded, Result::kFailed);
  item.failure = {.phase = extension::ModuleLifecyclePhase::kStart, .reason = "device unavailable"};
  const auto checks = doctor::CheckModuleLifecycle({item});
  ASSERT_EQ(checks.size(), 2u);
  EXPECT_EQ(checks[0].status, Status::kOk);
  EXPECT_EQ(checks[0].message.find("device unavailable"), std::string::npos);
  EXPECT_EQ(checks[1].status, Status::kWarning);
  EXPECT_NE(checks[1].message.find("device unavailable"), std::string::npos);
}

TEST(DiagDoctorModule, UnfinishedAndAppContextPhasesAreNotFailures) {
  for (auto initialize : {Result::kNotAttempted, Result::kInProgress}) {
    const auto checks = doctor::CheckModuleLifecycle({Managed(initialize, Result::kNotAttempted)});
    ASSERT_EQ(checks.size(), 2u);
    for (const auto& check : checks) EXPECT_EQ(check.status, Status::kSkipped);
  }
  auto app = Managed(Result::kSucceeded, Result::kNotApplicable);
  app.kind = extension::ModuleLifecycleKind::kAppContext;
  const auto checks = doctor::CheckModuleLifecycle({app});
  EXPECT_EQ(checks[0].status, Status::kOk);
  EXPECT_EQ(checks[1].status, Status::kSkipped);
}

TEST(DiagDoctorModule, DuplicateNamesAndImpossiblePhaseCombinationsAreRejected) {
  auto item = Managed(Result::kSucceeded, Result::kSucceeded);
  EXPECT_THROW(doctor::CheckModuleLifecycle({item, item}), std::invalid_argument);
  item.module_name.clear();
  EXPECT_THROW(doctor::CheckModuleLifecycle({item}), std::invalid_argument);
  EXPECT_THROW(doctor::CheckModuleLifecycle({Managed(Result::kFailed, Result::kSucceeded)}), std::invalid_argument);
  item = Managed(Result::kSucceeded, static_cast<Result>(99));
  EXPECT_THROW(doctor::CheckModuleLifecycle({item}), std::invalid_argument);
}
}  // namespace
}  // namespace ibmw::diag
