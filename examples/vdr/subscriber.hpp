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

/// @file subscriber.hpp
/// @brief VDR subscription management
///
/// Manages DDS subscriptions based on configuration.

#include "common/dds_wrapper.hpp"
#include "telemetry.h"
#include "vss_signal.h"

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

namespace vdr {

/*
 * Callback types for received data.
 */
using VssSignalCallback = std::function<void(const vss_Signal&)>;
using EventCallback = std::function<void(const telemetry_events_Event&)>;
using GaugeCallback = std::function<void(const telemetry_metrics_Gauge&)>;
using CounterCallback = std::function<void(const telemetry_metrics_Counter&)>;
using HistogramCallback = std::function<void(const telemetry_metrics_Histogram&)>;
using LogEntryCallback = std::function<void(const telemetry_logs_LogEntry&)>;
using ScalarMeasurementCallback = std::function<void(const telemetry_diagnostics_ScalarMeasurement&)>;
using VectorMeasurementCallback = std::function<void(const telemetry_diagnostics_VectorMeasurement&)>;

/*
 * Subscription configuration.
 */
struct SubscriptionConfig {
    bool vss_signals = true;
    bool events = true;
    bool gauges = true;
    bool counters = true;
    bool histograms = true;
    bool logs = true;
    bool scalar_measurements = true;
    bool vector_measurements = true;
};

/*
 * SubscriptionManager - manages all DDS subscriptions for VDR.
 *
 * Creates readers based on configuration and dispatches callbacks
 * when data arrives.
 */
class SubscriptionManager {
public:
    explicit SubscriptionManager(dds::Participant& participant,
                                  const SubscriptionConfig& config);
    ~SubscriptionManager();

    // Start receiving data (spawns background thread)
    void start();

    // Stop receiving data
    void stop();

    // Register callbacks
    void on_vss_signal(VssSignalCallback callback);
    void on_event(EventCallback callback);
    void on_gauge(GaugeCallback callback);
    void on_counter(CounterCallback callback);
    void on_histogram(HistogramCallback callback);
    void on_log_entry(LogEntryCallback callback);
    void on_scalar_measurement(ScalarMeasurementCallback callback);
    void on_vector_measurement(VectorMeasurementCallback callback);

private:
    void poll_loop();

    template<typename T, typename Callback>
    void process_reader(dds::Reader& reader, const Callback& callback);

    dds::Participant& participant_;
    SubscriptionConfig config_;

    // Topics
    std::unique_ptr<dds::Topic> topic_vss_signal_;
    std::unique_ptr<dds::Topic> topic_event_;
    std::unique_ptr<dds::Topic> topic_gauge_;
    std::unique_ptr<dds::Topic> topic_counter_;
    std::unique_ptr<dds::Topic> topic_histogram_;
    std::unique_ptr<dds::Topic> topic_log_entry_;
    std::unique_ptr<dds::Topic> topic_scalar_measurement_;
    std::unique_ptr<dds::Topic> topic_vector_measurement_;

    // Readers
    std::unique_ptr<dds::Reader> reader_vss_signal_;
    std::unique_ptr<dds::Reader> reader_event_;
    std::unique_ptr<dds::Reader> reader_gauge_;
    std::unique_ptr<dds::Reader> reader_counter_;
    std::unique_ptr<dds::Reader> reader_histogram_;
    std::unique_ptr<dds::Reader> reader_log_entry_;
    std::unique_ptr<dds::Reader> reader_scalar_measurement_;
    std::unique_ptr<dds::Reader> reader_vector_measurement_;

    // Callbacks
    VssSignalCallback cb_vss_signal_;
    EventCallback cb_event_;
    GaugeCallback cb_gauge_;
    CounterCallback cb_counter_;
    HistogramCallback cb_histogram_;
    LogEntryCallback cb_log_entry_;
    ScalarMeasurementCallback cb_scalar_measurement_;
    VectorMeasurementCallback cb_vector_measurement_;

    // Polling thread
    std::atomic<bool> running_{false};
    std::thread poll_thread_;
};

}  // namespace vdr
