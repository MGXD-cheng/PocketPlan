#pragma once

#include <esp_err.h>

/* WebDAV + provisioning HTTP server on port 80. */
esp_err_t pp_webdav_start(void);
