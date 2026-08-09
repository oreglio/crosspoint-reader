#include "CountdownRing.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
}  // namespace

void drawCountdownRing(const GfxRenderer& renderer, const int cx, const int cy, const int outerRadius, const int stroke,
                       const float fractionRemaining) {
  if (outerRadius <= 0 || stroke <= 0) return;

  const float fraction = std::clamp(fractionRemaining, 0.0f, 1.0f);
  if (fraction <= 0.0f) return;

  const int innerRadius = std::max(outerRadius - stroke, 1);
  const float sweep = fraction * kTwoPi;

  // One radial spoke per step. Half a pixel of arc between spokes at the outer
  // edge (step = 0.5 / outerRadius radians) is what keeps the rim solid instead
  // of dotted. For a 100px radius that is ~1250 spokes of a few pixels each,
  // drawn once a minute — the C3 has no FPU so the sines are software, which
  // costs single-digit milliseconds at this cadence.
  const float step = 0.5f / static_cast<float>(outerRadius);

  for (float angle = 0.0f; angle < sweep; angle += step) {
    const float s = sinf(angle);
    const float c = cosf(angle);
    // Clockwise from 12 o'clock: x follows sin, y follows -cos.
    const int x0 = cx + static_cast<int>(s * static_cast<float>(innerRadius));
    const int y0 = cy - static_cast<int>(c * static_cast<float>(innerRadius));
    const int x1 = cx + static_cast<int>(s * static_cast<float>(outerRadius));
    const int y1 = cy - static_cast<int>(c * static_cast<float>(outerRadius));
    renderer.drawLine(x0, y0, x1, y1, true);
  }
}
