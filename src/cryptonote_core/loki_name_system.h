#ifndef LOKI_NAME_SYSTEM_H
#define LOKI_NAME_SYSTEM_H

#include "crypto/crypto.h"
#include "cryptonote_config.h"
#include "span.h"
#include "cryptonote_basic/tx_extra.h"
#include "common/hex.h"

#include <string>

struct sqlite3;
struct sqlite3_stmt;
namespace cryptonote
{
struct checkpoint_t;
struct block;
class transaction;
struct account_address;
struct tx_extra_loki_name_system;
class Blockchain;
}; // namespace cryptonote

namespace lns
{

constexpr size_t WALLET_NAME_MAX                  = 96;
constexpr size_t WALLET_ACCOUNT_BINARY_LENGTH     = 2 * sizeof(crypto::public_key);
constexpr size_t LOKINET_DOMAIN_NAME_MAX          = 253;
constexpr size_t LOKINET_ADDRESS_BINARY_LENGTH    = sizeof(crypto::ed25519_public_key);
constexpr size_t SESSION_DISPLAY_NAME_MAX         = 64;
constexpr size_t SESSION_PUBLIC_KEY_BINARY_LENGTH = 1 + sizeof(crypto::ed25519_public_key); // Session keys at prefixed with 0x05 + ed25519 key

struct mapping_value
{
  static size_t constexpr BUFFER_SIZE = 255;
  std::array<uint8_t, BUFFER_SIZE> buffer;
  size_t len;

  std::string               to_string() const { return std::string(reinterpret_cast<char const *>(buffer.data()), len); }
  epee::span<const uint8_t> to_span()   const { return epee::span<const uint8_t>(reinterpret_cast<const uint8_t *>(buffer.data()), len); }
  bool operator==(mapping_value const &other) const { return other.len    == len && memcmp(buffer.data(), other.buffer.data(), len) == 0; }
  bool operator==(std::string   const &other) const { return other.size() == len && memcmp(buffer.data(), other.data(), len) == 0; }
};
inline std::ostream &operator<<(std::ostream &os, mapping_value const &v) { return os << hex::from_hex(v.buffer.begin(), v.buffer.begin() + v.len); }

inline char const *mapping_type_str(mapping_type type)
{
  switch(type)
  {
    case mapping_type::lokinet_1year:   return "lokinet_1year";
    case mapping_type::lokinet_2years:  return "lokinet_2years";
    case mapping_type::lokinet_5years:  return "lokinet_5years";
    case mapping_type::lokinet_10years: return "lokinet_10years";
    case mapping_type::session:         return "session";
    case mapping_type::wallet:          return "wallet";
    default: assert(false);             return "xx_unhandled_type";
  }
}
inline std::ostream &operator<<(std::ostream &os, mapping_type type) { return os << mapping_type_str(type); }
constexpr bool mapping_type_allowed(uint8_t hf_version, mapping_type type) { return type == mapping_type::session; }
constexpr bool is_lokinet_type     (lns::mapping_type type)                { return type >= mapping_type::lokinet_1year && type <= mapping_type::lokinet_10years; }
sqlite3       *init_loki_name_system(char const *file_path);

uint64_t constexpr NO_EXPIRY = static_cast<uint64_t>(-1);
// return: The number of blocks until expiry from the registration height, if there is no expiration NO_EXPIRY is returned.
uint64_t     expiry_blocks(cryptonote::network_type nettype, mapping_type type, uint64_t *renew_window = nullptr);
crypto::hash tx_extra_signature_hash(epee::span<const uint8_t> blob, crypto::hash const &prev_txid);
bool         validate_lns_name(mapping_type type, std::string const &name, std::string *reason = nullptr);

// Validate a human readable mapping value representation in 'value' and write the binary form into 'blob'.
// value: if type is session, 64 character hex string of an ed25519 public key
//                   lokinet, 52 character base32z string of an ed25519 public key
//                   wallet,  the wallet public address string
// blob: (optional) if function returns true, validate_mapping_value will convert the 'value' into a binary format suitable for encryption in encrypt_mapping_value(...)
bool         validate_mapping_value(cryptonote::network_type nettype, mapping_type type, std::string const &value, mapping_value *blob = nullptr, std::string *reason = nullptr);
bool         validate_encrypted_mapping_value(mapping_type type, std::string const &value, std::string *reason = nullptr);

// Converts a human readable case-insensitive string denoting the mapping type into a value suitable for storing into the LNS DB.
// Currently only accepts "session"
// mapping_type: (optional) if function returns true, the uint16_t value of the 'type' will be set
bool         validate_mapping_type(std::string const &type, mapping_type *mapping_type, std::string *reason);

// Takes a human readable mapping name and converts to a hash suitable for storing into the LNS DB.
crypto::hash name_to_hash(std::string const &name);

// Takes a binary value and encrypts it using 'name' as a secret key or vice versa, suitable for storing into the LNS DB.
// Only basic overflow validation is attempted, values should be pre-validated in the validate* functions.
bool         encrypt_mapping_value(std::string const &name, mapping_value const &value, mapping_value &encrypted_value);
bool         decrypt_mapping_value(std::string const &name, mapping_value const &encrypted_value, mapping_value &value);

struct owner_record
{
  operator bool() const { return loaded; }
  bool loaded;

  int64_t id;
  crypto::ed25519_public_key key;
};

struct settings_record
{
  operator bool() const { return loaded; }
  bool loaded;

  uint64_t     top_height;
  crypto::hash top_hash;
  int          version;
};

struct mapping_record
{
  // NOTE: We keep expired entries in the DB indefinitely because we need to
  // keep all LNS entries indefinitely to support large blockchain detachments.
  // A mapping_record forms a linked list of TXID's which allows us to revert
  // the LNS DB to any arbitrary height at a small additional storage cost.
  // return: if the record is still active and hasn't expired.
  bool active(cryptonote::network_type nettype, uint64_t blockchain_height) const;
  operator bool() const { return loaded; }

  bool                       loaded;
  mapping_type               type;
  crypto::hash               name_hash;
  mapping_value              encrypted_value;
  uint64_t                   register_height;
  int64_t                    owner_id;
  crypto::ed25519_public_key owner;
  crypto::hash               txid;
  crypto::hash               prev_txid;
};

struct name_system_db
{
  bool                        init        (cryptonote::network_type nettype, sqlite3 *db, uint64_t top_height, crypto::hash const &top_hash);
  bool                        add_block   (const cryptonote::block& block, const std::vector<cryptonote::transaction>& txs);

  cryptonote::network_type    network_type() const { return nettype; }
  uint64_t                    height      () const { return last_processed_height; }

  // Signifies the blockchain has reorganized commences the rollback and pruning procedures.
  void                        block_detach(cryptonote::Blockchain const &blockchain, uint64_t new_blockchain_height);

  bool                        save_owner   (crypto::ed25519_public_key const &key, int64_t *row_id);
  bool                        save_mapping (crypto::hash const &tx_hash, cryptonote::tx_extra_loki_name_system const &src, uint64_t height, int64_t owner_id);
  bool                        save_settings(uint64_t top_height, crypto::hash const &top_hash, int version);

  // Delete all mappings that are registered on height or newer followed by deleting all owners no longer referenced in the DB
  bool                        prune_db(uint64_t height);

  owner_record                get_owner_by_key      (crypto::ed25519_public_key const &key) const;
  owner_record                get_owner_by_id       (int64_t owner_id) const;
  mapping_record              get_mapping           (mapping_type type, crypto::hash const &name_hash) const;
  std::vector<mapping_record> get_mappings          (std::vector<uint16_t> const &types, crypto::hash const &name) const;
  std::vector<mapping_record> get_mappings_by_owner (crypto::ed25519_public_key const &key) const;
  std::vector<mapping_record> get_mappings_by_owners(std::vector<crypto::ed25519_public_key> const &keys) const;
  settings_record             get_settings          () const;

  // entry: (optional) if function returns true, the Loki Name System entry in the TX extra is copied into 'entry'
  bool                        validate_lns_tx       (uint8_t hf_version, uint64_t blockchain_height, cryptonote::transaction const &tx, cryptonote::tx_extra_loki_name_system *entry = nullptr, std::string *reason = nullptr) const;

  sqlite3 *db               = nullptr;
  bool    transaction_begun = false;
private:
  cryptonote::network_type nettype;
  uint64_t last_processed_height                     = 0;
  sqlite3_stmt *save_owner_sql                       = nullptr;
  sqlite3_stmt *save_mapping_sql                     = nullptr;
  sqlite3_stmt *save_settings_sql                    = nullptr;
  sqlite3_stmt *get_owner_by_key_sql                 = nullptr;
  sqlite3_stmt *get_owner_by_id_sql                  = nullptr;
  sqlite3_stmt *get_mapping_sql                      = nullptr;
  sqlite3_stmt *get_settings_sql                     = nullptr;
  sqlite3_stmt *prune_mappings_sql                   = nullptr;
  sqlite3_stmt *prune_owners_sql                     = nullptr;
  sqlite3_stmt *get_mappings_by_owner_sql            = nullptr;
  sqlite3_stmt *get_mappings_on_height_and_newer_sql = nullptr;
};

}; // namespace service_nodes
#endif // LOKI_NAME_SYSTEM_H
