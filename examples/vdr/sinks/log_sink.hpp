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

/// @file sinks/log_sink.hpp
/// @brief OutputSink implementation that logs to glog

#include "vdr/output_sink.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <string>

namespace vdr {
namespace sinks {

/// OutputSink that logs messages as JSON via glog.
/// Thread-safe.
class LogSink : public OutputSink {
public:
    LogSink() = default;
    ~LogSink() override = default;

    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;

    bool start() override;
    void stop() override;

    void send(const vss_Signal& msg) override;
    void send(const telemetry_events_Event& msg) override;
    void send(const telemetry_metrics_Gauge& msg) override;
    void send(const telemetry_metrics_Counter& msg) override;
    void send(const telemetry_metrics_Histogram& msg) override;
    void send(const telemetry_logs_LogEntry& msg) override;
    void send(const telemetry_diagnostics_ScalarMeasurement& msg) override;
    void send(const telemetry_diagnostics_VectorMeasurement& msg) override;

    bool healthy() const override { return running_; }
    SinkStats stats() const override;
    std::string name() const override { return "LogSink"; }

private:
    nlohmann::json encode_header(const vss_types_Header& header);
    void log_output(const std::string& topic, const nlohmann::json& payload);

    std::atomic<bool> running_{false};
    mutable std::mutex stats_mutex_;
    SinkStats stats_;
};

}  // namespace sinks
}  // namespace vdr
