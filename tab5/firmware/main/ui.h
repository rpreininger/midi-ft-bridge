// Touch UI on the Tab5 display.
//
// Song list: the clips of the pad mapping (same order as the Fantom pads) in
// the free canvas area right of panels B/C and below panel D; the clip that
// plays is highlighted.
// Picker: a tap on the preview (or the list) covers the preview with one big
// button per clip plus STOP. A tap on a button plays it and closes the picker;
// a tap outside the buttons, or 8 s without a tap, closes it as well.
#pragma once

#include "config.h"

void ui_start(const config_t *cfg);
// A tap at landscape (x, y) as if a finger went down and up there (console `tap`).
void ui_tap(int x, int y);
