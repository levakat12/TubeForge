#pragma once

#include <array>
#include <cstdint>

// Project-owned, redistributable fixed PCM captures for regression testing.
// These samples were rendered once from TubeForge's plucked-string capture model
// and are intentionally stored (rather than regenerated) to catch drift.
namespace nts::test::fixtures
{
inline constexpr std::array<std::int16_t, 64> guitarDi {
      0,  2841,  5417,  7534,  9052,  9890, 10031,  9544,
   8542,  7180,  5656,  4181,  2942,  2063,  1584,  1464,
   1587,  1731,  1666,  1258,   512,  -495, -1625, -2708,
  -3584, -4113, -4215, -3869, -3136, -2134, -1024,    23,
    855,  1359,  1480,  1234,   697,    23,  -608, -1018,
  -1084,  -751,   -80,   784,  1640,  2254,  2437,  2063,
   1139,  -229, -1798, -3256, -4261, -4530, -3907, -2441,
   -422,  1706,  3415,  4230,  3864,  2388,   153, -2241
};

inline constexpr std::array<std::int16_t, 64> bassDi {
      0,  1210,  2404,  3568,  4688,  5745,  6722,  7601,
   8366,  9000,  9488,  9818,  9982,  9969,  9782,  9428,
   8909,  8238,  7433,  6514,  5504,  4430,  3321,  2205,
   1113,    73,  -888, -1745, -2477, -3066, -3497, -3762,
  -3855, -3780, -3545, -3163, -2657, -2059, -1407,  -743,
   -109,   455,   916,  1244,  1417,  1420,  1251,   926,
    478,   -45,  -585, -1077, -1456, -1664, -1655, -1394,
   -877,  -137,   755,  1698,  2572,  3254,  3632,  3620
};
} // namespace nts::test::fixtures
