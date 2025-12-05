# Vehicle Data Readout (VDR) Ecosystem Specification

## Overview

This document specifies an onboard data collection system using DDS (Cyclone DDS) as the internal transport. The system consists of:

1. **Probes** — Components that collect data from various sources and publish to DDS
2. **VDR (Vehicle Data Readout)** — Central component that subscribes to DDS topics, applies offboard policies, and transmits data within link budget constraints
3. **Onboard Consumers** — Other systems (HMI, ADAS, logging) that consume the same DDS data feed

## Design Principles

1. **DDS as the onboard data bus** — Full-fidelity data available to all onboard consumers
2. **Separation of concerns** — DDS feed configuration (what's available) is separate from offboard policy (what's sent to cloud)
3. **Link budget awareness** — VDR respects bandwidth constraints and degrades gracefully
4. **Priority-based data handling** — Critical data never lost, low-priority data dropped first
5. **Offline resilience** — Buffering strategies for connectivity loss
6. **C++17 with RAII wrappers** — All C resources wrapped for safety

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           DDS Domain (Onboard)                              │
│                                                                             │
│   Full fidelity data feed - used by onboard consumers AND VDR              │
│                                                                             │
│   Topics:                                                                   │
│   ├── rt/vss/signals           (VSS signal samples, up to 100Hz)           │
│   ├── rt/events/vehicle        (Vehicle events)                            │
│   ├── rt/diagnostics/scalar    (Scalar diagnostic measurements)           │
│   ├── rt/diagnostics/vector    (Vector diagnostic measurements)           │
│   ├── rt/telemetry/gauges      (Prometheus-style gauges)                   │
│   ├── rt/telemetry/counters    (Prometheus-style counters)                 │
│   ├── rt/telemetry/histograms  (Prometheus-style histograms)               │
│   └── rt/logs/entries          (Log entries)                               │
│                                                                             │
│   ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐         │
│   │ Android │  │  ADAS   │  │ Logging │  │   HMI   │  │   VDR   │         │
│   │   HMI   │  │ Features│  │ Service │  │ Cluster │  │         │         │
│   └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘         │
│        └────────────┴────────────┴────────────┴────────────┘               │
│                                 DDS Subscribers                             │
└─────────────────────────────────────────────────────────────────────────────┘
        ▲               ▲               ▲               ▲
        │               │               │               │
┌───────────────┐ ┌───────────────┐ ┌───────────────┐ ┌───────────────┐ ┌───────────────┐
│  VSS Probe    │ │ Metrics Probe │ │ Event Probe   │ │ VSSDAG Probe  │ │  AVTP Probe   │
│  (Kuksa)      │ │ (OTel/Prom)   │ │ (System)      │ │ (CAN/DBC)     │ │ (IEEE 1722)   │
└───────────────┘ └───────────────┘ └───────────────┘ └───────────────┘ └───────────────┘
```

## Two Configuration Domains

### 1. DDS Feed Configuration (Probe-side)

Controls what data is available onboard at what fidelity. Onboard consumers (ADAS, HMI, logging) get full-rate data.

```yaml
# Example: vss_probe_config.yaml
source:
  type: kuksa                           # or "mock", "someip", "can"
  endpoint: "grpc://localhost:55555"

signals:
  - path: "Vehicle.Speed"
    sample_rate_hz: 100                 # Full fidelity for onboard use

  - path: "Vehicle.Powertrain.TractionBattery.StateOfCharge.Current"
    sample_rate_hz: 10

  - path: "Vehicle.CurrentLocation.*"   # Wildcard support
    sample_rate_hz: 10

  - path: "Vehicle.Cabin.HVAC.**"       # Recursive wildcard
    sample_rate_hz: 1
```

### 2. Offboard Policy Configuration (VDR-side)

Controls what/when to send to cloud given link budget, priorities, and connectivity state.

```yaml
# Example: vdr_offboard_policy.yaml
link_budget:
  normal_kbps: 50                       # Normal connectivity
  degraded_kbps: 5                      # Poor connectivity
  offline_buffer_mb: 100                # Flash buffer for offline

priorities:
  critical:
    buffer_type: persistent             # Persist to flash
    ram_buffer_kb: 1024
    flash_buffer_kb: 51200              # 50MB
    drop_policy: never
    max_latency_s: 5

  high:
    buffer_type: ring
    ram_buffer_kb: 5120                 # 5MB
    flash_buffer_kb: 30720              # 30MB
    drop_policy: oldest_first
    downsample_when_constrained: true

  medium:
    buffer_type: ring
    ram_buffer_kb: 2048
    flash_buffer_kb: 15360              # 15MB
    drop_policy: oldest_first
    downsample_when_constrained: true
    aggregate_when_offline: true

  low:
    buffer_type: ring
    ram_buffer_kb: 512
    flash_buffer_kb: 5120               # 5MB
    drop_policy: drop_all_when_constrained

# Map DDS topics to priorities
topic_priority:
  "rt/events/vehicle": critical
  "rt/vss/signals": high
  "rt/diagnostics/*": medium
  "rt/telemetry/*": medium
  "rt/logs/*": low

# Signal-specific offboard rules (overrides topic defaults)
signal_rules:
  - path: "Vehicle.Speed"
    offboard_max_hz: 1.0                # Downsample from 100Hz to 1Hz
    offboard_mode: periodic

  - path: "Vehicle.CurrentLocation.*"
    offboard_max_hz: 0.1                # Every 10 seconds
    offboard_mode: on_change_with_heartbeat
    heartbeat_s: 60

  - path: "Vehicle.Powertrain.TractionBattery.StateOfCharge.Current"
    offboard_max_hz: 0.1
    offboard_mode: on_change
    change_threshold: 1.0               # Only if SOC changes by 1%
```

## Priority-Based Data Handling

### Priority Levels

| Priority | Data Types | Buffer | Offline Behavior | Constrained Behavior |
|----------|-----------|--------|------------------|---------------------|
| **CRITICAL** | Events, safety alerts | Persistent (flash) | Store all, sync when online | Always send first |
| **HIGH** | VSS signals | Ring buffer | Keep latest N per signal | Downsample aggressively |
| **MEDIUM** | Diagnostics, metrics | Ring buffer | Keep latest snapshot, aggregate | Downsample or pause |
| **LOW** | Logs, debug telemetry | Ring buffer | Drop oldest aggressively | Drop all |

### Bandwidth Allocation

When link budget is constrained, bandwidth is allocated by priority:

```
Available: 50 KB/s (normal)
├── CRITICAL: Gets what it needs (typically 2-5 KB/s)
├── HIGH:     60% of remaining (~27 KB/s)
├── MEDIUM:   30% of remaining (~13 KB/s)
└── LOW:      10% of remaining (~5 KB/s)

Available: 5 KB/s (degraded)
├── CRITICAL: Gets what it needs (~2 KB/s)
├── HIGH:     Heavily downsampled (~2 KB/s)
├── MEDIUM:   Paused or minimal (~1 KB/s)
└── LOW:      Dropped (0 KB/s)

Available: 0 (offline)
├── CRITICAL: Buffer to flash, never drop
├── HIGH:     Buffer to flash, keep latest per signal
├── MEDIUM:   Aggregate summaries only
└── LOW:      Drop after RAM buffer full
```

## Offboard Sampling Modes

| Mode | Description | Use Case |
|------|-------------|----------|
| `periodic` | Publish at fixed rate (up to offboard_max_hz) | Speed, location - need regular updates |
| `on_change` | Publish only when value changes beyond threshold | Switches, discrete states, SOC |
| `on_change_with_heartbeat` | on_change + periodic heartbeat even if unchanged | Slow-changing values needing proof of life |

## Signal Aggregation (Offline/Constrained)

When bandwidth is constrained or during offline periods, signals can be aggregated:

```cpp
struct AggregatedSignal {
    std::string path;
    int64_t period_start_ns;
    int64_t period_end_ns;
    uint32_t sample_count;

    double min_value;
    double max_value;
    double avg_value;
    double last_value;
};
// Cloud receives: "Speed was 50-80 km/h, avg 65, ended at 72 over 5 minutes"
```

## VDR Internal Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                   VDR                                       │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                        DDS Subscriber                                  │ │
│  │  Receives full-rate data from all configured topics                   │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                    │                                        │
│                                    ▼                                        │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                     Offboard Policy Engine                             │ │
│  │                                                                        │ │
│  │  - Applies signal_rules (downsampling, change detection)              │ │
│  │  - Routes to priority buffers                                         │ │
│  │  - Tracks per-signal state (last value, last send time)               │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                    │                                        │
│                                    ▼                                        │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                       Priority Buffers                                 │ │
│  │                                                                        │ │
│  │  ┌────────────┐ ┌────────────┐ ┌────────────┐ ┌────────────┐         │ │
│  │  │  CRITICAL  │ │    HIGH    │ │   MEDIUM   │ │    LOW     │         │ │
│  │  │ persistent │ │ ring + agg │ │ ring + agg │ │ ring only  │         │ │
│  │  │  to flash  │ │  to flash  │ │  summaries │ │  drop old  │         │ │
│  │  └────────────┘ └────────────┘ └────────────┘ └────────────┘         │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                    │                                        │
│                                    ▼                                        │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                      Bandwidth Allocator                               │ │
│  │                                                                        │ │
│  │  - Receives link state from connectivity monitor                      │ │
│  │  - Allocates bytes/sec per priority level                             │ │
│  │  - Triggers aggregation when constrained                              │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                    │                                        │
│                                    ▼                                        │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                          Encoder                                       │ │
│  │                                                                        │ │
│  │  - Batches messages by topic                                          │ │
│  │  - Serializes to compact format (MessagePack/Protobuf)                │ │
│  │  - Optional compression for large batches                             │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                    │                                        │
│                                    ▼                                        │
│                              MQTT Publisher                                 │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Transmission Format

### Format Selection by Data Type

| Data Type | Format | Rationale |
|-----------|--------|-----------|
| Events | Protobuf | Self-describing, schema evolution, reliability critical |
| VSS Signals | MessagePack | Compact, schema-less, high volume |
| Metrics | MessagePack | Compact, flexible labels |
| Logs | MessagePack | Variable structure |
| Aggregated | MessagePack | Custom structure |

### Size Comparison (10 VSS signals)

| Format | Approximate Size |
|--------|------------------|
| JSON | ~1500 bytes |
| MessagePack | ~400 bytes |
| Protobuf | ~350 bytes |
| Custom binary + dictionary | ~150 bytes |

### Batching

Signals are batched before transmission:

```cpp
struct EncodedSignalBatch {
    uint64_t base_timestamp_ns;         // Batch timestamp
    std::string source_id;              // Common source
    std::vector<EncodedSignal> signals; // Individual signals with delta timestamps
};
```

## Directory Structure

```
/home/saka/BALI/DDS/
├── SPECIFICATION.md              # This document
├── install_deps.sh               # Dependency installation script
├── run_all.sh                    # Start all services
├── CMakeLists.txt                # Root CMake configuration
├── idl/
│   └── telemetry.idl             # DDS IDL definitions
├── src/
│   ├── common/
│   │   ├── dds_wrapper.hpp       # RAII wrappers for Cyclone DDS
│   │   ├── dds_wrapper.cpp
│   │   ├── qos_profiles.hpp      # QoS profile definitions
│   │   ├── qos_profiles.cpp
│   │   ├── time_utils.hpp        # Time utilities
│   │   └── time_utils.cpp
│   ├── vdr/
│   │   ├── main.cpp              # VDR entry point
│   │   ├── subscriber.hpp        # DDS subscription management
│   │   ├── subscriber.cpp
│   │   ├── encoder.hpp           # Message encoding
│   │   ├── encoder.cpp
│   │   ├── offboard_policy.hpp   # Offboard policy engine (planned)
│   │   ├── offboard_policy.cpp
│   │   ├── priority_buffer.hpp   # Priority-based buffering (planned)
│   │   ├── priority_buffer.cpp
│   │   ├── bandwidth_allocator.hpp # Bandwidth allocation (planned)
│   │   └── bandwidth_allocator.cpp
│   └── probes/
│       ├── vss_probe/
│       │   └── main.cpp
│       ├── metrics_probe/
│       │   └── main.cpp
│       ├── event_probe/
│       │   └── main.cpp
│       └── (probes moved to vep-core)
├── config/
│   ├── vdr_config.yaml           # Basic VDR config (current)
│   ├── vdr_offboard_policy.yaml  # Offboard policy (planned)
│   ├── vssdag_probe_config.yaml  # VSS DAG probe signal mappings
│   └── sample_vehicle.dbc        # Sample DBC file for testing
└── tests/
    └── test_dds_wrapper.cpp
```

## IDL Definitions

### Common Header

```idl
module telemetry {
    struct Header {
        string source_id;
        long long timestamp_ns;
        unsigned long seq_num;
        string correlation_id;
    };

    struct KeyValue {
        string key;
        string value;
    };
};
```

### VSS Signals

```idl
module telemetry {
module vss {
    enum Quality {
        QUALITY_NOT_AVAILABLE,
        QUALITY_VALID,
        QUALITY_INVALID
    };

    enum ValueType {
        VALUE_TYPE_BOOL,
        VALUE_TYPE_INT32,
        VALUE_TYPE_INT64,
        VALUE_TYPE_FLOAT,
        VALUE_TYPE_DOUBLE,
        VALUE_TYPE_STRING
    };

    struct Signal {
        string path;
        telemetry::Header header;
        Quality quality;
        ValueType value_type;
        boolean bool_value;
        long int32_value;
        long long int64_value;
        float float_value;
        double double_value;
        string string_value;
    };
    #pragma keylist Signal path
};
};
```

### Vehicle Events

```idl
module telemetry {
module events {
    enum Severity {
        SEVERITY_INFO,
        SEVERITY_WARNING,
        SEVERITY_ERROR,
        SEVERITY_CRITICAL
    };

    struct Event {
        string event_id;
        telemetry::Header header;
        string category;
        string event_type;
        Severity severity;
        sequence<octet> payload;
    };
    #pragma keylist Event event_id
};
};
```

### Diagnostic Measurements

```idl
module telemetry {
module diagnostics {
    enum MeasurementType {
        MEASUREMENT_TYPE_ACCUMULATED,
        MEASUREMENT_TYPE_MOMENTARY
    };

    struct ScalarMeasurement {
        string variable_id;
        telemetry::Header header;
        string unit;
        MeasurementType mtype;
        double value;
    };
    #pragma keylist ScalarMeasurement variable_id

    struct VectorMeasurement {
        string variable_id;
        telemetry::Header header;
        string unit;
        MeasurementType mtype;
        sequence<double> values;
    };
    #pragma keylist VectorMeasurement variable_id
};
};
```

### Telemetry Metrics

```idl
module telemetry {
module metrics {
    struct Counter {
        string name;
        telemetry::Header header;
        sequence<telemetry::KeyValue> labels;
        double value;
    };
    #pragma keylist Counter name

    struct Gauge {
        string name;
        telemetry::Header header;
        sequence<telemetry::KeyValue> labels;
        double value;
    };
    #pragma keylist Gauge name

    struct HistogramBucket {
        double upper_bound;
        unsigned long long cumulative_count;
    };

    struct Histogram {
        string name;
        telemetry::Header header;
        sequence<telemetry::KeyValue> labels;
        unsigned long long sample_count;
        double sample_sum;
        sequence<HistogramBucket> buckets;
    };
    #pragma keylist Histogram name
};
};
```

### Log Entries

```idl
module telemetry {
module logs {
    enum Level {
        LEVEL_DEBUG,
        LEVEL_INFO,
        LEVEL_WARN,
        LEVEL_ERROR
    };

    struct LogEntry {
        telemetry::Header header;
        Level level;
        string component;
        string message;
        sequence<telemetry::KeyValue> fields;
    };
    #pragma keylist LogEntry
};
};
```

## DDS Topic Configuration

| Topic Name | IDL Type | QoS Profile | Description |
|------------|----------|-------------|-------------|
| `rt/vss/signals` | `telemetry::vss::Signal` | Reliable, Keep Last 100 | VSS signal updates |
| `rt/events/vehicle` | `telemetry::events::Event` | Reliable, Keep All, Transient Local | Vehicle events |
| `rt/diagnostics/scalar` | `telemetry::diagnostics::ScalarMeasurement` | Reliable, Keep Last 10 | Scalar diagnostics |
| `rt/diagnostics/vector` | `telemetry::diagnostics::VectorMeasurement` | Reliable, Keep Last 10 | Vector diagnostics |
| `rt/telemetry/counters` | `telemetry::metrics::Counter` | Best Effort, Keep Last 1 | Prometheus counters |
| `rt/telemetry/gauges` | `telemetry::metrics::Gauge` | Best Effort, Keep Last 1 | Prometheus gauges |
| `rt/telemetry/histograms` | `telemetry::metrics::Histogram` | Best Effort, Keep Last 1 | Prometheus histograms |
| `rt/logs/entries` | `telemetry::logs::LogEntry` | Best Effort, Keep Last 100 | Log entries |
| `rt/avtp/can/frames` | `telemetry::avtp::AcfCanFrame` | Reliable, Keep Last 500 | IEEE 1722 CAN frames |
| `rt/avtp/can/tx` | `telemetry::avtp::AcfCanFrame` | Reliable, Keep Last 100 | CAN frames to transmit |
| `rt/avtp/stats` | `telemetry::avtp::StreamStats` | Best Effort, Keep Last 1 | Stream statistics |

## QoS Profiles

### Reliable Critical (Events)

```cpp
Qos qos;
qos.reliability_reliable(DDS_SECS(10))
   .durability_transient_local()
   .history_keep_all();
```

### Reliable Standard (VSS, Diagnostics)

```cpp
Qos qos;
qos.reliability_reliable(DDS_SECS(1))
   .durability_volatile()
   .history_keep_last(100);
```

### Best Effort (Telemetry, Logs)

```cpp
Qos qos;
qos.reliability_best_effort()
   .durability_volatile()
   .history_keep_last(1);
```

## Dependencies

| Package | Purpose | Ubuntu Package |
|---------|---------|----------------|
| Cyclone DDS | DDS implementation | `cyclonedds-dev`, `cyclonedds-tools` |
| glog | Logging | `libgoogle-glog-dev` |
| yaml-cpp | Configuration parsing | `libyaml-cpp-dev` |
| nlohmann-json | JSON encoding | `nlohmann-json3-dev` |
| GoogleTest | Unit testing | `libgtest-dev` |
| gRPC | OTLP transport | `libgrpc++-dev` |
| Protobuf | OTLP serialization | `libprotobuf-dev`, `protobuf-compiler` |
| Lua | libvssdag scripting | `liblua5.4-dev` |
| libvssdag | CAN-to-VSS transforms | Build from source |
| Open1722 | IEEE 1722 AVTP | Build from source |
| CMake | Build system | `cmake` |

## Implementation Status

### Completed (PoC)

- [x] DDS RAII wrappers
- [x] QoS profiles
- [x] IDL definitions and code generation
- [x] VDR basic subscriber
- [x] JSON encoder (simulated MQTT)
- [x] Sample probes (VSS, Metrics, Events)
- [x] OTLP-to-DDS bridge probe (metrics, logs via gRPC)
- [x] VSS DAG probe (CAN-to-VSS with libvssdag)
- [x] AVTP probe (IEEE 1722 CAN-over-Ethernet with Open1722)
- [x] Basic configuration
- [x] Unit tests

### Planned

- [ ] Offboard policy engine
- [ ] Priority-based buffers
- [ ] Bandwidth allocator
- [ ] Signal throttling and change detection
- [ ] Signal aggregation
- [ ] MessagePack encoder
- [ ] Flash persistence for offline buffering
- [ ] MQTT publisher
- [ ] Probe configuration (signal lists, sample rates)
- [ ] Connectivity monitor integration

## CAN-to-VSS Integration (libvssdag)

### Overview

The VSS DAG probe uses [libvssdag](https://github.com/tr-sdv-sandbox/libvssdag) to transform raw CAN signals into VSS format through a dependency-aware DAG pipeline with Lua scripting.

### Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           VSS DAG Probe                                      │
│                                                                              │
│  ┌────────────────┐                                                         │
│  │  CAN Interface │  SocketCAN (can0, vcan0, etc.)                         │
│  │  (SocketCAN)   │                                                         │
│  └───────┬────────┘                                                         │
│          │ Raw CAN frames                                                    │
│          ▼                                                                   │
│  ┌────────────────┐                                                         │
│  │   DBC Parser   │  Decode using .dbc signal definitions                   │
│  │  (libdbcppp)   │                                                         │
│  └───────┬────────┘                                                         │
│          │ SignalUpdate (name, value, quality)                              │
│          ▼                                                                   │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │                    SignalProcessorDAG (libvssdag)                       │ │
│  │                                                                         │ │
│  │  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐                 │ │
│  │  │ Topological │───►│ LuaMapper   │───►│ Quality     │                 │ │
│  │  │ Sort        │    │ (transforms)│    │ Propagation │                 │ │
│  │  └─────────────┘    └─────────────┘    └─────────────┘                 │ │
│  │                                                                         │ │
│  │  Built-in Lua functions:                                                │ │
│  │  • lowpass(x, α)        - Low-pass filter                              │ │
│  │  • moving_average(x, n) - Moving average                                │ │
│  │  • derivative(x)        - Rate of change                                │ │
│  │  • threshold(x, lo, hi) - Clamp to range                               │ │
│  │  • delayed(x, ms)       - Delayed propagation                          │ │
│  │  • sustained_condition(cond, ms) - Debouncing                          │ │
│  │  • state                - Persistent state table                        │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│          │ VSSSignal (path, qualified_value)                                │
│          ▼                                                                   │
│  ┌────────────────┐                                                         │
│  │   DDS Writer   │  Publish to rt/vss/signals                             │
│  └────────────────┘                                                         │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Signal Mapping Configuration

Signal mappings define how CAN signals are transformed to VSS:

```yaml
# config/vssdag_probe_config.yaml
signals:
  # Direct CAN-to-VSS with filtering
  - signal: Vehicle.Speed
    source:
      type: dbc
      name: DI_vehicleSpeed         # CAN signal name from DBC
    datatype: float
    transform:
      code: "return lowpass(x, 0.3)"
    interval_ms: 100

  # Derived signal (depends on another signal)
  - signal: Vehicle.Acceleration.Longitudinal
    depends_on:
      - Vehicle.Speed
    datatype: float
    transform:
      code: |
        local accel = derivative(deps['Vehicle.Speed'])
        return accel / 3.6  -- km/h/s to m/s²
    update_trigger: on_dependency

  # Value mapping for discrete signals
  - signal: Vehicle.Powertrain.Transmission.CurrentGear
    source:
      type: dbc
      name: DI_gear
    datatype: int32
    transform:
      value_map:
        "0": "-1"   # Reverse
        "1": "0"    # Park
        "2": "1"    # Drive
```

### Usage

```bash
# With real CAN interface and DBC file
./probe_vssdag --interface can0 --dbc config/sample_vehicle.dbc \
               --config config/vssdag_probe_config.yaml

# With virtual CAN for testing
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
./probe_vssdag --interface vcan0 --dbc config/sample_vehicle.dbc

# Simulation mode (no CAN, generates test data)
./probe_vssdag --config config/vssdag_probe_config.yaml
```

### Key Features

| Feature | Description |
|---------|-------------|
| **DBC Parsing** | Automatic CAN message decoding using standard DBC files |
| **Dependency DAG** | Topological sorting ensures derived signals update in correct order |
| **Lua Transforms** | Flexible scripting for filters, calculations, state machines |
| **Quality Tracking** | VALID/INVALID/NOT_AVAILABLE propagates through the pipeline |
| **Throttling** | Per-signal output rate limiting via `interval_ms` |
| **Lock-free Queuing** | Real-time safe signal handling |

### Quality Mapping

| libvssdag Quality | DDS Quality |
|-------------------|-------------|
| `VALID` | `QUALITY_VALID` |
| `INVALID` | `QUALITY_INVALID` |
| `NOT_AVAILABLE` | `QUALITY_NOT_AVAILABLE` |

## OpenTelemetry Collector Integration

### Overview

Applications instrumented with OpenTelemetry SDK can send telemetry (metrics, logs) to the DDS domain via an OTLP-to-DDS bridge probe. This allows existing observability patterns to integrate seamlessly with the vehicle data bus.

### Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           ECU / Container                                    │
│                                                                              │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐             │
│  │ App with OTel   │  │ Service with    │  │ Container with  │             │
│  │ SDK (metrics)   │  │ OTel SDK (logs) │  │ OTel agent      │             │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘             │
│           │ OTLP/gRPC          │ OTLP/gRPC          │ OTLP/gRPC            │
│           └────────────────────┼────────────────────┘                       │
│                                ▼                                             │
│                   ┌─────────────────────────────┐                           │
│                   │   OTLP-to-DDS Bridge Probe  │                           │
│                   │                             │                           │
│                   │  ┌───────────────────────┐  │                           │
│                   │  │  MetricsService (gRPC)│  │  port 4317 (default)     │
│                   │  │  LogsService (gRPC)   │  │                           │
│                   │  └───────────────────────┘  │                           │
│                   │              │              │                           │
│                   │  ┌───────────▼───────────┐  │                           │
│                   │  │  OTel → DDS Converter │  │                           │
│                   │  │                       │  │                           │
│                   │  │  • Gauge → Gauge      │  │                           │
│                   │  │  • Sum (mono) → Ctr   │  │                           │
│                   │  │  • Histogram → Hist   │  │                           │
│                   │  │  • LogRecord → Log    │  │                           │
│                   │  └───────────────────────┘  │                           │
│                   │              │              │                           │
│                   │  ┌───────────▼───────────┐  │                           │
│                   │  │     DDS Writers       │  │                           │
│                   │  │  rt/telemetry/*       │  │                           │
│                   │  │  rt/logs/entries      │  │                           │
│                   │  └───────────────────────┘  │                           │
│                   └──────────────┬──────────────┘                           │
│                                  │ DDS                                       │
└──────────────────────────────────┼───────────────────────────────────────────┘
                                   ▼
                          ┌────────────────┐
                          │   DDS Domain   │
                          │                │
                          │  VDR + other   │
                          │  consumers     │
                          └────────────────┘
```

### Integration Options

#### Option A: Direct OTLP Receiver (Implemented)

The `vdr_otel_probe` binary (in vep-core) implements OTLP gRPC services directly:

```bash
# Start the probe on default port 4317
./vdr_otel_probe

# Or specify custom port
./vdr_otel_probe 4318
```

Configure OTel SDK exporters to point to the bridge:

```yaml
# OTel SDK configuration
exporters:
  otlp:
    endpoint: "localhost:4317"
    tls:
      insecure: true
```

#### Option B: With OTel Collector (Recommended for production)

Use standard OTel Collector for processing, then export to the bridge:

```yaml
# otel-collector-config.yaml
receivers:
  otlp:
    protocols:
      grpc:
        endpoint: 0.0.0.0:4317

processors:
  batch:
    timeout: 1s

exporters:
  otlp/dds:
    endpoint: "otel-bridge:4318"   # Our bridge
    tls:
      insecure: true

service:
  pipelines:
    metrics:
      receivers: [otlp]
      processors: [batch]
      exporters: [otlp/dds]
    logs:
      receivers: [otlp]
      processors: [batch]
      exporters: [otlp/dds]
```

This provides:
- Standard OTel processing (batching, filtering, sampling)
- Multi-backend support (can also send to Prometheus, Jaeger, etc.)
- Schema evolution and data validation

### Data Type Mapping

| OTel Type | DDS Type | Notes |
|-----------|----------|-------|
| Gauge | `telemetry::metrics::Gauge` | Direct 1:1 mapping |
| Sum (monotonic=true) | `telemetry::metrics::Counter` | Counters |
| Sum (monotonic=false) | `telemetry::metrics::Gauge` | Up-down counters treated as gauges |
| Histogram | `telemetry::metrics::Histogram` | Bucket mapping |
| ExponentialHistogram | — | Not supported (logged as warning) |
| Summary | — | Not supported (legacy) |
| LogRecord | `telemetry::logs::LogEntry` | Severity and attributes mapped |

### Attribute Mapping

OTel resource and data point attributes are mapped to DDS message fields:

| OTel Attribute | DDS Field |
|---------------|-----------|
| `service.name` | `header.source_id` |
| `instrumentation_scope.name` | `component` (for logs) |
| Data point attributes | `labels` (for metrics) |
| Log attributes | `fields` (for logs) |

### Severity Mapping

| OTel Severity | DDS Level |
|--------------|-----------|
| TRACE, DEBUG | LEVEL_DEBUG |
| INFO | LEVEL_INFO |
| WARN | LEVEL_WARN |
| ERROR, FATAL | LEVEL_ERROR |

### Resource Requirements

The OTLP-to-DDS bridge is lightweight:

- Memory: ~10-20 MB
- CPU: Minimal (gRPC handling + DDS writes)
- Dependencies: gRPC, Protobuf, Cyclone DDS

### Example: Instrumenting a C++ Application

```cpp
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_metric_exporter.h>

// Configure OTLP exporter to point to the bridge
otlp::OtlpGrpcMetricExporterOptions opts;
opts.endpoint = "localhost:4317";
opts.use_ssl_credentials = false;

auto exporter = otlp::OtlpGrpcMetricExporterFactory::Create(opts);
auto provider = sdk::metrics::MeterProviderFactory::Create();
// ... configure and use metrics
```

## IEEE 1722 AVTP Integration (Open1722)

### Overview

The AVTP probe bridges CAN frames transported over Automotive Ethernet (IEEE 1722) to the DDS domain. This implements the MCU ↔ HPC communication layer described in the RT/HPC specification:

```
MCU → CAN frame → IEEE 1722 AVTPDU → Ethernet → HPC → DDS → SWCs
```

### Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              MCU Domain                                      │
│                                                                              │
│  ┌────────────────┐    ┌────────────────┐    ┌────────────────┐            │
│  │  CAN Controller│    │  CAN Controller│    │  LIN Controller│            │
│  │  (Vehicle Bus) │    │  (Powertrain)  │    │  (Body)        │            │
│  └───────┬────────┘    └───────┬────────┘    └───────┬────────┘            │
│          │                     │                     │                      │
│          └─────────────────────┼─────────────────────┘                      │
│                                ▼                                             │
│                   ┌─────────────────────────┐                               │
│                   │    MCU Middleware       │                               │
│                   │  - Frame → AVTPDU       │                               │
│                   │  - Manifest routing     │                               │
│                   │  - Time synchronization │                               │
│                   └────────────┬────────────┘                               │
│                                │ IEEE 1722 (AVTP)                           │
└────────────────────────────────┼────────────────────────────────────────────┘
                                 │ Automotive Ethernet (100BASE-T1)
                                 ▼
┌────────────────────────────────────────────────────────────────────────────┐
│                              HPC Domain                                      │
│                                                                              │
│                   ┌─────────────────────────┐                               │
│                   │     AVTP Probe          │                               │
│                   │  (probe_avtp)           │                               │
│                   │                         │                               │
│                   │  ┌───────────────────┐  │                               │
│                   │  │ Open1722 Parser   │  │  ACF CAN/LIN decapsulation   │
│                   │  │ (libopen1722)     │  │                               │
│                   │  └─────────┬─────────┘  │                               │
│                   │            │            │                               │
│                   │  ┌─────────▼─────────┐  │                               │
│                   │  │ DDS Writers       │  │                               │
│                   │  │ rt/avtp/can/*     │  │                               │
│                   │  └───────────────────┘  │                               │
│                   └────────────┬────────────┘                               │
│                                │ DDS                                         │
│                                ▼                                             │
│   ┌────────────────────────────────────────────────────────────────────┐   │
│   │                         DDS Domain                                   │   │
│   │                                                                      │   │
│   │  Topics:                                                             │   │
│   │  ├── rt/avtp/can/frames   (Individual ACF CAN frames)              │   │
│   │  ├── rt/avtp/can/tx       (Frames to transmit back to MCU)         │   │
│   │  └── rt/avtp/stats        (Stream statistics)                       │   │
│   │                                                                      │   │
│   │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐               │   │
│   │  │ VSSDAG  │  │  ADAS   │  │  Body   │  │   VDR   │               │   │
│   │  │ Probe   │  │ SWC     │  │ Control │  │         │               │   │
│   │  └─────────┘  └─────────┘  └─────────┘  └─────────┘               │   │
│   └────────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────────────┘
```

### IEEE 1722 Frame Types Supported

| Type | Description | Use Case |
|------|-------------|----------|
| **TSCF** | Time-Synchronous Control Format | Time-critical CAN frames with precise timing |
| **NTSCF** | Non-Time-Synchronous Control Format | Best-effort CAN frames |
| **ACF CAN** | AVTP Control Format for CAN | Full CAN frame with all flags |
| **ACF CAN Brief** | Compact CAN format | Reduced overhead for simple messages |

### DDS Topic Schema

#### ACF CAN Frame

```idl
module telemetry {
module avtp {
    struct CanFlags {
        boolean is_extended_id;   /* Extended (29-bit) vs standard (11-bit) */
        boolean is_fd;            /* CAN-FD frame */
        boolean is_brs;           /* Bit rate switch (CAN-FD) */
        boolean is_esi;           /* Error state indicator */
        boolean is_rtr;           /* Remote transmission request */
    };

    struct AcfCanFrame {
        telemetry::Header header;
        unsigned long long stream_id;     /* AVTP stream identifier */
        unsigned long can_id;             /* CAN arbitration ID */
        octet bus_id;                     /* CAN bus identifier (0-31) */
        CanFlags flags;
        sequence<octet, 64> payload;      /* Up to 64 bytes for CAN-FD */
        unsigned long long avtp_timestamp;
        unsigned long sequence_num;
    };
    #pragma keylist AcfCanFrame stream_id can_id
};
};
```

### Usage

```bash
# Listen on eth0 for AVTP frames
./probe_avtp --interface eth0

# Filter specific stream ID
./probe_avtp --interface eth0 --stream-id 0x0011223344556677

# Enable bidirectional (also send DDS frames as AVTP)
./probe_avtp --interface eth0 --tx

# Simulation mode (generates test frames)
./probe_avtp --simulate
```

### Integration with VSSDAG Probe

The AVTP probe can work alongside the VSSDAG probe for a complete CAN-to-VSS pipeline:

```
                    ┌────────────────────────────────────────────┐
                    │              Integration Options            │
                    └────────────────────────────────────────────┘

Option A: Direct (raw CAN access on HPC)
─────────────────────────────────────────
┌──────────┐        ┌──────────────┐        ┌─────────────┐
│ CAN Bus  │──────►│ VSSDAG Probe │──────►│ DDS Domain  │
│ (can0)   │        │ (DBC decode) │        │ rt/vss/*    │
└──────────┘        └──────────────┘        └─────────────┘

Option B: Via AVTP (CAN tunneled from MCU)
──────────────────────────────────────────
┌──────────┐        ┌──────────────┐        ┌─────────────┐
│ MCU      │──────►│ AVTP Probe   │──────►│ DDS Domain  │
│ (Eth)    │ AVTP   │              │        │ rt/avtp/*   │
└──────────┘        └──────────────┘        └─────────────┘
                           │
                           │ DDS (internal)
                           ▼
                    ┌──────────────┐        ┌─────────────┐
                    │ VSSDAG Probe │──────►│ DDS Domain  │
                    │ (subscribe   │        │ rt/vss/*    │
                    │  rt/avtp/*)  │        │             │
                    └──────────────┘        └─────────────┘

Option C: Combined Probe (planned)
──────────────────────────────────
┌──────────┐        ┌──────────────────────────────────────────┐
│ MCU      │──────►│           AVTP + VSSDAG Probe            │
│ (Eth)    │ AVTP   │  ┌────────────┐    ┌────────────────┐   │
└──────────┘        │  │ Open1722   │───►│ SignalProcessor│   │
                    │  │ Parser     │    │ DAG            │   │
                    │  └────────────┘    └───────┬────────┘   │
                    │                            │            │
                    │                    ┌───────▼────────┐   │
                    │                    │ DDS Writer     │   │
                    │                    │ rt/vss/signals │   │
                    │                    └────────────────┘   │
                    └─────────────────────────────────────────┘
```

### Stream Statistics

The probe publishes periodic stream statistics for monitoring:

```idl
struct StreamStats {
    telemetry::Header header;
    unsigned long long stream_id;
    unsigned long long frames_received;
    unsigned long long frames_sent;
    unsigned long long sequence_errors;    /* Gaps in AVTP sequence */
    unsigned long long timestamp_errors;   /* Timing violations */
    unsigned long long bytes_total;
    double average_latency_us;
};
```

### Network Requirements

| Parameter | Requirement |
|-----------|-------------|
| **Interface** | Ethernet with raw socket support |
| **Ethertype** | 0x22F0 (AVTP) |
| **Permissions** | CAP_NET_RAW or root (for raw sockets) |
| **Multicast** | Optional (for stream discovery) |

### Dependencies

| Library | Purpose |
|---------|---------|
| [Open1722](https://github.com/COVESA/Open1722) | IEEE 1722 parsing/serialization |
| Linux raw sockets | Network I/O |
| Cyclone DDS | DDS transport |

## Future Work (Out of Scope)

- Android bridge (WebSocket/gRPC)
- OTLP Traces support (spans)
- vsomeip bridge
- DDS Security plugins
- Multi-domain support
- Path dictionary for ultra-compact encoding
- Combined AVTP + VSSDAG probe
- ACF LIN / FlexRay support
