// Copyright 2025 VDR-Light Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// @file test_dds_wrapper.cpp
/// @brief Unit tests for DDS wrapper

#include "common/dds_wrapper.hpp"
#include "common/qos_profiles.hpp"
#include "common/time_utils.hpp"
#include "telemetry.h"
#include "vss_signal.h"
#include "vss_types.h"

#include <gtest/gtest.h>
#include <glog/logging.h>

#include <thread>
#include <chrono>

class DdsWrapperTest : public ::testing::Test {
protected:
    void SetUp() override {
        google::InitGoogleLogging("test");
    }

    void TearDown() override {
        google::ShutdownGoogleLogging();
    }
};

TEST_F(DdsWrapperTest, ParticipantCreation) {
    dds::Participant participant(DDS_DOMAIN_DEFAULT);
    EXPECT_TRUE(participant);
    EXPECT_GT(participant.get(), 0);
}

TEST_F(DdsWrapperTest, TopicCreation) {
    dds::Participant participant(DDS_DOMAIN_DEFAULT);

    dds::Topic topic(participant, &vss_Signal_desc, "test/vss/signals");
    EXPECT_TRUE(topic);
    EXPECT_EQ(topic.name(), "test/vss/signals");
}

TEST_F(DdsWrapperTest, WriterCreation) {
    dds::Participant participant(DDS_DOMAIN_DEFAULT);
    dds::Topic topic(participant, &vss_Signal_desc, "test/writer");
    dds::Writer writer(participant, topic);

    EXPECT_TRUE(writer);
}

TEST_F(DdsWrapperTest, ReaderCreation) {
    dds::Participant participant(DDS_DOMAIN_DEFAULT);
    dds::Topic topic(participant, &vss_Signal_desc, "test/reader");
    dds::Reader reader(participant, topic);

    EXPECT_TRUE(reader);
}

TEST_F(DdsWrapperTest, QosProfiles) {
    auto critical = dds::qos_profiles::reliable_critical();
    EXPECT_NE(critical.get(), nullptr);

    auto standard = dds::qos_profiles::reliable_standard(50);
    EXPECT_NE(standard.get(), nullptr);

    auto best_effort = dds::qos_profiles::best_effort(10);
    EXPECT_NE(best_effort.get(), nullptr);
}

TEST_F(DdsWrapperTest, WriteAndRead) {
    dds::Participant participant(DDS_DOMAIN_DEFAULT);

    auto qos = dds::qos_profiles::reliable_standard(10);
    dds::Topic topic(participant, &vss_Signal_desc,
                     "test/pubsub", qos.get());

    dds::Writer writer(participant, topic, qos.get());
    dds::Reader reader(participant, topic, qos.get());

    // Give time for discovery
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Write a sample
    vss_Signal msg = {};
    msg.path = const_cast<char*>("Vehicle.Speed");
    msg.header.source_id = const_cast<char*>("test");
    msg.header.timestamp_ns = utils::now_ns();
    msg.header.seq_num = 1;
    msg.header.correlation_id = const_cast<char*>("");
    msg.quality = vss_types_QUALITY_VALID;
    msg.value.type = vss_types_VALUE_TYPE_DOUBLE;
    msg.value.double_value = 42.0;

    writer.write(msg);

    // Give time for delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Read samples using callback (safe for string access)
    size_t count = 0;
    reader.take_each<vss_Signal>([&](const vss_Signal& sample) {
        EXPECT_STREQ(sample.path, "Vehicle.Speed");
        EXPECT_DOUBLE_EQ(sample.value.double_value, 42.0);
        ++count;
    }, 10);

    EXPECT_EQ(count, 1);
}

TEST_F(DdsWrapperTest, TimeUtils) {
    int64_t t1 = utils::now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    int64_t t2 = utils::now_ns();

    EXPECT_GT(t2, t1);
    EXPECT_GT(t2 - t1, 9000000);  // At least 9ms in nanoseconds
}

TEST_F(DdsWrapperTest, UuidGeneration) {
    std::string uuid1 = utils::generate_uuid();
    std::string uuid2 = utils::generate_uuid();

    EXPECT_NE(uuid1, uuid2);
    EXPECT_EQ(uuid1.length(), 36);  // Standard UUID format
}

TEST_F(DdsWrapperTest, EntityMoveSemantics) {
    dds::Participant p1(DDS_DOMAIN_DEFAULT);
    EXPECT_TRUE(p1);

    dds_entity_t handle = p1.get();

    dds::Participant p2(std::move(p1));
    EXPECT_TRUE(p2);
    EXPECT_EQ(p2.get(), handle);
}
