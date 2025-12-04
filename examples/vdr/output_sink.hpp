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

/// @file output_sink.hpp
/// @brief Abstract interface for VDR output destinations
///
/// OutputSink defines the contract for message output. Implementations
/// can target different backends: logging, MQTT, cloud APIs, etc.

#include "telemetry.h"
#include "vss_signal.h"

#include <cstdint>
#include <memory>
#include <string>

namespace vdr {

/// Statistics for an output sink
struct SinkStats {
    uint64_t messages_sent = 0;
    uint64_t messages_failed = 0;
    uint64_t bytes_sent = 0;
    uint64_t last_send_timestamp_ns = 0;
};

/// Abstract interface for output destinations.
///
/// Implementations must be thread-safe if used from multiple threads.
/// The VDR calls send() methods from its polling thread.
class OutputSink {
public:
    virtual ~OutputSink() = default;

    /// Initialize the sink. Called before any send() calls.
    /// @return true if initialization succeeded
    virtual bool start() = 0;

    /// Shutdown the sink. Flush any buffered data.
    virtual void stop() = 0;

    /// @name Message sending
    /// @{
    virtual void send(const vss_Signal& msg) = 0;
    virtual void send(const telemetry_events_Event& msg) = 0;
    virtual void send(const telemetry_metrics_Gauge& msg) = 0;
    virtual void send(const telemetry_metrics_Counter& msg) = 0;
    virtual void send(const telemetry_metrics_Histogram& msg) = 0;
    virtual void send(const telemetry_logs_LogEntry& msg) = 0;
    virtual void send(const telemetry_diagnostics_ScalarMeasurement& msg) = 0;
    virtual void send(const telemetry_diagnostics_VectorMeasurement& msg) = 0;
    /// @}

    /// Flush any buffered messages. Default is no-op for unbuffered sinks.
    virtual void flush() {}

    /// Check if sink is operational.
    /// @return true if sink can accept messages
    virtual bool healthy() const = 0;

    /// Get sink statistics.
    virtual SinkStats stats() const = 0;

    /// Get sink name for logging/debugging.
    virtual std::string name() const = 0;
};

/// Factory function type for creating sinks
using SinkFactory = std::unique_ptr<OutputSink>(*)();

}  // namespace vdr
