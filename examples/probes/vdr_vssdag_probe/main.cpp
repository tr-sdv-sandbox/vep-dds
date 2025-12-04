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

/// @file vssdag_probe/main.cpp
/// @brief CAN-to-VSS Probe using libvssdag
///
/// Transforms raw CAN signals into VSS format using a DAG-based pipeline
/// with Lua scripting for transforms, then publishes to DDS.
///
/// Features:
/// - DBC parsing for CAN message decoding
/// - Topological sorting for derived signal dependencies
/// - Lua-based transforms (filters, calculations, state machines)
/// - Quality tracking (VALID, INVALID, NOT_AVAILABLE)
/// - Configurable via YAML mapping files
/// - Full VSS type support (primitives, arrays, structs)

#include "common/dds_wrapper.hpp"
#include "common/qos_profiles.hpp"
#include "common/time_utils.hpp"
#include "telemetry.h"
#include "vss_signal.h"
#include "vss_types.h"

#include <vssdag/signal_processor.h>
#include <vssdag/can/can_source.h>
#include <vssdag/mapping_types.h>
#include <vssdag/lua_mapper.h>
#include <vss/types/types.hpp>
#include <vss/types/struct.hpp>

#include <glog/logging.h>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    LOG(INFO) << "Received signal " << signum << ", shutting down...";
    g_running = false;
}

// Convert vss::types::SignalQuality to DDS Quality enum
vss_types_Quality convert_quality(vss::types::SignalQuality quality) {
    switch (quality) {
        case vss::types::SignalQuality::VALID:
            return vss_types_QUALITY_VALID;
        case vss::types::SignalQuality::INVALID:
            return vss_types_QUALITY_INVALID;
        case vss::types::SignalQuality::NOT_AVAILABLE:
        default:
            return vss_types_QUALITY_NOT_AVAILABLE;
    }
}

// Forward declaration for recursive struct conversion
bool convert_struct_value(vss_types_StructValue& dds_struct,
                          const vss::types::StructValue& src_struct,
                          std::vector<std::string>& string_storage,
                          std::vector<vss_types_StructField>& field_storage);

// Storage for bool arrays (std::vector<bool> doesn't have .data())
static thread_local std::vector<std::vector<uint8_t>> bool_array_storage;

// Convert a vss::types::Value to a vss_types_StructField
// Used for struct fields (no nested struct support to avoid recursion)
bool set_struct_field(vss_types_StructField& field,
                      const std::string& name,
                      const vss::types::Value& value,
                      std::vector<std::string>& string_storage) {
    // Store field name
    string_storage.push_back(name);
    field.name = const_cast<char*>(string_storage.back().c_str());

    // Set value based on type
    if (std::holds_alternative<bool>(value)) {
        field.type = vss_types_VALUE_TYPE_BOOL;
        field.bool_value = std::get<bool>(value);
        return true;
    }
    if (std::holds_alternative<int8_t>(value)) {
        field.type = vss_types_VALUE_TYPE_INT8;
        field.int8_value = std::get<int8_t>(value);
        return true;
    }
    if (std::holds_alternative<int16_t>(value)) {
        field.type = vss_types_VALUE_TYPE_INT16;
        field.int16_value = std::get<int16_t>(value);
        return true;
    }
    if (std::holds_alternative<int32_t>(value)) {
        field.type = vss_types_VALUE_TYPE_INT32;
        field.int32_value = std::get<int32_t>(value);
        return true;
    }
    if (std::holds_alternative<int64_t>(value)) {
        field.type = vss_types_VALUE_TYPE_INT64;
        field.int64_value = std::get<int64_t>(value);
        return true;
    }
    if (std::holds_alternative<uint8_t>(value)) {
        field.type = vss_types_VALUE_TYPE_UINT8;
        field.uint8_value = std::get<uint8_t>(value);
        return true;
    }
    if (std::holds_alternative<uint16_t>(value)) {
        field.type = vss_types_VALUE_TYPE_UINT16;
        field.uint16_value = std::get<uint16_t>(value);
        return true;
    }
    if (std::holds_alternative<uint32_t>(value)) {
        field.type = vss_types_VALUE_TYPE_UINT32;
        field.uint32_value = std::get<uint32_t>(value);
        return true;
    }
    if (std::holds_alternative<uint64_t>(value)) {
        field.type = vss_types_VALUE_TYPE_UINT64;
        field.uint64_value = std::get<uint64_t>(value);
        return true;
    }
    if (std::holds_alternative<float>(value)) {
        field.type = vss_types_VALUE_TYPE_FLOAT;
        field.float_value = std::get<float>(value);
        return true;
    }
    if (std::holds_alternative<double>(value)) {
        field.type = vss_types_VALUE_TYPE_DOUBLE;
        field.double_value = std::get<double>(value);
        return true;
    }
    if (std::holds_alternative<std::string>(value)) {
        field.type = vss_types_VALUE_TYPE_STRING;
        string_storage.push_back(std::get<std::string>(value));
        field.string_value = const_cast<char*>(string_storage.back().c_str());
        return true;
    }

    // Arrays in struct fields
    if (std::holds_alternative<std::vector<bool>>(value)) {
        field.type = vss_types_VALUE_TYPE_BOOL_ARRAY;
        const auto& arr = std::get<std::vector<bool>>(value);
        // std::vector<bool> is special - copy to uint8_t storage
        bool_array_storage.emplace_back();
        auto& storage = bool_array_storage.back();
        storage.reserve(arr.size());
        for (bool b : arr) storage.push_back(b ? 1 : 0);
        field.bool_array._length = storage.size();
        field.bool_array._maximum = storage.size();
        field.bool_array._buffer = reinterpret_cast<bool*>(storage.data());
        return true;
    }
    if (std::holds_alternative<std::vector<int32_t>>(value)) {
        field.type = vss_types_VALUE_TYPE_INT32_ARRAY;
        const auto& arr = std::get<std::vector<int32_t>>(value);
        field.int32_array._length = arr.size();
        field.int32_array._maximum = arr.size();
        field.int32_array._buffer = const_cast<int32_t*>(arr.data());
        return true;
    }
    if (std::holds_alternative<std::vector<float>>(value)) {
        field.type = vss_types_VALUE_TYPE_FLOAT_ARRAY;
        const auto& arr = std::get<std::vector<float>>(value);
        field.float_array._length = arr.size();
        field.float_array._maximum = arr.size();
        field.float_array._buffer = const_cast<float*>(arr.data());
        return true;
    }
    if (std::holds_alternative<std::vector<double>>(value)) {
        field.type = vss_types_VALUE_TYPE_DOUBLE_ARRAY;
        const auto& arr = std::get<std::vector<double>>(value);
        field.double_array._length = arr.size();
        field.double_array._maximum = arr.size();
        field.double_array._buffer = const_cast<double*>(arr.data());
        return true;
    }

    // Note: Nested structs in struct fields not supported to avoid infinite recursion
    field.type = vss_types_VALUE_TYPE_EMPTY;
    return false;
}

// Convert vss::types::StructValue to DDS StructValue
bool convert_struct_value(vss_types_StructValue& dds_struct,
                          const vss::types::StructValue& src_struct,
                          std::vector<std::string>& string_storage,
                          std::vector<vss_types_StructField>& field_storage) {
    // Store type name
    string_storage.push_back(src_struct.type_name());
    dds_struct.type_name = const_cast<char*>(string_storage.back().c_str());

    // Convert fields
    const auto& src_fields = src_struct.fields();
    size_t field_start = field_storage.size();
    field_storage.resize(field_start + src_fields.size());

    size_t idx = 0;
    for (const auto& [name, value] : src_fields) {
        if (!set_struct_field(field_storage[field_start + idx], name, value, string_storage)) {
            LOG(WARNING) << "Failed to convert struct field: " << name;
        }
        ++idx;
    }

    dds_struct.fields._length = src_fields.size();
    dds_struct.fields._maximum = src_fields.size();
    dds_struct.fields._buffer = field_storage.data() + field_start;

    return true;
}

// Convert vss::types::Value to DDS vss_types_Value
// Returns true if conversion succeeded, false if type not supported
bool set_value_fields(vss_types_Value& dds_value,
                      const vss::types::Value& value,
                      std::vector<std::string>& string_storage,
                      std::vector<vss_types_StructValue>& struct_storage,
                      std::vector<vss_types_StructField>& field_storage) {
    // Initialize to empty
    memset(&dds_value, 0, sizeof(dds_value));

    // Primitives
    if (std::holds_alternative<bool>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_BOOL;
        dds_value.bool_value = std::get<bool>(value);
        return true;
    }
    if (std::holds_alternative<int8_t>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_INT8;
        dds_value.int8_value = std::get<int8_t>(value);
        return true;
    }
    if (std::holds_alternative<int16_t>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_INT16;
        dds_value.int16_value = std::get<int16_t>(value);
        return true;
    }
    if (std::holds_alternative<int32_t>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_INT32;
        dds_value.int32_value = std::get<int32_t>(value);
        return true;
    }
    if (std::holds_alternative<int64_t>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_INT64;
        dds_value.int64_value = std::get<int64_t>(value);
        return true;
    }
    if (std::holds_alternative<uint8_t>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_UINT8;
        dds_value.uint8_value = std::get<uint8_t>(value);
        return true;
    }
    if (std::holds_alternative<uint16_t>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_UINT16;
        dds_value.uint16_value = std::get<uint16_t>(value);
        return true;
    }
    if (std::holds_alternative<uint32_t>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_UINT32;
        dds_value.uint32_value = std::get<uint32_t>(value);
        return true;
    }
    if (std::holds_alternative<uint64_t>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_UINT64;
        dds_value.uint64_value = std::get<uint64_t>(value);
        return true;
    }
    if (std::holds_alternative<float>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_FLOAT;
        dds_value.float_value = std::get<float>(value);
        return true;
    }
    if (std::holds_alternative<double>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_DOUBLE;
        dds_value.double_value = std::get<double>(value);
        return true;
    }
    if (std::holds_alternative<std::string>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_STRING;
        string_storage.push_back(std::get<std::string>(value));
        dds_value.string_value = const_cast<char*>(string_storage.back().c_str());
        return true;
    }

    // Array types
    if (std::holds_alternative<std::vector<bool>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_BOOL_ARRAY;
        const auto& arr = std::get<std::vector<bool>>(value);
        // std::vector<bool> is special - copy to uint8_t storage
        bool_array_storage.emplace_back();
        auto& storage = bool_array_storage.back();
        storage.reserve(arr.size());
        for (bool b : arr) storage.push_back(b ? 1 : 0);
        dds_value.bool_array._length = storage.size();
        dds_value.bool_array._maximum = storage.size();
        dds_value.bool_array._buffer = reinterpret_cast<bool*>(storage.data());
        return true;
    }
    if (std::holds_alternative<std::vector<int8_t>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_INT8_ARRAY;
        const auto& arr = std::get<std::vector<int8_t>>(value);
        dds_value.int8_array._length = arr.size();
        dds_value.int8_array._maximum = arr.size();
        dds_value.int8_array._buffer = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(arr.data()));
        return true;
    }
    if (std::holds_alternative<std::vector<int16_t>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_INT16_ARRAY;
        const auto& arr = std::get<std::vector<int16_t>>(value);
        dds_value.int16_array._length = arr.size();
        dds_value.int16_array._maximum = arr.size();
        dds_value.int16_array._buffer = const_cast<int16_t*>(arr.data());
        return true;
    }
    if (std::holds_alternative<std::vector<int32_t>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_INT32_ARRAY;
        const auto& arr = std::get<std::vector<int32_t>>(value);
        dds_value.int32_array._length = arr.size();
        dds_value.int32_array._maximum = arr.size();
        dds_value.int32_array._buffer = const_cast<int32_t*>(arr.data());
        return true;
    }
    if (std::holds_alternative<std::vector<int64_t>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_INT64_ARRAY;
        const auto& arr = std::get<std::vector<int64_t>>(value);
        dds_value.int64_array._length = arr.size();
        dds_value.int64_array._maximum = arr.size();
        dds_value.int64_array._buffer = const_cast<int64_t*>(arr.data());
        return true;
    }
    if (std::holds_alternative<std::vector<uint8_t>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_UINT8_ARRAY;
        const auto& arr = std::get<std::vector<uint8_t>>(value);
        dds_value.uint8_array._length = arr.size();
        dds_value.uint8_array._maximum = arr.size();
        dds_value.uint8_array._buffer = const_cast<uint8_t*>(arr.data());
        return true;
    }
    if (std::holds_alternative<std::vector<uint16_t>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_UINT16_ARRAY;
        const auto& arr = std::get<std::vector<uint16_t>>(value);
        dds_value.uint16_array._length = arr.size();
        dds_value.uint16_array._maximum = arr.size();
        dds_value.uint16_array._buffer = const_cast<uint16_t*>(arr.data());
        return true;
    }
    if (std::holds_alternative<std::vector<uint32_t>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_UINT32_ARRAY;
        const auto& arr = std::get<std::vector<uint32_t>>(value);
        dds_value.uint32_array._length = arr.size();
        dds_value.uint32_array._maximum = arr.size();
        dds_value.uint32_array._buffer = const_cast<uint32_t*>(arr.data());
        return true;
    }
    if (std::holds_alternative<std::vector<uint64_t>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_UINT64_ARRAY;
        const auto& arr = std::get<std::vector<uint64_t>>(value);
        dds_value.uint64_array._length = arr.size();
        dds_value.uint64_array._maximum = arr.size();
        dds_value.uint64_array._buffer = const_cast<uint64_t*>(arr.data());
        return true;
    }
    if (std::holds_alternative<std::vector<float>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_FLOAT_ARRAY;
        const auto& arr = std::get<std::vector<float>>(value);
        dds_value.float_array._length = arr.size();
        dds_value.float_array._maximum = arr.size();
        dds_value.float_array._buffer = const_cast<float*>(arr.data());
        return true;
    }
    if (std::holds_alternative<std::vector<double>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_DOUBLE_ARRAY;
        const auto& arr = std::get<std::vector<double>>(value);
        dds_value.double_array._length = arr.size();
        dds_value.double_array._maximum = arr.size();
        dds_value.double_array._buffer = const_cast<double*>(arr.data());
        return true;
    }
    if (std::holds_alternative<std::vector<std::string>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_STRING_ARRAY;
        const auto& arr = std::get<std::vector<std::string>>(value);
        // String arrays require separate char* buffer management
        // For now, just store the count and log limitation
        dds_value.string_array._length = arr.size();
        dds_value.string_array._maximum = arr.size();
        LOG_FIRST_N(WARNING, 1) << "String arrays not fully supported in DDS yet";
        return false;
    }

    // Struct types
    if (std::holds_alternative<std::shared_ptr<vss::types::StructValue>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_STRUCT;
        const auto& struct_ptr = std::get<std::shared_ptr<vss::types::StructValue>>(value);
        if (struct_ptr) {
            struct_storage.emplace_back();
            if (!convert_struct_value(struct_storage.back(), *struct_ptr, string_storage, field_storage)) {
                return false;
            }
            dds_value.struct_value = struct_storage.back();
        }
        return true;
    }

    // Struct array
    if (std::holds_alternative<std::vector<std::shared_ptr<vss::types::StructValue>>>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_STRUCT_ARRAY;
        const auto& arr = std::get<std::vector<std::shared_ptr<vss::types::StructValue>>>(value);
        size_t start_idx = struct_storage.size();
        for (const auto& struct_ptr : arr) {
            if (struct_ptr) {
                struct_storage.emplace_back();
                convert_struct_value(struct_storage.back(), *struct_ptr, string_storage, field_storage);
            }
        }
        dds_value.struct_array._length = arr.size();
        dds_value.struct_array._maximum = arr.size();
        dds_value.struct_array._buffer = struct_storage.data() + start_idx;
        return true;
    }

    // Monostate (empty)
    if (std::holds_alternative<std::monostate>(value)) {
        dds_value.type = vss_types_VALUE_TYPE_EMPTY;
        return true;
    }

    // Unknown type
    LOG(WARNING) << "Unknown value type in variant";
    return false;
}

// Parse ValueType from string
vss::types::ValueType parse_datatype(const std::string& dtype) {
    if (dtype == "bool" || dtype == "boolean") return vss::types::ValueType::BOOL;
    if (dtype == "int8") return vss::types::ValueType::INT8;
    if (dtype == "int16") return vss::types::ValueType::INT16;
    if (dtype == "int32") return vss::types::ValueType::INT32;
    if (dtype == "int64") return vss::types::ValueType::INT64;
    if (dtype == "uint8") return vss::types::ValueType::UINT8;
    if (dtype == "uint16") return vss::types::ValueType::UINT16;
    if (dtype == "uint32") return vss::types::ValueType::UINT32;
    if (dtype == "uint64") return vss::types::ValueType::UINT64;
    if (dtype == "float") return vss::types::ValueType::FLOAT;
    if (dtype == "double") return vss::types::ValueType::DOUBLE;
    if (dtype == "string") return vss::types::ValueType::STRING;
    if (dtype == "struct") return vss::types::ValueType::STRUCT;
    return vss::types::ValueType::UNSPECIFIED;
}

// Load signal mappings from YAML file
std::unordered_map<std::string, vssdag::SignalMapping> load_mappings(
    const std::string& yaml_path) {

    std::unordered_map<std::string, vssdag::SignalMapping> mappings;

    YAML::Node config = YAML::LoadFile(yaml_path);

    // Support both 'signals' and 'mappings' keys
    YAML::Node signals_node;
    if (config["signals"]) {
        signals_node = config["signals"];
    } else if (config["mappings"]) {
        signals_node = config["mappings"];
    } else {
        LOG(WARNING) << "No 'signals' or 'mappings' section in config";
        return mappings;
    }

    for (const auto& sig : signals_node) {
        vssdag::SignalMapping mapping;

        std::string signal_name = sig["signal"].as<std::string>();

        // Data type
        if (sig["datatype"]) {
            std::string dtype = sig["datatype"].as<std::string>();
            mapping.datatype = parse_datatype(dtype);
        }

        // Source configuration
        if (sig["source"]) {
            auto source = sig["source"];
            mapping.source.type = source["type"].as<std::string>("dbc");
            mapping.source.name = source["name"].as<std::string>("");
        }

        // Dependencies
        if (sig["depends_on"]) {
            for (const auto& dep : sig["depends_on"]) {
                mapping.depends_on.push_back(dep.as<std::string>());
            }
        }

        // Transform
        if (sig["transform"]) {
            auto transform = sig["transform"];
            if (transform["code"]) {
                vssdag::CodeTransform code_transform;
                code_transform.expression = transform["code"].as<std::string>();
                mapping.transform = code_transform;
            } else if (transform["value_map"]) {
                vssdag::ValueMapping value_map;
                for (const auto& kv : transform["value_map"]) {
                    value_map.mappings[kv.first.as<std::string>()] =
                        kv.second.as<std::string>();
                }
                mapping.transform = value_map;
            }
        }

        // Output rate control
        if (sig["min_interval_ms"]) {
            mapping.min_interval_ms = sig["min_interval_ms"].as<int>();
        }
        if (sig["max_interval_ms"]) {
            mapping.max_interval_ms = sig["max_interval_ms"].as<int>();
        }

        // Change detection
        if (sig["change_threshold"]) {
            mapping.change_threshold = sig["change_threshold"].as<double>();
        }

        // Processing control for derived signals
        if (sig["eval_interval_ms"]) {
            mapping.eval_interval_ms = sig["eval_interval_ms"].as<int>();
        }

        mappings[signal_name] = std::move(mapping);
    }

    LOG(INFO) << "Loaded " << mappings.size() << " signal mappings";
    return mappings;
}

}  // namespace

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    google::SetStderrLogging(google::INFO);
    FLAGS_colorlogtostderr = true;

    LOG(INFO) << "VSS DAG Probe starting...";

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Parse command line arguments
    std::string config_path = "config/vssdag_probe_config.yaml";
    std::string can_interface = "vcan0";
    std::string dbc_path = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--interface" && i + 1 < argc) {
            can_interface = argv[++i];
        } else if (arg == "--dbc" && i + 1 < argc) {
            dbc_path = argv[++i];
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --config PATH     Signal mappings YAML file\n"
                      << "  --interface NAME  CAN interface (default: vcan0)\n"
                      << "  --dbc PATH        DBC file for CAN decoding\n"
                      << "  --help            Show this help\n";
            return 0;
        }
    }

    try {
        // Load configuration
        auto mappings = load_mappings(config_path);
        if (mappings.empty()) {
            LOG(ERROR) << "No signal mappings loaded from " << config_path;
            return 1;
        }

        // Create signal processor DAG
        vssdag::SignalProcessorDAG processor;
        if (!processor.initialize(mappings)) {
            LOG(ERROR) << "Failed to initialize signal processor";
            return 1;
        }

        LOG(INFO) << "Signal processor initialized with " << mappings.size()
                  << " mappings";

        // Create CAN signal source
        if (dbc_path.empty()) {
            LOG(ERROR) << "No DBC file specified. Use --dbc to provide a DBC file.";
            return 1;
        }

        auto can_source = std::make_unique<vssdag::CANSignalSource>(
            can_interface, dbc_path, mappings);

        if (!can_source->initialize()) {
            LOG(ERROR) << "Failed to initialize CAN source on " << can_interface;
            return 1;
        }

        LOG(INFO) << "CAN source initialized: " << can_interface
                  << " with DBC: " << dbc_path;

        // Create DDS participant and writer
        dds::Participant participant(DDS_DOMAIN_DEFAULT);

        auto qos = dds::qos_profiles::reliable_standard(100);
        dds::Topic topic(participant, &vss_Signal_desc,
                         "rt/vss/signals", qos.get());
        dds::Writer writer(participant, topic, qos.get());

        LOG(INFO) << "DDS writer created for rt/vss/signals";
        LOG(INFO) << "VSS DAG Probe ready. Press Ctrl+C to stop.";

        uint32_t seq = 0;
        uint64_t signals_published = 0;

        // Buffers for storage (must outlive DDS writes)
        std::string source_id = "vssdag_probe";
        std::string correlation_id = "";
        std::vector<std::string> path_buffers;
        std::vector<std::string> string_storage;
        std::vector<vss_types_StructValue> struct_storage;
        std::vector<vss_types_StructField> field_storage;

        while (g_running) {
            std::vector<vssdag::SignalUpdate> updates;

            // Poll CAN source for new signals
            updates = can_source->poll();

            // Process through DAG (transforms, filters, derived signals)
            if (!updates.empty()) {
                auto vss_signals = processor.process_signal_updates(updates);

                // Clear temporary storage for this batch
                string_storage.clear();
                struct_storage.clear();
                field_storage.clear();

                // Prepare buffers
                path_buffers.resize(vss_signals.size());

                // Publish each output signal to DDS
                for (size_t i = 0; i < vss_signals.size(); ++i) {
                    const auto& sig = vss_signals[i];

                    // Only publish valid signals
                    if (sig.qualified_value.quality != vss::types::SignalQuality::VALID) {
                        continue;
                    }

                    vss_Signal msg = {};

                    // Store path in buffer
                    path_buffers[i] = sig.path;
                    msg.path = const_cast<char*>(path_buffers[i].c_str());

                    // Header
                    msg.header.source_id = const_cast<char*>(source_id.c_str());
                    msg.header.timestamp_ns = utils::now_ns();
                    msg.header.seq_num = seq++;
                    msg.header.correlation_id = const_cast<char*>(correlation_id.c_str());

                    // Quality
                    msg.quality = convert_quality(sig.qualified_value.quality);

                    // Value (now uses the new Value struct)
                    if (!set_value_fields(msg.value, sig.qualified_value.value,
                                          string_storage, struct_storage, field_storage)) {
                        LOG(WARNING) << "Unsupported value type for signal: " << sig.path;
                        continue;
                    }

                    writer.write(msg);
                    ++signals_published;
                }
            }

            LOG_EVERY_N(INFO, 1000) << "Signals published: " << signals_published;

            // Small sleep to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Cleanup
        can_source->stop();

        LOG(INFO) << "VSS DAG Probe shutdown. Total signals published: "
                  << signals_published;

    } catch (const YAML::Exception& e) {
        LOG(FATAL) << "YAML error: " << e.what();
        return 1;
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
