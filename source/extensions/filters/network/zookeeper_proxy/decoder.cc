#include "source/extensions/filters/network/zookeeper_proxy/decoder.h"

#include <string>

#include "source/common/common/enum_to_int.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ZooKeeperProxy {

constexpr uint32_t BOOL_LENGTH = 1;
constexpr uint32_t INT_LENGTH = 4;
constexpr uint32_t LONG_LENGTH = 8;
constexpr uint32_t XID_LENGTH = 4;
constexpr uint32_t OPCODE_LENGTH = 4;
constexpr uint32_t ZXID_LENGTH = 8;
constexpr uint32_t TIMEOUT_LENGTH = 4;
constexpr uint32_t SESSION_LENGTH = 8;
constexpr uint32_t MULTI_HEADER_LENGTH = 9;
constexpr uint32_t PROTOCOL_VERSION_LENGTH = 4;
constexpr uint32_t SERVER_HEADER_LENGTH = 16;

const char* createFlagsToString(CreateFlags flags) {
  switch (flags) {
  case CreateFlags::Persistent:
    return "persistent";
  case CreateFlags::PersistentSequential:
    return "persistent_sequential";
  case CreateFlags::Ephemeral:
    return "ephemeral";
  case CreateFlags::EphemeralSequential:
    return "ephemeral_sequential";
  case CreateFlags::Container:
    return "container";
  case CreateFlags::PersistentWithTtl:
    return "persistent_with_ttl";
  case CreateFlags::PersistentSequentialWithTtl:
    return "persistent_sequential_with_ttl";
  }

  return "unknown";
}

void DecoderImpl::decodeOnData(Buffer::Instance& data, uint64_t& offset) {
  ENVOY_LOG(trace, "zookeeper_proxy: decoding request with {} bytes at offset {}", data.length(),
            offset);

  // Check message length.
  const int32_t len = helper_.peekInt32(data, offset);
  ENVOY_LOG(trace, "zookeeper_proxy: decoding request with len {} at offset {}", len, offset);
  ensureMinLength(len, XID_LENGTH + INT_LENGTH); // xid + opcode
  ensureMaxLength(len);

  auto start_time = time_source_.monotonicTime();

  // Control requests, with XIDs <= 0.
  //
  // These are meant to control the state of a session:
  // connect, keep-alive, authenticate and set initial watches.
  //
  // Note: setWatches is a command historically used to set watches
  //       right after connecting, typically used when roaming from one
  //       ZooKeeper server to the next. Thus, the special xid.
  //       However, some client implementations might expose setWatches
  //       as a regular data request, so we support that as well.
  const int32_t xid = helper_.peekInt32(data, offset);
  ENVOY_LOG(trace, "zookeeper_proxy: decoding request with xid {} at offset {}", xid, offset);
  switch (static_cast<XidCodes>(xid)) {
  case XidCodes::ConnectXid:
    parseConnect(data, offset, len);
    requests_by_xid_[xid] = {OpCodes::Connect, std::move(start_time)};
    return;
  case XidCodes::PingXid:
    offset += OPCODE_LENGTH;
    callbacks_.onPing();
    requests_by_xid_[xid] = {OpCodes::Ping, std::move(start_time)};
    return;
  case XidCodes::AuthXid:
    parseAuthRequest(data, offset, len);
    requests_by_xid_[xid] = {OpCodes::SetAuth, std::move(start_time)};
    return;
  case XidCodes::SetWatchesXid:
    offset += OPCODE_LENGTH;
    parseSetWatchesRequest(data, offset, len);
    requests_by_xid_[xid] = {OpCodes::SetWatches, std::move(start_time)};
    return;
  default:
    // WATCH_XID is generated by the server, so that and everything
    // else can be ignored here.
    break;
  }

  // Data requests, with XIDs > 0.
  //
  // These are meant to happen after a successful control request, except
  // for two cases: auth requests can happen at any time and ping requests
  // must happen every 1/3 of the negotiated session timeout, to keep
  // the session alive.
  const int32_t oc = helper_.peekInt32(data, offset);
  ENVOY_LOG(trace, "zookeeper_proxy: decoding request with opcode {} at offset {}", oc, offset);
  const auto opcode = static_cast<OpCodes>(oc);
  switch (opcode) {
  case OpCodes::GetData:
    parseGetDataRequest(data, offset, len);
    break;
  case OpCodes::Create:
  case OpCodes::Create2:
  case OpCodes::CreateContainer:
  case OpCodes::CreateTtl:
    parseCreateRequest(data, offset, len, static_cast<OpCodes>(opcode));
    break;
  case OpCodes::SetData:
    parseSetRequest(data, offset, len);
    break;
  case OpCodes::GetChildren:
    parseGetChildrenRequest(data, offset, len, false);
    break;
  case OpCodes::GetChildren2:
    parseGetChildrenRequest(data, offset, len, true);
    break;
  case OpCodes::Delete:
    parseDeleteRequest(data, offset, len);
    break;
  case OpCodes::Exists:
    parseExistsRequest(data, offset, len);
    break;
  case OpCodes::GetAcl:
    parseGetAclRequest(data, offset, len);
    break;
  case OpCodes::SetAcl:
    parseSetAclRequest(data, offset, len);
    break;
  case OpCodes::Sync:
    callbacks_.onSyncRequest(pathOnlyRequest(data, offset, len));
    break;
  case OpCodes::Check:
    parseCheckRequest(data, offset, len);
    break;
  case OpCodes::Multi:
    parseMultiRequest(data, offset, len);
    break;
  case OpCodes::Reconfig:
    parseReconfigRequest(data, offset, len);
    break;
  case OpCodes::SetWatches:
    parseSetWatchesRequest(data, offset, len);
    break;
  case OpCodes::CheckWatches:
    parseXWatchesRequest(data, offset, len, OpCodes::CheckWatches);
    break;
  case OpCodes::RemoveWatches:
    parseXWatchesRequest(data, offset, len, OpCodes::RemoveWatches);
    break;
  case OpCodes::GetEphemerals:
    callbacks_.onGetEphemeralsRequest(pathOnlyRequest(data, offset, len));
    break;
  case OpCodes::GetAllChildrenNumber:
    callbacks_.onGetAllChildrenNumberRequest(pathOnlyRequest(data, offset, len));
    break;
  case OpCodes::Close:
    callbacks_.onCloseRequest();
    break;
  default:
    throw EnvoyException(fmt::format("Unknown opcode: {}", enumToSignedInt(opcode)));
  }

  requests_by_xid_[xid] = {opcode, std::move(start_time)};
}

void DecoderImpl::decodeOnWrite(Buffer::Instance& data, uint64_t& offset) {
  ENVOY_LOG(trace, "zookeeper_proxy: decoding response with {} bytes at offset {}", data.length(),
            offset);

  // Check message length.
  const int32_t len = helper_.peekInt32(data, offset);
  ENVOY_LOG(trace, "zookeeper_proxy: decoding response with len {} at offset {}", len, offset);
  ensureMinLength(len, XID_LENGTH + ZXID_LENGTH + INT_LENGTH); // xid + zxid + err
  ensureMaxLength(len);

  const auto xid = helper_.peekInt32(data, offset);
  ENVOY_LOG(trace, "zookeeper_proxy: decoding response with xid {} at offset {}", xid, offset);
  const auto xid_code = static_cast<XidCodes>(xid);

  std::chrono::milliseconds latency;
  OpCodes opcode;

  if (xid_code != XidCodes::WatchXid) {
    // Find the corresponding request for this XID.
    const auto it = requests_by_xid_.find(xid);

    // If this fails, it's either a server-side bug or a malformed packet.
    if (it == requests_by_xid_.end()) {
      throw EnvoyException("xid not found");
    }

    latency = std::chrono::duration_cast<std::chrono::milliseconds>(time_source_.monotonicTime() -
                                                                    it->second.start_time);
    opcode = it->second.opcode;
    requests_by_xid_.erase(it);
  }

  // Connect responses are special, they have no full reply header
  // but just an XID with no zxid nor error fields like the ones
  // available for all other server generated messages.
  if (xid_code == XidCodes::ConnectXid) {
    parseConnectResponse(data, offset, len, latency);
    return;
  }

  // Control responses that aren't connect, with XIDs <= 0.
  const auto zxid = helper_.peekInt64(data, offset);
  const auto error = helper_.peekInt32(data, offset);
  ENVOY_LOG(trace, "zookeeper_proxy: decoding response with zxid {} and error {} at offset {}",
            zxid, error, offset);
  switch (xid_code) {
  case XidCodes::PingXid:
    callbacks_.onResponse(OpCodes::Ping, xid, zxid, error, latency);
    return;
  case XidCodes::AuthXid:
    callbacks_.onResponse(OpCodes::SetAuth, xid, zxid, error, latency);
    return;
  case XidCodes::SetWatchesXid:
    callbacks_.onResponse(OpCodes::SetWatches, xid, zxid, error, latency);
    return;
  case XidCodes::WatchXid:
    parseWatchEvent(data, offset, len, zxid, error);
    return;
  default:
    break;
  }

  callbacks_.onResponse(opcode, xid, zxid, error, latency);
  offset += (len - (XID_LENGTH + ZXID_LENGTH + INT_LENGTH));
}

void DecoderImpl::ensureMinLength(const int32_t len, const int32_t minlen) const {
  if (len < minlen) {
    throw EnvoyException("Packet is too small");
  }
}

void DecoderImpl::ensureMaxLength(const int32_t len) const {
  if (static_cast<uint32_t>(len) > max_packet_bytes_) {
    throw EnvoyException("Packet is too big");
  }
}

void DecoderImpl::parseConnect(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, XID_LENGTH + ZXID_LENGTH + TIMEOUT_LENGTH + SESSION_LENGTH + INT_LENGTH);

  // Skip zxid, timeout, and session id.
  offset += ZXID_LENGTH + TIMEOUT_LENGTH + SESSION_LENGTH;

  // Skip password.
  skipString(data, offset);

  const bool readonly = maybeReadBool(data, offset);

  callbacks_.onConnect(readonly);
}

void DecoderImpl::parseAuthRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + INT_LENGTH + INT_LENGTH + INT_LENGTH);

  // Skip opcode + type.
  offset += OPCODE_LENGTH + INT_LENGTH;
  const std::string scheme = helper_.peekString(data, offset);
  // Skip credential.
  skipString(data, offset);

  callbacks_.onAuthRequest(scheme);
}

void DecoderImpl::parseGetDataRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + INT_LENGTH + BOOL_LENGTH);

  const std::string path = helper_.peekString(data, offset);
  const bool watch = helper_.peekBool(data, offset);

  callbacks_.onGetDataRequest(path, watch);
}

void DecoderImpl::skipAcls(Buffer::Instance& data, uint64_t& offset) {
  const int32_t count = helper_.peekInt32(data, offset);

  for (int i = 0; i < count; ++i) {
    // Perms.
    helper_.peekInt32(data, offset);
    // Skip scheme.
    skipString(data, offset);
    // Skip cred.
    skipString(data, offset);
  }
}

void DecoderImpl::parseCreateRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len,
                                     OpCodes opcode) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + (3 * INT_LENGTH));

  const std::string path = helper_.peekString(data, offset);

  // Skip data.
  skipString(data, offset);
  skipAcls(data, offset);

  const CreateFlags flags = static_cast<CreateFlags>(helper_.peekInt32(data, offset));
  callbacks_.onCreateRequest(path, flags, opcode);
}

void DecoderImpl::parseSetRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + (3 * INT_LENGTH));

  const std::string path = helper_.peekString(data, offset);
  // Skip data.
  skipString(data, offset);
  // Ignore version.
  helper_.peekInt32(data, offset);

  callbacks_.onSetRequest(path);
}

void DecoderImpl::parseGetChildrenRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len,
                                          const bool two) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + INT_LENGTH + BOOL_LENGTH);

  const std::string path = helper_.peekString(data, offset);
  const bool watch = helper_.peekBool(data, offset);

  callbacks_.onGetChildrenRequest(path, watch, two);
}

void DecoderImpl::parseDeleteRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + (2 * INT_LENGTH));

  const std::string path = helper_.peekString(data, offset);
  const int32_t version = helper_.peekInt32(data, offset);

  callbacks_.onDeleteRequest(path, version);
}

void DecoderImpl::parseExistsRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + INT_LENGTH + BOOL_LENGTH);

  const std::string path = helper_.peekString(data, offset);
  const bool watch = helper_.peekBool(data, offset);

  callbacks_.onExistsRequest(path, watch);
}

void DecoderImpl::parseGetAclRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + INT_LENGTH);

  const std::string path = helper_.peekString(data, offset);

  callbacks_.onGetAclRequest(path);
}

void DecoderImpl::parseSetAclRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + (2 * INT_LENGTH));

  const std::string path = helper_.peekString(data, offset);
  skipAcls(data, offset);
  const int32_t version = helper_.peekInt32(data, offset);

  callbacks_.onSetAclRequest(path, version);
}

std::string DecoderImpl::pathOnlyRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + INT_LENGTH);
  return helper_.peekString(data, offset);
}

void DecoderImpl::parseCheckRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, (2 * INT_LENGTH));

  const std::string path = helper_.peekString(data, offset);
  const int32_t version = helper_.peekInt32(data, offset);

  callbacks_.onCheckRequest(path, version);
}

void DecoderImpl::parseMultiRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  // Treat empty transactions as a decoding error, there should be at least 1 header.
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + MULTI_HEADER_LENGTH);

  while (true) {
    const int32_t opcode = helper_.peekInt32(data, offset);
    const bool done = helper_.peekBool(data, offset);
    // Ignore error field.
    helper_.peekInt32(data, offset);

    if (done) {
      break;
    }

    switch (static_cast<OpCodes>(opcode)) {
    case OpCodes::Create:
      parseCreateRequest(data, offset, len, OpCodes::Create);
      break;
    case OpCodes::SetData:
      parseSetRequest(data, offset, len);
      break;
    case OpCodes::Check:
      parseCheckRequest(data, offset, len);
      break;
    default:
      throw EnvoyException(fmt::format("Unknown opcode within a transaction: {}", opcode));
    }
  }

  callbacks_.onMultiRequest();
}

void DecoderImpl::parseReconfigRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + (3 * INT_LENGTH) + LONG_LENGTH);

  // Skip joining.
  skipString(data, offset);
  // Skip leaving.
  skipString(data, offset);
  // Skip new members.
  skipString(data, offset);
  // Read config id.
  helper_.peekInt64(data, offset);

  callbacks_.onReconfigRequest();
}

void DecoderImpl::parseSetWatchesRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + (3 * INT_LENGTH));

  // Ignore relative Zxid.
  helper_.peekInt64(data, offset);
  // Data watches.
  skipStrings(data, offset);
  // Exist watches.
  skipStrings(data, offset);
  // Child watches.
  skipStrings(data, offset);

  callbacks_.onSetWatchesRequest();
}

void DecoderImpl::parseXWatchesRequest(Buffer::Instance& data, uint64_t& offset, uint32_t len,
                                       OpCodes opcode) {
  ensureMinLength(len, XID_LENGTH + OPCODE_LENGTH + (2 * INT_LENGTH));

  const std::string path = helper_.peekString(data, offset);
  const int32_t type = helper_.peekInt32(data, offset);

  if (opcode == OpCodes::CheckWatches) {
    callbacks_.onCheckWatchesRequest(path, type);
  } else {
    callbacks_.onRemoveWatchesRequest(path, type);
  }
}

void DecoderImpl::skipString(Buffer::Instance& data, uint64_t& offset) {
  const int32_t slen = helper_.peekInt32(data, offset);
  if (slen < 0) {
    ENVOY_LOG(trace,
              "zookeeper_proxy: decoding response with negative string length {} at offset {}",
              slen, offset);
    return;
  }
  helper_.skip(slen, offset);
}

void DecoderImpl::skipStrings(Buffer::Instance& data, uint64_t& offset) {
  const int32_t count = helper_.peekInt32(data, offset);

  for (int i = 0; i < count; ++i) {
    skipString(data, offset);
  }
}

Network::FilterStatus DecoderImpl::onData(Buffer::Instance& data) {
  return decodeAndBuffer(data, DecodeType::READ, zk_filter_read_buffer_);
}

Network::FilterStatus DecoderImpl::onWrite(Buffer::Instance& data) {
  return decodeAndBuffer(data, DecodeType::WRITE, zk_filter_write_buffer_);
}

Network::FilterStatus DecoderImpl::decodeAndBuffer(Buffer::Instance& data, DecodeType dtype,
                                                   Buffer::OwnedImpl& zk_filter_buffer) {
  const uint32_t zk_filter_buffer_len = zk_filter_buffer.length();

  if (zk_filter_buffer_len == 0) {
    decodeAndBufferHelper(data, dtype, zk_filter_buffer);
    return Network::FilterStatus::Continue;
  }

  // ZooKeeper filter buffer contains partial packet data from the previous network filter buffer.
  // Prepending ZooKeeper filter buffer to the current network filter buffer can help to generate
  // full packets.
  data.prepend(zk_filter_buffer);
  decodeAndBufferHelper(data, dtype, zk_filter_buffer);
  // Drain the prepended ZooKeeper filter buffer.
  data.drain(zk_filter_buffer_len);
  return Network::FilterStatus::Continue;
}

void DecoderImpl::decodeAndBufferHelper(Buffer::Instance& data, DecodeType dtype,
                                        Buffer::OwnedImpl& zk_filter_buffer) {
  ASSERT(dtype == DecodeType::READ || dtype == DecodeType::WRITE);

  const uint32_t data_len = data.length();
  uint64_t offset = 0;
  uint32_t len = 0;
  // Boolean to check whether there is at least one full packet in the network filter buffer (to
  // which the ZooKeeper filter buffer is prepended).
  bool has_full_packets = false;

  while (offset < data_len) {
    try {
      // Peek packet length.
      len = helper_.peekInt32(data, offset);
      ensureMinLength(len, dtype == DecodeType::READ ? XID_LENGTH + INT_LENGTH
                                                     : XID_LENGTH + ZXID_LENGTH + INT_LENGTH);
      ensureMaxLength(len);
      offset += len;
      if (offset <= data_len) {
        has_full_packets = true;
      }
    } catch (const EnvoyException& e) {
      ENVOY_LOG(debug, "zookeeper_proxy: decoding exception {}", e.what());
      callbacks_.onDecodeError();
      return;
    }
  }

  if (offset == data_len) {
    decode(data, dtype);
    return;
  }

  ASSERT(offset > data_len);
  std::string temp_data;

  if (has_full_packets) {
    offset -= INT_LENGTH + len;
    // Decode full packets.
    // TODO(Winbobob): use BufferFragment to avoid copying the full packets.
    temp_data.resize(offset);
    data.copyOut(0, offset, temp_data.data());
    Buffer::OwnedImpl full_packets;
    full_packets.add(temp_data.data(), temp_data.length());
    decode(full_packets, dtype);

    // Copy out the rest of the data to the ZooKeeper filter buffer.
    temp_data.resize(data_len - offset);
    data.copyOut(offset, data_len - offset, temp_data.data());
    zk_filter_buffer.add(temp_data.data(), temp_data.length());
  } else {
    // Copy out all the data to the ZooKeeper filter buffer, since after prepending the ZooKeeper
    // filter buffer is drained by the prepend() method.
    temp_data.resize(data_len);
    data.copyOut(0, data_len, temp_data.data());
    zk_filter_buffer.add(temp_data.data(), temp_data.length());
  }
}

void DecoderImpl::decode(Buffer::Instance& data, DecodeType dtype) {
  uint64_t offset = 0;

  try {
    while (offset < data.length()) {
      // Reset the helper's cursor, to ensure the current message stays within the
      // allowed max length, even when it's different than the declared length
      // by the message.
      //
      // Note: we need to keep two cursors — offset and helper_'s internal one — because
      //       a buffer may contain multiple messages, so offset is global while helper_'s
      //       internal cursor gets reset for each individual message.
      helper_.reset();

      const uint64_t current = offset;
      switch (dtype) {
      case DecodeType::READ:
        decodeOnData(data, offset);
        callbacks_.onRequestBytes(offset - current);
        break;
      case DecodeType::WRITE:
        decodeOnWrite(data, offset);
        callbacks_.onResponseBytes(offset - current);
        break;
      }
    }
  } catch (const EnvoyException& e) {
    ENVOY_LOG(debug, "zookeeper_proxy: decoding exception {}", e.what());
    callbacks_.onDecodeError();
  }
}

void DecoderImpl::parseConnectResponse(Buffer::Instance& data, uint64_t& offset, uint32_t len,
                                       const std::chrono::milliseconds& latency) {
  ensureMinLength(len, PROTOCOL_VERSION_LENGTH + TIMEOUT_LENGTH + SESSION_LENGTH + INT_LENGTH);

  const auto timeout = helper_.peekInt32(data, offset);

  // Skip session id + password.
  offset += SESSION_LENGTH;
  skipString(data, offset);

  const bool readonly = maybeReadBool(data, offset);

  callbacks_.onConnectResponse(0, timeout, readonly, latency);
}

void DecoderImpl::parseWatchEvent(Buffer::Instance& data, uint64_t& offset, const uint32_t len,
                                  const int64_t zxid, const int32_t error) {
  ensureMinLength(len, SERVER_HEADER_LENGTH + (3 * INT_LENGTH));

  const auto event_type = helper_.peekInt32(data, offset);
  const auto client_state = helper_.peekInt32(data, offset);
  const auto path = helper_.peekString(data, offset);

  callbacks_.onWatchEvent(event_type, client_state, path, zxid, error);
}

bool DecoderImpl::maybeReadBool(Buffer::Instance& data, uint64_t& offset) {
  if (data.length() >= offset + 1) {
    return helper_.peekBool(data, offset);
  }
  return false;
}

} // namespace ZooKeeperProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
