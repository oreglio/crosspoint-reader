# Pomodoro chaining mode, and a start cue on the figure

Date: 2026-08-09
Status: design approved, not implemented.

## Problem

Two small things on the countdown screens, both surfaced by using them.

**1. Starting a step gives no clear signal.** Until v1.5.33 the figure dropped a
step within a second of starting, which accidentally read as "it's running". The
rounding fix made it hold — correctly — for a full five seconds, and the screen
now looks frozen at the exact moment the user is waiting for a reaction.
`beginRunning()` already forces a full waveform, so the whole panel does flash,
but that reads as "the screen redrew", not as "the timer started".

**2. The chaining is not always wanted.** At the end of a step the screen shows
`+mm:ss` and waits for a press; that press starts the next step immediately.
Some sessions want the next step to wait for its own press — finish work, notice
it, then start the break when actually ready.

## What exists today, verified

- `PomodoroActivity` already has the three states this needs:
  `Gate { Ready, Running, Finished }`. `Ready` waits for a press before the clock
  starts; `Finished` counts `+mm:ss` and waits for a press. Subsequent steps are
  currently prepared with `Gate::Running`, which is the whole of the current
  chaining behaviour.
- `GfxRenderer::invertRect(x, y, w, h)` exists, rotates through
  `rotateCoordinates` for the active orientation, clips to the screen, and is
  already used by `ScreenshotUtil.cpp:99-102`. No new drawing primitive is needed.
- `CrossPointState` persists to `/.crosspoint/state.json` and already carries the
  three pomodoro durations, with defaults applied on read so an older file is
  read as the classic preset rather than as zeroes.
- `OptionSelectionActivity` returns an index and closes; chaining another
  activity from its result handler is supported — `ActivityManager` moves the
  handler into a local before invoking it, precisely for that.
- Both screens duplicate the centre block — value, label, truncation budgets —
  in near-identical code that this change would otherwise duplicate a third time.

## Design

### Chaining mode

A fifth row on the lengths list, carrying its own state:

```
Classique      25 / 5 / 15
Court          15 / 3 / 10
Long           50 / 10 / 20
Personnalisé   25 / 5 / 15
Enchaînement   Automatique     ← selecting it flips the value and reopens the list
```

Selecting it toggles, saves, and reopens the list so the new state is visible
before choosing a length to start with.

| Mode | End of a step | Next step |
|---|---|---|
| Automatic (today, default) | `+mm:ss`, one press | starts counting immediately |
| Manual | `+mm:ss`, one press | waits for a second press to start |

It is one argument at one call site:

```cpp
prepareStep(stepIndex + 1, manualStart ? Gate::Ready : Gate::Running);
```

The mode lives on the list rather than on a screen of its own because a separate
chooser would cost every launch an extra press, including for anyone who never
changes it. On the list it costs only when used.

Persisted as `pomodoroManualStart` in `CrossPointState`, defaulting to false, so
the behaviour shipped in v1.5.32 stays the default.

### Start cue

One inversion of the figure, about 300 ms, when a step begins:

```
press → full waveform (already there)
      → 10:00 inverted, ~300ms
      → 10:00 normal
```

Two fast refreshes, roughly 260 ms, once per start. One blink rather than two:
each toggle is a hard flash on e-ink, and a repeated one reads as a fault rather
than as a confirmation.

Driven from `loop()` through a `blinkUntilMs` deadline and a `requestUpdate()`,
never from `render()` — animating inside the render call would block the render
task for half a second.

Both screens get it: the pomodoro when a step begins, the countdown on the final
press of its picker.

### The duplication this removes

The centre block is extracted next to `CountdownLayout` as

```cpp
void drawCountdownCentre(const GfxRenderer& renderer, const CountdownLayout& layout,
                         const char* value, const char* label, bool inverted);
```

so the two screens share one implementation and the inversion flag has an
obvious home. This is not incidental cleanup: without it the blink would be
copied into both files alongside the block it inverts.

## Testing

Host tests do not earn their place here. The chaining rule is a boolean reaching
one call site, and a test asserting that `manualStart` selects `Gate::Ready`
would restate the line rather than check it. The blink is a timing deadline and
an inversion — both visual. Verification is on device, on X3 and X4:

- the lengths list shows `Enchaînement` with its current value, and selecting it
  flips and reopens the list
- automatic: at the end of a step, one press starts the next immediately
- manual: at the end of a step, one press moves to the next, which shows its full
  length and waits; a second press starts it
- the figure inverts once, briefly, whenever a step begins
- the mode survives a power cycle

## Risks

- **Flash budget.** 96.9% of `app0` is used, ~198 KB free. This adds a state
  field, one list row, three strings across 28 languages, and a shared helper
  that removes more code than it adds. Measure after implementation.
- **A blink is a refresh.** Two extra fast refreshes per start also mean two more
  contributions to the residue the periodic full waveform has to clear. At one
  start per phase this is negligible next to the repaint cadence.

## Out of scope

- A chaining mode that advances with no press at all.
- Blinking on anything but the start of a step.
- Making the blink duration or repeat count configurable.
