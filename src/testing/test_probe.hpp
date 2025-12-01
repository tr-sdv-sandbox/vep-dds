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

#pragma once

/// @file testing/test_probe.hpp
/// @brief Controllable probe for integration testing

#include "common/dds_wrapper.hpp"
#include "common/qos_profiles.hpp"
#include "common/time_utils.hpp"
#include "telemetry.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace vdr {
namespace testing {

/// Controllable probe for testing VDR.
/// Can send individual messages or bursts of messages.
class TestProbe {
public:
    /// Create a test probe
    /// @param source_id Identifier for this probe (appears in message headers)
    /// @param domain_id DDS domain (default 0)
    explicit TestProbe(const std::string& source_id = "test_probe",
                       dds_domainid_t domain_id = DDS_DOMAIN_DEFAULT);

    ~TestProbe();

    // Non-copyable
    TestProbe(const TestProbe&) = delete;
    TestProbe& operator=(const TestProbe&) = delete;

    /// Start the probe (creates DDS entities)
    bool start();

    /// Stop the probe
    void stop();

    /// Restart the probe (stop + start)
    bool restart();

    /// Check if running
    bool running() const { return running_; }

    /// @name Signal sending
    /// @{

    /// Send a VSS signal with double value
    void send_signal(const std::string& path, double value,
                     telemetry_vss_Quality quality = telemetry_vss_QUALITY_VALID);

    /// Send a VSS signal with string value
    void send_signal(const std::string& path, const std::string& value,
                     telemetry_vss_Quality quality = telemetry_vss_QUALITY_VALID);

    /// Send a VSS signal with int value
    void send_signal(const std::string& path, int32_t value,
                     telemetry_vss_Quality quality = telemetry_vss_QUALITY_VALID);

    /// Send a VSS signal with bool value
    void send_signal(const std::string& path, bool value,
                     telemetry_vss_Quality quality = telemetry_vss_QUALITY_VALID);

    /// @}

    /// @name Event sending
    /// @{

    /// Send a vehicle event
    void send_event(const std::string& category,
                    const std::string& event_type,
                    telemetry_events_Severity severity = telemetry_events_SEVERITY_INFO,
                    const std::vector<uint8_t>& payload = {});

    /// @}

    /// @name Burst sending
    /// @{

    /// Send a burst of signals
    /// @param count Number of signals to send
    /// @param interval Time between signals (0 = as fast as possible)
    void send_signal_burst(size_t count,
                           std::chrono::milliseconds interval = std::chrono::milliseconds(0));

    /// Send a burst of events
    void send_event_burst(size_t count,
                          std::chrono::milliseconds interval = std::chrono::milliseconds(0));

    /// @}

    /// @name Statistics
    /// @{

    uint64_t signals_sent() const { return signals_sent_; }
    uint64_t events_sent() const { return events_sent_; }
    uint32_t sequence() const { return seq_; }

    /// @}

private:
    std::string source_id_;
    dds_domainid_t domain_id_;

    std::atomic<bool> running_{false};
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint64_t> signals_sent_{0};
    std::atomic<uint64_t> events_sent_{0};

    std::unique_ptr<dds::Participant> participant_;
    std::unique_ptr<dds::Topic> topic_signal_;
    std::unique_ptr<dds::Topic> topic_event_;
    std::unique_ptr<dds::Writer> writer_signal_;
    std::unique_ptr<dds::Writer> writer_event_;
};

}  // namespace testing
}  // namespace vdr
