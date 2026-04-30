#include "OpenOCDPortForwardPorts.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using openocd_port_forward::parsePorts;
using openocd_port_forward::trim;

TEST(TrimTest, NoWhitespaceUnchanged)
{
    EXPECT_EQ(trim("4444"), "4444");
}

TEST(TrimTest, LeadingAndTrailingTrimmed)
{
    EXPECT_EQ(trim("   4444\t\n"), "4444");
}

TEST(TrimTest, EmptyAndAllWhitespace)
{
    EXPECT_EQ(trim(""), "");
    EXPECT_EQ(trim("   \t  "), "");
}

TEST(ParsePortsTest, SingleColonSeparated)
{
    std::vector<uint16_t> expected = {4444, 6666, 10000};
    EXPECT_EQ(parsePorts("10000:4444:6666"), expected);
}

TEST(ParsePortsTest, AcceptsCommaAndSemicolon)
{
    std::vector<uint16_t> expected = {4444, 6666, 10000};
    EXPECT_EQ(parsePorts("10000,4444;6666"), expected);
}

TEST(ParsePortsTest, ResultsAreSortedAndDeduped)
{
    std::vector<uint16_t> expected = {4444, 6666, 10000, 10001, 10002};
    EXPECT_EQ(parsePorts("10001:10000:10002:4444:6666:10001:4444"), expected);
}

TEST(ParsePortsTest, IgnoresEmptyTokens)
{
    std::vector<uint16_t> expected = {4444, 6666};
    EXPECT_EQ(parsePorts("::4444:::6666::"), expected);
}

TEST(ParsePortsTest, TrimsWhitespaceAroundTokens)
{
    std::vector<uint16_t> expected = {4444, 6666};
    EXPECT_EQ(parsePorts("  4444 : \t6666\n"), expected);
}

TEST(ParsePortsTest, EmptyInputRejected)
{
    EXPECT_TRUE(parsePorts("").empty());
    EXPECT_TRUE(parsePorts(":::").empty());
}

TEST(ParsePortsTest, NonNumericTokenRejected)
{
    EXPECT_TRUE(parsePorts("4444:abc").empty());
    EXPECT_TRUE(parsePorts("4444a").empty());
}

TEST(ParsePortsTest, ZeroPortRejected)
{
    EXPECT_TRUE(parsePorts("0").empty());
    EXPECT_TRUE(parsePorts("4444:0").empty());
}

TEST(ParsePortsTest, AbovePortRangeRejected)
{
    EXPECT_TRUE(parsePorts("65536").empty());
    EXPECT_TRUE(parsePorts("4444:99999").empty());
}

TEST(ParsePortsTest, OverflowingValueRejected)
{
    EXPECT_TRUE(parsePorts("99999999999999999999999").empty());
}
