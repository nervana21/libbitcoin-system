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
