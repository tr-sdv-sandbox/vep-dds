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

/// @file sinks/mqtt_sink.hpp
/// @brief OutputSink implementation using Mosquitto MQTT client
///
/// Reference implementation for MQTT publishing. Requires libmosquitto.
/// This demonstrates how a customer would implement a real output sink.

#include "vdr/output_sink.hpp"

#include <nlohmann/json.hpp>
#include <mosquitto.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <queue>
#include <condition_variable>

namespace vdr {
namespace sinks {

/// Configuration for MQTT sink
struct MqttConfig {
    std::string host = "localhost";
    int port = 1883;
    std::string client_id = "vdr";
    std::string username;
    std::string password;
    int keepalive_sec = 60;
    int qos = 1;  // 0=at most once, 1=at least once, 2=exactly once
    bool retain = false;
    std::string topic_prefix = "vdr/v1";
};

/// OutputSink that publishes to MQTT broker via Mosquitto.
///
/// Features:
/// - Async publishing with background thread
/// - Automatic reconnection on disconnect
/// - Message queuing during disconnection
/// - JSON payload encoding
///
/// Thread-safe.
class MqttSink : public OutputSink {
public:
    explicit MqttSink(const MqttConfig& config = MqttConfig{});
    ~MqttSink() override;

    MqttSink(const MqttSink&) = delete;
    MqttSink& operator=(const MqttSink&) = delete;

    bool start() override;
    void stop() override;
    void flush() override;

    void send(const telemetry_vss_Signal& msg) override;
    void send(const telemetry_events_Event& msg) override;
    void send(const telemetry_metrics_Gauge& msg) override;
    void send(const telemetry_metrics_Counter& msg) override;
    void send(const telemetry_metrics_Histogram& msg) override;
    void send(const telemetry_logs_LogEntry& msg) override;
    void send(const telemetry_diagnostics_ScalarMeasurement& msg) override;
    void send(const telemetry_diagnostics_VectorMeasurement& msg) override;

    bool healthy() const override;
    SinkStats stats() const override;
    std::string name() const override { return "MqttSink"; }

    /// Check if connected to broker
    bool connected() const { return connected_; }

private:
    // Internal message for publish queue
    struct PendingMessage {
        std::string topic;
        std::string payload;
    };

    void publish_loop();
    void publish(const std::string& topic, const nlohmann::json& payload);
    nlohmann::json encode_header(const telemetry_Header& header);

    // Mosquitto callbacks
    static void on_connect(struct mosquitto* mosq, void* obj, int rc);
    static void on_disconnect(struct mosquitto* mosq, void* obj, int rc);
    static void on_publish(struct mosquitto* mosq, void* obj, int mid);

    MqttConfig config_;
    struct mosquitto* mosq_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    // Publish queue
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<PendingMessage> queue_;
    static constexpr size_t MAX_QUEUE_SIZE = 10000;

    // Background thread for publishing
    std::thread publish_thread_;

    // Stats
    mutable std::mutex stats_mutex_;
    SinkStats stats_;
    std::atomic<uint64_t> queued_{0};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace sinks
}  // namespace vdr
