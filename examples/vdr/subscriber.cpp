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

#include "vdr/subscriber.hpp"
#include "common/qos_profiles.hpp"

#include <glog/logging.h>

namespace vdr {

SubscriptionManager::SubscriptionManager(dds::Participant& participant,
                                          const SubscriptionConfig& config)
    : participant_(participant), config_(config) {

    // Create topics and readers based on configuration

    if (config_.vss_signals) {
        auto qos = dds::qos_profiles::reliable_standard(100);
        topic_vss_signal_ = std::make_unique<dds::Topic>(
            participant_, &vss_Signal_desc,
            "rt/vss/signals", qos.get());
        reader_vss_signal_ = std::make_unique<dds::Reader>(
            participant_, *topic_vss_signal_, qos.get());
    }

    if (config_.events) {
        auto qos = dds::qos_profiles::reliable_critical();
        topic_event_ = std::make_unique<dds::Topic>(
            participant_, &telemetry_events_Event_desc,
            "rt/events/vehicle", qos.get());
        reader_event_ = std::make_unique<dds::Reader>(
            participant_, *topic_event_, qos.get());
    }

    if (config_.gauges) {
        auto qos = dds::qos_profiles::best_effort(1);
        topic_gauge_ = std::make_unique<dds::Topic>(
            participant_, &telemetry_metrics_Gauge_desc,
            "rt/telemetry/gauges", qos.get());
        reader_gauge_ = std::make_unique<dds::Reader>(
            participant_, *topic_gauge_, qos.get());
    }

    if (config_.counters) {
        auto qos = dds::qos_profiles::best_effort(1);
        topic_counter_ = std::make_unique<dds::Topic>(
            participant_, &telemetry_metrics_Counter_desc,
            "rt/telemetry/counters", qos.get());
        reader_counter_ = std::make_unique<dds::Reader>(
            participant_, *topic_counter_, qos.get());
    }

    if (config_.histograms) {
        auto qos = dds::qos_profiles::best_effort(1);
        topic_histogram_ = std::make_unique<dds::Topic>(
            participant_, &telemetry_metrics_Histogram_desc,
            "rt/telemetry/histograms", qos.get());
        reader_histogram_ = std::make_unique<dds::Reader>(
            participant_, *topic_histogram_, qos.get());
    }

    if (config_.logs) {
        auto qos = dds::qos_profiles::best_effort(100);
        topic_log_entry_ = std::make_unique<dds::Topic>(
            participant_, &telemetry_logs_LogEntry_desc,
            "rt/logs/entries", qos.get());
        reader_log_entry_ = std::make_unique<dds::Reader>(
            participant_, *topic_log_entry_, qos.get());
    }

    if (config_.scalar_measurements) {
        auto qos = dds::qos_profiles::reliable_standard(10);
        topic_scalar_measurement_ = std::make_unique<dds::Topic>(
            participant_, &telemetry_diagnostics_ScalarMeasurement_desc,
            "rt/diagnostics/scalar", qos.get());
        reader_scalar_measurement_ = std::make_unique<dds::Reader>(
            participant_, *topic_scalar_measurement_, qos.get());
    }

    if (config_.vector_measurements) {
        auto qos = dds::qos_profiles::reliable_standard(10);
        topic_vector_measurement_ = std::make_unique<dds::Topic>(
            participant_, &telemetry_diagnostics_VectorMeasurement_desc,
            "rt/diagnostics/vector", qos.get());
        reader_vector_measurement_ = std::make_unique<dds::Reader>(
            participant_, *topic_vector_measurement_, qos.get());
    }

    LOG(INFO) << "SubscriptionManager initialized";
}

SubscriptionManager::~SubscriptionManager() {
    stop();
}

void SubscriptionManager::start() {
    if (running_.exchange(true)) {
        LOG(WARNING) << "SubscriptionManager already running";
        return;
    }

    poll_thread_ = std::thread(&SubscriptionManager::poll_loop, this);
    LOG(INFO) << "SubscriptionManager started";
}

void SubscriptionManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
    LOG(INFO) << "SubscriptionManager stopped";
}

void SubscriptionManager::on_vss_signal(VssSignalCallback callback) {
    cb_vss_signal_ = std::move(callback);
}

void SubscriptionManager::on_event(EventCallback callback) {
    cb_event_ = std::move(callback);
}

void SubscriptionManager::on_gauge(GaugeCallback callback) {
    cb_gauge_ = std::move(callback);
}

void SubscriptionManager::on_counter(CounterCallback callback) {
    cb_counter_ = std::move(callback);
}

void SubscriptionManager::on_histogram(HistogramCallback callback) {
    cb_histogram_ = std::move(callback);
}

void SubscriptionManager::on_log_entry(LogEntryCallback callback) {
    cb_log_entry_ = std::move(callback);
}

void SubscriptionManager::on_scalar_measurement(ScalarMeasurementCallback callback) {
    cb_scalar_measurement_ = std::move(callback);
}

void SubscriptionManager::on_vector_measurement(VectorMeasurementCallback callback) {
    cb_vector_measurement_ = std::move(callback);
}

void SubscriptionManager::poll_loop() {
    LOG(INFO) << "Poll loop started";

    while (running_) {
        // Poll each reader
        if (reader_vss_signal_ && cb_vss_signal_) {
            process_reader<vss_Signal>(*reader_vss_signal_, cb_vss_signal_);
        }

        if (reader_event_ && cb_event_) {
            process_reader<telemetry_events_Event>(*reader_event_, cb_event_);
        }

        if (reader_gauge_ && cb_gauge_) {
            process_reader<telemetry_metrics_Gauge>(*reader_gauge_, cb_gauge_);
        }

        if (reader_counter_ && cb_counter_) {
            process_reader<telemetry_metrics_Counter>(*reader_counter_, cb_counter_);
        }

        if (reader_histogram_ && cb_histogram_) {
            process_reader<telemetry_metrics_Histogram>(*reader_histogram_, cb_histogram_);
        }

        if (reader_log_entry_ && cb_log_entry_) {
            process_reader<telemetry_logs_LogEntry>(*reader_log_entry_, cb_log_entry_);
        }

        if (reader_scalar_measurement_ && cb_scalar_measurement_) {
            process_reader<telemetry_diagnostics_ScalarMeasurement>(
                *reader_scalar_measurement_, cb_scalar_measurement_);
        }

        if (reader_vector_measurement_ && cb_vector_measurement_) {
            process_reader<telemetry_diagnostics_VectorMeasurement>(
                *reader_vector_measurement_, cb_vector_measurement_);
        }

        // Small sleep to avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    LOG(INFO) << "Poll loop exited";
}

template<typename T, typename Callback>
void SubscriptionManager::process_reader(dds::Reader& reader, const Callback& callback) {
    try {
        reader.take_each<T>(callback, 100);
    } catch (const dds::Error& e) {
        LOG(ERROR) << "Error reading from DDS: " << e.what();
    }
}

}  // namespace vdr
