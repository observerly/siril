#ifndef ALGOS_RANDOM_H
#define ALGOS_RANDOM_H

#ifdef __cplusplus
#include <cstdint>
#include "core/siril.h"
#undef _
extern "C" {
#endif

void siril_initialize_rng();
double siril_random_double();
float siril_random_float();
WORD siril_random_WORD();
BYTE siril_random_BYTE();
uint32_t siril_random_uint();
int32_t siril_random_int();

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
template <typename T>
T siril_templated_random();
template <typename T>
T siril_templated_random_max();
#endif

#endif // ALGOS_RANDOM_H
