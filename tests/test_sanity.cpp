#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// C++20 sets __cplusplus to 202002L, and C++23 is officially 202302L. Some
// compilers report something in between for -std=c++23 (GCC 13 gives 202100L),
// so the check is "newer than C++20" rather than an exact match.
static_assert(__cplusplus > 202002L, "C++23 is not enabled");

TEST_CASE("toolchain sanity", "[sanity]")
{
    int a = 1;
    REQUIRE(a + 1 == 2);
}

TEST_CASE("std library links and runs", "[sanity]")
{
    std::vector<std::string> v{"bunny", "pooh"};
    REQUIRE(v.size() == 2u);
    CHECK(v.front() == "bunny");
}


