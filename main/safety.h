#pragma once
#include <stdbool.h>
#include <stdint.h>
void safety_start(void);
void safety_confirm(const char *why);
bool safety_on_trial(void);
int  safety_trial_left(void);
uint8_t safety_crashes(void);
