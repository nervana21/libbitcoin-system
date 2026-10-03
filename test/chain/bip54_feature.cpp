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

BOOST_AUTO_TEST_SUITE(bip54_feature_tests)

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

transaction decode_tx(const std::string& hex)
{
    return transaction{ decode_hex_chunk(hex), true };
}

transaction make_oversigops_tx(size_t sigops)
{
    operations bomb;
    bomb.reserve(sigops);
    for (size_t n = 0; n < sigops; ++n)
        bomb.emplace_back(opcode::checksig);

    input in{ point{ null_hash, 0u }, script{}, 0xffffffff };
    in.prevout = to_shared<output>(1_u64, script{ bomb });
    return transaction
    {
        1u,
        inputs{ std::move(in) },
        outputs{ output{ 1_u64, script{} } },
        0u
    };
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

} // namespace

BOOST_AUTO_TEST_CASE(bip54_feature__sigops_single__flag_matrix)
{
    auto over = make_oversigops_tx(max_tx_bip54_sigops + 1);
    BOOST_REQUIRE(over.bip54_signature_operations() > max_tx_bip54_sigops);

    const auto off = bip54_ctx(false);
    BOOST_REQUIRE_EQUAL(over.accept(off), error::transaction_success);
    BOOST_REQUIRE_EQUAL(over.accept_guard(off), error::bip54_sigop_limit);

    const auto on = bip54_ctx(true);
    BOOST_REQUIRE_EQUAL(over.accept(on), error::bip54_sigop_limit);
    BOOST_REQUIRE_EQUAL(over.accept_guard(on), error::bip54_sigop_limit);
}

BOOST_AUTO_TEST_CASE(bip54_feature__sigops_split__each_within_limit)
{
    auto left = make_oversigops_tx(max_tx_bip54_sigops);
    auto right = make_oversigops_tx(1);
    BOOST_REQUIRE_EQUAL(left.bip54_signature_operations(), max_tx_bip54_sigops);
    BOOST_REQUIRE_EQUAL(right.bip54_signature_operations(), 1u);

    auto on = bip54_ctx(true);
    on.flags |= flags::bip16_rule;
    BOOST_REQUIRE_EQUAL(left.accept(on), error::transaction_success);
    BOOST_REQUIRE_EQUAL(right.accept(on), error::transaction_success);
}

// ctx.timestamp is deliberately stale (not the header time). header.accept must
// pass header.timestamp() into the BIP54 helpers so a mismatched context cannot
// skip the check.
BOOST_AUTO_TEST_CASE(bip54_feature__timewarp__flag_matrix)
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

    context off = on;
    off.flags = flags::no_rules;
    BOOST_REQUIRE(!off.is_early_timestamp(attack, retargeting_interval));
    BOOST_REQUIRE(attack_header.accept(off, retargeting_interval) !=
        error::early_timestamp);
}

// Same stale ctx.timestamp contract as the timewarp flag matrix above.
BOOST_AUTO_TEST_CASE(bip54_feature__murch_zawy__flag_matrix)
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
    BOOST_REQUIRE(on.is_negative_interval(attack, retargeting_interval));
    BOOST_REQUIRE_EQUAL(attack_header.accept(on, retargeting_interval),
        error::negative_interval);

    context off = on;
    off.flags = flags::no_rules;
    BOOST_REQUIRE(!off.is_negative_interval(attack, retargeting_interval));
    BOOST_REQUIRE(attack_header.accept(off, retargeting_interval) !=
        error::negative_interval);
}

BOOST_AUTO_TEST_CASE(bip54_feature__coinbase_lock__flag_matrix)
{
    constexpr auto height = 100u;
    const auto unlocked = make_bad_locktime_coinbase(height);
    BOOST_REQUIRE(unlocked.is_coinbase());
    BOOST_REQUIRE(unlocked.serialized_size(false) != invalid_tx_nonwitness_size);

    const auto off = bip54_ctx(false, height);
    BOOST_REQUIRE_EQUAL(unlocked.check(off), error::transaction_success);

    const auto on = bip54_ctx(true, height);
    BOOST_REQUIRE_EQUAL(unlocked.check(on), error::invalid_coinbase_locktime);
}

BOOST_AUTO_TEST_CASE(bip54_feature__coinbase_sequence__flag_matrix)
{
    constexpr auto height = 100u;
    const auto final_seq = make_final_sequence_coinbase(height);
    BOOST_REQUIRE(final_seq.is_coinbase());
    BOOST_REQUIRE(final_seq.serialized_size(false) != invalid_tx_nonwitness_size);

    const auto off = bip54_ctx(false, height);
    BOOST_REQUIRE_EQUAL(final_seq.check(off), error::transaction_success);

    const auto on = bip54_ctx(true, height);
    BOOST_REQUIRE_EQUAL(final_seq.check(on), error::invalid_coinbase_sequence);
}

BOOST_AUTO_TEST_CASE(bip54_feature__tx_size_64__flag_matrix)
{
    const auto root = load_json("txsize.json");
    std::optional<transaction> bad64;
    for (const auto& case_: root.as_array())
    {
        if (!case_.at("valid").as_bool())
        {
            bad64 = decode_tx(std::string(case_.at("tx").as_string()));
            break;
        }
    }
    BOOST_REQUIRE(bad64.has_value());
    BOOST_REQUIRE_EQUAL(bad64->serialized_size(false), invalid_tx_nonwitness_size);

    const auto off = bip54_ctx(false);
    BOOST_REQUIRE_EQUAL(bad64->check(off), error::transaction_success);
    BOOST_REQUIRE_EQUAL(bad64->check_guard(off), error::invalid_tx_size_64);

    const auto on = bip54_ctx(true);
    BOOST_REQUIRE_EQUAL(bad64->check(on), error::invalid_tx_size_64);
    BOOST_REQUIRE_EQUAL(bad64->check_guard(on), error::invalid_tx_size_64);
}

BOOST_AUTO_TEST_CASE(bip54_feature__coinbase_size_64__flag_matrix)
{
    auto cb = make_64byte_coinbase();
    BOOST_REQUIRE(cb.is_coinbase());
    BOOST_REQUIRE_EQUAL(cb.serialized_size(false), invalid_tx_nonwitness_size);

    const auto off = bip54_ctx(false, 1u);
    BOOST_REQUIRE_EQUAL(cb.check(off), error::transaction_success);
    BOOST_REQUIRE_EQUAL(cb.check_guard(off), error::invalid_tx_size_64);

    const auto on = bip54_ctx(true, 1u);
    BOOST_REQUIRE_EQUAL(cb.check(on), error::invalid_tx_size_64);
}

BOOST_AUTO_TEST_SUITE_END()
