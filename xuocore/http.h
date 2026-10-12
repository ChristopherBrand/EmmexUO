// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2020 Danny Angelo Carminati Grein

#pragma once

#include <stdint.h>
#include <string>
#include <vector>

void http_init();
void http_shutdown();
void http_set_user_agent(const char *name);
bool http_get_binary(const char *url, const uint8_t *buf, size_t *size);
bool http_get_binary(const char *url, std::vector<uint8_t> &data);
bool http_get_string(const char *url, std::string &data);
bool http_get_file(const char *url, const char *filename);
// Like http_get_file, reporting progress; returning false from the callback cancels.
typedef bool (*http_progress_fn)(void *user, uint64_t now, uint64_t total);
bool http_get_file(const char *url, const char *filename, http_progress_fn progress, void *user);
std::string http_urlencode(std::string str);