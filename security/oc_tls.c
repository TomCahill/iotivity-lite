/****************************************************************************
 *
 * Copyright (c) 2016-2019 Intel Corporation, All Rights Reserved.
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

#ifdef OC_SECURITY

#include "oc_tls.h"
#include "api/oc_events.h"
#include "api/oc_main.h"
#include "api/oc_network_events_internal.h"
#include "api/oc_session_events_internal.h"
#include "messaging/coap/engine.h"
#include "messaging/coap/observe.h"
#include "oc_api.h"
#include "oc_buffer.h"
#include "oc_client_state.h"
#include "oc_config.h"
#include "oc_core_res.h"
#include "oc_endpoint.h"
#include "oc_pki.h"
#include "port/oc_connectivity.h"
#include "port/oc_connectivity_internal.h"
#include "util/oc_features.h"
#include "security/oc_acl_internal.h"
#include "security/oc_audit.h"
#include "security/oc_cred_internal.h"
#include "security/oc_doxm.h"
#include "security/oc_entropy_internal.h"
#include "security/oc_pstat.h"
#include "security/oc_roles_internal.h"
#include "security/oc_security_internal.h"
#include "security/oc_svr.h"

#ifdef OC_PKI
#include "oc_certs_internal.h"
#include "oc_certs_validate_internal.h"
#endif /* OC_PKI */

#ifdef OC_OSCORE
#include "security/oc_oscore.h"
#endif /* OC_OSCORE */

#include "mbedtls/build_info.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "mbedtls/oid.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_cookie.h"
#include "mbedtls/timing.h"
#ifdef OC_DEBUG
#include "mbedtls/debug.h"
#include "mbedtls/error.h"
#include "mbedtls/platform.h"
#endif /* OC_DEBUG */

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

OC_PROCESS(oc_tls_handler, "TLS Process");
OC_MEMB(g_tls_peers_s, oc_tls_peer_t, OC_MAX_TLS_PEERS);
OC_LIST(g_tls_peers);

static mbedtls_entropy_context g_entropy_ctx;
mbedtls_ctr_drbg_context g_oc_ctr_drbg_ctx;
static mbedtls_ssl_cookie_ctx g_cookie_ctx;
static oc_random_pin_t g_random_pin;
#define PIN_LEN (8)
static unsigned char g_pin[PIN_LEN];

void
oc_tls_generate_random_pin(void)
{
  int p = 0;
  while (p < PIN_LEN) {
    g_pin[p++] = oc_random_value() % 10 + 48;
  }
  if (g_random_pin.cb) {
    g_random_pin.cb(g_pin, PIN_LEN, g_random_pin.data);
  }
}

void
oc_set_random_pin_callback(oc_random_pin_cb_t cb, void *data)
{
  g_random_pin.cb = cb;
  g_random_pin.data = data;
}

#ifdef OC_CLIENT
static bool use_pin_obt_psk_identity = false;
void
oc_tls_use_pin_obt_psk_identity(void)
{
  use_pin_obt_psk_identity = true;
}
#endif /* OC_CLIENT */

#ifdef OC_PKI
static bool auto_assert_all_roles = true;
void
oc_auto_assert_roles(bool auto_assert)
{
  auto_assert_all_roles = auto_assert;
}

typedef struct oc_x509_cacrt_t
{
  struct oc_x509_cacrt_t *next;
  size_t device;
  oc_sec_cred_t *cred;
  mbedtls_x509_crt *cert;
} oc_x509_cacrt_t;

OC_MEMB(g_ca_certs_s, oc_x509_cacrt_t, OC_MAX_NUM_DEVICES);
OC_LIST(g_ca_certs);

static mbedtls_x509_crt g_trust_anchors;

typedef struct oc_x509_crt_t
{
  struct oc_x509_crt_t *next;
  size_t device;
  oc_sec_cred_t *cred;
  mbedtls_x509_crt cert;
  mbedtls_pk_context pk;
  oc_x509_cacrt_t *ctx;
} oc_x509_crt_t;

OC_MEMB(g_identity_certs_s, oc_x509_crt_t, 2 * OC_MAX_NUM_DEVICES);
OC_LIST(g_identity_certs);

#endif /* OC_PKI */

#define PERSONALIZATION_DATA "IoTivity-Lite-TLS"

#define CCM_MAC_KEY_LENGTH (0)
#define CBC_IV_LENGTH (0)
#define CCM_IV_LENGTH (4)
#define GCM_IV_LENGTH (12)
#define AES128_KEY_LENGTH (16)
#define AES256_KEY_LENGTH (32)
#define SHA256_MAC_KEY_LENGTH (32)

static int *ciphers = NULL;
#ifdef OC_PKI
static int selected_mfg_cred = -1;
static int selected_id_cred = -1;
#ifdef OC_CLOUD
static const int default_priority[12] = {
#else  /* OC_CLOUD */
static const int default_priority[6] = {
#endif /* !OC_CLOUD */
#else  /* OC_PKI */
static const int default_priority[2] = {
#endif /* !OC_PKI */
  MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256,
#ifdef OC_PKI
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CCM_8,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CCM,
#ifdef OC_CLOUD
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
#endif /* OC_CLOUD */
#endif /* OC_PKI */
  0
};

#ifdef OC_CLIENT
static const int psk_priority[2] = {
  MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256, 0
};

static const int anon_ecdh_priority[2] = {
  MBEDTLS_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA256, 0
};
#endif /* OC_CLIENT */

static const int jw_otm_priority[2] = {
  MBEDTLS_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA256, 0
};

static const int pin_otm_priority[2] = {
  MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256, 0
};

#ifdef OC_PKI
static const int cert_otm_priority[5] = {
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CCM_8,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CCM, 0
};
#endif /* OC_PKI */

#ifdef OC_CLIENT
#ifdef OC_PKI
#ifdef OC_CLOUD
static const int cloud_priority[7] = {
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
  0
};
#endif /* OC_CLOUD */

static const int cert_priority[5] = {
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CCM_8,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CCM, 0
};
#endif /* OC_PKI */
#endif /* OC_CLIENT */

#ifdef OC_PKI
mbedtls_x509_crt *
oc_tls_get_trust_anchors(void)
{
  return &g_trust_anchors;
}
#endif /* OC_PKI */

#ifdef OC_DEBUG
static void
oc_mbedtls_debug(void *ctx, int level, const char *file, int line,
                 const char *str)
{
  (void)ctx;
  (void)level;
  PRINT("mbedtls_log: %s:%04d: %s", file, line, str);
}
#endif /* OC_DEBUG */

static bool
is_peer_active(oc_tls_peer_t *peer)
{
  oc_tls_peer_t *p = (oc_tls_peer_t *)oc_list_head(g_tls_peers);
  while (p != NULL) {
    if (p == peer) {
      return true;
    }
    p = p->next;
  }
  return false;
}

static oc_event_callback_retval_t
reset_in_RFOTM(void *data)
{
  size_t device = (size_t)data;
  oc_pstat_reset_device(device, true);
  return OC_EVENT_DONE;
}

static oc_event_callback_retval_t oc_tls_inactive(void *data);

#ifdef OC_CLIENT
static void
oc_tls_free_invalid_peer(oc_tls_peer_t *peer)
{
  OC_DBG("\noc_tls: removing invalid peer");

  oc_list_remove(g_tls_peers, peer);

  size_t device = peer->endpoint.device;
  oc_sec_pstat_t *pstat = oc_sec_get_pstat(device);
  if (pstat->s == OC_DOS_RFOTM) {
    oc_set_delayed_callback((void *)device, &reset_in_RFOTM, 0);
  }

  oc_ri_remove_timed_event_callback(peer, oc_tls_inactive);

  mbedtls_ssl_free(&peer->ssl_ctx);
  oc_message_t *message = (oc_message_t *)oc_list_pop(peer->send_q);
  while (message != NULL) {
    oc_message_unref(message);
    message = (oc_message_t *)oc_list_pop(peer->send_q);
  }
  message = (oc_message_t *)oc_list_pop(peer->recv_q);
  while (message != NULL) {
    oc_message_unref(message);
    message = (oc_message_t *)oc_list_pop(peer->recv_q);
  }
#ifdef OC_PKI
  oc_free_string(&peer->public_key);
#endif /* OC_PKI */
#ifdef OC_TCP
  if (peer->processed_recv_message != NULL) {
    oc_message_unref(peer->processed_recv_message);
  }
#endif
  mbedtls_ssl_config_free(&peer->ssl_conf);
  oc_etimer_stop(&peer->timer.fin_timer);
  oc_memb_free(&g_tls_peers_s, peer);
}
#endif /* OC_CLIENT */

static void
oc_tls_free_peer(oc_tls_peer_t *peer, bool inactivity_cb)
{
  OC_DBG("\noc_tls: removing peer");
  oc_list_remove(g_tls_peers, peer);

  size_t device = peer->endpoint.device;
  oc_sec_pstat_t *pstat = oc_sec_get_pstat(device);
  if (pstat->s == OC_DOS_RFOTM) {
    oc_set_delayed_callback((void *)device, &reset_in_RFOTM, 0);
  }

#ifdef OC_SERVER
  /* remove all observations by this peer */
  coap_remove_observer_by_client(&peer->endpoint);
#endif /* OC_SERVER */
  /* remove all open transactions associated to this endpoint */
  coap_free_transactions_by_endpoint(&peer->endpoint);
#ifdef OC_CLIENT
  /* remove all remaining client_cbs awaiting a response from this endpoint and
   * notify a 5.03 status to the application.
   */
  oc_ri_free_client_cbs_by_endpoint(&peer->endpoint);
#endif /* OC_CLIENT */

#ifdef OC_PKI
  /* Free all roles bound to this (D)TLS session */
  oc_sec_free_roles(peer);
#endif /* OC_PKI */

#ifdef OC_TCP
  if (peer->processed_recv_message != NULL) {
    oc_message_unref(peer->processed_recv_message);
  }
  if (peer->endpoint.flags & TCP) {
    oc_connectivity_end_session(&peer->endpoint);
  } else
#endif /* OC_TCP */
  {
    oc_handle_session(&peer->endpoint, OC_SESSION_DISCONNECTED);
  }

  if (!inactivity_cb) {
    oc_ri_remove_timed_event_callback(peer, oc_tls_inactive);
  }
  mbedtls_ssl_free(&peer->ssl_ctx);
  oc_message_t *message = (oc_message_t *)oc_list_pop(peer->send_q);
  while (message != NULL) {
    oc_message_unref(message);
    message = (oc_message_t *)oc_list_pop(peer->send_q);
  }
  message = (oc_message_t *)oc_list_pop(peer->recv_q);
  while (message != NULL) {
    oc_message_unref(message);
    message = (oc_message_t *)oc_list_pop(peer->recv_q);
  }
#ifdef OC_PKI
  oc_free_string(&peer->public_key);
#endif /* OC_PKI */
  mbedtls_ssl_config_free(&peer->ssl_conf);
  oc_etimer_stop(&peer->timer.fin_timer);
  oc_memb_free(&g_tls_peers_s, peer);
}

oc_tls_peer_t *
oc_tls_get_peer(const oc_endpoint_t *endpoint)
{
  oc_tls_peer_t *peer = oc_list_head(g_tls_peers);
  while (peer != NULL) {
    if (endpoint == NULL ||
        oc_endpoint_compare(&peer->endpoint, endpoint) == 0) {
      return peer;
    }
    peer = peer->next;
  }
  return NULL;
}

void
oc_tls_remove_peer(const oc_endpoint_t *endpoint)
{
  oc_tls_peer_t *peer = oc_tls_get_peer(endpoint);
  if (peer != NULL) {
    oc_tls_free_peer(peer, false);
  }
}

static void
oc_tls_close_peer(oc_tls_peer_t *peer)
{
  assert(peer != NULL);
  mbedtls_ssl_close_notify(&peer->ssl_ctx);
  if ((peer->endpoint.flags & TCP) == 0) {
    mbedtls_ssl_close_notify(&peer->ssl_ctx);
  }
  oc_tls_free_peer(peer, false);
}

void
oc_tls_close_peers(oc_tls_peer_filter_t filter, void *user_data)
{
  oc_tls_peer_t *peer = oc_list_head(g_tls_peers);
  while (peer != NULL) {
    oc_tls_peer_t *peer_next = peer->next;
    if (filter == NULL || filter(peer, user_data)) {
      oc_tls_close_peer(peer);
    }
    peer = peer_next;
  }
}

bool
oc_tls_is_pin_otm_supported(size_t device)
{
  (void)device;
  if (g_random_pin.cb) {
    return true;
  }
  return false;
}

#ifdef OC_PKI
bool
oc_tls_is_cert_otm_supported(size_t device)
{
  oc_x509_crt_t *crt = (oc_x509_crt_t *)oc_list_head(g_identity_certs);
  while (crt) {
    if (crt->device == device &&
        crt->cred->credusage == OC_CREDUSAGE_MFG_CERT) {
      return true;
    }
    crt = crt->next;
  }
  return false;
}
#endif /* OC_PKI */

static void
oc_tls_handler_schedule_read(oc_tls_peer_t *peer)
{
  oc_process_post(&oc_tls_handler, oc_events[TLS_READ_DECRYPTED_DATA], peer);
}

#ifdef OC_CLIENT
static void
oc_tls_handler_schedule_write(oc_tls_peer_t *peer)
{
  oc_process_post(&oc_tls_handler, oc_events[TLS_WRITE_APPLICATION_DATA], peer);
}
#endif /* OC_CLIENT */

static oc_event_callback_retval_t
oc_tls_inactive(void *data)
{
  OC_DBG("oc_tls: DTLS inactivity callback");
  oc_tls_peer_t *peer = (oc_tls_peer_t *)data;
  if (is_peer_active(peer)) {
    oc_clock_time_t time = oc_clock_time();
    time -= peer->timestamp;
    if (time < (oc_clock_time_t)OC_DTLS_INACTIVITY_TIMEOUT *
                 (oc_clock_time_t)OC_CLOCK_SECOND) {
      OC_DBG("oc_tls: Resetting DTLS inactivity callback");
      return OC_EVENT_CONTINUE;
    }
    mbedtls_ssl_close_notify(&peer->ssl_ctx);
    if ((peer->endpoint.flags & TCP) == 0) {
      mbedtls_ssl_close_notify(&peer->ssl_ctx);
    }
    oc_tls_free_peer(peer, true);
  }
  OC_DBG("oc_tls: Terminating DTLS inactivity callback");
  return OC_EVENT_DONE;
}

static int
ssl_recv(void *ctx, unsigned char *buf, size_t len)
{
  oc_tls_peer_t *peer = (oc_tls_peer_t *)ctx;
  oc_message_t *message = (oc_message_t *)oc_list_head(peer->recv_q);
  if (message) {
    size_t recv_len = 0;
#ifdef OC_TCP
    if (message->endpoint.flags & TCP) {
      recv_len = message->length - message->read_offset;
      recv_len = (recv_len < len) ? recv_len : len;
      memcpy(buf, message->data + message->read_offset, recv_len);
      message->read_offset += recv_len;
      if (message->read_offset == message->length) {
        oc_list_remove(peer->recv_q, message);
        oc_message_unref(message);
      }
    } else
#endif /* OC_TCP */
    {
      recv_len = (message->length < len) ? message->length : len;
      memcpy(buf, message->data, recv_len);
      oc_list_remove(peer->recv_q, message);
      oc_message_unref(message);
    }
    return (int)recv_len;
  }
  return MBEDTLS_ERR_SSL_WANT_READ;
}

static int
ssl_send(void *ctx, const unsigned char *buf, size_t len)
{
  oc_tls_peer_t *peer = (oc_tls_peer_t *)ctx;
  peer->timestamp = oc_clock_time();
  oc_message_t *message = oc_allocate_message();
  if (message == NULL) {
    return 0;
  }
  memcpy(&message->endpoint, &peer->endpoint, sizeof(oc_endpoint_t));
  size_t send_len = (len < (unsigned)OC_PDU_SIZE) ? len : (unsigned)OC_PDU_SIZE;
  memcpy(message->data, buf, send_len);
  message->length = send_len;
  // TODO: oc_message_shrink_buffer
  message->encrypted = 1;
  int ret = oc_send_buffer2(message, false);
  oc_message_unref(message);
  return ret;
}

static void
check_retr_timers(void)
{
  oc_tls_peer_t *peer = (oc_tls_peer_t *)oc_list_head(g_tls_peers);
  while (peer != NULL) {
    oc_tls_peer_t *next = peer->next;
    if (peer->ssl_ctx.state != MBEDTLS_SSL_HANDSHAKE_OVER) {
      if (oc_etimer_expired(&peer->timer.fin_timer)) {
        int ret = mbedtls_ssl_handshake(&peer->ssl_ctx);
        if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
          mbedtls_ssl_session_reset(&peer->ssl_ctx);
          if (peer->role == MBEDTLS_SSL_IS_SERVER &&
              mbedtls_ssl_set_client_transport_id(
                &peer->ssl_ctx, (const unsigned char *)&peer->endpoint.addr,
                sizeof(peer->endpoint.addr)) != 0) {
            oc_tls_free_peer(peer, false);
            peer = next;
            continue;
          }
        }
        if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
#ifdef OC_DEBUG
          char buf[256];
          mbedtls_strerror(ret, buf, 256);
          OC_ERR("oc_tls: mbedtls_error: %s", buf);
#endif /* OC_DEBUG */
          oc_tls_free_peer(peer, false);
        }
      }
    }
    peer = next;
  }
}

static void
ssl_set_timer(void *ctx, uint32_t int_ms, uint32_t fin_ms)
{
  if (fin_ms != 0) {
    oc_tls_retr_timer_t *timer = (oc_tls_retr_timer_t *)ctx;
    timer->int_ticks = (oc_clock_time_t)((int_ms * OC_CLOCK_SECOND) / 1.e03);
    oc_etimer_stop(&timer->fin_timer);
    timer->fin_timer.timer.interval =
      (oc_clock_time_t)((fin_ms * OC_CLOCK_SECOND) / 1.e03);
    OC_PROCESS_CONTEXT_BEGIN(&oc_tls_handler);
    oc_etimer_restart(&timer->fin_timer);
    OC_PROCESS_CONTEXT_END(&oc_tls_handler);
  }
}

int
oc_tls_pbkdf2(const unsigned char *pin, size_t pin_len, oc_uuid_t *uuid,
              unsigned int c, uint8_t *key, uint32_t key_len)
{
  mbedtls_md_context_t hmac_SHA256;
  mbedtls_md_init(&hmac_SHA256);

  mbedtls_md_setup(&hmac_SHA256, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   1);

  memset(key, 0, key_len);

  int ret = mbedtls_pkcs5_pbkdf2_hmac(&hmac_SHA256, pin, pin_len,
                                      (const unsigned char *)uuid->id, 16, c,
                                      key_len, key);

  mbedtls_md_free(&hmac_SHA256);

  if (ret != 0) {
    ret = -1;
  }

  return ret;
}

static void
oc_tls_audit_log(const char *aeid, const char *message, uint8_t category,
                 uint8_t priority, oc_tls_peer_t *peer)
{
  char buff[IPADDR_BUFF_SIZE];
  if (peer) {
    SNPRINTFipaddr(buff, IPADDR_BUFF_SIZE, peer->endpoint);
  } else {
    buff[0] = '\0';
  }
  char *aux[] = { buff };
  oc_audit_log(peer != NULL ? peer->endpoint.device : 0, aeid, message,
               category, priority, (const char **)aux, 1);
}

static int
get_psk_cb(void *data, mbedtls_ssl_context *ssl, const unsigned char *identity,
           size_t identity_len)
{
  (void)data;
  (void)identity_len;
  OC_DBG("oc_tls: In PSK callback");
  oc_tls_peer_t *peer = oc_list_head(g_tls_peers);
  while (peer != NULL) {
    if (&peer->ssl_ctx == ssl) {
      break;
    }
    peer = peer->next;
  }
  if (peer) {
    OC_DBG("oc_tls: Found peer object");
    oc_sec_pstat_t *ps = oc_sec_get_pstat(peer->endpoint.device);
    /* To an OBT performing the PIN OTM, a device signals its identity
     * with the oic.sec.doxm.rdp: prefix.
     */
    if (ps->s == OC_DOS_RFNOP && identity_len > 16 &&
        memcmp(identity, "oic.sec.doxm.rdp:", 17) == 0) {
      identity += 17;
      identity_len -= 17;
    }
    oc_sec_cred_t *cred =
      oc_sec_find_cred(NULL, (oc_uuid_t *)identity, OC_CREDTYPE_PSK,
                       OC_CREDUSAGE_NULL, peer->endpoint.device);
    if (cred != NULL) {
      OC_DBG("oc_tls: Found peer credential");
      memcpy(peer->uuid.id, identity, 16);
      OC_DBG("oc_tls: Setting the key:");
      OC_LOGbytes(oc_string(cred->privatedata.data),
                  oc_string_len(cred->privatedata.data));
      if (mbedtls_ssl_set_hs_psk(ssl,
                                 oc_cast(cred->privatedata.data, const uint8_t),
                                 oc_string_len(cred->privatedata.data)) != 0) {
        return -1;
      }
      OC_DBG("oc_tls: Set peer credential to SSL handle");
      return 0;
    }
    oc_sec_doxm_t *doxm = oc_sec_get_doxm(peer->endpoint.device);
    if (ps->s == OC_DOS_RFOTM && doxm->oxmsel == OC_OXMTYPE_RDP) {
      if (identity_len != 16 || memcmp(identity, "oic.sec.doxm.rdp", 16) != 0) {
        OC_ERR("oc_tls: OBT identity incorrectly set for PIN OTM");
        return -1;
      }
      OC_DBG("oc_tls: deriving PPSK for PIN OTM");
      memcpy(peer->uuid.id, identity, 16);

      uint8_t key[16];

      if (oc_tls_pbkdf2(g_pin, PIN_LEN, &doxm->deviceuuid, 1000, key, 16) !=
          0) {
        OC_ERR("oc_tls: error deriving PPSK");
        return -1;
      }

      if (mbedtls_ssl_set_hs_psk(ssl, key, 16) != 0) {
        OC_ERR("oc_tls: error applying PPSK to current handshake");
        return -1;
      }
      return 0;
    }
  }
  OC_ERR("oc_tls: could not find peer credential");
  oc_tls_audit_log("AUTH-1",
                   "DLTS handshake error, could not find peer credential", 0x08,
                   1, peer);
  return -1;
}

static int
ssl_get_timer(void *ctx)
{
  oc_tls_retr_timer_t *timer = (oc_tls_retr_timer_t *)ctx;
  if (timer->fin_timer.timer.interval == 0)
    return -1;
  if (oc_etimer_expired(&timer->fin_timer)) {
    timer->fin_timer.timer.interval = 0;
    timer->int_ticks = 0;
    return 2;
  } else if (oc_clock_time() >
             (timer->fin_timer.timer.start + timer->int_ticks)) {
    return 1;
  }
  return 0;
}

#ifdef OC_PKI
typedef bool (*check_if_known_cert_cb)(oc_sec_cred_t *cred);
typedef void (*add_new_cert_cb)(oc_sec_cred_t *cred, size_t device);

static void
oc_tls_add_new_certs(oc_sec_credusage_t credusage,
                     check_if_known_cert_cb is_known_cert,
                     add_new_cert_cb add_new_cert)
{
  size_t device;
  for (device = 0; device < oc_core_get_num_devices(); device++) {
    oc_sec_creds_t *creds = oc_sec_get_creds(device);
    oc_sec_cred_t *cred = (oc_sec_cred_t *)oc_list_head(creds->creds);
    for (; cred != NULL; cred = cred->next) {
      /* Pick all "leaf" certificates with matching credusage */
      if ((cred->credusage & credusage) != 0 && !cred->child) {

        if (is_known_cert(cred)) {
          continue;
        }

        add_new_cert(cred, device);
      }
    }
  }
}

static bool
is_known_identity_cert(oc_sec_cred_t *cred)
{
  oc_x509_crt_t *certs = (oc_x509_crt_t *)oc_list_head(g_identity_certs);

  /* Look for a matching end-entity cert chain */
  for (; certs != NULL; certs = certs->next) {
    if (certs->cred == cred) {
      break;
    }
  }

  if (!certs) {
    OC_DBG("identity cert chain not found in known list; Tracking new identity "
           "cert chain...");
    return false;
  }

  /* Identity cert chain currently tracked by mbedTLS */
  mbedtls_x509_crt *id_cert = &certs->cert;
  mbedtls_x509_crt cert_in_cred;
  mbedtls_x509_crt *cert = &cert_in_cred;
next_cred_in_chain:

  while (cred) {
    mbedtls_x509_crt_init(cert);

    /* Parse cert in cred entry for matching below */
    size_t cert_len = oc_string_len(cred->publicdata.data);
    if (cred->publicdata.encoding == OC_ENCODING_PEM) {
      cert_len++;
    }
    int ret = mbedtls_x509_crt_parse(
      cert, (const unsigned char *)oc_string(cred->publicdata.data), cert_len);
    if (ret < 0) {
      OC_ERR("could not parse identity cert from cred");
      return true;
    }

    /* Walk through chain of tracked identity certs and match against
     * chain of certs currently within and/or spread across cred entries
     * to identify new (and recently provisioned) certs via new cred
     * entries and accordingly add them. Once added, the new chain is
     * presented during future TLS handshakes that employ the cert chain.
     */
    while (id_cert && cert) {
      if (id_cert->raw.len == cert->raw.len &&
          memcmp(id_cert->raw.p, cert->raw.p, cert->raw.len) == 0) {

        if (cert->next) {
          OC_DBG("found matching cert..proceeding further down the chain");
          cert = cert->next;
          continue;
        } else {
          OC_DBG("found matching cert..proceeding further down the chain");
          cred = cred->chain;
          mbedtls_x509_crt_free(&cert_in_cred);
          goto next_cred_in_chain;
        }
      } else if (!id_cert->next) {
        OC_DBG("new cert chains to known cert chain; Add cert to chain and "
               "proceed...");
        ret =
          mbedtls_x509_crt_parse_der(&certs->cert, cert->raw.p, cert->raw.len);
        if (ret < 0) {
          OC_WRN("could not parse cert in provided chain");
          mbedtls_x509_crt_free(&cert_in_cred);
          return true;
        }
#ifdef OC_DEBUG
        mbedtls_x509_crt *c = &certs->cert;
        int chain_length = 0;
        while (c) {
          chain_length++;
          c = c->next;
        }
        OC_DBG("identity cert chain is now of size %d", chain_length);
#endif /* OC_DEBUG */

        if (cert->next) {
          OC_DBG("processing other new certs, if any, further down the chain");
          cert = cert->next;
          continue;
        } else {
          OC_DBG("processing other new certs, if any, further down the chain");
          mbedtls_x509_crt_free(&cert_in_cred);
          cred = cred->chain;
          goto next_cred_in_chain;
        }
      }
      id_cert = id_cert->next;
    }
    cred = cred->chain;
  }

  return true;
}

static void
add_new_identity_cert(oc_sec_cred_t *cred, size_t device)
{
  oc_x509_crt_t *cert = oc_memb_alloc(&g_identity_certs_s);
  if (!cert) {
    OC_WRN("could not allocate memory for identity cert");
    return;
  }

  cert->device = device;
  cert->cred = cred;

  mbedtls_x509_crt_init(&cert->cert);

  while (cred) {
    size_t len = oc_string_len(cred->publicdata.data);
    if (cred->publicdata.encoding == OC_ENCODING_PEM) {
      len++;
    }
    int ret = mbedtls_x509_crt_parse(
      &cert->cert, (const unsigned char *)oc_string(cred->publicdata.data),
      len);
    if (ret < 0) {
      OC_ERR("could not parse identity cert");
      goto add_new_identity_cert_error;
    }
    if (oc_string_len(cred->privatedata.data) > 0) {
      ret = mbedtls_pk_parse_key(
        &cert->pk,
        (const unsigned char *)oc_cast(cred->privatedata.data, uint8_t),
        oc_string_len(cred->privatedata.data) + 1, NULL, 0,
        mbedtls_ctr_drbg_random, &g_oc_ctr_drbg_ctx);
      if (ret != 0) {
        OC_ERR("could not parse private key %zd",
               oc_string_len(cred->privatedata.data));
        goto add_new_identity_cert_error;
      }
    }
    OC_DBG("identity cert for credential(credid=%d) parsed", cred->credid);
    cred = cred->chain;
  }

#ifdef OC_DEBUG
  mbedtls_x509_crt *c = &cert->cert;
  int chain_length = 0;
  while (c) {
    chain_length++;
    c = c->next;
  }
  OC_DBG("adding new identity cert chain of size %d", chain_length);
#endif /* OC_DEBUG */

  oc_list_add(g_identity_certs, cert);

  return;

add_new_identity_cert_error:
  OC_ERR("error adding identity cert");
  mbedtls_x509_crt_free(&cert->cert);
  mbedtls_pk_free(&cert->pk);
  oc_memb_free(&g_identity_certs_s, cert);
}

void
oc_tls_resolve_new_identity_certs(void)
{
  OC_DBG("resolving new identity certs");
  oc_tls_add_new_certs(OC_CREDUSAGE_MFG_CERT | OC_CREDUSAGE_IDENTITY_CERT,
                       is_known_identity_cert, add_new_identity_cert);
}

static oc_x509_crt_t *
oc_tls_find_identity_cert(const oc_sec_cred_t *cred)
{
  oc_x509_crt_t *cert = (oc_x509_crt_t *)oc_list_head(g_identity_certs);
  while (cert != NULL && cert->cred != cred) {
    cert = cert->next;
  }
  return cert;
}

mbedtls_x509_crt *
oc_tls_get_identity_cert_for_cred(const oc_sec_cred_t *cred)
{
  oc_x509_crt_t *crt = oc_tls_find_identity_cert(cred);
  if (crt == NULL) {
    return NULL;
  }
  return &crt->cert;
}

bool
oc_tls_remove_identity_cert(oc_sec_cred_t *cred)
{
  oc_x509_crt_t *cert = oc_tls_find_identity_cert(cred);
  if (!cert) {
    return false;
  }
  OC_DBG("identity cert for credential(credid=%d) removed", cred->credid);
  oc_list_remove(g_identity_certs, cert);
  mbedtls_x509_crt_free(&cert->cert);
  mbedtls_pk_free(&cert->pk);
  oc_memb_free(&g_identity_certs_s, cert);
  return true;
}

static oc_x509_cacrt_t *
oc_tls_find_trust_anchor_for_cred(const oc_sec_cred_t *cred)
{
  oc_x509_cacrt_t *cert = (oc_x509_cacrt_t *)oc_list_head(g_ca_certs);
  while (cert != NULL && cert->cred != cred) {
    cert = cert->next;
  }
  return cert;
}

mbedtls_x509_crt *
oc_tls_get_trust_anchor_for_cred(const oc_sec_cred_t *cred)
{
  oc_x509_cacrt_t *crt = oc_tls_find_trust_anchor_for_cred(cred);
  if (crt == NULL) {
    return NULL;
  }
  return crt->cert;
}

static int
oc_tls_reload_trust_anchors(void)
{
  oc_x509_cacrt_t *cert = (oc_x509_cacrt_t *)oc_list_head(g_ca_certs);
  while (cert != NULL) {
    int ret = mbedtls_x509_crt_parse(
      &g_trust_anchors,
      (const unsigned char *)oc_string(cert->cred->publicdata.data),
      oc_string_len(cert->cred->publicdata.data) + 1);
    if (ret != 0) {
      OC_WRN("could not parse an trustca/mfgtrustca root certificate %d", ret);
      return -1;
    }

    mbedtls_x509_crt *c = &g_trust_anchors;
#ifdef OC_DEBUG
    int chain_length = 1;
#endif /* OC_DEBUG */
    while (c->next) {
#ifdef OC_DEBUG
      ++chain_length;
#endif /* OC_DEBUG */
      c = c->next;
    }
    cert->cert = c;
#ifdef OC_DEBUG
    char buf[256];
    if (mbedtls_x509_serial_gets(buf, sizeof(buf) - 1, &c->serial) > 0) {
      OC_DBG("trust anchor(serial: %s) added to chain", buf);
    }
    OC_DBG("trust anchor chain is now of size %d", chain_length);
#endif /* OC_DEBUG */

    cert = cert->next;
  }
  return 0;
}

bool
oc_tls_remove_trust_anchor(oc_sec_cred_t *cred)
{
  oc_x509_cacrt_t *cert = oc_tls_find_trust_anchor_for_cred(cred);
  if (cert == NULL) {
    return false;
  }
  oc_list_remove(g_ca_certs, cert);
  oc_memb_free(&g_ca_certs_s, cert);
  OC_DBG("trust anchor for credential(credid=%d) removed from ca certs",
         cred->credid);
  mbedtls_x509_crt_free(&g_trust_anchors);
  mbedtls_x509_crt_init(&g_trust_anchors);
  OC_DBG("trust anchor chain cleared");
  return oc_tls_reload_trust_anchors() == 0;
}

#ifdef OC_TEST

static oc_sec_cred_t *
oc_tls_find_cert_cred(size_t device, oc_sec_cred_t *cert_cred)
{
  if (cert_cred == NULL) {
    return NULL;
  }
  oc_sec_creds_t *creds = oc_sec_get_creds(device);
  for (oc_sec_cred_t *cred = (oc_sec_cred_t *)oc_list_head(creds->creds);
       cred != NULL; cred = cred->next) {
    if (cert_cred == cred) {
      return cred;
    }
  }
  return NULL;
}

static bool
oc_tls_validate_identity_certs_consistency_for_device(size_t device)
{
  // check device credentials and list of identity certs equivalence
  // - all credentials for identity certificates are contained in the list of
  // identity certificates
  oc_sec_creds_t *creds = oc_sec_get_creds(device);
  for (oc_sec_cred_t *cred = (oc_sec_cred_t *)oc_list_head(creds->creds);
       cred != NULL; cred = cred->next) {
    /* Get leaf identity / mfg certificates */
    if ((cred->credusage &
         (OC_CREDUSAGE_MFG_CERT | OC_CREDUSAGE_IDENTITY_CERT)) != 0 &&
        !cred->child) {
      OC_DBG("search for identity cert for cred(%d)", cred->credid);
      oc_x509_crt_t *cert = oc_tls_find_identity_cert(cred);
      if (cert == NULL) {
        OC_DBG("\tidentity not found");
        return false;
      }
      OC_DBG("\tidentity cert(%p) found", (void *)cert);
    }
  }

  // - all identity certificates have a credential
  oc_x509_crt_t *cert = (oc_x509_crt_t *)oc_list_head(g_identity_certs);
  while (cert != NULL) {
    OC_DBG("search for cred for identity cert(%p)", (void *)cert);
    oc_sec_cred_t *cred = oc_tls_find_cert_cred(cert->device, cert->cred);
    if (cred == NULL) {
      OC_DBG("\tcred not found");
      return false;
    }
    OC_DBG("\tcred(%d) found", cred->credid);
    cert = cert->next;
  }

  return true;
}

bool
oc_tls_validate_identity_certs_consistency()
{
  for (size_t device = 0; device < oc_core_get_num_devices(); device++) {
    if (!oc_tls_validate_identity_certs_consistency_for_device(device)) {
      return false;
    }
  }
  return true;
}

static oc_x509_cacrt_t *
oc_tls_find_ca_cert(mbedtls_x509_crt *cert)
{
  if (cert == NULL) {
    return NULL;
  }
  oc_x509_cacrt_t *c = (oc_x509_cacrt_t *)oc_list_head(g_ca_certs);
  while (c != NULL) {
    if (c->cert == cert) {
      return c;
    }
    c = c->next;
  }
  return NULL;
}

static mbedtls_x509_crt *
oc_tls_find_trust_anchor(oc_x509_cacrt_t *cacert)
{
  if (cacert == NULL) {
    return NULL;
  }

  mbedtls_x509_crt *c = &g_trust_anchors;
  while (c != NULL) {
    if (c == cacert->cert) {
      return c;
    }
    c = c->next;
  }
  return NULL;
}

static bool
oc_tls_trust_anchors_is_empty(void)
{
  mbedtls_x509_crt crt;
  memset(&crt, 0, sizeof(mbedtls_x509_crt));
  return memcmp(&g_trust_anchors, &crt, sizeof(mbedtls_x509_crt)) == 0;
}

static bool
oc_tls_validate_trust_anchors_consistency_for_device(size_t device)
{
  // We have 3 containers that should contain the same trust anchors:
  // - global list of credentials (creds)
  // - global list of trust anchors (g_ca_certs)
  // - mbedtls container of trust anchors (g_trust_anchors)

  // - check that the g_ca_certs list contains all trust anchors from the creds
  // list
  oc_sec_creds_t *creds = oc_sec_get_creds(device);
  for (oc_sec_cred_t *cred = (oc_sec_cred_t *)oc_list_head(creds->creds);
       cred != NULL; cred = cred->next) {
    /* Get leaf trust anchors */
    if ((cred->credusage & (OC_CREDUSAGE_TRUSTCA | OC_CREDUSAGE_MFG_TRUSTCA)) !=
          0 &&
        !cred->child) {
      OC_DBG("search for trust anchor for cred(%d)", cred->credid);
      oc_x509_cacrt_t *cert = oc_tls_find_trust_anchor_for_cred(cred);
      if (cert == NULL) {
        OC_DBG("\ttrust anchor not found");
        return false;
      }
      OC_DBG("\ttrust anchor(%p) found", (void *)cert);
    }
  }

  // - check that the creds list contains all trust anchors from the g_ca_certs
  // list
  oc_x509_cacrt_t *cert = (oc_x509_cacrt_t *)oc_list_head(g_ca_certs);
  while (cert != NULL) {
    OC_DBG("search for cred for trust anchor(%p)", (void *)cert);
    oc_sec_cred_t *cred = oc_tls_find_cert_cred(cert->device, cert->cred);
    if (cred == NULL) {
      OC_DBG("\tcred not found");
      return false;
    }
    OC_DBG("\tcred(%d) found", cred->credid);
    cert = cert->next;
  }

  // - check that the g_ca_certs list contains all trust anchors from the
  // g_trust_anchors container
  if (!oc_tls_trust_anchors_is_empty()) {
    mbedtls_x509_crt *c = &g_trust_anchors;
    while (c != NULL) {
      OC_DBG("search for trust anchor for mbedtls trust anchor(%p)", (void *)c);
      cert = oc_tls_find_ca_cert(c);
      if (cert == NULL) {
        OC_DBG("\ttrust anchor not found");
        return false;
      }
      OC_DBG("\ttrust anchor(%p) found", (void *)cert);
      c = c->next;
    }
  }

  // - check that the g_trust_anchors container contains all trust anchors from
  // the g_ca_certs list
  cert = (oc_x509_cacrt_t *)oc_list_head(g_ca_certs);
  while (cert != NULL) {
    OC_DBG("search for mbedtls trust anchor for trust anchor(%p)",
           (void *)cert);
    mbedtls_x509_crt *c = oc_tls_find_trust_anchor(cert);
    if (c == NULL) {
      OC_DBG("\tmbedtls trust anchor not found");
      return false;
    }
    OC_DBG("\tmbedtls trust anchor(%p) found", (void *)c);
    cert = cert->next;
  }

  return true;
}

bool
oc_tls_validate_trust_anchors_consistency(void)
{
  for (size_t device = 0; device < oc_core_get_num_devices(); device++) {
    if (!oc_tls_validate_trust_anchors_consistency_for_device(device)) {
      return false;
    }
  }
  return true;
}

#endif /* OC_TEST */

static int
oc_tls_configure_end_entity_cert_chain(mbedtls_ssl_config *conf, size_t device,
                                       oc_sec_credusage_t credusage, int credid)
{
  oc_x509_crt_t *cert = (oc_x509_crt_t *)oc_list_head(g_identity_certs);
  while (cert != NULL) {
    if (cert->device == device && cert->cred->credusage == credusage &&
        (credid == -1 || cert->cred->credid == credid)) {
      break;
    }
    cert = cert->next;
  }

  if (!cert || mbedtls_ssl_conf_own_cert(conf, &cert->cert, &cert->pk) != 0) {
    OC_WRN("error configuring identity cert");
    return -1;
  }

  return 0;
}

static int
oc_tls_load_mfg_cert_chain(mbedtls_ssl_config *conf, size_t device, int credid)
{
  OC_DBG("loading manufacturer cert chain");
  return oc_tls_configure_end_entity_cert_chain(conf, device,
                                                OC_CREDUSAGE_MFG_CERT, credid);
}

static int
oc_tls_load_identity_cert_chain(mbedtls_ssl_config *conf, size_t device,
                                int credid)
{
  OC_DBG("loading identity cert chain");
  return oc_tls_configure_end_entity_cert_chain(
    conf, device, OC_CREDUSAGE_IDENTITY_CERT, credid);
}

static bool
is_known_trust_anchor(oc_sec_cred_t *cred)
{
  oc_x509_cacrt_t *cert = (oc_x509_cacrt_t *)oc_list_head(g_ca_certs);

  for (; cert != NULL; cert = cert->next) {
    if (cert->cred == cred) {
      return true;
    }
  }

  return false;
}

static void
add_new_trust_anchor(oc_sec_cred_t *cred, size_t device)
{
  int ret = mbedtls_x509_crt_parse(
    &g_trust_anchors, (const unsigned char *)oc_string(cred->publicdata.data),
    oc_string_len(cred->publicdata.data) + 1);
  if (ret != 0) {
    OC_WRN("could not parse an trustca/mfgtrustca root certificate %d", ret);
    return;
  }

  oc_x509_cacrt_t *cert = oc_memb_alloc(&g_ca_certs_s);
  if (!cert) {
    OC_WRN("could not allocate memory for new trust anchor");
    return;
  }

  cert->device = device;
  cert->cred = cred;
  mbedtls_x509_crt *c = &g_trust_anchors;
#ifdef OC_DEBUG
  int chain_length = 1;
#endif /* OC_DEBUG */
  while (c->next) {
#ifdef OC_DEBUG
    ++chain_length;
#endif /* OC_DEBUG */
    c = c->next;
  }
  cert->cert = c;
#ifdef OC_DEBUG
  char buf[256];
  if (mbedtls_x509_serial_gets(buf, sizeof(buf) - 1, &c->serial) > 0) {
    OC_DBG("trust anchor(serial: %s) added to chain", buf);
  }
  OC_DBG("trust anchor chain is now of size %d", chain_length);
#endif /* OC_DEBUG */

  oc_list_add(g_ca_certs, cert);
  OC_DBG("appended new trust anchor to ca certs");
}

void
oc_tls_resolve_new_trust_anchors(void)
{
  OC_DBG("adding new trust anchors");
  oc_tls_add_new_certs(OC_CREDUSAGE_MFG_TRUSTCA | OC_CREDUSAGE_TRUSTCA,
                       is_known_trust_anchor, add_new_trust_anchor);
}

#ifdef OC_CLIENT
void
oc_tls_reset_ciphersuite()
{
  OC_DBG("oc_tls: client resets ciphersuite priority");
  ciphers = (int *)NULL;
}

void
oc_tls_select_cert_ciphersuite(void)
{
  OC_DBG("oc_tls: client requesting cert ciphersuite priority");
  ciphers = (int *)cert_priority;
}

#ifdef OC_CLOUD
void
oc_tls_select_cloud_ciphersuite(void)
{
  OC_DBG("oc_tls: client requesting cloud ciphersuite priority");
  ciphers = (int *)cloud_priority;
}
#endif /* OC_CLOUD */
#endif /* OC_CLIENT */

void
oc_tls_select_mfg_cert_chain(int credid)
{
  selected_mfg_cred = credid;
}

void
oc_tls_select_identity_cert_chain(int credid)
{
  selected_id_cred = credid;
}

static int
get_cert_from_ssl_config(void *ctx, const mbedtls_x509_crt *own_cert,
                         const mbedtls_pk_context *pk_key)
{
  (void)pk_key;
  const mbedtls_x509_crt **out = (const mbedtls_x509_crt **)ctx;
  *out = own_cert;
  return 1;
}

static oc_x509_crt_t *
get_identity_cert_for_session(const mbedtls_ssl_config *conf)
{
  const mbedtls_x509_crt *conf_cert = NULL;
  mbedtls_ssl_conf_iterate_own_certs(conf, get_cert_from_ssl_config,
                                     &conf_cert);
  if (conf_cert == NULL) {
    OC_ERR("no identify certificate found in SSL configuration");
    return NULL;
  }

  oc_x509_crt_t *cert = (oc_x509_crt_t *)oc_list_head(g_identity_certs);
  while (cert != NULL) {
    if (&cert->cert == conf_cert) {
      return cert;
    }
    cert = cert->next;
  }
  return NULL;
}
#endif /* OC_PKI */

static void
oc_tls_set_ciphersuites(mbedtls_ssl_config *conf, const oc_endpoint_t *endpoint)
{
#ifdef OC_PKI
  mbedtls_ssl_conf_ca_chain(conf, &g_trust_anchors, NULL);
#ifdef OC_CLIENT
  bool loaded_chain = false;
#endif /* OC_CLIENT */
  size_t device = endpoint->device;
  oc_sec_doxm_t *doxm = oc_sec_get_doxm(device);
  /* Decide between configuring the identity cert chain vs manufacturer cert
   * chain for this device based on device ownership status.
   */
  if (doxm->owned &&
      oc_tls_load_identity_cert_chain(conf, device, selected_id_cred) == 0) {
#ifdef OC_CLIENT
    loaded_chain = true;
#endif /* OC_CLIENT */
  } else if (oc_tls_load_mfg_cert_chain(conf, device, selected_mfg_cred) == 0) {
#ifdef OC_CLIENT
    loaded_chain = true;
#endif /* OC_CLIENT */
  }
  selected_mfg_cred = -1;
  selected_id_cred = -1;
#endif /* OC_PKI */
  oc_sec_pstat_t *ps = oc_sec_get_pstat(endpoint->device);
  if (conf->endpoint == MBEDTLS_SSL_IS_SERVER && ps->s == OC_DOS_RFOTM) {
    OC_DBG(
      "oc_tls_set_ciphersuites: server selecting OTM ciphersuite priority");
    oc_sec_doxm_t *d = oc_sec_get_doxm(endpoint->device);
    switch (d->oxmsel) {
    case OC_OXMTYPE_JW:
      OC_DBG("oc_tls: selected JW OTM priority");
      ciphers = (int *)jw_otm_priority;
      break;
    case OC_OXMTYPE_RDP:
      OC_DBG("oc_tls: selected PIN OTM priority");
      ciphers = (int *)pin_otm_priority;
      break;
#ifdef OC_PKI
    case OC_OXMTYPE_MFG_CERT:
      OC_DBG("oc_tls: selected cert OTM priority");
      ciphers = (int *)cert_otm_priority;
      break;
#endif /* OC_PKI */
    default:
      OC_DBG("oc_tls: selected default OTM priority");
      ciphers = (int *)default_priority;
      break;
    }
  } else if (!ciphers) {
    OC_DBG("oc_tls_set_ciphersuites: server selecting default ciphersuite "
           "priority");
    ciphers = (int *)default_priority;
#ifdef OC_CLIENT
    if (conf->endpoint == MBEDTLS_SSL_IS_CLIENT) {
      oc_sec_cred_t *cred =
        oc_sec_find_creds_for_subject(NULL, &endpoint->di, endpoint->device);
      if (cred && cred->credtype == OC_CREDTYPE_PSK) {
        OC_DBG("oc_tls_set_ciphersuites: client selecting PSK ciphersuite "
               "priority");
        ciphers = (int *)psk_priority;
      }
#ifdef OC_PKI
      else if (loaded_chain) {
        OC_DBG("oc_tls_set_ciphersuites: client selecting cert ciphersuite "
               "priority");
        ciphers = (int *)cert_priority;
      }
#endif /* OC_PKI */
    }
#endif /* OC_CLIENT */
  }
  mbedtls_ssl_conf_ciphersuites(conf, ciphers);
  ciphers = NULL;
  OC_DBG("oc_tls: resetting ciphersuite selection for next handshakes");
}

#ifdef OC_CLIENT
void
oc_tls_select_psk_ciphersuite(void)
{
  OC_DBG("oc_tls: client requesting PSK ciphersuite priority");
  ciphers = (int *)psk_priority;
}

void
oc_tls_select_anon_ciphersuite(void)
{
  OC_DBG("oc_tls: client requesting anon ECDH ciphersuite priority");
  ciphers = (int *)anon_ecdh_priority;
}
#endif /* OC_CLIENT */

#ifdef OC_PKI
static int
verify_certificate(void *opq, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
  oc_tls_peer_t *peer = (oc_tls_peer_t *)opq;
  uint32_t f = 0;
  if (flags == NULL) {
    flags = &f;
  }
  OC_DBG("verifying certificate at depth %d with flags %u", depth, *flags);
  const char *audit_message =
    "DLTS handshake error, failed to verify end entity cert";
  if (depth > 0) {
    audit_message =
      "DLTS handshake error, failed to verify root or intermediate cert";
    /* For D2D handshakes involving identity certificates:
     * Find a trusted root that matches the peer's root and store it
     * as context accompanying the identity certificate. This is queried
     * after validating the end-entity certificate to authorize the
     * the peer per the OCF Specification. */
    oc_x509_crt_t *id_cert = get_identity_cert_for_session(&peer->ssl_conf);
    oc_sec_pstat_t *ps = oc_sec_get_pstat(peer->endpoint.device);
    if (oc_certs_validate_non_end_entity_cert(crt, true, ps->s == OC_DOS_RFOTM,
                                              depth, flags) < 0) {
      if (oc_certs_validate_non_end_entity_cert(
            crt, false, ps->s == OC_DOS_RFOTM, depth, flags) < 0) {
        OC_ERR("failed to verify root or intermediate cert");
        oc_tls_audit_log("AUTH-1", audit_message, 0x08, 1, peer);
        return -1;
      }
    } else {
      if (id_cert && id_cert->cred->credusage == OC_CREDUSAGE_IDENTITY_CERT) {
        oc_x509_cacrt_t *ca_cert = (oc_x509_cacrt_t *)oc_list_head(g_ca_certs);
        while (ca_cert) {
          if (ca_cert->device == id_cert->device &&
              ca_cert->cred->credusage == OC_CREDUSAGE_TRUSTCA &&
              crt->raw.len == ca_cert->cert->raw.len &&
              memcmp(crt->raw.p, ca_cert->cert->raw.p, crt->raw.len) == 0) {
            id_cert->ctx = ca_cert;
            break;
          }
          ca_cert = ca_cert->next;
        }
      }
    }
  } else if (oc_certs_validate_end_entity_cert(crt, flags) < 0) {
    OC_ERR("failed to verify end entity cert");
    oc_tls_audit_log("AUTH-1", audit_message, 0x08, 1, peer);
    return -1;
  }

  if (depth == 0) {
    oc_x509_crt_t *id_cert = get_identity_cert_for_session(&peer->ssl_conf);

    /* Parse the peer's subjectuuid from its end-entity certificate */
    char uuid[OC_UUID_LEN] = { 0 };
    if (!oc_certs_extract_CN_for_UUID(crt, uuid, sizeof(uuid))) {
      if (id_cert && id_cert->cred->credusage == OC_CREDUSAGE_IDENTITY_CERT) {
        OC_ERR("unable to retrieve UUID from the cert's CN");
        return -1;
      } else {
        peer->uuid.id[0] = '*';
        OC_DBG("attempting to connect with peer *");
      }
    } else {
      oc_str_to_uuid(uuid, &peer->uuid);
      OC_DBG("attempting to connect with peer %s", uuid);
    }

    if (oc_certs_extract_public_key_to_oc_string(crt, &peer->public_key) < 0) {
      OC_ERR("unable to extract public key from cert");
      return -1;
    }

    if (id_cert && id_cert->cred->credusage != OC_CREDUSAGE_MFG_CERT) {
      OC_DBG("checking if peer is authorized to connect with us");
      oc_uuid_t wildcard_sub;
      memset(&wildcard_sub, 0, sizeof(oc_uuid_t));
      wildcard_sub.id[0] = '*';

      /* Get a handle to the peer's root certificate */
      if (!id_cert->ctx || !id_cert->ctx->cert) {
        OC_DBG("could not find peer's root certificate");
        return -1;
      }
      mbedtls_x509_crt *root_crt = id_cert->ctx->cert;

      OC_DBG(
        "looking for a matching trustca entry currently tracked by oc_tls");
      oc_x509_cacrt_t *ca_cert = (oc_x509_cacrt_t *)oc_list_head(g_ca_certs);
      for (; ca_cert != NULL && ca_cert->device == id_cert->device &&
             ca_cert->cred->credusage == OC_CREDUSAGE_TRUSTCA;
           ca_cert = ca_cert->next) {
        if (root_crt->raw.len == ca_cert->cert->raw.len &&
            memcmp(root_crt->raw.p, ca_cert->cert->raw.p, root_crt->raw.len) ==
              0) {
        } else {
          OC_DBG("trustca mismatch, check next known trustca");
          continue;
        }
        OC_DBG("found matching trustca; check if trustca's cred entry has a "
               "UUID matching with the peer's UUID, or *");
#ifdef OC_DEBUG
        if (ca_cert->cred->subjectuuid.id[0] != '*') {
          char ca_uuid[OC_UUID_LEN] = { 0 };
          oc_uuid_to_str(&ca_cert->cred->subjectuuid, ca_uuid, sizeof(ca_uuid));
          OC_DBG("trustca cred UUID is %s", ca_uuid);
        } else {
          OC_DBG("trustca cred UUID is the wildcard *");
        }
#endif /* OC_DEBUG */
        if (memcmp(ca_cert->cred->subjectuuid.id, peer->uuid.id, 16) != 0) {
          if (memcmp(ca_cert->cred->subjectuuid.id, wildcard_sub.id, 16) != 0) {
            OC_DBG("trustca cred's UUID does not match with with peer's UUID "
                   "or the wildcard subject *; checking next known trustca");
            continue;
          } else {
            OC_DBG("trustca cred entry bears the wildcard subject * -> "
                   "authorization successful");
            break;
          }
        } else {
          OC_DBG("trustca cred entry has subjectuuid that matches with the "
                 "peer's UUID -> authorization successful");
          break;
        }
      }

      if (!ca_cert) {
        OC_ERR("could not find authorizing trustca cred for peer");
        return -1;
      }
    }
  }
  OC_DBG("verified certificate at depth %d", depth);
  if ((oc_pki_get_verify_certificate_cb()(peer, crt, depth, flags) != 0) ||
      (*flags != 0)) {
    OC_ERR("failed in global verify certificate callback with flags %u",
           *flags);
    oc_tls_audit_log("AUTH-1", audit_message, 0x08, 1, peer);
    return -1;
  }
  return 0;
}

#if defined(OC_CLOUD) && defined(OC_CLIENT)
static int
verify_cloud_certificate(void *opq, mbedtls_x509_crt *crt, int depth,
                         uint32_t *flags)
{
  oc_tls_peer_t *peer = (oc_tls_peer_t *)opq;
  uint32_t f = 0;
  if (flags == NULL) {
    flags = &f;
  }
  OC_DBG("verifying cloud certificate at depth %d with flags %u", depth,
         *flags);

  if (depth != 0) {
    return oc_pki_get_verify_certificate_cb()(peer, crt, depth, flags);
  }
  char uuid[OC_UUID_LEN] = { 0 };
  if (!oc_certs_extract_CN_for_UUID(crt, uuid, sizeof(uuid))) {
    peer->uuid.id[0] = '*';
  } else {
    oc_str_to_uuid(uuid, &peer->uuid);
  }
  if (oc_pki_get_verify_certificate_cb()(peer, crt, depth, flags) != 0) {
    OC_ERR("failed in global verify certificate callback");
    return -1;
  }
  return *flags == 0 ? 0 : -1;
}
#endif /* OC_CLOUD && OC_CLIENT */
#endif /* OC_PKI */

static int
oc_tls_populate_ssl_config(mbedtls_ssl_config *conf, size_t device, int role,
                           int transport_type)
{
  mbedtls_ssl_config_init(conf);

  if (mbedtls_ssl_config_defaults(conf, role, transport_type,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    return -1;
  }

  oc_uuid_t *device_id = oc_core_get_device_id(device);
#ifdef OC_CLIENT
  if (role == MBEDTLS_SSL_IS_CLIENT && use_pin_obt_psk_identity) {
    use_pin_obt_psk_identity = false;
    if (mbedtls_ssl_conf_psk(conf, device_id->id, 1,
                             (const unsigned char *)"oic.sec.doxm.rdp",
                             16) != 0) {
      return -1;
    }
  } else
#endif /* OC_CLIENT */
  {
    unsigned char identity_hint[33];
    size_t identity_hint_len = 33;
    oc_sec_doxm_t *doxm = oc_sec_get_doxm(device);
    oc_sec_pstat_t *pstat = oc_sec_get_pstat(device);
    if (pstat->s == OC_DOS_RFOTM && doxm->oxmsel == OC_OXMTYPE_RDP) {
      memcpy(identity_hint, "oic.sec.doxm.rdp:", 17);
      memcpy(identity_hint + 17, device_id->id, 16);
      identity_hint_len = 33;
    } else {
      memcpy(identity_hint, device_id->id, 16);
      identity_hint_len = 16;
    }
    if (mbedtls_ssl_conf_psk(conf, identity_hint, 1, identity_hint,
                             identity_hint_len) != 0) {
      return -1;
    }
  }

#ifdef OC_DEBUG
  mbedtls_ssl_conf_dbg(conf, oc_mbedtls_debug, stdout);
#endif /* OC_DEBUG */

  mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, &g_oc_ctr_drbg_ctx);
  mbedtls_ssl_conf_min_version(conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                               MBEDTLS_SSL_MINOR_VERSION_3);
  oc_sec_pstat_t *ps = oc_sec_get_pstat(device);
  if ((ps->s > OC_DOS_RFOTM) || (role != MBEDTLS_SSL_IS_SERVER)) {
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  }
  mbedtls_ssl_conf_psk_cb(conf, get_psk_cb, NULL);
  if (transport_type == MBEDTLS_SSL_TRANSPORT_DATAGRAM) {
    mbedtls_ssl_conf_dtls_cookies(conf, mbedtls_ssl_cookie_write,
                                  mbedtls_ssl_cookie_check, &g_cookie_ctx);
    mbedtls_ssl_conf_handshake_timeout(conf, 1000, 20000);
  }

  return 0;
}

int
oc_tls_num_peers(size_t device)
{
  int num_peers = 0;
  oc_tls_peer_t *peer = (oc_tls_peer_t *)oc_list_head(g_tls_peers);
  while (peer) {
    if (peer->endpoint.device == device) {
      ++num_peers;
    }
    peer = peer->next;
  }
  return num_peers;
}

static oc_tls_peer_t *
oc_tls_peer_allocate(const oc_endpoint_t *endpoint, int role, bool doc)
{
  oc_tls_peer_t *peer = oc_memb_alloc(&g_tls_peers_s);
  if (peer == NULL) {
    OC_WRN("TLS peers exhausted");
    return NULL;
  }
  OC_DBG("oc_tls: Allocating new peer");
  memcpy(&peer->endpoint, endpoint, sizeof(oc_endpoint_t));
  OC_LIST_STRUCT_INIT(peer, recv_q);
  OC_LIST_STRUCT_INIT(peer, send_q);
  peer->next = NULL;
  peer->doc = doc;
  assert(role == MBEDTLS_SSL_IS_CLIENT || role == MBEDTLS_SSL_IS_SERVER);
  peer->role = role;
  memset(&peer->timer, 0, sizeof(oc_tls_retr_timer_t));
#ifdef OC_TCP
  peer->processed_recv_message = NULL;
#endif /* OC_TCP */
  return peer;
}

static void
oc_tls_export_keys(void *p_expkey, mbedtls_ssl_key_export_type type,
                   const unsigned char *secret, size_t secret_len,
                   const unsigned char client_random[32],
                   const unsigned char server_random[32],
                   mbedtls_tls_prf_types tls_prf_type)
{
  (void)type;
  (void)tls_prf_type;
  OC_DBG("oc_tls_export_keys: Type=%d", type);

  oc_tls_peer_t *peer = (oc_tls_peer_t *)p_expkey;
  OC_DBG("oc_tls: Got master secret (len=%zu)", secret_len);
  assert(secret_len == sizeof(peer->master_secret));
  memcpy(peer->master_secret, secret, secret_len);
  OC_LOGbytes(peer->master_secret, secret_len);

  memcpy(peer->client_server_random, client_random, 32);
  memcpy(peer->client_server_random + 32, server_random, 32);
  OC_DBG("oc_tls: Got nonce\n");
  OC_LOGbytes(peer->client_server_random, sizeof(peer->client_server_random));
}

static int
oc_tls_peer_ssl_init(oc_tls_peer_t *peer)
{
  mbedtls_ssl_init(&peer->ssl_ctx);

  int transport_type = (peer->endpoint.flags & TCP) != 0
                         ? MBEDTLS_SSL_TRANSPORT_STREAM
                         : MBEDTLS_SSL_TRANSPORT_DATAGRAM;

  if (oc_tls_populate_ssl_config(&peer->ssl_conf, peer->endpoint.device,
                                 peer->role, transport_type) < 0) {
    OC_ERR("oc_tls: error in tls_populate_ssl_config");
    return -1;
  }

#ifdef OC_PKI
#if defined(OC_CLOUD) && defined(OC_CLIENT)
  if (ciphers == cloud_priority) {
    mbedtls_ssl_conf_verify(&peer->ssl_conf, verify_cloud_certificate, peer);
  } else
#endif /* OC_CLOUD && OC_CLIENT */
  {
    mbedtls_ssl_conf_verify(&peer->ssl_conf, verify_certificate, peer);
  }
#endif /* OC_PKI */

  oc_tls_set_ciphersuites(&peer->ssl_conf, &peer->endpoint);

  int err = mbedtls_ssl_setup(&peer->ssl_ctx, &peer->ssl_conf);
  if (err != 0) {
    OC_ERR("oc_tls: error in mbedtls_ssl_setup: %d", err);
    return -1;
  }
  /* Fix maximum size of outgoing encrypted application payloads when sent
   * over UDP */
  if (transport_type == MBEDTLS_SSL_TRANSPORT_DATAGRAM) {
    mbedtls_ssl_set_mtu(&peer->ssl_ctx,
                        (OC_PDU_SIZE > UINT16_MAX ? UINT16_MAX : OC_PDU_SIZE));
  }

  mbedtls_ssl_set_bio(&peer->ssl_ctx, peer, ssl_send, ssl_recv, NULL);

  if (peer->role == MBEDTLS_SSL_IS_SERVER &&
      mbedtls_ssl_set_client_transport_id(
        &peer->ssl_ctx, (const unsigned char *)&peer->endpoint.addr,
        sizeof(peer->endpoint.addr)) != 0) {
    return -1;
  }

  mbedtls_ssl_set_export_keys_cb(&peer->ssl_ctx, oc_tls_export_keys, peer);
  return 0;
}

oc_tls_peer_t *
oc_tls_add_peer(const oc_endpoint_t *endpoint, int role)
{
  oc_tls_peer_t *peer = oc_tls_get_peer(endpoint);
  if (peer != NULL) {
    return peer;
  }

  /* Check if this a Device Ownership Connection (DOC) */
  bool doc = false;
  const oc_sec_pstat_t *pstat = oc_sec_get_pstat(endpoint->device);
  if (pstat->s == OC_DOS_RFOTM) {
    const oc_sec_doxm_t *doxm = oc_sec_get_doxm(endpoint->device);
    if (doxm->oxmsel == 4) {
      /* Prior to a successful anonymous Update of "oxmsel" in
       *  "/oic/sec/doxm", all attempts to establish new DTLS connections
       * shall be rejected.
       */
      return NULL;
    }
    if (oc_list_length(g_tls_peers) != 0) {
      /* Allow only a single DOC */
      return NULL;
    }
    doc = true;
  }
  peer = oc_tls_peer_allocate(endpoint, role, doc);
  if (peer == NULL) {
    return NULL;
  }

  if (oc_tls_peer_ssl_init(peer) != 0) {
    oc_tls_free_peer(peer, false);
    return NULL;
  }

  oc_list_add(g_tls_peers, peer);

  if ((endpoint->flags & TCP) == 0) {
    mbedtls_ssl_set_timer_cb(&peer->ssl_ctx, &peer->timer, ssl_set_timer,
                             ssl_get_timer);
    oc_ri_add_timed_event_callback_seconds(
      peer, oc_tls_inactive, (oc_clock_time_t)OC_DTLS_INACTIVITY_TIMEOUT);
  }
  return peer;
}

void
oc_tls_shutdown(void)
{
  oc_tls_peer_t *p = oc_list_pop(g_tls_peers);
  while (p != NULL) {
    oc_tls_free_peer(p, false);
    p = oc_list_pop(g_tls_peers);
  }
#ifdef OC_PKI
  oc_x509_crt_t *cert = (oc_x509_crt_t *)oc_list_pop(g_identity_certs);
  while (cert != NULL) {
    mbedtls_x509_crt_free(&cert->cert);
    mbedtls_pk_free(&cert->pk);
    oc_memb_free(&g_identity_certs_s, cert);
    cert = (oc_x509_crt_t *)oc_list_pop(g_identity_certs);
  }
  oc_x509_cacrt_t *ca = (oc_x509_cacrt_t *)oc_list_pop(g_ca_certs);
  while (ca) {
    oc_memb_free(&g_ca_certs_s, ca);
    ca = (oc_x509_cacrt_t *)oc_list_pop(g_ca_certs);
  }
  mbedtls_x509_crt_free(&g_trust_anchors);
#endif /* OC_PKI */
  mbedtls_ctr_drbg_free(&g_oc_ctr_drbg_ctx);
  mbedtls_ssl_cookie_free(&g_cookie_ctx);
  mbedtls_entropy_free(&g_entropy_ctx);
}

int
oc_tls_init_context(void)
{
  oc_mbedtls_init();
  mbedtls_entropy_init(&g_entropy_ctx);
  oc_entropy_add_source(&g_entropy_ctx);
  mbedtls_ssl_cookie_init(&g_cookie_ctx);
  mbedtls_ctr_drbg_init(&g_oc_ctr_drbg_ctx);
  if (mbedtls_ctr_drbg_seed(&g_oc_ctr_drbg_ctx, mbedtls_entropy_func,
                            &g_entropy_ctx,
                            (const unsigned char *)PERSONALIZATION_DATA,
                            strlen(PERSONALIZATION_DATA)) != 0) {
    OC_ERR("error initializing RNG");
    goto dtls_init_err;
  }
  if (mbedtls_ssl_cookie_setup(&g_cookie_ctx, mbedtls_ctr_drbg_random,
                               &g_oc_ctr_drbg_ctx) != 0) {
    goto dtls_init_err;
  }

#ifdef OC_PKI
  mbedtls_x509_crt_init(&g_trust_anchors);
#endif /* OC_PKI */

  return 0;
dtls_init_err:
  OC_ERR("oc_tls: TLS initialization error");
  oc_tls_shutdown();
  return -1;
}

void
oc_tls_close_connection(const oc_endpoint_t *endpoint)
{
  oc_tls_peer_t *peer = oc_tls_get_peer(endpoint);
  if (peer != NULL) {
    oc_tls_close_peer(peer);
  }
}

static int
oc_tls_prf(const uint8_t *secret, size_t secret_len, uint8_t *output,
           size_t output_len, size_t num_message_fragments, ...)
{
#define MBEDTLS_MD(func, ...)                                                  \
  do {                                                                         \
    if (func(__VA_ARGS__) != 0) {                                              \
      gen_output = -1;                                                         \
      goto exit_tls_prf;                                                       \
    }                                                                          \
  } while (0)
  uint8_t A[MBEDTLS_MD_MAX_SIZE], buf[MBEDTLS_MD_MAX_SIZE];
  size_t i, msg_len;
  int gen_output = 0, copy_len,
      hash_len =
        mbedtls_md_get_size(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256));
  mbedtls_md_context_t hmacA, hmacA_next;
  va_list msg_list;
  const uint8_t *msg;
  va_start(msg_list, num_message_fragments);

  mbedtls_md_init(&hmacA);
  mbedtls_md_init(&hmacA_next);

  MBEDTLS_MD(mbedtls_md_setup, &hmacA,
             mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  MBEDTLS_MD(mbedtls_md_setup, &hmacA_next,
             mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);

  MBEDTLS_MD(mbedtls_md_hmac_starts, &hmacA, secret, secret_len);
  for (i = 0; i < num_message_fragments; i++) {
    msg = va_arg(msg_list, const uint8_t *);
    msg_len = va_arg(msg_list, size_t);
    MBEDTLS_MD(mbedtls_md_hmac_update, &hmacA, msg, msg_len);
  }
  va_end(msg_list);
  MBEDTLS_MD(mbedtls_md_hmac_finish, &hmacA, A);

  while (gen_output < (int)output_len) {
    MBEDTLS_MD(mbedtls_md_hmac_reset, &hmacA);
    MBEDTLS_MD(mbedtls_md_hmac_starts, &hmacA, secret, secret_len);
    MBEDTLS_MD(mbedtls_md_hmac_update, &hmacA, A, hash_len);
    va_start(msg_list, num_message_fragments);
    for (i = 0; i < num_message_fragments; i++) {
      msg = va_arg(msg_list, const uint8_t *);
      msg_len = va_arg(msg_list, size_t);
      MBEDTLS_MD(mbedtls_md_hmac_update, &hmacA, msg, msg_len);
    }
    va_end(msg_list);
    MBEDTLS_MD(mbedtls_md_hmac_finish, &hmacA, buf);

    copy_len = (((int)output_len - gen_output) < hash_len)
                 ? ((int)output_len - gen_output)
                 : hash_len;
    memcpy(output + gen_output, buf, copy_len);
    gen_output += copy_len;

    if (copy_len == hash_len) {
      MBEDTLS_MD(mbedtls_md_hmac_reset, &hmacA_next);
      MBEDTLS_MD(mbedtls_md_hmac_starts, &hmacA_next, secret, secret_len);
      MBEDTLS_MD(mbedtls_md_hmac_update, &hmacA_next, A, hash_len);
      MBEDTLS_MD(mbedtls_md_hmac_finish, &hmacA_next, A);
    }
  }

exit_tls_prf:
#undef MBEDTLS_MD
  va_end(msg_list);
  mbedtls_md_free(&hmacA);
  mbedtls_md_free(&hmacA_next);
  return gen_output;
}

bool
oc_sec_derive_owner_psk(const oc_endpoint_t *endpoint, const uint8_t *oxm,
                        const size_t oxm_len, const uint8_t *server_uuid,
                        const size_t server_uuid_len, const uint8_t *obt_uuid,
                        const size_t obt_uuid_len, uint8_t *key,
                        const size_t key_len)
{
  oc_tls_peer_t *peer = oc_tls_get_peer(endpoint);
  if (!peer) {
    return false;
  }
  if (!peer->ssl_ctx.session) {
    return false;
  }
  size_t j;
  for (j = 0; j < 48; j++) {
    if (peer->master_secret[j] != 0) {
      break;
    }
  }
  if (j == 48) {
    return false;
  }
  for (j = 0; j < 64; j++) {
    if (peer->client_server_random[j] != 0) {
      break;
    }
  }
  if (j == 64) {
    return false;
  }
  uint8_t key_block[184];

  uint8_t label[] = { 0x6b, 0x65, 0x79, 0x20, 0x65, 0x78, 0x70,
                      0x61, 0x6e, 0x73, 0x69, 0x6f, 0x6e };

  // key_block_len set up according to OIC 1.1 Security Specification Section
  // 7.3.2
  int mac_key_len = 0;
  int iv_size = 0;
  int key_size = 0;
  int key_block_len = 0;
  if (MBEDTLS_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA256 ==
        peer->ssl_ctx.session->ciphersuite ||
      MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256 ==
        peer->ssl_ctx.session->ciphersuite ||
      MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256 ==
        peer->ssl_ctx.session->ciphersuite) {
    // 2 * ( 32 + 0 + 16 ) = 96
    mac_key_len = SHA256_MAC_KEY_LENGTH;
    iv_size = CBC_IV_LENGTH;
    key_size = AES128_KEY_LENGTH;
  } else if (MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM ==
               peer->ssl_ctx.session->ciphersuite ||
             MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8 ==
               peer->ssl_ctx.session->ciphersuite) {
    // 2 * ( 0 + 4 + 16 ) = 40
    mac_key_len = CCM_MAC_KEY_LENGTH;
    iv_size = CCM_IV_LENGTH;
    key_size = AES128_KEY_LENGTH;
  } else if (peer->ssl_ctx.session->ciphersuite ==
               MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CCM ||
             peer->ssl_ctx.session->ciphersuite ==
               MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CCM_8) {
    // 2 * (0 + 4 + 32) = 72
    mac_key_len = CCM_MAC_KEY_LENGTH;
    iv_size = CCM_IV_LENGTH;
    key_size = AES256_KEY_LENGTH;
  } else if (MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 ==
             peer->ssl_ctx.session->ciphersuite) {
    // 2 * ( 32 + 12 + 16 ) = 120
    mac_key_len = SHA256_MAC_KEY_LENGTH;
    iv_size = GCM_IV_LENGTH;
    key_size = AES128_KEY_LENGTH;
  }

  key_block_len = 2 * (mac_key_len + key_size + iv_size);

  if (oc_tls_prf(peer->master_secret, 48, key_block, key_block_len, 3, label,
                 sizeof(label), peer->client_server_random + 32, (size_t)32,
                 peer->client_server_random, (size_t)32) != key_block_len) {
    return false;
  }

  if (oc_tls_prf(key_block, key_block_len, key, key_len, 3, oxm, oxm_len,
                 obt_uuid, obt_uuid_len, server_uuid,
                 server_uuid_len) != (int)key_len) {
    return false;
  }

  OC_DBG("oc_tls: master secret:");
  OC_LOGbytes(peer->master_secret, 48);
  OC_DBG("oc_tls: client_server_random:");
  OC_LOGbytes(peer->client_server_random, 64);
  OC_DBG("oc_tls: key_block");
  OC_LOGbytes(key_block, key_block_len);
  OC_DBG("oc_tls: PSK ");
  OC_LOGbytes(key, key_len);

  return true;
}

#ifdef OC_TCP
static int
ssl_write_tcp(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len)
{
  size_t length = 0;
  while (length < len) {
    int ret = mbedtls_ssl_write(ssl, buf + length, len - length);
    if (ret < 0) {
      if (ret == MBEDTLS_ERR_SSL_WANT_READ &&
          ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        continue;
      }
      return ret;
    }
    length += ret;
  }
  return (int)length;
}
#endif

size_t
oc_tls_send_message(oc_message_t *message)
{
  size_t length = 0;
  oc_tls_peer_t *peer = oc_tls_get_peer(&message->endpoint);
  if (peer) {
    int ret = 0;
#ifdef OC_TCP
    if (peer->endpoint.flags & TCP) {
      ret = ssl_write_tcp(&peer->ssl_ctx, (unsigned char *)message->data,
                          message->length);
    } else
#endif
    {
      ret = mbedtls_ssl_write(&peer->ssl_ctx, (unsigned char *)message->data,
                              message->length);
    }
    if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ &&
        ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
#ifdef OC_DEBUG
      char buf[256];
      mbedtls_strerror(ret, buf, 256);
      OC_ERR("oc_tls: mbedtls_error: %s", buf);
#endif /* OC_DEBUG */
      oc_tls_free_peer(peer, false);
    } else {
      length = message->length;
    }
  }
  oc_message_unref(message);
  return length;
}

static bool
oc_tls_peer_connected(const oc_tls_peer_t *peer)
{
  return peer->ssl_ctx.state == MBEDTLS_SSL_HANDSHAKE_OVER;
}

bool
oc_tls_connected(const oc_endpoint_t *endpoint)
{
  oc_tls_peer_t *peer = oc_tls_get_peer(endpoint);
  if (peer != NULL) {
    return oc_tls_peer_connected(peer);
  }
  return false;
}

#ifdef OC_CLIENT
static void
write_application_data(oc_tls_peer_t *peer)
{
  if (!is_peer_active(peer)) {
    OC_DBG("oc_tls: write_application_data: Peer not active");
    return;
  }
  oc_message_t *message = (oc_message_t *)oc_list_pop(peer->send_q);
  while (message != NULL) {
    int ret = 0;
#ifdef OC_TCP
    if (peer->endpoint.flags & TCP) {
      ret = ssl_write_tcp(&peer->ssl_ctx, (unsigned char *)message->data,
                          message->length);
    } else
#endif
    {
      ret = mbedtls_ssl_write(&peer->ssl_ctx, (unsigned char *)message->data,
                              message->length);
    }
    oc_message_unref(message);
    if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ &&
        ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
#ifdef OC_DEBUG
      char buf[256];
      mbedtls_strerror(ret, buf, 256);
      OC_ERR("oc_tls: mbedtls_error: %s", buf);
#endif /* OC_DEBUG */
      oc_tls_free_peer(peer, false);
      break;
    }
    message = (oc_message_t *)oc_list_pop(peer->send_q);
  }
}

static void
oc_tls_handshake(oc_tls_peer_t *peer)
{
  int ret = mbedtls_ssl_handshake(&peer->ssl_ctx);
  if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ &&
      ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
#ifdef OC_DEBUG
    char buf[256];
    mbedtls_strerror(ret, buf, sizeof(buf));
    OC_ERR("oc_tls: mbedtls_error: %s", buf);
#endif /* OC_DEBUG */
    oc_tls_free_peer(peer, false);
    return;
  }
  if (ret == 0) {
    oc_tls_handler_schedule_write(peer);
    return;
  }
}

#ifdef OC_HAS_FEATURE_TCP_ASYNC_CONNECT
static void
oc_tls_on_tcp_connect(const oc_endpoint_t *endpoint, int state, void *data)
{
  (void)data;
  OC_DBG("oc_tls_on_tcp_connect: %d", state);
  oc_tls_peer_t *peer = oc_tls_get_peer(endpoint);
  if (peer == NULL) {
    OC_ERR("oc_tls_on_tcp_connect: peer for endpoint not found");
    return;
  }
  if (state == OC_TCP_SOCKET_STATE_CONNECTED) {
    oc_tls_handshake(peer);
    return;
  }
  oc_tls_free_peer(peer, false);
}
#endif /* OC_HAS_FEATURE_TCP_ASYNC_CONNECT */

static void
oc_tls_init_connection(oc_message_t *message)
{
  oc_sec_pstat_t *pstat = oc_sec_get_pstat(message->endpoint.device);
  if (pstat->s != OC_DOS_RFNOP) {
    oc_message_unref(message);
    return;
  }

  oc_tls_peer_t *peer = oc_tls_get_peer(&message->endpoint);
  if (peer != NULL && peer->role != MBEDTLS_SSL_IS_CLIENT) {
    oc_tls_free_invalid_peer(peer);
    peer = NULL;
  }

  if (peer == NULL) {
    peer = oc_tls_add_peer(&message->endpoint, MBEDTLS_SSL_IS_CLIENT);
  }

  if (peer != NULL) {
    oc_message_t *duplicate = oc_list_head(peer->send_q);
    while (duplicate != NULL) {
      if (duplicate == message) {
        break;
      }
      duplicate = duplicate->next;
    }
    if (duplicate == NULL) {
      oc_message_add_ref(message);
      oc_list_add(peer->send_q, message);
    }
#ifdef OC_HAS_FEATURE_TCP_ASYNC_CONNECT
    if ((peer->endpoint.flags & TCP) != 0) {
      int state = oc_tcp_connect(&peer->endpoint, oc_tls_on_tcp_connect, NULL);
      if (state == OC_TCP_SOCKET_STATE_CONNECTED ||
          state == OC_TCP_SOCKET_ERROR_EXISTS_CONNECTED) {
        oc_tls_handshake(peer);
        oc_message_unref(message);
        return;
      }
      if (state == OC_TCP_SOCKET_STATE_CONNECTING ||
          state == OC_TCP_SOCKET_ERROR_EXISTS_CONNECTING) {
        // just wait for connection to be established; oc_tls_handshake or
        // oc_tls_free_peer will be called from oc_tls_on_tcp_connect
        oc_message_unref(message);
        return;
      }

      oc_tls_free_peer(peer, false);
      oc_message_unref(message);
      return;
    }
#endif /* OC_HAS_FEATURE_TCP_ASYNC_CONNECT */
    oc_tls_handshake(peer);
  }
  oc_message_unref(message);
}
#endif /* OC_CLIENT */

bool
oc_tls_uses_psk_cred(oc_tls_peer_t *peer)
{
  if (!peer) {
    return false;
  }
  if (!peer->ssl_ctx.session) {
    return false;
  }
  int cipher = peer->ssl_ctx.session->ciphersuite;
  if (MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256 == cipher ||
      MBEDTLS_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA256 == cipher) {
    return true;
  }
  return false;
}

oc_uuid_t *
oc_tls_get_peer_uuid(const oc_endpoint_t *endpoint)
{
  oc_tls_peer_t *peer = oc_tls_get_peer(endpoint);
  if (peer) {
    return &peer->uuid;
  }
  return NULL;
}

#if defined(OC_PKI) && defined(OC_CLIENT)
static void
assert_all_roles_internal(oc_client_response_t *data)
{
  oc_tls_handler_schedule_write(data->user_data);
}
#endif /* OC_PKI && OC_CLIENT */

#ifdef OC_TCP
#define DEFAULT_RECEIVE_SIZE                                                   \
  (COAP_TCP_DEFAULT_HEADER_LEN + COAP_TCP_MAX_EXTENDED_LENGTH_LEN)

static void
read_application_data_tcp(oc_tls_peer_t *peer)
{
  if (peer->processed_recv_message == NULL) {
    peer->processed_recv_message = oc_allocate_message();
    if (peer->processed_recv_message) {
      peer->processed_recv_message->encrypted = 0;
      memcpy(&peer->processed_recv_message->endpoint, &peer->endpoint,
             sizeof(oc_endpoint_t));
    }
  }
  while (true) {
    if (peer->processed_recv_message) {
      size_t want_read = 0;
      size_t total_length = 0;
      if (peer->processed_recv_message->length < DEFAULT_RECEIVE_SIZE) {
        want_read = DEFAULT_RECEIVE_SIZE - peer->processed_recv_message->length;
      } else {
        total_length =
          coap_tcp_get_packet_size(peer->processed_recv_message->data);
        if (total_length > (size_t)OC_PDU_SIZE) {
          OC_ERR("oc_tls_tcp: total receive length(%ld) is bigger than max pdu "
                 "size(%ld)",
                 (long)total_length, (long)OC_PDU_SIZE);
          oc_tls_free_peer(peer, false);
          return;
        }
        want_read = total_length - peer->processed_recv_message->length;
      }
      OC_DBG("oc_tls_tcp: mbedtls_ssl_read want read: %d", (int)want_read);
      int ret = mbedtls_ssl_read(&peer->ssl_ctx,
                                 peer->processed_recv_message->data +
                                   peer->processed_recv_message->length,
                                 want_read);
      OC_DBG("oc_tls_tcp: mbedtls_ssl_read returns: %d", ret);
      if (ret <= 0) {
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
          OC_DBG("oc_tls_tcp: Received WantRead/WantWrite");
          return;
        }
        oc_message_unref(peer->processed_recv_message);
        peer->processed_recv_message = NULL;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
          OC_DBG("oc_tls_tcp: Close-Notify received");
        } else if (ret == MBEDTLS_ERR_SSL_CLIENT_RECONNECT) {
          OC_DBG("oc_tls_tcp: Client wants to reconnect");
        } else {
#ifdef OC_DEBUG
          char buf[256];
          mbedtls_strerror(ret, buf, 256);
          OC_ERR("oc_tls_tcp: mbedtls_error: %s", buf);
#endif /* OC_DEBUG */
        }
        if (peer->role == MBEDTLS_SSL_IS_SERVER &&
            (peer->endpoint.flags & TCP) == 0) {
          mbedtls_ssl_close_notify(&peer->ssl_ctx);
          mbedtls_ssl_close_notify(&peer->ssl_ctx);
        }
        oc_tls_free_peer(peer, false);
        return;
      }
      peer->processed_recv_message->length += ret;
      if (total_length &&
          peer->processed_recv_message->length == total_length) {
        OC_DBG("oc_tls_tcp: Decrypted incoming message %d",
               (int)(total_length));
        peer->processed_recv_message->encrypted = 0;
        memcpy(peer->processed_recv_message->endpoint.di.id, peer->uuid.id, 16);
        if (oc_process_post(&g_coap_engine, oc_events[INBOUND_RI_EVENT],
                            peer->processed_recv_message) ==
            OC_PROCESS_ERR_FULL) {
          oc_message_unref(peer->processed_recv_message);
        }
        peer->processed_recv_message = NULL;
        return;
      }
    } else {
      return;
    }
  }
}
#endif

static void
read_application_data(oc_tls_peer_t *peer)
{
  OC_DBG("oc_tls: In read_application_data");
  if (!is_peer_active(peer)) {
    OC_DBG("oc_tls: read_application_data: Peer not active");
    return;
  }

  if (peer->ssl_ctx.state != MBEDTLS_SSL_HANDSHAKE_OVER) {
    int ret = 0;
    do {
      ret = mbedtls_ssl_handshake_step(&peer->ssl_ctx);
      if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
        mbedtls_ssl_session_reset(&peer->ssl_ctx);
        /* For HelloVerifyRequest cookies */
        if (peer->role == MBEDTLS_SSL_IS_SERVER &&
            mbedtls_ssl_set_client_transport_id(
              &peer->ssl_ctx, (const unsigned char *)&peer->endpoint.addr,
              sizeof(peer->endpoint.addr)) != 0) {
          oc_tls_free_peer(peer, false);
          return;
        }
      } else if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ &&
                 ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
#ifdef OC_DEBUG
        char buf[256];
        mbedtls_strerror(ret, buf, 256);
        OC_ERR("oc_tls: mbedtls_error: %s", buf);
#endif /* OC_DEBUG */
        oc_tls_free_peer(peer, false);
        return;
      }
    } while (ret == 0 && peer->ssl_ctx.state != MBEDTLS_SSL_HANDSHAKE_OVER);

    if (peer->ssl_ctx.state == MBEDTLS_SSL_HANDSHAKE_OVER) {
      OC_DBG("oc_tls: (D)TLS Session is connected via ciphersuite [0x%x]",
             peer->ssl_ctx.session->ciphersuite);
      oc_handle_session(&peer->endpoint, OC_SESSION_CONNECTED);
#ifdef OC_CLIENT
#ifdef OC_PKI
      if (auto_assert_all_roles && !oc_tls_uses_psk_cred(peer) &&
          oc_get_all_roles()) {
        oc_assert_all_roles(&peer->endpoint, assert_all_roles_internal, peer);
      } else
#endif /* OC_PKI */
      {
        oc_tls_handler_schedule_write(peer);
      }
#endif /* OC_CLIENT */
    }
  } else {
#ifdef OC_TCP
    if (peer->endpoint.flags & TCP) {
      read_application_data_tcp(peer);
      return;
    }
#endif
#ifdef OC_INOUT_BUFFER_SIZE
    oc_message_t message[1];
#else  /* OC_INOUT_BUFFER_SIZE */
    oc_message_t *message = oc_allocate_message();
    if (message) {
#endif /* !OC_INOUT_BUFFER_SIZE */
    memcpy(&message->endpoint, &peer->endpoint, sizeof(oc_endpoint_t));
    int ret = mbedtls_ssl_read(&peer->ssl_ctx, message->data, OC_PDU_SIZE);
    if (ret <= 0) {
#ifndef OC_INOUT_BUFFER_SIZE
      oc_message_unref(message);
#endif /* OC_INOUT_BUFFER_SIZE */
      if (ret == 0 || ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        OC_DBG("oc_tls: Received WantRead/WantWrite");
        return;
      }
      if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        OC_DBG("oc_tls: Close-Notify received");
      } else if (ret == MBEDTLS_ERR_SSL_CLIENT_RECONNECT) {
        OC_DBG("oc_tls: Client wants to reconnect");
      } else {
#ifdef OC_DEBUG
        char buf[256];
        mbedtls_strerror(ret, buf, 256);
        OC_ERR("oc_tls: mbedtls_error: %s", buf);
#endif /* OC_DEBUG */
      }
      if (peer->role == MBEDTLS_SSL_IS_SERVER &&
          (peer->endpoint.flags & TCP) == 0) {
        mbedtls_ssl_close_notify(&peer->ssl_ctx);
        mbedtls_ssl_close_notify(&peer->ssl_ctx);
      }
      oc_tls_free_peer(peer, false);
      return;
    }
    message->length = ret;
    message->encrypted = 0;
    oc_message_t *msg = message;
#ifdef OC_INOUT_BUFFER_SIZE
    msg = oc_allocate_message();
    if (!msg) {
      OC_WRN("oc_tls: could not allocate incoming message buffer");
      return;
    }
#endif /* OC_INOUT_BUFFER_SIZE */
    memcpy(&msg->endpoint, &message->endpoint, sizeof(oc_endpoint_t));
    memcpy(&msg->endpoint.di.id, &peer->uuid.id, 16);
    msg->length = message->length;
    memcpy(msg->data, message->data, message->length);
#ifdef OC_OSCORE
    if (oc_process_post(&oc_oscore_handler, oc_events[INBOUND_OSCORE_EVENT],
                        msg) == OC_PROCESS_ERR_FULL) {
#ifndef OC_INOUT_BUFFER_SIZE
      oc_message_unref(msg);
#endif /* !OC_INOUT_BUFFER_SIZE */
    }
#else /* OC_OSCORE */
      if (oc_process_post(&g_coap_engine, oc_events[INBOUND_RI_EVENT], msg) ==
          OC_PROCESS_ERR_FULL) {
#ifndef OC_INOUT_BUFFER_SIZE
        oc_message_unref(msg);
#endif /* !OC_INOUT_BUFFER_SIZE */
      }
#endif /* !OC_OSCORE */
  }
  OC_DBG("oc_tls: Decrypted incoming message");
#ifndef OC_INOUT_BUFFER_SIZE
}
#endif /* !OC_INOUT_BUFFER_SIZE */
}

static void
oc_tls_recv_message(oc_message_t *message)
{
  oc_tls_peer_t *peer =
    oc_tls_add_peer(&message->endpoint, MBEDTLS_SSL_IS_SERVER);

  if (peer == NULL) {
    oc_message_unref(message);
    return;
  }
#ifdef OC_DEBUG
  char u[OC_UUID_LEN];
  oc_uuid_to_str(&peer->uuid, u, OC_UUID_LEN);
  OC_DBG("oc_tls: Received message from device %s", u);
  if (peer->endpoint.flags & TCP) {
    OC_DBG("oc_tls_recv_message_tcp: length=%zu  peer=%p", message->length,
           (void *)peer);
  }
#endif /* OC_DEBUG */

  oc_list_add(peer->recv_q, message);
  peer->timestamp = oc_clock_time();
  oc_tls_handler_schedule_read(peer);
}

static void
close_all_tls_sessions_for_device(size_t device)
{
  OC_DBG("oc_tls: closing all open (D)TLS sessions on device %zd", device);
  oc_tls_peer_t *p = oc_list_head(g_tls_peers);
  while (p != NULL) {
    oc_tls_peer_t *next = p->next;
    if (p->endpoint.device == device) {
      oc_tls_close_connection(&p->endpoint);
    }
    p = next;
  }
}

static void
close_all_tls_sessions(void)
{
  OC_DBG("oc_tls: closing all open (D)TLS sessions on all devices");
  oc_tls_peer_t *p = oc_list_head(g_tls_peers);
  while (p != NULL) {
    oc_tls_peer_t *next = p->next;
    oc_tls_close_connection(&p->endpoint);
    p = next;
  }
}

OC_PROCESS_THREAD(oc_tls_handler, ev, data)
{
  OC_PROCESS_POLLHANDLER(close_all_tls_sessions());
  OC_PROCESS_BEGIN();
  while (true) {
    OC_PROCESS_YIELD();

    if (ev == oc_events[UDP_TO_TLS_EVENT]) {
      oc_tls_recv_message(data);
      continue;
    }
#ifdef OC_CLIENT
    if (ev == oc_events[INIT_TLS_CONN_EVENT]) {
      oc_tls_init_connection(data);
      continue;
    }
#endif /* OC_CLIENT */
    if (ev == oc_events[RI_TO_TLS_EVENT]) {
      oc_tls_send_message(data);
      continue;
    }
    if (ev == OC_PROCESS_EVENT_TIMER) {
      check_retr_timers();
      continue;
    }
    if (ev == oc_events[TLS_READ_DECRYPTED_DATA]) {
      read_application_data(data);
      continue;
    }
#ifdef OC_CLIENT
    if (ev == oc_events[TLS_WRITE_APPLICATION_DATA]) {
      write_application_data(data);
      continue;
    }
#endif /* OC_CLIENT */
    if (ev == oc_events[TLS_CLOSE_ALL_SESSIONS]) {
      size_t device = (size_t)data;
      close_all_tls_sessions_for_device(device);
      continue;
    }
  }

  OC_PROCESS_END();
}
#endif /* OC_SECURITY */
