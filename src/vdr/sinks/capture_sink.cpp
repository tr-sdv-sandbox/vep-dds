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

#include "vdr/sinks/capture_sink.hpp"

namespace vdr {
namespace sinks {

void CaptureSink::send(const telemetry_vss_Signal& msg) {
    if (!running_) return;

    CapturedSignal cap;
    cap.path = msg.path ? msg.path : "";
    cap.source_id = msg.header.source_id ? msg.header.source_id : "";
    cap.timestamp_ns = msg.header.timestamp_ns;
    cap.seq_num = msg.header.seq_num;
    cap.quality = msg.quality;
    cap.value_type = msg.value_type;
    cap.bool_value = msg.bool_value;
    cap.int32_value = msg.int32_value;
    cap.int64_value = msg.int64_value;
    cap.float_value = msg.float_value;
    cap.double_value = msg.double_value;
    cap.string_value = msg.string_value ? msg.string_value : "";

    {
        std::lock_guard<std::mutex> lock(mutex_);
        signals_.push_back(std::move(cap));
    }
    cv_.notify_all();
}

void CaptureSink::send(const telemetry_events_Event& msg) {
    if (!running_) return;

    CapturedEvent cap;
    cap.event_id = msg.event_id ? msg.event_id : "";
    cap.source_id = msg.header.source_id ? msg.header.source_id : "";
    cap.category = msg.category ? msg.category : "";
    cap.event_type = msg.event_type ? msg.event_type : "";
    cap.severity = msg.severity;
    cap.timestamp_ns = msg.header.timestamp_ns;
    cap.seq_num = msg.header.seq_num;

    if (msg.payload._length > 0 && msg.payload._buffer) {
        cap.payload.assign(msg.payload._buffer,
                          msg.payload._buffer + msg.payload._length);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(cap));
    }
    cv_.notify_all();
}

void CaptureSink::send(const telemetry_metrics_Gauge&) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++gauge_count_;
    }
    cv_.notify_all();
}

void CaptureSink::send(const telemetry_metrics_Counter&) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counter_count_;
    }
    cv_.notify_all();
}

void CaptureSink::send(const telemetry_metrics_Histogram&) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++histogram_count_;
    }
    cv_.notify_all();
}

void CaptureSink::send(const telemetry_logs_LogEntry&) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++log_count_;
    }
    cv_.notify_all();
}

void CaptureSink::send(const telemetry_diagnostics_ScalarMeasurement&) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++scalar_count_;
    }
    cv_.notify_all();
}

void CaptureSink::send(const telemetry_diagnostics_VectorMeasurement&) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++vector_count_;
    }
    cv_.notify_all();
}

SinkStats CaptureSink::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SinkStats s;
    s.messages_sent = signals_.size() + events_.size() +
                      gauge_count_ + counter_count_ + histogram_count_ +
                      log_count_ + scalar_count_ + vector_count_;
    return s;
}

std::vector<CapturedSignal> CaptureSink::signals() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return signals_;
}

std::vector<CapturedEvent> CaptureSink::events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}

size_t CaptureSink::total_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return signals_.size() + events_.size() +
           gauge_count_ + counter_count_ + histogram_count_ +
           log_count_ + scalar_count_ + vector_count_;
}

void CaptureSink::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    signals_.clear();
    events_.clear();
    gauge_count_ = 0;
    counter_count_ = 0;
    histogram_count_ = 0;
    log_count_ = 0;
    scalar_count_ = 0;
    vector_count_ = 0;
}

bool CaptureSink::wait_for(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] {
        return (signals_.size() + events_.size() +
                gauge_count_ + counter_count_ + histogram_count_ +
                log_count_ + scalar_count_ + vector_count_) >= count;
    });
}

bool CaptureSink::wait_for_signals(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] {
        return signals_.size() >= count;
    });
}

bool CaptureSink::wait_for_events(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] {
        return events_.size() >= count;
    });
}

}  // namespace sinks
}  // namespace vdr
