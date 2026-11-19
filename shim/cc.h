#pragma once

/* Log MSM8996 MMCC/GCC display clock and GDSC state to the pstore log.
 * Touches only always-on clock-controller blocks, never MDSS/DSI/MDP. */
void cc_probe(void);
