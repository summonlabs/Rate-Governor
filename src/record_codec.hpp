#ifndef RATE_GOVERNOR_RECORD_CODEC_HPP
#define RATE_GOVERNOR_RECORD_CODEC_HPP

// Internal (non-installed) record codec: the exact durable encoding of every
// authoritative object. Kept separate from the public headers so that the
// on-disk layout can evolve with the journal format version without changing
// the library's API surface.

#include <cstddef>

#include "rate_governor/codec.hpp"
#include "rate_governor/entities.hpp"
#include "rate_governor/envelope.hpp"

namespace rate_governor {

inline constexpr usize kRecordTextHardLimit = 4096;

void encode_provenance(ByteWriter& writer, const Provenance& provenance);
bool decode_provenance(ByteReader& reader, Provenance& out);

void encode_flow(ByteWriter& writer, const Flow& flow);
bool decode_flow(ByteReader& reader, Flow& out);
void encode_grant(ByteWriter& writer, const Grant& grant);
bool decode_grant(ByteReader& reader, Grant& out);
void encode_reservation(ByteWriter& writer, const Reservation& reservation);
bool decode_reservation(ByteReader& reader, Reservation& out);
void encode_policy(ByteWriter& writer, const Policy& policy);
bool decode_policy(ByteReader& reader, Policy& out);
void encode_resource(ByteWriter& writer, const Resource& resource);
bool decode_resource(ByteReader& reader, Resource& out);
void encode_backend(ByteWriter& writer, const BackendDescriptor& backend);
bool decode_backend(ByteReader& reader, BackendDescriptor& out);

void encode_fingerprint(ByteWriter& writer, const PlanFingerprint& fingerprint);
bool decode_fingerprint(ByteReader& reader, PlanFingerprint& out);
void encode_plan(ByteWriter& writer, const EffectivePlan& plan);
bool decode_plan(ByteReader& reader, EffectivePlan& out);
void encode_bucket(ByteWriter& writer, const TokenBucket& bucket);
bool decode_bucket(ByteReader& reader, TokenBucket& out);

void encode_envelope(ByteWriter& writer, const RateEnvelope& envelope);
bool decode_envelope(ByteReader& reader, RateEnvelope& out);
void encode_attempt(ByteWriter& writer, const EnforcementAttempt& attempt);
bool decode_attempt(ByteReader& reader, EnforcementAttempt& out);

// Small payloads used by dedicated record types.
void encode_epoch_payload(ByteWriter& writer, FabricEpoch epoch, WorkerBootId boot,
                          ReasonCode reason);
bool decode_epoch_payload(ByteReader& reader, FabricEpoch& epoch, WorkerBootId& boot,
                          ReasonCode& reason);

void encode_bucket_payload(ByteWriter& writer, const RateEnvelope& envelope);
bool decode_bucket_payload(ByteReader& reader, RateEnvelope& out);

void encode_grant_state_payload(ByteWriter& writer, GrantId id, Generation generation,
                                GrantState state, ReasonCode reason);
bool decode_grant_state_payload(ByteReader& reader, GrantId& id, Generation& generation,
                                GrantState& state, ReasonCode& reason);
void encode_reservation_state_payload(ByteWriter& writer, ReservationId id, Generation generation,
                                      ReservationState state, ReasonCode reason);
bool decode_reservation_state_payload(ByteReader& reader, ReservationId& id, Generation& generation,
                                      ReservationState& state, ReasonCode& reason);
void encode_policy_state_payload(ByteWriter& writer, PolicyId id, Generation generation,
                                 PolicyState state, ReasonCode reason);
bool decode_policy_state_payload(ByteReader& reader, PolicyId& id, Generation& generation,
                                 PolicyState& state, ReasonCode& reason);
void encode_resource_state_payload(ByteWriter& writer, ResourceId id, Generation generation,
                                   ResourceState state, ReasonCode reason);
bool decode_resource_state_payload(ByteReader& reader, ResourceId& id, Generation& generation,
                                   ResourceState& state, ReasonCode& reason);

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_RECORD_CODEC_HPP
