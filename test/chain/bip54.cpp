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

transaction decode_tx(const std::string& hex)
{
    return transaction{ decode_hex_chunk(hex), true };
}

output decode_output(const std::string& hex)
{
    return output{ decode_hex_chunk(hex) };
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

BOOST_AUTO_TEST_SUITE_END()
