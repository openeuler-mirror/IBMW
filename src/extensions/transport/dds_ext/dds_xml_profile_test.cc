// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "dds_ext/dds_bus.h"

namespace ibmw::extensions::dds_extension {
namespace {

class TemporaryXmlProfile {
 public:
  explicit TemporaryXmlProfile(std::string profile_name)
      : path_(std::filesystem::temp_directory_path() /
              ("ibmw_dds_profile_" + profile_name + ".xml")) {}

  ~TemporaryXmlProfile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  TemporaryXmlProfile(const TemporaryXmlProfile&) = delete;
  TemporaryXmlProfile& operator=(const TemporaryXmlProfile&) = delete;

  void Write(const std::string& profile_name,
             const std::string& participant_name,
             bool with_locators = false) const {
    std::ofstream output(path_);
    ASSERT_TRUE(output.is_open());
    output << R"xml(<?xml version="1.0"?>
<dds xmlns="http://www.eprosima.com">
  <profiles>
    <transport_descriptors>
      <transport_descriptor>
        <transport_id>)xml"
           << profile_name << R"xml(_shm</transport_id>
        <type>SHM</type>
      </transport_descriptor>
      <transport_descriptor>
        <transport_id>)xml"
           << profile_name << R"xml(_udp</transport_id>
        <type>UDPv4</type>
      </transport_descriptor>
    </transport_descriptors>
    <participant profile_name=")xml"
           << profile_name << R"xml(" is_default_profile="false">
      <rtps>
        <name>)xml"
           << participant_name << R"xml(</name>
)xml";
    if (with_locators) {
      output << R"xml(        <defaultUnicastLocatorList>
          <locator>
            <udpv4>
              <address>192.168.1.2</address>
              <port>2019</port>
            </udpv4>
          </locator>
        </defaultUnicastLocatorList>
        <defaultMulticastLocatorList>
          <locator>
            <udpv4>
              <address>239.255.0.1</address>
              <port>2021</port>
            </udpv4>
          </locator>
        </defaultMulticastLocatorList>
        <builtin>
          <metatrafficUnicastLocatorList>
            <locator>
              <udpv4>
                <address>192.168.1.5</address>
                <port>9999</port>
              </udpv4>
            </locator>
          </metatrafficUnicastLocatorList>
          <metatrafficMulticastLocatorList>
            <locator>
              <udpv4>
                <address>239.255.0.2</address>
                <port>32</port>
              </udpv4>
            </locator>
          </metatrafficMulticastLocatorList>
          <initialPeersList>
            <locator>
              <udpv4>
                <address>192.168.1.6</address>
                <port>21120</port>
              </udpv4>
            </locator>
          </initialPeersList>
        </builtin>
)xml";
    }
    output << R"xml(        <useBuiltinTransports>false</useBuiltinTransports>
        <userTransports>
          <transport_id>)xml"
           << profile_name << R"xml(_shm</transport_id>
          <transport_id>)xml"
           << profile_name << R"xml(_udp</transport_id>
        </userTransports>
      </rtps>
    </participant>
  </profiles>
</dds>
)xml";
    output.close();
    ASSERT_TRUE(output.good());
  }

  std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

uint32_t UniqueDomain() {
  static std::atomic_uint32_t next_domain{180};
  return next_domain.fetch_add(1);
}

std::string UniqueName(const char* suffix) {
  static std::atomic_uint32_t next_name{0};
  return std::string("ibmw_dds_xml_") + suffix + "_" +
         std::to_string(next_name.fetch_add(1));
}

DdsBusXmlProfileConfig XmlConfig(const TemporaryXmlProfile& xml,
                                 const std::string& profile_name) {
  return {xml.path(), profile_name};
}

TEST(DdsXmlProfileTest, InitializeRejectsFileWithoutProfileName) {
  DdsBusXmlProfileConfig xml_config;
  xml_config.file = "/tmp/ibmw_dds_missing_profile.xml";
  DdsBus bus(UniqueDomain(), UniqueName("file_only"), {}, xml_config);

  EXPECT_THROW(bus.Initialize(), std::runtime_error);
}

TEST(DdsXmlProfileTest, InitializeRejectsProfileNameWithoutFile) {
  DdsBusXmlProfileConfig xml_config;
  xml_config.participant_profile = UniqueName("name_only");
  DdsBus bus(UniqueDomain(), UniqueName("name_only_bus"), {}, xml_config);

  EXPECT_THROW(bus.Initialize(), std::runtime_error);
}

TEST(DdsXmlProfileTest, InitializeRejectsMissingXmlFile) {
  DdsBusXmlProfileConfig xml_config;
  xml_config.file =
      (std::filesystem::temp_directory_path() / UniqueName("missing") /
       "profile.xml")
          .string();
  xml_config.participant_profile = UniqueName("missing_profile");
  DdsBus bus(UniqueDomain(), UniqueName("missing_bus"), {}, xml_config);

  EXPECT_THROW(bus.Initialize(), std::runtime_error);
}

TEST(DdsXmlProfileTest, InitializeRejectsUnknownProfileName) {
  const std::string profile_name = UniqueName("known");
  TemporaryXmlProfile xml(profile_name);
  xml.Write(profile_name, "xml_participant_name");

  DdsBus bus(UniqueDomain(), UniqueName("unknown_bus"), {},
             XmlConfig(xml, UniqueName("unknown_profile")));

  EXPECT_THROW(bus.Initialize(), std::runtime_error);
}

TEST(DdsXmlProfileTest, InitializeUsesMixedXmlProfileAndOverridesParticipantName) {
  const std::string profile_name = UniqueName("mixed");
  const std::string participant_name = UniqueName("participant");
  TemporaryXmlProfile xml(profile_name);
  xml.Write(profile_name, "name_from_xml");
  DdsBus bus(UniqueDomain(), participant_name, {},
             XmlConfig(xml, profile_name));

  ASSERT_NO_THROW(bus.Initialize());
  ASSERT_NE(bus.GetParticipant(), nullptr);

  eprosima::fastdds::dds::DomainParticipantQos qos;
  bus.GetParticipant()->get_qos(qos);
  EXPECT_EQ(qos.name(), participant_name);
  EXPECT_FALSE(qos.transport().use_builtin_transports);
  EXPECT_EQ(qos.transport().user_transports.size(), 2u);
}

TEST(DdsXmlProfileTest, ShmOnlyPolicyOverridesXmlTransports) {
  const std::string profile_name = UniqueName("policy");
  TemporaryXmlProfile xml(profile_name);
  xml.Write(profile_name, "name_from_xml", true);
  DdsBusTransportConfig config;
  config.policy = DdsBusTransportPolicy::kShmOnly;
  DdsBus bus(UniqueDomain(), UniqueName("policy_bus"), config,
             XmlConfig(xml, profile_name));

  ASSERT_NO_THROW(bus.Initialize());
  ASSERT_NE(bus.GetParticipant(), nullptr);

  eprosima::fastdds::dds::DomainParticipantQos qos;
  bus.GetParticipant()->get_qos(qos);
  EXPECT_TRUE(
      qos.wire_protocol().builtin.metatrafficUnicastLocatorList.empty());
  EXPECT_TRUE(
      qos.wire_protocol().builtin.metatrafficMulticastLocatorList.empty());
  EXPECT_TRUE(qos.wire_protocol().builtin.initialPeersList.empty());
  EXPECT_TRUE(qos.wire_protocol().default_unicast_locator_list.empty());
  EXPECT_TRUE(qos.wire_protocol().default_multicast_locator_list.empty());
  EXPECT_FALSE(qos.transport().use_builtin_transports);
  EXPECT_EQ(qos.transport().user_transports.size(), 1u);
}

}  // namespace
}  // namespace ibmw::extensions::dds_extension
