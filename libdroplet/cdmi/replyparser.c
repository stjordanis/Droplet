/*
 * Copyright (C) 2010 SCALITY SA. All rights reserved.
 * http://www.scality.com
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY SCALITY SA ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SCALITY SA OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * official policies, either expressed or implied, of SCALITY SA.
 *
 * https://github.com/scality/Droplet
 */
#include "dropletp.h"
#include <json/json.h>
#include <droplet/cdmi/reqbuilder.h>

//#define DPRINTF(fmt,...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define DPRINTF(fmt,...)

dpl_status_t
dpl_cdmi_get_metadata_from_headers(const dpl_dict_t *headers,
                                   dpl_dict_t **metadatap,
                                   dpl_sysmd_t *sysmdp)
{
  dpl_dict_t *metadata = NULL;
  dpl_status_t ret, ret2;

  if (NULL == metadatap)
    {
      ret = DPL_SUCCESS;
      goto end;
    }

  metadata = dpl_dict_new(13);
  if (NULL == metadata)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  //XXX check if container
  ret2 = dpl_dict_filter_prefix(metadata, headers, "X-Object-Meta-");
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  *metadatap = metadata;
  metadata = NULL;

  ret = DPL_SUCCESS;
  
 end:

  if (NULL != metadata)
    dpl_dict_free(metadata);

  return ret;
}

/**/

dpl_status_t
dpl_cdmi_parse_list_bucket(dpl_ctx_t *ctx,
                           const char *buf,
                           int len,
                           const char *prefix,
                           dpl_vec_t *objects,
                           dpl_vec_t *common_prefixes)
{
  int ret, ret2;
  json_tokener *tok = NULL;
  json_object *obj = NULL;
  json_object *children = NULL;
  int n_children, i;
  dpl_common_prefix_t *common_prefix = NULL;
  dpl_object_t *object = NULL;

  //  write(1, buf, len);

  tok = json_tokener_new();
  if (NULL == tok)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  obj = json_tokener_parse_ex(tok, buf, len);
  if (NULL == obj)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  children = json_object_object_get(obj, "children");
  if (NULL == children)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  if (json_type_array != json_object_get_type(children))
    {
      ret = DPL_FAILURE;
      goto end;
    }

  n_children = json_object_array_length(children);

  for (i = 0;i < n_children;i++)
    {
      json_object *child = json_object_array_get_idx(children, i);
      char name[1024];
      int name_len;
      
      if (json_type_string != json_object_get_type(child))
        {
          ret = DPL_FAILURE;
          goto end;
        }

      snprintf(name, sizeof (name), "%s%s", NULL != prefix ? prefix : "", json_object_get_string(child));
      name_len = strlen(name);

      if (name_len > 0 && name[name_len-1] == '/')
        {
          //this is a directory
          common_prefix = malloc(sizeof (*common_prefix));
          if (NULL == common_prefix)
            {
              ret = DPL_ENOMEM;
              goto end;
            }
          memset(common_prefix, 0, sizeof (*common_prefix));
          common_prefix->prefix = strdup(name);
          if (NULL == common_prefix->prefix)
            {
              ret = DPL_ENOMEM;
              goto end;
            }

          ret2 = dpl_vec_add(common_prefixes, common_prefix);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }

          common_prefix = NULL;
        }
      else
        {
          object = malloc(sizeof (*object));
          if (NULL == object)
            {
              ret = DPL_ENOMEM;
              goto end;
            }
          memset(object, 0, sizeof (*object));
          object->path = strdup(name);
          if (NULL == object->path)
            {
              ret = DPL_ENOMEM;
              goto end;
            }

          ret2 = dpl_vec_add(objects, object);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }

          object = NULL;
        }
    }

  ret = DPL_SUCCESS;

 end:

  if (NULL != common_prefix)
    dpl_common_prefix_free(common_prefix);

  if (NULL != object)
    dpl_object_free(object);

  if (NULL != obj)
    json_object_put(obj);

  if (NULL != tok)
    json_tokener_free(tok);

  return ret;
}

/**/

static dpl_status_t
convert_obj_to_value(dpl_ctx_t *ctx,
                     struct json_object *obj,
                     int level,
                     dpl_value_t **valp)
{
  int ret, ret2;
  dpl_value_t *val = NULL;
  char *key; 
  struct lh_entry *entry;
  json_object *child;
  dpl_dict_t *subdict = NULL;
  dpl_vec_t *vector = NULL;

  DPRINTF("convert_obj_to_value level=%d type=%d\n", level, json_object_get_type(obj));

  val = malloc(sizeof (*val));
  if (NULL == val)
    {
      ret = DPL_ENOMEM;
      goto end;
    }
  memset(val, 0, sizeof (*val));

  switch (json_object_get_type(obj))
    {
    case json_type_null:
      return DPL_ENOTSUPP;
    case json_type_array:
      {
        int n_items = json_object_array_length(obj);
        int i;

        vector = dpl_vec_new(2, 2);
        if (NULL == vector)
          {
            ret = DPL_ENOMEM;
            goto end;
          }

        for (i = 0;i < n_items;i++)
          {
            child = json_object_array_get_idx(obj, i);
            dpl_value_t *subval;

            ret2 = convert_obj_to_value(ctx, child, level+1, &subval);
            if (DPL_SUCCESS != ret2)
              {
                ret = ret2;
                goto end;
              }
            
            ret2 = dpl_vec_add_value(vector, subval);

            dpl_value_free(subval);
            
            if (DPL_SUCCESS != ret2)
              {
                ret = ret2;
                goto end;
              }
          }

        val->type = DPL_VALUE_VECTOR;
        val->vector = vector;
        vector = NULL;
        break ;
      }
    case json_type_object:
      {
        subdict = dpl_dict_new(13);
        if (NULL == subdict)
          {
            ret = DPL_ENOMEM;
            goto end;
          }
        
        for (entry = json_object_get_object(obj)->head; (entry ? (key = (char*)entry->k, child = (struct json_object*)entry->v, entry) : 0); entry = entry->next)
          {
            dpl_value_t *subval;

            DPRINTF("key='%s'\n", key);

            ret2 = convert_obj_to_value(ctx, child, level+1, &subval);
            if (DPL_SUCCESS != ret2)
              {
                ret = ret2;
                goto end;
              }
            
            ret2 = dpl_dict_add_value(subdict, key, subval, 0);

            dpl_value_free(subval);

            if (DPL_SUCCESS != ret2)
              {
                ret = ret2;
                goto end;
              }
          }

        val->type = DPL_VALUE_SUBDICT;
        val->subdict = subdict;
        subdict = NULL;
        break ;
      }
    case json_type_boolean:
    case json_type_double:
    case json_type_int:
    case json_type_string:
      {
        pthread_mutex_lock(&ctx->lock); //lock for objects other than string
        val->string = strdup((char *) json_object_get_string(obj));
        pthread_mutex_unlock(&ctx->lock);

        if (NULL == val->string)
          {
            ret = DPL_ENOMEM;
            goto end;
          }
        val->type = DPL_VALUE_STRING;
        break ;
      }
    }

  if (NULL != valp)
    {
      *valp = val;
      val = NULL;
    }

  ret = DPL_SUCCESS;
  
 end:

  if (NULL != vector)
    dpl_vec_free(vector);

  if (NULL != subdict)
    dpl_dict_free(subdict);

  if (NULL != val)
    dpl_value_free(val);

  DPRINTF("level=%d ret=%d\n", level, ret);

  return ret;
}

dpl_status_t
dpl_cdmi_parse_metadata(dpl_ctx_t *ctx,
                        const char *buf,
                        int len,
                        dpl_dict_t **metadatap)
{
  int ret, ret2;
  json_tokener *tok = NULL;
  json_object *obj = NULL;
  dpl_value_t *val = NULL;

  //  write(1, buf, len);

  tok = json_tokener_new();
  if (NULL == tok)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  obj = json_tokener_parse_ex(tok, buf, len);
  if (NULL == obj)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  ret2 = convert_obj_to_value(ctx, obj, 0, &val);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }
  
  if (DPL_VALUE_SUBDICT != val->type)
    {
      ret = DPL_EINVAL;
      goto end;
    }

  if (NULL != metadatap)
    {
      //pointer stealing
      *metadatap = (dpl_dict_t *) val->subdict;
      val->subdict = NULL;
    }

  ret = DPL_SUCCESS;

 end:

  if (NULL != val)
    dpl_value_free(val);

  if (NULL != obj)
    json_object_put(obj);

  if (NULL != tok)
    json_tokener_free(tok);

  return ret;
}

dpl_status_t
dpl_cdmi_get_metadata_from_json_metadata(dpl_dict_t *json_metadata,
                                         dpl_dict_t **metadatap)
{
  dpl_status_t ret, ret2;
  dpl_dict_var_t *var;
  dpl_dict_t *metadata = NULL;

  //find the metadata ARRAY
  ret2 = dpl_dict_get_lowered(json_metadata, "metadata", &var);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  if (DPL_VALUE_SUBDICT != var->val->type)
    {
      ret = DPL_EINVAL;
      goto end;
    }

  metadata = dpl_dict_dup(var->val->subdict);
  if (NULL == metadata)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  if (NULL != metadatap)
    {
      *metadatap = metadata;
      metadata = NULL;
    }

  ret = DPL_SUCCESS;
  
 end:

  if (NULL != metadata)
    dpl_dict_free(metadata);

  return ret;
}

dpl_ftype_t
dpl_cdmi_content_type_to_ftype(const char *str)
{
  if (!strcmp(DPL_CDMI_CONTENT_TYPE_OBJECT, str))
    return DPL_FTYPE_REG;
  else if (!strcmp(DPL_CDMI_CONTENT_TYPE_CONTAINER, str))
    return DPL_FTYPE_DIR;
  else if (!strcmp(DPL_CDMI_CONTENT_TYPE_CAPABILITY, str))
    return DPL_FTYPE_CAP;
  else if (!strcmp(DPL_CDMI_CONTENT_TYPE_DOMAIN, str))
    return DPL_FTYPE_DOM;

  return DPL_FTYPE_UNDEF;
}

dpl_status_t
dpl_cdmi_get_sysmd_from_json_metadata(dpl_dict_t *json_metadata,
                                      dpl_sysmd_t *sysmd)
{
  dpl_status_t ret, ret2;
  dpl_dict_var_t *var, *var2;

  if (NULL == sysmd)
    {
      ret = DPL_SUCCESS;
      goto end;
    }

  sysmd->mask = 0;

  var = dpl_dict_get(json_metadata, "objectID");
  if (NULL != var)
    {
      if (DPL_VALUE_STRING != var->val->type)
        {
          ret = DPL_EINVAL;
          goto end;
        }
      
      sysmd->mask |= DPL_SYSMD_MASK_ID;
      strncpy(sysmd->id, var->val->string, DPL_SYSMD_ID_SIZE);
      sysmd->id[DPL_SYSMD_ID_SIZE] = 0;
    }

  var = dpl_dict_get(json_metadata, "parentID");
  if (NULL != var)
    {
      if (DPL_VALUE_STRING != var->val->type)
        {
          ret = DPL_EINVAL;
          goto end;
        }

      sysmd->mask |= DPL_SYSMD_MASK_PARENT_ID;
      strncpy(sysmd->parent_id, var->val->string, DPL_SYSMD_ID_SIZE);
      sysmd->parent_id[DPL_SYSMD_ID_SIZE] = 0;
    }

  var = dpl_dict_get(json_metadata, "objectType");
  if (NULL != var)
    {
      if (DPL_VALUE_STRING != var->val->type)
        {
          ret = DPL_EINVAL;
          goto end;
        }
      
      sysmd->mask |= DPL_SYSMD_MASK_FTYPE;
      sysmd->ftype = dpl_cdmi_content_type_to_ftype(var->val->string);
    }

  //those sysmds are stored in metadata

  ret2 = dpl_dict_get_lowered(json_metadata, "metadata", &var);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }
  
  if (DPL_VALUE_SUBDICT != var->val->type)
    {
      ret = DPL_EINVAL;
      goto end;
    }
  
  var2 = dpl_dict_get(var->val->subdict, "cdmi_mtime");
  if (NULL != var2)
    {
      if (DPL_VALUE_STRING != var2->val->type)
        {
          ret = DPL_EINVAL;
          goto end;
        }
      
      sysmd->mask |= DPL_SYSMD_MASK_MTIME;
      sysmd->mtime = dpl_iso8601totime(var2->val->string);
    }

  var2 = dpl_dict_get(var->val->subdict, "cdmi_atime");
  if (NULL != var2)
    {
      if (DPL_VALUE_STRING != var2->val->type)
        {
          ret = DPL_EINVAL;
          goto end;
        }
      
      sysmd->mask |= DPL_SYSMD_MASK_ATIME;
      sysmd->atime = dpl_iso8601totime(var2->val->string);
    }

  var2 = dpl_dict_get(var->val->subdict, "cdmi_size");
  if (NULL != var2)
    {
      if (DPL_VALUE_STRING != var2->val->type)
        {
          ret = DPL_EINVAL;
          goto end;
        }
      
      sysmd->mask |= DPL_SYSMD_MASK_SIZE;
      sysmd->size = strtoull(var2->val->string, NULL, 0);
    }

  ret = DPL_SUCCESS;
  
 end:

  return ret;
}
