/****************************************************************************
 *
 * Copyright (c) 2016-2019 Intel Corporation
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

#include "oc_collection.h"

#if defined(OC_COLLECTIONS) && defined(OC_SERVER)
#include "messaging/coap/observe.h"
#include "oc_api.h"
#include "oc_core_res.h"
#include "oc_discovery_internal.h"
#ifdef OC_COLLECTIONS_IF_CREATE
#include "api/oc_resource_factory.h"
#endif /* OC_COLLECTIONS_IF_CREATE */
#include "util/oc_memb.h"
#ifdef OC_SECURITY
#include "security/oc_acl_internal.h"
#endif /* OC_SECURITY */

OC_MEMB(oc_collections_s, oc_collection_t, OC_MAX_NUM_COLLECTIONS);
OC_LIST(oc_collections);
/* Allocator for links */
OC_MEMB(oc_links_s, oc_link_t, OC_MAX_APP_RESOURCES);
/* Allocator for oc_rtt_t */
OC_MEMB(rtt_s, oc_rt_t, 1);
/* Allocator for link parameters */
OC_MEMB(oc_params_s, oc_link_params_t, 1);
#ifdef OC_COLLECTIONS_IF_CREATE
/* Allocator for resource factories */
OC_MEMB(rts_s, oc_rt_factory_t, 1);
OC_LIST(rt_factories);
OC_LIST(params_list);
#endif /* OC_COLLECTIONS_IF_CREATE */

oc_collection_t *
oc_collection_alloc(void)
{
  oc_collection_t *collection =
    (oc_collection_t *)oc_memb_alloc(&oc_collections_s);
  if (collection) {
    OC_LIST_STRUCT_INIT(collection, supported_rts);
    OC_LIST_STRUCT_INIT(collection, mandatory_rts);
    OC_LIST_STRUCT_INIT(collection, links);
    return collection;
  }
  OC_WRN("insufficient memory to create new collection");
  return NULL;
}

void
oc_collection_free(oc_collection_t *collection)
{
  if (collection != NULL) {
    oc_list_remove(oc_collections, collection);
    oc_ri_free_resource_properties((oc_resource_t *)collection);

    oc_link_t *link;
    while ((link = (oc_link_t *)oc_list_pop(collection->links)) != NULL) {
      oc_delete_link(link);
    }

    if (oc_list_length(collection->supported_rts) > 0) {
      oc_rt_t *rtt = (oc_rt_t *)oc_list_pop(collection->supported_rts);
      while (rtt) {
        oc_free_string(&rtt->rt);
        oc_memb_free(&rtt_s, rtt);
        rtt = (oc_rt_t *)oc_list_pop(collection->supported_rts);
      }
    }

    if (oc_list_length(collection->mandatory_rts) > 0) {
      oc_rt_t *rtt = (oc_rt_t *)oc_list_pop(collection->mandatory_rts);
      while (rtt) {
        oc_free_string(&rtt->rt);
        oc_memb_free(&rtt_s, rtt);
        rtt = (oc_rt_t *)oc_list_pop(collection->mandatory_rts);
      }
    }

    oc_memb_free(&oc_collections_s, collection);
  }
}

oc_link_t *
oc_new_link(oc_resource_t *resource)
{
  if (resource) {
    oc_link_t *link = (oc_link_t *)oc_memb_alloc(&oc_links_s);
    if (link) {
      oc_new_string_array(&link->rel, 3);
      oc_string_array_add_item(link->rel, "hosts");
      link->resource = resource;
      link->interfaces = resource->interfaces;
      resource->num_links++;
      link->next = 0;
      link->ins = (int64_t)oc_random_value();
      OC_LIST_STRUCT_INIT(link, params);
      return link;
    }
    OC_WRN("insufficient memory to create new link");
  }
  return NULL;
}

void
oc_delete_link(oc_link_t *link)
{
  if (link) {
    oc_link_params_t *p = (oc_link_params_t *)oc_list_pop(link->params);
    while (p) {
      oc_free_string(&p->key);
      oc_free_string(&p->value);
      oc_memb_free(&oc_params_s, p);
      p = (oc_link_params_t *)oc_list_pop(link->params);
    }
    if (oc_ri_is_app_resource_valid(link->resource) ||
        oc_check_if_collection(link->resource)) {
      link->resource->num_links--;
    }
    oc_free_string_array(&(link->rel));
    oc_memb_free(&oc_links_s, link);
  }
}

static oc_event_callback_retval_t
batch_notify_collection(void *data)
{
  if (coap_notify_collection_batch((oc_collection_t *)data) != 0) {
    OC_WRN("failed to send batch notification to collection observers");
  }
  return OC_EVENT_DONE;
}

static oc_event_callback_retval_t
baseline_notify_collection(void *data)
{
  if (coap_notify_collection_baseline((oc_collection_t *)data) != 0) {
    OC_WRN("failed to send baseline notification to collection observers");
  }
  return OC_EVENT_DONE;
}

static oc_event_callback_retval_t
links_list_notify_collection(void *data)
{
  if (coap_notify_collection_links_list(data) != 0) {
    OC_WRN("failed to send linked list notification to collection observers");
  }
  oc_set_delayed_callback(data, baseline_notify_collection, 0);
  return OC_EVENT_DONE;
}

void
oc_collection_add_link(oc_resource_t *collection, oc_link_t *link)
{
  oc_collection_t *c = (oc_collection_t *)collection;

  if (link->resource != NULL && oc_string_len(link->resource->uri) > 0) {
    const char *link_uri = oc_string(link->resource->uri);
    const size_t link_uri_len = oc_string_len(link->resource->uri);
    // Find position to insert to keep the list sorted by primarily by href
    // length and secondarily by href value.
    // Keeping the links ordered like this enables use to use O(n) algorithm
    // to find a unique index for a new link.
    // Example of list sorted in this order:
    // ["/lights", "/switch", "/lights/1", "/lights/2", "/lights/10"]
    oc_link_t *next = oc_list_head(c->links), *prev = NULL;
    while (next != NULL) {
      if ((next->resource != NULL) &&
          (oc_string_len(next->resource->uri) > 0)) {
        // primary order by length
        if (link_uri_len < oc_string_len(next->resource->uri)) {
          break;
        }
        // secondary order by value
        if (link_uri_len == oc_string_len(next->resource->uri) &&
            strcmp(link_uri, oc_string(next->resource->uri)) < 0) {
          break;
        }
      }
      prev = next;
      next = next->next;
    }
    oc_list_insert(c->links, prev, link);
  } else {
    oc_list_push(c->links, link);
  }
  if (link->resource == collection) {
    oc_string_array_add_item(link->rel, "self");
  }
  oc_set_delayed_callback(collection, links_list_notify_collection, 0);
#if defined(OC_RES_BATCH_SUPPORT) && defined(OC_DISCOVERY_RESOURCE_OBSERVABLE)
  coap_notify_discovery_batch_observers(collection);
#endif /* OC_RES_BATCH_SUPPORT && OC_DISCOVERY_RESOURCE_OBSERVABLE */
}

void
oc_collection_remove_link(oc_resource_t *collection, oc_link_t *link)
{
  if (collection && link) {
    oc_collection_t *c = (oc_collection_t *)collection;
    oc_list_remove(c->links, link);
    oc_set_delayed_callback(collection, links_list_notify_collection, 0);
#if defined(OC_RES_BATCH_SUPPORT) && defined(OC_DISCOVERY_RESOURCE_OBSERVABLE)
    coap_notify_discovery_batch_observers(collection);
#endif /* OC_RES_BATCH_SUPPORT && OC_DISCOVERY_RESOURCE_OBSERVABLE */
  }
}

oc_link_t *
oc_collection_get_links(oc_resource_t *collection)
{
  if (collection)
    return (oc_link_t *)oc_list_head(((oc_collection_t *)collection)->links);
  return NULL;
}

void
oc_link_add_rel(oc_link_t *link, const char *rel)
{
  if (link) {
    oc_string_array_add_item(link->rel, rel);
  }
}

void
oc_link_add_link_param(oc_link_t *link, const char *key, const char *value)
{
  if (link) {
    oc_link_params_t *p = oc_memb_alloc(&oc_params_s);
    if (p) {
      oc_new_string(&p->key, key, strlen(key));
      oc_new_string(&p->value, value, strlen(value));
      oc_list_add(link->params, p);
    }
  }
}

void
oc_link_set_interfaces(oc_link_t *link, oc_interface_mask_t new_interfaces)
{
  link->interfaces = new_interfaces;
}

oc_collection_t *
oc_get_collection_by_uri(const char *uri_path, size_t uri_path_len,
                         size_t device)
{
  while (uri_path[0] == '/') {
    uri_path++;
    uri_path_len--;
  }
  oc_resource_t *collection = (oc_resource_t *)oc_list_head(oc_collections);
  while (collection != NULL) {
    if (oc_string_len(collection->uri) == (uri_path_len + 1) &&
        strncmp(oc_string(collection->uri) + 1, uri_path, uri_path_len) == 0 &&
        collection->device == device)
      break;
    collection = collection->next;
  }
  return (oc_collection_t *)collection;
}

oc_link_t *
oc_get_link_by_uri(oc_collection_t *collection, const char *uri_path,
                   int uri_path_len)
{
  oc_link_t *link = NULL;

  if (collection && uri_path && uri_path_len > 0) {
    while (uri_path[0] == '/') {
      uri_path++;
      uri_path_len--;
    }

    link = (oc_link_t *)oc_list_head(collection->links);
    while (link != NULL) {
      if (link->resource &&
          (int)oc_string_len(link->resource->uri) == (uri_path_len + 1) &&
          strncmp(oc_string(link->resource->uri) + 1, uri_path, uri_path_len) ==
            0) {
        break;
      }
      link = link->next;
    }
  }

  return link;
}

bool
oc_check_if_collection(oc_resource_t *resource)
{
  oc_resource_t *collection = (oc_resource_t *)oc_list_head(oc_collections);
  while (collection != NULL) {
    if (resource == collection)
      return true;
    collection = collection->next;
  }
  return false;
}

void
oc_collection_add(oc_collection_t *collection)
{
  oc_list_add(oc_collections, collection);
}

static oc_rt_t *
is_known_rt(oc_list_t list, const char *rt)
{
  oc_rt_t *rtt = (oc_rt_t *)oc_list_head(list);
  size_t rt_len = strlen(rt);
  while (rtt) {
    if (rt_len == oc_string_len(rtt->rt) &&
        memcmp(rt, oc_string(rtt->rt), rt_len) == 0) {
      return rtt;
    }
    rtt = rtt->next;
  }

  return NULL;
}

#ifdef OC_COLLECTIONS_IF_CREATE
static oc_rt_factory_t *
is_known_rtfactory(const char *rt)
{
  oc_rt_factory_t *rf = (oc_rt_factory_t *)oc_list_head(rt_factories);
  size_t rt_len = strlen(rt);
  while (rf) {
    if (rt_len == oc_string_len(rf->rt) &&
        memcmp(rt, oc_string(rf->rt), rt_len) == 0) {
      return rf;
    }
    rf = rf->next;
  }

  return NULL;
}

void
oc_collections_free_rt_factories(void)
{
  oc_fi_factory_free_all_created_resources();
  oc_rt_factory_t *rf = (oc_rt_factory_t *)oc_list_pop(rt_factories);
  while (rf) {
    oc_free_string(&rf->rt);
    oc_memb_free(&rts_s, rf);
    rf = (oc_rt_factory_t *)oc_list_pop(rt_factories);
  }
}

bool
oc_collections_add_rt_factory(const char *rt,
                              oc_resource_get_instance_t get_instance,
                              oc_resource_free_instance_t free_instance)
{
  if (is_known_rtfactory(rt)) {
    return true;
  }

  oc_rt_factory_t *rf = (oc_rt_factory_t *)oc_memb_alloc(&rts_s);
  if (!rf) {
    return false;
  }

  oc_new_string(&rf->rt, rt, strlen(rt));
  rf->get_instance = get_instance;
  rf->free_instance = free_instance;
  oc_list_add(rt_factories, rf);

  return true;
}

static void
add_link_param(const char *key, const char *value)
{
  oc_link_params_t *p = oc_memb_alloc(&oc_params_s);

  if (p) {
    oc_new_string(&p->key, key, strlen(key));
    oc_new_string(&p->value, value, strlen(value));
    oc_list_add(params_list, p);
  }
}

static bool
oc_handle_collection_create_request(oc_method_t method, oc_request_t *request)
{
  oc_collection_t *collection = (oc_collection_t *)request->resource;
  if (method == OC_PUT || method == OC_POST) {
    oc_rep_t *rep = request->request_payload;
    oc_string_array_t *rt = NULL;
    oc_interface_mask_t interfaces = 0;
    oc_resource_properties_t bm = 0;
    oc_rep_t *payload = NULL;
    while (rep) {
      switch (rep->type) {
      case OC_REP_STRING_ARRAY: {
        size_t i;
        if (oc_string_len(rep->name) == 2 &&
            strncmp(oc_string(rep->name), "rt", 2) == 0) {
          rt = &rep->value.array;
        } else {
          for (i = 0; i < oc_string_array_get_allocated_size(rep->value.array);
               i++) {
            interfaces |= oc_ri_get_interface_mask(
              oc_string_array_get_item(rep->value.array, i),
              oc_string_array_get_item_size(rep->value.array, i));
          }
        }
      } break;
      case OC_REP_OBJECT: {
        oc_rep_t *obj = rep->value.object;
        if (obj && oc_string_len(rep->name) == 1 &&
            *(oc_string(rep->name)) == 'p' && obj->type == OC_REP_INT &&
            oc_string_len(obj->name) == 2 &&
            memcmp(oc_string(obj->name), "bm", 2) == 0) {
          bm = obj->value.integer;
        } else if (oc_string_len(rep->name) == 3 &&
                   memcmp(oc_string(rep->name), "rep", 3) == 0) {
          payload = obj;
        }
      } break;
      case OC_REP_STRING:
        /* Other arbitrary link parameters to be stored in the link to the
         * created resource.
         */
        add_link_param(oc_string(rep->name), oc_string(rep->value.string));
        break;
      default:
        break;
      }
      rep = rep->next;
    }

    if (!rt || (interfaces == 0)) {
      goto error;
    }
#ifdef OC_SECURITY
    bm |= OC_SECURE;
#endif /* OC_SECURITY */
    const char *type = oc_string_array_get_item(*rt, 0);
    bool is_rt_found = (oc_list_length(collection->supported_rts) > 0 &&
                        is_known_rt(collection->supported_rts, type)) ||
                       (oc_list_length(collection->mandatory_rts) > 0 &&
                        is_known_rt(collection->mandatory_rts, type));
    if (!is_rt_found) {
      goto error;
    }
    oc_rt_factory_t *rf = is_known_rtfactory(type);
    if (!rf) {
      goto error;
    }

    oc_rt_created_t *new_res = oc_rt_factory_create_resource(
      collection, rt, bm, interfaces, rf, request->resource->device);
    if (!new_res) {
      goto error;
    }
    if (!payload || !new_res->resource->set_properties.cb.set_props(
                      new_res->resource, payload,
                      new_res->resource->set_properties.user_data)) {
      oc_rt_factory_free_created_resource(new_res, rf);
      goto error;
    }

    CborEncoder encoder;
    oc_link_t *link = oc_new_link(new_res->resource);
    oc_collection_add_link((oc_resource_t *)collection, link);

    oc_rep_start_root_object();
    memcpy(&encoder, oc_rep_get_encoder(), sizeof(CborEncoder));
    oc_rep_set_text_string(root, href, oc_string(new_res->resource->uri));
    oc_rep_set_string_array(root, rt, new_res->resource->types);
    oc_core_encode_interfaces_mask(oc_rep_object(root),
                                   new_res->resource->interfaces);
    oc_rep_set_object(root, p);
    oc_rep_set_uint(p, bm, (uint8_t)(bm & ~(OC_PERIODIC | OC_SECURE)));
    oc_rep_close_object(root, p);
    oc_rep_set_int(root, ins, link->ins);
    oc_rep_set_key(oc_rep_object(root), "rep");
    memcpy(oc_rep_get_encoder(), &root_map, sizeof(CborEncoder));
    oc_rep_start_root_object();
    new_res->resource->get_properties.cb.get_props(
      new_res->resource, OC_IF_BASELINE,
      new_res->resource->get_properties.user_data);
    oc_rep_end_root_object();
    memcpy(&root_map, oc_rep_get_encoder(), sizeof(CborEncoder));
    memcpy(oc_rep_get_encoder(), &encoder, sizeof(CborEncoder));

    oc_link_params_t *p = (oc_link_params_t *)oc_list_pop(params_list);
    while (p) {
      oc_rep_set_key(oc_rep_object(root), oc_string(p->key));
      oc_rep_set_value_text_string(root, oc_string(p->value));
      oc_list_add(link->params, p);
      p = (oc_link_params_t *)oc_list_pop(params_list);
    }

    oc_rep_end_root_object();
#ifdef OC_SECURITY
    oc_sec_acl_add_created_resource_ace(
      oc_string(new_res->resource->uri), request->origin,
      request->resource->device,
      false); /* TODO: handle creation of Collections */
#endif        /* OC_SECURITY */
  } else if (method == OC_GET) {
    oc_rep_start_root_object();
    oc_rep_end_root_object();
  } else {
    goto error;
  }

  return true;

error : {
  oc_link_params_t *p = (oc_link_params_t *)oc_list_pop(params_list);
  while (p) {
    oc_free_string(&p->key);
    oc_free_string(&p->value);
    oc_memb_free(&oc_params_s, p);
    p = (oc_link_params_t *)oc_list_pop(params_list);
  }
}
  return false;
}
#endif /* OC_COLLECTIONS_IF_CREATE */

bool
oc_collection_add_supported_rt(oc_resource_t *collection, const char *rt)
{
  oc_collection_t *col = (oc_collection_t *)collection;
  if (!is_known_rt(col->supported_rts, rt)) {
    oc_rt_t *rtt = (oc_rt_t *)oc_memb_alloc(&rtt_s);
    if (rtt) {
      oc_new_string(&rtt->rt, rt, strlen(rt));
      oc_list_add(col->supported_rts, rtt);
      return true;
    }
  }
  return false;
}

bool
oc_collection_add_mandatory_rt(oc_resource_t *collection, const char *rt)
{
  oc_collection_t *col = (oc_collection_t *)collection;
  if (!is_known_rt(col->mandatory_rts, rt)) {
    oc_rt_t *rtt = (oc_rt_t *)oc_memb_alloc(&rtt_s);
    if (rtt) {
      oc_new_string(&rtt->rt, rt, strlen(rt));
      oc_list_add(col->mandatory_rts, rtt);
      return true;
    }
  }
  return false;
}

oc_collection_t *
oc_get_next_collection_with_link(oc_resource_t *resource,
                                 oc_collection_t *start)
{
  oc_collection_t *collection = start;

  if (!collection) {
    collection = oc_collection_get_all();
  } else {
    collection = (oc_collection_t *)collection->res.next;
  }

  while (collection && collection->res.device == resource->device) {
    oc_link_t *link = (oc_link_t *)oc_list_head(collection->links);
    while (link) {
      if (link->resource == resource) {
        return collection;
      }
      link = link->next;
    }
    collection = (oc_collection_t *)collection->res.next;
  }

  return collection;
}

typedef struct oc_handle_collection_request_result_t
{
  bool ok;
  int ecode;
  int pcode;
} oc_handle_collection_request_result_t;

OC_NO_DISCARD_RETURN
static oc_handle_collection_request_result_t
oc_handle_collection_baseline_request(oc_method_t method, oc_request_t *request)
{
  int ecode = oc_status_code(OC_STATUS_OK);
  int pcode = oc_status_code(OC_STATUS_BAD_REQUEST);
  oc_collection_t *collection = (oc_collection_t *)request->resource;
  if (method == OC_GET) {
    oc_link_t *link = (oc_link_t *)oc_list_head(collection->links);
    oc_rep_start_root_object();
    oc_process_baseline_interface(request->resource);
    /* rts */
    if (oc_list_length(collection->supported_rts) > 0) {
      oc_rep_open_array(root, rts);
      oc_rt_t *rtt = (oc_rt_t *)oc_list_head(collection->supported_rts);
      while (rtt) {
        oc_rep_add_text_string(rts, oc_string(rtt->rt));
        rtt = rtt->next;
      }
      oc_rep_close_array(root, rts);
    }
    /* rts-m */
    if (oc_list_length(collection->mandatory_rts) > 0) {
      const char *rtsm_key = "rts-m";
      oc_rep_set_key(oc_rep_object(root), rtsm_key);
      oc_rep_start_array(oc_rep_object(root), rtsm);
      oc_rt_t *rtt = (oc_rt_t *)oc_list_head(collection->mandatory_rts);
      while (rtt) {
        oc_rep_add_text_string(rtsm, oc_string(rtt->rt));
        rtt = rtt->next;
      }
      oc_rep_end_array(oc_rep_object(root), rtsm);
    }
    oc_rep_set_array(root, links);
    while (link != NULL) {
      if (oc_filter_resource_by_rt(link->resource, request)) {
        oc_rep_object_array_start_item(links);
        oc_rep_set_text_string(links, href, oc_string(link->resource->uri));
        oc_rep_set_string_array(links, rt, link->resource->types);
        oc_core_encode_interfaces_mask(oc_rep_object(links), link->interfaces);
        oc_rep_set_string_array(links, rel, link->rel);
        oc_rep_set_int(links, ins, link->ins);
        oc_link_params_t *p = (oc_link_params_t *)oc_list_head(link->params);
        while (p) {
          oc_rep_set_key(oc_rep_object(links), oc_string(p->key));
          oc_rep_set_value_text_string(links, oc_string(p->value));
          p = p->next;
        }
        oc_rep_set_object(links, p);
        oc_rep_set_uint(
          p, bm,
          (uint8_t)(link->resource->properties & ~(OC_PERIODIC | OC_SECURE)));
        oc_rep_close_object(links, p);

        // tag-pos-desc
        if (link->resource->tag_pos_desc > 0) {
          const char *desc =
            oc_enum_pos_desc_to_str(link->resource->tag_pos_desc);
          if (desc) {
            // clang-format off
            oc_rep_set_text_string(links, tag-pos-desc, desc);
            // clang-format on
          }
        }

        // tag-func-desc
        if (link->resource->tag_func_desc > 0) {
          const char *func = oc_enum_to_str(link->resource->tag_func_desc);
          if (func) {
            // clang-format off
            oc_rep_set_text_string(links, tag-func-desc, func);
            // clang-format on
          }
        }

        // tag-pos-rel
        double *pos = link->resource->tag_pos_rel;
        if (pos[0] != 0 || pos[1] != 0 || pos[2] != 0) {
          oc_rep_set_key(oc_rep_object(links), "tag-pos-rel");
          oc_rep_start_array(oc_rep_object(links), tag_pos_rel);
          oc_rep_add_double(tag_pos_rel, pos[0]);
          oc_rep_add_double(tag_pos_rel, pos[1]);
          oc_rep_add_double(tag_pos_rel, pos[2]);
          oc_rep_end_array(oc_rep_object(links), tag_pos_rel);
        }

        // eps
        oc_rep_set_array(links, eps);
        oc_endpoint_t *eps =
          oc_connectivity_get_endpoints(link->resource->device);
        for (; eps != NULL; eps = eps->next) {
          if (oc_filter_out_ep_for_resource(eps, link->resource,
                                            request->origin,
                                            link->resource->device, false)) {
            continue;
          }
          oc_rep_object_array_start_item(eps);
          oc_string_t ep;
          if (oc_endpoint_to_string(eps, &ep) == 0) {
            oc_rep_set_text_string(eps, ep, oc_string(ep));
            oc_free_string(&ep);
          }
          oc_rep_object_array_end_item(eps);
        }
        oc_rep_close_array(links, eps);

        oc_rep_object_array_end_item(links);
      }
      link = link->next;
    }
    oc_rep_close_array(root, links);
    if (collection->res.get_properties.cb.get_props) {
      collection->res.get_properties.cb.get_props(
        (oc_resource_t *)collection, OC_IF_BASELINE,
        collection->res.get_properties.user_data);
    }
    oc_rep_end_root_object();
    pcode = ecode = oc_status_code(OC_STATUS_OK);
  } else if (method == OC_PUT || method == OC_POST) {
    if (collection->res.set_properties.cb.set_props) {
      collection->res.set_properties.cb.set_props(
        (oc_resource_t *)collection, request->request_payload,
        collection->res.set_properties.user_data);
    }
  }

  oc_handle_collection_request_result_t result = {
    .ok = true,
    .ecode = ecode,
    .pcode = pcode,
  };
  return result;
}

static void
oc_handle_collection_linked_list_request(oc_request_t *request)
{
  const oc_collection_t *collection = (oc_collection_t *)request->resource;
  oc_link_t *link = (oc_link_t *)oc_list_head(collection->links);
  oc_rep_start_links_array();
  while (link != NULL) {
    if (oc_filter_resource_by_rt(link->resource, request)) {
      oc_rep_object_array_start_item(links);
      oc_rep_set_text_string(links, href, oc_string(link->resource->uri));
      oc_rep_set_string_array(links, rt, link->resource->types);
      oc_core_encode_interfaces_mask(oc_rep_object(links), link->interfaces);
      oc_rep_set_string_array(links, rel, link->rel);
      oc_rep_set_int(links, ins, link->ins);
      oc_link_params_t *p = (oc_link_params_t *)oc_list_head(link->params);
      while (p) {
        oc_rep_set_key(oc_rep_object(links), oc_string(p->key));
        oc_rep_set_value_text_string(links, oc_string(p->value));
        p = p->next;
      }
      oc_rep_set_object(links, p);
      oc_rep_set_uint(
        p, bm,
        (uint8_t)(link->resource->properties & ~(OC_PERIODIC | OC_SECURE)));
      oc_rep_close_object(links, p);

      // tag-pos-desc
      if (link->resource->tag_pos_desc > 0) {
        const char *desc =
          oc_enum_pos_desc_to_str(link->resource->tag_pos_desc);
        if (desc) {
          // clang-format off
          oc_rep_set_text_string(links, tag-pos-desc, desc);
          // clang-format on
        }
      }

      // tag-func-desc
      if (link->resource->tag_func_desc > 0) {
        const char *func = oc_enum_to_str(link->resource->tag_func_desc);
        if (func) {
          // clang-format off
          oc_rep_set_text_string(links, tag-func-desc, func);
          // clang-format on
        }
      }

      // tag-pos-rel
      double *pos = link->resource->tag_pos_rel;
      if (pos[0] != 0 || pos[1] != 0 || pos[2] != 0) {
        oc_rep_set_key(oc_rep_object(links), "tag-pos-rel");
        oc_rep_start_array(oc_rep_object(links), tag_pos_rel);
        oc_rep_add_double(tag_pos_rel, pos[0]);
        oc_rep_add_double(tag_pos_rel, pos[1]);
        oc_rep_add_double(tag_pos_rel, pos[2]);
        oc_rep_end_array(oc_rep_object(links), tag_pos_rel);
      }

      // eps
      oc_rep_set_array(links, eps);
      oc_endpoint_t *eps =
        oc_connectivity_get_endpoints(link->resource->device);
      for (; eps != NULL; eps = eps->next) {
        if (oc_filter_out_ep_for_resource(eps, link->resource, request->origin,
                                          link->resource->device, false)) {
          continue;
        }
        oc_rep_object_array_start_item(eps);
        oc_string_t ep;
        if (oc_endpoint_to_string(eps, &ep) == 0) {
          oc_rep_set_text_string(eps, ep, oc_string(ep));
          oc_free_string(&ep);
        }
        oc_rep_object_array_end_item(eps);
      }
      oc_rep_close_array(links, eps);

      oc_rep_object_array_end_item(links);
    }
    link = link->next;
  }
  oc_rep_end_links_array();
}

OC_NO_DISCARD_RETURN
static oc_handle_collection_request_result_t
oc_handle_collection_batch_request(oc_method_t method, oc_request_t *request,
                                   const oc_resource_t *notify_resource)
{
  int ecode = oc_status_code(OC_STATUS_OK);
  int pcode = oc_status_code(OC_STATUS_BAD_REQUEST);
  CborEncoder encoder, prev_link;
  oc_request_t rest_request = { 0 };
  oc_response_t response = { 0 };
  oc_response_buffer_t response_buffer;
  bool method_not_found = false, get_delete = false;
  oc_rep_t *rep = request->request_payload;
  oc_string_t *href = NULL;
  const oc_collection_t *collection = (oc_collection_t *)request->resource;
  oc_link_t *link = NULL;

  response.response_buffer = &response_buffer;
  rest_request.response = &response;
  rest_request.origin = request->origin;

  oc_rep_start_links_array();
  memcpy(&encoder, oc_rep_get_encoder(), sizeof(CborEncoder));
  if (method == OC_GET || method == OC_DELETE) {
    get_delete = true;
  }

  if (get_delete) {
    goto process_request;
  }

  while (rep != NULL) {
    switch (rep->type) {
    case OC_REP_OBJECT: {
      href = NULL;
      oc_rep_t *pay = rep->value.object;
      while (pay != NULL) {
        switch (pay->type) {
        case OC_REP_STRING:
          href = &pay->value.string;
          break;
        case OC_REP_OBJECT:
          rest_request.request_payload = pay->value.object;
          break;
        default:
          break;
        }
        pay = pay->next;
      }
      if (!href || (href && oc_string_len(*href) == 0)) {
        ecode = oc_status_code(OC_STATUS_BAD_REQUEST);
        goto processed_request;
      }
    process_request:
      link = (oc_link_t *)oc_list_head(collection->links);
      while (link != NULL) {
        if (link->resource &&
            (!notify_resource == !(link->resource == notify_resource))) {
          if (oc_filter_resource_by_rt(link->resource, request)) {
            if (!get_delete && href && oc_string_len(*href) > 0 &&
                (oc_string_len(*href) != oc_string_len(link->resource->uri) ||
                 memcmp(oc_string(*href), oc_string(link->resource->uri),
                        oc_string_len(*href)) != 0)) {
              goto next;
            }
            memcpy(&prev_link, &links_array, sizeof(CborEncoder));
            oc_rep_object_array_start_item(links);

            rest_request.query = 0;
            rest_request.query_len = 0;

            oc_rep_set_text_string(links, href, oc_string(link->resource->uri));
            oc_rep_set_key(oc_rep_object(links), "rep");
            memcpy(oc_rep_get_encoder(), &links_map, sizeof(CborEncoder));

            int size_before = oc_rep_get_encoded_payload_size();
            rest_request.resource = link->resource;
            response_buffer.code = 0;
            response_buffer.response_length = 0;
            method_not_found = false;
#ifdef OC_SECURITY
            if (request && request->origin &&
                !oc_sec_check_acl(method, link->resource, request->origin)) {
              response_buffer.code = oc_status_code(OC_STATUS_FORBIDDEN);
            } else
#endif /* OC_SECURITY */
            {
              if ((link->resource != (oc_resource_t *)collection) &&
                  oc_check_if_collection(link->resource)) {
                request->resource = link->resource;
                if (!oc_handle_collection_request(
                      method, request, link->resource->default_interface,
                      NULL)) {
                  oc_handle_collection_request_result_t res = {
                    .ok = false,
                    .ecode = oc_status_code(OC_STATUS_OK),
                    .pcode = oc_status_code(OC_STATUS_BAD_REQUEST),
                  };
                  return res;
                }
                request->resource = (oc_resource_t *)collection;
              } else {
                oc_interface_mask_t req_iface =
                  link->resource->default_interface;
                if (link->resource == (oc_resource_t *)collection) {
                  req_iface = OC_IF_BASELINE;
                }
                switch (method) {
                case OC_GET:
                  if (link->resource->get_handler.cb)
                    link->resource->get_handler.cb(
                      &rest_request, req_iface,
                      link->resource->get_handler.user_data);
                  else
                    method_not_found = true;
                  break;
                case OC_PUT:
                  if (link->resource->put_handler.cb)
                    link->resource->put_handler.cb(
                      &rest_request, req_iface,
                      link->resource->put_handler.user_data);
                  else
                    method_not_found = true;
                  break;
                case OC_POST:
                  if (link->resource->post_handler.cb)
                    link->resource->post_handler.cb(
                      &rest_request, req_iface,
                      link->resource->post_handler.user_data);
                  else
                    method_not_found = true;
                  break;
                case OC_DELETE:
                  if (link->resource->delete_handler.cb)
                    link->resource->delete_handler.cb(
                      &rest_request, req_iface,
                      link->resource->delete_handler.user_data);
                  else
                    method_not_found = true;
                  break;
                default:
                  break;
                }
              }
            }
            if (method_not_found) {
              ecode = oc_status_code(OC_STATUS_METHOD_NOT_ALLOWED);
              memcpy(&links_array, &prev_link, sizeof(CborEncoder));
              goto next;
            } else {
              if ((method == OC_PUT || method == OC_POST) &&
                  response_buffer.code <
                    oc_status_code(OC_STATUS_BAD_REQUEST)) {
              }
              if (response_buffer.code <
                  oc_status_code(OC_STATUS_BAD_REQUEST)) {
                pcode = response_buffer.code;
              } else {
                ecode = response_buffer.code;
              }
              int size_after = oc_rep_get_encoded_payload_size();
              if (size_before == size_after) {
                oc_rep_start_root_object();
                oc_rep_end_root_object();
              }
            }

            memcpy(&links_map, oc_rep_get_encoder(), sizeof(CborEncoder));
            oc_rep_object_array_end_item(links);
          }
        }
      next:
        link = link->next;
      }
      if (get_delete) {
        goto processed_request;
      }
    } break;
    default:
      ecode = oc_status_code(OC_STATUS_BAD_REQUEST);
      goto processed_request;
    }
    rep = rep->next;
  }
processed_request:
  memcpy(oc_rep_get_encoder(), &encoder, sizeof(CborEncoder));
  oc_rep_end_links_array();

  oc_handle_collection_request_result_t result = {
    .ok = true,
    .ecode = ecode,
    .pcode = pcode,
  };
  return result;
}

bool
oc_handle_collection_request(oc_method_t method, oc_request_t *request,
                             oc_interface_mask_t iface_mask,
                             const oc_resource_t *notify_resource)
{
  int ecode = oc_status_code(OC_STATUS_OK);
  int pcode = oc_status_code(OC_STATUS_BAD_REQUEST);
  switch (iface_mask) {
#ifdef OC_COLLECTIONS_IF_CREATE
  case OC_IF_CREATE:
    if (oc_handle_collection_create_request(method, request)) {
      pcode = ecode = oc_status_code(OC_STATUS_OK);
    } else {
      pcode = ecode = oc_status_code(OC_STATUS_BAD_REQUEST);
    }
    break;
#endif /* OC_COLLECTIONS_IF_CREATE */
  case OC_IF_BASELINE: {
    oc_handle_collection_request_result_t res =
      oc_handle_collection_baseline_request(method, request);
    if (!res.ok) {
      return false;
    }
    pcode = res.pcode;
    ecode = res.ecode;
    break;
  }
  case OC_IF_LL:
    oc_handle_collection_linked_list_request(request);
    pcode = ecode = oc_status_code(OC_STATUS_OK);
    break;
  case OC_IF_B: {
    oc_handle_collection_request_result_t res =
      oc_handle_collection_batch_request(method, request, notify_resource);
    if (!res.ok) {
      return false;
    }
    pcode = res.pcode;
    ecode = res.ecode;
    break;
  }
  default:
    break;
  }

  oc_collection_t *collection = (oc_collection_t *)request->resource;
  int size = oc_rep_get_encoded_payload_size();
  if (size == -1) {
    OC_ERR("failed to handle collection(%s) request: payload too large",
           oc_string_len(collection->res.uri) > 0
             ? oc_string(collection->res.uri)
             : "");
    return false;
  }

  int code = oc_status_code(OC_STATUS_BAD_REQUEST);
  if (ecode < oc_status_code(OC_STATUS_BAD_REQUEST) &&
      pcode < oc_status_code(OC_STATUS_BAD_REQUEST)) {
    switch (method) {
    case OC_GET:
      code = oc_status_code(OC_STATUS_OK);
      break;
    case OC_POST:
    case OC_PUT:
      if (iface_mask == OC_IF_CREATE) {
        code = oc_status_code(OC_STATUS_CREATED);
      } else {
        code = oc_status_code(OC_STATUS_CHANGED);
      }
      break;
    case OC_DELETE:
      code = oc_status_code(OC_STATUS_DELETED);
      break;
    default:
      break;
    }
  }
  request->response->response_buffer->content_format = APPLICATION_VND_OCF_CBOR;
  request->response->response_buffer->response_length = size;
  request->response->response_buffer->code = code;

  if ((method == OC_PUT || method == OC_POST) &&
      code < oc_status_code(OC_STATUS_BAD_REQUEST)) {
    if (iface_mask == OC_IF_CREATE) {
      coap_notify_collection_observers(
        collection, request->response->response_buffer, iface_mask);
    } else if (iface_mask == OC_IF_B) {
      oc_set_delayed_callback(collection, batch_notify_collection, 0);
    }
  }

  return true;
}

oc_collection_t *
oc_collection_get_all(void)
{
  return (oc_collection_t *)oc_list_head(oc_collections);
}

#endif /* OC_COLLECTIONS && OC_SERVER */
