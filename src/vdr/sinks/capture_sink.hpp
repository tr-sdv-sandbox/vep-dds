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

/// @file sinks/capture_sink.hpp
/// @brief OutputSink that captures messages for test verification

#include "vdr/output_sink.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace vdr {
namespace sinks {

/// Captured VSS signal with deep-copied strings
struct CapturedSignal {
    std::string path;
    std::string source_id;
    int64_t timestamp_ns;
    uint32_t seq_num;
    telemetry_vss_Quality quality;
    telemetry_vss_ValueType value_type;
    bool bool_value;
    int32_t int32_value;
    int64_t int64_value;
    float float_value;
    double double_value;
    std::string string_value;
};

/// Captured event with deep-copied strings
struct CapturedEvent {
    std::string event_id;
    std::string source_id;
    std::string category;
    std::string event_type;
    telemetry_events_Severity severity;
    std::vector<uint8_t> payload;
    int64_t timestamp_ns;
    uint32_t seq_num;
};

/// OutputSink that captures messages for test assertions.
/// Thread-safe. Supports waiting for expected message counts.
class CaptureSink : public OutputSink {
public:
    CaptureSink() = default;
    ~CaptureSink() override = default;

    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }

    void send(const telemetry_vss_Signal& msg) override;
    void send(const telemetry_events_Event& msg) override;
    void send(const telemetry_metrics_Gauge& msg) override;
    void send(const telemetry_metrics_Counter& msg) override;
    void send(const telemetry_metrics_Histogram& msg) override;
    void send(const telemetry_logs_LogEntry& msg) override;
    void send(const telemetry_diagnostics_ScalarMeasurement& msg) override;
    void send(const telemetry_diagnostics_VectorMeasurement& msg) override;

    bool healthy() const override { return running_; }
    SinkStats stats() const override;
    std::string name() const override { return "CaptureSink"; }

    /// @name Test accessors
    /// @{

    /// Get captured signals (thread-safe copy)
    std::vector<CapturedSignal> signals() const;

    /// Get captured events (thread-safe copy)
    std::vector<CapturedEvent> events() const;

    /// Total message count across all types
    size_t total_count() const;

    /// Clear all captured messages
    void clear();

    /// Wait until at least `count` total messages received
    /// @return true if count reached, false if timeout
    bool wait_for(size_t count, std::chrono::milliseconds timeout);

    /// Wait until at least `count` signals received
    bool wait_for_signals(size_t count, std::chrono::milliseconds timeout);

    /// Wait until at least `count` events received
    bool wait_for_events(size_t count, std::chrono::milliseconds timeout);

    /// @}

private:
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::vector<CapturedSignal> signals_;
    std::vector<CapturedEvent> events_;
    uint64_t gauge_count_ = 0;
    uint64_t counter_count_ = 0;
    uint64_t histogram_count_ = 0;
    uint64_t log_count_ = 0;
    uint64_t scalar_count_ = 0;
    uint64_t vector_count_ = 0;
};

}  // namespace sinks
}  // namespace vdr
