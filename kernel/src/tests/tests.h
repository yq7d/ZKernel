#pragma once
#include <stdbool.h>

bool test_printf(void);
bool test_string(void);
bool test_mm(void);
bool test_timer(void);
bool test_sched(void);
bool test_vfs(void);
bool test_proc(void);
void trigger_fault(const char *name);
