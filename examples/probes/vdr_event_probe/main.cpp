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

/// @file event_probe/main.cpp
/// @brief Sample vehicle event probe
///
/// Simulates a probe that detects vehicle events and publishes to DDS.
/// In production, this would monitor actual vehicle systems.

#include "common/dds_wrapper.hpp"
#include "common/qos_profiles.hpp"
#include "common/time_utils.hpp"
#include "telemetry.h"

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

struct EventType {
    const char* category;
    const char* event_type;
    telemetry_events_Severity severity;
    int weight;  // Relative frequency
};

// Simulated event types
const EventType EVENT_TYPES[] = {
    {"POWERTRAIN", "regenerative_brake", telemetry_events_SEVERITY_INFO, 50},
    {"POWERTRAIN", "high_power_demand", telemetry_events_SEVERITY_INFO, 30},
    {"ADAS", "forward_collision_warning", telemetry_events_SEVERITY_WARNING, 10},
    {"ADAS", "lane_departure_warning", telemetry_events_SEVERITY_WARNING, 15},
    {"ADAS", "emergency_brake_assist", telemetry_events_SEVERITY_CRITICAL, 2},
    {"CAB", "door_open_while_moving", telemetry_events_SEVERITY_WARNING, 5},
    {"CAB", "seatbelt_unfastened", telemetry_events_SEVERITY_WARNING, 20},
    {"DIAGNOSTICS", "dtc_set", telemetry_events_SEVERITY_ERROR, 8},
    {"DIAGNOSTICS", "dtc_cleared", telemetry_events_SEVERITY_INFO, 5},
    {"THERMAL", "battery_temperature_high", telemetry_events_SEVERITY_WARNING, 10},
    {"THERMAL", "motor_temperature_high", telemetry_events_SEVERITY_WARNING, 8},
    {"CONNECTIVITY", "connection_lost", telemetry_events_SEVERITY_WARNING, 15},
    {"CONNECTIVITY", "connection_restored", telemetry_events_SEVERITY_INFO, 15},
};
constexpr size_t NUM_EVENT_TYPES = sizeof(EVENT_TYPES) / sizeof(EVENT_TYPES[0]);

// Calculate total weight for random selection
int total_weight() {
    int sum = 0;
    for (size_t i = 0; i < NUM_EVENT_TYPES; ++i) {
        sum += EVENT_TYPES[i].weight;
    }
    return sum;
}

// Select random event type based on weights
const EventType& select_event_type(std::mt19937& gen) {
    std::uniform_int_distribution<> dist(0, total_weight() - 1);
    int r = dist(gen);

    int cumulative = 0;
    for (size_t i = 0; i < NUM_EVENT_TYPES; ++i) {
        cumulative += EVENT_TYPES[i].weight;
        if (r < cumulative) {
            return EVENT_TYPES[i];
        }
    }
    return EVENT_TYPES[0];  // Fallback
}

}  // namespace

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    google::SetStderrLogging(google::INFO);
    FLAGS_colorlogtostderr = true;

    LOG(INFO) << "Event Probe starting...";

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Parse average events per minute
    double events_per_min = 2.0;
    if (argc > 1) {
        events_per_min = std::stod(argv[1]);
    }
    LOG(INFO) << "Average event rate: " << events_per_min << " events/minute";

    // Mean interval between events (in ms)
    double mean_interval_ms = 60000.0 / events_per_min;

    try {
        // Create DDS participant
        dds::Participant participant(DDS_DOMAIN_DEFAULT);

        // Create topic with reliable critical QoS (events must not be lost)
        auto qos = dds::qos_profiles::reliable_critical();
        dds::Topic topic(participant, &telemetry_events_Event_desc,
                         "rt/events/vehicle", qos.get());

        dds::Writer writer(participant, topic, qos.get());

        LOG(INFO) << "Event Probe ready. Press Ctrl+C to stop.";

        std::random_device rd;
        std::mt19937 gen(rd());

        // Exponential distribution for inter-event times
        std::exponential_distribution<> interval_dist(1.0 / mean_interval_ms);

        uint32_t sequence = 0;
        uint32_t event_count = 0;

        while (g_running) {
            // Wait for random interval
            double wait_ms = interval_dist(gen);
            // Clamp to reasonable range
            wait_ms = std::max(100.0, std::min(wait_ms, 30000.0));

            // Sleep in small chunks to allow quick shutdown
            auto remaining = std::chrono::milliseconds(static_cast<int>(wait_ms));
            auto chunk = std::chrono::milliseconds(100);
            while (g_running && remaining > std::chrono::milliseconds(0)) {
                auto sleep_time = std::min(remaining, chunk);
                std::this_thread::sleep_for(sleep_time);
                remaining -= sleep_time;
            }

            if (!g_running) break;

            // Select and publish an event
            const auto& evt = select_event_type(gen);

            telemetry_events_Event msg = {};

            // Generate unique event ID
            std::string event_id = utils::generate_uuid();
            msg.event_id = const_cast<char*>(event_id.c_str());

            // Set header
            msg.header.source_id = const_cast<char*>("event_probe");
            msg.header.timestamp_ns = utils::now_ns();
            msg.header.seq_num = sequence++;
            msg.header.correlation_id = const_cast<char*>("");

            // Set event fields
            msg.category = const_cast<char*>(evt.category);
            msg.event_type = const_cast<char*>(evt.event_type);
            msg.severity = evt.severity;

            // Add attributes (replaces old payload field)
            // Note: attributes is a sequence of KeyValue, we skip for simple simulation
            msg.attributes._buffer = nullptr;
            msg.attributes._length = 0;
            msg.attributes._maximum = 0;

            // Context signals (replaces old payload)
            msg.context._buffer = nullptr;
            msg.context._length = 0;
            msg.context._maximum = 0;

            writer.write(msg);
            event_count++;

            const char* severity_str = "INFO";
            switch (evt.severity) {
                case telemetry_events_SEVERITY_WARNING: severity_str = "WARNING"; break;
                case telemetry_events_SEVERITY_ERROR: severity_str = "ERROR"; break;
                case telemetry_events_SEVERITY_CRITICAL: severity_str = "CRITICAL"; break;
                default: break;
            }

            LOG(INFO) << "Published event: " << evt.category << "/" << evt.event_type
                      << " [" << severity_str << "] id=" << event_id;
        }

        LOG(INFO) << "Event Probe shutdown. Total events published: " << event_count;

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
