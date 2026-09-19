// Заглушка Arduino для хост-превью. Хватает ровно того, что трогает face.cpp.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

extern uint32_t g_virtualMs;          // виртуальные часы превью
inline uint32_t millis() { return g_virtualMs; }
inline uint32_t micros() { return g_virtualMs * 1000u; }
inline void delay(uint32_t) {}

inline void randomSeed(unsigned long s) { srand((unsigned)s); }
inline long random(long hi) { return hi > 0 ? rand() % hi : 0; }
inline long random(long lo, long hi) { return hi > lo ? lo + rand() % (hi - lo) : lo; }
