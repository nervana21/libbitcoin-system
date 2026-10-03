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
#include <optional>
#include <filesystem>
#include <boost/json.hpp>

BOOST_AUTO_TEST_SUITE(bip54_tests)

using namespace system::chain;
namespace json = boost::json;

namespace {

std::filesystem::path bip54_vector(const std::string& name) NOEXCEPT
{
    return std::filesystem::path(__FILE__).parent_path() / "bip54" / name;
}

json::value load_json(const std::string& name)
{
    const auto path = bip54_vector(name);
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
    return transaction{ decode_hex_chunk(hex), true };
}

block decode_block(const std::string& hex)
{
    return block{ decode_hex_chunk(hex), true };
}

bool bip54_timestamps_ok(const std::vector<header>& headers,
    size_t retargeting_interval) NOEXCEPT
{
    for (size_t height{0}; height < headers.size(); ++height)
    {
        const auto& hdr = headers.at(height);
        context ctx{};
        ctx.flags = flags::bip54_rule;
        ctx.timestamp = hdr.timestamp();
        ctx.height = height;
        ctx.previous_timestamp = is_zero(height) ? 0 :
            headers.at(sub1(height)).timestamp();

        const auto period_start = height - (height % retargeting_interval);
        ctx.period_start_timestamp = headers.at(period_start).timestamp();

        ctx.work_required = hdr.bits();
        ctx.minimum_block_version = 0;
        ctx.median_time_past = 0;

        const auto interval = possible_narrow_cast<uint32_t>(
            retargeting_interval);
        const bool helper_bad =
            ctx.is_early_timestamp(hdr.timestamp(), interval) ||
            ctx.is_negative_period_duration(hdr.timestamp(), interval);

        const auto ec = hdr.accept(ctx, interval);
        const bool accept_bad = (ec == error::early_timestamp) ||
            (ec == error::negative_period_duration);

        if (helper_bad != accept_bad)
            return false;

        if (helper_bad)
            return false;
    }

    return true;
}

struct timestamp_case
{
    std::vector<header> headers;
    bool valid;
    std::string comment;
};

void collect_timestamp_cases(const json::value& node,
    std::vector<header> prefix, std::vector<timestamp_case>& out)
{
    for (const auto& hex: node.at("block_headers").as_array())
        prefix.push_back(decode_header(std::string(hex.as_string())));

    if (const auto* extensions = node.as_object().if_contains("extensions"))
    {
        for (const auto& branch: extensions->as_array())
            collect_timestamp_cases(branch, prefix, out);
        return;
    }

    out.push_back(
    {
        std::move(prefix),
        node.at("valid").as_bool(),
        std::string(node.at("comment").as_string())
    });
}

transaction make_coinbase(uint32_t locktime, uint32_t sequence,
    const data_chunk& script_sig = {})
{
    const auto bytes = script_sig.empty() ?
        data_chunk{ 0x00, 0x00 } : script_sig;
    return transaction
    {
        1u,
        inputs{ input{ point{}, script{ bytes, false }, sequence } },
        outputs{ output{ 0u, script{} } },
        locktime
    };
}

transaction make_bad_locktime_coinbase(size_t height)
{
    return make_coinbase(static_cast<uint32_t>(height), max_input_sequence);
}

transaction make_final_sequence_coinbase(size_t height)
{
    return make_coinbase(static_cast<uint32_t>(sub1(height)),
        max_input_sequence);
}

transaction make_64byte_coinbase()
{
    return make_coinbase(0u, max_input_sequence,
        data_chunk{ 0x00, 0x00, 0x00, 0x00 });
}

context bip54_ctx(bool on, size_t height = 0) NOEXCEPT
{
    context ctx{};
    ctx.flags = on ? flags::bip54_rule : flags::no_rules;
    ctx.height = height;
    return ctx;
}

transaction load_bad64_tx()
{
    const auto root = load_json("txsize.json");
    for (const auto& case_: root.as_array())
    {
        if (!case_.at("valid").as_bool())
            return decode_tx(std::string(case_.at("tx").as_string()));
    }

    BOOST_REQUIRE(false);
    return {};
}

} // namespace

// vectors
// ----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(bip54__timestamps_vectors__match_expected)
{
    constexpr size_t retargeting_interval = 2016;
    std::vector<timestamp_case> cases;
    collect_timestamp_cases(load_json("timestamps.json"), {}, cases);
    BOOST_REQUIRE(!cases.empty());

    size_t index{};
    for (const auto& case_: cases)
    {
        ++index;
        const auto ok = bip54_timestamps_ok(case_.headers,
            retargeting_interval);
        BOOST_REQUIRE_MESSAGE(ok == case_.valid,
            "timestamps case " + std::to_string(index) + ": " + case_.comment);
    }
}

BOOST_AUTO_TEST_CASE(bip54__txsize_vectors__match_expected)
{
    const auto root = load_json("txsize.json");
    size_t index{};
    for (const auto& case_: root.as_array())
    {
        ++index;
        auto tx = decode_tx(std::string(case_.at("tx").as_string()));
        const auto size64 = tx.serialized_size(false) ==
            invalid_nonwitness_tx_size;
        const auto expected = case_.at("valid").as_bool();
        BOOST_REQUIRE_MESSAGE((!size64) == expected,
            "txsize case " + std::to_string(index) + ": " +
            std::string(case_.at("comment").as_string()));

        context ctx{};
        ctx.flags = flags::bip54_rule;
        const auto ec = tx.check(ctx);
        if (expected)
            BOOST_REQUIRE_EQUAL(ec, error::transaction_success);
        else
            BOOST_REQUIRE_EQUAL(ec, error::invalid_tx_size_64);
    }
}

BOOST_AUTO_TEST_CASE(bip54__coinbases_vectors__match_expected)
{
    const auto root = load_json("coinbases.json");
    size_t index{};
    for (const auto& case_: root.as_array())
    {
        ++index;
        const auto& chain = case_.at("block_chain").as_array();
        BOOST_REQUIRE(!chain.empty());

        const auto height = sub1(chain.size());
        auto blk = decode_block(std::string(chain.back().as_string()));
        BOOST_REQUIRE(!blk.transactions_ptr()->empty());

        context ctx{};
        ctx.flags = flags::bip54_rule;
        ctx.height = height;

        const auto& coinbase = *blk.transactions_ptr()->front();
        const auto ec = coinbase.check(ctx);
        const auto expected = case_.at("valid").as_bool();
        if (expected)
            BOOST_REQUIRE_MESSAGE(ec == error::transaction_success,
                "coinbase case " + std::to_string(index) + ": " +
                std::string(case_.at("comment").as_string()));
        else
            BOOST_REQUIRE_MESSAGE(
                ec == error::invalid_coinbase_locktime ||
                ec == error::invalid_coinbase_sequence,
                "coinbase case " + std::to_string(index) + ": " +
                std::string(case_.at("comment").as_string()) +
                " ec=" + ec.message());
    }
}

// edges
// ----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(bip54__coinbase__height_zero_and_flag_off__skip)
{
    constexpr auto h = 100u;
    const transaction unlocked
    {
        1u,
        inputs{ input{ point{}, script{}, max_input_sequence } },
        outputs{ output{ 0u, script{} } },
        static_cast<uint32_t>(h)
    };
    BOOST_REQUIRE(unlocked.is_coinbase());

    context genesis{};
    genesis.flags = flags::bip54_rule;
    genesis.height = 0;
    BOOST_REQUIRE_EQUAL(unlocked.check(genesis), error::transaction_success);

    context off{};
    off.flags = flags::no_rules;
    off.height = h;
    BOOST_REQUIRE_EQUAL(unlocked.check(off), error::transaction_success);
}

// integration
// ----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(bip54__testnet3_timewarp_pin__bip54_rejects)
{
    constexpr auto tip_hash =
        "00000000118da1e2165a19307b86f87eba814845e8a0f99734dce279ca3fb029";
    constexpr size_t tip_height = 8064;
    constexpr auto prev_hex =
        "01000000018e9da3242e84256c2605065e79241810915244a31015b8d9e93d6200000000d3ebb7200f444512b7f3c983224ed29db1e434f6d5b90e130a874fccc40f1d4ffbd70c50f0ff0f1c011c4aeb";
    constexpr auto tip_hex =
        "01000000d5be886aabaff7ec7a48d67cbef287228219289e2b0446f2799c9c0a000000001445b86a883acddb79b0c566a7750ca3f1ff2645e21bfbb7be6b1fc98be8e5384a05c34fc0ff3f1c11336d5b";

    const auto prev = decode_header(prev_hex);
    const auto tip = decode_header(tip_hex);
    BOOST_REQUIRE_EQUAL(encode_hash(tip.hash()), tip_hash);
    BOOST_REQUIRE_EQUAL(encode_hash(tip.previous_block_hash()),
        encode_hash(prev.hash()));

    settings cfg{ selection::testnet3 };
    cfg.forks.bip54 = true;
    const auto retargeting_interval = possible_narrow_cast<uint32_t>(
        cfg.retargeting_interval());
    BOOST_REQUIRE_EQUAL(retargeting_interval, 2016u);
    BOOST_REQUIRE(is_zero(tip_height % retargeting_interval));

    context on{};
    on.flags = chain_state::configured_flags(cfg.forks);
    on.height = tip_height;
    on.timestamp = tip.timestamp();
    on.previous_timestamp = prev.timestamp();
    on.period_start_timestamp = tip.timestamp();
    on.work_required = tip.bits();
    on.minimum_block_version = 0;
    on.median_time_past = 0;

    BOOST_REQUIRE(to_bool(on.flags & flags::bip54_rule));
    BOOST_REQUIRE(on.is_early_timestamp(tip.timestamp(),
        retargeting_interval));
    const auto ec_on = tip.accept(on, retargeting_interval);
    BOOST_REQUIRE_EQUAL(ec_on, error::early_timestamp);

    context off = on;
    off.flags = flags::no_rules;
    BOOST_REQUIRE(!off.is_early_timestamp(tip.timestamp(),
        retargeting_interval));
    const auto ec_off = tip.accept(off, retargeting_interval);
    BOOST_REQUIRE(ec_off != error::early_timestamp);
    BOOST_REQUIRE(ec_off != error::negative_period_duration);
}

// gates
// ----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(bip54__header_accept__stale_ctx_timewarp_off__success)
{
    constexpr auto prev = 1'000'000u;
    constexpr auto attack = possible_narrow_cast<uint32_t>(
        prev - max_timewarp_bip54 - 1u);
    constexpr auto retargeting_interval = 4u;

    const header attack_header
    {
        1u, null_hash, null_hash, attack, 0x207fffffu, 0u
    };

    context off
    {
        flags::no_rules,
        prev,
        0u,
        0u,
        1u,
        0x207fffffu,
        prev,
        0u
    };
    BOOST_REQUIRE(!off.is_early_timestamp(attack, retargeting_interval));
    BOOST_REQUIRE(attack_header.accept(off, retargeting_interval) !=
        error::early_timestamp);
}

BOOST_AUTO_TEST_CASE(bip54__header_accept__stale_ctx_timewarp_on__early_timestamp)
{
    constexpr auto prev = 1'000'000u;
    constexpr auto attack = possible_narrow_cast<uint32_t>(
        prev - max_timewarp_bip54 - 1u);
    constexpr auto retargeting_interval = 4u;

    const header attack_header
    {
        1u, null_hash, null_hash, attack, 0x207fffffu, 0u
    };

    context on
    {
        flags::bip54_rule,
        prev,
        0u,
        0u,
        1u,
        0x207fffffu,
        prev,
        0u
    };
    BOOST_REQUIRE(on.is_early_timestamp(attack, retargeting_interval));
    BOOST_REQUIRE_EQUAL(attack_header.accept(on, retargeting_interval),
        error::early_timestamp);
}

BOOST_AUTO_TEST_CASE(bip54__header_accept__stale_ctx_negative_period_off__success)
{
    constexpr auto period_start = 1'000u;
    constexpr auto attack = period_start - 1u;
    constexpr auto retargeting_interval = 4u;

    const header attack_header
    {
        1u, null_hash, null_hash, attack, 0x207fffffu, 0u
    };

    context off
    {
        flags::no_rules,
        period_start,
        0u,
        sub1(retargeting_interval),
        1u,
        0x207fffffu,
        0u,
        period_start
    };
    BOOST_REQUIRE(!off.is_negative_period_duration(attack, retargeting_interval));
    BOOST_REQUIRE(attack_header.accept(off, retargeting_interval) !=
        error::negative_period_duration);
}

BOOST_AUTO_TEST_CASE(bip54__header_accept__stale_ctx_negative_period_on__negative_period_duration)
{
    constexpr auto period_start = 1'000u;
    constexpr auto attack = period_start - 1u;
    constexpr auto retargeting_interval = 4u;

    const header attack_header
    {
        1u, null_hash, null_hash, attack, 0x207fffffu, 0u
    };

    context on
    {
        flags::bip54_rule,
        period_start,
        0u,
        sub1(retargeting_interval),
        1u,
        0x207fffffu,
        0u,
        period_start
    };
    BOOST_REQUIRE(on.is_negative_period_duration(attack, retargeting_interval));
    BOOST_REQUIRE_EQUAL(attack_header.accept(on, retargeting_interval),
        error::negative_period_duration);
}

BOOST_AUTO_TEST_CASE(bip54__transaction_check__tx_size_64_off__success)
{
    auto bad64 = load_bad64_tx();
    BOOST_REQUIRE_EQUAL(bad64.serialized_size(false), invalid_nonwitness_tx_size);

    const auto off = bip54_ctx(false);
    BOOST_REQUIRE_EQUAL(bad64.check(off), error::transaction_success);
    BOOST_REQUIRE_EQUAL(bad64.check_guard(off), error::invalid_tx_size_64);
}

BOOST_AUTO_TEST_CASE(bip54__transaction_check__tx_size_64_on__invalid_tx_size_64)
{
    auto bad64 = load_bad64_tx();
    BOOST_REQUIRE_EQUAL(bad64.serialized_size(false), invalid_nonwitness_tx_size);

    const auto on = bip54_ctx(true);
    BOOST_REQUIRE_EQUAL(bad64.check(on), error::invalid_tx_size_64);
    BOOST_REQUIRE_EQUAL(bad64.check_guard(on), error::invalid_tx_size_64);
}

BOOST_AUTO_TEST_CASE(bip54__transaction_check__coinbase_size_64_off__success)
{
    auto cb = make_64byte_coinbase();
    BOOST_REQUIRE(cb.is_coinbase());
    BOOST_REQUIRE_EQUAL(cb.serialized_size(false), invalid_nonwitness_tx_size);

    const auto off = bip54_ctx(false, 1u);
    BOOST_REQUIRE_EQUAL(cb.check(off), error::transaction_success);
    BOOST_REQUIRE_EQUAL(cb.check_guard(off), error::invalid_tx_size_64);
}

BOOST_AUTO_TEST_CASE(bip54__transaction_check__coinbase_size_64_on__invalid_tx_size_64)
{
    auto cb = make_64byte_coinbase();
    BOOST_REQUIRE(cb.is_coinbase());
    BOOST_REQUIRE_EQUAL(cb.serialized_size(false), invalid_nonwitness_tx_size);

    const auto on = bip54_ctx(true, 1u);
    BOOST_REQUIRE_EQUAL(cb.check(on), error::invalid_tx_size_64);
}

BOOST_AUTO_TEST_CASE(bip54__transaction_check__coinbase_lock_off__success)
{
    constexpr auto height = 100u;
    const auto unlocked = make_bad_locktime_coinbase(height);
    BOOST_REQUIRE(unlocked.is_coinbase());
    BOOST_REQUIRE(unlocked.serialized_size(false) != invalid_nonwitness_tx_size);

    const auto off = bip54_ctx(false, height);
    BOOST_REQUIRE_EQUAL(unlocked.check(off), error::transaction_success);
}

BOOST_AUTO_TEST_CASE(bip54__transaction_check__coinbase_lock_on__invalid_coinbase_locktime)
{
    constexpr auto height = 100u;
    const auto unlocked = make_bad_locktime_coinbase(height);
    BOOST_REQUIRE(unlocked.is_coinbase());
    BOOST_REQUIRE(unlocked.serialized_size(false) != invalid_nonwitness_tx_size);

    const auto on = bip54_ctx(true, height);
    BOOST_REQUIRE_EQUAL(unlocked.check(on), error::invalid_coinbase_locktime);
}

BOOST_AUTO_TEST_CASE(bip54__transaction_check__coinbase_sequence_off__success)
{
    constexpr auto height = 100u;
    const auto final_seq = make_final_sequence_coinbase(height);
    BOOST_REQUIRE(final_seq.is_coinbase());
    BOOST_REQUIRE(final_seq.serialized_size(false) != invalid_nonwitness_tx_size);

    const auto off = bip54_ctx(false, height);
    BOOST_REQUIRE_EQUAL(final_seq.check(off), error::transaction_success);
}

BOOST_AUTO_TEST_CASE(bip54__transaction_check__coinbase_sequence_on__invalid_coinbase_sequence)
{
    constexpr auto height = 100u;
    const auto final_seq = make_final_sequence_coinbase(height);
    BOOST_REQUIRE(final_seq.is_coinbase());
    BOOST_REQUIRE(final_seq.serialized_size(false) != invalid_nonwitness_tx_size);

    const auto on = bip54_ctx(true, height);
    BOOST_REQUIRE_EQUAL(final_seq.check(on), error::invalid_coinbase_sequence);
}

BOOST_AUTO_TEST_SUITE_END()
