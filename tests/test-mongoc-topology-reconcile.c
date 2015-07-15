#include <mongoc.h>

#include "mongoc-client-private.h"
#include "utlist.h"

#include "mock_server/mock-server.h"
#include "TestSuite.h"
#include "test-conveniences.h"

#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "topology-reconcile-test"


static const mongoc_topology_scanner_node_t *
get_node (mongoc_topology_scanner_t *ts,
          const char *host_and_port)
{
   mongoc_topology_scanner_node_t *node;

   DL_FOREACH (ts->nodes, node)
   {
      if (!strcmp (host_and_port, node->host.host_and_port)) {
         return node;
      }
   }

   return NULL;
}


void
rs_response_to_ismaster (mock_server_t *server,
                         bool primary,
                         bool has_tags,
                         ...)
{
   va_list ap;
   bson_string_t *hosts;
   bool first;
   mock_server_t *host;
   char *ismaster_response;

   hosts = bson_string_new ("");

   va_start (ap, has_tags);

   first = true;
   while ((host = va_arg (ap, mock_server_t *))) {
      if (first) {
         first = false;
      } else {
         bson_string_append (hosts, ",");
      }

      bson_string_append_printf (hosts,
                                 "'%s'",
                                 mock_server_get_host_and_port (host));
   }

   va_end (ap);

   ismaster_response = bson_strdup_printf (
      "{'ok': 1, "
      " 'setName': 'rs',"
      " 'ismaster': %s,"
      " 'secondary': %s,"
      " 'tags': {%s},"
      " 'hosts': [%s]"
      "}",
      primary ? "true" : "false",
      primary ? "false" : "true",
      has_tags ? "'key': 'value'" : "",
      hosts->str);

   mock_server_auto_ismaster (server, ismaster_response);

   bson_free (ismaster_response);
   bson_string_free (hosts, true);
}


#define RS_RESPONSE_TO_ISMASTER(server, primary, has_tags, ...) \
   rs_response_to_ismaster (server, primary, has_tags, __VA_ARGS__, NULL)


bool
selects_server (mongoc_client_t *client,
                mongoc_read_prefs_t *read_prefs,
                mock_server_t *server)
{
   bson_error_t error;
   mongoc_server_description_t *sd;
   bool result;

   sd = mongoc_topology_select (client->topology, MONGOC_SS_READ,
                                read_prefs, 15, &error);

   if (!sd) {
      fprintf (stderr, "%s\n", error.message);
      return false;
   }

   result = (0 == strcmp (mongoc_server_description_host (sd)->host_and_port,
                          mock_server_get_host_and_port (server)));

   mongoc_server_description_destroy (sd);

   return result;
}


static void
_test_topology_reconcile_rs (bool pooled)
{
   mock_server_t *server0;
   mock_server_t *server1;
   char *uri_str;
   mongoc_uri_t *uri;
   mongoc_client_pool_t *pool = NULL;
   mongoc_client_t *client;
   mongoc_read_prefs_t *secondary_read_prefs;
   mongoc_read_prefs_t *primary_read_prefs;
   mongoc_read_prefs_t *tag_read_prefs;

   server0 = mock_server_new ();
   server1 = mock_server_new ();
   mock_server_run (server0);
   mock_server_run (server1);

   /* secondary, no tags */
   RS_RESPONSE_TO_ISMASTER (server0, false, false, server0, server1);
   /* primary, no tags */
   RS_RESPONSE_TO_ISMASTER (server1, true, false, server0, server1);

   /* provide secondary in seed list */
   uri_str = bson_strdup_printf (
      "mongodb://%s/?replicaSet=rs&connectTimeoutMS=10",
      mock_server_get_host_and_port (server0));

   uri = mongoc_uri_new (uri_str);

   if (pooled) {
      pool = mongoc_client_pool_new (uri);
      client = mongoc_client_pool_pop (pool);
   } else {
      client = mongoc_client_new (uri_str);
   }

   secondary_read_prefs = mongoc_read_prefs_new (MONGOC_READ_SECONDARY);
   primary_read_prefs = mongoc_read_prefs_new (MONGOC_READ_PRIMARY);
   tag_read_prefs = mongoc_read_prefs_new (MONGOC_READ_NEAREST);
   mongoc_read_prefs_add_tag (tag_read_prefs, tmp_bson ("{'key': 'value'}"));

   /*
    * server0 is selected, server1 is discovered and added to scanner.
    */
   assert (selects_server (client, secondary_read_prefs, server0));
   assert (get_node (client->topology->scanner,
                     mock_server_get_host_and_port (server1)));

   /*
    * select again with mode "primary": server1 is selected.
    */
   assert (selects_server (client, primary_read_prefs, server1));

   /*
    * remove server0 from set. tag primary, select w/ tags to trigger re-scan.
    */
   RS_RESPONSE_TO_ISMASTER (server1, true, true, server1);  /* server0 absent */
   assert (selects_server (client, tag_read_prefs, server1));

   assert (!get_node (client->topology->scanner,
                      mock_server_get_host_and_port (server0)));

   mongoc_read_prefs_destroy (primary_read_prefs);
   mongoc_read_prefs_destroy (secondary_read_prefs);
   mongoc_read_prefs_destroy (tag_read_prefs);

   if (pooled) {
      mongoc_client_pool_push (pool, client);
      mongoc_client_pool_destroy (pool);
   } else {
      mongoc_client_destroy (client);
   }

   mongoc_uri_destroy (uri);
   bson_free (uri_str);
   mock_server_destroy (server1);
   mock_server_destroy (server0);
}


static void
test_topology_reconcile_rs_single (void)
{
   _test_topology_reconcile_rs (false);
}


static void
test_topology_reconcile_rs_pooled (void)
{
   _test_topology_reconcile_rs (true);
}


static void
_test_topology_reconcile_sharded (bool pooled)
{
   mock_server_t *server0;
   mock_server_t *server1;
   char *uri_str;
   mongoc_uri_t *uri;
   mongoc_client_pool_t *pool = NULL;
   mongoc_client_t *client;
   mongoc_read_prefs_t *primary_read_prefs;
   int64_t start;

   server0 = mock_server_new ();
   server1 = mock_server_new ();
   mock_server_run (server0);
   mock_server_run (server1);

   /* mongos */
   mock_server_auto_ismaster (server0,
                              "{'ok': 1, 'ismaster': true, 'msg': 'isdbgrid'}");

   /* replica set secondary - should be removed */
   RS_RESPONSE_TO_ISMASTER (server1, false, false, server0, server1);

   /* provide both servers in seed list */
   uri_str = bson_strdup_printf (
      "mongodb://%s,%s/?connectTimeoutMS=10&serverselectiontimeoutms=1000",
      mock_server_get_host_and_port (server0),
      mock_server_get_host_and_port (server1));

   uri = mongoc_uri_new (uri_str);

   if (pooled) {
      pool = mongoc_client_pool_new (uri);
      client = mongoc_client_pool_pop (pool);
   } else {
      client = mongoc_client_new (uri_str);
   }

   primary_read_prefs = mongoc_read_prefs_new (MONGOC_READ_PRIMARY);

   /*
    * server0 is selected, server1 is removed.
    */
   assert (selects_server (client, primary_read_prefs, server0));

   if (pooled) {
      /* wait a second for scanner thread to remove server1 */
      start = bson_get_monotonic_time ();
      while (get_node (client->topology->scanner,
                       mock_server_get_host_and_port (server1)))
      {
         assert (bson_get_monotonic_time () - start < 1000000);
      }

   } else {
      assert (!get_node (client->topology->scanner,
                         mock_server_get_host_and_port (server1)));
   }

   mongoc_read_prefs_destroy (primary_read_prefs);

   if (pooled) {
      mongoc_client_pool_push (pool, client);
      mongoc_client_pool_destroy (pool);
   } else {
      mongoc_client_destroy (client);
   }

   mongoc_uri_destroy (uri);
   bson_free (uri_str);
   mock_server_destroy (server1);
   mock_server_destroy (server0);
}


static void
test_topology_reconcile_sharded_single (void)
{
   _test_topology_reconcile_sharded (false);
}


static void
test_topology_reconcile_sharded_pooled (void)
{
   _test_topology_reconcile_sharded (true);
}


void
test_topology_reconcile_install (TestSuite *suite)
{
   TestSuite_Add (suite, "/TOPOLOGY/reconcile/rs/pooled",
                  test_topology_reconcile_rs_pooled);
   TestSuite_Add (suite, "/TOPOLOGY/reconcile/rs/single",
                  test_topology_reconcile_rs_single);
   TestSuite_Add (suite, "/TOPOLOGY/reconcile/sharded/pooled",
                  test_topology_reconcile_sharded_pooled);
   TestSuite_Add (suite, "/TOPOLOGY/reconcile/sharded/single",
                  test_topology_reconcile_sharded_single);
}