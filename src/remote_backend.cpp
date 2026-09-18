#include "rate_governor/remote_backend.hpp"

#include <string>
#include <utility>

#include "rate_governor/version.hpp"

namespace rate_governor {

RemoteBackend::~RemoteBackend() {
  // Destruction only drops the session. Stopping the worker is an explicit
  // operator action (shutdown()); a coordinator that simply exits must never
  // take the enforcement process down with it.
  std::lock_guard<std::mutex> guard(mutex_);
  live_ = false;
  socket_.close();
}

std::unique_ptr<RemoteBackend> RemoteBackend::connect(const Endpoint& endpoint, FabricEpoch epoch,
                                                      WorkerBootId boot, std::string& error) {
  ensure_socket_runtime();
  auto backend = std::unique_ptr<RemoteBackend>(new RemoteBackend());
  if (!Socket::connect_to(endpoint, backend->socket_, error)) {
    return nullptr;
  }
  backend->endpoint_ = endpoint;
  backend->coordinator_boot_ = boot;

  HelloMessage hello;
  hello.epoch = epoch;
  hello.boot = boot;
  hello.label = "coordinator";
  hello.build = std::string(kProductName) + " " + std::string(kVersionString);
  Frame request;
  request.type = MessageType::Hello;
  request.payload = encode_message(hello);
  if (request.payload.empty()) {
    error = "hello could not be encoded";
    return nullptr;
  }
  if (!backend->socket_.send_frame(request, error)) {
    return nullptr;
  }
  Frame response;
  ReasonCode reason = ReasonCode::None;
  if (!backend->socket_.recv_frame(response, error, reason)) {
    return nullptr;
  }
  if (response.type == MessageType::ErrorResponse) {
    ErrorMessage message;
    if (decode_message(response.payload, message)) {
      error = std::string("worker refused the session: ") +
              std::string(to_string_view(message.reason)) + " (" + message.detail + ")";
    } else {
      error = "worker refused the session with an undecodable error";
    }
    return nullptr;
  }
  if (response.type != MessageType::HelloAck) {
    error = "worker did not acknowledge the handshake";
    return nullptr;
  }
  HelloMessage ack;
  if (!decode_message(response.payload, ack)) {
    error = "handshake acknowledgement could not be decoded";
    return nullptr;
  }
  if (!ack.boot.valid()) {
    error = "handshake acknowledgement carried no worker boot id";
    return nullptr;
  }
  if (ack.epoch != epoch) {
    error = "handshake acknowledged a different fabric epoch";
    return nullptr;
  }
  backend->hello_ = ack;
  backend->worker_boot_ = ack.boot;
  backend->admitted_epoch_ = ack.epoch;
  backend->descriptor_.id = ack.backend;
  backend->descriptor_.generation = ack.backend_generation;
  backend->descriptor_.kind = ack.kind;
  backend->descriptor_.capabilities = ack.capabilities;
  backend->descriptor_.verification = ack.verification;
  backend->descriptor_.max_rate_ups = ack.max_rate_ups;
  backend->descriptor_.max_burst_tokens = ack.max_burst_tokens;
  backend->descriptor_.label = ack.label;
  backend->descriptor_.provenance.source = AuthoritySource::BackendReport;
  backend->descriptor_.provenance.epoch = ack.epoch;
  backend->descriptor_.provenance.boot = ack.boot;
  backend->descriptor_.provenance.detail = "reported by the worker during the handshake";
  backend->live_ = true;
  return backend;
}

BackendDescriptor RemoteBackend::describe() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return descriptor_;
}

void RemoteBackend::note_failure(std::string error, ReasonCode reason) {
  live_ = false;
  last_error_ = std::move(error);
  if (reason != ReasonCode::None) {
    last_error_.append(" [");
    last_error_.append(to_string_view(reason));
    last_error_.append("]");
  }
  socket_.close();
}

void RemoteBackend::mark_dead(std::string reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  note_failure(std::move(reason), ReasonCode::BackendSessionLost);
}

bool RemoteBackend::transact(const Frame& request, MessageType expected, Frame& response,
                             std::string& error) {
  if (!live_) {
    error = "backend session is not live";
    return false;
  }
  if (!socket_.send_frame(request, error)) {
    note_failure(error, ReasonCode::BackendSessionLost);
    return false;
  }
  ReasonCode reason = ReasonCode::None;
  Frame received;
  if (!socket_.recv_frame(received, error, reason)) {
    note_failure(error, reason == ReasonCode::None ? ReasonCode::BackendSessionLost : reason);
    return false;
  }
  ++round_trips_;
  if (received.type == MessageType::ErrorResponse) {
    ErrorMessage message;
    if (decode_message(received.payload, message)) {
      error = std::string("worker error: ") + std::string(to_string_view(message.reason)) + " (" +
              message.detail + ")";
    } else {
      error = "worker returned an undecodable error";
    }
    return false;
  }
  if (received.type != expected) {
    error = "unexpected response frame type";
    note_failure(error, ReasonCode::BackendRejectedMalformedRequest);
    return false;
  }
  response = std::move(received);
  return true;
}

DispatchOutcome RemoteBackend::dispatch_apply(const ApplyRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  DispatchOutcome outcome;
  outcome.epoch = admitted_epoch_;
  outcome.boot = worker_boot_;
  if (!live_) {
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::BackendSessionLost;
    outcome.detail = "backend session is not live; the effect is UNKNOWN";
    return outcome;
  }
  ApplyRequestMessage message;
  message.attempt = request.attempt;
  message.envelope = request.envelope;
  message.envelope_generation = request.envelope_generation;
  message.rate_ups = request.rate_ups;
  message.burst_tokens = request.burst_tokens;
  message.refill = request.refill;
  message.epoch = request.epoch;
  // Requests carry the incarnation that is asking, not the one that answers:
  // the device fences on the coordinator's boot id.
  message.boot = coordinator_boot_;
  message.deadline_ns = 0;
  Frame frame;
  frame.type = MessageType::ApplyRequest;
  frame.payload = encode_message(message);
  if (frame.payload.empty()) {
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::MalformedInput;
    outcome.detail = "apply request could not be encoded";
    return outcome;
  }
  Frame response;
  std::string error;
  if (!transact(frame, MessageType::ApplyResponse, response, error)) {
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::BackendSessionLost;
    outcome.detail = error;
    return outcome;
  }
  ApplyResponseMessage decoded;
  if (!decode_message(response.payload, decoded)) {
    note_failure("apply response could not be decoded", ReasonCode::BackendRejectedMalformedRequest);
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::BackendRejectedMalformedRequest;
    outcome.detail = "apply response could not be decoded";
    return outcome;
  }
  if (decoded.attempt != request.attempt) {
    note_failure("apply response referenced a different attempt",
                 ReasonCode::BackendRejectedMalformedRequest);
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::BackendRejectedMalformedRequest;
    outcome.detail = "apply response referenced a different attempt";
    return outcome;
  }
  if (decoded.boot != worker_boot_) {
    // The answer did not come from the incarnation this session was admitted
    // to. It is not evidence about anything.
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::AttemptFencedByBoot;
    outcome.detail = "apply response carried a foreign boot id";
    return outcome;
  }
  if (decoded.epoch != admitted_epoch_) {
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::AttemptFencedByEpoch;
    outcome.detail = "apply response carried a different fabric epoch";
    return outcome;
  }
  outcome.status = decoded.accepted ? DispatchStatus::Completed : DispatchStatus::Rejected;
  outcome.reason = decoded.reason;
  outcome.applied_rate_ups = decoded.applied_rate_ups;
  outcome.applied_burst_tokens = decoded.applied_burst_tokens;
  outcome.epoch = decoded.epoch;
  outcome.boot = decoded.boot;
  outcome.detail = decoded.detail;
  return outcome;
}

DispatchOutcome RemoteBackend::dispatch_revoke(const RevokeRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  DispatchOutcome outcome;
  outcome.epoch = admitted_epoch_;
  outcome.boot = worker_boot_;
  if (!live_) {
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::BackendSessionLost;
    outcome.detail = "backend session is not live; the effect is UNKNOWN";
    return outcome;
  }
  RevokeRequestMessage message;
  message.attempt = request.attempt;
  message.envelope = request.envelope;
  message.envelope_generation = request.envelope_generation;
  message.epoch = request.epoch;
  message.boot = coordinator_boot_;
  message.reason = request.reason;
  Frame frame;
  frame.type = MessageType::RevokeRequest;
  frame.payload = encode_message(message);
  if (frame.payload.empty()) {
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::MalformedInput;
    outcome.detail = "revoke request could not be encoded";
    return outcome;
  }
  Frame response;
  std::string error;
  if (!transact(frame, MessageType::RevokeResponse, response, error)) {
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::BackendSessionLost;
    outcome.detail = error;
    return outcome;
  }
  RevokeResponseMessage decoded;
  if (!decode_message(response.payload, decoded)) {
    note_failure("revoke response could not be decoded",
                 ReasonCode::BackendRejectedMalformedRequest);
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::BackendRejectedMalformedRequest;
    outcome.detail = "revoke response could not be decoded";
    return outcome;
  }
  if (decoded.attempt != request.attempt || decoded.boot != worker_boot_ ||
      decoded.epoch != admitted_epoch_) {
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::AttemptFencedByBoot;
    outcome.detail = "revoke response did not match the dispatched attempt";
    return outcome;
  }
  outcome.status = decoded.removed ? DispatchStatus::Completed : DispatchStatus::Rejected;
  outcome.reason = decoded.reason;
  outcome.epoch = decoded.epoch;
  outcome.boot = decoded.boot;
  outcome.detail = decoded.detail;
  return outcome;
}

VerificationOutcome RemoteBackend::verify(const ApplyRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  VerificationOutcome outcome;
  outcome.epoch = admitted_epoch_;
  outcome.boot = worker_boot_;
  if (!live_) {
    outcome.status = VerificationStatus::Failed;
    outcome.reason = ReasonCode::ApplyVerificationUnknown;
    outcome.detail = "backend session is not live; the effect is UNKNOWN";
    return outcome;
  }
  VerifyRequestMessage message;
  message.attempt = request.attempt;
  message.envelope = request.envelope;
  message.envelope_generation = request.envelope_generation;
  message.epoch = request.epoch;
  message.boot = coordinator_boot_;
  Frame frame;
  frame.type = MessageType::VerifyRequest;
  frame.payload = encode_message(message);
  if (frame.payload.empty()) {
    outcome.status = VerificationStatus::Failed;
    outcome.reason = ReasonCode::MalformedInput;
    outcome.detail = "verify request could not be encoded";
    return outcome;
  }
  Frame response;
  std::string error;
  if (!transact(frame, MessageType::VerifyResponse, response, error)) {
    outcome.status = VerificationStatus::Failed;
    outcome.reason = ReasonCode::ApplyVerificationUnknown;
    outcome.detail = error;
    return outcome;
  }
  VerifyResponseMessage decoded;
  if (!decode_message(response.payload, decoded)) {
    note_failure("verify response could not be decoded",
                 ReasonCode::BackendRejectedMalformedRequest);
    outcome.status = VerificationStatus::Failed;
    outcome.reason = ReasonCode::ApplyVerificationUnknown;
    outcome.detail = "verify response could not be decoded";
    return outcome;
  }
  if (decoded.attempt != request.attempt) {
    outcome.status = VerificationStatus::Failed;
    outcome.reason = ReasonCode::ApplyVerificationUnknown;
    outcome.detail = "verify response referenced a different attempt";
    return outcome;
  }
  if (decoded.boot != worker_boot_ || decoded.epoch != admitted_epoch_) {
    outcome.status = VerificationStatus::Failed;
    outcome.reason = ReasonCode::AttemptFencedByBoot;
    outcome.detail = "verify response came from a fenced incarnation";
    return outcome;
  }
  // Both "present" and "absent" are confirmed readbacks. The distinction is
  // carried by observed_present, and the engine decides what it means.
  outcome.status = VerificationStatus::Confirmed;
  outcome.reason = decoded.reason;
  outcome.observed_present = decoded.present;
  outcome.observed_rate_ups = decoded.observed_rate_ups;
  outcome.observed_burst_tokens = decoded.observed_burst_tokens;
  outcome.detail = decoded.detail;
  return outcome;
}

bool RemoteBackend::fetch_accounting(AccountingResponseMessage& out, std::string& error) {
  std::lock_guard<std::mutex> guard(mutex_);
  Frame frame;
  frame.type = MessageType::AccountingRequest;
  Frame response;
  if (!transact(frame, MessageType::AccountingResponse, response, error)) {
    return false;
  }
  return decode_message(response.payload, out);
}

void RemoteBackend::shutdown() noexcept {
  std::lock_guard<std::mutex> guard(mutex_);
  if (live_) {
    Frame frame;
    frame.type = MessageType::ShutdownRequest;
    Frame response;
    std::string error;
    if (transact(frame, MessageType::ShutdownResponse, response, error)) {
      AccountingResponseMessage final_accounting;
      (void)decode_message(response.payload, final_accounting);
    }
  }
  live_ = false;
  socket_.close();
}

std::string RemoteBackend::session_description() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::string out = "remote worker ";
  out.append(endpoint_.to_string());
  out.append(" boot=");
  out.append(worker_boot_.to_string());
  out.append(" epoch=");
  out.append(admitted_epoch_.to_string());
  out.append(" kind=");
  out.append(to_string_view(descriptor_.kind));
  if (!hello_.build.empty()) {
    out.append(" build=");
    out.append(hello_.build);
  }
  return out;
}

u64 RemoteBackend::round_trips() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return round_trips_;
}

u64 RemoteBackend::bytes_sent() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return socket_.bytes_sent();
}

u64 RemoteBackend::bytes_received() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return socket_.bytes_received();
}

}  // namespace rate_governor
