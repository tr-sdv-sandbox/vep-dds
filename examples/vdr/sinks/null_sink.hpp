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

/// @file sinks/null_sink.hpp
/// @brief OutputSink that discards all messages
///
/// Useful for load testing and benchmarking without I/O overhead.

#include "vdr/output_sink.hpp"

#include <atomic>

namespace vdr {
namespace sinks {

/// OutputSink that discards all messages.
/// Thread-safe. Counts messages for stats.
class NullSink : public OutputSink {
public:
    NullSink() = default;
    ~NullSink() override = default;

    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }

    void send(const vss_Signal&) override { ++count_; }
    void send(const telemetry_events_Event&) override { ++count_; }
    void send(const telemetry_metrics_Gauge&) override { ++count_; }
    void send(const telemetry_metrics_Counter&) override { ++count_; }
    void send(const telemetry_metrics_Histogram&) override { ++count_; }
    void send(const telemetry_logs_LogEntry&) override { ++count_; }
    void send(const telemetry_diagnostics_ScalarMeasurement&) override { ++count_; }
    void send(const telemetry_diagnostics_VectorMeasurement&) override { ++count_; }

    bool healthy() const override { return running_; }

    SinkStats stats() const override {
        SinkStats s;
        s.messages_sent = count_.load();
        return s;
    }

    std::string name() const override { return "NullSink"; }

private:
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> count_{0};
};

}  // namespace sinks
}  // namespace vdr
