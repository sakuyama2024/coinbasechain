#include "network/message.hpp"
#include "util/sha256.hpp"
#include "chain/block.hpp"
#include "chain/endian.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace coinbasechain {
namespace message {

// VarInt implementation
size_t VarInt::encoded_size() const {
  if (value < 0xfd)
    return 1;
  if (value <= 0xffff)
    return 3;
  if (value <= 0xffffffff)
    return 5;
  return 9;
}

size_t VarInt::encode(uint8_t *buffer) const {
  if (value < 0xfd) {
    buffer[0] = static_cast<uint8_t>(value);
    return 1;
  } else if (value <= 0xffff) {
    buffer[0] = 0xfd;
    endian::WriteLE16(buffer + 1, static_cast<uint16_t>(value));
    return 3;
  } else if (value <= 0xffffffff) {
    buffer[0] = 0xfe;
    endian::WriteLE32(buffer + 1, static_cast<uint32_t>(value));
    return 5;
  } else {
    buffer[0] = 0xff;
    endian::WriteLE64(buffer + 1, value);
    return 9;
  }
}

size_t VarInt::decode(const uint8_t *buffer, size_t available) {
  if (available < 1)
    return 0;

  uint8_t first = buffer[0];
  if (first < 0xfd) {
    value = first;
    return 1;
  } else if (first == 0xfd) {
    if (available < 3)
      return 0;
    value = endian::ReadLE16(buffer + 1);
    return 3;
  } else if (first == 0xfe) {
    if (available < 5)
      return 0;
    value = endian::ReadLE32(buffer + 1);
    return 5;
  } else {
    if (available < 9)
      return 0;
    value = endian::ReadLE64(buffer + 1);
    return 9;
  }
}

// MessageSerializer implementation
MessageSerializer::MessageSerializer() {
  buffer_.reserve(1024); // Reserve some space
}

void MessageSerializer::write_uint8(uint8_t value) { buffer_.push_back(value); }

void MessageSerializer::write_uint16(uint16_t value) {
  size_t pos = buffer_.size();
  buffer_.resize(pos + 2);
  endian::WriteLE16(buffer_.data() + pos, value);
}

void MessageSerializer::write_uint32(uint32_t value) {
  size_t pos = buffer_.size();
  buffer_.resize(pos + 4);
  endian::WriteLE32(buffer_.data() + pos, value);
}

void MessageSerializer::write_uint64(uint64_t value) {
  size_t pos = buffer_.size();
  buffer_.resize(pos + 8);
  endian::WriteLE64(buffer_.data() + pos, value);
}

void MessageSerializer::write_int32(int32_t value) {
  write_uint32(static_cast<uint32_t>(value));
}

void MessageSerializer::write_int64(int64_t value) {
  write_uint64(static_cast<uint64_t>(value));
}

void MessageSerializer::write_bool(bool value) { write_uint8(value ? 1 : 0); }

void MessageSerializer::write_varint(uint64_t value) {
  VarInt vi(value);
  size_t pos = buffer_.size();
  buffer_.resize(pos + vi.encoded_size());
  vi.encode(buffer_.data() + pos);
}

void MessageSerializer::write_string(const std::string &str) {
  write_varint(str.length());
  write_bytes(reinterpret_cast<const uint8_t *>(str.data()), str.length());
}

void MessageSerializer::write_bytes(const uint8_t *data, size_t len) {
  buffer_.insert(buffer_.end(), data, data + len);
}

void MessageSerializer::write_bytes(const std::vector<uint8_t> &data) {
  buffer_.insert(buffer_.end(), data.begin(), data.end());
}

void MessageSerializer::write_network_address(
    const protocol::NetworkAddress &addr, bool include_timestamp) {
  if (include_timestamp) {
    // Timestamp should be written by caller before calling this function
  }
  // Services (LE)
  write_uint64(addr.services);
  // IP (16 bytes)
  write_bytes(addr.ip.data(), addr.ip.size());
  // Port (BE)
  size_t pos = buffer_.size();
  buffer_.resize(pos + 2);
  endian::WriteBE16(buffer_.data() + pos, addr.port);
}

void MessageSerializer::write_inventory_vector(
    const protocol::InventoryVector &inv) {
  write_uint32(static_cast<uint32_t>(inv.type));
  write_bytes(inv.hash.data(), inv.hash.size());
}

// MessageDeserializer implementation
MessageDeserializer::MessageDeserializer(const uint8_t *data, size_t size)
    : data_(data), size_(size), position_(0), error_(false) {}

MessageDeserializer::MessageDeserializer(const std::vector<uint8_t> &data)
    : data_(data.data()), size_(data.size()), position_(0), error_(false) {}

void MessageDeserializer::check_available(size_t bytes) {
  if (bytes_remaining() < bytes) {
    error_ = true;
  }
}

uint8_t MessageDeserializer::read_uint8() {
  check_available(1);
  if (error_)
    return 0;
  return data_[position_++];
}

uint16_t MessageDeserializer::read_uint16() {
  check_available(2);
  if (error_)
    return 0;
  uint16_t value = endian::ReadLE16(data_ + position_);
  position_ += 2;
  return value;
}

uint32_t MessageDeserializer::read_uint32() {
  check_available(4);
  if (error_)
    return 0;
  uint32_t value = endian::ReadLE32(data_ + position_);
  position_ += 4;
  return value;
}

uint64_t MessageDeserializer::read_uint64() {
  check_available(8);
  if (error_)
    return 0;
  uint64_t value = endian::ReadLE64(data_ + position_);
  position_ += 8;
  return value;
}

int32_t MessageDeserializer::read_int32() {
  return static_cast<int32_t>(read_uint32());
}

int64_t MessageDeserializer::read_int64() {
  return static_cast<int64_t>(read_uint64());
}

bool MessageDeserializer::read_bool() { return read_uint8() != 0; }

uint64_t MessageDeserializer::read_varint() {
  VarInt vi;
  check_available(1);
  if (error_)
    return 0;

  size_t consumed = vi.decode(data_ + position_, bytes_remaining());
  if (consumed == 0) {
    error_ = true;
    return 0;
  }

  // Validate against MAX_SIZE to prevent DoS attacks
  if (vi.value > protocol::MAX_SIZE) {
    error_ = true;
    return 0;
  }

  position_ += consumed;
  return vi.value;
}

std::string MessageDeserializer::read_string() {
  uint64_t len = read_varint();
  if (error_ || len > bytes_remaining()) {
    error_ = true;
    return "";
  }

  std::string result(reinterpret_cast<const char *>(data_ + position_), len);
  position_ += len;
  return result;
}

std::vector<uint8_t> MessageDeserializer::read_bytes(size_t count) {
  check_available(count);
  if (error_)
    return {};

  std::vector<uint8_t> result(data_ + position_, data_ + position_ + count);
  position_ += count;
  return result;
}

protocol::NetworkAddress
MessageDeserializer::read_network_address(bool has_timestamp) {
  if (has_timestamp) {
    read_uint32(); // Skip timestamp
  }

  protocol::NetworkAddress addr;
  addr.services = read_uint64();

  check_available(16);
  if (error_)
    return addr;
  std::memcpy(addr.ip.data(), data_ + position_, 16);
  position_ += 16;

  // Port is big-endian
  check_available(2);
  if (error_)
    return addr;
  addr.port = endian::ReadBE16(data_ + position_);
  position_ += 2;

  return addr;
}

protocol::TimestampedAddress MessageDeserializer::read_timestamped_address() {
  protocol::TimestampedAddress ts_addr;
  ts_addr.timestamp = read_uint32();
  ts_addr.address = read_network_address(false);
  return ts_addr;
}

protocol::InventoryVector MessageDeserializer::read_inventory_vector() {
  protocol::InventoryVector inv;
  inv.type = static_cast<protocol::InventoryType>(read_uint32());

  check_available(32);
  if (error_)
    return inv;
  std::memcpy(inv.hash.data(), data_ + position_, 32);
  position_ += 32;

  return inv;
}

// Crypto utility functions
std::array<uint8_t, 32> sha256(const uint8_t *data, size_t size) {
  std::array<uint8_t, 32> hash;
  CSHA256().Write(data, size).Finalize(hash.data());
  return hash;
}

std::array<uint8_t, 32> sha256(const std::vector<uint8_t> &data) {
  return sha256(data.data(), data.size());
}

std::array<uint8_t, 32> double_sha256(const uint8_t *data, size_t size) {
  auto first_hash = sha256(data, size);
  return sha256(first_hash.data(), first_hash.size());
}

std::array<uint8_t, 32> double_sha256(const std::vector<uint8_t> &data) {
  return double_sha256(data.data(), data.size());
}

std::array<uint8_t, 4> compute_checksum(const std::vector<uint8_t> &payload) {
  auto hash = double_sha256(payload);
  std::array<uint8_t, 4> checksum;
  std::memcpy(checksum.data(), hash.data(), 4);
  return checksum;
}

protocol::MessageHeader create_header(uint32_t magic,
                                      const std::string &command,
                                      const std::vector<uint8_t> &payload) {
  protocol::MessageHeader header(magic, command,
                                 static_cast<uint32_t>(payload.size()));
  header.checksum = compute_checksum(payload);
  return header;
}

std::vector<uint8_t> serialize_header(const protocol::MessageHeader &header) {
  std::vector<uint8_t> buffer(protocol::MESSAGE_HEADER_SIZE);
  size_t pos = 0;

  endian::WriteLE32(buffer.data() + pos, header.magic);
  pos += 4;

  std::memcpy(buffer.data() + pos, header.command.data(),
              protocol::COMMAND_SIZE);
  pos += protocol::COMMAND_SIZE;

  endian::WriteLE32(buffer.data() + pos, header.length);
  pos += 4;

  std::memcpy(buffer.data() + pos, header.checksum.data(),
              protocol::CHECKSUM_SIZE);

  return buffer;
}

bool deserialize_header(const uint8_t *data, size_t size,
                        protocol::MessageHeader &header) {
  if (size < protocol::MESSAGE_HEADER_SIZE) {
    return false;
  }

  size_t pos = 0;

  header.magic = endian::ReadLE32(data + pos);
  pos += 4;

  std::memcpy(header.command.data(), data + pos, protocol::COMMAND_SIZE);
  pos += protocol::COMMAND_SIZE;

  header.length = endian::ReadLE32(data + pos);
  pos += 4;

  // Prevents attackers from sending 4+ GB messages causing memory exhaustion
  if (header.length > protocol::MAX_PROTOCOL_MESSAGE_LENGTH) {
    return false;
  }

  std::memcpy(header.checksum.data(), data + pos, protocol::CHECKSUM_SIZE);

  return true;
}

std::unique_ptr<Message> create_message(const std::string &command) {
  if (command == protocol::commands::VERSION)
    return std::make_unique<VersionMessage>();
  if (command == protocol::commands::VERACK)
    return std::make_unique<VerackMessage>();
  if (command == protocol::commands::PING)
    return std::make_unique<PingMessage>();
  if (command == protocol::commands::PONG)
    return std::make_unique<PongMessage>();
  if (command == protocol::commands::ADDR)
    return std::make_unique<AddrMessage>();
  if (command == protocol::commands::GETADDR)
    return std::make_unique<GetAddrMessage>();
  if (command == protocol::commands::INV)
    return std::make_unique<InvMessage>();
  if (command == protocol::commands::GETHEADERS)
    return std::make_unique<GetHeadersMessage>();
  if (command == protocol::commands::HEADERS)
    return std::make_unique<HeadersMessage>();

  return nullptr;
}

// Message implementations

// VersionMessage
VersionMessage::VersionMessage()
    : version(protocol::PROTOCOL_VERSION), services(protocol::NODE_NETWORK),
      timestamp(0), nonce(0), user_agent(protocol::GetUserAgent()),
      start_height(0) {}

std::vector<uint8_t> VersionMessage::serialize() const {
  MessageSerializer s;
  s.write_int32(version);
  s.write_uint64(services);
  s.write_int64(timestamp);
  s.write_network_address(addr_recv, false);
  s.write_network_address(addr_from, false);
  s.write_uint64(nonce);
  s.write_string(user_agent);
  s.write_int32(start_height);
  return s.data();
}

bool VersionMessage::deserialize(const uint8_t *data, size_t size) {
  MessageDeserializer d(data, size);
  version = d.read_int32();
  services = d.read_uint64();
  timestamp = d.read_int64();
  addr_recv = d.read_network_address(false);
  addr_from = d.read_network_address(false);
  nonce = d.read_uint64();
  user_agent = d.read_string();

  // SECURITY: Enforce maximum user agent length to prevent memory exhaustion
  // Legitimate user agents are ~20-50 bytes (e.g., "/CoinbaseChain:1.0.0/")
  if (user_agent.length() > protocol::MAX_SUBVERSION_LENGTH) {
    return false;
  }

  start_height = d.read_int32();
  return !d.has_error();
}

// VerackMessage
std::vector<uint8_t> VerackMessage::serialize() const {
  return {}; // Empty payload
}

bool VerackMessage::deserialize(const uint8_t *data, size_t size) {
  return size == 0; // Should be empty
}

// PingMessage
std::vector<uint8_t> PingMessage::serialize() const {
  MessageSerializer s;
  s.write_uint64(nonce);
  return s.data();
}

bool PingMessage::deserialize(const uint8_t *data, size_t size) {
  MessageDeserializer d(data, size);
  nonce = d.read_uint64();
  return !d.has_error();
}

// PongMessage
std::vector<uint8_t> PongMessage::serialize() const {
  MessageSerializer s;
  s.write_uint64(nonce);
  return s.data();
}

bool PongMessage::deserialize(const uint8_t *data, size_t size) {
  MessageDeserializer d(data, size);
  nonce = d.read_uint64();
  return !d.has_error();
}

// AddrMessage
std::vector<uint8_t> AddrMessage::serialize() const {
  MessageSerializer s;
  s.write_varint(addresses.size());
  for (const auto &addr : addresses) {
    s.write_uint32(addr.timestamp);
    s.write_network_address(addr.address, false);
  }
  return s.data();
}

bool AddrMessage::deserialize(const uint8_t *data, size_t size) {
  MessageDeserializer d(data, size);
  uint64_t count = d.read_varint();
  if (count > protocol::MAX_ADDR_SIZE)
    return false;

  // Incremental allocation to prevent DoS attacks
  // Instead of reserve(count) which could allocate GBs, allocate in 5 MB
  // batches
  addresses.clear();
  uint64_t allocated = 0;
  constexpr size_t batch_size =
      protocol::MAX_VECTOR_ALLOCATE / sizeof(protocol::TimestampedAddress);

  for (uint64_t i = 0; i < count; ++i) {
    // Allocate next batch when needed
    if (addresses.size() >= allocated) {
      allocated = std::min(count, allocated + batch_size);
      addresses.reserve(allocated);
    }
    addresses.push_back(d.read_timestamped_address());
    if (d.has_error())
      return false;
  }
  return !d.has_error();
}

// GetAddrMessage
std::vector<uint8_t> GetAddrMessage::serialize() const {
  return {}; // Empty payload
}

bool GetAddrMessage::deserialize(const uint8_t *data, size_t size) {
  return size == 0;
}

// InvMessage
std::vector<uint8_t> InvMessage::serialize() const {
  MessageSerializer s;
  s.write_varint(inventory.size());
  for (const auto &inv : inventory) {
    s.write_inventory_vector(inv);
  }
  return s.data();
}

bool InvMessage::deserialize(const uint8_t *data, size_t size) {
  MessageDeserializer d(data, size);
  uint64_t count = d.read_varint();
  if (count > protocol::MAX_INV_SIZE)
    return false;

  // Incremental allocation to prevent DoS attacks
  inventory.clear();
  uint64_t allocated = 0;
  constexpr size_t batch_size =
      protocol::MAX_VECTOR_ALLOCATE / sizeof(protocol::InventoryVector);

  for (uint64_t i = 0; i < count; ++i) {
    if (inventory.size() >= allocated) {
      allocated = std::min(count, allocated + batch_size);
      inventory.reserve(allocated);
    }
    inventory.push_back(d.read_inventory_vector());
    if (d.has_error())
      return false;
  }
  return !d.has_error();
}


// GetHeadersMessage
GetHeadersMessage::GetHeadersMessage() : version(protocol::PROTOCOL_VERSION) {
  hash_stop.fill(0);
}

std::vector<uint8_t> GetHeadersMessage::serialize() const {
  MessageSerializer s;
  s.write_uint32(version);
  s.write_varint(block_locator_hashes.size());
  for (const auto &hash : block_locator_hashes) {
    s.write_bytes(hash.data(), hash.size());
  }
  s.write_bytes(hash_stop.data(), hash_stop.size());
  return s.data();
}

bool GetHeadersMessage::deserialize(const uint8_t *data, size_t size) {
  MessageDeserializer d(data, size);
  version = d.read_uint32();

  uint64_t count = d.read_varint();

  // Enforce MAX_LOCATOR_SZ to prevent CPU exhaustion attacks
  // Prevents attackers from sending 1000+ locator hashes causing expensive
  // FindFork() operations
  if (count > protocol::MAX_LOCATOR_SZ)
    return false;

  // Incremental allocation to prevent DoS attacks
  block_locator_hashes.clear();
  uint64_t allocated = 0;
  constexpr size_t batch_size =
      protocol::MAX_VECTOR_ALLOCATE / 32; // 32 bytes per hash

  for (uint64_t i = 0; i < count; ++i) {
    if (block_locator_hashes.size() >= allocated) {
      allocated = std::min(count, allocated + batch_size);
      block_locator_hashes.reserve(allocated);
    }
    std::array<uint8_t, 32> hash;
    auto bytes = d.read_bytes(32);
    if (bytes.size() != 32)
      return false;
    std::memcpy(hash.data(), bytes.data(), 32);
    block_locator_hashes.push_back(hash);
  }

  auto stop_hash = d.read_bytes(32);
  if (stop_hash.size() != 32)
    return false;
  std::memcpy(hash_stop.data(), stop_hash.data(), 32);

  return !d.has_error();
}

// HeadersMessage
std::vector<uint8_t> HeadersMessage::serialize() const {
  MessageSerializer s;
  s.write_varint(headers.size());
  for (const auto &header : headers) {
    auto header_bytes = header.Serialize();
    s.write_bytes(header_bytes);
    // Headers-only chain: no transaction count needed
  }
  return s.data();
}

bool HeadersMessage::deserialize(const uint8_t *data, size_t size) {
  MessageDeserializer d(data, size);
  uint64_t count = d.read_varint();
  if (count > protocol::MAX_HEADERS_SIZE)
    return false;

  // SECURITY: Incremental allocation to prevent DoS attacks
  headers.clear();
  uint64_t allocated = 0;
  constexpr size_t batch_size =
      protocol::MAX_VECTOR_ALLOCATE / sizeof(CBlockHeader);

  for (uint64_t i = 0; i < count; ++i) {
    if (headers.size() >= allocated) {
      allocated = std::min(count, allocated + batch_size);
      headers.reserve(allocated);
    }

    // Read header bytes
    auto header_bytes = d.read_bytes(CBlockHeader::HEADER_SIZE);
    if (header_bytes.size() != CBlockHeader::HEADER_SIZE)
      return false;

    CBlockHeader header;
    if (!header.Deserialize(header_bytes.data(), header_bytes.size())) {
      return false;
    }

    headers.push_back(header);
  }
  return !d.has_error();
}

} // namespace message
} // namespace coinbasechain
