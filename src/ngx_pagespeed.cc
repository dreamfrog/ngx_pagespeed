/*
 * Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Usage:
 *   server {
 *     pagespeed    on|off;
 *   }
 */

extern "C" {
  #include <ngx_config.h>
  #include <ngx_core.h>
  #include <ngx_http.h>
}

#include <unistd.h>

#include "ngx_rewrite_driver_factory.h"
#include "ngx_base_fetch.h"

#include "net/instaweb/automatic/public/proxy_fetch.h"
#include "net/instaweb/rewriter/public/furious_matcher.h"
#include "net/instaweb/rewriter/public/process_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/public/version.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/google_message_handler.h"

extern ngx_module_t ngx_pagespeed;

// Hacks for debugging.
#define DBG(r, args...)                                       \
  ngx_log_error(NGX_LOG_ALERT, (r)->connection->log, 0, args)
#define CDBG(cf, args...)                                     \
  ngx_conf_log_error(NGX_LOG_ALERT, cf, 0, args)

typedef struct {
  ngx_flag_t active;
  ngx_str_t cache_dir;
  net_instaweb::NgxRewriteDriverFactory* driver_factory;
  net_instaweb::ServerContext* server_context;
  net_instaweb::ProxyFetchFactory* proxy_fetch_factory;
  net_instaweb::MessageHandler* handler;
} ngx_http_pagespeed_srv_conf_t;

typedef struct {
  ngx_http_pagespeed_srv_conf_t* cfg;
  net_instaweb::RewriteDriver* driver;
  net_instaweb::ProxyFetch* proxy_fetch;
  net_instaweb::NgxBaseFetch* base_fetch;
  bool data_received;
  int pipe_fd;
  ngx_connection_t* pagespeed_connection;
  ngx_http_request_t* r;
} ngx_http_pagespeed_request_ctx_t;

static ngx_command_t ngx_http_pagespeed_commands[] = {
  { ngx_string("pagespeed"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_flag_slot,
    NGX_HTTP_SRV_CONF_OFFSET,
    offsetof(ngx_http_pagespeed_srv_conf_t, active),
    NULL },

  { ngx_string("pagespeed_cache"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_SRV_CONF_OFFSET,
    offsetof(ngx_http_pagespeed_srv_conf_t, cache_dir),
    NULL },

  ngx_null_command
};

static ngx_int_t
ngx_http_pagespeed_body_filter(ngx_http_request_t* r, ngx_chain_t* in);

static StringPiece
ngx_http_pagespeed_str_to_string_piece(ngx_str_t* s) {
  return StringPiece(reinterpret_cast<char*>(s->data), s->len);
}

static void*
ngx_http_pagespeed_create_srv_conf(ngx_conf_t* cf)
{
  ngx_http_pagespeed_srv_conf_t* conf;

  conf = static_cast<ngx_http_pagespeed_srv_conf_t*>(
      ngx_pcalloc(cf->pool, sizeof(ngx_http_pagespeed_srv_conf_t)));
  if (conf == NULL) {
    return NGX_CONF_ERROR;
  }
  conf->active = NGX_CONF_UNSET;

  // set by ngx_pcalloc():
  //   conf->cache_dir = { 0, NULL };
  //   conf->driver_factory = NULL;
  //   conf->server_context = NULL;

  return conf;
}

static char*
ngx_http_pagespeed_merge_srv_conf(ngx_conf_t* cf, void* parent, void* child)
{
  ngx_http_pagespeed_srv_conf_t* prev =
      static_cast<ngx_http_pagespeed_srv_conf_t*>(parent);
  ngx_http_pagespeed_srv_conf_t* conf =
      static_cast<ngx_http_pagespeed_srv_conf_t*>(child);

  ngx_conf_merge_value(conf->active, prev->active, 0);  // Default off.
  ngx_conf_merge_str_value(conf->cache_dir, prev->cache_dir, "");

  return NGX_CONF_OK;
}

static ngx_http_output_header_filter_pt ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt ngx_http_next_body_filter;

static void
ngx_http_pagespeed_release_request_context(
    ngx_http_request_t* r, ngx_http_pagespeed_request_ctx_t* ctx) {

  DBG(r, "releasing request context");

  // release request context
  ngx_http_set_ctx(r, NULL, ngx_pagespeed);

  // BaseFetch doesn't delete itself
  delete ctx->base_fetch;

  // Stop watching the pipe.
  close(ctx->pipe_fd);

  // the proxy fetch deleted itself when we called Done()
  delete ctx;
}


static GoogleString
ngx_http_pagespeed_determine_url(ngx_http_request_t* r) {
  // Based on ngx_http_variable_scheme.
  bool is_https = false;
#if (NGX_HTTP_SSL)
  is_https = r->connection->ssl;
#endif

  // Based on ngx_http_variable_server_port.
  ngx_uint_t port;
  bool have_port = false;
#if (NGX_HAVE_INET6)
  if (r->connection->local_sockaddr->sa_family == AF_INET6) {
    port = ntohs(reinterpret_cast<struct sockaddr_in6*>(
        r->connection->local_sockaddr)->sin6_port);
    have_port= true;
  }
#endif
  if (!have_port) {
    port = ntohs(reinterpret_cast<struct sockaddr_in*>(
        r->connection->local_sockaddr)->sin_port);
  }

  GoogleString port_string;
  if ((is_https && port == 443) || (!is_https && port == 80)) {
    // No port specifier needed for requests on default ports.
    port_string = "";
  } else {
    port_string = net_instaweb::StrCat(
        ":", net_instaweb::IntegerToString(port));
  }

  StringPiece host =
      ngx_http_pagespeed_str_to_string_piece(&r->headers_in.server);
  if (host.size() == 0) {
    // If host is unspecified, perhaps because of a pure HTTP 1.0 "GET /path",
    // fall back to server IP address.  Based on ngx_http_variable_server_addr.
    ngx_str_t  s;
    u_char addr[NGX_SOCKADDR_STRLEN];
    s.len = NGX_SOCKADDR_STRLEN;
    s.data = addr;
    ngx_int_t rc = ngx_connection_local_sockaddr(r->connection, &s, 0);
    if (rc != NGX_OK) {
      s.len = 0;
    }
    host = ngx_http_pagespeed_str_to_string_piece(&s);
  }

  return net_instaweb::StrCat(
      is_https ? "https://" : "http://", host, port_string,
      ngx_http_pagespeed_str_to_string_piece(&r->unparsed_uri));
}

// Get the context for this request.  ngx_http_pagespeed_create_request_context
// should already have been called to create it.
static ngx_http_pagespeed_request_ctx_t*
ngx_http_pagespeed_get_request_context(ngx_http_request_t* r) {
  ngx_http_pagespeed_request_ctx_t* ctx =
      static_cast<ngx_http_pagespeed_request_ctx_t*>(
          ngx_http_get_module_ctx(r, ngx_pagespeed));
  if (ctx != NULL && ctx->r != r) {
    DBG(r, "ngx_http_pagespeed_get_request_context: "
        "Broken request pointer");
    return NULL;
  }
  return ctx;
}

// Initialize the ngx_http_pagespeed_srv_conf_t by allocating and configuring
// the long-lived objects it contains.
// TODO(jefftk): This shouldn't be done on the first request but instead
// when we're done processing the configuration.
static void
ngx_http_pagespeed_initialize_server_context(
    ngx_http_pagespeed_srv_conf_t* cfg) {
  net_instaweb::NgxRewriteDriverFactory::Initialize();
  // TODO(jefftk): We should call NgxRewriteDriverFactory::Terminate() when
  // we're done with it.

  // TODO(jefftk): Replace this with a real message handler that logs errors
  // somewhere useful.
  cfg->handler = new net_instaweb::GoogleMessageHandler();

  cfg->driver_factory = new net_instaweb::NgxRewriteDriverFactory();
  cfg->driver_factory->set_filename_prefix(
      ngx_http_pagespeed_str_to_string_piece(&cfg->cache_dir));
  cfg->server_context = cfg->driver_factory->CreateServerContext();
  cfg->proxy_fetch_factory =
      new net_instaweb::ProxyFetchFactory(cfg->server_context);

  // Turn on some filters so we can see if this is working.  These filters are
  // specifically chosen as ones that don't make requests for subresources or do
  // work that needs to complete asynchronously.  They should be fast enough to
  // run in the line of the request.
  net_instaweb::RewriteOptions* global_options =
      cfg->server_context->global_options();
  //global_options->SetRewriteLevel(net_instaweb::RewriteOptions::kPassThrough);
  global_options->SetRewriteLevel(net_instaweb::RewriteOptions::kCoreFilters);
  global_options->EnableFiltersByCommaSeparatedList(
      "collapse_whitespace,remove_comments,remove_quotes", cfg->handler);
}

// Returns:
//   NGX_OK: pagespeed is done, request complete
//   NGX_AGAIN: pagespeed still working, needs to be called again later
//   NGX_ERROR: error
static ngx_int_t
ngx_http_pagespeed_update(ngx_http_request_t* r,
                          ngx_http_pagespeed_request_ctx_t* ctx,
                          ngx_event_t* ev) {

  fprintf(stderr, "processing event, ctx=%p for r=%p uri=%*s\n",
          ctx, r, static_cast<int>(r->uri.len), r->uri.data);

  int rc;
  // char chr;
  //rc = read(ctx->pipe_fd, &chr, 1);
  //if (rc != 1) {
  //  perror("ngx_http_pagespeed_connection_read_handler");
  //  return NGX_ERROR;
  // }
  //fprintf(stderr, "Got '%c'\n", chr);

  // Get any finished data back
  ngx_chain_t* in;
  rc = ctx->base_fetch->CollectAccumulatedWrites(&in);
  if (rc != NGX_OK) {
    DBG(r, "problem with CollectAccumulatedWrites");
    return rc;
  }

  if (in == NULL) {
    DBG(r, "got null from pagespeed");
  } else {
    DBG(r, "got '%*s' from pagespeed",
        in->buf->end - in->buf->pos, in->buf->pos);
  }

  return ngx_http_next_body_filter(r, in);
}

static void
ngx_http_pagespeed_connection_read_handler(ngx_event_t* ev) {
  if (ev == NULL) {
    fprintf(stderr, "ev is null\n");
    return;
  }

  ngx_connection_t* c = static_cast<ngx_connection_t*>(ev->data);
  if (c == NULL) {
    fprintf(stderr, "c is null\n");
    ngx_del_event(ev, NGX_READ_EVENT, 0);
    return;
  }

  ngx_http_pagespeed_request_ctx_t* ctx =
      static_cast<ngx_http_pagespeed_request_ctx_t*>(c->data);
  if (ctx == NULL) {
    fprintf(stderr, "ctx is null\n");
    ngx_del_event(ev, NGX_READ_EVENT, 0);
    return;
  }

  ngx_http_request_t* r = ctx->r;
  if (r == NULL) {
    fprintf(stderr, "r is null\n");
    ngx_del_event(ev, NGX_READ_EVENT, 0);
    return;
  }

  int rc = ngx_http_pagespeed_update(r, ctx, ev);
  if (rc == NGX_OK) {
    DBG(r, "ok");
    // request complete
    ngx_del_event(ev, NGX_READ_EVENT, 0);
    ngx_http_pagespeed_release_request_context(r, ctx);
    ngx_http_finalize_request(r, NGX_DONE);
  } else if (rc == NGX_ERROR) {
    DBG(r, "error");
    ngx_del_event(ev, NGX_READ_EVENT, 0);
    ngx_http_finalize_request(r, NGX_ERROR);
  } else if (rc == NGX_AGAIN) { 
    DBG(r, "again");
    // request needs more work by pagespeed
    rc = ngx_handle_read_event(ev, 0);
    if (rc != NGX_OK) {
      DBG(r, "ngx_handle_read_event failed");
    }
  } else {
    DBG(r, "Got unknown return code %d from ngx_http_pagespeed_update", rc);
  }
}

// Set us up for processing a request.  When the request finishes, call
// ngx_http_pagespeed_release_request_context.
static ngx_int_t
ngx_http_pagespeed_create_request_context(ngx_http_request_t* r) {
  ngx_http_pagespeed_request_ctx_t* ctx =
      new ngx_http_pagespeed_request_ctx_t();

  fprintf(stderr, "created request context %p for %p uri=%*s\n",
          ctx, r, static_cast<int>(r->uri.len), r->uri.data);

  ctx->r = r;

  ctx->cfg = static_cast<ngx_http_pagespeed_srv_conf_t*>(
      ngx_http_get_module_srv_conf(r, ngx_pagespeed));

  int file_descriptors[2];
  int rc = pipe2(file_descriptors, O_NONBLOCK);
  if (rc != 0) {
    ngx_log_error(NGX_LOG_ERR, (r)->connection->log, 0, "pipe() failed");
    return NGX_ERROR;
  }
  fprintf(stderr, "pipe created: %d -> %d\n",
          file_descriptors[1], file_descriptors[0]);

  ctx->pipe_fd = file_descriptors[0];
  ctx->pagespeed_connection =
      ngx_get_connection(ctx->pipe_fd, r->connection->log);
  if (ctx->pagespeed_connection == NULL) {
    close(file_descriptors[0]);
    close(file_descriptors[1]);

    ngx_log_error(NGX_LOG_ERR, (r)->connection->log, 0,
                  "ngx_http_pagespeed_create_request_context: "
                  "no pagespeed connection.");
    return NGX_ERROR;
  }

  ctx->pagespeed_connection->data = ctx;

  ctx->pagespeed_connection->read->handler =
      ngx_http_pagespeed_connection_read_handler;

  ngx_add_event(ctx->pagespeed_connection->read, NGX_READ_EVENT, 0);

  // Deletes itself when HandleDone is called, which happens when we call Done()
  // on the proxy fetch below.
  ctx->base_fetch = new net_instaweb::NgxBaseFetch(r, file_descriptors[1]);

  if (ctx->cfg->driver_factory == NULL) {
    // This is the first request handled by this server block.
    ngx_http_pagespeed_initialize_server_context(ctx->cfg);
  }

  if (ctx->cfg->server_context == NULL) {
    ngx_log_error(NGX_LOG_ERR, (r)->connection->log, 0,
                  "ServerContext should have been initialized.");
    return NGX_ERROR;
  }

  GoogleString url_string = ngx_http_pagespeed_determine_url(r);
  net_instaweb::GoogleUrl request_url(url_string);

  // This code is based off of ProxyInterface::ProxyRequestCallback and
  // ProxyFetchFactory::StartNewProxyFetch

  // If the global options say we're running furious (the experiment framework)
  // then clone them into custom_options so we can manipulate custom options
  // without affecting the global options.
  net_instaweb::RewriteOptions* custom_options = NULL;
  net_instaweb::RewriteOptions* global_options =
      ctx->cfg->server_context->global_options();
  if (global_options->running_furious()) {
    custom_options = global_options->Clone();
    custom_options->set_need_to_store_experiment_data(
        ctx->cfg->server_context->furious_matcher()->ClassifyIntoExperiment(
            *ctx->base_fetch->request_headers(), custom_options));
  }

  // TODO(jefftk): port ProxyInterface::InitiatePropertyCacheLookup so that we
  // have the propery cache in nginx.

  // If we don't have custom options we can use NewRewriteDriver which reuses
  // rewrite drivers and so is faster because there's no wait to construct
  // them.  Otherwise we have to build a new one every time.
  if (custom_options == NULL) {
    ctx->driver = ctx->cfg->server_context->NewRewriteDriver();
  } else {
    // NewCustomRewriteDriver takes ownership of custom_options.
    ctx->driver = ctx->cfg->server_context->NewCustomRewriteDriver(
        custom_options);
  }
  ctx->driver->set_log_record(ctx->base_fetch->log_record());

  // TODO(jefftk): FlushEarlyFlow would go here.

  // Will call StartParse etc.
  ctx->proxy_fetch = ctx->cfg->proxy_fetch_factory->CreateNewProxyFetch(
      url_string, ctx->base_fetch, ctx->driver,
      NULL /* property_callback */,
      NULL /* original_content_fetch */);

  ngx_http_set_ctx(r, ctx, ngx_pagespeed);
  return NGX_OK;
}

// Send each buffer in the chain to the proxy_fetch for optimization.
// Eventually it will make it's way, optimized, to base_fetch.
static void
ngx_http_pagespeed_send_to_pagespeed(
    ngx_http_request_t* r,
    ngx_http_pagespeed_request_ctx_t* ctx,
    ngx_chain_t* in) {

  ngx_chain_t* cur;
  int last_buf = 0;
  int last_in_chain = 0;
  for (cur = in; cur != NULL;) {
    last_buf = cur->buf->last_buf;
    last_in_chain = cur->buf->last_in_chain;

    ctx->proxy_fetch->Write(StringPiece(reinterpret_cast<char*>(cur->buf->pos),
                                        cur->buf->last - cur->buf->pos),
                            ctx->cfg->handler);

    ngx_chain_t* next_link = cur->next;

    // We're done with buffers as we pass them through, so free them and their
    // chain links as we go.
    ngx_pfree(r->pool, cur->buf);
    ngx_pfree(r->pool, cur);
    cur = next_link;
  }
  in = NULL;  // We freed all the buffers.

  if (last_buf) {
    ctx->proxy_fetch->Done(true /* success */);
  } else {
    // TODO(jefftk): Decide whether Flush() is warranted here.
    ctx->proxy_fetch->Flush(ctx->cfg->handler);
  }
}

static ngx_int_t
ngx_http_pagespeed_body_filter(ngx_http_request_t* r, ngx_chain_t* in)
{
  ngx_http_pagespeed_request_ctx_t* ctx =
      ngx_http_pagespeed_get_request_context(r);
  if (ctx == NULL) {
    return NGX_ERROR;
  }

  if (!ctx->data_received) {
    DBG(r, "initial buffer");
    // This is the first set of buffers we've got for this request.
    ctx->data_received = true;
    ctx->base_fetch->PopulateHeaders();  // TODO(jefftk): is this thread safe?
  }

  if (in != NULL) {
    // Send all input data to the proxy fetch.
    ngx_http_pagespeed_send_to_pagespeed(r, ctx, in);
  }

  DBG(r, "not finished until pagespeed returns");

  return NGX_AGAIN;
}

static ngx_int_t
ngx_http_pagespeed_header_filter(ngx_http_request_t* r)
{
  // We're modifying content below, so switch to 'Transfer-Encoding: chunked'
  // and calculate on the fly.
  ngx_http_clear_content_length(r);

  r->filter_need_in_memory = 1;

  int rc = ngx_http_pagespeed_create_request_context(r);
  if (rc != NGX_OK) {
    return rc;
  }

  return ngx_http_next_header_filter(r);
}

static ngx_int_t
ngx_http_pagespeed_init(ngx_conf_t* cf)
{
  ngx_http_pagespeed_srv_conf_t* pagespeed_config;
  pagespeed_config = static_cast<ngx_http_pagespeed_srv_conf_t*>(
    ngx_http_conf_get_module_srv_conf(cf, ngx_pagespeed));

  if (pagespeed_config->active) {
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_pagespeed_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_pagespeed_body_filter;
  }

  return NGX_OK;
}

static ngx_http_module_t ngx_http_pagespeed_module = {
  NULL,  // preconfiguration
  ngx_http_pagespeed_init,  // postconfiguration

  NULL,  // create main configuration
  NULL,  // init main configuration

  ngx_http_pagespeed_create_srv_conf,  // create server configuration
  ngx_http_pagespeed_merge_srv_conf,  // merge server configuration

  NULL,  // create location configuration
  NULL  // merge location configuration
};

ngx_module_t ngx_pagespeed = {
  NGX_MODULE_V1,
  &ngx_http_pagespeed_module,
  ngx_http_pagespeed_commands,
  NGX_HTTP_MODULE,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NGX_MODULE_V1_PADDING
};

