#ifndef __lwm2m_telemetry_pack_hpp__
#define __lwm2m_telemetry_pack_hpp__

#include <string>
#include <utility>
#include <vector>

#include "lwm2m_codec_senml.hpp"

/**
 * @file lwm2m_telemetry_pack.hpp
 * @brief Build / parse a SenML pack of timestamped telemetry samples.
 *
 * The layer between buffered vehicle samples and the SenML codec for LwM2M
 * Send (CoAP POST /dp): a batch of readings, each captured at a real time,
 * encoded as one SenML pack so every reading keeps its own timestamp across
 * the batch (vs the cloud-poll snapshot stream, which only timestamps on
 * arrival). Uses the bt/t fields added to the codec.
 *
 * Pure + standalone (no ACE / CoAP): the client Send path calls build_pack()
 * then senml::encode_cbor(); the server /dp path calls senml::decode_* then
 * parse_pack(). Both directions are unit-tested round-trip.
 */

namespace lwm2m { namespace telemetry {

/// One timestamped reading set captured at `timeUnix` (absolute unix seconds).
/// `values` are (resource-name, numeric value) pairs under the pack's base
/// path — e.g. {"10", 62.0} for /33000/0/10 (speed). Telemetry is numeric;
/// non-numeric resources are out of scope for a batch.
struct Sample {
    double timeUnix{0.0};
    std::vector<std::pair<std::string, double>> values;
};

/// Build a SenML record list for `basePath` (e.g. "/33000/0/") from
/// oldest-first `samples`. The first record carries bn=basePath + bt=the first
/// sample's time; every record's t is its sample's offset from that base time,
/// so each reading round-trips to its real capture time. Empty in → empty out.
std::vector<senml::Record> build_pack(const std::string& basePath,
                                      const std::vector<Sample>& samples);

/// Inverse of build_pack: regroup a decoded SenML record list into
/// per-timestamp samples (consecutive records sharing an effective time
/// coalesce into one Sample, as build_pack emits them). Non-numeric records
/// are skipped. Returns oldest-first.
std::vector<Sample> parse_pack(const std::vector<senml::Record>& records);

}} // namespace lwm2m::telemetry

#endif /*__lwm2m_telemetry_pack_hpp__*/
