#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

/* Minimal chat client for any OpenAI compatible endpoint (DeepSeek by
 * default). Keeps a short rolling history so the conversation has context. */
#define PP_AI_HISTORY 8

esp_err_t pp_ai_init(void);
bool pp_ai_configured(void);

/* Blocking call, run it from a worker task. Returns a malloc()ed reply. */
char *pp_ai_send(const char *message);

/* True when the last pp_ai_send() returned an error string, not a model reply. */
bool pp_ai_last_error(void);

/* Push one exchange into the rolling history (used when the web UI talks to
 * the model as well). The history is mirrored to ai_chat.json (internal flash
 * or the SD card, see pp_data_on_sd()) so the conversation survives a reboot. */
void pp_ai_history_push(const char *role, const char *content);
void pp_ai_history_clear(void);

/* Read only view for the chat page and GET /api/ai. The pointers stay valid
 * until the next push or clear. */
int pp_ai_history_count(void);
const char *pp_ai_history_role(int index);
const char *pp_ai_history_text(int index);