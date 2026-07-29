/*
Deck Out
Copyright (C) 2026 Ben Todman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool deckout_registry_claim(const char *device_hash, const char *owner_id);
void deckout_registry_release(const char *device_hash, const char *owner_id);
bool deckout_registry_is_claimed(const char *device_hash, const char *owner_id);

#ifdef __cplusplus
}
#endif
