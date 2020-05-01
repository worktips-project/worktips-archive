#include <bitset>
#include "loki_name_system.h"

#include "checkpoints/checkpoints.h"
#include "common/loki.h"
#include "common/util.h"
#include "common/base32z.h"
#include "crypto/hash.h"
#include "cryptonote_basic/cryptonote_basic.h"
#include "cryptonote_basic/cryptonote_basic_impl.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "cryptonote_core/cryptonote_tx_utils.h"
#include "cryptonote_basic/tx_extra.h"
#include "cryptonote_core/blockchain.h"
#include "loki_economy.h"
#include "string_coding.h"

#include <lokimq/hex.h>

#include <sqlite3.h>

extern "C"
{
#include <sodium.h>
}

#undef LOKI_DEFAULT_LOG_CATEGORY
#define LOKI_DEFAULT_LOG_CATEGORY "lns"

namespace lns
{
enum struct lns_sql_type
{
  save_owner,
  save_setting,
  save_mapping,
  pruning,

  get_sentinel_start,
  get_mapping,
  get_mappings,
  get_mappings_by_owner,
  get_mappings_by_owners,
  get_mappings_on_height_and_newer,
  get_owner,
  get_setting,
  get_sentinel_end,
};

enum struct lns_db_setting_column
{
  id,
  top_height,
  top_hash,
  version,
};

enum struct owner_record_column
{
  id,
  public_key,
};

enum struct mapping_record_column
{
  id,
  type,
  name_hash,
  encrypted_value,
  txid,
  prev_txid,
  register_height,
  owner_id,
  backup_owner_id,
  _count,
};

static char const *mapping_record_column_string(mapping_record_column col)
{
  switch (col)
  {
    case mapping_record_column::id: return "id";
    case mapping_record_column::type: return "type";
    case mapping_record_column::name_hash: return "name_hash";
    case mapping_record_column::encrypted_value: return "encrypted_value";
    case mapping_record_column::txid: return "txid";
    case mapping_record_column::prev_txid: return "prev_txid";
    case mapping_record_column::register_height: return "register_height";
    case mapping_record_column::owner_id: return "owner_id";
    case mapping_record_column::backup_owner_id: return "backup_owner_id";
    default: return "xx_invalid";
  }
}

static std::ostream &operator<<(std::ostream &stream, cryptonote::tx_extra_loki_name_system const &data)
{
  stream << "LNS Extra={";
  if (data.is_buying())
  {
    stream << "owner=" << data.owner;
    if (data.backup_owner) stream << "backup_owner=" << data.backup_owner;
  }
  else
    stream << "signature=" << epee::string_tools::pod_to_hex(data.signature);

  stream << ", type=" << data.type << ", name_hash=" << data.name_hash << "}";
  return stream;
}

static bool sql_copy_blob(sqlite3_stmt *statement, int column, void *dest, int dest_size)
{
  void const *blob = sqlite3_column_blob(statement, column);
  int blob_len     = sqlite3_column_bytes(statement, column);
  if (blob_len != dest_size)
  {
    LOG_PRINT_L0("Unexpected blob size=" << blob_len << ", in LNS DB does not match expected size=" << dest_size);
    assert(blob_len == dest_size);
    return false;
  }

  memcpy(dest, blob, blob_len);
  return true;
}

static mapping_record sql_get_mapping_from_statement(sqlite3_stmt *statement)
{
  mapping_record result = {};
  int type_int = static_cast<uint16_t>(sqlite3_column_int(statement, static_cast<int>(mapping_record_column::type)));
  if (type_int >= tools::enum_count<mapping_type>)
    return result;

  result.type            = static_cast<mapping_type>(type_int);
  result.register_height = static_cast<uint16_t>(sqlite3_column_int(statement, static_cast<int>(mapping_record_column::register_height)));
  result.owner_id        = sqlite3_column_int(statement, static_cast<int>(mapping_record_column::owner_id));
  result.backup_owner_id = sqlite3_column_int(statement, static_cast<int>(mapping_record_column::backup_owner_id));

  // Copy encrypted_value
  {
    size_t value_len = static_cast<size_t>(sqlite3_column_bytes(statement, static_cast<int>(mapping_record_column::encrypted_value)));
    auto *value      = reinterpret_cast<char const *>(sqlite3_column_text(statement, static_cast<int>(mapping_record_column::encrypted_value)));
    if (value_len > result.encrypted_value.buffer.size())
    {
      MERROR("Unexpected encrypted value blob with size=" << value_len << ", in LNS db larger than the available size=" << result.encrypted_value.buffer.size());
      return result;
    }
    result.encrypted_value.len = value_len;
    memcpy(&result.encrypted_value.buffer[0], value, value_len);
  }

  // Copy name hash
  {
    size_t value_len = static_cast<size_t>(sqlite3_column_bytes(statement, static_cast<int>(mapping_record_column::name_hash)));
    auto *value      = reinterpret_cast<char const *>(sqlite3_column_text(statement, static_cast<int>(mapping_record_column::name_hash)));
    result.name_hash.append(value, value_len);
  }

  if (!sql_copy_blob(statement, static_cast<int>(mapping_record_column::txid), result.txid.data, sizeof(result.txid)))
    return result;

  if (!sql_copy_blob(statement, static_cast<int>(mapping_record_column::prev_txid), result.prev_txid.data, sizeof(result.prev_txid)))
    return result;

  int owner_column = tools::enum_count<mapping_record_column>;
  if (!sql_copy_blob(statement, owner_column, reinterpret_cast<void *>(&result.owner), sizeof(result.owner)))
    return result;

  if (result.backup_owner_id > 0)
  {
    if (!sql_copy_blob(statement, owner_column + 1, reinterpret_cast<void *>(&result.backup_owner), sizeof(result.backup_owner)))
      return result;
  }

  result.loaded = true;
  return result;
}

static bool sql_run_statement(cryptonote::network_type nettype, lns_sql_type type, sqlite3_stmt *statement, void *context)
{
  bool data_loaded = false;
  bool result      = false;

  for (bool infinite_loop = true; infinite_loop;)
  {
    int step_result = sqlite3_step(statement);
    switch (step_result)
    {
      case SQLITE_ROW:
      {
        switch (type)
        {
          default:
          {
            MERROR("Unhandled lns type enum with value: " << (int)type << ", in: " << __func__);
          }
          break;

          case lns_sql_type::get_owner:
          {
            auto *entry = reinterpret_cast<owner_record *>(context);
            entry->id   = sqlite3_column_int(statement, static_cast<int>(owner_record_column::id));
            if (!sql_copy_blob(statement, static_cast<int>(owner_record_column::public_key), reinterpret_cast<void *>(&entry->key), sizeof(entry->key)))
              return false;
            data_loaded = true;
          }
          break;

          case lns_sql_type::get_setting:
          {
            auto *entry       = reinterpret_cast<settings_record *>(context);
            entry->top_height = static_cast<uint64_t>(sqlite3_column_int64(statement, static_cast<int>(lns_db_setting_column::top_height)));
            if (!sql_copy_blob(statement, static_cast<int>(lns_db_setting_column::top_hash), entry->top_hash.data, sizeof(entry->top_hash.data)))
              return false;
            entry->version = sqlite3_column_int(statement, static_cast<int>(lns_db_setting_column::version));
            data_loaded = true;
          }
          break;

          case lns_sql_type::get_mappings_on_height_and_newer: /* FALLTHRU */
          case lns_sql_type::get_mappings_by_owners: /* FALLTHRU */
          case lns_sql_type::get_mappings_by_owner: /* FALLTHRU */
          case lns_sql_type::get_mappings: /* FALLTHRU */
          case lns_sql_type::get_mapping:
          {
            if (mapping_record tmp_entry = sql_get_mapping_from_statement(statement))
            {
              data_loaded = true;
              if (type == lns_sql_type::get_mapping)
              {
                auto *entry = reinterpret_cast<mapping_record *>(context);
                *entry      = std::move(tmp_entry);
              }
              else
              {
                auto *records = reinterpret_cast<std::vector<mapping_record> *>(context);
                records->emplace_back(std::move(tmp_entry));
              }
            }
          }
          break;
        }
      }
      break;

      case SQLITE_BUSY: break;
      case SQLITE_DONE:
      {
        infinite_loop = false;
        result        = (type > lns_sql_type::get_sentinel_start && type < lns_sql_type::get_sentinel_end) ? data_loaded : true;
        break;
      }

      default:
      {
        LOG_PRINT_L1("Failed to execute statement: " << sqlite3_sql(statement) <<", reason: " << sqlite3_errstr(step_result));
        infinite_loop = false;
        break;
      }
    }
  }

  sqlite3_reset(statement);
  sqlite3_clear_bindings(statement);
  return result;
}

bool mapping_record::active(cryptonote::network_type nettype, uint64_t blockchain_height) const
{
  if (!loaded) return false;
  uint64_t expiry_blocks = lns::expiry_blocks(nettype, static_cast<lns::mapping_type>(type));
  uint64_t const last_active_height = expiry_blocks == NO_EXPIRY ? NO_EXPIRY : (register_height + expiry_blocks);
  return last_active_height >= (blockchain_height - 1);
}

static bool sql_compile_statement(sqlite3 *db, char const *query, int query_len, sqlite3_stmt **statement, bool optimise_for_multiple_usage = true)
{
#if SQLITE_VERSION_NUMBER >= 3020000
  int prepare_result = sqlite3_prepare_v3(db, query, query_len, optimise_for_multiple_usage ? SQLITE_PREPARE_PERSISTENT : 0, statement, nullptr /*pzTail*/);
#else
  int prepare_result = sqlite3_prepare_v2(db, query, query_len, statement, nullptr /*pzTail*/);
#endif

  bool result        = prepare_result == SQLITE_OK;
  if (!result) MERROR("Can not compile SQL statement:\n" << query << "\nReason: " << sqlite3_errstr(prepare_result));
  return result;
}

sqlite3 *init_loki_name_system(char const *file_path)
{
  sqlite3 *result = nullptr;
  int sql_init    = sqlite3_initialize();
  if (sql_init != SQLITE_OK)
  {
    MERROR("Failed to initialize sqlite3: " << sqlite3_errstr(sql_init));
    return nullptr;
  }

  int sql_open = sqlite3_open(file_path, &result);
  if (sql_open != SQLITE_OK)
  {
    MERROR("Failed to open LNS db at: " << file_path << ", reason: " << sqlite3_errstr(sql_init));
    return nullptr;
  }

  return result;
}

uint64_t expiry_blocks(cryptonote::network_type nettype, mapping_type type, uint64_t *renew_window)
{
  uint64_t renew_window_ = 0;
  uint64_t result        = NO_EXPIRY;
  if (is_lokinet_type(type))
  {
    renew_window_ = BLOCKS_EXPECTED_IN_DAYS(31);

    if (type == mapping_type::lokinet_1year)        result = BLOCKS_EXPECTED_IN_YEARS(1);
    else if (type == mapping_type::lokinet_2years)  result = BLOCKS_EXPECTED_IN_YEARS(2);
    else if (type == mapping_type::lokinet_5years)  result = BLOCKS_EXPECTED_IN_YEARS(5);
    else if (type == mapping_type::lokinet_10years) result = BLOCKS_EXPECTED_IN_YEARS(10);

    result += renew_window_;
    if (nettype == cryptonote::FAKECHAIN)
    {
      renew_window_ = 10;
      result        = 10 + renew_window_;
    }
    else if (nettype == cryptonote::TESTNET)
    {
      renew_window_ = BLOCKS_EXPECTED_IN_DAYS(1);
      result        = BLOCKS_EXPECTED_IN_DAYS(1) + renew_window_;
    }
  }

  if (renew_window) *renew_window = renew_window_;
  return result;
}

crypto::hash tx_extra_signature_hash(epee::span<const uint8_t> value, crypto::generic_public_key const *owner, crypto::generic_public_key const *backup_owner, crypto::hash const &prev_txid)
{
  static_assert(sizeof(crypto::hash) == crypto_generichash_BYTES, "Using libsodium generichash for signature hash, require we fit into crypto::hash");
  crypto::hash result = {};
  if (value.size() > mapping_value::BUFFER_SIZE)
  {
    MERROR("Unexpected value len=" << value.size() << " greater than the expected capacity=" << mapping_value::BUFFER_SIZE);
    return result;
  }

  unsigned char buffer[mapping_value::BUFFER_SIZE + sizeof(*owner) + sizeof(*backup_owner) + sizeof(prev_txid)] = {};
  size_t buffer_len = value.size() + sizeof(prev_txid);
  if (owner)        buffer_len += sizeof(owner->data) + sizeof(owner->type);
  if (backup_owner) buffer_len += sizeof(backup_owner->data) + sizeof(backup_owner->type);

  unsigned char *ptr = buffer;
  memcpy(ptr, value.data(), value.size());
  ptr += value.size();

  if (owner)
  {
    memcpy(ptr, owner->data, sizeof(owner->data)); ptr += sizeof(owner->data);
    memcpy(ptr, reinterpret_cast<uint8_t const *>(&owner->type), sizeof(owner->type)); ptr += sizeof(owner->type);
  }

  if (backup_owner)
  {
    memcpy(ptr, backup_owner->data, sizeof(backup_owner->data)); ptr += sizeof(backup_owner->data);
    memcpy(ptr, reinterpret_cast<uint8_t const *>(&backup_owner->type), sizeof(backup_owner->type)); ptr += sizeof(backup_owner->type);
  }
  static_assert(sizeof(owner->type) == sizeof(char), "Require byte alignment to avoid unaligned access exceptions");

  crypto_generichash(reinterpret_cast<unsigned char *>(result.data), sizeof(result), buffer, buffer_len, NULL /*key*/, 0 /*key_len*/);
  return result;
}

crypto::generic_signature make_monero_signature(crypto::hash const &hash, crypto::public_key const &pkey, crypto::secret_key const &skey)
{
  crypto::generic_signature result = {};
  result.type                      = crypto::generic_key_sig_type::monero;
  generate_signature(hash, pkey, skey, result.monero);
  return result;
}

crypto::generic_signature make_ed25519_signature(crypto::hash const &hash, crypto::ed25519_secret_key const &skey)
{
  crypto::generic_signature result = {};
  result.type                      = crypto::generic_key_sig_type::ed25519;
  crypto_sign_detached(result.ed25519.data, NULL, reinterpret_cast<unsigned char const *>(hash.data), sizeof(hash), skey.data);
  return result;
}

crypto::generic_public_key make_monero_public_key(crypto::public_key const &pkey)
{
  crypto::generic_public_key result = {};
  result.type                       = crypto::generic_key_sig_type::monero;
  result.monero                     = pkey;
  return result;
}

crypto::generic_public_key make_ed25519_public_key(crypto::ed25519_public_key const &pkey)
{
  crypto::generic_public_key result = {};
  result.type                       = crypto::generic_key_sig_type::ed25519;
  result.ed25519                    = pkey;
  return result;
}

bool parse_owner_to_generic_key(cryptonote::network_type nettype, std::string const &owner, crypto::generic_public_key &key, std::string *reason)
{
  cryptonote::address_parse_info parsed_addr;
  crypto::ed25519_public_key ed_key;
  if (cryptonote::get_account_address_from_str(parsed_addr, nettype, owner))
  {
    key = lns::make_monero_public_key(parsed_addr.address.m_spend_public_key);
  }
  else if (epee::string_tools::hex_to_pod(owner, key))
  {
    key = lns::make_ed25519_public_key(ed_key);
  }
  else
  {
    if (reason)
    {
      char const *type_heuristic =
          (owner.size() == sizeof(crypto::ed25519_public_key) * 2) ? "ED25519 Key" : "Wallet address";
      *reason = type_heuristic;
      *reason += " provided could not be parsed owner=";
      *reason += key;
    }
    return false;
  }
  return true;
}


static bool char_is_num(char c)
{
  bool result = c >= '0' && c <= '9';
  return result;
}

static bool char_is_alpha(char c)
{
  bool result = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  return result;
}

static bool char_is_alphanum(char c)
{
  bool result = char_is_num(c) || char_is_alpha(c);
  return result;
}

template <typename... T>
static bool check_condition(bool condition, std::string* reason, T&&... args) {
  if (condition && reason)
  {
    std::ostringstream os;
#ifdef __cpp_fold_expressions // C++17
    (os << ... << std::forward<T>(args));
#else
    (void) std::initializer_list<int>{(os << std::forward<T>(args), 0)...};
#endif
    *reason = os.str();
  }
  return condition;
}

bool validate_lns_name(mapping_type type, std::string const &name, std::string *reason)
{
  std::stringstream err_stream;
  LOKI_DEFER { if (reason) *reason = err_stream.str(); };

  size_t max_name_len = 0;
  if (type == mapping_type::session)      max_name_len = lns::SESSION_DISPLAY_NAME_MAX;
  else if (type == mapping_type::wallet)  max_name_len = lns::WALLET_NAME_MAX;
  else if (is_lokinet_type(type))         max_name_len = lns::LOKINET_DOMAIN_NAME_MAX;
  else
  {
    if (reason) err_stream << "LNS type=" << type << ", specifies unhandled mapping type in name validation";
    return false;
  }

  // NOTE: Validate name length
  if (check_condition((name.empty() || name.size() > max_name_len), reason, "LNS type=", type, ", specifies mapping from name -> value where the name's length=", name.size(), " is 0 or exceeds the maximum length=", max_name_len, ", given name=", name))
    return false;

  // NOTE: Validate domain specific requirements
  if (type == mapping_type::session)
  {
  }
  else if (is_lokinet_type(type))
  {
    // Domain has to start with a letter or digit, and can have letters, digits, or hyphens in between and must end with a .loki
    // ^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\\.loki$
    {
      char const SHORTEST_DOMAIN[] = "a.loki";
      if (check_condition((name.size() < static_cast<int>(loki::char_count(SHORTEST_DOMAIN))), reason, "LNS type=lokinet, specifies mapping from name -> value where the name is shorter than the shortest possible name=", SHORTEST_DOMAIN, ", given name=", name))
        return false;
    }

    // Must start with alphanumeric
    if (check_condition(!char_is_alphanum(name[0]), reason, "LNS type=lokinet, specifies mapping from name -> value where the name does not start with an alphanumeric character, name=", name))
      return false;

    // Must end with .loki
    char const SUFFIX[]     = ".loki";
    char const *name_suffix = name.data() + (name.size() - loki::char_count(SUFFIX));
    if (check_condition((memcmp(name_suffix, SUFFIX, loki::char_count(SUFFIX)) != 0), reason, "LNS type=lokinet, specifies mapping from name -> value where the name does not end with the domain .loki, name=", name))
      return false;

    // Characted preceeding suffix must be alphanumeric
    char const *char_preceeding_suffix = name_suffix - 1;
    if (check_condition(!char_is_alphanum(char_preceeding_suffix[0]), reason, "LNS type=lokinet, specifies mapping from name -> value where the character preceeding the <char>.loki is not alphanumeric, char=", char_preceeding_suffix[0], ", name=", name))
      return false;

    char const *begin = name.data() + 1;
    char const *end   = char_preceeding_suffix;
    for (char const *it = begin; it < end; it++) // Inbetween start and preceeding suffix, alphanumeric and hyphen characters permitted
    {
      char c = it[0];
      if (check_condition(!(char_is_alphanum(c) || c == '-'), reason, "LNS type=lokinet, specifies mapping from name -> value where the domain name contains more than the permitted alphanumeric or hyphen characters name=", name))
        return false;
    }
  }

  return true;
}

static bool check_lengths(mapping_type type, std::string const &value, size_t max, bool binary_val, std::string *reason)
{
  bool result = (value.size() == max);
  if (!result)
  {
    if (reason)
    {
      std::stringstream err_stream;
      err_stream << "LNS type=" << type << ", specifies mapping from name_hash->encrypted_value where the value's length=" << value.size() << ", does not equal the required length=" << max << ", given value=";
      if (binary_val) err_stream << epee::to_hex::string(epee::span<const uint8_t>(reinterpret_cast<uint8_t const *>(value.data()), value.size()));
      else            err_stream << value;
      *reason = err_stream.str();
    }
  }

  return result;
}

bool validate_mapping_value(cryptonote::network_type nettype, mapping_type type, std::string const &value, mapping_value *blob, std::string *reason)
{
  if (blob) *blob = {};

  // Check length of the value
  std::stringstream err_stream;
  cryptonote::address_parse_info addr_info = {};
  if (type == mapping_type::wallet)
  {
    if (value.empty() || !get_account_address_from_str(addr_info, nettype, value))
    {
      if (reason)
      {
        if (value.empty())
        {
          err_stream << "The value=" << value;
          err_stream << ", mapping into the wallet address, specifies a wallet address of 0 length";
        }
        else
        {
          err_stream << "Could not convert the wallet address string, check it is correct, value=" << value;
        }
        *reason = err_stream.str();
      }
      return false;
    }
  }
  else
  {
    int max_value_len = 0;
    if (is_lokinet_type(type))              max_value_len = (LOKINET_ADDRESS_BINARY_LENGTH * 2);
    else if (type == mapping_type::session) max_value_len = (SESSION_PUBLIC_KEY_BINARY_LENGTH * 2);
    else
    {
      if (reason)
      {
        err_stream << "Unhandled type passed into: " << __func__;
        *reason = err_stream.str();
      }
      return false;
    }

    if (!check_lengths(type, value, max_value_len, false /*binary_val*/, reason))
      return false;
  }

  // Validate blob contents and generate the binary form if possible
  if (type == mapping_type::wallet)
  {
    if (blob)
    {
      blob->len = sizeof(addr_info.address);
      memcpy(blob->buffer.data(), &addr_info.address, blob->len);
    }
  }
  else if (is_lokinet_type(type))
  {
    if (check_condition(value.size() != 52, reason, "The lokinet value=", value, ", should be a 52 char base32z string, length=", value.size()))
      return false;

    crypto::ed25519_public_key pkey;
    if (check_condition(!base32z::decode(value, pkey), reason, "The value=", value, ", was not a decodable base32z value."))
      return false;

    if (blob)
    {
      blob->len = sizeof(pkey);
      memcpy(blob->buffer.data(), pkey.data, blob->len);
    }
  }
  else
  {
    assert(type == mapping_type::session);
    // NOTE: Check value is hex
    if (check_condition((value.size() % 2) != 0, reason, "The value=", value, ", should be a hex string that has an even length to be convertible back into binary, length=", value.size()))
      return false;

    if (check_condition(!lokimq::is_hex(value), reason, ", specifies name -> value mapping where the value is not a hex string given value="))
      return false;

    if (blob) // NOTE: Given blob, write the binary output
    {
      blob->len = value.size() / 2;
      assert(blob->len <= blob->buffer.size());
      lokimq::from_hex(value.begin(), value.end(), blob->buffer.begin());
    }

    // NOTE: Session public keys are 33 bytes, with the first byte being 0x05 and the remaining 32 being the public key.
    if (check_condition(!(value[0] == '0' && value[1] == '5'), reason, "LNS type=session, specifies mapping from name -> ed25519 key where the key is not prefixed with 53 (0x05), prefix=", std::to_string(value[0]), " (", value[0], "), given ed25519=", value))
      return false;
  }

  return true;
}

bool validate_encrypted_mapping_value(mapping_type type, std::string const &value, std::string *reason)
{
  std::stringstream err_stream;
  int max_value_len = crypto_secretbox_MACBYTES;
  if (is_lokinet_type(type)) max_value_len              += LOKINET_ADDRESS_BINARY_LENGTH;
  else if (type == mapping_type::session) max_value_len += SESSION_PUBLIC_KEY_BINARY_LENGTH;
  else if (type == mapping_type::wallet)  max_value_len += WALLET_ACCOUNT_BINARY_LENGTH;
  else
  {
    if (reason)
    {
      err_stream << "Unhandled type passed into " << __func__;
      *reason = err_stream.str();
    }
    return false;
  }

  if (!check_lengths(type, value, max_value_len, true /*binary_val*/, reason))
    return false;
  return true;
}

static std::string hash_to_base64(crypto::hash const &hash)
{
  std::string result = epee::string_encoding::base64_encode(reinterpret_cast<unsigned char const *>(hash.data), sizeof(hash));
  return result;
}

static bool verify_lns_signature(crypto::hash const &hash, crypto::generic_signature const &signature, crypto::generic_public_key const &key)
{
  if (!key) return false;
  if (key.type != signature.type) return false;
  if (signature.type == crypto::generic_key_sig_type::monero)
  {
    return crypto::check_signature(hash, key.monero, signature.monero);
  }
  else
  {
    return (crypto_sign_verify_detached(signature.data, reinterpret_cast<unsigned char const *>(hash.data), sizeof(hash.data), key.ed25519.data) == 0);
  }
}

static bool validate_against_previous_mapping(lns::name_system_db const &lns_db, uint64_t blockchain_height, cryptonote::transaction const &tx, cryptonote::tx_extra_loki_name_system const &lns_extra, std::string *reason = nullptr)
{
  std::stringstream err_stream;
  LOKI_DEFER { if (reason && reason->empty()) *reason = err_stream.str(); };

  crypto::hash expected_prev_txid = crypto::null_hash;
  std::string name_hash           = hash_to_base64(lns_extra.name_hash);
  lns::mapping_record mapping     = lns_db.get_mapping(lns_extra.type, name_hash);

  if (check_condition(lns_extra.is_updating() && !mapping, reason, tx, ", ", lns_extra, " update requested but mapping does not exist."))
    return false;

  if (mapping)
  {
    expected_prev_txid = mapping.txid;
    if (lns_extra.is_updating())
    {
      if (check_condition(is_lokinet_type(lns_extra.type) && !mapping.active(lns_db.network_type(), blockchain_height), reason, tx, ", ", lns_extra, " TX requested to update mapping that has already expired"))
        return false;

      auto span_a = epee::strspan<uint8_t>(lns_extra.encrypted_value);
      auto span_b = mapping.encrypted_value.to_span();
      char const SPECIFYING_SAME_VALUE_ERR[] = " field to update is specifying the same mapping ";
      if (check_condition(lns_extra.field_is_set(lns::extra_field::encrypted_value) && (span_a.size() == span_b.size() && memcmp(span_a.data(), span_b.data(), span_a.size()) == 0), reason, tx, ", ", lns_extra, SPECIFYING_SAME_VALUE_ERR, "value"))
        return false;

      if (check_condition(lns_extra.field_is_set(lns::extra_field::owner) && lns_extra.owner == mapping.owner, reason, tx, ", ", lns_extra, SPECIFYING_SAME_VALUE_ERR, "owner"))
        return false;

      if (check_condition(lns_extra.field_is_set(lns::extra_field::backup_owner) && lns_extra.backup_owner == mapping.backup_owner, reason, tx, ", ", lns_extra, SPECIFYING_SAME_VALUE_ERR, "backup_owner"))
        return false;

      // Validate signature
      {
        auto value = epee::strspan<uint8_t>(lns_extra.encrypted_value);
        crypto::hash hash = tx_extra_signature_hash(value,
                                                    lns_extra.field_is_set(lns::extra_field::owner) ? &lns_extra.owner : nullptr,
                                                    lns_extra.field_is_set(lns::extra_field::backup_owner) ? &lns_extra.backup_owner : nullptr,
                                                    expected_prev_txid);
        if (check_condition(!verify_lns_signature(hash, lns_extra.signature, mapping.owner) &&
                            !verify_lns_signature(hash, lns_extra.signature, mapping.backup_owner), reason,
                            tx, ", ", lns_extra, " failed to verify signature for LNS update, current owner=", mapping.owner, ", backup owner=", mapping.backup_owner))
        {
          return false;
        }
      }
    }
    else
    {
      if (!is_lokinet_type(lns_extra.type))
      {
        lns::owner_record owner = lns_db.get_owner_by_id(mapping.owner_id);
        if (check_condition(true, reason, tx, ", ", lns_extra, " non-lokinet entries can NOT be renewed, mapping already exists with name_hash=", mapping.name_hash, ", owner=", owner.key, ", type=", mapping.type))
          return false;
      }

      if (check_condition(!(lns_extra.field_is_set(lns::extra_field::buy) || lns_extra.field_is_set(lns::extra_field::buy_no_backup)), reason,
                          " TX is buying mapping but serialized unexpected fields not relevant for buying"))
      {
        return false;
      }

      uint64_t renew_window              = 0;
      uint64_t expiry_blocks             = lns::expiry_blocks(lns_db.network_type(), lns_extra.type, &renew_window);
      uint64_t const renew_window_offset = expiry_blocks - renew_window;
      uint64_t const min_renew_height    = mapping.register_height + renew_window_offset;

      if (check_condition(min_renew_height >= blockchain_height, reason, tx, ", ", lns_extra, " trying to renew too early, the earliest renew height=", min_renew_height, ", current height=", blockchain_height))
          return false;

      if (mapping.active(lns_db.network_type(), blockchain_height))
      {
        // Lokinet entry expired i.e. it's no longer active. A purchase for this name is valid
        // Check that the request originates from the owner of this mapping
        lns::owner_record const requester = lns_db.get_owner_by_key(lns_extra.owner);
        if (check_condition(requester, reason, tx, ", ", lns_extra, " trying to renew existing mapping but owner specified in LNS extra does not exist, rejected"))
          return false;

        lns::owner_record const owner = lns_db.get_owner_by_id(mapping.owner_id);
        if (check_condition(owner, reason, tx, ", ", lns_extra, " unexpected owner_id=", mapping.owner_id, " does not exist"))
          return false;

        if (check_condition(requester.id != owner.id, reason, tx, ", ", lns_extra, " actual owner=",  owner.key, ", with owner_id=", mapping.owner_id, ", does not match requester=", requester.key, ", with id=", requester.id))
          return false;

      }
      // else mapping has expired, new purchase is valid
    }
  }

  if (check_condition(lns_extra.prev_txid != expected_prev_txid, reason, tx, ", ", lns_extra, " specified prior txid=", lns_extra.prev_txid, ", but LNS DB reports=", expected_prev_txid, ", possible competing TX was submitted and accepted before this TX was processed"))
    return false;

  return true;
}

bool name_system_db::validate_lns_tx(uint8_t hf_version, uint64_t blockchain_height, cryptonote::transaction const &tx, cryptonote::tx_extra_loki_name_system *lns_extra, std::string *reason) const
{
  // -----------------------------------------------------------------------------------------------
  // Pull out LNS Extra from TX
  // -----------------------------------------------------------------------------------------------
  cryptonote::tx_extra_loki_name_system lns_extra_;
  {
    if (!lns_extra) lns_extra = &lns_extra_;
    if (check_condition(tx.type != cryptonote::txtype::loki_name_system, reason, tx, ", uses wrong tx type, expected=", cryptonote::txtype::loki_name_system))
      return false;

    if (check_condition(!cryptonote::get_loki_name_system_from_tx_extra(tx.extra, *lns_extra), reason, tx, ", didn't have loki name service in the tx_extra"))
      return false;
  }


  // -----------------------------------------------------------------------------------------------
  // Check TX LNS Serialized Fields are NULL if they are not specified
  // -----------------------------------------------------------------------------------------------
  {
    char const VALUE_SPECIFIED_BUT_NOT_REQUESTED[] = ", given field but field is not requested to be serialised=";
    if (check_condition(!lns_extra->field_is_set(lns::extra_field::encrypted_value) && lns_extra->encrypted_value.size(), reason, tx, ", ", *lns_extra, VALUE_SPECIFIED_BUT_NOT_REQUESTED, "encrypted_value"))
      return false;

    if (check_condition(!lns_extra->field_is_set(lns::extra_field::owner) && lns_extra->owner, reason, tx, ", ", *lns_extra, VALUE_SPECIFIED_BUT_NOT_REQUESTED, "owner"))
      return false;

    if (check_condition(!lns_extra->field_is_set(lns::extra_field::backup_owner) && lns_extra->backup_owner, reason, tx, ", ", *lns_extra, VALUE_SPECIFIED_BUT_NOT_REQUESTED, "backup_owner"))
      return false;

    if (check_condition(!lns_extra->field_is_set(lns::extra_field::signature) && lns_extra->signature, reason, tx, ", ", *lns_extra, VALUE_SPECIFIED_BUT_NOT_REQUESTED, "signature"))
      return false;
  }

  // -----------------------------------------------------------------------------------------------
  // Simple LNS Extra Validation
  // -----------------------------------------------------------------------------------------------
  {
    if (check_condition(lns_extra->version != 0, reason, tx, ", ", *lns_extra, " unexpected version=", std::to_string(lns_extra->version), ", expected=0"))
      return false;

    if (check_condition(!lns::mapping_type_allowed(hf_version, lns_extra->type), reason, tx, ", ", *lns_extra, " specifying type=", lns_extra->type, " that is disallowed"))
      return false;

    // -----------------------------------------------------------------------------------------------
    // Serialized Values Check
    // -----------------------------------------------------------------------------------------------
    if (check_condition(!lns_extra->is_buying() && !lns_extra->is_updating(), reason, tx, ", ", *lns_extra, " TX extra does not specify valid combination of bits for serialized fields=", std::bitset<sizeof(lns_extra->fields) * 8>(static_cast<size_t>(lns_extra->fields)).to_string()))
      return false;

    if (check_condition(lns_extra->field_is_set(lns::extra_field::owner) &&
                        lns_extra->field_is_set(lns::extra_field::backup_owner) &&
                        lns_extra->owner == lns_extra->backup_owner,
                        reason, tx, ", ", *lns_extra, " specifying owner the same as the backup owner=", lns_extra->backup_owner))
    {
      return false;
    }
   }

  // -----------------------------------------------------------------------------------------------
  // LNS Field(s) Validation
  // -----------------------------------------------------------------------------------------------
  {
    static const crypto::hash null_name_hash = name_to_hash(""); // Sanity check the empty name hash
    if (check_condition((lns_extra->name_hash == null_name_hash || lns_extra->name_hash == crypto::null_hash), reason, tx, ", ", *lns_extra, " specified the null name hash"))
        return false;

    if (lns_extra->field_is_set(lns::extra_field::encrypted_value))
    {
      if (!validate_encrypted_mapping_value(lns_extra->type, lns_extra->encrypted_value, reason))
        return false;
    }

    if (!validate_against_previous_mapping(*this, blockchain_height, tx, *lns_extra, reason))
      return false;
  }

  // -----------------------------------------------------------------------------------------------
  // Burn Validation
  // -----------------------------------------------------------------------------------------------
  {
    uint64_t burn                = cryptonote::get_burned_amount_from_tx_extra(tx.extra);
    uint64_t const burn_required = lns_extra->is_buying() ? burn_needed(hf_version, lns_extra->type) : 0;
    if (burn != burn_required)
    {
      char const *over_or_under = burn > burn_required ? "too much " : "insufficient ";
      if (check_condition(true, reason, tx, ", ", *lns_extra, " burned ", over_or_under, "loki=", burn, ", require=", burn_required))
        return false;
    }
  }

  return true;
}

static std::string lowercase_string(std::string src)
{
  for (char &ch : src)
  {
    if (ch >= 'A' && ch <= 'Z')
      ch = ch + ('a' - 'A');
  }
  return src;
}

bool validate_mapping_type(std::string const &mapping_type_str, lns::mapping_type *mapping_type, std::string *reason)
{
  std::string mapping = lowercase_string(mapping_type_str);

  lns::mapping_type mapping_type_;
  if (mapping == "session") mapping_type_ = lns::mapping_type::session;
  else
  {
    if (reason) *reason = "Failed to convert lns mapping (was not proper integer, or not one of the recognised: \"session\"), string was=" + mapping_type_str;
    return false;
  }

  if (mapping_type) *mapping_type = static_cast<lns::mapping_type>(mapping_type_);
  return true;
}

crypto::hash name_to_hash(std::string const &name)
{
  crypto::hash result = {};
  static_assert(sizeof(result) >= crypto_generichash_BYTES, "Sodium can generate arbitrary length hashes, but recommend the minimum size for a secure hash must be >= crypto_generichash_BYTES");
  crypto_generichash_blake2b(reinterpret_cast<unsigned char *>(result.data),
                             sizeof(result),
                             reinterpret_cast<const unsigned char *>(name.data()),
                             static_cast<unsigned long long>(name.size()),
                             nullptr /*key*/,
                             0 /*keylen*/);
  return result;
}

std::string name_to_base64_hash(std::string const &name)
{
  crypto::hash hash  = name_to_hash(name);
  std::string result = hash_to_base64(hash);
  return result;
}

struct alignas(size_t) secretbox_secret_key_ { unsigned char data[crypto_secretbox_KEYBYTES]; };
using secretbox_secret_key = epee::mlocked<tools::scrubbed<secretbox_secret_key_>>;

static bool name_to_encryption_key(std::string const &name, secretbox_secret_key &out)
{
  static_assert(sizeof(out) >= crypto_secretbox_KEYBYTES, "Encrypting key needs to have sufficient space for running encryption functions via libsodium");
  static unsigned char constexpr SALT[crypto_pwhash_SALTBYTES] = {};
  bool result = (crypto_pwhash(out.data, sizeof(out), name.data(), name.size(), SALT, crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE, crypto_pwhash_ALG_ARGON2ID13) == 0);
  return result;
}

static unsigned char const ENCRYPTION_NONCE[crypto_secretbox_NONCEBYTES] = {}; // NOTE: Not meant to be extremely secure, just use an empty nonce
bool encrypt_mapping_value(std::string const &name, mapping_value const &value, mapping_value &encrypted_value)
{
  static_assert(mapping_value::BUFFER_SIZE >= SESSION_PUBLIC_KEY_BINARY_LENGTH + crypto_secretbox_MACBYTES, "Value blob assumes the largest size required, all other values should be able to fit into this buffer");
  static_assert(mapping_value::BUFFER_SIZE >= LOKINET_ADDRESS_BINARY_LENGTH    + crypto_secretbox_MACBYTES, "Value blob assumes the largest size required, all other values should be able to fit into this buffer");
  static_assert(mapping_value::BUFFER_SIZE >= WALLET_ACCOUNT_BINARY_LENGTH     + crypto_secretbox_MACBYTES, "Value blob assumes the largest size required, all other values should be able to fit into this buffer");

  bool result                 = false;
  size_t const encryption_len = value.len + crypto_secretbox_MACBYTES;
  if (encryption_len > encrypted_value.buffer.size())
  {
    MERROR("Encrypted value pre-allocated buffer too small=" << encrypted_value.buffer.size() << ", required=" << encryption_len);
    return result;
  }

  encrypted_value     = {};
  encrypted_value.len = encryption_len;

  secretbox_secret_key skey;
  if (name_to_encryption_key(name, skey))
    result = (crypto_secretbox_easy(encrypted_value.buffer.data(), value.buffer.data(), value.len, ENCRYPTION_NONCE, reinterpret_cast<unsigned char *>(&skey)) == 0);
  return result;
}

bool decrypt_mapping_value(std::string const &name, mapping_value const &encrypted_value, mapping_value &value)
{
  bool result = false;
  if (encrypted_value.len <= crypto_secretbox_MACBYTES)
  {
    MERROR("Encrypted value is too short=" << encrypted_value.len << ", at least required=" << crypto_secretbox_MACBYTES + 1);
    return result;
  }

  value     = {};
  value.len = encrypted_value.len - crypto_secretbox_MACBYTES;

  secretbox_secret_key skey;
  if (name_to_encryption_key(name, skey))
    result = crypto_secretbox_open_easy(value.buffer.data(), encrypted_value.buffer.data(), encrypted_value.len, ENCRYPTION_NONCE, reinterpret_cast<unsigned char *>(&skey)) == 0;
  return result;
}


static bool build_default_tables(sqlite3 *db)
{
  constexpr char BUILD_TABLE_SQL[] = R"(
CREATE TABLE IF NOT EXISTS "owner"(
    "id" INTEGER PRIMARY KEY AUTOINCREMENT,
    "public_key" BLOB NOT NULL UNIQUE
);

CREATE TABLE IF NOT EXISTS "settings" (
    "id" INTEGER PRIMARY KEY NOT NULL,
    "top_height" INTEGER NOT NULL,
    "top_hash" VARCHAR NOT NULL,
    "version" INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS "mappings" (
    "id" INTEGER PRIMARY KEY NOT NULL,
    "type" INTEGER NOT NULL,
    "name_hash" VARCHAR NOT NULL,
    "encrypted_value" BLOB NOT NULL,
    "txid" BLOB NOT NULL,
    "prev_txid" BLOB NOT NULL,
    "register_height" INTEGER NOT NULL,
    "owner_id" INTEGER NOT NULL REFERENCES "owner" ("id"),
    "backup_owner_id" INTEGER REFERENCES "owner" ("id")
);
CREATE UNIQUE INDEX IF NOT EXISTS "name_hash_type_id" ON mappings("name_hash", "type");
CREATE INDEX IF NOT EXISTS "owner_id_index" ON mappings("owner_id");
CREATE INDEX IF NOT EXISTS "backup_owner_id_index" ON mappings("backup_owner_index");
)";

  char *table_err_msg = nullptr;
  int table_created   = sqlite3_exec(db, BUILD_TABLE_SQL, nullptr /*callback*/, nullptr /*callback context*/, &table_err_msg);
  if (table_created != SQLITE_OK)
  {
    MERROR("Can not generate SQL table for LNS: " << (table_err_msg ? table_err_msg : "??"));
    sqlite3_free(table_err_msg);
    return false;
  }

  return true;
}

static std::string sql_cmd_combine_mappings_and_owner_table(char const *suffix)
{
  std::stringstream stream;
  stream <<
R"(SELECT "mappings".*, "o1"."public_key", "o2"."public_key" FROM "mappings"
JOIN "owner" "o1" ON "mappings"."owner_id" = "o1"."id"
LEFT JOIN "owner" "o2" ON "mappings"."backup_owner_id" = "o2"."id")" << "\n";

  if (suffix)
    stream << suffix;
  return stream.str();
}

enum struct db_version { v1, };
auto constexpr DB_VERSION = db_version::v1;
bool name_system_db::init(cryptonote::network_type nettype, sqlite3 *db, uint64_t top_height, crypto::hash const &top_hash)
{
  if (!db) return false;
  this->db      = db;
  this->nettype = nettype;

  std::string const get_mappings_by_owner_str            = sql_cmd_combine_mappings_and_owner_table(R"(WHERE ? IN ("o1"."public_key", "o2"."public_key"))");
  std::string const get_mappings_on_height_and_newer_str = sql_cmd_combine_mappings_and_owner_table(R"(WHERE "register_height" >= ?)");
  std::string const get_mapping_str                      = sql_cmd_combine_mappings_and_owner_table(R"(WHERE "type" = ? AND "name_hash" = ?)");

  char constexpr GET_OWNER_BY_ID_STR[]  = R"(SELECT * FROM "owner" WHERE "id" = ?)";
  char constexpr GET_OWNER_BY_KEY_STR[] = R"(SELECT * FROM "owner" WHERE "public_key" = ?)";
  char constexpr GET_SETTINGS_STR[]     = R"(SELECT * FROM "settings" WHERE "id" = 1)";
  char constexpr PRUNE_MAPPINGS_STR[]   = R"(DELETE FROM "mappings" WHERE "register_height" >= ?)";

  char constexpr PRUNE_OWNERS_STR[] =
R"(DELETE FROM "owner"
WHERE NOT EXISTS (SELECT * FROM "mappings" WHERE "owner"."id" = "mappings"."owner_id")
AND NOT EXISTS   (SELECT * FROM "mappings" WHERE "owner"."id" = "mappings"."backup_owner_id"))";

  char constexpr SAVE_MAPPING_STR[]     = R"(INSERT OR REPLACE INTO "mappings" ("type", "name_hash", "encrypted_value", "txid", "prev_txid", "register_height", "owner_id", "backup_owner_id") VALUES (?,?,?,?,?,?,?,?))";
  char constexpr SAVE_OWNER_STR[]       = R"(INSERT INTO "owner" ("public_key") VALUES (?))";
  char constexpr SAVE_SETTINGS_STR[]    = R"(INSERT OR REPLACE INTO "settings" ("id", "top_height", "top_hash", "version") VALUES (1,?,?,?))";

  sqlite3_stmt *test;

  if (!build_default_tables(db))
    return false;

  if (
      !sql_compile_statement(db, get_mappings_by_owner_str.c_str(),            get_mappings_by_owner_str.size(),            &get_mappings_by_owner_sql) ||
      !sql_compile_statement(db, get_mappings_on_height_and_newer_str.c_str(), get_mappings_on_height_and_newer_str.size(), &get_mappings_on_height_and_newer_sql) ||
      !sql_compile_statement(db, get_mapping_str.c_str(),                      get_mapping_str.size(),                      &get_mapping_sql) ||
      !sql_compile_statement(db, GET_SETTINGS_STR,                             loki::array_count(GET_SETTINGS_STR),         &get_settings_sql) ||
      !sql_compile_statement(db, GET_OWNER_BY_ID_STR,                          loki::array_count(GET_OWNER_BY_ID_STR),      &get_owner_by_id_sql) ||
      !sql_compile_statement(db, GET_OWNER_BY_KEY_STR,                         loki::array_count(GET_OWNER_BY_KEY_STR),     &get_owner_by_key_sql) ||
      !sql_compile_statement(db, PRUNE_MAPPINGS_STR,                           loki::array_count(PRUNE_MAPPINGS_STR),       &prune_mappings_sql) ||
      !sql_compile_statement(db, PRUNE_OWNERS_STR,                             loki::array_count(PRUNE_OWNERS_STR),         &prune_owners_sql) ||
      !sql_compile_statement(db, SAVE_MAPPING_STR,                             loki::array_count(SAVE_MAPPING_STR),         &save_mapping_sql) ||
      !sql_compile_statement(db, SAVE_SETTINGS_STR,                            loki::array_count(SAVE_SETTINGS_STR),        &save_settings_sql) ||
      !sql_compile_statement(db, SAVE_OWNER_STR,                               loki::array_count(SAVE_OWNER_STR),           &save_owner_sql)
    )
  {
    return false;
  }

  if (settings_record settings = get_settings())
  {
    if (settings.top_height == top_height && settings.top_hash == top_hash)
    {
      this->last_processed_height = settings.top_height;
      assert(settings.version == static_cast<int>(DB_VERSION));
    }
    else
    {
      char constexpr DROP_TABLE_SQL[] = R"(DROP TABLE IF EXISTS "owner"; DROP TABLE IF EXISTS "settings"; DROP TABLE IF EXISTS "mappings")";
      sqlite3_exec(db, DROP_TABLE_SQL, nullptr /*callback*/, nullptr /*callback context*/, nullptr);
      if (!build_default_tables(db)) return false;
    }
  }

  return true;
}

struct scoped_db_transaction
{
  scoped_db_transaction(name_system_db &lns_db);
  ~scoped_db_transaction();
  operator bool() const { return initialised; }
  name_system_db &lns_db;
  bool commit      = false; // If true, on destruction- END the transaction otherwise ROLLBACK all SQLite events prior for the lns_db
  bool initialised = false;
};

scoped_db_transaction::scoped_db_transaction(name_system_db &lns_db)
: lns_db(lns_db)
{
  if (lns_db.transaction_begun)
  {
    MERROR("Failed to begin transaction, transaction exists previously that was not closed properly");
    return;
  }

  char *sql_err = nullptr;
  if (sqlite3_exec(lns_db.db, "BEGIN;", nullptr, nullptr, &sql_err) != SQLITE_OK)
  {
    MERROR("Failed to begin transaction " << ", reason=" << (sql_err ? sql_err : "??"));
    sqlite3_free(sql_err);
    return;
  }

  initialised              = true;
  lns_db.transaction_begun = true;
}

scoped_db_transaction::~scoped_db_transaction()
{
  if (!initialised) return;
  if (!lns_db.transaction_begun)
  {
    MERROR("Trying to apply non-existent transaction (no prior history of a db transaction beginning) to the LNS DB");
    return;
  }

  char *sql_err = nullptr;
  if (sqlite3_exec(lns_db.db, commit ? "END;" : "ROLLBACK;", NULL, NULL, &sql_err) != SQLITE_OK)
  {
    MERROR("Failed to " << (commit ? "end " : "rollback ") << " transaction to LNS DB, reason=" << (sql_err ? sql_err : "??"));
    sqlite3_free(sql_err);
    return;
  }

  lns_db.transaction_begun = false;
}

static int64_t add_or_get_owner_id(lns::name_system_db &lns_db, crypto::hash const &tx_hash, cryptonote::tx_extra_loki_name_system const &entry, crypto::generic_public_key const &key)
{
  int64_t result = 0;
  if (owner_record owner = lns_db.get_owner_by_key(key)) result = owner.id;
  if (result == 0)
  {
    if (!lns_db.save_owner(key, &result))
    {
      LOG_PRINT_L1("Failed to save LNS owner to DB tx: " << tx_hash << ", type: " << entry.type << ", name_hash: " << entry.name_hash << ", owner: " << entry.owner);
      return result;
    }
  }
  return result;
}

static bool add_lns_entry(lns::name_system_db &lns_db, uint64_t height, cryptonote::tx_extra_loki_name_system const &entry, crypto::hash const &tx_hash)
{
  // -----------------------------------------------------------------------------------------------
  // New Mapping Insert or Completely Replace
  // -----------------------------------------------------------------------------------------------
  if (entry.is_buying())
  {
    int64_t owner_id = add_or_get_owner_id(lns_db, tx_hash, entry, entry.owner);
    if (owner_id == 0)
    {
      MERROR("Failed to add or get owner with key=" << entry.owner);
      assert(owner_id != 0);
      return false;
    }

    int64_t backup_owner_id = 0;
    if (entry.backup_owner)
    {
      backup_owner_id = add_or_get_owner_id(lns_db, tx_hash, entry, entry.backup_owner);
      if (backup_owner_id == 0)
      {
        MERROR("Failed to add or get backup owner with key=" << entry.backup_owner);
        assert(backup_owner_id != 0);
        return false;
      }
    }

    if (!lns_db.save_mapping(tx_hash, entry, height, owner_id, backup_owner_id))
    {
      LOG_PRINT_L1("Failed to save LNS entry to DB tx: " << tx_hash << ", type: " << entry.type << ", name_hash: " << entry.name_hash << ", owner: " << entry.owner);
      return false;
    }
  }
  // -----------------------------------------------------------------------------------------------
  // Update Mapping, do a SQL command of the type
  // UPDATE "mappings" SET <field> = entry.<field>, ...  WHERE type = entry.type AND name_hash = name_hash_base64
  // -----------------------------------------------------------------------------------------------
  else
  {
    // Generate the SQL command
    std::string sql_statement;
    int64_t owner_id        = 0;
    int64_t backup_owner_id = 0;
    size_t column_count     = 0;
    std::array<mapping_record_column, tools::enum_count<mapping_record_column>> columns; // Enumerate the columns we're going to update
    {
      columns[column_count++] = mapping_record_column::prev_txid;
      columns[column_count++] = mapping_record_column::txid;

      if (entry.field_is_set(lns::extra_field::owner))
      {
        columns[column_count++] = mapping_record_column::owner_id;
        owner_id                = add_or_get_owner_id(lns_db, tx_hash, entry, entry.owner);
        if (owner_id == 0)
        {
          MERROR("Failed to add or get owner with key=" << entry.owner);
          assert(owner_id != 0);
          return false;
        }
      }

      if (entry.field_is_set(lns::extra_field::backup_owner))
      {
        columns[column_count++] = mapping_record_column::backup_owner_id;
        backup_owner_id         = add_or_get_owner_id(lns_db, tx_hash, entry, entry.backup_owner);
        if (backup_owner_id == 0)
        {
          MERROR("Failed to add or get backup owner with key=" << entry.backup_owner);
          assert(backup_owner_id != 0);
          return false;
        }
      }

      if (entry.field_is_set(lns::extra_field::encrypted_value))
        columns[column_count++] = mapping_record_column::encrypted_value;

      // Build the SQL statement
      std::stringstream stream;
      stream << R"(UPDATE "mappings" SET )";
      for (size_t i = 0; i < column_count; i++)
      {
        if (i) stream << ", ";
        auto column_type = columns[i];
        stream << "\"" << mapping_record_column_string(column_type) << "\" = ?";
      }

      columns[column_count++] = mapping_record_column::type;
      columns[column_count++] = mapping_record_column::name_hash;
      stream << R"( WHERE "type" = ? AND "name_hash" = ?)";
      sql_statement = stream.str();
    }

    // Compile sql statement && bind parameters to statement
    std::string const name_hash   = hash_to_base64(entry.name_hash);
    sqlite3_stmt *statement = nullptr;
    {
      if (!sql_compile_statement(lns_db.db, sql_statement.c_str(), sql_statement.size(), &statement, false /*optimise_for_multiple_usage*/))
      {
        MERROR("Failed to compile SQL statement for updating LNS record=" << sql_statement);
        return false;
      }

      // Bind step
      int sql_param_index = 1;
      for (size_t i = 0; i < column_count; i++)
      {
        auto column_type = columns[i];
        switch (column_type)
        {
          case mapping_record_column::type:            sqlite3_bind_int  (statement, sql_param_index++, static_cast<uint16_t>(entry.type)); break;
          case mapping_record_column::name_hash:       sqlite3_bind_text (statement, sql_param_index++, name_hash.data(), name_hash.size(), nullptr /*destructor*/); break;
          case mapping_record_column::encrypted_value: sqlite3_bind_blob (statement, sql_param_index++, entry.encrypted_value.data(), entry.encrypted_value.size(), nullptr /*destructor*/); break;
          case mapping_record_column::txid:            sqlite3_bind_blob (statement, sql_param_index++, tx_hash.data, sizeof(tx_hash), nullptr /*destructor*/); break;
          case mapping_record_column::prev_txid:       sqlite3_bind_blob (statement, sql_param_index++, entry.prev_txid.data, sizeof(entry.prev_txid), nullptr /*destructor*/); break;
          case mapping_record_column::owner_id:        sqlite3_bind_int64(statement, sql_param_index++, owner_id); break;
          case mapping_record_column::backup_owner_id: sqlite3_bind_int64(statement, sql_param_index++, backup_owner_id); break;
          default: assert(false); return false;
        }
      }
    }

    if (!sql_run_statement(lns_db.network_type(), lns_sql_type::save_mapping, statement, nullptr))
      return false;
  }

  return true;
}

bool name_system_db::add_block(const cryptonote::block &block, const std::vector<cryptonote::transaction> &txs)
{
  uint64_t height = cryptonote::get_block_height(block);
  if (last_processed_height >= height)
      return true;

  scoped_db_transaction db_transaction(*this);
  if (!db_transaction)
   return false;

  if (block.major_version >= cryptonote::network_version_15_lns)
  {
    for (cryptonote::transaction const &tx : txs)
    {
      if (tx.type != cryptonote::txtype::loki_name_system)
        continue;

      cryptonote::tx_extra_loki_name_system entry = {};
      std::string fail_reason;
      if (!validate_lns_tx(block.major_version, height, tx, &entry, &fail_reason))
      {
        LOG_PRINT_L0("LNS TX: Failed to validate for tx=" << get_transaction_hash(tx) << ". This should have failed validation earlier reason=" << fail_reason);
        assert("Failed to validate acquire name service. Should already have failed validation prior" == nullptr);
        return false;
      }

      crypto::hash const &tx_hash = cryptonote::get_transaction_hash(tx);
      if (!add_lns_entry(*this, height, entry, tx_hash))
        return false;
    }
  }

  last_processed_height = height;
  save_settings(height, cryptonote::get_block_hash(block), static_cast<int>(DB_VERSION));
  db_transaction.commit = true;
  return true;
}

static bool get_txid_lns_entry(cryptonote::Blockchain const &blockchain, crypto::hash txid, cryptonote::tx_extra_loki_name_system &extra)
{
  if (txid == crypto::null_hash) return false;
  std::vector<cryptonote::transaction> txs;
  std::vector<crypto::hash> missed_txs;
  if (!blockchain.get_transactions({txid}, txs, missed_txs) || txs.empty())
    return false;

  return cryptonote::get_loki_name_system_from_tx_extra(txs[0].extra, extra);
}

struct lns_update_history
{
  uint64_t value_last_update_height        = static_cast<uint64_t>(-1);
  uint64_t owner_last_update_height        = static_cast<uint64_t>(-1);
  uint64_t backup_owner_last_update_height = static_cast<uint64_t>(-1);

  void     update(uint64_t height, cryptonote::tx_extra_loki_name_system const &lns_extra);
  uint64_t newest_update_height() const;
};

void lns_update_history::update(uint64_t height, cryptonote::tx_extra_loki_name_system const &lns_extra)
{
  if (lns_extra.field_is_set(lns::extra_field::encrypted_value))
    value_last_update_height = height;

  if (lns_extra.field_is_set(lns::extra_field::owner))
    owner_last_update_height = height;

  if (lns_extra.field_is_set(lns::extra_field::backup_owner))
    backup_owner_last_update_height = height;
}

uint64_t lns_update_history::newest_update_height() const
{
  uint64_t result = std::max(std::max(value_last_update_height, owner_last_update_height), backup_owner_last_update_height);
  return result;
}

struct replay_lns_tx
{
  uint64_t                              height;
  crypto::hash                          tx_hash;
  cryptonote::tx_extra_loki_name_system entry;
};

static std::vector<replay_lns_tx> find_lns_txs_to_replay(cryptonote::Blockchain const &blockchain, lns::mapping_record const &mapping, uint64_t blockchain_height)
{
  /*
     Detach Logic
     -----------------------------------------------------------------------------------------------
     LNS Buy    @ Height 100: LNS Record={field1=a1, field2=b1, field3=c1}
     LNS Update @ Height 200: LNS Record={field1=a2                      }
     LNS Update @ Height 300: LNS Record={           field2=b2           }
     LNS Update @ Height 400: LNS Record={                      field3=c2}
     LNS Update @ Height 500: LNS Record={field1=a3, field2=b3, field3=c3}
     LNS Update @ Height 600: LNS Record={                      field3=c4}

     Blockchain detaches to height 401, the target LNS record now looks like
                                         {field1=a2, field2=b2, field3=c2}

     Our current LNS record looks like
                                         {field1=a3, field2=b3, field3=c4}

     To get all the fields back, we can't just replay the latest LNS update
     transactions in reverse chronological order back to the detach height,
     otherwise we miss the update to field1=a2 and field2=b2.

     To rebuild our LNS record, we need to iterate back until we find all the
     TX's that updated the LNS field(s) until all fields have been reverted to
     a state representative of pre-detach height.

     i.e. Go back to the closest LNS record to the detach height, at height 300.
     Next, iterate back until all LNS fields have been touched at a point in
     time before the detach height (i.e. height 200 with field=a2). Replay the
     transactions.
  */

  std::vector<replay_lns_tx> result;
  lns_update_history update_history = {};
  for (crypto::hash curr_txid = mapping.prev_txid;
       update_history.newest_update_height() >= blockchain_height;
      )
  {
    cryptonote::tx_extra_loki_name_system curr_lns_extra = {};
    if (!get_txid_lns_entry(blockchain, curr_txid, curr_lns_extra))
    {
      if (curr_txid != crypto::null_hash)
        MERROR("Unexpected error querying TXID=" << curr_txid << ", from DB for LNS");
      return result;
    }

    std::vector<uint64_t> curr_heights = blockchain.get_transactions_heights({curr_txid});
    if (curr_heights.empty())
    {
      MERROR("Unexpected error querying TXID=" << curr_txid << ", height from DB for LNS");
      return result;
    }

    if (curr_heights[0] < blockchain_height)
      result.push_back({curr_heights[0], curr_txid, curr_lns_extra});

    update_history.update(curr_heights[0], curr_lns_extra);
    curr_txid = curr_lns_extra.prev_txid;
  }

  return result;
}

void name_system_db::block_detach(cryptonote::Blockchain const &blockchain, uint64_t new_blockchain_height)
{
  std::vector<mapping_record> new_mappings = {};
  {
    sqlite3_stmt *statement = get_mappings_on_height_and_newer_sql;
    sqlite3_clear_bindings(statement);
    sqlite3_bind_int(statement, 1 /*sql param index*/, new_blockchain_height);
    sql_run_statement(nettype, lns_sql_type::get_mappings_on_height_and_newer, statement, &new_mappings);
  }

  std::vector<replay_lns_tx> txs_to_replay;
  for (auto const &mapping : new_mappings)
  {
    std::vector<replay_lns_tx> replay_txs = find_lns_txs_to_replay(blockchain, mapping, new_blockchain_height);
    txs_to_replay.reserve(txs_to_replay.size() + replay_txs.size());
    txs_to_replay.insert(txs_to_replay.end(), replay_txs.begin(), replay_txs.end());
  }

  prune_db(new_blockchain_height);
  for (auto it = txs_to_replay.rbegin(); it != txs_to_replay.rend(); it++)
  {
    if (!add_lns_entry(*this, it->height, it->entry, it->tx_hash))
      MERROR("Unexpected failure to add historical LNS into the DB on reorganization from tx=" << it->tx_hash);
  }
}

bool name_system_db::save_owner(crypto::generic_public_key const &key, int64_t *row_id)
{
  sqlite3_stmt *statement = save_owner_sql;
  sqlite3_clear_bindings(statement);
  sqlite3_bind_blob(statement, 1 /*sql param index*/, &key, sizeof(key), nullptr /*destructor*/);
  bool result = sql_run_statement(nettype, lns_sql_type::save_owner, statement, nullptr);
  if (row_id) *row_id = sqlite3_last_insert_rowid(db);
  return result;
}

bool name_system_db::save_mapping(crypto::hash const &tx_hash, cryptonote::tx_extra_loki_name_system const &src, uint64_t height, int64_t owner_id, int64_t backup_owner_id)
{
  if (!src.is_buying())
    return false;

  std::string name_hash = hash_to_base64(src.name_hash);
  sqlite3_stmt *statement = save_mapping_sql;
  sqlite3_bind_int  (statement, static_cast<int>(mapping_record_column::type), static_cast<uint16_t>(src.type));
  sqlite3_bind_text (statement, static_cast<int>(mapping_record_column::name_hash), name_hash.data(), name_hash.size(), nullptr /*destructor*/);
  sqlite3_bind_blob (statement, static_cast<int>(mapping_record_column::encrypted_value), src.encrypted_value.data(), src.encrypted_value.size(), nullptr /*destructor*/);
  sqlite3_bind_blob (statement, static_cast<int>(mapping_record_column::txid), tx_hash.data, sizeof(tx_hash), nullptr /*destructor*/);
  sqlite3_bind_blob (statement, static_cast<int>(mapping_record_column::prev_txid), src.prev_txid.data, sizeof(src.prev_txid), nullptr /*destructor*/);
  sqlite3_bind_int64(statement, static_cast<int>(mapping_record_column::register_height), static_cast<int64_t>(height));
  sqlite3_bind_int64(statement, static_cast<int>(mapping_record_column::owner_id), owner_id);
  if (backup_owner_id != 0)
  {
    sqlite3_bind_int64(statement, static_cast<int>(mapping_record_column::backup_owner_id), backup_owner_id);
  }
  bool result = sql_run_statement(nettype, lns_sql_type::save_mapping, statement, nullptr);
  return result;
}

bool name_system_db::save_settings(uint64_t top_height, crypto::hash const &top_hash, int version)
{
  sqlite3_stmt *statement = save_settings_sql;
  sqlite3_bind_int64(statement, static_cast<int>(lns_db_setting_column::top_height), top_height);
  sqlite3_bind_blob (statement, static_cast<int>(lns_db_setting_column::top_hash),   top_hash.data, sizeof(top_hash), nullptr /*destructor*/);
  sqlite3_bind_int  (statement, static_cast<int>(lns_db_setting_column::version),    version);
  bool result = sql_run_statement(nettype, lns_sql_type::save_setting, statement, nullptr);
  return result;
}

bool name_system_db::prune_db(uint64_t height)
{
  {
    sqlite3_stmt *statement = prune_mappings_sql;
    sqlite3_bind_int64(statement, 1 /*sql param index*/, height);
    if (!sql_run_statement(nettype, lns_sql_type::pruning, statement, nullptr)) return false;
  }

  {
    sqlite3_stmt *statement = prune_owners_sql;
    if (!sql_run_statement(nettype, lns_sql_type::pruning, statement, nullptr)) return false;
  }

  return true;
}

owner_record name_system_db::get_owner_by_key(crypto::generic_public_key const &key) const
{
  sqlite3_stmt *statement = get_owner_by_key_sql;
  sqlite3_clear_bindings(statement);
  sqlite3_bind_blob(statement, 1 /*sql param index*/, &key, sizeof(key), nullptr /*destructor*/);

  owner_record result = {};
  result.loaded      = sql_run_statement(nettype, lns_sql_type::get_owner, statement, &result);
  return result;
}

owner_record name_system_db::get_owner_by_id(int64_t owner_id) const
{
  sqlite3_stmt *statement = get_owner_by_id_sql;
  sqlite3_clear_bindings(statement);
  sqlite3_bind_int(statement, 1 /*sql param index*/, owner_id);

  owner_record result = {};
  result.loaded      = sql_run_statement(nettype, lns_sql_type::get_owner, statement, &result);
  return result;
}

mapping_record name_system_db::get_mapping(mapping_type type, std::string const &name_base64_hash) const
{
  sqlite3_stmt *statement = get_mapping_sql;
  sqlite3_clear_bindings(statement);
  sqlite3_bind_int(statement, 1 /*sql param index*/, static_cast<uint16_t>(type));
  sqlite3_bind_text(statement, 2 /*sql param index*/, name_base64_hash.data(), name_base64_hash.size(), nullptr /*destructor*/);

  mapping_record result = {};
  result.loaded         = sql_run_statement(nettype, lns_sql_type::get_mapping, statement, &result);
  return result;
}

std::vector<mapping_record> name_system_db::get_mappings(std::vector<uint16_t> const &types, std::string const &name_base64_hash) const
{
  std::string sql_statement;
  sql_statement.reserve(120 + 7 * types.size());
  // Generate string statement

  if (types.size())
  {
    sql_statement += sql_cmd_combine_mappings_and_owner_table(R"(WHERE "name_hash" = ? AND "type" in ()");
    for (size_t i = 0; i < types.size(); i++)
    {
      sql_statement += std::to_string(types[i]);
      if (i < (types.size() - 1)) sql_statement += ", ";
    }
    sql_statement += ")";
  }
  else
  {
    sql_statement = R"(SELECT * FROM "mappings" JOIN "owner" ON "mappings"."owner_id" = "owner"."id" WHERE "name_hash" = ?)";
  }

  // Compile Statement
  std::vector<mapping_record> result;
  sqlite3_stmt *statement = nullptr;
  if (!sql_compile_statement(db, sql_statement.c_str(), sql_statement.size(), &statement, false /*optimise_for_multiple_usage*/))
    return result;

  sqlite3_bind_text(statement, 1 /*sql param index*/, name_base64_hash.data(), name_base64_hash.size(), nullptr /*destructor*/);

  // Execute
  sql_run_statement(nettype, lns_sql_type::get_mappings, statement, &result);
  return result;
}

std::vector<mapping_record> name_system_db::get_mappings_by_owners(std::vector<crypto::generic_public_key> const &keys) const
{
  std::string sql_statement;
  // Generate string statement
  {
    std::string const sql_prefix_str = sql_cmd_combine_mappings_and_owner_table(R"(WHERE "o1"."public_key" in ()");
    char constexpr SQL_MIDDLE[]  = R"(OR "o2"."public_key" in ()";
    char constexpr SQL_SUFFIX[]  = R"())";

    std::stringstream stream;
    stream << sql_prefix_str;
    for (size_t i = 0; i < keys.size(); i++)
    {
      stream << "?";
      if (i < (keys.size() - 1)) stream << ", ";
    }
    stream << SQL_SUFFIX;

    stream << SQL_MIDDLE;
    for (size_t i = 0; i < keys.size(); i++)
    {
      stream << "?";
      if (i < (keys.size() - 1)) stream << ", ";
    }
    stream << SQL_SUFFIX;

    stream << SQL_MIDDLE;
    for (size_t i = 0; i < keys.size(); i++)
    {
      stream << "?";
      if (i < (keys.size() - 1)) stream << ", ";
    }
    stream << SQL_SUFFIX;
    sql_statement = stream.str();
  }

  // Compile Statement
  std::vector<mapping_record> result;
  sqlite3_stmt *statement = nullptr;
  if (!sql_compile_statement(db, sql_statement.c_str(), sql_statement.size(), &statement, false /*optimise_for_multiple_usage*/))
    return result;

  // Bind parameters statements
  int sql_param_index = 1;
  for (size_t i = 0; i < keys.size(); i++)
    for (auto const &key : keys)
      sqlite3_bind_blob(statement, sql_param_index++, key.data, sizeof(key), nullptr /*destructor*/);

  // Execute
  sql_run_statement(nettype, lns_sql_type::get_mappings_by_owners, statement, &result);
  return result;
}

std::vector<mapping_record> name_system_db::get_mappings_by_owner(crypto::generic_public_key const &key) const
{
  std::vector<mapping_record> result = {};
  sqlite3_stmt *statement = get_mappings_by_owner_sql;
  sqlite3_clear_bindings(statement);
  sqlite3_bind_blob(statement, 1 /*sql param index*/, key.data, sizeof(key), nullptr /*destructor*/);
  sqlite3_bind_blob(statement, 2 /*sql param index*/, key.data, sizeof(key), nullptr /*destructor*/);
  sql_run_statement(nettype, lns_sql_type::get_mappings_by_owner, statement, &result);
  return result;
}

settings_record name_system_db::get_settings() const
{
  sqlite3_stmt *statement = get_settings_sql;
  settings_record result  = {};
  result.loaded           = sql_run_statement(nettype, lns_sql_type::get_setting, statement, &result);
  return result;
}
}; // namespace service_nodes
