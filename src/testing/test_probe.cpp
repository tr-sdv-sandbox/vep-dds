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

#include "testing/test_probe.hpp"

#include <glog/logging.h>

namespace vdr {
namespace testing {

TestProbe::TestProbe(const std::string& source_id, dds_domainid_t domain_id)
    : source_id_(source_id)
    , domain_id_(domain_id) {}

TestProbe::~TestProbe() {
    stop();
}

bool TestProbe::start() {
    if (running_) {
        return true;
    }

    try {
        participant_ = std::make_unique<dds::Participant>(domain_id_);

        auto signal_qos = dds::qos_profiles::reliable_standard(100);
        topic_signal_ = std::make_unique<dds::Topic>(
            *participant_, &telemetry_vss_Signal_desc,
            "rt/vss/signals", signal_qos.get());
        writer_signal_ = std::make_unique<dds::Writer>(
            *participant_, *topic_signal_, signal_qos.get());

        auto event_qos = dds::qos_profiles::reliable_critical();
        topic_event_ = std::make_unique<dds::Topic>(
            *participant_, &telemetry_events_Event_desc,
            "rt/events/vehicle", event_qos.get());
        writer_event_ = std::make_unique<dds::Writer>(
            *participant_, *topic_event_, event_qos.get());

        running_ = true;
        LOG(INFO) << "TestProbe '" << source_id_ << "' started";
        return true;

    } catch (const dds::Error& e) {
        LOG(ERROR) << "TestProbe start failed: " << e.what();
        return false;
    }
}

void TestProbe::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    writer_signal_.reset();
    writer_event_.reset();
    topic_signal_.reset();
    topic_event_.reset();
    participant_.reset();

    LOG(INFO) << "TestProbe '" << source_id_ << "' stopped. "
              << "Signals sent: " << signals_sent_
              << ", Events sent: " << events_sent_;
}

bool TestProbe::restart() {
    stop();
    // Small delay for DDS cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return start();
}

void TestProbe::send_signal(const std::string& path, double value,
                            telemetry_vss_Quality quality) {
    if (!running_) return;

    telemetry_vss_Signal msg = {};
    msg.path = const_cast<char*>(path.c_str());
    msg.header.source_id = const_cast<char*>(source_id_.c_str());
    msg.header.timestamp_ns = utils::now_ns();
    msg.header.seq_num = seq_++;
    msg.header.correlation_id = const_cast<char*>("");
    msg.quality = quality;
    msg.value_type = telemetry_vss_VALUE_TYPE_DOUBLE;
    msg.double_value = value;

    writer_signal_->write(msg);
    ++signals_sent_;
}

void TestProbe::send_signal(const std::string& path, const std::string& value,
                            telemetry_vss_Quality quality) {
    if (!running_) return;

    telemetry_vss_Signal msg = {};
    msg.path = const_cast<char*>(path.c_str());
    msg.header.source_id = const_cast<char*>(source_id_.c_str());
    msg.header.timestamp_ns = utils::now_ns();
    msg.header.seq_num = seq_++;
    msg.header.correlation_id = const_cast<char*>("");
    msg.quality = quality;
    msg.value_type = telemetry_vss_VALUE_TYPE_STRING;
    msg.string_value = const_cast<char*>(value.c_str());

    writer_signal_->write(msg);
    ++signals_sent_;
}

void TestProbe::send_signal(const std::string& path, int32_t value,
                            telemetry_vss_Quality quality) {
    if (!running_) return;

    telemetry_vss_Signal msg = {};
    msg.path = const_cast<char*>(path.c_str());
    msg.header.source_id = const_cast<char*>(source_id_.c_str());
    msg.header.timestamp_ns = utils::now_ns();
    msg.header.seq_num = seq_++;
    msg.header.correlation_id = const_cast<char*>("");
    msg.quality = quality;
    msg.value_type = telemetry_vss_VALUE_TYPE_INT32;
    msg.int32_value = value;

    writer_signal_->write(msg);
    ++signals_sent_;
}

void TestProbe::send_signal(const std::string& path, bool value,
                            telemetry_vss_Quality quality) {
    if (!running_) return;

    telemetry_vss_Signal msg = {};
    msg.path = const_cast<char*>(path.c_str());
    msg.header.source_id = const_cast<char*>(source_id_.c_str());
    msg.header.timestamp_ns = utils::now_ns();
    msg.header.seq_num = seq_++;
    msg.header.correlation_id = const_cast<char*>("");
    msg.quality = quality;
    msg.value_type = telemetry_vss_VALUE_TYPE_BOOL;
    msg.bool_value = value;

    writer_signal_->write(msg);
    ++signals_sent_;
}

void TestProbe::send_event(const std::string& category,
                           const std::string& event_type,
                           telemetry_events_Severity severity,
                           const std::vector<uint8_t>& payload) {
    if (!running_) return;

    std::string event_id = utils::generate_uuid();

    telemetry_events_Event msg = {};
    msg.event_id = const_cast<char*>(event_id.c_str());
    msg.header.source_id = const_cast<char*>(source_id_.c_str());
    msg.header.timestamp_ns = utils::now_ns();
    msg.header.seq_num = seq_++;
    msg.header.correlation_id = const_cast<char*>("");
    msg.category = const_cast<char*>(category.c_str());
    msg.event_type = const_cast<char*>(event_type.c_str());
    msg.severity = severity;

    if (!payload.empty()) {
        msg.payload._buffer = const_cast<uint8_t*>(payload.data());
        msg.payload._length = static_cast<uint32_t>(payload.size());
        msg.payload._maximum = static_cast<uint32_t>(payload.size());
    }

    writer_event_->write(msg);
    ++events_sent_;
}

void TestProbe::send_signal_burst(size_t count, std::chrono::milliseconds interval) {
    for (size_t i = 0; i < count && running_; ++i) {
        send_signal("Vehicle.Test.Burst", static_cast<double>(i));
        if (interval.count() > 0) {
            std::this_thread::sleep_for(interval);
        }
    }
}

void TestProbe::send_event_burst(size_t count, std::chrono::milliseconds interval) {
    for (size_t i = 0; i < count && running_; ++i) {
        send_event("TEST", "burst_event", telemetry_events_SEVERITY_INFO);
        if (interval.count() > 0) {
            std::this_thread::sleep_for(interval);
        }
    }
}

}  // namespace testing
}  // namespace vdr
