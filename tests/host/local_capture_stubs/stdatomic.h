#pragma once
/* Deterministic single-thread scheduling. Hardware atomicity is compiled by ESP-IDF. */
#include <stdbool.h>
typedef unsigned atomic_uint;
typedef bool atomic_bool;
#define atomic_load(p) (*(p))
#define atomic_store(p,v) (*(p)=(v))
