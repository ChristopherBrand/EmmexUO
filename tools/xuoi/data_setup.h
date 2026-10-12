// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

struct ui_model;

void ui_data_setup(ui_model &m);
void data_setup_shutdown(); // cancels and joins a running download
