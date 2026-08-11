#pragma once

#include "config.h"

void buttonHandlerInit();
void buttonHandlerPoll();

/** Advance to the next non-zero-coordinate location, animate, and redraw. */
void triggerLocationCycle();
/** Advance to the next range preset, animate, and redraw. */
void triggerRangeCycle();