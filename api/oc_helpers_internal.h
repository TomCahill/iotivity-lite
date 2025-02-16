/****************************************************************************
 *
 * Copyright 2022 Daniel Adam, All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"),
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/

#ifndef OC_HELPERS_INTERNAL_H
#define OC_HELPERS_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fill buffer with random values.
 *
 * @param buffer output buffer (cannot be NULL)
 * @param buffer_size size of the output buffer
 */
void oc_random_buffer(uint8_t *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* OC_HELPERS_INTERNAL_H */
