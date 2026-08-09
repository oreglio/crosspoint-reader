#pragma once

class GfxRenderer;

// A ring that starts whole and drains clockwise from 12 o'clock as the countdown
// runs. fractionRemaining is clamped to [0, 1]; at 0 nothing is drawn.
//
// GfxRenderer::drawArc() cannot serve here: it takes xDir/yDir quadrant
// selectors rather than angles and fills whole quadrants only.
void drawCountdownRing(const GfxRenderer& renderer, int cx, int cy, int outerRadius, int stroke,
                       float fractionRemaining);
