#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);

/* True when the ramdisk holds a KAG2TWRP chainload payload. */
int loader_chain_payload_present(uintptr_t dtb);
