#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

#define PP_AP_PASSWORD "pocketplan"

esp_err_t pp_net_start(void);

bool pp_net_sta_connected(void);
const char *pp_net_ap_ssid(void);
void pp_net_ap_ip(char *out, size_t out_size);
void pp_net_sta_ip(char *out, size_t out_size);

/* Persist new credentials in NVS and (re)connect the station. */
esp_err_t pp_net_set_credentials(const char *ssid, const char *password);
/* Currently stored SSID ("" when the device has never been provisioned). */
const char *pp_net_saved_ssid(void);
