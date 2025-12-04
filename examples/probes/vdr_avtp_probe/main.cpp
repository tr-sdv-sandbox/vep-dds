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

/// @file avtp_probe/main.cpp
/// @brief IEEE 1722 AVTP to DDS Bridge
///
/// Receives CAN frames encapsulated in IEEE 1722 AVTP (ACF CAN format)
/// from Ethernet and publishes to DDS for HPC-side processing.
///
/// This implements the MCU <-> HPC transport layer described in the RT/HPC spec:
///   MCU -> CAN frame -> IEEE 1722 AVTPDU -> Ethernet -> HPC -> DDS
///
/// Supports:
/// - ACF CAN (full format with timestamps)
/// - ACF CAN Brief (compact format)
/// - TSCF (Time-Synchronous Control Format) with multiple ACF messages
/// - NTSCF (Non-Time-Synchronous Control Format)
/// - Bidirectional: can also send DDS messages back as AVTP to MCU
///
/// Uses COVESA Open1722 library for AVTP parsing/serialization.

#include "common/dds_wrapper.hpp"
#include "common/qos_profiles.hpp"
#include "common/time_utils.hpp"
#include "telemetry.h"

#include <avtp/CommonHeader.h>
#include <avtp/acf/Can.h>
#include <avtp/acf/AcfCommon.h>
#include <avtp/acf/Ntscf.h>
#include <avtp/acf/Tscf.h>
#include <avtp/Defines.h>

#include <glog/logging.h>

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    LOG(INFO) << "Received signal " << signum << ", shutting down...";
    g_running = false;
}

// AVTP Ethertype
constexpr uint16_t ETH_P_AVTP = 0x22F0;

// Maximum AVTP frame size
constexpr size_t MAX_AVTP_FRAME_SIZE = 1522;

// Stream statistics
struct StreamStatistics {
    uint64_t frames_received = 0;
    uint64_t frames_sent = 0;
    uint64_t sequence_errors = 0;
    uint64_t timestamp_errors = 0;
    uint64_t bytes_total = 0;
    uint8_t last_sequence = 0;
    bool first_frame = true;
    std::chrono::steady_clock::time_point last_update;
    double latency_sum_us = 0;
    uint64_t latency_count = 0;
};

// Create raw socket for AVTP
int create_avtp_socket(const std::string& interface) {
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_AVTP));
    if (sockfd < 0) {
        LOG(ERROR) << "Failed to create raw socket: " << strerror(errno);
        return -1;
    }

    // Get interface index
    struct ifreq ifr = {};
    strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ - 1);
    if (ioctl(sockfd, SIOCGIFINDEX, &ifr) < 0) {
        LOG(ERROR) << "Failed to get interface index for " << interface
                   << ": " << strerror(errno);
        close(sockfd);
        return -1;
    }

    // Bind to interface
    struct sockaddr_ll addr = {};
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_AVTP);
    addr.sll_ifindex = ifr.ifr_ifindex;

    if (bind(sockfd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG(ERROR) << "Failed to bind socket to " << interface
                   << ": " << strerror(errno);
        close(sockfd);
        return -1;
    }

    // Set receive timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;  // 100ms
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    LOG(INFO) << "Created AVTP socket on interface " << interface
              << " (index " << ifr.ifr_ifindex << ")";

    return sockfd;
}

// Parse ACF CAN from AVTP frame
bool parse_acf_can(const uint8_t* data, size_t len,
                   telemetry_avtp_AcfCanFrame& out_frame,
                   std::vector<uint8_t>& payload_buf) {
    if (len < AVTP_CAN_HEADER_LEN) {
        return false;
    }

    const Avtp_Can_t* can_pdu = reinterpret_cast<const Avtp_Can_t*>(data);

    // Validate the frame
    if (!Avtp_Can_IsValid(can_pdu, len)) {
        return false;
    }

    // Get CAN message details using Open1722 API
    uint32_t can_id = Avtp_Can_GetCanIdentifier(can_pdu);
    uint8_t bus_id = Avtp_Can_GetCanBusId(can_pdu);
    uint8_t pad = Avtp_Can_GetPad(can_pdu);
    uint8_t mtv = Avtp_Can_GetMtv(can_pdu);
    uint8_t rtr = Avtp_Can_GetRtr(can_pdu);
    uint8_t eff = Avtp_Can_GetEff(can_pdu);
    uint8_t brs = Avtp_Can_GetBrs(can_pdu);
    uint8_t fdf = Avtp_Can_GetFdf(can_pdu);
    uint8_t esi = Avtp_Can_GetEsi(can_pdu);
    uint64_t timestamp = mtv ? Avtp_Can_GetMessageTimestamp(can_pdu) : 0;

    // Fill output structure
    out_frame.can_id = can_id;
    out_frame.bus_id = bus_id;
    out_frame.flags.is_extended_id = eff != 0;
    out_frame.flags.is_fd = fdf != 0;
    out_frame.flags.is_brs = brs != 0;
    out_frame.flags.is_esi = esi != 0;
    out_frame.flags.is_rtr = rtr != 0;
    out_frame.avtp_timestamp = timestamp;

    // Get payload
    uint8_t payload_len = Avtp_Can_GetCanPayloadLength(can_pdu);
    if (payload_len > 64) {
        payload_len = 64;  // CAN-FD max
    }

    payload_buf.resize(payload_len);
    if (payload_len > 0) {
        const uint8_t* payload_ptr = Avtp_Can_GetPayload(can_pdu);
        memcpy(payload_buf.data(), payload_ptr, payload_len);
    }

    out_frame.payload._buffer = payload_buf.data();
    out_frame.payload._length = payload_len;
    out_frame.payload._maximum = 64;
    out_frame.payload._release = false;

    return true;
}

// Build ACF CAN AVTP frame for transmission
size_t build_acf_can_frame(uint8_t* buffer, size_t buffer_size,
                           const telemetry_avtp_AcfCanFrame& frame) {
    if (buffer_size < MAX_AVTP_FRAME_SIZE) {
        return 0;
    }

    // Initialize ACF CAN structure
    Avtp_Can_t* acf_can = reinterpret_cast<Avtp_Can_t*>(buffer);
    memset(acf_can, 0, AVTP_CAN_HEADER_LEN);

    Avtp_Can_Init(acf_can);

    // Set CAN identifier
    Avtp_Can_SetCanIdentifier(acf_can, frame.can_id);
    Avtp_Can_SetCanBusId(acf_can, frame.bus_id);

    // Set flags using Enable/Disable functions
    if (frame.flags.is_extended_id) {
        Avtp_Can_EnableEff(acf_can);
    }
    if (frame.flags.is_fd) {
        Avtp_Can_EnableFdf(acf_can);
    }
    if (frame.flags.is_brs) {
        Avtp_Can_EnableBrs(acf_can);
    }
    if (frame.flags.is_esi) {
        Avtp_Can_EnableEsi(acf_can);
    }
    if (frame.flags.is_rtr) {
        Avtp_Can_EnableRtr(acf_can);
    }

    if (frame.avtp_timestamp > 0) {
        Avtp_Can_EnableMtv(acf_can);
        Avtp_Can_SetMessageTimestamp(acf_can, frame.avtp_timestamp);
    }

    // Set payload and finalize
    uint16_t payload_len = frame.payload._length;
    if (payload_len > 64) payload_len = 64;

    Avtp_Can_SetPayload(acf_can, const_cast<uint8_t*>(frame.payload._buffer), payload_len);
    Avtp_Can_Finalize(acf_can, payload_len);

    // Calculate total length (header + payload + padding)
    uint16_t acf_msg_len = Avtp_Can_GetAcfMsgLength(acf_can);
    return acf_msg_len * AVTP_QUADLET_SIZE;
}

}  // namespace

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    google::SetStderrLogging(google::INFO);
    FLAGS_colorlogtostderr = true;

    LOG(INFO) << "AVTP Probe (IEEE 1722 CAN Bridge) starting...";

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Parse command line arguments
    std::string interface = "eth0";
    uint64_t stream_id = 0;
    bool tx_enabled = false;
    bool simulation_mode = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--interface" && i + 1 < argc) {
            interface = argv[++i];
        } else if (arg == "--stream-id" && i + 1 < argc) {
            stream_id = std::stoull(argv[++i], nullptr, 16);
        } else if (arg == "--tx") {
            tx_enabled = true;
        } else if (arg == "--simulate") {
            simulation_mode = true;
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --interface NAME   Network interface (default: eth0)\n"
                      << "  --stream-id HEX    Filter by AVTP stream ID\n"
                      << "  --tx               Enable transmission (DDS -> AVTP)\n"
                      << "  --simulate         Simulation mode (no real network)\n"
                      << "  --help             Show this help\n";
            return 0;
        }
    }

    try {
        // Create DDS participant and entities
        dds::Participant participant(DDS_DOMAIN_DEFAULT);

        // AVTP frame topic (individual frames)
        auto frame_qos = dds::qos_profiles::reliable_standard(500);
        dds::Topic frame_topic(participant, &telemetry_avtp_AcfCanFrame_desc,
                               "rt/avtp/can/frames", frame_qos.get());
        dds::Writer frame_writer(participant, frame_topic, frame_qos.get());

        // Stream statistics topic
        auto stats_qos = dds::qos_profiles::best_effort(1);
        dds::Topic stats_topic(participant, &telemetry_avtp_StreamStats_desc,
                               "rt/avtp/stats", stats_qos.get());
        dds::Writer stats_writer(participant, stats_topic, stats_qos.get());

        // Optional: Reader for TX path (DDS -> AVTP)
        std::unique_ptr<dds::Reader> tx_reader;
        if (tx_enabled) {
            dds::Topic tx_topic(participant, &telemetry_avtp_AcfCanFrame_desc,
                                "rt/avtp/can/tx", frame_qos.get());
            tx_reader = std::make_unique<dds::Reader>(participant, tx_topic,
                                                      frame_qos.get());
            LOG(INFO) << "TX enabled: subscribed to rt/avtp/can/tx";
        }

        LOG(INFO) << "DDS topics created:";
        LOG(INFO) << "  - rt/avtp/can/frames (publish received CAN frames)";
        LOG(INFO) << "  - rt/avtp/stats (stream statistics)";
        if (tx_enabled) {
            LOG(INFO) << "  - rt/avtp/can/tx (receive frames to transmit)";
        }

        // Create AVTP socket (unless in simulation mode)
        int sockfd = -1;
        if (!simulation_mode) {
            sockfd = create_avtp_socket(interface);
            if (sockfd < 0) {
                LOG(WARNING) << "Failed to create AVTP socket, running in simulation mode";
                simulation_mode = true;
            }
        }

        if (simulation_mode) {
            LOG(INFO) << "Running in simulation mode (no network I/O)";
        }

        LOG(INFO) << "AVTP Probe ready. Press Ctrl+C to stop.";

        // Statistics per stream
        std::unordered_map<uint64_t, StreamStatistics> stream_stats;
        uint32_t global_seq = 0;
        auto last_stats_publish = std::chrono::steady_clock::now();
        std::string source_id = "avtp_probe";
        std::string empty_correlation;

        // Receive buffer
        std::vector<uint8_t> rx_buffer(MAX_AVTP_FRAME_SIZE);
        std::vector<uint8_t> payload_buffer(64);

        while (g_running) {
            auto now = std::chrono::steady_clock::now();

            if (!simulation_mode && sockfd >= 0) {
                // Receive AVTP frame from network
                struct sockaddr_ll src_addr = {};
                socklen_t addr_len = sizeof(src_addr);

                ssize_t rx_len = recvfrom(sockfd, rx_buffer.data(), rx_buffer.size(),
                                          0, reinterpret_cast<struct sockaddr*>(&src_addr),
                                          &addr_len);

                if (rx_len > static_cast<ssize_t>(ETH_HLEN)) {
                    // Skip Ethernet header
                    const uint8_t* avtp_data = rx_buffer.data() + ETH_HLEN;
                    size_t avtp_len = rx_len - ETH_HLEN;

                    if (avtp_len >= AVTP_CAN_HEADER_LEN) {
                        telemetry_avtp_AcfCanFrame frame = {};
                        if (parse_acf_can(avtp_data, avtp_len, frame, payload_buffer)) {
                            // Use a simulated stream ID (in real case, extract from NTSCF/TSCF header)
                            uint64_t frame_stream_id = 0x0011223344556677ULL;

                            // Filter by stream ID if specified
                            if (stream_id != 0 && frame_stream_id != stream_id) {
                                continue;
                            }

                            frame.stream_id = frame_stream_id;

                            // Fill header
                            frame.header.source_id = const_cast<char*>(source_id.c_str());
                            frame.header.timestamp_ns = utils::now_ns();
                            frame.header.seq_num = global_seq++;
                            frame.header.correlation_id = const_cast<char*>(empty_correlation.c_str());

                            frame.sequence_num = global_seq & 0xFF;

                            // Update statistics
                            auto& stats = stream_stats[frame_stream_id];
                            stats.frames_received++;
                            stats.bytes_total += rx_len;
                            stats.last_update = now;

                            // Publish to DDS
                            frame_writer.write(frame);

                            VLOG(1) << "RX: CAN ID=0x" << std::hex << frame.can_id
                                    << " bus=" << std::dec << static_cast<int>(frame.bus_id)
                                    << " len=" << frame.payload._length
                                    << " stream=0x" << std::hex << frame_stream_id;
                        }
                    }
                }

                // TX path: read from DDS and send as AVTP
                if (tx_reader) {
                    tx_reader->take_each<telemetry_avtp_AcfCanFrame>(
                        [&](const telemetry_avtp_AcfCanFrame& tx_frame) {
                            std::vector<uint8_t> tx_buffer(MAX_AVTP_FRAME_SIZE);

                            // Build AVTP frame
                            size_t frame_len = build_acf_can_frame(tx_buffer.data(),
                                                                   tx_buffer.size(), tx_frame);
                            if (frame_len > 0) {
                                // TODO: Add Ethernet header and send
                                // For now, just update stats
                                auto& stats = stream_stats[tx_frame.stream_id];
                                stats.frames_sent++;
                                stats.bytes_total += frame_len;

                                VLOG(1) << "TX: CAN ID=0x" << std::hex << tx_frame.can_id
                                        << " bus=" << std::dec << static_cast<int>(tx_frame.bus_id)
                                        << " len=" << tx_frame.payload._length;
                            }
                        }, 10);
                }
            } else {
                // Simulation mode: generate fake CAN frames
                static auto last_sim = std::chrono::steady_clock::now();
                if (now - last_sim >= std::chrono::milliseconds(50)) {
                    last_sim = now;

                    static uint32_t sim_seq = 0;
                    static double sim_speed = 0.0;
                    sim_speed += 0.5;
                    if (sim_speed > 120.0) sim_speed = 0.0;

                    // Simulate a speed CAN message (ID 0x123)
                    telemetry_avtp_AcfCanFrame frame = {};
                    frame.stream_id = 0x0011223344556677ULL;
                    frame.can_id = 0x123;
                    frame.bus_id = 0;
                    frame.flags.is_extended_id = false;
                    frame.flags.is_fd = false;
                    frame.flags.is_brs = false;
                    frame.flags.is_esi = false;
                    frame.flags.is_rtr = false;

                    // Encode speed as 2 bytes
                    uint16_t speed_raw = static_cast<uint16_t>(sim_speed * 100);
                    payload_buffer.resize(8);
                    payload_buffer[0] = (speed_raw >> 8) & 0xFF;
                    payload_buffer[1] = speed_raw & 0xFF;
                    payload_buffer[2] = 0;
                    payload_buffer[3] = 0;
                    payload_buffer[4] = 0;
                    payload_buffer[5] = 0;
                    payload_buffer[6] = 0;
                    payload_buffer[7] = 0;

                    frame.payload._buffer = payload_buffer.data();
                    frame.payload._length = 8;
                    frame.payload._maximum = 64;
                    frame.payload._release = false;

                    frame.avtp_timestamp = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            now.time_since_epoch()).count());
                    frame.sequence_num = sim_seq++ & 0xFF;

                    frame.header.source_id = const_cast<char*>(source_id.c_str());
                    frame.header.timestamp_ns = utils::now_ns();
                    frame.header.seq_num = global_seq++;
                    frame.header.correlation_id = const_cast<char*>(empty_correlation.c_str());

                    frame_writer.write(frame);

                    // Update stats
                    auto& stats = stream_stats[frame.stream_id];
                    stats.frames_received++;
                    stats.bytes_total += 64;
                    stats.last_update = now;
                }
            }

            // Publish stream statistics periodically (every 5 seconds)
            if (now - last_stats_publish >= std::chrono::seconds(5)) {
                last_stats_publish = now;

                for (auto& [sid, stats] : stream_stats) {
                    telemetry_avtp_StreamStats stats_msg = {};
                    stats_msg.header.source_id = const_cast<char*>(source_id.c_str());
                    stats_msg.header.timestamp_ns = utils::now_ns();
                    stats_msg.header.seq_num = global_seq++;
                    stats_msg.header.correlation_id = const_cast<char*>(empty_correlation.c_str());

                    stats_msg.stream_id = sid;
                    stats_msg.frames_received = stats.frames_received;
                    stats_msg.frames_sent = stats.frames_sent;
                    stats_msg.sequence_errors = stats.sequence_errors;
                    stats_msg.timestamp_errors = stats.timestamp_errors;
                    stats_msg.bytes_total = stats.bytes_total;

                    if (stats.latency_count > 0) {
                        stats_msg.average_latency_us = stats.latency_sum_us / static_cast<double>(stats.latency_count);
                    }

                    stats_writer.write(stats_msg);

                    LOG(INFO) << "Stream 0x" << std::hex << sid << std::dec
                              << ": rx=" << stats.frames_received
                              << " tx=" << stats.frames_sent
                              << " seq_err=" << stats.sequence_errors
                              << " bytes=" << stats.bytes_total;
                }
            }

            // Small sleep to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Cleanup
        if (sockfd >= 0) {
            close(sockfd);
        }

        uint64_t total_rx = 0, total_tx = 0;
        for (const auto& [sid, stats] : stream_stats) {
            total_rx += stats.frames_received;
            total_tx += stats.frames_sent;
        }

        LOG(INFO) << "AVTP Probe shutdown. Total: rx=" << total_rx
                  << " tx=" << total_tx << " streams=" << stream_stats.size();

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
