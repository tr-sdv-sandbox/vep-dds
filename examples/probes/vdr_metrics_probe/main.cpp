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

/// @file metrics_probe/main.cpp
/// @brief Sample Prometheus-style metrics probe
///
/// Simulates a probe that collects metrics and publishes to DDS.
/// In production, this would scrape /metrics endpoints or receive OTLP.

#include "common/dds_wrapper.hpp"
#include "common/qos_profiles.hpp"
#include "common/time_utils.hpp"
#include "telemetry.h"
#include "vss_types.h"

#include <glog/logging.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <random>
#include <thread>
#include <cstring>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    LOG(INFO) << "Received signal " << signum << ", shutting down...";
    g_running = false;
}

// Helper to create a KeyValue
vss_types_KeyValue make_label(const char* key, const char* value) {
    vss_types_KeyValue kv = {};
    kv.key = const_cast<char*>(key);
    kv.value = const_cast<char*>(value);
    return kv;
}

}  // namespace

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    google::SetStderrLogging(google::INFO);
    FLAGS_colorlogtostderr = true;

    LOG(INFO) << "Metrics Probe starting...";

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Parse scrape interval (seconds)
    double interval_sec = 5.0;
    if (argc > 1) {
        interval_sec = std::stod(argv[1]);
    }
    auto interval = std::chrono::milliseconds(static_cast<int>(interval_sec * 1000));
    LOG(INFO) << "Scrape interval: " << interval_sec << " s";

    try {
        // Create DDS participant
        dds::Participant participant(DDS_DOMAIN_DEFAULT);

        // Create topics with best-effort QoS
        auto qos = dds::qos_profiles::best_effort(1);

        dds::Topic topic_gauge(participant, &telemetry_metrics_Gauge_desc,
                               "rt/telemetry/gauges", qos.get());
        dds::Topic topic_counter(participant, &telemetry_metrics_Counter_desc,
                                 "rt/telemetry/counters", qos.get());
        dds::Topic topic_histogram(participant, &telemetry_metrics_Histogram_desc,
                                   "rt/telemetry/histograms", qos.get());

        dds::Writer writer_gauge(participant, topic_gauge, qos.get());
        dds::Writer writer_counter(participant, topic_counter, qos.get());
        dds::Writer writer_histogram(participant, topic_histogram, qos.get());

        LOG(INFO) << "Metrics Probe ready. Press Ctrl+C to stop.";

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> cpu_dist(0.0, 100.0);
        std::uniform_real_distribution<> mem_dist(0.0, 100.0);
        std::uniform_real_distribution<> latency_dist(0.001, 0.5);

        uint32_t sequence = 0;
        double request_count = 0;
        double error_count = 0;

        // Pre-allocate label arrays
        vss_types_KeyValue gauge_labels[2];
        vss_types_KeyValue counter_labels[2];
        vss_types_KeyValue histogram_labels[1];

        // Pre-allocate histogram buckets
        telemetry_metrics_HistogramBucket buckets[6];
        double bucket_bounds[] = {0.005, 0.01, 0.025, 0.05, 0.1, 0.5};

        while (g_running) {
            auto start = std::chrono::steady_clock::now();

            // --- Gauges ---

            // CPU usage gauge
            {
                telemetry_metrics_Gauge msg = {};
                msg.name = const_cast<char*>("process_cpu_percent");
                msg.header.source_id = const_cast<char*>("metrics_probe");
                msg.header.timestamp_ns = utils::now_ns();
                msg.header.seq_num = sequence++;
                msg.header.correlation_id = const_cast<char*>("");

                gauge_labels[0] = make_label("component", "vdr");
                gauge_labels[1] = make_label("instance", "0");
                msg.labels._buffer = gauge_labels;
                msg.labels._length = 2;
                msg.labels._maximum = 2;

                msg.value = cpu_dist(gen);

                writer_gauge.write(msg);
            }

            // Memory usage gauge
            {
                telemetry_metrics_Gauge msg = {};
                msg.name = const_cast<char*>("process_memory_percent");
                msg.header.source_id = const_cast<char*>("metrics_probe");
                msg.header.timestamp_ns = utils::now_ns();
                msg.header.seq_num = sequence++;
                msg.header.correlation_id = const_cast<char*>("");

                gauge_labels[0] = make_label("component", "vdr");
                gauge_labels[1] = make_label("instance", "0");
                msg.labels._buffer = gauge_labels;
                msg.labels._length = 2;
                msg.labels._maximum = 2;

                msg.value = mem_dist(gen);

                writer_gauge.write(msg);
            }

            // --- Counters ---

            // Request counter (monotonically increasing)
            request_count += std::uniform_int_distribution<>(1, 100)(gen);
            {
                telemetry_metrics_Counter msg = {};
                msg.name = const_cast<char*>("http_requests_total");
                msg.header.source_id = const_cast<char*>("metrics_probe");
                msg.header.timestamp_ns = utils::now_ns();
                msg.header.seq_num = sequence++;
                msg.header.correlation_id = const_cast<char*>("");

                counter_labels[0] = make_label("method", "GET");
                counter_labels[1] = make_label("status", "200");
                msg.labels._buffer = counter_labels;
                msg.labels._length = 2;
                msg.labels._maximum = 2;

                msg.value = request_count;

                writer_counter.write(msg);
            }

            // Error counter
            if (std::uniform_int_distribution<>(0, 10)(gen) > 8) {
                error_count += 1;
            }
            {
                telemetry_metrics_Counter msg = {};
                msg.name = const_cast<char*>("http_requests_total");
                msg.header.source_id = const_cast<char*>("metrics_probe");
                msg.header.timestamp_ns = utils::now_ns();
                msg.header.seq_num = sequence++;
                msg.header.correlation_id = const_cast<char*>("");

                counter_labels[0] = make_label("method", "GET");
                counter_labels[1] = make_label("status", "500");
                msg.labels._buffer = counter_labels;
                msg.labels._length = 2;
                msg.labels._maximum = 2;

                msg.value = error_count;

                writer_counter.write(msg);
            }

            // --- Histogram ---
            {
                telemetry_metrics_Histogram msg = {};
                msg.name = const_cast<char*>("http_request_duration_seconds");
                msg.header.source_id = const_cast<char*>("metrics_probe");
                msg.header.timestamp_ns = utils::now_ns();
                msg.header.seq_num = sequence++;
                msg.header.correlation_id = const_cast<char*>("");

                histogram_labels[0] = make_label("handler", "/api/v1/data");
                msg.labels._buffer = histogram_labels;
                msg.labels._length = 1;
                msg.labels._maximum = 1;

                // Simulate histogram data
                msg.sample_count = static_cast<uint64_t>(request_count);
                msg.sample_sum = request_count * 0.02;  // Average 20ms

                // Fill buckets
                for (int i = 0; i < 6; ++i) {
                    buckets[i].upper_bound = bucket_bounds[i];
                    // Simulate cumulative counts
                    buckets[i].cumulative_count = static_cast<uint64_t>(
                        request_count * (0.1 + 0.15 * i));
                }
                msg.buckets._buffer = buckets;
                msg.buckets._length = 6;
                msg.buckets._maximum = 6;

                writer_histogram.write(msg);
            }

            LOG_EVERY_N(INFO, 10) << "Published metrics batch, sequence=" << sequence;

            // Sleep for remainder of interval
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed < interval) {
                std::this_thread::sleep_for(interval - elapsed);
            }
        }

        LOG(INFO) << "Metrics Probe shutdown. Total metrics published: " << sequence;

    } catch (const dds::Error& e) {
        LOG(FATAL) << "DDS error: " << e.what();
        return 1;
    } catch (const std::exception& e) {
        LOG(FATAL) << "Error: " << e.what();
        return 1;
    }

    google::ShutdownGoogleLogging();
    return 0;
}
