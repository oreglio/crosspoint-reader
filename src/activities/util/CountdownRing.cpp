#include "CountdownRing.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;

int isqrt(const int value) { return value <= 0 ? 0 : static_cast<int>(sqrtf(static_cast<float>(value))); }
}  // namespace

void drawCountdownRing(const GfxRenderer& renderer, const int cx, const int cy, const int outerRadius, const int stroke,
                       const float fractionRemaining) {
  if (outerRadius <= 0 || stroke <= 0) return;

  const float fraction = std::clamp(fractionRemaining, 0.0f, 1.0f);
  if (fraction <= 0.0f) return;

  const int innerRadius = std::max(outerRadius - stroke, 1);
  const int outerSq = outerRadius * outerRadius;
  const int innerSq = innerRadius * innerRadius;

  // Filled row by row rather than as a fan of radial lines. A Bresenham line at
  // 45 degrees advances diagonally and skips every other pixel of the band it
  // crosses, so neighbouring spokes leave a checkerboard between them: measured
  // at 9.5% holes in the diagonal quadrants against 0.3% on the axes, which is
  // exactly the speckle it produced on the panel. A scanline fill covers every
  // pixel of the annulus exactly once.
  const float theta = fraction * kTwoPi;
  const bool full = fraction >= 1.0f;
  const bool wide = theta > static_cast<float>(M_PI);
  const float sinT = sinf(theta);
  const float cosT = cosf(theta);

  for (int dy = -outerRadius; dy <= outerRadius; ++dy) {
    const int dy2 = dy * dy;
    if (dy2 > outerSq) continue;

    const int xOuter = isqrt(outerSq - dy2);
    const int xInner = dy2 < innerSq ? isqrt(innerSq - dy2) : -1;

    for (int dx = -xOuter; dx <= xOuter; ++dx) {
      if (xInner >= 0 && std::abs(dx) < xInner) continue;  // the hollow centre

      if (!full) {
        // Clockwise from 12 o'clock. Rather than an angle per pixel, compare the
        // point against the two boundary directions by cross product: the start
        // (0,-1) gives dx >= 0, and the end (sin, -cos) gives the second test.
        // Below half a turn the point must satisfy both, beyond it either.
        const bool afterStart = dx >= 0;
        const bool beforeEnd = (-static_cast<float>(dx) * cosT - static_cast<float>(dy) * sinT) >= 0.0f;
        if (wide ? !(afterStart || beforeEnd) : !(afterStart && beforeEnd)) continue;
      }

      renderer.drawPixel(cx + dx, cy + dy, true);
    }
  }
}
