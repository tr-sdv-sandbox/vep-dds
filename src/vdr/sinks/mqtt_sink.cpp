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

#include "vdr/sinks/mqtt_sink.hpp"
#include "common/time_utils.hpp"

#include <glog/logging.h>

namespace vdr {
namespace sinks {

MqttSink::MqttSink(const MqttConfig& config)
    : config_(config) {
    mosquitto_lib_init();
}

MqttSink::~MqttSink() {
    stop();
    mosquitto_lib_cleanup();
}

bool MqttSink::start() {
    if (running_) {
        return true;
    }

    // Create mosquitto client
    mosq_ = mosquitto_new(config_.client_id.c_str(), true, this);
    if (!mosq_) {
        LOG(ERROR) << "MqttSink: Failed to create mosquitto client";
        return false;
    }

    // Set callbacks
    mosquitto_connect_callback_set(mosq_, on_connect);
    mosquitto_disconnect_callback_set(mosq_, on_disconnect);
    mosquitto_publish_callback_set(mosq_, on_publish);

    // Set credentials if provided
    if (!config_.username.empty()) {
        mosquitto_username_pw_set(mosq_, config_.username.c_str(),
                                  config_.password.c_str());
    }

    // Connect to broker
    int rc = mosquitto_connect_async(mosq_, config_.host.c_str(),
                                      config_.port, config_.keepalive_sec);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG(ERROR) << "MqttSink: Failed to connect to " << config_.host
                   << ":" << config_.port << " - " << mosquitto_strerror(rc);
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        return false;
    }

    // Start mosquitto network loop
    rc = mosquitto_loop_start(mosq_);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG(ERROR) << "MqttSink: Failed to start loop - " << mosquitto_strerror(rc);
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        return false;
    }

    running_ = true;

    // Start publish thread
    publish_thread_ = std::thread(&MqttSink::publish_loop, this);

    LOG(INFO) << "MqttSink started, connecting to " << config_.host
              << ":" << config_.port;
    return true;
}

void MqttSink::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    // Wake up publish thread
    queue_cv_.notify_all();

    if (publish_thread_.joinable()) {
        publish_thread_.join();
    }

    if (mosq_) {
        mosquitto_loop_stop(mosq_, false);
        mosquitto_disconnect(mosq_);
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
    }

    connected_ = false;

    LOG(INFO) << "MqttSink stopped. Stats: sent=" << stats_.messages_sent
              << " failed=" << stats_.messages_failed
              << " dropped=" << dropped_.load();
}

void MqttSink::flush() {
    // Wait for queue to drain
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
        return queue_.empty();
    });
}

void MqttSink::publish_loop() {
    while (running_) {
        PendingMessage msg;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !queue_.empty() || !running_;
            });

            if (!running_ && queue_.empty()) {
                break;
            }

            if (queue_.empty()) {
                continue;
            }

            msg = std::move(queue_.front());
            queue_.pop();
        }

        // Publish if connected
        if (connected_ && mosq_) {
            int rc = mosquitto_publish(mosq_, nullptr,
                                       msg.topic.c_str(),
                                       static_cast<int>(msg.payload.size()),
                                       msg.payload.c_str(),
                                       config_.qos,
                                       config_.retain);
            if (rc != MOSQ_ERR_SUCCESS) {
                LOG(WARNING) << "MqttSink: Publish failed - " << mosquitto_strerror(rc);
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.messages_failed++;
            } else {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.messages_sent++;
                stats_.bytes_sent += msg.payload.size();
                stats_.last_send_timestamp_ns = utils::now_ns();
            }
        } else {
            // Not connected, message is lost (or could re-queue)
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.messages_failed++;
        }
    }
}

void MqttSink::publish(const std::string& topic, const nlohmann::json& payload) {
    if (!running_) return;

    std::string full_topic = config_.topic_prefix + "/" + topic;
    std::string json_str = payload.dump();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= MAX_QUEUE_SIZE) {
            // Drop oldest message
            queue_.pop();
            ++dropped_;
        }
        queue_.push({full_topic, json_str});
        ++queued_;
    }
    queue_cv_.notify_one();
}

nlohmann::json MqttSink::encode_header(const telemetry_Header& header) {
    return {
        {"source_id", header.source_id ? header.source_id : ""},
        {"timestamp_ns", header.timestamp_ns},
        {"seq_num", header.seq_num},
        {"correlation_id", header.correlation_id ? header.correlation_id : ""}
    };
}

void MqttSink::send(const telemetry_vss_Signal& msg) {
    nlohmann::json payload = {
        {"header", encode_header(msg.header)},
        {"path", msg.path ? msg.path : ""},
        {"quality", static_cast<int>(msg.quality)},
        {"value_type", static_cast<int>(msg.value_type)}
    };

    switch (msg.value_type) {
        case telemetry_vss_VALUE_TYPE_BOOL:
            payload["value"] = msg.bool_value;
            break;
        case telemetry_vss_VALUE_TYPE_INT32:
            payload["value"] = msg.int32_value;
            break;
        case telemetry_vss_VALUE_TYPE_INT64:
            payload["value"] = msg.int64_value;
            break;
        case telemetry_vss_VALUE_TYPE_FLOAT:
            payload["value"] = msg.float_value;
            break;
        case telemetry_vss_VALUE_TYPE_DOUBLE:
            payload["value"] = msg.double_value;
            break;
        case telemetry_vss_VALUE_TYPE_STRING:
            payload["value"] = msg.string_value ? msg.string_value : "";
            break;
    }

    publish("vss/signals", payload);
}

void MqttSink::send(const telemetry_events_Event& msg) {
    nlohmann::json payload = {
        {"header", encode_header(msg.header)},
        {"event_id", msg.event_id ? msg.event_id : ""},
        {"category", msg.category ? msg.category : ""},
        {"event_type", msg.event_type ? msg.event_type : ""},
        {"severity", static_cast<int>(msg.severity)}
    };

    if (msg.payload._length > 0) {
        payload["payload_size"] = msg.payload._length;
    }

    publish("events", payload);
}

void MqttSink::send(const telemetry_metrics_Gauge& msg) {
    nlohmann::json labels = nlohmann::json::object();
    for (uint32_t i = 0; i < msg.labels._length; ++i) {
        const auto& kv = msg.labels._buffer[i];
        if (kv.key && kv.value) {
            labels[kv.key] = kv.value;
        }
    }

    nlohmann::json payload = {
        {"header", encode_header(msg.header)},
        {"name", msg.name ? msg.name : ""},
        {"labels", labels},
        {"value", msg.value}
    };

    publish("telemetry/gauges", payload);
}

void MqttSink::send(const telemetry_metrics_Counter& msg) {
    nlohmann::json labels = nlohmann::json::object();
    for (uint32_t i = 0; i < msg.labels._length; ++i) {
        const auto& kv = msg.labels._buffer[i];
        if (kv.key && kv.value) {
            labels[kv.key] = kv.value;
        }
    }

    nlohmann::json payload = {
        {"header", encode_header(msg.header)},
        {"name", msg.name ? msg.name : ""},
        {"labels", labels},
        {"value", msg.value}
    };

    publish("telemetry/counters", payload);
}

void MqttSink::send(const telemetry_metrics_Histogram& msg) {
    nlohmann::json labels = nlohmann::json::object();
    for (uint32_t i = 0; i < msg.labels._length; ++i) {
        const auto& kv = msg.labels._buffer[i];
        if (kv.key && kv.value) {
            labels[kv.key] = kv.value;
        }
    }

    nlohmann::json buckets = nlohmann::json::array();
    for (uint32_t i = 0; i < msg.buckets._length; ++i) {
        const auto& bucket = msg.buckets._buffer[i];
        buckets.push_back({
            {"upper_bound", bucket.upper_bound},
            {"cumulative_count", bucket.cumulative_count}
        });
    }

    nlohmann::json payload = {
        {"header", encode_header(msg.header)},
        {"name", msg.name ? msg.name : ""},
        {"labels", labels},
        {"sample_count", msg.sample_count},
        {"sample_sum", msg.sample_sum},
        {"buckets", buckets}
    };

    publish("telemetry/histograms", payload);
}

void MqttSink::send(const telemetry_logs_LogEntry& msg) {
    nlohmann::json fields = nlohmann::json::object();
    for (uint32_t i = 0; i < msg.fields._length; ++i) {
        const auto& kv = msg.fields._buffer[i];
        if (kv.key && kv.value) {
            fields[kv.key] = kv.value;
        }
    }

    nlohmann::json payload = {
        {"header", encode_header(msg.header)},
        {"level", static_cast<int>(msg.level)},
        {"component", msg.component ? msg.component : ""},
        {"message", msg.message ? msg.message : ""},
        {"fields", fields}
    };

    publish("logs", payload);
}

void MqttSink::send(const telemetry_diagnostics_ScalarMeasurement& msg) {
    nlohmann::json payload = {
        {"header", encode_header(msg.header)},
        {"variable_id", msg.variable_id ? msg.variable_id : ""},
        {"unit", msg.unit ? msg.unit : ""},
        {"mtype", static_cast<int>(msg.mtype)},
        {"value", msg.value}
    };

    publish("diagnostics/scalar", payload);
}

void MqttSink::send(const telemetry_diagnostics_VectorMeasurement& msg) {
    nlohmann::json values = nlohmann::json::array();
    for (uint32_t i = 0; i < msg.values._length; ++i) {
        values.push_back(msg.values._buffer[i]);
    }

    nlohmann::json payload = {
        {"header", encode_header(msg.header)},
        {"variable_id", msg.variable_id ? msg.variable_id : ""},
        {"unit", msg.unit ? msg.unit : ""},
        {"mtype", static_cast<int>(msg.mtype)},
        {"values", values}
    };

    publish("diagnostics/vector", payload);
}

bool MqttSink::healthy() const {
    return running_ && connected_;
}

SinkStats MqttSink::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

// Static callbacks
void MqttSink::on_connect(struct mosquitto*, void* obj, int rc) {
    auto* self = static_cast<MqttSink*>(obj);
    if (rc == 0) {
        self->connected_ = true;
        LOG(INFO) << "MqttSink: Connected to broker";
    } else {
        self->connected_ = false;
        LOG(WARNING) << "MqttSink: Connection failed - " << mosquitto_connack_string(rc);
    }
}

void MqttSink::on_disconnect(struct mosquitto*, void* obj, int rc) {
    auto* self = static_cast<MqttSink*>(obj);
    self->connected_ = false;
    if (rc != 0) {
        LOG(WARNING) << "MqttSink: Unexpected disconnect - " << mosquitto_strerror(rc);
    } else {
        LOG(INFO) << "MqttSink: Disconnected from broker";
    }
}

void MqttSink::on_publish(struct mosquitto*, void*, int) {
    // Could track in-flight messages here if needed
}

}  // namespace sinks
}  // namespace vdr
