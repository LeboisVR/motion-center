/*
 * MiscUtils.h
 * Code version 1.0
 * Declares miscellaneous helper functions used by the competition control
 * box.  These functions are defined in MiscUtils.cpp and provide
 * behaviours that do not logically belong in the main sketch.  Keeping
 * them in a separate translation unit helps to declutter the .ino file
 * and makes it easier to port to other platforms.
 */

#pragma once

// Trigger the security shutdown.  This function disables the servo
// power via disableServo() and then flashes the LED alternately green
// and red until the board is reset.  It never returns.
void Security();

