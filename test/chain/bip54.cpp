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

BOOST_AUTO_TEST_SUITE(bip54_tests)

using namespace system::chain;

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

BOOST_AUTO_TEST_SUITE_END()
