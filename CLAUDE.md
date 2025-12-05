# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Install dependencies (Ubuntu 24.04)
./install_deps.sh

# Configure and build
cmake -B build
cmake --build build -j$(nproc)

# Run tests
./build/test_dds_wrapper

# Run individual components
./build/vdr                       # Main VDR subscriber
./build/vdr_vss_probe             # VSS signal probe
./build/vdr_metrics_probe         # Prometheus-style metrics probe
./build/vdr_event_probe           # Vehicle events probe
```

## Architecture Overview

This is a **Vehicle Data Readout (VDR) ecosystem** using Cyclone DDS as the onboard data bus. The system collects telemetry from various sources via **probes** and routes it through DDS topics for consumption by onboard systems and eventual cloud transmission.

### Core Components

**vdr_common library** (`src/common/`): RAII C++ wrappers around Cyclone DDS C API
- `dds_wrapper.hpp` - Type-safe wrappers: `dds::Participant`, `dds::Topic`, `dds::Writer`, `dds::Reader`, `dds::Qos`
- `qos_profiles.hpp` - Predefined QoS: `reliable_critical()`, `reliable_standard(depth)`, `best_effort(depth)`
- `time_utils.hpp` - Timestamp utilities: `utils::now_ns()`, `utils::generate_uuid()`

**VDR** (`src/vdr/`): Central subscriber that receives all DDS data, applies offboard policies, and encodes for transmission

**Probes** (`src/probes/`): Data collectors that publish to DDS topics
- Each probe is a standalone executable with its own main.cpp
- All probes use the pattern: signal handlers for graceful shutdown, DDS entity creation, main loop with rate control

### IDL and Message Types

Message definitions are in `idl/telemetry.idl`. The IDL compiler generates C code in `build/generated/telemetry.h`.

Key modules:
- `telemetry::vss::Signal` - VSS signal with quality and typed value
- `telemetry::events::Event` - Vehicle events with severity
- `telemetry::metrics::{Gauge,Counter,Histogram}` - Prometheus-style metrics
- `telemetry::avtp::AcfCanFrame` - IEEE 1722 CAN-over-Ethernet frames
- `telemetry::logs::LogEntry` - Structured logging

All messages include a common `telemetry::Header` with `source_id`, `timestamp_ns`, `seq_num`, `correlation_id`.

### DDS Topic Naming

Topics follow the pattern `rt/<category>/<subcategory>`:
- `rt/vss/signals` - VSS signals
- `rt/events/vehicle` - Vehicle events
- `rt/avtp/can/frames` - IEEE 1722 CAN frames
- `rt/telemetry/{gauges,counters,histograms}` - Metrics
- `rt/logs/entries` - Log entries

### Optional Dependencies

Some probes require external libraries (built conditionally):
- **libvssdag** - CAN-to-VSS transformation with Lua scripting
- **Open1722** - IEEE 1722 AVTP parsing for CAN-over-Ethernet

Check cmake output for which optional probes are enabled.

### Writing DDS Messages

DDS-generated structs use C-style strings. When populating messages:
```cpp
std::string source_id = "my_probe";
msg.header.source_id = const_cast<char*>(source_id.c_str());
msg.header.timestamp_ns = utils::now_ns();
msg.header.seq_num = seq++;
writer.write(msg);
```

String buffers must remain valid until after `write()` completes.

### Reading DDS Messages

Use `take_each` with explicit template parameter for type-safe reading:
```cpp
reader.take_each<telemetry_vss_Signal>([](const telemetry_vss_Signal& msg) {
    // Process msg - valid only within callback
}, max_samples);
```
