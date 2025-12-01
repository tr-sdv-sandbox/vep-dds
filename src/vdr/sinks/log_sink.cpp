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

#include "vdr/sinks/log_sink.hpp"
#include "common/time_utils.hpp"

#include <glog/logging.h>

namespace vdr {
namespace sinks {

bool LogSink::start() {
    running_ = true;
    LOG(INFO) << "LogSink started";
    return true;
}

void LogSink::stop() {
    running_ = false;
    LOG(INFO) << "LogSink stopped. Stats: sent=" << stats_.messages_sent
              << " failed=" << stats_.messages_failed;
}

SinkStats LogSink::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

nlohmann::json LogSink::encode_header(const telemetry_Header& header) {
    return {
        {"source_id", header.source_id ? header.source_id : ""},
        {"timestamp_ns", header.timestamp_ns},
        {"seq_num", header.seq_num},
        {"correlation_id", header.correlation_id ? header.correlation_id : ""}
    };
}

void LogSink::log_output(const std::string& topic, const nlohmann::json& payload) {
    std::string json_str = payload.dump();
    LOG(INFO) << "[MQTT] topic=" << topic << " payload=" << json_str;

    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.messages_sent++;
    stats_.bytes_sent += json_str.size();
    stats_.last_send_timestamp_ns = utils::now_ns();
}

void LogSink::send(const telemetry_vss_Signal& msg) {
    if (!running_) return;

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

    log_output("v1/vss/signals", payload);
}

void LogSink::send(const telemetry_events_Event& msg) {
    if (!running_) return;

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

    log_output("v1/events", payload);
}

void LogSink::send(const telemetry_metrics_Gauge& msg) {
    if (!running_) return;

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

    log_output("v1/telemetry/gauges", payload);
}

void LogSink::send(const telemetry_metrics_Counter& msg) {
    if (!running_) return;

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

    log_output("v1/telemetry/counters", payload);
}

void LogSink::send(const telemetry_metrics_Histogram& msg) {
    if (!running_) return;

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

    log_output("v1/telemetry/histograms", payload);
}

void LogSink::send(const telemetry_logs_LogEntry& msg) {
    if (!running_) return;

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

    log_output("v1/logs", payload);
}

void LogSink::send(const telemetry_diagnostics_ScalarMeasurement& msg) {
    if (!running_) return;

    nlohmann::json payload = {
        {"header", encode_header(msg.header)},
        {"variable_id", msg.variable_id ? msg.variable_id : ""},
        {"unit", msg.unit ? msg.unit : ""},
        {"mtype", static_cast<int>(msg.mtype)},
        {"value", msg.value}
    };

    log_output("v1/diagnostics/scalar", payload);
}

void LogSink::send(const telemetry_diagnostics_VectorMeasurement& msg) {
    if (!running_) return;

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

    log_output("v1/diagnostics/vector", payload);
}

}  // namespace sinks
}  // namespace vdr
