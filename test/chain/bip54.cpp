/**
 * Copyright (c) 2011-2026 libbitcoin developers
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "../test.hpp"

#include <fstream>
#include <iterator>
#include <filesystem>
#include <boost/json.hpp>

BOOST_AUTO_TEST_SUITE(bip54_tests)

using namespace system::chain;
namespace json = boost::json;

namespace {

json::value load_json(const std::string& name)
{
    const auto path = std::filesystem::path(__FILE__).parent_path() / "bip54" / name;
    std::ifstream file{ path };
    BOOST_REQUIRE_MESSAGE(file.good(), path.string());
    const std::string body{ std::istreambuf_iterator<char>(file), {} };
    return json::parse(body);
}

data_chunk decode_hex_chunk(const std::string& hex)
{
    data_chunk out;
    BOOST_REQUIRE(decode_base16(out, hex));
    return out;
}

header decode_header(const std::string& hex)
{
    return header{ decode_hex_chunk(hex) };
}

transaction decode_tx(const std::string& hex)
{
    return transaction{ decode_hex_chunk(hex), /*witness=*/true };
}

block decode_block(const std::string& hex)
{
    return block{ decode_hex_chunk(hex), /*witness=*/true };
}

transaction make_coinbase(uint32_t sequence, uint32_t locktime,
    const data_chunk& script_sig = {})
{
    const auto bytes = script_sig.empty() ?
        data_chunk{ 0x00, 0x00 } : script_sig;
    return transaction
    {
        2u,
        inputs{ input{ point{}, script{ bytes, /*prefix=*/false }, sequence } },
        outputs{ output{ 0u, script{} } },
        locktime
    };
}

transaction make_64byte_coinbase()
{
    const auto cb64 = make_coinbase(max_input_sequence, /*locktime=*/0u,
        data_chunk{ 0x00, 0x00, 0x00, 0x00 });
    BOOST_REQUIRE_EQUAL(cb64.serialized_size(/*witness=*/false),
        invalid_nonwitness_tx_size);
    return cb64;
}

transaction make_64byte_tx()
{
    const transaction tx
    {
        2u,
        inputs{ input{ point{ null_hash, 0u }, script{}, max_input_sequence } },
        outputs{ output{ 0u, script{ data_chunk{ 0x00, 0x00, 0x00, 0x00 },
            /*prefix=*/false } } },
        0u
    };
    BOOST_REQUIRE_EQUAL(tx.serialized_size(/*witness=*/false),
        invalid_nonwitness_tx_size);
    return tx;
}

struct timestamp_vector
{
    std::vector<header> headers;
    bool valid;
    std::string comment;
};

void collect_timestamp_vectors(const json::value& entry,
    std::vector<header> prefix, std::vector<timestamp_vector>& out)
{
    for (const auto& hex: entry.at("block_headers").as_array())
        prefix.push_back(decode_header(std::string(hex.as_string())));

    if (const auto* extensions = entry.as_object().if_contains("extensions"))
    {
        for (const auto& branch: extensions->as_array())
            collect_timestamp_vectors(branch, prefix, out);
        return;
    }

    out.push_back(
    {
        std::move(prefix),
        entry.at("valid").as_bool(),
        std::string(entry.at("comment").as_string())
    });
}

} // namespace

// gates
// ----------------------------------------------------------------------------

constexpr context bip54_ctx(bool engaged, size_t height = 0) NOEXCEPT
{
    context ctx{};
    ctx.flags = engaged ? flags::bip54_rule : flags::no_rules;
    ctx.height = height;
    return ctx;
}

bool bip54_timestamps_valid(const std::vector<header>& headers,
    size_t retargeting_interval) NOEXCEPT
{
    for (size_t height{0}; height < headers.size(); ++height)
    {
        const auto& header = headers.at(height);
        auto on = bip54_ctx(true, height);
        on.previous_timestamp = is_zero(height) ? 0 :
            headers.at(sub1(height)).timestamp();

        const auto period_start = height - (height % retargeting_interval);
        on.period_start_timestamp = headers.at(period_start).timestamp();
        on.work_required = header.bits();

        const bool via_predicates =
            on.is_early_timestamp(header.timestamp(), retargeting_interval) ||
            on.is_negative_period_duration(header.timestamp(), retargeting_interval);

        const auto ec_accept = header.accept(on, retargeting_interval);
        const bool via_accept = (ec_accept == error::early_timestamp) ||
            (ec_accept == error::negative_period_duration);

        BOOST_REQUIRE_EQUAL(via_predicates, via_accept);
        if (via_predicates)
            return false;
    }

    return true;
}

BOOST_AUTO_TEST_CASE(bip54__accept__stale_ctx_timewarp_off__block_success)
{
    constexpr auto prev_block_timestamp = 1'000'000u;
    constexpr auto attack = possible_narrow_cast<uint32_t>(
        prev_block_timestamp - max_timewarp_bip54 - 1u);
    const auto retargeting_interval = settings{}.retargeting_interval();
    const header attack_header
    {
        2u, null_hash, null_hash, /*timestamp=*/attack, 0u, 0u
    };

    auto off = bip54_ctx(false);
    off.previous_timestamp = prev_block_timestamp;
    BOOST_REQUIRE(!off.is_early_timestamp(attack, retargeting_interval));
    BOOST_REQUIRE(attack_header.accept(off, retargeting_interval) !=
        error::early_timestamp);
}

BOOST_AUTO_TEST_CASE(bip54__accept__stale_ctx_timewarp_on__early_timestamp)
{
    constexpr auto prev_block_timestamp = 1'000'000u;
    constexpr auto attack = possible_narrow_cast<uint32_t>(
        prev_block_timestamp - max_timewarp_bip54 - 1u);
    const auto retargeting_interval = settings{}.retargeting_interval();
    const header attack_header
    {
        2u, null_hash, null_hash, /*timestamp=*/attack, 0u, 0u
    };

    auto on = bip54_ctx(true);
    on.previous_timestamp = prev_block_timestamp;
    BOOST_REQUIRE(on.is_early_timestamp(attack, retargeting_interval));
    BOOST_REQUIRE_EQUAL(attack_header.accept(on, retargeting_interval),
        error::early_timestamp);
}

BOOST_AUTO_TEST_CASE(bip54__accept__stale_ctx_negative_period_off__block_success)
{
    constexpr auto period_start = 1'000u;
    constexpr auto attack = period_start - 1u;
    const auto retargeting_interval = settings{}.retargeting_interval();
    const header attack_header
    {
        2u, null_hash, null_hash, /*timestamp=*/attack, 0u, 0u
    };

    auto off = bip54_ctx(false, sub1(retargeting_interval));
    off.period_start_timestamp = period_start;
    BOOST_REQUIRE(!off.is_negative_period_duration(attack, retargeting_interval));
    BOOST_REQUIRE(attack_header.accept(off, retargeting_interval) !=
        error::negative_period_duration);
}

BOOST_AUTO_TEST_CASE(bip54__accept__stale_ctx_negative_period_on__negative_period_duration)
{
    constexpr auto period_start = 1'000u;
    constexpr auto attack = period_start - 1u;
    const auto retargeting_interval = settings{}.retargeting_interval();
    const header attack_header
    {
        2u, null_hash, null_hash, /*timestamp=*/attack, 0u, 0u
    };

    auto on = bip54_ctx(true, sub1(retargeting_interval));
    on.period_start_timestamp = period_start;
    BOOST_REQUIRE(on.is_negative_period_duration(attack, retargeting_interval));
    BOOST_REQUIRE_EQUAL(attack_header.accept(on, retargeting_interval),
        error::negative_period_duration);
}

// pool (check_guard)
// ----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(bip54__check_guard__tx_size_64__invalid_nonwitness_tx_size)
{
    const auto tx = make_64byte_tx();
    BOOST_REQUIRE_EQUAL(tx.check_guard(bip54_ctx(false)),
        error::invalid_nonwitness_tx_size);
    BOOST_REQUIRE_EQUAL(tx.check_guard(bip54_ctx(true)),
        error::invalid_nonwitness_tx_size);
}

// block (check)
// ----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(bip54__check__tx_size_64__invalid_nonwitness_tx_size)
{
    const auto tx = make_64byte_tx();
    BOOST_REQUIRE_EQUAL(tx.check(bip54_ctx(false)),
        error::transaction_success);
    BOOST_REQUIRE_EQUAL(tx.check(bip54_ctx(true)),
        error::invalid_nonwitness_tx_size);
}

BOOST_AUTO_TEST_CASE(bip54__check__coinbase_size_64__invalid_nonwitness_tx_size)
{
    const auto cb = make_64byte_coinbase();
    BOOST_REQUIRE_EQUAL(cb.check(bip54_ctx(false)),
        error::transaction_success);
    BOOST_REQUIRE_EQUAL(cb.check(bip54_ctx(true)),
        error::invalid_nonwitness_tx_size);
}

BOOST_AUTO_TEST_CASE(bip54__check__coinbase_lock_off__transaction_success)
{
    const auto bad_lock = make_coinbase(sub1(max_input_sequence), /*locktime=*/0u);
    
    constexpr auto off = bip54_ctx(false, /*height=*/21u);
    BOOST_REQUIRE_EQUAL(bad_lock.check(off), error::transaction_success);
}

BOOST_AUTO_TEST_CASE(bip54__check__coinbase_lock_on__invalid_coinbase_locktime)
{
    const auto bad_lock = make_coinbase(sub1(max_input_sequence), /*locktime=*/0u);

    constexpr auto on = bip54_ctx(true, /*height=*/21u);
    BOOST_REQUIRE_EQUAL(bad_lock.check(on), error::invalid_coinbase_locktime);
}

BOOST_AUTO_TEST_CASE(bip54__check__coinbase_sequence_off__transaction_success)
{
    constexpr auto height = 21u;
    const auto bad_seq = make_coinbase(max_input_sequence, sub1(height));

    constexpr auto off = bip54_ctx(false, height);
    BOOST_REQUIRE_EQUAL(bad_seq.check(off), error::transaction_success);
}

BOOST_AUTO_TEST_CASE(bip54__check__coinbase_sequence_on__invalid_coinbase_sequence)
{
    constexpr auto height = 21u;
    const auto bad_seq = make_coinbase(max_input_sequence, sub1(height));

    constexpr auto on = bip54_ctx(true, height);
    BOOST_REQUIRE_EQUAL(bad_seq.check(on), error::invalid_coinbase_sequence);
}

// edges
// ----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(bip54__check__coinbase_height_zero__transaction_success)
{
    // Would fail lock and sequence at nonzero height. Rule skips height 0.
    const auto cb = make_coinbase(max_input_sequence, /*locktime=*/0u);

    constexpr auto on = bip54_ctx(true, /*height=*/0);
    BOOST_REQUIRE_EQUAL(cb.check(on), error::transaction_success);
}

// vectors
// ----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(bip54__timestamps__vectors__expected)
{
    const auto retargeting_interval = settings{}.retargeting_interval();
    std::vector<timestamp_vector> vectors;
    collect_timestamp_vectors(load_json("timestamps.json"), {}, vectors);
    BOOST_REQUIRE(!vectors.empty());

    for (size_t i{0}; i < vectors.size(); ++i)
    {
        const auto timestamps_ok = bip54_timestamps_valid(
            vectors[i].headers, retargeting_interval);
        const auto valid = vectors[i].valid;
        BOOST_REQUIRE_MESSAGE(timestamps_ok == valid,
            "timestamps case " + std::to_string(i + 1) + ": " +
            vectors[i].comment);
    }
}

BOOST_AUTO_TEST_CASE(bip54__txsize__vectors__expected)
{
    const auto document = load_json("txsize.json");
    const auto& vectors = document.as_array();
    for (size_t i{0}; i < vectors.size(); ++i)
    {
        const auto& vector = vectors.at(i);
        const auto tx = decode_tx(std::string(vector.at("tx").as_string()));
        const auto size64 = tx.serialized_size(/*witness=*/false) ==
            invalid_nonwitness_tx_size;
        const bool valid = vector.at("valid").as_bool();
        BOOST_REQUIRE_MESSAGE(size64 != valid,
            "txsize case " + std::to_string(i + 1) + ": " +
            std::string(vector.at("comment").as_string()));

        constexpr auto on = bip54_ctx(true);
        const auto ec_check = tx.check(on);
        if (valid)
            BOOST_REQUIRE_EQUAL(ec_check, error::transaction_success);
        else
            BOOST_REQUIRE_EQUAL(ec_check, error::invalid_nonwitness_tx_size);
    }
}

BOOST_AUTO_TEST_CASE(bip54__coinbases__vectors__expected)
{
    const auto document = load_json("coinbases.json");
    const auto& vectors = document.as_array();
    for (size_t i{0}; i < vectors.size(); ++i)
    {
        const auto& vector = vectors.at(i);
        const auto& blockchain = vector.at("block_chain").as_array();
        const auto height = sub1(blockchain.size());
        const auto block = decode_block(std::string(blockchain.back().as_string()));
        const auto on = bip54_ctx(true, height);
        const auto& coinbase = *block.transactions_ptr()->front();
        const auto ec_check = coinbase.check(on);
        const bool valid = vector.at("valid").as_bool();
        const auto comment = std::string(vector.at("comment").as_string());
        if (valid)
        {
            BOOST_REQUIRE_MESSAGE(ec_check == error::transaction_success,
                "coinbase case " + std::to_string(i + 1) + ": " + comment);
        }
        else if (coinbase.locktime() != sub1(height))
        {
            BOOST_REQUIRE_MESSAGE(ec_check == error::invalid_coinbase_locktime,
                "coinbase case " + std::to_string(i + 1) + ": " + comment +
                " ec_check=" + ec_check.message());
        }
        else
        {
            BOOST_REQUIRE_MESSAGE(ec_check == error::invalid_coinbase_sequence,
                "coinbase case " + std::to_string(i + 1) + ": " + comment +
                " ec_check=" + ec_check.message());
        }
    }
}

// integration
// ----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(bip54__accept__testnet3_timewarp_pin__early_timestamp)
{
    constexpr auto tip_height = 8064u;
    constexpr auto prev_block_hex =
        "01000000018e9da3242e84256c2605065e79241810915244a31015b8d9e93d6200000000d3ebb7200f444512b7f3c983224ed29db1e434f6d5b90e130a874fccc40f1d4ffbd70c50f0ff0f1c011c4aeb";
    constexpr auto tip_hex =
        "01000000d5be886aabaff7ec7a48d67cbef287228219289e2b0446f2799c9c0a000000001445b86a883acddb79b0c566a7750ca3f1ff2645e21bfbb7be6b1fc98be8e5384a05c34fc0ff3f1c11336d5b";
    const auto prev_block = decode_header(prev_block_hex);
    const auto tip = decode_header(tip_hex);
    const auto retargeting_interval =
        settings{ selection::testnet3 }.retargeting_interval();

    auto on = bip54_ctx(true, tip_height);
    on.previous_timestamp = prev_block.timestamp();
    on.work_required = tip.bits();
    BOOST_REQUIRE_EQUAL(tip.accept(on, retargeting_interval),
        error::early_timestamp);

    auto off = on;
    off.flags = flags::no_rules;
    BOOST_REQUIRE(tip.accept(off, retargeting_interval) !=
        error::early_timestamp);
}

BOOST_AUTO_TEST_SUITE_END()
