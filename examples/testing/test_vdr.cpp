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

#include "testing/test_vdr.hpp"

#include <glog/logging.h>

#include <thread>

namespace vdr {
namespace testing {

TestVdr::TestVdr(dds_domainid_t domain_id)
    : domain_id_(domain_id) {}

TestVdr::~TestVdr() {
    stop();
}

bool TestVdr::start(std::unique_ptr<OutputSink> sink,
                    const SubscriptionConfig& config) {
    if (running_) {
        return true;
    }

    config_ = config;
    sink_ = std::move(sink);

    if (!sink_->start()) {
        LOG(ERROR) << "TestVdr: Failed to start sink";
        return false;
    }

    try {
        participant_ = std::make_unique<dds::Participant>(domain_id_);
        subscriptions_ = std::make_unique<SubscriptionManager>(*participant_, config_);

        // Wire callbacks to sink
        subscriptions_->on_vss_signal([this](const vss_Signal& msg) {
            sink_->send(msg);
        });

        subscriptions_->on_event([this](const telemetry_events_Event& msg) {
            sink_->send(msg);
        });

        subscriptions_->on_gauge([this](const telemetry_metrics_Gauge& msg) {
            sink_->send(msg);
        });

        subscriptions_->on_counter([this](const telemetry_metrics_Counter& msg) {
            sink_->send(msg);
        });

        subscriptions_->on_histogram([this](const telemetry_metrics_Histogram& msg) {
            sink_->send(msg);
        });

        subscriptions_->on_log_entry([this](const telemetry_logs_LogEntry& msg) {
            sink_->send(msg);
        });

        subscriptions_->on_scalar_measurement([this](const telemetry_diagnostics_ScalarMeasurement& msg) {
            sink_->send(msg);
        });

        subscriptions_->on_vector_measurement([this](const telemetry_diagnostics_VectorMeasurement& msg) {
            sink_->send(msg);
        });

        subscriptions_->start();
        running_ = true;

        LOG(INFO) << "TestVdr started";
        return true;

    } catch (const dds::Error& e) {
        LOG(ERROR) << "TestVdr start failed: " << e.what();
        sink_->stop();
        return false;
    }
}

void TestVdr::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (subscriptions_) {
        subscriptions_->stop();
    }

    if (sink_) {
        sink_->stop();
        auto stats = sink_->stats();
        LOG(INFO) << "TestVdr stopped. Messages: " << stats.messages_sent;
    }

    subscriptions_.reset();
    participant_.reset();
}

bool TestVdr::restart() {
    auto saved_sink = std::move(sink_);
    stop();

    // Small delay for DDS cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return start(std::move(saved_sink), config_);
}

bool TestVdr::restart(std::unique_ptr<OutputSink> sink) {
    stop();

    // Small delay for DDS cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return start(std::move(sink), config_);
}

bool TestVdr::healthy() const {
    return running_ && sink_ && sink_->healthy();
}

}  // namespace testing
}  // namespace vdr
