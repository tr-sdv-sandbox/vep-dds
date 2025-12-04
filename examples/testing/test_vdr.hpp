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

/// @file testing/test_vdr.hpp
/// @brief Controllable VDR instance for integration testing

#include "common/dds_wrapper.hpp"
#include "vdr/subscriber.hpp"
#include "vdr/output_sink.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace vdr {
namespace testing {

/// Controllable VDR instance for testing probes.
/// Creates a VDR with a configurable output sink.
class TestVdr {
public:
    /// Create a test VDR
    /// @param domain_id DDS domain (default 0)
    explicit TestVdr(dds_domainid_t domain_id = DDS_DOMAIN_DEFAULT);

    ~TestVdr();

    // Non-copyable
    TestVdr(const TestVdr&) = delete;
    TestVdr& operator=(const TestVdr&) = delete;

    /// Start the VDR with the given output sink
    /// @param sink Output sink (ownership transferred)
    /// @param config Subscription configuration
    /// @return true if started successfully
    bool start(std::unique_ptr<OutputSink> sink,
               const SubscriptionConfig& config = SubscriptionConfig{});

    /// Stop the VDR
    void stop();

    /// Restart with the same sink and config
    bool restart();

    /// Restart with a new sink
    bool restart(std::unique_ptr<OutputSink> sink);

    /// Check if running
    bool running() const { return running_; }

    /// Get the current output sink (for test assertions)
    OutputSink* sink() { return sink_.get(); }

    /// Check if VDR is healthy
    bool healthy() const;

private:
    dds_domainid_t domain_id_;
    SubscriptionConfig config_;

    std::atomic<bool> running_{false};

    std::unique_ptr<dds::Participant> participant_;
    std::unique_ptr<SubscriptionManager> subscriptions_;
    std::unique_ptr<OutputSink> sink_;
};

}  // namespace testing
}  // namespace vdr
