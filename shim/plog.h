#pragma once
#include <stdint.h>

void plog_init(void);
void plog_str(const char *s);
void plog_hex(const char *key, uint64_t value);
void plog_commit(void);
void fbsnap_decode(void);
