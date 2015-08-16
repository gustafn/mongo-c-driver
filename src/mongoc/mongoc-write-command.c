/*
 * Copyright 2014 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "mongoc-client-private.h"
#include "mongoc-error.h"
#include "mongoc-trace.h"
#include "mongoc-write-command-private.h"
#include "mongoc-write-concern-private.h"


/*
 * TODO:
 *
 *    - Remove error parameter to ops, favor result->error.
 */

#define WRITE_COMMAND_WIRE_VERSION 2

#define WRITE_CONCERN_DOC(wc) \
   (wc && _mongoc_write_concern_needs_gle ((wc))) ? \
   (_mongoc_write_concern_get_bson((mongoc_write_concern_t*)(wc))) : \
   (&gEmptyWriteConcern)


typedef void (*mongoc_write_op_t) (mongoc_write_command_t       *command,
                                   mongoc_client_t              *client,
                                   uint32_t                      hint,
                                   const char                   *database,
                                   const char                   *collection,
                                   const mongoc_write_concern_t *write_concern,
                                   uint32_t                      offset,
                                   mongoc_write_result_t        *result,
                                   bson_error_t                 *error);


static bson_t gEmptyWriteConcern = BSON_INITIALIZER;

/* indexed by MONGOC_WRITE_COMMAND_DELETE, INSERT, UPDATE */
static const char *gCommandNames[] = { "delete", "insert", "update"};
static const char *gCommandFields[] = { "deletes", "documents", "updates"};

static int32_t
_mongoc_write_result_merge_arrays (uint32_t               offset,
                                   mongoc_write_result_t *result,
                                   bson_t                *dest,
                                   bson_iter_t           *iter);

void
_mongoc_write_command_insert_append (mongoc_write_command_t *command,
                                     const bson_t * const   *documents,
                                     uint32_t                n_documents)
{
   const char *key;
   bson_iter_t iter;
   bson_oid_t oid;
   uint32_t i;
   bson_t tmp;
   char keydata [16];

   ENTRY;

   BSON_ASSERT (command);
   BSON_ASSERT (command->type == MONGOC_WRITE_COMMAND_INSERT);
   BSON_ASSERT (!n_documents || documents);

   for (i = 0; i < n_documents; i++) {
      BSON_ASSERT (documents [i]);
      BSON_ASSERT (documents [i]->len >= 5);

      key = NULL;
      bson_uint32_to_string (i, &key, keydata, sizeof keydata);
      BSON_ASSERT (key);

      /*
       * If the document does not contain an "_id" field, we need to generate
       * a new oid for "_id".
       */
      if (!bson_iter_init_find (&iter, documents [i], "_id")) {
         bson_init (&tmp);
         bson_oid_init (&oid, NULL);
         BSON_APPEND_OID (&tmp, "_id", &oid);
         bson_concat (&tmp, documents [i]);
         BSON_APPEND_DOCUMENT (command->documents, key, &tmp);
         bson_destroy (&tmp);
      } else {
         BSON_APPEND_DOCUMENT (command->documents, key, documents [i]);
      }
   }

   command->n_documents += n_documents;

   EXIT;
}

void
_mongoc_write_command_update_append (mongoc_write_command_t *command,
                                     const bson_t           *selector,
                                     const bson_t           *update,
                                     bool                    upsert,
                                     bool                    multi)
{
   const char *key;
   char keydata [16];
   bson_t doc;

   ENTRY;

   BSON_ASSERT (command);
   BSON_ASSERT (command->type == MONGOC_WRITE_COMMAND_UPDATE);
   BSON_ASSERT (selector && update);

   bson_init (&doc);
   BSON_APPEND_DOCUMENT (&doc, "q", selector);
   BSON_APPEND_DOCUMENT (&doc, "u", update);
   BSON_APPEND_BOOL (&doc, "upsert", upsert);
   BSON_APPEND_BOOL (&doc, "multi", multi);

   key = NULL;
   bson_uint32_to_string (command->n_documents, &key, keydata, sizeof keydata);
   BSON_ASSERT (key);
   BSON_APPEND_DOCUMENT (command->documents, key, &doc);
   command->n_documents++;

   bson_destroy (&doc);

   EXIT;
}

void
_mongoc_write_command_delete_append (mongoc_write_command_t *command,
                                     const bson_t           *selector)
{
   const char *key;
   char keydata [16];
   bson_t doc;

   ENTRY;

   BSON_ASSERT (command);
   BSON_ASSERT (command->type == MONGOC_WRITE_COMMAND_DELETE);
   BSON_ASSERT (selector);

   BSON_ASSERT (selector->len >= 5);

   bson_init (&doc);
   BSON_APPEND_DOCUMENT (&doc, "q", selector);
   BSON_APPEND_INT32 (&doc, "limit", command->u.delete_.multi ? 0 : 1);

   key = NULL;
   bson_uint32_to_string (command->n_documents, &key, keydata, sizeof keydata);
   BSON_ASSERT (key);
   BSON_APPEND_DOCUMENT (command->documents, key, &doc);
   command->n_documents++;

   bson_destroy (&doc);

   EXIT;
}

void
_mongoc_write_command_init_insert (mongoc_write_command_t *command,              /* IN */
                                   const bson_t *const    *documents,            /* IN */
                                   uint32_t                n_documents,          /* IN */
                                   bool                    ordered,              /* IN */
                                   bool                    allow_bulk_op_insert) /* IN */
{
   ENTRY;

   BSON_ASSERT (command);
   BSON_ASSERT (!n_documents || documents);

   command->type = MONGOC_WRITE_COMMAND_INSERT;
   command->documents = bson_new ();
   command->n_documents = 0;
   command->ordered = (uint8_t)ordered;
   command->u.insert.allow_bulk_op_insert = (uint8_t)allow_bulk_op_insert;

   if (n_documents) {
      _mongoc_write_command_insert_append (command, documents, n_documents);
   }

   EXIT;
}


void
_mongoc_write_command_init_delete (mongoc_write_command_t *command,  /* IN */
                                   const bson_t           *selector, /* IN */
                                   bool                    multi,    /* IN */
                                   bool                    ordered)  /* IN */
{
   ENTRY;

   BSON_ASSERT (command);
   BSON_ASSERT (selector);

   command->type = MONGOC_WRITE_COMMAND_DELETE;
   command->documents = bson_new ();
   command->n_documents = 0;
   command->u.delete_.multi = (uint8_t)multi;
   command->ordered = (uint8_t)ordered;

   _mongoc_write_command_delete_append (command, selector);

   EXIT;
}


void
_mongoc_write_command_init_update (mongoc_write_command_t *command,  /* IN */
                                   const bson_t           *selector, /* IN */
                                   const bson_t           *update,   /* IN */
                                   bool                    upsert,   /* IN */
                                   bool                    multi,    /* IN */
                                   bool                    ordered)  /* IN */
{
   ENTRY;

   BSON_ASSERT (command);
   BSON_ASSERT (selector);
   BSON_ASSERT (update);

   command->type = MONGOC_WRITE_COMMAND_UPDATE;
   command->documents = bson_new ();
   command->n_documents = 0;
   command->ordered = (uint8_t) ordered;

   _mongoc_write_command_update_append (command, selector, update, upsert, multi);

   EXIT;
}


static void
_mongoc_write_command_delete_legacy (mongoc_write_command_t       *command,
                                     mongoc_client_t              *client,
                                     uint32_t                      hint,
                                     const char                   *database,
                                     const char                   *collection,
                                     const mongoc_write_concern_t *write_concern,
                                     uint32_t                      offset,
                                     mongoc_write_result_t        *result,
                                     bson_error_t                 *error)
{
   const uint8_t *data;
   mongoc_rpc_t rpc;
   bson_iter_t iter;
   bson_iter_t q_iter;
   uint32_t len;
   bson_t *gle = NULL;
   char ns [MONGOC_NAMESPACE_MAX + 1];
   bool r;

   ENTRY;

   BSON_ASSERT (command);
   BSON_ASSERT (client);
   BSON_ASSERT (database);
   BSON_ASSERT (hint);
   BSON_ASSERT (collection);

   r = bson_iter_init (&iter, command->documents);

   if (!r) {
      BSON_ASSERT (false);
      EXIT;
   }

   if (!command->n_documents || !bson_iter_next (&iter)) {
      bson_set_error (error,
                      MONGOC_ERROR_COLLECTION,
                      MONGOC_ERROR_COLLECTION_DELETE_FAILED,
                      "Cannot do an empty delete.");
      result->failed = true;
      EXIT;
   }

   bson_snprintf (ns, sizeof ns, "%s.%s", database, collection);

   do {
      /* the document is like { "q": { <selector> }, limit: <0 or 1> } */
      r = (bson_iter_recurse (&iter, &q_iter) &&
           bson_iter_find (&q_iter, "q") &&
           BSON_ITER_HOLDS_DOCUMENT (&q_iter));

      if (!r) {
         BSON_ASSERT (false);
         EXIT;
      }

      bson_iter_document (&q_iter, &len, &data);
      BSON_ASSERT (data);
      BSON_ASSERT (len >= 5);

      rpc.delete_.msg_len = 0;
      rpc.delete_.request_id = 0;
      rpc.delete_.response_to = 0;
      rpc.delete_.opcode = MONGOC_OPCODE_DELETE;
      rpc.delete_.zero = 0;
      rpc.delete_.collection = ns;
      rpc.delete_.flags = command->u.delete_.multi ? MONGOC_DELETE_NONE
                         : MONGOC_DELETE_SINGLE_REMOVE;
      rpc.delete_.selector = data;

      hint = _mongoc_client_sendv (client, &rpc, 1, hint, write_concern,
                                   NULL, error);

      if (!hint) {
         result->failed = true;
         EXIT;
      }

      if (_mongoc_write_concern_needs_gle (write_concern)) {
         if (!_mongoc_client_recv_gle (client, hint, &gle, error)) {
            result->failed = true;
            bson_destroy (gle);
            EXIT;
         }

         _mongoc_write_result_merge_legacy (result, command, gle, offset);
         offset++;
         bson_destroy (gle);
      }
   } while (bson_iter_next (&iter));

   EXIT;
}


/*
 *-------------------------------------------------------------------------
 *
 * too_large_error --
 *
 *       Fill a bson_error_t and optional bson_t with error info after
 *       receiving a document for bulk insert, update, or remove that is
 *       larger than max_bson_size.
 *
 *       "err_doc" should be NULL or an empty initialized bson_t.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       "error" and optionally "err_doc" are filled out.
 *
 *-------------------------------------------------------------------------
 */

static void
too_large_error (bson_error_t *error,
                 int32_t       index,
                 int32_t       len,
                 int32_t       max_bson_size,
                 bson_t       *err_doc)
{
   /* MongoDB 2.6 uses code 2 for "too large". TODO: see CDRIVER-644 */
   const int code = 2;

   bson_set_error (error, MONGOC_ERROR_BSON, code,
                   "Document %u is too large for the cluster. "
                   "Document is %u bytes, max is %d.",
                   index, len, max_bson_size);

   if (err_doc) {
      BSON_APPEND_INT32 (err_doc, "index", index);
      BSON_APPEND_UTF8 (err_doc, "err", error->message);
      BSON_APPEND_INT32 (err_doc, "code", code);
   }
}


static void
_mongoc_write_command_insert_legacy (mongoc_write_command_t       *command,
                                     mongoc_client_t              *client,
                                     uint32_t                      hint,
                                     const char                   *database,
                                     const char                   *collection,
                                     const mongoc_write_concern_t *write_concern,
                                     uint32_t                      offset,
                                     mongoc_write_result_t        *result,
                                     bson_error_t                 *error)
{
   uint32_t current_offset;
   mongoc_iovec_t *iov;
   const uint8_t *data;
   mongoc_rpc_t rpc;
   bson_iter_t iter;
   uint32_t len;
   bson_t *gle = NULL;
   uint32_t size = 0;
   bool has_more;
   char ns [MONGOC_NAMESPACE_MAX + 1];
   bool r;
   uint32_t n_docs_in_batch;
   uint32_t index = 0;
   int32_t max_msg_size;
   int32_t max_bson_obj_size;
   bool singly;

   ENTRY;

   BSON_ASSERT (command);
   BSON_ASSERT (client);
   BSON_ASSERT (database);
   BSON_ASSERT (hint);
   BSON_ASSERT (collection);
   BSON_ASSERT (command->type == MONGOC_WRITE_COMMAND_INSERT);

   current_offset = offset;

   max_bson_obj_size = mongoc_cluster_node_max_bson_obj_size(&client->cluster, hint);
   max_msg_size = mongoc_cluster_node_max_msg_size (&client->cluster, hint);
   singly = !command->u.insert.allow_bulk_op_insert;

   r = bson_iter_init (&iter, command->documents);

   if (!r) {
      BSON_ASSERT (false);
      EXIT;
   }

   if (!command->n_documents || !bson_iter_next (&iter)) {
      bson_set_error (error,
                      MONGOC_ERROR_COLLECTION,
                      MONGOC_ERROR_COLLECTION_INSERT_FAILED,
                      "Cannot do an empty insert.");
      result->failed = true;
      EXIT;
   }

   bson_snprintf (ns, sizeof ns, "%s.%s", database, collection);

   iov = (mongoc_iovec_t *)bson_malloc ((sizeof *iov) * command->n_documents);

again:
   has_more = false;
   n_docs_in_batch = 0;
   size = (uint32_t)(sizeof (mongoc_rpc_header_t) +
                     4 +
                     strlen (database) +
                     1 +
                     strlen (collection) +
                     1);

   do {
      BSON_ASSERT (BSON_ITER_HOLDS_DOCUMENT (&iter));
      BSON_ASSERT (n_docs_in_batch <= index);
      BSON_ASSERT (index < command->n_documents);

      bson_iter_document (&iter, &len, &data);

      BSON_ASSERT (data);
      BSON_ASSERT (len >= 5);

      if (len > max_bson_obj_size) {
         /* document is too large */
         bson_t write_err_doc = BSON_INITIALIZER;

         too_large_error (error, index, len,
                          max_bson_obj_size, &write_err_doc);

         _mongoc_write_result_merge_legacy (result, command,
                                            &write_err_doc, offset + index);

         bson_destroy (&write_err_doc);

         if (command->ordered) {
            /* send the batch so far (if any) and return the error */
            break;
         }
      } else if ((n_docs_in_batch == 1 && singly) || size > (max_msg_size - len)) {
         /* batch is full, send it and then start the next batch */
         has_more = true;
         break;
      } else {
         /* add document to batch and continue building the batch */
         iov[n_docs_in_batch].iov_base = (void *) data;
         iov[n_docs_in_batch].iov_len = len;
         size += len;
         n_docs_in_batch++;
      }

      index++;
   } while (bson_iter_next (&iter));

   if (n_docs_in_batch) {
      rpc.insert.msg_len = 0;
      rpc.insert.request_id = 0;
      rpc.insert.response_to = 0;
      rpc.insert.opcode = MONGOC_OPCODE_INSERT;
      rpc.insert.flags = (
         command->ordered ? MONGOC_INSERT_NONE
                          : MONGOC_INSERT_CONTINUE_ON_ERROR);
      rpc.insert.collection = ns;
      rpc.insert.documents = iov;
      rpc.insert.n_documents = n_docs_in_batch;

      hint = _mongoc_client_sendv (client, &rpc, 1, hint, write_concern,
                                   NULL, error);

      if (!hint) {
         result->failed = true;
         GOTO (cleanup);
      }

      if (_mongoc_write_concern_needs_gle (write_concern)) {
         bool err = false;
         bson_iter_t citer;

         if (!_mongoc_client_recv_gle (client, hint, &gle, error)) {
            result->failed = true;
            GOTO (cleanup);
         }

         err = (bson_iter_init_find (&citer, gle, "err")
                && bson_iter_as_bool (&citer));

         /*
          * Overwrite the "n" field since it will be zero. Otherwise, our
          * merge_legacy code will not know how many we tried in this batch.
          */
         if (!err &&
             bson_iter_init_find (&citer, gle, "n") &&
             BSON_ITER_HOLDS_INT32 (&citer) &&
             !bson_iter_int32 (&citer)) {
            bson_iter_overwrite_int32 (&citer, n_docs_in_batch);
         }
      }
   }

cleanup:

   if (gle) {
      _mongoc_write_result_merge_legacy (result, command, gle, current_offset);
      current_offset = offset + index;
      bson_destroy (gle);
      gle = NULL;
   }

   if (has_more) {
      GOTO (again);
   }

   bson_free (iov);

   EXIT;
}


void
_empty_error (mongoc_write_command_t *command,
              bson_error_t           *error)
{
   static const uint32_t codes[] = {
      MONGOC_ERROR_COLLECTION_DELETE_FAILED,
      MONGOC_ERROR_COLLECTION_INSERT_FAILED,
      MONGOC_ERROR_COLLECTION_UPDATE_FAILED
   };

   bson_set_error (error,
                   MONGOC_ERROR_COLLECTION,
                   codes[command->type],
                   "Cannot do an empty %s",
                   gCommandNames[command->type]);
}


bool
_mongoc_write_command_will_overflow (uint32_t len_so_far,
                                     uint32_t document_len,
                                     uint32_t n_documents_written,
                                     int32_t  max_bson_size,
                                     int32_t  max_write_batch_size)
{
   /* max BSON object size + 16k - 2 bytes for ending NUL bytes.
    * server guarantees there is enough room: SERVER-10643
    */
   int32_t max_cmd_size = max_bson_size + 16382;

   BSON_ASSERT (max_bson_size);


   if (len_so_far + document_len > max_cmd_size) {
      return true;
   } else if (max_write_batch_size > 0 &&
              n_documents_written >= max_write_batch_size) {
      return true;
   }

   return false;
}


static void
_mongoc_write_command_update_legacy (mongoc_write_command_t       *command,
                                     mongoc_client_t              *client,
                                     uint32_t                      hint,
                                     const char                   *database,
                                     const char                   *collection,
                                     const mongoc_write_concern_t *write_concern,
                                     uint32_t                      offset,
                                     mongoc_write_result_t        *result,
                                     bson_error_t                 *error)
{
   mongoc_rpc_t rpc;
   bson_iter_t iter, subiter, subsubiter;
   bson_t doc;
   bool has_update, has_selector, is_upsert;
   bson_t update, selector;
   bson_t *gle = NULL;
   const uint8_t *data = NULL;
   uint32_t len = 0;
   size_t err_offset;
   bool val = false;
   char ns [MONGOC_NAMESPACE_MAX + 1];
   int32_t affected = 0;
   int vflags = (BSON_VALIDATE_UTF8 | BSON_VALIDATE_UTF8_ALLOW_NULL
               | BSON_VALIDATE_DOLLAR_KEYS | BSON_VALIDATE_DOT_KEYS);

   ENTRY;

   BSON_ASSERT (command);
   BSON_ASSERT (client);
   BSON_ASSERT (database);
   BSON_ASSERT (hint);
   BSON_ASSERT (collection);

   bson_iter_init (&iter, command->documents);
   while (bson_iter_next (&iter)) {
      if (bson_iter_recurse (&iter, &subiter) &&
          bson_iter_find (&subiter, "u") &&
          BSON_ITER_HOLDS_DOCUMENT (&subiter)) {
         bson_iter_document (&subiter, &len, &data);
         bson_init_static (&doc, data, len);

         if (bson_iter_init (&subsubiter, &doc) &&
             bson_iter_next (&subsubiter) &&
             (bson_iter_key (&subsubiter) [0] != '$') &&
             !bson_validate (&doc, (bson_validate_flags_t)vflags, &err_offset)) {
            result->failed = true;
            bson_set_error (error,
                            MONGOC_ERROR_BSON,
                            MONGOC_ERROR_BSON_INVALID,
                            "update document is corrupt or contains "
                            "invalid keys including $ or .");
            EXIT;
         }
      } else {
         result->failed = true;
         bson_set_error (error,
                         MONGOC_ERROR_BSON,
                         MONGOC_ERROR_BSON_INVALID,
                         "updates is malformed.");
         EXIT;
      }
   }

   bson_snprintf (ns, sizeof ns, "%s.%s", database, collection);

   bson_iter_init (&iter, command->documents);
   while (bson_iter_next (&iter)) {
      rpc.update.msg_len = 0;
      rpc.update.request_id = 0;
      rpc.update.response_to = 0;
      rpc.update.opcode = MONGOC_OPCODE_UPDATE;
      rpc.update.zero = 0;
      rpc.update.collection = ns;
      rpc.update.flags = MONGOC_UPDATE_NONE;

      has_update = false;
      has_selector = false;
      is_upsert = false;

      bson_iter_recurse (&iter, &subiter);
      while (bson_iter_next (&subiter)) {
         if (strcmp (bson_iter_key (&subiter), "u") == 0) {
            bson_iter_document (&subiter, &len, &data);
            rpc.update.update = data;
            bson_init_static (&update, data, len);
            has_update = true;
         } else if (strcmp (bson_iter_key (&subiter), "q") == 0) {
            bson_iter_document (&subiter, &len, &data);
            rpc.update.selector = data;
            bson_init_static (&selector, data, len);
            has_selector = true;
         } else if (strcmp (bson_iter_key (&subiter), "multi") == 0) {
            val = bson_iter_bool (&subiter);
            if (val) {
               rpc.update.flags = (mongoc_update_flags_t)(
                     rpc.update.flags | MONGOC_UPDATE_MULTI_UPDATE);
            }
         } else if (strcmp (bson_iter_key (&subiter), "upsert") == 0) {
            val = bson_iter_bool (&subiter);
            if (val) {
               rpc.update.flags = (mongoc_update_flags_t)(
                     rpc.update.flags | MONGOC_UPDATE_UPSERT);
            }
            is_upsert = true;
         }
      }

      hint = _mongoc_client_sendv (client, &rpc, 1, hint, write_concern,
                                   NULL, error);

      if (!hint) {
         result->failed = true;
         EXIT;
      }

      if (_mongoc_write_concern_needs_gle (write_concern)) {
         if (!_mongoc_client_recv_gle (client, hint, &gle, error)) {
            result->failed = true;
            bson_destroy (gle);

            EXIT;
         }

         if (bson_iter_init_find (&subiter, gle, "n") &&
             BSON_ITER_HOLDS_INT32 (&subiter)) {
            affected = bson_iter_int32 (&subiter);
         }

         /*
          * CDRIVER-372:
          *
          * Versions of MongoDB before 2.6 don't return the _id for an
          * upsert if _id is not an ObjectId.
          */
         if (is_upsert &&
             affected &&
             !bson_iter_init_find (&subiter, gle, "upserted") &&
             bson_iter_init_find (&subiter, gle, "updatedExisting") &&
             BSON_ITER_HOLDS_BOOL (&subiter) &&
             !bson_iter_bool (&subiter)) {
            if (has_update && bson_iter_init_find (&subiter, &update, "_id")) {
               bson_append_iter (gle, "upserted", 8, &subiter);
            } else if (has_selector &&
                       bson_iter_init_find (&subiter, &selector, "_id")) {
               bson_append_iter (gle, "upserted", 8, &subiter);
            }
         }

         _mongoc_write_result_merge_legacy (result, command, gle, offset);
         offset++;
         bson_destroy (gle);
      }
   }

   EXIT;
}


static mongoc_write_op_t gLegacyWriteOps[3] = {
   _mongoc_write_command_delete_legacy,
   _mongoc_write_command_insert_legacy,
   _mongoc_write_command_update_legacy };


static void
_mongoc_write_command(mongoc_write_command_t       *command,
                      mongoc_client_t              *client,
                      uint32_t                      hint,
                      const char                   *database,
                      const char                   *collection,
                      const mongoc_write_concern_t *write_concern,
                      uint32_t                      offset,
                      mongoc_write_result_t        *result,
                      bson_error_t                 *error)
{
   const uint8_t *data;
   bson_iter_t iter;
   const char *key;
   uint32_t len = 0;
   bson_t tmp;
   bson_t ar;
   bson_t cmd;
   bson_t reply;
   char str [16];
   bool has_more;
   bool ret = false;
   uint32_t i;
   int32_t max_bson_obj_size;
   int32_t max_write_batch_size;
   int32_t min_wire_version;
   uint32_t key_len;

   ENTRY;

   BSON_ASSERT (command);
   BSON_ASSERT (client);
   BSON_ASSERT (database);
   BSON_ASSERT (hint);
   BSON_ASSERT (collection);

   max_bson_obj_size = mongoc_cluster_node_max_bson_obj_size(&client->cluster, hint);
   max_write_batch_size = mongoc_cluster_node_max_write_batch_size(&client->cluster, hint);

   /*
    * If we have an unacknowledged write and the server supports the legacy
    * opcodes, then submit the legacy opcode so we don't need to wait for
    * a response from the server.
    */

   min_wire_version = mongoc_cluster_node_min_wire_version (&client->cluster,
                                                            hint);
   if (min_wire_version == -1) {
      EXIT;
   }

   if ((min_wire_version == 0) &&
       !_mongoc_write_concern_needs_gle (write_concern)) {
      gLegacyWriteOps[command->type] (command, client, hint, database,
                                      collection, write_concern, offset,
                                      result, error);
      EXIT;
   }

   if (!command->n_documents ||
       !bson_iter_init (&iter, command->documents) ||
       !bson_iter_next (&iter)) {
      _empty_error (command, error);
      result->failed = true;
      EXIT;
   }

again:
   bson_init (&cmd);
   has_more = false;
   i = 0;

   BSON_APPEND_UTF8 (&cmd, gCommandNames[command->type], collection);
   BSON_APPEND_DOCUMENT (&cmd, "writeConcern",
                         WRITE_CONCERN_DOC (write_concern));
   BSON_APPEND_BOOL (&cmd, "ordered", command->ordered);

   if (!_mongoc_write_command_will_overflow (0,
                                             command->documents->len,
                                             command->n_documents,
                                             max_bson_obj_size,
                                             max_write_batch_size)) {
      /* copy the whole documents buffer as e.g. "updates": [...] */
      BSON_APPEND_ARRAY (&cmd,
                         gCommandFields[command->type],
                         command->documents);
      i = command->n_documents;
   } else {
      bson_append_array_begin (&cmd, gCommandFields[command->type], -1, &ar);

      do {
         if (!BSON_ITER_HOLDS_DOCUMENT (&iter)) {
            BSON_ASSERT (false);
         }

         bson_iter_document (&iter, &len, &data);
         key_len = (uint32_t) bson_uint32_to_string (i, &key, str, sizeof str);

         if (_mongoc_write_command_will_overflow (ar.len,
                                                  key_len + len + 2,
                                                  i,
                                                  max_bson_obj_size,
                                                  max_write_batch_size)) {
            has_more = true;
            break;
         }

         if (!bson_init_static (&tmp, data, len)) {
            BSON_ASSERT (false);
         }

         BSON_APPEND_DOCUMENT (&ar, key, &tmp);

         bson_destroy (&tmp);

         i++;
      } while (bson_iter_next (&iter));

      bson_append_array_end (&cmd, &ar);
   }

   if (!i) {
      too_large_error (error, i, len, max_bson_obj_size, NULL);
      result->failed = true;
      ret = false;
   } else {
      ret = _mongoc_client_command_simple_with_hint (client, database, &cmd, NULL,
                                          &reply, hint, error);

      if (!ret) {
         result->failed = true;
      }

      _mongoc_write_result_merge (result, command, &reply, offset);
      offset += i;
      bson_destroy (&reply);
   }

   bson_destroy (&cmd);

   if (has_more && (ret || !command->ordered)) {
      GOTO (again);
   }

   EXIT;
}


void
_mongoc_write_command_execute (mongoc_write_command_t       *command,       /* IN */
                               mongoc_client_t              *client,        /* IN */
                               uint32_t                      hint,          /* IN */
                               const char                   *database,      /* IN */
                               const char                   *collection,    /* IN */
                               const mongoc_write_concern_t *write_concern, /* IN */
                               uint32_t                      offset,        /* IN */
                               mongoc_write_result_t        *result)        /* OUT */
{
   int32_t max_wire_version;

   ENTRY;

   BSON_ASSERT (command);
   BSON_ASSERT (client);
   BSON_ASSERT (database);
   BSON_ASSERT (collection);
   BSON_ASSERT (result);

   if (!write_concern) {
      write_concern = client->write_concern;
   }

   if (!_mongoc_write_concern_is_valid(write_concern)) {
      bson_set_error (&result->error,
                      MONGOC_ERROR_COMMAND,
                      MONGOC_ERROR_COMMAND_INVALID_ARG,
                      "The write concern is invalid.");
      result->failed = true;
      EXIT;
   }

   if (!hint) {
      hint = _mongoc_client_preselect (client, MONGOC_OPCODE_INSERT,
                                       write_concern, NULL, &result->error);
      if (!hint) {
         result->failed = true;
         EXIT;
      }
   }

   command->hint = hint;

   max_wire_version = mongoc_cluster_node_max_wire_version (&client->cluster, hint);
   if (max_wire_version == -1) {
      EXIT;
   }

   if (max_wire_version >= WRITE_COMMAND_WIRE_VERSION) {
      _mongoc_write_command (command, client, hint, database,
                             collection, write_concern, offset,
                             result, &result->error);
   } else {
      gLegacyWriteOps[command->type] (command, client, hint, database,
                                      collection, write_concern, offset,
                                      result, &result->error);
   }

   EXIT;
}


void
_mongoc_write_command_destroy (mongoc_write_command_t *command)
{
   ENTRY;

   if (command) {
      bson_destroy (command->documents);
   }

   EXIT;
}


void
_mongoc_write_result_init (mongoc_write_result_t *result) /* IN */
{
   ENTRY;

   BSON_ASSERT (result);

   memset (result, 0, sizeof *result);

   bson_init (&result->upserted);
   bson_init (&result->writeConcernError);
   bson_init (&result->writeErrors);

   EXIT;
}


void
_mongoc_write_result_destroy (mongoc_write_result_t *result)
{
   ENTRY;

   BSON_ASSERT (result);

   bson_destroy (&result->upserted);
   bson_destroy (&result->writeConcernError);
   bson_destroy (&result->writeErrors);

   EXIT;
}


static void
_mongoc_write_result_append_upsert (mongoc_write_result_t *result,
                                    int32_t                idx,
                                    const bson_value_t    *value)
{
   bson_t child;
   const char *keyptr = NULL;
   char key[12];
   int len;

   BSON_ASSERT (result);
   BSON_ASSERT (value);

   len = (int)bson_uint32_to_string (result->upsert_append_count, &keyptr, key,
                                     sizeof key);

   bson_append_document_begin (&result->upserted, keyptr, len, &child);
   BSON_APPEND_INT32 (&child, "index", idx);
   BSON_APPEND_VALUE (&child, "_id", value);
   bson_append_document_end (&result->upserted, &child);

   result->upsert_append_count++;
}


void
_mongoc_write_result_merge_legacy (mongoc_write_result_t  *result,  /* IN */
                                   mongoc_write_command_t *command, /* IN */
                                   const bson_t           *reply,   /* IN */
                                   uint32_t                offset)
{
   const bson_value_t *value;
   bson_t holder, write_errors, child;
   bson_iter_t iter;
   bson_iter_t ar;
   bson_iter_t citer;
   const char *err = NULL;
   int32_t code = 0;
   int32_t n = 0;
   int32_t upsert_idx = 0;

   ENTRY;

   BSON_ASSERT (result);
   BSON_ASSERT (reply);

   if (bson_iter_init_find (&iter, reply, "n") &&
       BSON_ITER_HOLDS_INT32 (&iter)) {
      n = bson_iter_int32 (&iter);
   }

   if (bson_iter_init_find (&iter, reply, "err") &&
       BSON_ITER_HOLDS_UTF8 (&iter)) {
      err = bson_iter_utf8 (&iter, NULL);
   }

   if (bson_iter_init_find (&iter, reply, "code") &&
       BSON_ITER_HOLDS_INT32 (&iter)) {
      code = bson_iter_int32 (&iter);
   }

   if (code && err) {
      bson_set_error (&result->error,
                      MONGOC_ERROR_COLLECTION,
                      code,
                      "%s", err);
      result->failed = true;

      bson_init(&holder);
      bson_append_array_begin(&holder, "0", 1, &write_errors);
      bson_append_document_begin(&write_errors, "0", 1, &child);
      bson_append_int32(&child, "index", 5, 0);
      bson_append_int32(&child, "code", 4, code);
      bson_append_utf8(&child, "errmsg", 6, err, -1);
      bson_append_document_end(&write_errors, &child);
      bson_append_array_end(&holder, &write_errors);
      bson_iter_init(&iter, &holder);
      bson_iter_next(&iter);

      _mongoc_write_result_merge_arrays (offset, result, &result->writeErrors,
                                         &iter);

      bson_destroy(&holder);
   }

   switch (command->type) {
   case MONGOC_WRITE_COMMAND_INSERT:
      if (n) {
         result->nInserted += n;
      }
      break;
   case MONGOC_WRITE_COMMAND_DELETE:
      result->nRemoved += n;
      break;
   case MONGOC_WRITE_COMMAND_UPDATE:
      if (bson_iter_init_find (&iter, reply, "upserted") &&
         !BSON_ITER_HOLDS_ARRAY (&iter)) {
         result->nUpserted += n;
         value = bson_iter_value (&iter);
         _mongoc_write_result_append_upsert (result, offset, value);
      } else if (bson_iter_init_find (&iter, reply, "upserted") &&
                 BSON_ITER_HOLDS_ARRAY (&iter)) {
         result->nUpserted += n;
         if (bson_iter_recurse (&iter, &ar)) {
            while (bson_iter_next (&ar)) {
               if (BSON_ITER_HOLDS_DOCUMENT (&ar) &&
                   bson_iter_recurse (&ar, &citer) &&
                   bson_iter_find (&citer, "_id")) {
                  value = bson_iter_value (&citer);
                  _mongoc_write_result_append_upsert (result,
                                                      offset + upsert_idx,
                                                      value);
                  upsert_idx++;
               }
            }
         }
      } else if ((n == 1) &&
                 bson_iter_init_find (&iter, reply, "updatedExisting") &&
                 BSON_ITER_HOLDS_BOOL (&iter) &&
                 !bson_iter_bool (&iter)) {
         result->nUpserted += n;
      } else {
         result->nMatched += n;
      }
      break;
   default:
      break;
   }

   result->omit_nModified = true;

   EXIT;
}


static int32_t
_mongoc_write_result_merge_arrays (uint32_t               offset,
                                   mongoc_write_result_t *result, /* IN */
                                   bson_t                *dest,   /* IN */
                                   bson_iter_t           *iter)   /* IN */
{
   const bson_value_t *value;
   bson_iter_t ar;
   bson_iter_t citer;
   int32_t idx;
   int32_t count = 0;
   int32_t aridx;
   bson_t child;
   const char *keyptr = NULL;
   char key[12];
   int len;

   ENTRY;

   BSON_ASSERT (result);
   BSON_ASSERT (dest);
   BSON_ASSERT (iter);
   BSON_ASSERT (BSON_ITER_HOLDS_ARRAY (iter));

   aridx = bson_count_keys (dest);

   if (bson_iter_recurse (iter, &ar)) {
      while (bson_iter_next (&ar)) {
         if (BSON_ITER_HOLDS_DOCUMENT (&ar) &&
             bson_iter_recurse (&ar, &citer)) {
            len = (int)bson_uint32_to_string (aridx++, &keyptr, key,
                                              sizeof key);
            bson_append_document_begin (dest, keyptr, len, &child);
            while (bson_iter_next (&citer)) {
               if (BSON_ITER_IS_KEY (&citer, "index")) {
                  idx = bson_iter_int32 (&citer) + offset;
                  BSON_APPEND_INT32 (&child, "index", idx);
               } else {
                  value = bson_iter_value (&citer);
                  BSON_APPEND_VALUE (&child, bson_iter_key (&citer), value);
               }
            }
            bson_append_document_end (dest, &child);
            count++;
         }
      }
   }

   RETURN (count);
}


void
_mongoc_write_result_merge (mongoc_write_result_t  *result,  /* IN */
                            mongoc_write_command_t *command, /* IN */
                            const bson_t           *reply,   /* IN */
                            uint32_t                offset)
{
   int32_t server_index = 0;
   const bson_value_t *value;
   bson_iter_t iter;
   bson_iter_t citer;
   bson_iter_t ar;
   int32_t n_upserted = 0;
   int32_t affected = 0;

   ENTRY;

   BSON_ASSERT (result);
   BSON_ASSERT (reply);

   if (bson_iter_init_find (&iter, reply, "n") &&
       BSON_ITER_HOLDS_INT32 (&iter)) {
      affected = bson_iter_int32 (&iter);
   }

   if (bson_iter_init_find (&iter, reply, "writeErrors") &&
       BSON_ITER_HOLDS_ARRAY (&iter) &&
       bson_iter_recurse (&iter, &citer) &&
       bson_iter_next (&citer)) {
      result->failed = true;
   }

   switch (command->type) {
   case MONGOC_WRITE_COMMAND_INSERT:
      result->nInserted += affected;
      break;
   case MONGOC_WRITE_COMMAND_DELETE:
      result->nRemoved += affected;
      break;
   case MONGOC_WRITE_COMMAND_UPDATE:

      /* server returns each upserted _id with its index into this batch
       * look for "upserted": [{"index": 4, "_id": ObjectId()}, ...] */
      if (bson_iter_init_find (&iter, reply, "upserted")) {
         if (BSON_ITER_HOLDS_ARRAY (&iter) &&
             (bson_iter_recurse (&iter, &ar))) {

            while (bson_iter_next (&ar)) {
               if (BSON_ITER_HOLDS_DOCUMENT (&ar) &&
                   bson_iter_recurse (&ar, &citer) &&
                   bson_iter_find (&citer, "index") &&
                   BSON_ITER_HOLDS_INT32 (&citer)) {
                  server_index = bson_iter_int32 (&citer);

                  if (bson_iter_recurse (&ar, &citer) &&
                      bson_iter_find (&citer, "_id")) {
                     value = bson_iter_value (&citer);
                     _mongoc_write_result_append_upsert (result,
                                                         offset + server_index,
                                                         value);
                     n_upserted++;
                  }
               }
            }
         }
         result->nUpserted += n_upserted;
         /*
          * XXX: The following addition to nMatched needs some checking.
          *      I'm highly skeptical of it.
          */
         result->nMatched += BSON_MAX (0, (affected - n_upserted));
      } else {
         result->nMatched += affected;
      }
      /*
       * SERVER-13001 - in a mixed sharded cluster a call to update could
       * return nModified (>= 2.6) or not (<= 2.4).  If any call does not
       * return nModified we can't report a valid final count so omit the
       * field completely.
       */
      if (bson_iter_init_find (&iter, reply, "nModified") &&
          BSON_ITER_HOLDS_INT32 (&iter)) {
         result->nModified += bson_iter_int32 (&iter);
      } else {
         /*
          * nModified could be BSON_TYPE_NULL, which should also be omitted.
          */
         result->omit_nModified = true;
      }
      break;
   default:
      BSON_ASSERT (false);
      break;
   }

   if (bson_iter_init_find (&iter, reply, "writeErrors") &&
       BSON_ITER_HOLDS_ARRAY (&iter)) {
      _mongoc_write_result_merge_arrays (offset, result, &result->writeErrors,
                                         &iter);
   }

   if (bson_iter_init_find (&iter, reply, "writeConcernError") &&
       BSON_ITER_HOLDS_DOCUMENT (&iter)) {

      uint32_t len;
      const uint8_t *data;
      bson_t write_concern_error;

      bson_iter_document (&iter, &len, &data);
      bson_init_static (&write_concern_error, data, len);
      bson_concat (&result->writeConcernError, &write_concern_error);
   }

   EXIT;
}


bool
_mongoc_write_result_complete (mongoc_write_result_t *result,
                               bson_t                *bson,
                               bson_error_t          *error)
{
   bson_iter_t iter;
   bson_iter_t citer;
   const char *err = NULL;
   uint32_t code = 0;
   bool ret;

   ENTRY;

   BSON_ASSERT (result);

   ret = (!result->failed &&
          bson_empty0 (&result->writeConcernError) &&
          bson_empty0 (&result->writeErrors));

   if (bson) {
      BSON_APPEND_INT32 (bson, "nInserted", result->nInserted);
      BSON_APPEND_INT32 (bson, "nMatched", result->nMatched);
      if (!result->omit_nModified) {
         BSON_APPEND_INT32 (bson, "nModified", result->nModified);
      }
      BSON_APPEND_INT32 (bson, "nRemoved", result->nRemoved);
      BSON_APPEND_INT32 (bson, "nUpserted", result->nUpserted);
      if (!bson_empty0 (&result->upserted)) {
         BSON_APPEND_ARRAY (bson, "upserted", &result->upserted);
      }
      BSON_APPEND_ARRAY (bson, "writeErrors", &result->writeErrors);
      if (!bson_empty0 (&result->writeConcernError)) {
         BSON_APPEND_DOCUMENT (bson, "writeConcernError",
                            &result->writeConcernError);
      }
   }

   if (error) {
      memcpy (error, &result->error, sizeof *error);
   }

   if (!ret &&
       !bson_empty0 (&result->writeErrors) &&
       bson_iter_init (&iter, &result->writeErrors) &&
       bson_iter_next (&iter) &&
       BSON_ITER_HOLDS_DOCUMENT (&iter) &&
       bson_iter_recurse (&iter, &citer)) {
      while (bson_iter_next (&citer)) {
         if (BSON_ITER_IS_KEY (&citer, "errmsg")) {
            err = bson_iter_utf8 (&citer, NULL);
         } else if (BSON_ITER_IS_KEY (&citer, "code")) {
            code = bson_iter_int32 (&citer);
         }
      }
      if (err && code) {
         bson_set_error (error, MONGOC_ERROR_COMMAND, code, "%s", err);
      }
   }

   RETURN (ret);
}
