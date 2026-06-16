# ROTR Path C — Stage Prompts for Claude Code

Three prompts, fed to Claude Code **one at a time**, in order.

## Before you start
1. Fork `https://github.com/martial-cc/MOTR` on GitHub.
2. Put the project brief in the repo as `DESIGN-BRIEF.md` (the prompts tell the agent to read it as the source of truth).
3. Make sure the GitHub CLI is authenticated in Claude Code (`gh auth login`) so it can push, trigger builds, read logs, and download the `.uf2`.

## How the loop works
- **Claude Code** handles the build loop itself: edit → push → trigger Actions → read logs → fix → download `.uf2`. You don't relay build errors to anyone.
- **You** flash each stage's `.uf2` and test on hardware.
- **Report the hardware result back to Claude (chat)** — pass/fail and what it actually did. Claude confirms the stage gate or helps diagnose, then you run the next prompt.
- Only move to the next prompt once the current stage is confirmed working on the device.

---

## STAGE 1 PROMPT — prove the base

```
We are building the ROTR "Path C" dual-mode ZMK firmware. Read DESIGN-BRIEF.md in
this repo first — it is the source of truth for hardware facts, architecture, and
the must-preserve constraints. Do not deviate from that architecture or substitute
a simpler one.

This session is STAGE 1 ONLY: prove the base. Scope:
- This repo is a fork of MOTR. Get it building on upstream ZMK via GitHub Actions.
- Buttons: Left = Copy (LC(C)), Middle = Select-All (LC(A)), Right = Paste (LC(V)).
  (Single layer this stage, so the middle button is just Select-All — no hold/layer
  switching yet.)
- One layer only. The knob is a smooth VERTICAL mouse wheel, MOTR-style
  (INPUT_REL_WHEEL through the input subsystem). Reuse MOTR's MA730 driver; do not
  rewrite its angle maths.
- Nothing else: no extra layers, no selector, no per-layer RGB.

Workflow: GitHub Actions only — I do NOT build locally. Use gh to commit, push,
trigger the workflow, watch it, read logs and fix on failure, and download the .uf2
artifact to an obvious path.

Definition of done: it builds and produces a .uf2. Then STOP, tell me the artifact
path, and wait — I will flash it and report whether the buttons and smooth scroll
work on hardware. Do NOT start Stage 2. If the build keeps failing, show me the
relevant log and what you tried.
```

---

## STAGE 2 PROMPT — dual-mode + full layer map

```
Continuing the ROTR Path C firmware (see DESIGN-BRIEF.md). Stage 1 is flashed and
confirmed working on hardware.

This session is STAGE 2: dual-mode encoder + full layer map. Scope:
- Implement the layer-aware dual-mode knob: each poll, the driver checks the highest
  active layer and emits into sensor-world or scroll-world per a per-layer mode table.
- Landing layers:
    0 default    — sensor — knob INACTIVE (no rotation output)
    1 brightness — sensor — F13 (clockwise) / F14 (counter-clockwise)
    2 volume     — sensor — Vol Up / Vol Down
    3 scroll-v   — scroll — smooth vertical wheel (INPUT_REL_WHEEL)
    4 scroll-h   — scroll — smooth horizontal wheel (INPUT_REL_HWHEEL)
- Buttons stay Copy / Select-All / Paste on every layer.
- Handle the accumulator reset on any mode/layer change so crossing between a
  keyboard layer and a scroll layer never emits a stray tick.
- The proper hold-middle-turn selector is STAGE 3. For now, use the simplest possible
  TEMPORARY way to reach each layer for testing (e.g. a momentary &mo or a &tog on a
  button). Tell me exactly what temporary method you used so I can test each layer.

Workflow: GitHub Actions only, via gh, same as Stage 1.

Definition of done: builds to a .uf2. Then STOP, give me the artifact path AND the
temporary layer-switch method, and wait for my hardware report. Do NOT start Stage 3.
```

---

## STAGE 3 PROMPT — selector + RGB + edge-case polish

```
Continuing the ROTR Path C firmware (see DESIGN-BRIEF.md). Stage 2 is flashed and
confirmed: all five layers' knob behaviours work, and there are no stray scroll-ticks
when crossing between layers.

This session is STAGE 3: selector + per-layer RGB + final polish. Scope:
- Reimplement the layer selector to feel IDENTICAL to the stock ROTR: hold the MIDDLE
  button, turn the knob to select the active base layer, release to land. The hold is
  the trigger (a keyboard behaviour); on the selector layer the knob is in sensor mode
  so turning selects layers. Replace whatever temporary switching Stage 2 used.
- Per-layer RGB underglow colour for each landing layer (0-4), via a layer-state-
  changed listener. Keep the RGB on/off toggle on the selector layer's right button
  (&rgb_ug RGB_TOG).
- Finalise the accumulator-reset edge case for clean transitions in BOTH directions.
- RGB colours: [either] use sensible distinct defaults and tell me what you chose,
  [or] ask me and I will specify a colour per layer.

Workflow: GitHub Actions only, via gh, same as before.

Definition of done: builds to a .uf2. STOP and give me the artifact path; I will
verify the selector feels identical and the colours and transitions are right. Record
what changed in DESIGN.md.
```

---

## After Stage 3
Once it's confirmed on hardware, that's your daily-driver firmware. Keep your current
known-good `.uf2` and keymap archived as a fallback in case you ever want to roll back.
