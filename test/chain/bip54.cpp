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

// gates
// ----------------------------------------------------------------------------

constexpr context bip54_ctx(bool engaged, size_t height = 0) NOEXCEPT
{
    context ctx{};
    ctx.flags = engaged ? flags::bip54_rule : flags::no_rules;
    ctx.height = height;
    return ctx;
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
