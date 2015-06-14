/*
 * Copyright 2015 MongoDB, Inc.
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


#include <bson.h>

#include "mock-rs.h"
#include "sync-queue.h"


struct _mock_rs_t {
   int n_secondaries;
   int n_arbiters;
   mongoc_array_t servers;
   int32_t max_wire_version;
   char *hosts_str;
   mongoc_uri_t *uri;
   sync_queue_t *q;
   bool verbose;
};


mock_server_t *
get_server (mongoc_array_t *servers, int i)
{
   return _mongoc_array_index (servers, mock_server_t *, i);
}


void
append_array (mongoc_array_t *dst, mongoc_array_t *src)
{
   _mongoc_array_append_vals (dst, src->data, (uint32_t) src->len);
}


/* a string like: "localhost:1","localhost:2","localhost:3" */
char *
hosts (mongoc_array_t *servers)
{
   int i;
   const char *host_and_port;
   bson_string_t *hosts_str = bson_string_new ("");

   for (i = 0; i < servers->len; i++) {
      host_and_port = mock_server_get_host_and_port (get_server (servers, i));
      bson_string_append_printf (hosts_str, "\"%s\"", host_and_port);

      if (i < servers->len - 1) {
         bson_string_append_printf (hosts_str, ", ");
      }
   }

   return bson_string_free (hosts_str, false);  /* detach buffer */
}


mongoc_uri_t *
make_uri (mongoc_array_t *servers)
{
   int i;
   const char *host_and_port;
   bson_string_t *uri_str = bson_string_new ("mongodb://");
   mongoc_uri_t *uri;

   for (i = 0; i < servers->len; i++) {
      host_and_port = mock_server_get_host_and_port (get_server (servers, i));
      bson_string_append_printf (uri_str, "%s", host_and_port);

      if (i < servers->len - 1) {
         bson_string_append_printf (uri_str, ",");
      }
   }

   bson_string_append_printf (uri_str, "/?replicaSet=rs");

   uri = mongoc_uri_new (uri_str->str);

   bson_string_free (uri_str, true);

   return uri;
}


/*--------------------------------------------------------------------------
 *
 * mock_rs_with_autoismaster --
 *
 *       A new mock replica set. Each member autoresponds to ismaster.
 *       Call mock_rs_run to start it, then mock_rs_get_uri to connect.
 *
 * Returns:
 *       A replica set you must mock_rs_destroy.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mock_rs_t *
mock_rs_with_autoismaster (int32_t max_wire_version,
                           int n_secondaries,
                           int n_arbiters)
{
   mock_rs_t *rs = bson_malloc0 (sizeof (mock_rs_t));

   rs->max_wire_version = max_wire_version;
   rs->n_secondaries = n_secondaries;
   rs->n_arbiters = n_arbiters;
   _mongoc_array_init (&rs->servers, sizeof (mock_server_t *));
   rs->q = q_new ();

   return rs;   
}


/*--------------------------------------------------------------------------
 *
 * mock_rs_set_verbose --
 *
 *       Tell the replica set whether to log during normal operation.
 *
 *--------------------------------------------------------------------------
 */

void
mock_rs_set_verbose (mock_rs_t *rs,
                     bool verbose)
{
   int i;

   rs->verbose = true;

   for (i = 0; i < rs->servers.len; i++) {
      mock_server_set_verbose (get_server (&rs->servers, i), verbose);
   }
}


static bool
rs_q_append (request_t *request,
             void *data)
{
   mock_rs_t *rs = (mock_rs_t *)data;

   q_put (rs->q, (void *)request);

   return true;  /* handled */
}


/*--------------------------------------------------------------------------
 *
 * mock_rs_run --
 *
 *       Start each member listening on an unused port. After this, call
 *       mock_rs_get_uri to connect.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       The replica set's URI is set.
 *
 *--------------------------------------------------------------------------
 */

void
mock_rs_run (mock_rs_t *rs)
{
   int i;
   mock_server_t *primary;
   mongoc_array_t secondaries;
   mongoc_array_t arbiters;
   mock_server_t *server;
   char *hosts_str;
   char *ismaster_json;

   /* start primary */
   primary = mock_server_new ();
   mock_server_run (primary);

   /* start secondaries */
   _mongoc_array_init (&secondaries, sizeof(mock_server_t *));

   for (i = 0; i < rs->n_secondaries; i++) {
      server = mock_server_new ();
      mock_server_run (server);
      _mongoc_array_append_val (&secondaries, server);
   }

   /* start arbiters */
   _mongoc_array_init (&arbiters, sizeof(mock_server_t *));

   for (i = 0; i < rs->n_arbiters; i++) {
      server = mock_server_new ();
      mock_server_run (server);
      _mongoc_array_append_val (&arbiters, server);
   }

   /* add all servers to replica set */
   _mongoc_array_append_val (&rs->servers, primary);
   append_array (&rs->servers, &secondaries);
   append_array (&rs->servers, &arbiters);

   /* enqueue unhandled requests. added 1st so run last, after auto-ismaster */
   for (i = 0; i < rs->servers.len; i++) {
      mock_server_autoresponds (get_server (&rs->servers, i),
                                rs_q_append,
                                (void *) rs,
                                NULL);
   }


   /* now we know all servers' ports and we have them in one array */
   rs->hosts_str = hosts_str = hosts (&rs->servers);
   rs->uri = make_uri (&rs->servers);

   /* primary's ismaster response */
   ismaster_json = bson_strdup_printf (
      "{'ok': 1, 'ismaster': true, 'secondary': false, 'maxWireVersion': %d, "
      "'setName': 'rs', 'hosts': [%s]}", rs->max_wire_version, hosts_str);
   mock_server_auto_ismaster (primary, ismaster_json);

   bson_free (ismaster_json);

   /* secondaries' ismaster response */
   ismaster_json = bson_strdup_printf (
      "{'ok': 1, 'ismaster': false, 'secondary': true, 'maxWireVersion': %d, "
      "'setName': 'rs', 'hosts': [%s]}", rs->max_wire_version, hosts_str);

   for (i = 0; i < rs->n_secondaries; i++) {
      mock_server_auto_ismaster (get_server (&secondaries, i), ismaster_json);
   }

   bson_free (ismaster_json);

   /* arbiters' ismaster response */
   ismaster_json = bson_strdup_printf (
      "{'ok': 1, 'ismaster': true, 'arbiterOnly': true, 'maxWireVersion': %d, "
      "'setName': 'rs', 'hosts': [%s]}", rs->max_wire_version, hosts_str);

   for (i = 0; i < rs->n_arbiters; i++) {
      mock_server_auto_ismaster (get_server (&arbiters, i), ismaster_json);
   }

   for (i = 0; i < rs->servers.len; i++) {
      mock_server_set_verbose (get_server (&rs->servers, i), rs->verbose);
   }

   bson_free (ismaster_json);
   _mongoc_array_destroy (&secondaries);
   _mongoc_array_destroy (&arbiters);
}


/*--------------------------------------------------------------------------
 *
 * mock_rs_get_uri --
 *
 *       Call after mock_rs_run to get the connection string.
 *
 * Returns:
 *       A const URI.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

const mongoc_uri_t *
mock_rs_get_uri (mock_rs_t *rs)
{
   return rs->uri;
}


/*--------------------------------------------------------------------------
 *
 * mock_rs_receives_query --
 *
 *       Pop a client request if one is enqueued, or wait up to
 *       request_timeout_ms for the client to send a request.
 *
 * Returns:
 *       A request you must request_destroy, or NULL if the request does
 *       not match.
 *
 * Side effects:
 *       Logs if the current request is not a query matching ns, flags,
 *       skip, n_return, query_json, and fields_json.
 *
 *--------------------------------------------------------------------------
 */

/* TODO: refactor with mock_server_receives_query, etc.? */
request_t *
mock_rs_receives_query (mock_rs_t *rs,
                        const char *ns,
                        mongoc_query_flags_t flags,
                        uint32_t skip,
                        uint32_t n_return,
                        const char *query_json,
                        const char *fields_json)
{
   request_t *request;

   /* TODO: configurable timeout val */
   request = (request_t *) q_get (rs->q, 100 * 1000);

   if (!request_matches_query (request,
                               ns,
                               flags,
                               skip,
                               n_return,
                               query_json,
                               fields_json,
                               false)) {
      request_destroy (request);
      return NULL;
   }

   return request;
}


/*--------------------------------------------------------------------------
 *
 * mock_rs_receives_kill_cursors --
 *
 *       Pop a client request if one is enqueued, or wait up to
 *       request_timeout_ms for the client to send a request.
 *
 *       Real-life OP_KILLCURSORS can take multiple ids, but that is
 *       not yet supported here.
 *
 * Returns:
 *       A request you must request_destroy, or NULL if the request
 *       does not match.
 *
 * Side effects:
 *       Logs if the current request is not an OP_KILLCURSORS with the
 *       expected cursor_id.
 *
 *--------------------------------------------------------------------------
 */

request_t *
mock_rs_receives_kill_cursors (mock_rs_t *rs,
                               int64_t cursor_id)
{
   request_t *request;

   /* TODO: configurable timeout val */
   request = (request_t *) q_get (rs->q, 100 * 1000);

   if (!request_matches_kill_cursors (request, cursor_id)) {
      request_destroy (request);
      return NULL;
   }

   return request;
}


/*--------------------------------------------------------------------------
 *
 * mock_rs_replies --
 *
 *       Respond to a client request.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       Sends an OP_REPLY to the client.
 *
 *--------------------------------------------------------------------------
 */

void
mock_rs_replies (request_t *request,
                 uint32_t flags,
                 int64_t cursor_id,
                 int32_t starting_from,
                 int32_t number_returned,
                 const char *docs_json)
{
   mock_server_replies (request, flags, cursor_id, starting_from,
                        number_returned, docs_json);
}


/*--------------------------------------------------------------------------
 *
 * mock_rs_destroy --
 *
 *       Free a mock_rs_t.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       Destroys each member mock_server_t, closes sockets, joins threads.
 *
 *--------------------------------------------------------------------------
 */

void
mock_rs_destroy (mock_rs_t *rs)
{
   int i;

   for (i = 0; i < rs->servers.len; i++) {
      mock_server_destroy (get_server (&rs->servers, i));
   }

   _mongoc_array_destroy (&rs->servers);

   bson_free (rs->hosts_str);
   mongoc_uri_destroy (rs->uri);
   q_destroy (rs->q);
}