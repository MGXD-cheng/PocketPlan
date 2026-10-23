#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

/* Registers the REST endpoints and the embedded web app. Must be called after
 * httpd_start() and BEFORE the WebDAV wildcard handlers, which would otherwise
 * swallow every GET. */
esp_err_t pp_webapi_register(httpd_handle_t server);
