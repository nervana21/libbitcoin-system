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

BOOST_AUTO_TEST_SUITE(bip54_tests)

using namespace system::chain;

namespace {

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

} // namespace

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

BOOST_AUTO_TEST_SUITE_END()
