#pragma once
#include <stdbool.h>
#include <stdint.h>

void acpi_init(uint64_t rsdp_phys);
// these only return if they didn't work
bool acpi_poweroff(void);
void acpi_reset(void);
