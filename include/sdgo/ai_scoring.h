#pragma once

namespace sdgo::ai {
// Native grid-match totals for 1..6 players per team. A supported eight-player
// AI room may have seven on one side; retain the largest native full-bar scale
// in that case instead of leaving the server/client gauge at zero.
inline int GridGauge(int perTeam)
{
    switch (perTeam) {
    case 1: case 2: case 4: return 0x50;
    case 3: return 0x3C;
    case 5: return 0x78;
    case 6: case 7: return 0xA0;
    default: return 0;
    }
}

inline int GridGaugeForTeams(int first, int second)
{
    return GridGauge(first > second ? first : second);
}
}
