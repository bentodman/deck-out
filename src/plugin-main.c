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

#include <obs-module.h>
#include <plugin-support.h>

#include "decklink-filter.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static void deckout_log_build_info(void)
{
	obs_log(LOG_INFO,
		"Build identity: version=%s build=%s git=%s config=%s compiled=%s %s",
		PLUGIN_VERSION, PLUGIN_BUILD_NUMBER, PLUGIN_GIT_COMMIT, PLUGIN_BUILD_TYPE, __DATE__, __TIME__);
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded");
	deckout_log_build_info();
	return true;
}

void obs_module_post_load(void)
{
	const bool decklink_available = obs_get_output_properties("decklink_output") != NULL;
	deckout_set_decklink_output_available(decklink_available);

	if (!decklink_available) {
		obs_log(LOG_WARNING,
			"DeckLink output is not available. Install OBS with DeckLink support to use this plugin.");
		return;
	}

	obs_register_source(&deckout_filter_info);
	obs_log(LOG_INFO, "DeckLink output filter registered (version=%s build=%s git=%s)", PLUGIN_VERSION,
		PLUGIN_BUILD_NUMBER, PLUGIN_GIT_COMMIT);
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
