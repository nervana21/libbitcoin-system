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

BC_AUTO_TEST_SUITE(bip54_tests)

using namespace system::chain;
using namespace system::json;

namespace {

constexpr uint32_t bip54_flags =
    flags::bip54_timewarp_rule |
    flags::bip54_timespan_rule |
    flags::bip54_sigops_rule |
    flags::bip54_txsize_rule |
    flags::bip54_coinbase_rule;

value load_json(const std::string& name)
{
    const auto path = std::filesystem::path(__FILE__).parent_path() / "bip54" / name;
    std::ifstream file{ path };
    BC_REQUIRE_MESSAGE(file.good(), path.string());
    const std::string body{ std::istreambuf_iterator<char>(file), {} };
    return parse(body);
}

data_chunk decode_hex_chunk(const std::string& hex)
{
    data_chunk out;
    BC_REQUIRE(decode_base16(out, hex));
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

output decode_output(const std::string& hex)
{
    return output{ decode_hex_chunk(hex) };
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
    BC_REQUIRE_EQUAL(cb64.serialized_size(/*witness=*/false),
        invalid_transaction_size);
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
    BC_REQUIRE_EQUAL(tx.serialized_size(/*witness=*/false),
        invalid_transaction_size);
    return tx;
}

transaction make_sigops_tx(size_t sigops)
{
    operations ops;
    ops.reserve(sigops);
    for (size_t n{0}; n < sigops; ++n)
        ops.emplace_back(opcode::checksig);

    input inpt{ point{ null_hash, 0u }, script{}, max_input_sequence };
    inpt.prevout = to_shared<output>(1_u64, script{ ops });
    const transaction tx
    {
        2u,
        inputs{ std::move(inpt) },
        outputs{ output{ 1_u64, script{} } },
        0u
    };
    BC_REQUIRE_EQUAL(tx.prevout_signature_operations(), sigops);
    return tx;
}

block as_block(const transaction& tx) NOEXCEPT
{
    return block
    {
        header{},
        transactions{ tx }
    };
}

struct timestamp_vector
{
    std::vector<header> headers;
    bool valid;
    std::string comment;
};

void collect_timestamp_vectors(const value& entry,
    std::vector<header> prefix, std::vector<timestamp_vector>& out)
{
    for (const auto& hex: entry.at("block_headers").as_array())
        prefix.push_back(decode_header(hex.as_string()));

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
        entry.at("comment").as_string()
    });
}

} // namespace

// gates
// ----------------------------------------------------------------------------

constexpr context bip54_ctx(bool engaged, size_t height = 0) NOEXCEPT
{
    context ctx{};
    ctx.flags = engaged ? bip54_flags : flags::no_rules;
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
        on.retarget_timestamp = headers.at(period_start).timestamp();
        on.work_required = header.bits();

        const bool via_predicates =
            on.is_early_timestamp(header.timestamp(), retargeting_interval) ||
            on.is_negative_timespan(header.timestamp(), retargeting_interval);

        const auto ec_accept = header.accept(on, retargeting_interval);
        const bool via_accept = (ec_accept == error::early_timestamp) ||
            (ec_accept == error::negative_timespan);

        BC_REQUIRE_EQUAL(via_predicates, via_accept);
        if (via_predicates)
            return false;
    }

    return true;
}

BC_AUTO_TEST_CASE(bip54__accept__stale_ctx_timewarp_off__block_success)
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
    // Stale ctx.timestamp would look safe; header time still decides.
    off.timestamp = prev_block_timestamp;
    BC_REQUIRE(!off.is_early_timestamp(attack, retargeting_interval));
    BC_REQUIRE(attack_header.accept(off, retargeting_interval) !=
        error::early_timestamp);
}

BC_AUTO_TEST_CASE(bip54__accept__stale_ctx_timewarp_on__early_timestamp)
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
    // Stale ctx.timestamp looks safe; header time still fails.
    on.timestamp = prev_block_timestamp;
    BC_REQUIRE(on.is_early_timestamp(attack, retargeting_interval));
    BC_REQUIRE(!on.is_early_timestamp(retargeting_interval));
    BC_REQUIRE_EQUAL(attack_header.accept(on, retargeting_interval),
        error::early_timestamp);
}

BC_AUTO_TEST_CASE(bip54__accept__stale_ctx_negative_timespan_off__block_success)
{
    constexpr auto period_start = 1'000u;
    constexpr auto attack = period_start - 1u;
    const auto retargeting_interval = settings{}.retargeting_interval();
    const header attack_header
    {
        2u, null_hash, null_hash, /*timestamp=*/attack, 0u, 0u
    };

    auto off = bip54_ctx(false, sub1(retargeting_interval));
    off.retarget_timestamp = period_start;
    off.timestamp = period_start;
    // Predicate is flagless (unlike is_early_timestamp). Accept gates the rule.
    BC_REQUIRE(off.is_negative_timespan(attack, retargeting_interval));
    BC_REQUIRE(attack_header.accept(off, retargeting_interval) !=
        error::negative_timespan);
}

BC_AUTO_TEST_CASE(bip54__accept__stale_ctx_negative_timespan_on__negative_timespan)
{
    constexpr auto period_start = 1'000u;
    constexpr auto attack = period_start - 1u;
    const auto retargeting_interval = settings{}.retargeting_interval();
    const header attack_header
    {
        2u, null_hash, null_hash, /*timestamp=*/attack, 0u, 0u
    };

    auto on = bip54_ctx(true, sub1(retargeting_interval));
    on.retarget_timestamp = period_start;
    on.timestamp = period_start;
    BC_REQUIRE(on.is_negative_timespan(attack, retargeting_interval));
    BC_REQUIRE(!on.is_negative_timespan(retargeting_interval));
    BC_REQUIRE_EQUAL(attack_header.accept(on, retargeting_interval),
        error::negative_timespan);
}

// block (check / accept)
// ----------------------------------------------------------------------------

BC_AUTO_TEST_CASE(bip54__check__tx_size_64__invalid_transaction_size)
{
    const auto blk = as_block(make_64byte_tx());
    BC_REQUIRE_EQUAL(blk.check(bip54_ctx(false)),
        error::block_success);
    BC_REQUIRE_EQUAL(blk.check(bip54_ctx(true)),
        error::invalid_transaction_size);
}

BC_AUTO_TEST_CASE(bip54__check__coinbase_size_64__invalid_transaction_size)
{
    const auto blk = as_block(make_64byte_coinbase());
    BC_REQUIRE_EQUAL(blk.check(bip54_ctx(false)),
        error::block_success);
    BC_REQUIRE_EQUAL(blk.check(bip54_ctx(true)),
        error::invalid_transaction_size);
}

BC_AUTO_TEST_CASE(bip54__check__coinbase_lock_off__block_success)
{
    const auto blk = as_block(
        make_coinbase(sub1(max_input_sequence), /*locktime=*/0u));

    constexpr auto off = bip54_ctx(false, /*height=*/21u);
    BC_REQUIRE_EQUAL(blk.check(off), error::block_success);
}

BC_AUTO_TEST_CASE(bip54__check__coinbase_lock_on__coinbase_locktime_mismatch)
{
    const auto blk = as_block(
        make_coinbase(sub1(max_input_sequence), /*locktime=*/0u));

    constexpr auto on = bip54_ctx(true, /*height=*/21u);
    BC_REQUIRE_EQUAL(blk.check(on), error::coinbase_locktime_mismatch);
}

BC_AUTO_TEST_CASE(bip54__check__coinbase_sequence_off__block_success)
{
    constexpr auto height = 21u;
    const auto blk = as_block(
        make_coinbase(max_input_sequence, sub1(height)));

    constexpr auto off = bip54_ctx(false, height);
    BC_REQUIRE_EQUAL(blk.check(off), error::block_success);
}

BC_AUTO_TEST_CASE(bip54__check__coinbase_sequence_on__coinbase_sequence_final)
{
    constexpr auto height = 21u;
    const auto blk = as_block(
        make_coinbase(max_input_sequence, sub1(height)));

    constexpr auto on = bip54_ctx(true, height);
    BC_REQUIRE_EQUAL(blk.check(on), error::coinbase_sequence_final);
}

BC_AUTO_TEST_CASE(bip54__accept__sigops_over__transaction_prevout_sigop_limit)
{
    const auto excess = make_sigops_tx(add1(max_prevout_sigops));
    BC_REQUIRE_EQUAL(excess.accept(bip54_ctx(false)),
        error::transaction_success);
    BC_REQUIRE_EQUAL(excess.accept(bip54_ctx(true)),
        error::transaction_prevout_sigop_limit);
}

// calculations
// ----------------------------------------------------------------------------

BC_AUTO_TEST_CASE(bip54__sigops__count_boundaries__exact)
{
    const size_t samples[]
    {
        0u,
        1u,
        max_prevout_sigops,
        add1(max_prevout_sigops)
    };

    for (const auto sigops: samples)
    {
        const auto tx = make_sigops_tx(sigops);
        BC_REQUIRE_EQUAL(tx.prevout_signature_operations(), sigops);

        const bool over = sigops > max_prevout_sigops;
        constexpr auto on = bip54_ctx(true);
        BC_REQUIRE_EQUAL(tx.accept(on) == error::transaction_prevout_sigop_limit,
            over);
    }
}

// vectors
// ----------------------------------------------------------------------------

BC_AUTO_TEST_CASE(bip54__timestamps__vectors__expected)
{
    const auto retargeting_interval = settings{}.retargeting_interval();
    std::vector<timestamp_vector> vectors;
    collect_timestamp_vectors(load_json("timestamps.json"), {}, vectors);
    BC_REQUIRE(!vectors.empty());

    for (size_t i{0}; i < vectors.size(); ++i)
    {
        const auto timestamps_ok = bip54_timestamps_valid(
            vectors[i].headers, retargeting_interval);
        const auto valid = vectors[i].valid;
        BC_REQUIRE_MESSAGE(timestamps_ok == valid,
            "timestamps case " + std::to_string(i + 1) + ": " +
            vectors[i].comment);
    }
}

BC_AUTO_TEST_CASE(bip54__txsize__vectors__expected)
{
    // Only the txsize rule. Other BIP54 flags (e.g. coinbase) would reject a
    // lone spend as if it were a coinbase.
    context on{};
    on.flags = flags::bip54_txsize_rule;

    const auto document = load_json("txsize.json");
    const auto& vectors = document.as_array();
    for (size_t i{0}; i < vectors.size(); ++i)
    {
        const auto& vector = vectors.at(i);
        const auto tx = decode_tx(vector.at("tx").as_string());
        const auto size64 = tx.serialized_size(/*witness=*/false) ==
            invalid_transaction_size;
        const bool valid = vector.at("valid").as_bool();
        BC_REQUIRE_MESSAGE(size64 != valid,
            "txsize case " + std::to_string(i + 1) + ": " +
            vector.at("comment").as_string());

        const auto ec_check = as_block(tx).check(on);
        if (valid)
            BC_REQUIRE_EQUAL(ec_check, error::block_success);
        else
            BC_REQUIRE_EQUAL(ec_check, error::invalid_transaction_size);
    }
}

BC_AUTO_TEST_CASE(bip54__coinbases__vectors__expected)
{
    const auto document = load_json("coinbases.json");
    const auto& vectors = document.as_array();
    for (size_t i{0}; i < vectors.size(); ++i)
    {
        const auto& vector = vectors.at(i);
        const auto& blockchain = vector.at("block_chain").as_array();
        const auto height = sub1(blockchain.size());
        const auto blk = decode_block(blockchain.back().as_string());
        const auto on = bip54_ctx(true, height);
        const auto& coinbase = *blk.transactions_ptr()->front();
        const auto ec_check = blk.check(on);
        const bool valid = vector.at("valid").as_bool();
        const auto& comment = vector.at("comment").as_string();
        if (valid)
        {
            BC_REQUIRE_MESSAGE(ec_check == error::block_success,
                "coinbase case " + std::to_string(i + 1) + ": " + comment);
        }
        else if (coinbase.locktime() != sub1(height))
        {
            BC_REQUIRE_MESSAGE(ec_check == error::coinbase_locktime_mismatch,
                "coinbase case " + std::to_string(i + 1) + ": " + comment +
                " ec_check=" + ec_check.message());
        }
        else
        {
            BC_REQUIRE_MESSAGE(ec_check == error::coinbase_sequence_final,
                "coinbase case " + std::to_string(i + 1) + ": " + comment +
                " ec_check=" + ec_check.message());
        }
    }
}

BC_AUTO_TEST_CASE(bip54__sigops__vectors__expected)
{
    const auto document = load_json("sigops.json");
    const auto& vectors = document.as_array();
    for (size_t i{0}; i < vectors.size(); ++i)
    {
        const auto& vector = vectors.at(i);
        const auto tx = decode_tx(vector.at("tx").as_string());
        const auto& spent = vector.at("spent_outputs").as_array();
        BC_REQUIRE_EQUAL(spent.size(), tx.inputs());

        for (size_t j{0}; j < spent.size(); ++j)
        {
            const auto prev = decode_output(spent.at(j).as_string());
            (*tx.inputs_ptr())[j]->prevout = to_shared(std::move(prev));
        }

        const bool within_limit = tx.prevout_signature_operations() <=
            max_prevout_sigops;
        const bool valid = vector.at("valid").as_bool();
        BC_REQUIRE_MESSAGE(within_limit == valid,
            "sigops case " + std::to_string(i + 1) + ": " +
            vector.at("comment").as_string());

        constexpr auto on = bip54_ctx(true);
        const auto ec_accept = tx.accept(on);
        if (valid)
            BC_REQUIRE_EQUAL(ec_accept, error::transaction_success);
        else
            BC_REQUIRE_EQUAL(ec_accept, error::transaction_prevout_sigop_limit);
    }
}

// end-to-end (all mitigations)
// ----------------------------------------------------------------------------

BC_AUTO_TEST_CASE(bip54__mitigations__end_to_end__expected)
{
    // Condensed feature_bip54.py story. Off accepts the attack shapes. On
    // refuses them. Per-tx sigops split stays under the limit either way.
    const auto retargeting_interval = settings{}.retargeting_interval();

    constexpr auto prev_block_timestamp = 1'000'000u;
    constexpr auto timewarp_stamp = possible_narrow_cast<uint32_t>(
        prev_block_timestamp - max_timewarp_bip54 - 1u);
    const header timewarp_header
    {
        2u, null_hash, null_hash, /*timestamp=*/timewarp_stamp, 0u, 0u
    };

    constexpr auto period_start = 1'000u;
    constexpr auto negative_stamp = period_start - 1u;
    const header negative_header
    {
        2u, null_hash, null_hash, /*timestamp=*/negative_stamp, 0u, 0u
    };

    const auto tx64 = as_block(make_64byte_tx());
    const auto bad_coinbase = as_block(
        make_coinbase(max_input_sequence, /*locktime=*/0u));
    const auto excess_sigops = make_sigops_tx(add1(max_prevout_sigops));
    const auto under_sigops = make_sigops_tx(max_prevout_sigops);
    const auto split_sigops = make_sigops_tx(max_prevout_sigops);

    {
        auto off = bip54_ctx(false);
        off.previous_timestamp = prev_block_timestamp;
        BC_REQUIRE(timewarp_header.accept(off, retargeting_interval) !=
            error::early_timestamp);

        off = bip54_ctx(false, sub1(retargeting_interval));
        off.retarget_timestamp = period_start;
        BC_REQUIRE(negative_header.accept(off, retargeting_interval) !=
            error::negative_timespan);

        constexpr auto off_tx = bip54_ctx(false, /*height=*/21u);
        BC_REQUIRE_EQUAL(tx64.check(off_tx), error::block_success);
        BC_REQUIRE_EQUAL(bad_coinbase.check(off_tx), error::block_success);
        BC_REQUIRE_EQUAL(excess_sigops.accept(off_tx),
            error::transaction_success);
        BC_REQUIRE_EQUAL(under_sigops.accept(off_tx),
            error::transaction_success);
        BC_REQUIRE_EQUAL(split_sigops.accept(off_tx),
            error::transaction_success);
    }

    {
        auto on = bip54_ctx(true);
        on.previous_timestamp = prev_block_timestamp;
        BC_REQUIRE_EQUAL(timewarp_header.accept(on, retargeting_interval),
            error::early_timestamp);

        on = bip54_ctx(true, sub1(retargeting_interval));
        on.retarget_timestamp = period_start;
        BC_REQUIRE_EQUAL(negative_header.accept(on, retargeting_interval),
            error::negative_timespan);

        constexpr auto on_tx = bip54_ctx(true, /*height=*/21u);
        BC_REQUIRE_EQUAL(tx64.check(on_tx), error::invalid_transaction_size);
        BC_REQUIRE_EQUAL(bad_coinbase.check(on_tx),
            error::coinbase_locktime_mismatch);
        BC_REQUIRE_EQUAL(excess_sigops.accept(on_tx),
            error::transaction_prevout_sigop_limit);
        BC_REQUIRE_EQUAL(under_sigops.accept(on_tx),
            error::transaction_success);
        BC_REQUIRE_EQUAL(split_sigops.accept(on_tx),
            error::transaction_success);
    }
}

// integration
// ----------------------------------------------------------------------------

BC_AUTO_TEST_CASE(bip54__accept__testnet3_timewarp_pin__early_timestamp)
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
    BC_REQUIRE_EQUAL(tip.accept(on, retargeting_interval),
        error::early_timestamp);

    auto off = on;
    off.flags = flags::no_rules;
    BC_REQUIRE(tip.accept(off, retargeting_interval) !=
        error::early_timestamp);
}

BC_AUTO_TEST_SUITE_END()
