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

/// @file vdr/main.cpp
/// @brief Vehicle Data Readout main entry point
///
/// VDR subscribes to DDS topics and forwards data for offboarding.
/// In this PoC, "offboarding" means logging what would be sent via MQTT.

#include "common/dds_wrapper.hpp"
#include "vdr/subscriber.hpp"
#include "vdr/output_sink.hpp"
#include "vdr/sinks/log_sink.hpp"

#include <glog/logging.h>
#include <yaml-cpp/yaml.h>

#include <csignal>
#include <atomic>
#include <iostream>
#include <memory>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    LOG(INFO) << "Received signal " << signum << ", shutting down...";
    g_running = false;
}

vdr::SubscriptionConfig load_config(const std::string& config_path) {
    vdr::SubscriptionConfig config;

    try {
        YAML::Node yaml = YAML::LoadFile(config_path);

        if (yaml["subscriptions"]) {
            for (const auto& sub : yaml["subscriptions"]) {
                std::string topic = sub["topic"].as<std::string>("");
                bool enabled = sub["enabled"].as<bool>(true);

                if (topic == "rt/vss/signals") {
                    config.vss_signals = enabled;
                } else if (topic == "rt/events/vehicle") {
                    config.events = enabled;
                } else if (topic == "rt/telemetry/gauges") {
                    config.gauges = enabled;
                } else if (topic == "rt/telemetry/counters") {
                    config.counters = enabled;
                } else if (topic == "rt/telemetry/histograms") {
                    config.histograms = enabled;
                } else if (topic == "rt/logs/entries") {
                    config.logs = enabled;
                } else if (topic == "rt/diagnostics/scalar") {
                    config.scalar_measurements = enabled;
                } else if (topic == "rt/diagnostics/vector") {
                    config.vector_measurements = enabled;
                }
            }
        }

        LOG(INFO) << "Loaded configuration from " << config_path;
    } catch (const YAML::Exception& e) {
        LOG(WARNING) << "Failed to load config from " << config_path
                     << ": " << e.what() << ". Using defaults.";
    }

    return config;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Initialize logging
    google::InitGoogleLogging(argv[0]);
    google::SetStderrLogging(google::INFO);
    FLAGS_colorlogtostderr = true;

    LOG(INFO) << "VDR (Vehicle Data Readout) starting...";

    // Signal handling
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Load configuration
    std::string config_path = "config/vdr_config.yaml";
    if (argc > 1) {
        config_path = argv[1];
    }

    auto config = load_config(config_path);

    // Log configuration
    LOG(INFO) << "Subscription config:";
    LOG(INFO) << "  vss_signals: " << (config.vss_signals ? "enabled" : "disabled");
    LOG(INFO) << "  events: " << (config.events ? "enabled" : "disabled");
    LOG(INFO) << "  gauges: " << (config.gauges ? "enabled" : "disabled");
    LOG(INFO) << "  counters: " << (config.counters ? "enabled" : "disabled");
    LOG(INFO) << "  histograms: " << (config.histograms ? "enabled" : "disabled");
    LOG(INFO) << "  logs: " << (config.logs ? "enabled" : "disabled");
    LOG(INFO) << "  scalar_measurements: " << (config.scalar_measurements ? "enabled" : "disabled");
    LOG(INFO) << "  vector_measurements: " << (config.vector_measurements ? "enabled" : "disabled");

    try {
        // Create DDS participant
        dds::Participant participant(DDS_DOMAIN_DEFAULT);

        // Create output sink (default: LogSink)
        auto sink = std::make_unique<vdr::sinks::LogSink>();
        if (!sink->start()) {
            LOG(FATAL) << "Failed to start output sink";
            return 1;
        }

        // Create subscription manager
        vdr::SubscriptionManager subscriptions(participant, config);

        // Register callbacks - each forwards to sink
        subscriptions.on_vss_signal([&sink](const telemetry_vss_Signal& msg) {
            sink->send(msg);
        });

        subscriptions.on_event([&sink](const telemetry_events_Event& msg) {
            sink->send(msg);
        });

        subscriptions.on_gauge([&sink](const telemetry_metrics_Gauge& msg) {
            sink->send(msg);
        });

        subscriptions.on_counter([&sink](const telemetry_metrics_Counter& msg) {
            sink->send(msg);
        });

        subscriptions.on_histogram([&sink](const telemetry_metrics_Histogram& msg) {
            sink->send(msg);
        });

        subscriptions.on_log_entry([&sink](const telemetry_logs_LogEntry& msg) {
            sink->send(msg);
        });

        subscriptions.on_scalar_measurement([&sink](const telemetry_diagnostics_ScalarMeasurement& msg) {
            sink->send(msg);
        });

        subscriptions.on_vector_measurement([&sink](const telemetry_diagnostics_VectorMeasurement& msg) {
            sink->send(msg);
        });

        // Start receiving
        subscriptions.start();

        LOG(INFO) << "VDR running. Press Ctrl+C to stop.";

        // Main loop - just wait for signal
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Stop subscriptions and sink
        subscriptions.stop();
        sink->stop();

        auto stats = sink->stats();
        LOG(INFO) << "VDR shutdown complete. Messages sent: " << stats.messages_sent
                  << ", failed: " << stats.messages_failed;

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
