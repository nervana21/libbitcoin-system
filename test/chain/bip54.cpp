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

BC_AUTO_TEST_SUITE(bip54_tests)

using namespace system::chain;

namespace {

constexpr uint32_t bip54_flags =
    flags::bip54_timewarp_rule |
    flags::bip54_timespan_rule |
    flags::bip54_sigops_rule |
    flags::bip54_txsize_rule |
    flags::bip54_coinbase_rule;

constexpr context bip54_ctx(bool engaged, size_t height = 0) NOEXCEPT
{
    context ctx{};
    ctx.flags = engaged ? bip54_flags : flags::no_rules;
    ctx.height = height;
    return ctx;
}

} // namespace

// header::accept currently reads ctx.timestamp for BIP54 timewarp and
// negative timespan. A stale safe ctx.timestamp skips the header's own stamp.
// ----------------------------------------------------------------------------

BC_AUTO_TEST_CASE(bip54__accept__stale_ctx_timewarp_on__skips_early_timestamp)
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
    // Stale ctx.timestamp looks safe; accept currently uses it.
    on.timestamp = prev_block_timestamp;
    BC_REQUIRE(!on.is_early_timestamp(retargeting_interval));
    BC_REQUIRE(attack_header.accept(on, retargeting_interval) !=
        error::early_timestamp); // TODO: should be early_timestamp
}

BC_AUTO_TEST_CASE(bip54__accept__stale_ctx_negative_timespan_on__skips_negative_timespan)
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
    // Stale ctx.timestamp looks safe; accept currently uses it.
    on.timestamp = period_start;
    BC_REQUIRE(!on.is_negative_timespan(retargeting_interval));
    BC_REQUIRE(attack_header.accept(on, retargeting_interval) !=
        error::negative_timespan); // TODO: should be negative_timespan
}

BC_AUTO_TEST_SUITE_END()
