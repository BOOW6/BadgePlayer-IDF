#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t audio_player_init(void);
esp_err_t audio_player_play(const char *url);
esp_err_t audio_player_stop(void);
bool audio_player_is_playing(void);
