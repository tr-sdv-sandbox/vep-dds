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

/// @file test_integration.cpp
/// @brief Integration tests for VDR ecosystem

#include "testing/test_probe.hpp"
#include "testing/test_vdr.hpp"
#include "vdr/sinks/capture_sink.hpp"
#include "vdr/sinks/null_sink.hpp"

#include <gtest/gtest.h>
#include <glog/logging.h>

#include <chrono>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use unique domain per test to avoid interference
        domain_id_ = DDS_DOMAIN_DEFAULT;
    }

    void TearDown() override {
        // Ensure cleanup
        std::this_thread::sleep_for(50ms);
    }

    dds_domainid_t domain_id_;
};

// =============================================================================
// Happy Path Tests
// =============================================================================

TEST_F(IntegrationTest, ProbeToVdr_SingleSignal_Delivered) {
    // Setup VDR with capture sink
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    // Setup probe
    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    // Allow DDS discovery
    std::this_thread::sleep_for(200ms);

    // Send signal
    probe.send_signal("Vehicle.Speed", 42.5);

    // Wait for delivery
    ASSERT_TRUE(capture->wait_for_signals(1, 2000ms));

    // Verify
    auto signals = capture->signals();
    ASSERT_EQ(signals.size(), 1);
    EXPECT_EQ(signals[0].path, "Vehicle.Speed");
    EXPECT_DOUBLE_EQ(signals[0].double_value, 42.5);
    EXPECT_EQ(signals[0].source_id, "test_probe");

    // Cleanup
    probe.stop();
    vdr.stop();
}

TEST_F(IntegrationTest, ProbeToVdr_MultipleSignals_AllDelivered) {
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    std::this_thread::sleep_for(200ms);

    // Send multiple signals
    const int count = 10;
    for (int i = 0; i < count; ++i) {
        probe.send_signal("Vehicle.Test", static_cast<double>(i));
    }

    // Wait for all
    ASSERT_TRUE(capture->wait_for_signals(count, 2000ms));

    auto signals = capture->signals();
    EXPECT_EQ(signals.size(), count);

    probe.stop();
    vdr.stop();
}

TEST_F(IntegrationTest, ProbeToVdr_Event_Delivered) {
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    std::this_thread::sleep_for(200ms);

    // Send event
    probe.send_event("ADAS", "forward_collision_warning",
                     telemetry_events_SEVERITY_WARNING);

    // Wait for delivery
    ASSERT_TRUE(capture->wait_for_events(1, 2000ms));

    auto events = capture->events();
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].category, "ADAS");
    EXPECT_EQ(events[0].event_type, "forward_collision_warning");
    EXPECT_EQ(events[0].severity, telemetry_events_SEVERITY_WARNING);

    probe.stop();
    vdr.stop();
}

TEST_F(IntegrationTest, ProbeToVdr_SignalOrdering_Preserved) {
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    std::this_thread::sleep_for(200ms);

    // Send numbered signals
    const int count = 100;
    for (int i = 0; i < count; ++i) {
        probe.send_signal("Vehicle.Sequence", i);
    }

    ASSERT_TRUE(capture->wait_for_signals(count, 5000ms));

    auto signals = capture->signals();
    ASSERT_EQ(signals.size(), count);

    // Verify ordering by sequence number
    for (int i = 1; i < count; ++i) {
        EXPECT_GT(signals[i].seq_num, signals[i-1].seq_num)
            << "Sequence broken at index " << i;
    }

    probe.stop();
    vdr.stop();
}

// =============================================================================
// Resilience Tests
// =============================================================================

TEST_F(IntegrationTest, VdrRestart_ProbeReconnects) {
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    std::this_thread::sleep_for(200ms);

    // Send before restart
    probe.send_signal("Vehicle.Before", 1.0);
    ASSERT_TRUE(capture->wait_for_signals(1, 2000ms));

    // Restart VDR with new sink
    auto sink2 = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture2 = sink2.get();
    ASSERT_TRUE(vdr.restart(std::move(sink2)));

    std::this_thread::sleep_for(200ms);

    // Send after restart
    probe.send_signal("Vehicle.After", 2.0);
    ASSERT_TRUE(capture2->wait_for_signals(1, 2000ms));

    auto signals = capture2->signals();
    ASSERT_GE(signals.size(), 1);
    EXPECT_EQ(signals.back().path, "Vehicle.After");

    probe.stop();
    vdr.stop();
}

TEST_F(IntegrationTest, ProbeRestart_VdrContinuesReceiving) {
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    std::this_thread::sleep_for(200ms);

    // Send before restart
    probe.send_signal("Vehicle.Before", 1.0);
    ASSERT_TRUE(capture->wait_for_signals(1, 2000ms));

    // Restart probe
    ASSERT_TRUE(probe.restart());

    std::this_thread::sleep_for(200ms);

    // Send after restart
    probe.send_signal("Vehicle.After", 2.0);
    ASSERT_TRUE(capture->wait_for_signals(2, 2000ms));

    probe.stop();
    vdr.stop();
}

TEST_F(IntegrationTest, MultipleProbes_AllReceived) {
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe1("probe_1", domain_id_);
    vdr::testing::TestProbe probe2("probe_2", domain_id_);
    vdr::testing::TestProbe probe3("probe_3", domain_id_);

    ASSERT_TRUE(probe1.start());
    ASSERT_TRUE(probe2.start());
    ASSERT_TRUE(probe3.start());

    std::this_thread::sleep_for(200ms);

    // Each probe sends signals
    probe1.send_signal("Vehicle.Probe1", 1.0);
    probe2.send_signal("Vehicle.Probe2", 2.0);
    probe3.send_signal("Vehicle.Probe3", 3.0);

    ASSERT_TRUE(capture->wait_for_signals(3, 2000ms));

    auto signals = capture->signals();
    EXPECT_EQ(signals.size(), 3);

    // Verify all sources present
    std::set<std::string> sources;
    for (const auto& sig : signals) {
        sources.insert(sig.source_id);
    }
    EXPECT_TRUE(sources.count("probe_1") > 0);
    EXPECT_TRUE(sources.count("probe_2") > 0);
    EXPECT_TRUE(sources.count("probe_3") > 0);

    probe1.stop();
    probe2.stop();
    probe3.stop();
    vdr.stop();
}

// =============================================================================
// Load Tests
// =============================================================================

TEST_F(IntegrationTest, Burst_500Messages_Delivered) {
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    std::this_thread::sleep_for(200ms);

    // Send burst with small interval to avoid DDS queue overflow
    // (default QoS has history depth of 100)
    const size_t count = 500;
    probe.send_signal_burst(count, 1ms);

    // Allow time for delivery
    ASSERT_TRUE(capture->wait_for_signals(count, 10000ms));

    EXPECT_EQ(capture->signals().size(), count);
    EXPECT_TRUE(vdr.healthy());

    probe.stop();
    vdr.stop();
}

TEST_F(IntegrationTest, Sustained_100PerSecond_NoLoss) {
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    std::this_thread::sleep_for(200ms);

    // 100 msg/sec for 2 seconds = 200 messages
    const size_t count = 200;
    probe.send_signal_burst(count, 10ms);

    ASSERT_TRUE(capture->wait_for_signals(count, 10000ms));

    EXPECT_EQ(capture->signals().size(), count);
    EXPECT_TRUE(vdr.healthy());

    probe.stop();
    vdr.stop();
}

TEST_F(IntegrationTest, NullSink_HighThroughput) {
    // Test with null sink to measure raw throughput
    auto sink = std::make_unique<vdr::sinks::NullSink>();
    auto* null_sink = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    std::this_thread::sleep_for(200ms);

    // Send as fast as possible
    const size_t count = 10000;
    auto start = std::chrono::steady_clock::now();
    probe.send_signal_burst(count, 0ms);
    auto end = std::chrono::steady_clock::now();

    // Wait for processing
    std::this_thread::sleep_for(1000ms);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    auto stats = null_sink->stats();

    LOG(INFO) << "Sent " << count << " messages in " << elapsed.count() << "ms";
    LOG(INFO) << "Throughput: " << (count * 1000.0 / elapsed.count()) << " msg/sec";
    LOG(INFO) << "Received: " << stats.messages_sent;

    EXPECT_TRUE(vdr.healthy());

    probe.stop();
    vdr.stop();
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TEST_F(IntegrationTest, EmptyStrings_Handled) {
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    std::this_thread::sleep_for(200ms);

    // Send signal with empty path (edge case)
    probe.send_signal("", 0.0);

    ASSERT_TRUE(capture->wait_for_signals(1, 2000ms));

    auto signals = capture->signals();
    ASSERT_EQ(signals.size(), 1);
    EXPECT_EQ(signals[0].path, "");

    probe.stop();
    vdr.stop();
}

TEST_F(IntegrationTest, AllValueTypes_Handled) {
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    std::this_thread::sleep_for(200ms);

    // Send different value types
    probe.send_signal("Vehicle.Bool", true);
    probe.send_signal("Vehicle.Int", 42);
    probe.send_signal("Vehicle.Double", 3.14159);
    probe.send_signal("Vehicle.String", std::string("hello"));

    ASSERT_TRUE(capture->wait_for_signals(4, 2000ms));

    auto signals = capture->signals();
    EXPECT_EQ(signals.size(), 4);

    probe.stop();
    vdr.stop();
}

TEST_F(IntegrationTest, QualityFlags_Preserved) {
    auto sink = std::make_unique<vdr::sinks::CaptureSink>();
    auto* capture = sink.get();

    vdr::testing::TestVdr vdr(domain_id_);
    ASSERT_TRUE(vdr.start(std::move(sink)));

    vdr::testing::TestProbe probe("test_probe", domain_id_);
    ASSERT_TRUE(probe.start());

    std::this_thread::sleep_for(200ms);

    // Send signals with different quality
    probe.send_signal("Vehicle.Valid", 1.0, telemetry_vss_QUALITY_VALID);
    probe.send_signal("Vehicle.Invalid", 2.0, telemetry_vss_QUALITY_INVALID);
    probe.send_signal("Vehicle.NotAvailable", 3.0, telemetry_vss_QUALITY_NOT_AVAILABLE);

    ASSERT_TRUE(capture->wait_for_signals(3, 2000ms));

    auto signals = capture->signals();
    ASSERT_EQ(signals.size(), 3);

    // Find and verify each quality
    for (const auto& sig : signals) {
        if (sig.path == "Vehicle.Valid") {
            EXPECT_EQ(sig.quality, telemetry_vss_QUALITY_VALID);
        } else if (sig.path == "Vehicle.Invalid") {
            EXPECT_EQ(sig.quality, telemetry_vss_QUALITY_INVALID);
        } else if (sig.path == "Vehicle.NotAvailable") {
            EXPECT_EQ(sig.quality, telemetry_vss_QUALITY_NOT_AVAILABLE);
        }
    }

    probe.stop();
    vdr.stop();
}

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    google::SetStderrLogging(google::WARNING);  // Less verbose for tests
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
