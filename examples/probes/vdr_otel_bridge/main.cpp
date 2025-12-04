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

/// @file otel_bridge/main.cpp
/// @brief OTLP-to-DDS Bridge Probe
///
/// Receives OpenTelemetry metrics and logs via gRPC (OTLP protocol)
/// and publishes them to DDS topics for VDR consumption.
///
/// Architecture:
///   OTel SDK --> OTLP/gRPC --> [This Bridge] --> DDS --> VDR

#include "common/dds_wrapper.hpp"
#include "common/qos_profiles.hpp"
#include "common/time_utils.hpp"
#include "telemetry.h"
#include "vss_types.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "opentelemetry/proto/collector/metrics/v1/metrics_service.grpc.pb.h"
#include "opentelemetry/proto/collector/logs/v1/logs_service.grpc.pb.h"

#include <glog/logging.h>

#include <atomic>
#include <csignal>
#include <memory>
#include <string>
#include <thread>
#include <cstring>

namespace otlp_metrics = opentelemetry::proto::collector::metrics::v1;
namespace otlp_logs = opentelemetry::proto::collector::logs::v1;
namespace otel_metrics = opentelemetry::proto::metrics::v1;
namespace otel_logs = opentelemetry::proto::logs::v1;
namespace otel_common = opentelemetry::proto::common::v1;

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    LOG(INFO) << "Received signal " << signum << ", shutting down...";
    g_running = false;
}

// Helper to extract string from AnyValue
std::string get_string_value(const otel_common::AnyValue& value) {
    if (value.has_string_value()) {
        return value.string_value();
    } else if (value.has_int_value()) {
        return std::to_string(value.int_value());
    } else if (value.has_double_value()) {
        return std::to_string(value.double_value());
    } else if (value.has_bool_value()) {
        return value.bool_value() ? "true" : "false";
    }
    return "";
}

// Convert OTel severity to our log level
telemetry_logs_Level convert_severity(otel_logs::SeverityNumber severity) {
    if (severity <= otel_logs::SEVERITY_NUMBER_DEBUG4) {
        return telemetry_logs_LEVEL_DEBUG;
    } else if (severity <= otel_logs::SEVERITY_NUMBER_INFO4) {
        return telemetry_logs_LEVEL_INFO;
    } else if (severity <= otel_logs::SEVERITY_NUMBER_WARN4) {
        return telemetry_logs_LEVEL_WARN;
    } else if (severity <= otel_logs::SEVERITY_NUMBER_ERROR4) {
        return telemetry_logs_LEVEL_ERROR;
    }
    return telemetry_logs_LEVEL_ERROR;  // Map FATAL to ERROR (our max level)
}

}  // namespace

/*
 * MetricsServiceImpl - Implements OTLP MetricsService
 */
class MetricsServiceImpl final : public otlp_metrics::MetricsService::Service {
public:
    MetricsServiceImpl(dds::Writer& gauge_writer,
                       dds::Writer& counter_writer,
                       dds::Writer& histogram_writer)
        : gauge_writer_(gauge_writer)
        , counter_writer_(counter_writer)
        , histogram_writer_(histogram_writer) {}

    grpc::Status Export(
        grpc::ServerContext* /*context*/,
        const otlp_metrics::ExportMetricsServiceRequest* request,
        otlp_metrics::ExportMetricsServiceResponse* response) override {

        int64_t rejected = 0;

        for (const auto& rm : request->resource_metrics()) {
            // Extract resource attributes (e.g., service.name)
            std::string source_id = "otel_bridge";
            for (const auto& attr : rm.resource().attributes()) {
                if (attr.key() == "service.name") {
                    source_id = get_string_value(attr.value());
                    break;
                }
            }

            for (const auto& sm : rm.scope_metrics()) {
                for (const auto& metric : sm.metrics()) {
                    try {
                        process_metric(metric, source_id);
                    } catch (const std::exception& e) {
                        LOG(ERROR) << "Failed to process metric " << metric.name()
                                   << ": " << e.what();
                        ++rejected;
                    }
                }
            }
        }

        if (rejected > 0) {
            response->mutable_partial_success()->set_rejected_data_points(rejected);
        }

        ++metrics_received_;
        LOG_EVERY_N(INFO, 100) << "Metrics batches received: " << metrics_received_;

        return grpc::Status::OK;
    }

private:
    void process_metric(const otel_metrics::Metric& metric,
                        const std::string& source_id) {
        if (metric.has_gauge()) {
            for (const auto& dp : metric.gauge().data_points()) {
                publish_gauge(metric.name(), dp, source_id);
            }
        } else if (metric.has_sum()) {
            bool is_counter = metric.sum().is_monotonic();
            for (const auto& dp : metric.sum().data_points()) {
                if (is_counter) {
                    publish_counter(metric.name(), dp, source_id);
                } else {
                    publish_gauge(metric.name(), dp, source_id);
                }
            }
        } else if (metric.has_histogram()) {
            for (const auto& dp : metric.histogram().data_points()) {
                publish_histogram(metric.name(), dp, source_id);
            }
        }
        // Summary and ExponentialHistogram not yet supported
    }

    void publish_gauge(const std::string& name,
                       const otel_metrics::NumberDataPoint& dp,
                       const std::string& source_id) {
        telemetry_metrics_Gauge msg = {};

        // Store strings to keep them alive
        name_buf_ = name;
        source_buf_ = source_id;
        correlation_buf_ = "";

        msg.name = const_cast<char*>(name_buf_.c_str());
        msg.header.source_id = const_cast<char*>(source_buf_.c_str());
        msg.header.timestamp_ns = static_cast<int64_t>(dp.time_unix_nano());
        msg.header.seq_num = seq_++;
        msg.header.correlation_id = const_cast<char*>(correlation_buf_.c_str());

        // Convert attributes to labels
        labels_.clear();
        label_keys_.clear();
        label_values_.clear();

        for (const auto& attr : dp.attributes()) {
            label_keys_.push_back(attr.key());
            label_values_.push_back(get_string_value(attr.value()));
        }

        for (size_t i = 0; i < label_keys_.size() && i < 16; ++i) {
            vss_types_KeyValue kv = {};
            kv.key = const_cast<char*>(label_keys_[i].c_str());
            kv.value = const_cast<char*>(label_values_[i].c_str());
            labels_.push_back(kv);
        }

        if (!labels_.empty()) {
            msg.labels._buffer = labels_.data();
            msg.labels._length = static_cast<uint32_t>(labels_.size());
            msg.labels._maximum = static_cast<uint32_t>(labels_.size());
        }

        msg.value = dp.has_as_double() ? dp.as_double() : static_cast<double>(dp.as_int());

        gauge_writer_.write(msg);
    }

    void publish_counter(const std::string& name,
                         const otel_metrics::NumberDataPoint& dp,
                         const std::string& source_id) {
        telemetry_metrics_Counter msg = {};

        name_buf_ = name;
        source_buf_ = source_id;
        correlation_buf_ = "";

        msg.name = const_cast<char*>(name_buf_.c_str());
        msg.header.source_id = const_cast<char*>(source_buf_.c_str());
        msg.header.timestamp_ns = static_cast<int64_t>(dp.time_unix_nano());
        msg.header.seq_num = seq_++;
        msg.header.correlation_id = const_cast<char*>(correlation_buf_.c_str());

        // Convert attributes
        labels_.clear();
        label_keys_.clear();
        label_values_.clear();

        for (const auto& attr : dp.attributes()) {
            label_keys_.push_back(attr.key());
            label_values_.push_back(get_string_value(attr.value()));
        }

        for (size_t i = 0; i < label_keys_.size() && i < 16; ++i) {
            vss_types_KeyValue kv = {};
            kv.key = const_cast<char*>(label_keys_[i].c_str());
            kv.value = const_cast<char*>(label_values_[i].c_str());
            labels_.push_back(kv);
        }

        if (!labels_.empty()) {
            msg.labels._buffer = labels_.data();
            msg.labels._length = static_cast<uint32_t>(labels_.size());
            msg.labels._maximum = static_cast<uint32_t>(labels_.size());
        }

        msg.value = dp.has_as_double() ? dp.as_double() : static_cast<double>(dp.as_int());

        counter_writer_.write(msg);
    }

    void publish_histogram(const std::string& name,
                           const otel_metrics::HistogramDataPoint& dp,
                           const std::string& source_id) {
        telemetry_metrics_Histogram msg = {};

        name_buf_ = name;
        source_buf_ = source_id;
        correlation_buf_ = "";

        msg.name = const_cast<char*>(name_buf_.c_str());
        msg.header.source_id = const_cast<char*>(source_buf_.c_str());
        msg.header.timestamp_ns = static_cast<int64_t>(dp.time_unix_nano());
        msg.header.seq_num = seq_++;
        msg.header.correlation_id = const_cast<char*>(correlation_buf_.c_str());

        // Convert attributes
        labels_.clear();
        label_keys_.clear();
        label_values_.clear();

        for (const auto& attr : dp.attributes()) {
            label_keys_.push_back(attr.key());
            label_values_.push_back(get_string_value(attr.value()));
        }

        for (size_t i = 0; i < label_keys_.size() && i < 16; ++i) {
            vss_types_KeyValue kv = {};
            kv.key = const_cast<char*>(label_keys_[i].c_str());
            kv.value = const_cast<char*>(label_values_[i].c_str());
            labels_.push_back(kv);
        }

        if (!labels_.empty()) {
            msg.labels._buffer = labels_.data();
            msg.labels._length = static_cast<uint32_t>(labels_.size());
            msg.labels._maximum = static_cast<uint32_t>(labels_.size());
        }

        msg.sample_count = dp.count();
        msg.sample_sum = dp.has_sum() ? dp.sum() : 0.0;

        // Convert buckets
        buckets_.clear();
        int num_bounds = dp.explicit_bounds_size();
        int num_counts = dp.bucket_counts_size();

        for (int i = 0; i < num_counts && i < 32; ++i) {
            telemetry_metrics_HistogramBucket bucket = {};
            bucket.cumulative_count = dp.bucket_counts(i);
            bucket.upper_bound = (i < num_bounds) ? dp.explicit_bounds(i)
                                                   : std::numeric_limits<double>::infinity();
            buckets_.push_back(bucket);
        }

        if (!buckets_.empty()) {
            msg.buckets._buffer = buckets_.data();
            msg.buckets._length = static_cast<uint32_t>(buckets_.size());
            msg.buckets._maximum = static_cast<uint32_t>(buckets_.size());
        }

        histogram_writer_.write(msg);
    }

    dds::Writer& gauge_writer_;
    dds::Writer& counter_writer_;
    dds::Writer& histogram_writer_;

    uint32_t seq_ = 0;
    uint64_t metrics_received_ = 0;

    // Buffers for string storage (to keep pointers valid during write)
    std::string name_buf_;
    std::string source_buf_;
    std::string correlation_buf_;
    std::vector<std::string> label_keys_;
    std::vector<std::string> label_values_;
    std::vector<vss_types_KeyValue> labels_;
    std::vector<telemetry_metrics_HistogramBucket> buckets_;
};

/*
 * LogsServiceImpl - Implements OTLP LogsService
 */
class LogsServiceImpl final : public otlp_logs::LogsService::Service {
public:
    explicit LogsServiceImpl(dds::Writer& log_writer)
        : log_writer_(log_writer) {}

    grpc::Status Export(
        grpc::ServerContext* /*context*/,
        const otlp_logs::ExportLogsServiceRequest* request,
        otlp_logs::ExportLogsServiceResponse* response) override {

        int64_t rejected = 0;

        for (const auto& rl : request->resource_logs()) {
            // Extract resource attributes
            std::string source_id = "otel_bridge";
            for (const auto& attr : rl.resource().attributes()) {
                if (attr.key() == "service.name") {
                    source_id = get_string_value(attr.value());
                    break;
                }
            }

            for (const auto& sl : rl.scope_logs()) {
                std::string component = sl.scope().name();
                if (component.empty()) {
                    component = "unknown";
                }

                for (const auto& log : sl.log_records()) {
                    try {
                        publish_log(log, source_id, component);
                    } catch (const std::exception& e) {
                        LOG(ERROR) << "Failed to process log: " << e.what();
                        ++rejected;
                    }
                }
            }
        }

        if (rejected > 0) {
            response->mutable_partial_success()->set_rejected_log_records(rejected);
        }

        ++logs_received_;
        LOG_EVERY_N(INFO, 100) << "Log batches received: " << logs_received_;

        return grpc::Status::OK;
    }

private:
    void publish_log(const otel_logs::LogRecord& log,
                     const std::string& source_id,
                     const std::string& component) {
        telemetry_logs_LogEntry msg = {};

        source_buf_ = source_id;
        correlation_buf_ = "";
        component_buf_ = component;

        // Extract message from body
        if (log.has_body()) {
            message_buf_ = get_string_value(log.body());
        } else {
            message_buf_ = "";
        }

        msg.header.source_id = const_cast<char*>(source_buf_.c_str());
        msg.header.timestamp_ns = static_cast<int64_t>(log.time_unix_nano());
        msg.header.seq_num = seq_++;
        msg.header.correlation_id = const_cast<char*>(correlation_buf_.c_str());

        msg.level = convert_severity(log.severity_number());
        msg.component = const_cast<char*>(component_buf_.c_str());
        msg.message = const_cast<char*>(message_buf_.c_str());

        // Convert attributes to fields
        fields_.clear();
        field_keys_.clear();
        field_values_.clear();

        for (const auto& attr : log.attributes()) {
            field_keys_.push_back(attr.key());
            field_values_.push_back(get_string_value(attr.value()));
        }

        for (size_t i = 0; i < field_keys_.size() && i < 16; ++i) {
            vss_types_KeyValue kv = {};
            kv.key = const_cast<char*>(field_keys_[i].c_str());
            kv.value = const_cast<char*>(field_values_[i].c_str());
            fields_.push_back(kv);
        }

        if (!fields_.empty()) {
            msg.fields._buffer = fields_.data();
            msg.fields._length = static_cast<uint32_t>(fields_.size());
            msg.fields._maximum = static_cast<uint32_t>(fields_.size());
        }

        log_writer_.write(msg);
    }

    dds::Writer& log_writer_;

    uint32_t seq_ = 0;
    uint64_t logs_received_ = 0;

    std::string source_buf_;
    std::string correlation_buf_;
    std::string component_buf_;
    std::string message_buf_;
    std::vector<std::string> field_keys_;
    std::vector<std::string> field_values_;
    std::vector<vss_types_KeyValue> fields_;
};

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    google::SetStderrLogging(google::INFO);
    FLAGS_colorlogtostderr = true;

    LOG(INFO) << "OTLP-to-DDS Bridge starting...";

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Parse gRPC port
    int port = 4317;  // Default OTLP gRPC port
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    std::string server_address = "0.0.0.0:" + std::to_string(port);

    try {
        // Create DDS participant
        dds::Participant participant(DDS_DOMAIN_DEFAULT);

        // Create topics and writers
        auto qos_metrics = dds::qos_profiles::best_effort(1);
        auto qos_logs = dds::qos_profiles::best_effort(100);

        dds::Topic topic_gauge(participant, &telemetry_metrics_Gauge_desc,
                               "rt/telemetry/gauges", qos_metrics.get());
        dds::Topic topic_counter(participant, &telemetry_metrics_Counter_desc,
                                 "rt/telemetry/counters", qos_metrics.get());
        dds::Topic topic_histogram(participant, &telemetry_metrics_Histogram_desc,
                                   "rt/telemetry/histograms", qos_metrics.get());
        dds::Topic topic_logs(participant, &telemetry_logs_LogEntry_desc,
                              "rt/logs/entries", qos_logs.get());

        dds::Writer writer_gauge(participant, topic_gauge, qos_metrics.get());
        dds::Writer writer_counter(participant, topic_counter, qos_metrics.get());
        dds::Writer writer_histogram(participant, topic_histogram, qos_metrics.get());
        dds::Writer writer_logs(participant, topic_logs, qos_logs.get());

        LOG(INFO) << "DDS writers created";

        // Create gRPC services
        MetricsServiceImpl metrics_service(writer_gauge, writer_counter, writer_histogram);
        LogsServiceImpl logs_service(writer_logs);

        // Build and start gRPC server
        grpc::EnableDefaultHealthCheckService(true);

        grpc::ServerBuilder builder;
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(&metrics_service);
        builder.RegisterService(&logs_service);

        std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
        LOG(INFO) << "OTLP gRPC server listening on " << server_address;
        LOG(INFO) << "Configure OTel Collector exporter: endpoint=" << server_address;

        // Wait for shutdown signal
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        LOG(INFO) << "Shutting down gRPC server...";
        server->Shutdown();

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
