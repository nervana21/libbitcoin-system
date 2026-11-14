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
#ifndef LIBBITCOIN_SYSTEM_CHAIN_CONTEXT_HPP
#define LIBBITCOIN_SYSTEM_CHAIN_CONTEXT_HPP

#include <bitcoin/system/define.hpp>
#include <bitcoin/system/chain/enums/flags.hpp>
#include <bitcoin/system/chain/enums/magic_numbers.hpp>
#include <bitcoin/system/chain/enums/policy.hpp>

namespace libbitcoin {
namespace system {
namespace chain {

class BC_API context final
{
public:
    /// Determine if the flag is active for this block.
    inline bool is_enabled(chain::flags flag) const NOEXCEPT
    {
        return to_bool(flag & flags);
    }

    // ************************************************************************
    // CONSENSUS: Soft forks imposed minimum block versioning using a signed
    // interpretation of header.version, which would otherwise be unsigned.
    // ************************************************************************
    inline bool is_insufficient_version(uint32_t version) const NOEXCEPT
    {
        return is_nonzero(minimum_block_version) &&
            to_signed(version) < to_signed(minimum_block_version);
    }

    inline bool is_anachronistic_timestamp(uint32_t time_stamp) const NOEXCEPT
    {
        return time_stamp <= median_time_past;
    }

    inline bool is_invalid_work(uint32_t bits) const NOEXCEPT
    {
        return bits != work_required;
    }

    // True when the candidate block stamp is too early on a retarget height.
    // block_timestamp is the header stamp under judgment, not ctx.timestamp.
    // Callers must pass the header stamp so a stale ctx.timestamp cannot skip.
    inline bool is_early_timestamp(uint32_t block_timestamp,
        uint32_t retargeting_interval) const NOEXCEPT
    {
        // BIP94 time warp patch on testnet4 uses the wider max_timewarp bound.
        const auto bip94 = is_enabled(chain::flags::time_warp_patch);

        // BIP54 timewarp rule uses the tighter max_timewarp_bip54 bound.
        const auto bip54 = is_enabled(chain::flags::bip54_timewarp_rule);

        // Prefer the BIP94 bound when both rules are active.
        const auto limit = bip94 ? max_timewarp : max_timewarp_bip54;

        // Only retarget heights enforce the early stamp floor.
        const auto at_retarget_height = is_zero(height % retargeting_interval);

        // Floor is previous_timestamp minus limit, never underflows.
        const auto earliest_allowed = floored_subtract<uint32_t>(
            previous_timestamp, limit);

        // Compare the header stamp argument, never the ctx.timestamp field.
        const auto stamp_too_early = block_timestamp < earliest_allowed;

        return (bip94 || bip54) && at_retarget_height && stamp_too_early;
    }

    // True when the last block of a BIP54 period stamps before the period start.
    // block_timestamp is the header stamp under judgment, not ctx.timestamp.
    // Callers must pass the header stamp so a stale ctx.timestamp cannot skip.
    inline bool is_negative_timespan(uint32_t block_timestamp,
        uint32_t retargeting_interval) const NOEXCEPT
    {
        // Last height of a period is interval minus one modulo interval.
        const auto last_height_of_period =
            height % retargeting_interval == sub1(retargeting_interval);

        // Compare the header stamp argument against the period first stamp.
        const auto stamp_before_period_start =
            block_timestamp < retarget_timestamp;

        return last_height_of_period && stamp_before_period_start;
    }

    /// Header context within chain.
    uint32_t flags{};
    uint32_t timestamp{};
    uint32_t median_time_past{};
    size_t height{};
    uint32_t minimum_block_version{};
    uint32_t work_required{};
    uint32_t previous_timestamp{};
    uint32_t retarget_timestamp{};
};

BC_API bool operator==(const context& left, const context& right) NOEXCEPT;
BC_API bool operator!=(const context& left, const context& right) NOEXCEPT;

} // namespace chain
} // namespace system
} // namespace libbitcoin

#endif
