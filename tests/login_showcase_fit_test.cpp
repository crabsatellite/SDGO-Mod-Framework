#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

#include "mods/startup_crash_fix/model_fit.h"

namespace
{
    unsigned checks = 0;

    void Check(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
            throw std::runtime_error(message);
    }

    bool Near(float left, float right, float tolerance = 0.001f)
    {
        return std::fabs(left - right) <= tolerance;
    }
}

int main()
{
    using namespace sdgo::login_showcase_fit;
    try {
        float scale = 0.0f;
        Check(ReplacementScale(
                  {-5.119f, -0.507f, -9.500f,
                   5.119f, 8.142f, 4.747f},
                  20.0f, scale) &&
                  Near(scale, 20.0f),
              "a small replacement retains native scale without enlargement");

        Check(ReplacementScale(
                  {-18.860f, -1.599f, -23.781f,
                   18.800f, 29.676f, 12.246f},
                  20.0f, scale) &&
                  Near(scale, 20.0f * 26.0f / 37.660f),
              "an oversized replacement is uniformly clamped by its tightest axis");

        Check(ReplacementScale(
                  {-16.417f, -4.297f, -21.920f,
                   16.410f, 25.663f, 19.995f},
                  12.0f, scale) &&
                  Near(scale, 12.0f),
              "the fit never enlarges a model already below the bounded scale");

        const float nan = std::numeric_limits<float>::quiet_NaN();
        Check(!ReplacementScale(
                   {nan, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
                   20.0f, scale),
              "non-finite native bounds are rejected");
        Check(!ReplacementScale(
                   {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f},
                   20.0f, scale),
              "degenerate native bounds are rejected");
        Check(!ReplacementScale(
                   {0.0f, 0.0f, 0.0f, 20000.0f, 1.0f, 1.0f},
                   20.0f, scale),
              "implausible native bounds are rejected");

        std::printf("%u login showcase fit checks passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
