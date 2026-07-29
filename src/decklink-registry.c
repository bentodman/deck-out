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

#include "decklink-registry.h"

#include <obs-module.h>
#include <pthread.h>
#include <string.h>

struct registry_entry {
	char *device_hash;
	char *owner_id;
};

static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct registry_entry *registry_entries = NULL;
static size_t registry_count = 0;

static bool entry_matches(const struct registry_entry *entry, const char *device_hash, const char *owner_id)
{
	return strcmp(entry->device_hash, device_hash) == 0 && strcmp(entry->owner_id, owner_id) == 0;
}

static bool registry_is_claimed_locked(const char *device_hash, const char *owner_id)
{
	for (size_t i = 0; i < registry_count; i++) {
		struct registry_entry *entry = &registry_entries[i];
		if (strcmp(entry->device_hash, device_hash) != 0)
			continue;
		if (owner_id && entry_matches(entry, device_hash, owner_id))
			continue;

		return true;
	}

	return false;
}

bool deckout_registry_is_claimed(const char *device_hash, const char *owner_id)
{
	bool claimed = false;

	if (!device_hash || !*device_hash)
		return false;

	pthread_mutex_lock(&registry_mutex);
	claimed = registry_is_claimed_locked(device_hash, owner_id);
	pthread_mutex_unlock(&registry_mutex);
	return claimed;
}

bool deckout_registry_claim(const char *device_hash, const char *owner_id)
{
	if (!device_hash || !*device_hash || !owner_id || !*owner_id)
		return false;

	pthread_mutex_lock(&registry_mutex);

	if (registry_is_claimed_locked(device_hash, owner_id)) {
		pthread_mutex_unlock(&registry_mutex);
		return false;
	}

	struct registry_entry entry = {
		.device_hash = bstrdup(device_hash),
		.owner_id = bstrdup(owner_id),
	};

	if (!entry.device_hash || !entry.owner_id) {
		bfree(entry.device_hash);
		bfree(entry.owner_id);
		pthread_mutex_unlock(&registry_mutex);
		return false;
	}

	struct registry_entry *new_entries =
		brealloc(registry_entries, (registry_count + 1) * sizeof(struct registry_entry));
	if (!new_entries) {
		bfree(entry.device_hash);
		bfree(entry.owner_id);
		pthread_mutex_unlock(&registry_mutex);
		return false;
	}

	registry_entries = new_entries;
	registry_entries[registry_count++] = entry;

	pthread_mutex_unlock(&registry_mutex);
	return true;
}

void deckout_registry_release(const char *device_hash, const char *owner_id)
{
	if (!device_hash || !*device_hash || !owner_id || !*owner_id)
		return;

	pthread_mutex_lock(&registry_mutex);

	for (size_t i = 0; i < registry_count; i++) {
		struct registry_entry *entry = &registry_entries[i];
		if (!entry_matches(entry, device_hash, owner_id))
			continue;

		bfree(entry->device_hash);
		bfree(entry->owner_id);
		registry_count--;

		if (i < registry_count)
			registry_entries[i] = registry_entries[registry_count];

		break;
	}

	pthread_mutex_unlock(&registry_mutex);
}
