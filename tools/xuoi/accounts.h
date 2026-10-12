// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2020 Danny Angelo Carminati Grein

#pragma once

struct ui_model;

extern const char *client_types[];
extern const char *client_types_cfg[];

void load_accounts();
void write_accounts(void *fp);
void ui_accounts(ui_model &m);
bool accounts_empty();
// Adds a profile (or updates the one with the same name) and selects it in the accounts view.
int account_add(const char *profile, const char *loginserver, const char *clientversion, const char *datapath);
