/****************************************************************************
 *
 * Copyright (c) 2019 Intel Corporation
 * Copyright 2019 Jozef Kralik All Rights Reserved.
 * Copyright 2018 Samsung Electronics All Rights Reserved.
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

#ifndef OC_CLOUD_INTERNAL_H
#define OC_CLOUD_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "oc_api.h"
#include "oc_cloud.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Value of 0 means that the check which removes links after their Time to Live
 * property expires should be skipped. Thus the Time to Live of such link
 * is unlimited. This is the default value for the Time to Live property.
 */
#define RD_PUBLISH_TTL_UNLIMITED 0

typedef struct cloud_conf_update_t
{
  char *access_token; /**< Access Token resolved with an auth code. */
  size_t access_token_len;
  char *auth_provider; /**< Auth Provider ID*/
  size_t auth_provider_len;
  char *ci_server; /**< Cloud Interface Server URL which an Enrollee is going to
                      registered. */
  size_t ci_server_len;
  char *sid; /**< OCF Cloud Identity as defined in OCF CNC 2.0 Spec. */
  size_t sid_len;
} cloud_conf_update_t;

typedef struct cloud_api_param_t
{
  oc_cloud_context_t *ctx;
  oc_cloud_cb_t cb;
  void *data;
} cloud_api_param_t;

cloud_api_param_t *alloc_api_param(void);
void free_api_param(cloud_api_param_t *p);
int conv_cloud_endpoint(oc_cloud_context_t *ctx);

int oc_cloud_init(void);
void oc_cloud_shutdown(void);

void oc_cloud_register_handler(oc_client_response_t *data);
void oc_cloud_login_handler(oc_client_response_t *data);
void oc_cloud_refresh_token_handler(oc_client_response_t *data);
void oc_cloud_clear_context(oc_cloud_context_t *ctx);
int oc_cloud_reset_context(size_t device);

void cloud_close_endpoint(oc_endpoint_t *cloud_ep);

void cloud_store_dump_async(const oc_cloud_store_t *store);
int cloud_store_load(oc_cloud_store_t *store);
long cloud_store_dump(const oc_cloud_store_t *store);
void cloud_store_initialize(oc_cloud_store_t *store);
void cloud_store_deinitialize(oc_cloud_store_t *store);
void cloud_manager_cb(oc_cloud_context_t *ctx);
void cloud_set_string(oc_string_t *dst, const char *data, size_t len);
void cloud_set_last_error(oc_cloud_context_t *ctx, oc_cloud_error_t error);
void cloud_set_cps(oc_cloud_context_t *ctx, oc_cps_t cps);
void cloud_set_cps_and_last_error(oc_cloud_context_t *ctx, oc_cps_t cps,
                                  oc_cloud_error_t error);
void cloud_update_by_resource(oc_cloud_context_t *ctx,
                              const cloud_conf_update_t *data);
/**
 * @brief Update resource links after manager status change.
 *
 * If cloud is in logged in state the function executes several resource links
 * updates: deletes links scheduled to be deleted, publishes links scheduled
 * to be published and republishes links that were already published.
 * Additionally, if Time to Live property is not equal to
 * RD_PUBLISH_TTL_UNLIMITED then published links are scheduled to be republished
 * each hour. (If cloud_rd_manager_status_changed function is triggered again
 * before the scheduled time passes the republishing is rescheduled with updated
 * time.)
 *
 * @param ctx Cloud context, must not be NULL
 */
void cloud_rd_manager_status_changed(oc_cloud_context_t *ctx);

/**
 * @brief Deallocate all resource directory context member variables.
 *
 * Deallocate the list of to be published resources, the list of published
 * resources and the list of to be deleted resources. Remove delayed callback
 * that republishes resources (if it's active).
 *
 * @param ctx Cloud context, must not be NULL
 */
void cloud_rd_deinit(oc_cloud_context_t *ctx);

/**
 * @brief Reset resource directory context member variables.
 *
 * Items in the list of published resources are moved to the list of to be
 * published resources. The list of to be deleted resources is cleared.
 *
 * @param ctx Cloud context, must not be NULL
 */
void cloud_rd_reset_context(oc_cloud_context_t *ctx);

void cloud_manager_start(oc_cloud_context_t *ctx);
void cloud_manager_stop(oc_cloud_context_t *ctx);

void oc_create_cloudconf_resource(size_t device);

/**
 * @brief Check whether refresh token is set.
 *
 * @return true refresh token is set
 */
bool cloud_has_refresh_token(const oc_cloud_context_t *ctx);

/**
 * @brief Checks whether the access token is set and whether it is permanent
 * (ie. the expires in time of the access token has special value which means
 * that the token is permanent).
 *
 * @return true access token is permanent
 */
bool cloud_has_permanent_access_token(const oc_cloud_context_t *ctx);

/** @brief Clear access token from context */
void cloud_clear_access_token(oc_cloud_context_t *ctx);

/**
 * @brief Send a ping over the cloud connected connection
 *
 * @param endpoint endpoint to be used
 * @param timeout_seconds timeout for the ping
 * @param handler the response handler
 * @param user_data the user data to be conveyed to the response handler
 * @return true on success
 */
bool cloud_send_ping(oc_endpoint_t *endpoint, uint16_t timeout_seconds,
                     oc_response_handler_t handler, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* OC_CLOUD_INTERNAL_H */
