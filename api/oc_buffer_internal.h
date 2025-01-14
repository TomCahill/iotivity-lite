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

#ifndef OC_BUFFER_INTERNAL_H
#define OC_BUFFER_INTERNAL_H

#include "api/oc_network_events_internal.h"
#include "api/oc_tcp_internal.h"
#include "util/oc_features.h"
#include <stddef.h>

#ifdef OC_HAS_FEATURE_TCP_ASYNC_CONNECT
/**
 * @brief finish connecting tcp session
 *
 * @param event on tcp connect event
 */
void oc_tcp_connect_session(oc_tcp_on_connect_event_t *event);
#endif /* OC_HAS_FEATURE_TCP_ASYNC_CONNECT */

#ifdef OC_SECURITY
/**
 * @brief close all tls session for the specific device
 *
 * @param device the device index
 */
void oc_close_all_tls_sessions_for_device(size_t device);

/**
 * @brief close all tls sessions
 *
 */
void oc_close_all_tls_sessions(void);
#endif /* OC_SECURITY */

#endif /* OC_BUFFER_INTERNAL_H */
