/**
 * Copyright (c) libbitcoin developers
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
#include <random>
#include <boost/json.hpp>

BOOST_AUTO_TEST_SUITE(bip54_tests)

using namespace system::chain;
namespace json = boost::json;

namespace {

std::filesystem::path bip54_vector(const std::string& name) NOEXCEPT
{
    return std::filesystem::path(__FILE__).parent_path().parent_path()
        / "data" / "bip54" / name;
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

output decode_output(const std::string& hex)
{
    return output{ decode_hex_chunk(hex) };
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
            ctx.is_negative_interval(hdr.timestamp(), interval);

        const auto ec = hdr.accept(ctx, interval);
        const bool accept_bad = (ec == error::early_timestamp) ||
            (ec == error::negative_interval);

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

} // namespace

BOOST_AUTO_TEST_CASE(bip54__settings__regtest_on_mainnet_off)
{
    BOOST_REQUIRE(settings(selection::regtest).forks.bip54);
    BOOST_REQUIRE(!settings(selection::mainnet).forks.bip54);
    BOOST_REQUIRE(!settings(selection::testnet3).forks.bip54);
    BOOST_REQUIRE(!settings(selection::testnet4).forks.bip54);
}

BOOST_AUTO_TEST_CASE(bip54__configured_flags__bip54_fork__flagged)
{
    forks configured{};
    configured.bip54 = true;
    BOOST_REQUIRE(to_bool(
        chain_state::configured_flags(configured) & flags::bip54_rule));
}

BOOST_AUTO_TEST_CASE(bip54__configured_flags__bip54_off__clear)
{
    forks configured{};
    configured.bip54 = false;
    BOOST_REQUIRE(!to_bool(
        chain_state::configured_flags(configured) & flags::bip54_rule));
}

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

BOOST_AUTO_TEST_CASE(bip54__early_timestamp__bip54_grace_7200)
{
    constexpr auto prev = 10'000u;
    constexpr auto limit = possible_narrow_cast<uint32_t>(
        prev - max_timewarp_bip54);
    context ctx{ flags::bip54_rule, sub1(limit), 0, 2016, 0, 0, prev, 0 };
    BOOST_REQUIRE(ctx.is_early_timestamp(2016));

    ctx.timestamp = limit;
    BOOST_REQUIRE(!ctx.is_early_timestamp(2016));
}

BOOST_AUTO_TEST_CASE(bip54__early_timestamp__bip94_and_bip54__tighter_grace)
{
    constexpr auto prev = 1'000'000u;
    constexpr auto retargeting_interval = 4u;
    context both
    {
        flags::bip54_rule | flags::time_warp_patch,
        0u, 0u, retargeting_interval, 0u, 0u, prev
    };

    BOOST_REQUIRE(both.is_early_timestamp(
        prev - max_timewarp_testnet4 - 1u, retargeting_interval));
    BOOST_REQUIRE(!both.is_early_timestamp(
        prev - max_timewarp_testnet4, retargeting_interval));
}

BOOST_AUTO_TEST_CASE(bip54__testnet3_timewarp_pin__bip54_rejects)
{
    // BIP54 README testnet3 timewarp pin. Height 8064 is the period start.
    // The previous header supplies previous_timestamp. This is not Core's
    // regtest BIP94 short-period harness.
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
    BOOST_REQUIRE(ec_off != error::negative_interval);
}

BOOST_AUTO_TEST_CASE(bip54__chain_state__regtest_promotes_retarget_for_murch_zawy)
{
    settings cfg{ selection::regtest };
    BOOST_REQUIRE(cfg.forks.bip54);
    BOOST_REQUIRE(!cfg.forks.retarget);

    cfg.block_spacing_seconds = 1;
    cfg.retargeting_interval_seconds = 4;
    constexpr auto retargeting_interval = 4u;
    BOOST_REQUIRE_EQUAL(cfg.retargeting_interval(), retargeting_interval);

    constexpr auto period_start = 1'000u;
    chain_state::data genesis_data{};
    genesis_data.height = 0;
    genesis_data.hash = one_hash;
    genesis_data.timestamp.self = period_start;
    genesis_data.bits.self = 0x207fffffu;
    genesis_data.bits.ordered.push_back(genesis_data.bits.self);
    genesis_data.version.self = 1u;
    genesis_data.version.ordered.push_back(1u);

    const chain_state genesis{ std::move(genesis_data), cfg };

    const header header1
    {
        1u, genesis.hash(), null_hash, period_start + 1u, 0x207fffffu, 0u
    };
    const chain_state state1{ genesis, header1, cfg };
    BOOST_REQUIRE_EQUAL(state1.height(), 1u);
    BOOST_REQUIRE_EQUAL(state1.period_start_timestamp(), period_start);

    const header header2
    {
        1u, state1.hash(), null_hash, period_start + 2u, 0x207fffffu, 0u
    };
    const chain_state state2{ state1, header2, cfg };
    BOOST_REQUIRE_EQUAL(state2.height(), 2u);
    BOOST_REQUIRE_EQUAL(state2.period_start_timestamp(), period_start);

    constexpr auto early = period_start - 1u;
    const header end_header
    {
        1u, state2.hash(), null_hash, early, 0x207fffffu, 0u
    };
    const chain_state end_state{ state2, end_header, cfg };
    const auto ctx = end_state.context();

    BOOST_REQUIRE_EQUAL(ctx.height, sub1(retargeting_interval));
    BOOST_REQUIRE_EQUAL(ctx.period_start_timestamp, period_start);
    BOOST_REQUIRE_EQUAL(ctx.timestamp, early);
    BOOST_REQUIRE(ctx.is_enabled(flags::bip54_rule));
    BOOST_REQUIRE(ctx.is_negative_interval(retargeting_interval));
}

BOOST_AUTO_TEST_CASE(bip54__negative_interval__period_end)
{
    context ctx{ flags::bip54_rule, 100, 0, 2015, 0, 0, 0, 101 };
    BOOST_REQUIRE(ctx.is_negative_interval(2016));

    ctx.timestamp = 101;
    BOOST_REQUIRE(!ctx.is_negative_interval(2016));
}

BOOST_AUTO_TEST_CASE(bip54__sigops_vectors__match_expected)
{
    const auto root = load_json("sigops.json");
    size_t index{};
    for (const auto& case_: root.as_array())
    {
        ++index;
        auto tx = decode_tx(std::string(case_.at("tx").as_string()));
        const auto& spent = case_.at("spent_outputs").as_array();
        BOOST_REQUIRE_EQUAL(spent.size(), tx.inputs());

        size_t i{};
        for (auto& input: *tx.inputs_ptr())
        {
            auto prev = decode_output(std::string(spent.at(i).as_string()));
            input->prevout = to_shared(std::move(prev));
            ++i;
        }

        const auto within = tx.bip54_signature_operations() <= max_tx_bip54_sigops;
        const auto expected = case_.at("valid").as_bool();
        BOOST_REQUIRE_MESSAGE(within == expected,
            "sigops case " + std::to_string(index) + ": " +
            std::string(case_.at("comment").as_string()));

        context ctx{};
        ctx.flags = flags::bip54_rule | flags::bip16_rule;
        const auto ec = tx.accept(ctx);
        if (expected)
            BOOST_REQUIRE_EQUAL(ec, error::transaction_success);
        else
            BOOST_REQUIRE_EQUAL(ec, error::bip54_sigop_limit);
    }
}

BOOST_AUTO_TEST_CASE(bip54__sigops__p2sh_truncated_scriptsig__no_redeem_count)
{
    data_chunk sig_bytes{ 0x4d, 0xc6, 0x09 };
    sig_bytes.insert(sig_bytes.end(), 2501, 0xac);
    script script_sig{ sig_bytes, false };
    BOOST_REQUIRE(script_sig.is_underflow());

    data_chunk p2sh_bytes{ 0xa9, 0x14 };
    p2sh_bytes.insert(p2sh_bytes.end(), 20, 0x11);
    p2sh_bytes.push_back(0x87);
    script p2sh{ p2sh_bytes, false };
    BOOST_REQUIRE(script::is_pay_script_hash_pattern(p2sh.ops()));

    input in{ point{ null_hash, 0u }, script_sig, 0xffffffff };
    in.prevout = to_shared<output>(1_u64, p2sh);
    transaction tx
    {
        1u,
        inputs{ std::move(in) },
        outputs{ output{ 1_u64, script{} } },
        0u
    };

    BOOST_REQUIRE_EQUAL(tx.bip54_signature_operations(), 0u);

    context ctx{};
    ctx.flags = flags::bip54_rule | flags::bip16_rule;
    BOOST_REQUIRE_EQUAL(tx.accept(ctx), error::transaction_success);
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
            invalid_tx_nonwitness_size;
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

BOOST_AUTO_TEST_CASE(bip54__e2e__flag_gates_block_rules_pool_always_on)
{
    const auto txsize = load_json("txsize.json");
    std::optional<transaction> bad64;
    for (const auto& case_: txsize.as_array())
    {
        if (!case_.at("valid").as_bool())
        {
            bad64 = decode_tx(std::string(case_.at("tx").as_string()));
            break;
        }
    }
    BOOST_REQUIRE(bad64.has_value());

    operations bomb;
    bomb.reserve(max_tx_bip54_sigops + 1);
    for (size_t n = 0; n <= max_tx_bip54_sigops; ++n)
        bomb.emplace_back(opcode::checksig);
    input over_in{ point{ null_hash, 0u }, script{}, 0xffffffff };
    over_in.prevout = to_shared<output>(1_u64, script{ bomb });
    transaction over
    {
        1u,
        inputs{ std::move(over_in) },
        outputs{ output{ 1_u64, script{} } },
        0u
    };
    BOOST_REQUIRE(over.bip54_signature_operations() > max_tx_bip54_sigops);

    context off{};
    off.flags = flags::no_rules;
    BOOST_REQUIRE_EQUAL(bad64->check(off), error::transaction_success);
    BOOST_REQUIRE_EQUAL(over.accept(off), error::transaction_success);
    BOOST_REQUIRE_EQUAL(bad64->check_guard(off), error::invalid_tx_size_64);
    BOOST_REQUIRE_EQUAL(over.accept_guard(off), error::bip54_sigop_limit);

    context on{};
    on.flags = flags::bip54_rule;
    BOOST_REQUIRE_EQUAL(bad64->check(on), error::invalid_tx_size_64);
    BOOST_REQUIRE_EQUAL(over.accept(on), error::bip54_sigop_limit);
    BOOST_REQUIRE_EQUAL(bad64->check_guard(on), error::invalid_tx_size_64);
    BOOST_REQUIRE_EQUAL(over.accept_guard(on), error::bip54_sigop_limit);
}

BOOST_AUTO_TEST_SUITE_END()
