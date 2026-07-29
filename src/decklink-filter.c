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

#include "decklink-filter.h"
#include "decklink-registry.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <obs-properties.h>
#include <graphics/vec2.h>
#include <util/darray.h>
#include <util/platform.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define DECKLINK_OUTPUT_ID "decklink_output"
#define DECKLINK_INPUT_ID "decklink-input"
#define PROP_DEVICE_HASH "device_hash"
#define PROP_AUTO_START "auto_start"
#define PROP_AUDIO_ROUTING "deckout_audio_routing"
#define PROP_AUDIO_SOURCE "deckout_audio_source"
#define PROP_AUDIO_TAP "deckout_audio_tap"
#define PROP_STATUS "deckout_status"
#define PROP_START_STOP "deckout_start_stop"
#define PROP_MODE_ID "mode_id"
#define PROP_MODE_NAME "mode_name"
#define PROP_KEYER "keyer"
#define PROP_FORCE_SDR "force_sdr"
/* Legacy setting; migrated into PROP_AUDIO_ROUTING on update. */
#define PROP_MUTE_AUDIO_LEGACY "deckout_mute_audio"

#define AUDIO_ROUTE_MASTER 0
#define AUDIO_ROUTE_LOCAL 1
#define AUDIO_ROUTE_MUTE 2
#define AUDIO_ROUTE_CUSTOM 3
#define AUDIO_TAP_POST 0
#define AUDIO_TAP_PRE 1
#define HOTPLUG_CHECK_INTERVAL_SEC 1.0f
#define DECKOUT_AUDIO_MIX_FRAMES (AUDIO_OUTPUT_FRAMES * 8)

enum deckout_status {
	DECKOUT_STATUS_IDLE = 0,
	DECKOUT_STATUS_STARTING,
	DECKOUT_STATUS_ACTIVE,
	DECKOUT_STATUS_INVALID_CONFIG,
	DECKOUT_STATUS_DEVICE_UNAVAILABLE,
	DECKOUT_STATUS_WAITING_DEVICE,
	DECKOUT_STATUS_UNSUPPORTED_MODE,
	DECKOUT_STATUS_CONFLICT,
	DECKOUT_STATUS_INPUT_CONFLICT,
};

enum deckout_audio_routing {
	DECKOUT_AUDIO_ROUTE_MASTER = AUDIO_ROUTE_MASTER,
	DECKOUT_AUDIO_ROUTE_LOCAL = AUDIO_ROUTE_LOCAL,
	DECKOUT_AUDIO_ROUTE_MUTE = AUDIO_ROUTE_MUTE,
	DECKOUT_AUDIO_ROUTE_CUSTOM = AUDIO_ROUTE_CUSTOM,
};

struct deckout_audio {
	audio_t *audio;
	pthread_mutex_t mutex;
	bool mutex_initialized;
	float *mix[MAX_AUDIO_CHANNELS];
	size_t mix_frames;
	size_t channels;
	uint32_t sample_rate;
	bool pre_fader;
	obs_source_t *root;
	DARRAY(obs_source_t *) captures;
};

struct deckout_filter {
	obs_source_t *context;
	obs_output_t *output;
	obs_canvas_t *canvas;
	obs_scene_t *scale_scene;
	obs_source_t *parent;
	pthread_mutex_t state_mutex;
	bool state_mutex_initialized;
	char owner_id[64];
	char *device_hash;
	long long active_mode_id;
	long long active_keyer;
	bool active_force_sdr;
	bool waiting_hotplug;
	bool recover_manual_start;
	float hotplug_check_elapsed;
	bool active;
	bool starting;
	bool stopping;
	bool auto_start;
	bool manual_start;
	bool user_stopped; /* manual Stop suppresses auto_start until Start or auto_start re-enabled */
	bool pending_start;
	bool properties_refresh_pending;
	bool showing_ref;
	bool audio_showing_ref;
	enum deckout_audio_routing audio_routing;
	bool audio_pre_fader;
	char *audio_source_uuid;
	enum deckout_status status;
	struct deckout_audio local_audio;
};

static bool g_frontend_ready = false;
static bool g_shutting_down = false;
static bool g_decklink_output_available = false;

static void deckout_audio_capture(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted);

static const char *deckout_resolve_mode_name(obs_data_t *settings, long long mode_id, char *mode_buf, size_t mode_buf_size)
{
	if (!settings || !mode_buf || mode_buf_size == 0)
		return NULL;

	mode_buf[0] = '\0';
	obs_properties_t *props = obs_get_output_properties(DECKLINK_OUTPUT_ID);
	if (!props)
		return NULL;

	obs_property_t *device_prop = obs_properties_get(props, PROP_DEVICE_HASH);
	if (device_prop)
		obs_property_modified(device_prop, settings);

	obs_property_t *mode_prop = obs_properties_get(props, PROP_MODE_ID);
	if (mode_prop) {
		const size_t count = obs_property_list_item_count(mode_prop);
		for (size_t i = 0; i < count; i++) {
			if (obs_property_list_item_int(mode_prop, i) == mode_id) {
				const char *name = obs_property_list_item_name(mode_prop, i);
				if (name && *name) {
					snprintf(mode_buf, mode_buf_size, "%s", name);
					break;
				}
			}
		}
	}

	obs_properties_destroy(props);
	return mode_buf[0] ? mode_buf : NULL;
}

static void deckout_log_mode_map(obs_data_t *settings, long long selected_mode_id)
{
	if (!settings)
		return;

	obs_properties_t *props = obs_get_output_properties(DECKLINK_OUTPUT_ID);
	if (!props)
		return;

	obs_property_t *device_prop = obs_properties_get(props, PROP_DEVICE_HASH);
	if (device_prop)
		obs_property_modified(device_prop, settings);

	obs_property_t *mode_prop = obs_properties_get(props, PROP_MODE_ID);
	if (!mode_prop) {
		obs_properties_destroy(props);
		return;
	}

	const size_t count = obs_property_list_item_count(mode_prop);
	obs_log(LOG_INFO, "[deck-out] Mode map for selected device (%zu entries, selected_id=%lld):", count,
		selected_mode_id);
	for (size_t i = 0; i < count; i++) {
		const long long id = obs_property_list_item_int(mode_prop, i);
		const char *name = obs_property_list_item_name(mode_prop, i);
		obs_log(LOG_INFO, "[deck-out]   %s id=%lld name='%s'", id == selected_mode_id ? ">" : " ", id,
			name && *name ? name : "(unnamed)");
	}

	obs_properties_destroy(props);
}

struct decklink_input_check {
	const char *device_hash;
	bool found;
};

static bool deckout_enum_decklink_input(void *data, obs_source_t *source)
{
	struct decklink_input_check *check = data;

	if (strcmp(obs_source_get_id(source), DECKLINK_INPUT_ID) != 0)
		return true;

	if (!obs_source_active(source))
		return true;

	obs_data_t *input_settings = obs_source_get_settings(source);
	const char *input_hash = obs_data_get_string(input_settings, PROP_DEVICE_HASH);
	const bool match = input_hash && *input_hash && strcmp(input_hash, check->device_hash) == 0;
	obs_data_release(input_settings);

	if (match) {
		obs_log(LOG_WARNING, "[deck-out] Active DeckLink input '%s' uses the same device '%s'",
			obs_source_get_name(source), check->device_hash);
		check->found = true;
	}

	return true;
}

static bool deckout_device_has_active_input(const char *device_hash)
{
	struct decklink_input_check check = {
		.device_hash = device_hash,
		.found = false,
	};

	obs_enum_sources(deckout_enum_decklink_input, &check);
	return check.found;
}

struct decklink_output_check {
	const char *device_hash;
	const obs_output_t *self;
	const char *conflict_name;
	bool found;
};

static bool deckout_enum_decklink_output(void *data, obs_output_t *output)
{
	struct decklink_output_check *check = data;

	if (output == check->self)
		return true;

	if (strcmp(obs_output_get_id(output), DECKLINK_OUTPUT_ID) != 0)
		return true;

	if (!obs_output_active(output))
		return true;

	obs_data_t *out_settings = obs_output_get_settings(output);
	const char *out_hash = obs_data_get_string(out_settings, PROP_DEVICE_HASH);
	const bool match = out_hash && *out_hash && strcmp(out_hash, check->device_hash) == 0;
	obs_data_release(out_settings);

	if (!match)
		return true;

	check->found = true;
	check->conflict_name = obs_output_get_name(output);
	obs_log(LOG_WARNING, "[deck-out] Active DeckLink output '%s' already uses device '%s'",
		check->conflict_name ? check->conflict_name : "(unnamed)", check->device_hash);
	return false;
}

static bool deckout_device_has_active_output(const char *device_hash, const obs_output_t *self)
{
	struct decklink_output_check check = {
		.device_hash = device_hash,
		.self = self,
		.conflict_name = NULL,
		.found = false,
	};

	obs_enum_outputs(deckout_enum_decklink_output, &check);
	return check.found;
}

static bool deckout_device_is_present(const char *device_hash)
{
	if (!device_hash || !*device_hash)
		return false;

	obs_properties_t *props = obs_get_output_properties(DECKLINK_OUTPUT_ID);
	if (!props)
		return false;

	obs_property_t *device_prop = obs_properties_get(props, PROP_DEVICE_HASH);
	bool found = false;

	if (device_prop) {
		const size_t count = obs_property_list_item_count(device_prop);
		for (size_t i = 0; i < count; i++) {
			const char *hash = obs_property_list_item_string(device_prop, i);
			if (hash && strcmp(hash, device_hash) == 0) {
				found = true;
				break;
			}
		}
	}

	obs_properties_destroy(props);
	return found;
}

static obs_data_t *deckout_build_output_settings(obs_data_t *filter_settings)
{
	obs_data_t *output_settings = obs_data_create();

	obs_data_set_string(output_settings, PROP_DEVICE_HASH, obs_data_get_string(filter_settings, PROP_DEVICE_HASH));
	obs_data_set_int(output_settings, PROP_MODE_ID, obs_data_get_int(filter_settings, PROP_MODE_ID));
	obs_data_set_int(output_settings, PROP_KEYER, obs_data_get_int(filter_settings, PROP_KEYER));
	obs_data_set_bool(output_settings, PROP_FORCE_SDR, obs_data_get_bool(filter_settings, PROP_FORCE_SDR));

	const char *mode_name = obs_data_get_string(filter_settings, PROP_MODE_NAME);
	if (mode_name && *mode_name)
		obs_data_set_string(output_settings, PROP_MODE_NAME, mode_name);

	return output_settings;
}

static void deckout_log_demanded_mode(struct deckout_filter *filter, obs_data_t *settings, long long mode_id)
{
	char resolved_mode[256];
	const char *mode_name = obs_data_get_string(settings, PROP_MODE_NAME);
	const char *resolved = deckout_resolve_mode_name(settings, mode_id, resolved_mode, sizeof(resolved_mode));
	struct obs_video_info ovi;
	const bool have_ovi = obs_get_video_info(&ovi);

	obs_log(LOG_INFO,
		"[deck-out] Demanded mode: id=%lld, mode_name_setting='%s', resolved_mode='%s', demanded_canvas=%ux%u @ %u/%u",
		mode_id, mode_name && *mode_name ? mode_name : "(unset)", resolved ? resolved : "(unresolved)",
		have_ovi ? ovi.output_width : 0, have_ovi ? ovi.output_height : 0, have_ovi ? ovi.fps_num : 0,
		have_ovi ? ovi.fps_den : 0);
	deckout_log_mode_map(settings, mode_id);

	UNUSED_PARAMETER(filter);
}

static void deckout_log_active_output_mode(struct deckout_filter *filter)
{
	if (!filter || !filter->output)
		return;

	obs_data_t *out_settings = obs_output_get_settings(filter->output);
	const long long active_mode_id = out_settings ? obs_data_get_int(out_settings, PROP_MODE_ID) : 0;
	char resolved_mode[256];
	const char *resolved = out_settings ? deckout_resolve_mode_name(out_settings, active_mode_id, resolved_mode,
									 sizeof(resolved_mode))
					    : NULL;

	const struct video_scale_info *conv = obs_output_get_video_conversion(filter->output);
	const uint32_t conv_width = conv ? conv->width : 0;
	const uint32_t conv_height = conv ? conv->height : 0;

	struct obs_video_info cvi;
	const bool have_cvi = filter->canvas && obs_canvas_get_video_info(filter->canvas, &cvi);
	obs_log(LOG_INFO,
		"[deck-out] Active output mode: id=%lld, resolved_mode='%s', output_conversion=%ux%u, active_canvas=%ux%u @ %u/%u",
		active_mode_id, resolved ? resolved : "(unresolved)", conv_width, conv_height, have_cvi ? cvi.output_width : 0,
		have_cvi ? cvi.output_height : 0, have_cvi ? cvi.fps_num : 0, have_cvi ? cvi.fps_den : 0);

	if (out_settings)
		obs_data_release(out_settings);
}

static const char *status_to_string(enum deckout_status status)
{
	switch (status) {
	case DECKOUT_STATUS_IDLE:
		return obs_module_text("Deckout.Status.Idle");
	case DECKOUT_STATUS_STARTING:
		return obs_module_text("Deckout.Status.Starting");
	case DECKOUT_STATUS_ACTIVE:
		return obs_module_text("Deckout.Status.Active");
	case DECKOUT_STATUS_INVALID_CONFIG:
		return obs_module_text("Deckout.Status.InvalidConfig");
	case DECKOUT_STATUS_DEVICE_UNAVAILABLE:
		return obs_module_text("Deckout.Status.DeviceUnavailable");
	case DECKOUT_STATUS_WAITING_DEVICE:
		return obs_module_text("Deckout.Status.WaitingDevice");
	case DECKOUT_STATUS_UNSUPPORTED_MODE:
		return obs_module_text("Deckout.Status.UnsupportedMode");
	case DECKOUT_STATUS_CONFLICT:
		return obs_module_text("Deckout.Status.Conflict");
	case DECKOUT_STATUS_INPUT_CONFLICT:
		return obs_module_text("Deckout.Status.InputConflict");
	}

	return obs_module_text("Deckout.Status.Idle");
}

static enum obs_text_info_type status_to_info_type(enum deckout_status status)
{
	switch (status) {
	case DECKOUT_STATUS_ACTIVE:
		return OBS_TEXT_INFO_NORMAL;
	case DECKOUT_STATUS_CONFLICT:
	case DECKOUT_STATUS_INPUT_CONFLICT:
	case DECKOUT_STATUS_DEVICE_UNAVAILABLE:
	case DECKOUT_STATUS_UNSUPPORTED_MODE:
	case DECKOUT_STATUS_INVALID_CONFIG:
		return OBS_TEXT_INFO_ERROR;
	case DECKOUT_STATUS_WAITING_DEVICE:
	case DECKOUT_STATUS_STARTING:
		return OBS_TEXT_INFO_WARNING;
	default:
		return OBS_TEXT_INFO_NORMAL;
	}
}

static void deckout_filter_set_status(struct deckout_filter *filter, enum deckout_status status)
{
	if (!filter || filter->status == status)
		return;

	filter->status = status;
}

static inline void deckout_filter_lock(struct deckout_filter *filter)
{
	if (filter && filter->state_mutex_initialized)
		pthread_mutex_lock(&filter->state_mutex);
}

static inline void deckout_filter_unlock(struct deckout_filter *filter)
{
	if (filter && filter->state_mutex_initialized)
		pthread_mutex_unlock(&filter->state_mutex);
}

static bool deckout_parent_is_scene(obs_source_t *parent)
{
	return parent && obs_source_get_type(parent) == OBS_SOURCE_TYPE_SCENE;
}

static void deckout_audio_free(struct deckout_audio *audio)
{
	if (!audio)
		return;

	for (size_t i = 0; i < audio->captures.num; i++) {
		obs_source_t *source = audio->captures.array[i];
		if (!source)
			continue;
		obs_source_remove_audio_capture_callback(source, deckout_audio_capture, audio);
		obs_source_release(source);
	}
	da_free(audio->captures);

	if (audio->root) {
		obs_source_release(audio->root);
		audio->root = NULL;
	}

	if (audio->audio) {
		audio_output_close(audio->audio);
		audio->audio = NULL;
	}

	if (audio->mutex_initialized) {
		pthread_mutex_lock(&audio->mutex);
		for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			bfree(audio->mix[i]);
			audio->mix[i] = NULL;
		}
		audio->mix_frames = 0;
		pthread_mutex_unlock(&audio->mutex);
		pthread_mutex_destroy(&audio->mutex);
		audio->mutex_initialized = false;
	}
	memset(audio, 0, sizeof(*audio));
}

static void deckout_audio_capture(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	struct deckout_audio *audio = param;

	if (!audio || !audio_data || !audio_data->frames)
		return;

	const bool pre_fader = audio->pre_fader;
	if (!pre_fader && (muted || (source && obs_source_muted(source))))
		return;

	size_t frames = audio_data->frames;
	if (frames > DECKOUT_AUDIO_MIX_FRAMES)
		frames = DECKOUT_AUDIO_MIX_FRAMES;

	float vol = 1.0f;
	if (!pre_fader && source) {
		vol = obs_source_get_volume(source);
		if (vol <= 0.0f)
			return;
	}

	pthread_mutex_lock(&audio->mutex);
	if (audio->mix_frames < frames) {
		for (size_t ch = 0; ch < audio->channels; ch++) {
			if (!audio->mix[ch])
				continue;
			memset(audio->mix[ch] + audio->mix_frames, 0,
			       (frames - audio->mix_frames) * sizeof(float));
		}
		audio->mix_frames = frames;
	}

	for (size_t ch = 0; ch < audio->channels; ch++) {
		float *out = audio->mix[ch];
		const float *in = (const float *)audio_data->data[ch];
		if (!out || !in)
			continue;
		if (vol == 1.0f) {
			for (size_t i = 0; i < frames; i++)
				out[i] += in[i];
		} else {
			for (size_t i = 0; i < frames; i++)
				out[i] += in[i] * vol;
		}
	}
	pthread_mutex_unlock(&audio->mutex);
}

static bool deckout_audio_input_cb(void *param, uint64_t start_ts, uint64_t end_ts, uint64_t *new_ts,
				   uint32_t active_mixers, struct audio_output_data *mixes)
{
	struct deckout_audio *audio = param;
	const size_t frames = AUDIO_OUTPUT_FRAMES;
	const size_t bytes = frames * sizeof(float);

	UNUSED_PARAMETER(end_ts);
	UNUSED_PARAMETER(active_mixers);

	pthread_mutex_lock(&audio->mutex);
	const size_t take = audio->mix_frames < frames ? audio->mix_frames : frames;

	for (size_t ch = 0; ch < audio->channels; ch++) {
		float *out = mixes[0].data[ch];
		memset(out, 0, bytes);
		if (!audio->mix[ch] || !take)
			continue;

		memcpy(out, audio->mix[ch], take * sizeof(float));
		if (audio->mix_frames > take) {
			memmove(audio->mix[ch], audio->mix[ch] + take,
				(audio->mix_frames - take) * sizeof(float));
		}
	}

	if (take)
		audio->mix_frames -= take;
	pthread_mutex_unlock(&audio->mutex);

	*new_ts = start_ts;
	return true;
}

static bool deckout_audio_capture_already(struct deckout_audio *audio, obs_source_t *source)
{
	for (size_t i = 0; i < audio->captures.num; i++) {
		if (audio->captures.array[i] == source)
			return true;
	}
	return false;
}

static void deckout_audio_register_capture(struct deckout_audio *audio, obs_source_t *source)
{
	if (!audio || !source || deckout_audio_capture_already(audio, source))
		return;

	const uint32_t flags = obs_source_get_output_flags(source);
	if (!(flags & OBS_SOURCE_AUDIO) || (flags & OBS_SOURCE_COMPOSITE))
		return;

	obs_source_t *ref = obs_source_get_ref(source);
	if (!ref)
		return;

	obs_source_add_audio_capture_callback(ref, deckout_audio_capture, audio);
	da_push_back(audio->captures, &ref);
}

static void deckout_audio_collect_tree(obs_source_t *parent, obs_source_t *source, void *param)
{
	UNUSED_PARAMETER(parent);
	deckout_audio_register_capture(param, source);
}

static bool deckout_audio_init(struct deckout_filter *filter, obs_source_t *target, bool capture)
{
	struct deckout_audio *audio = &filter->local_audio;
	struct obs_audio_info oai;

	if (!obs_get_audio_info(&oai)) {
		obs_log(LOG_WARNING, "[deck-out] Failed to fetch OBS audio info for local audio routing");
		return false;
	}

	if (pthread_mutex_init(&audio->mutex, NULL) != 0) {
		obs_log(LOG_WARNING, "[deck-out] Failed to initialize local audio mutex");
		return false;
	}
	audio->mutex_initialized = true;
	audio->channels = get_audio_channels(oai.speakers);
	audio->sample_rate = oai.samples_per_sec;
	audio->pre_fader = filter->audio_pre_fader;
	da_init(audio->captures);

	for (size_t ch = 0; ch < audio->channels; ch++) {
		audio->mix[ch] = bzalloc(DECKOUT_AUDIO_MIX_FRAMES * sizeof(float));
		if (!audio->mix[ch]) {
			obs_log(LOG_WARNING, "[deck-out] Failed to allocate audio mix buffers");
			deckout_audio_free(audio);
			return false;
		}
	}

	struct audio_output_info ai = {
		.name = capture ? "deckout_filter_audio" : "deckout_filter_audio_silent",
		.samples_per_sec = oai.samples_per_sec,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.speakers = oai.speakers,
		.input_callback = deckout_audio_input_cb,
		.input_param = audio,
	};

	if (audio_output_open(&audio->audio, &ai) != AUDIO_OUTPUT_SUCCESS) {
		obs_log(LOG_WARNING, "[deck-out] Failed to open local audio output");
		deckout_audio_free(audio);
		return false;
	}

	if (capture && target) {
		audio->root = obs_source_get_ref(target);
		if (!audio->root) {
			obs_log(LOG_WARNING, "[deck-out] Failed to reference audio root source");
			deckout_audio_free(audio);
			return false;
		}

		const uint32_t flags = obs_source_get_output_flags(target);
		if (flags & OBS_SOURCE_COMPOSITE) {
			/* Scenes/groups never fire capture callbacks on themselves. */
			obs_source_enum_active_tree(target, deckout_audio_collect_tree, audio);
			obs_log(LOG_INFO,
				"[deck-out] Scene/composite audio initialized (root='%s', taps=%zu, pre_fader=%s, channels=%zu, sample_rate=%u)",
				obs_source_get_name(target), audio->captures.num, audio->pre_fader ? "true" : "false",
				audio->channels, audio->sample_rate);
		} else {
			deckout_audio_register_capture(audio, target);
			obs_log(LOG_INFO,
				"[deck-out] Local audio routing initialized (source='%s', taps=%zu, pre_fader=%s, channels=%zu, sample_rate=%u)",
				obs_source_get_name(target), audio->captures.num, audio->pre_fader ? "true" : "false",
				audio->channels, audio->sample_rate);
		}
	} else {
		obs_log(LOG_INFO, "[deck-out] Silent audio initialized (channels=%zu, sample_rate=%u)", audio->channels,
			audio->sample_rate);
	}
	return true;
}

static void deckout_filter_release_audio_showing(struct deckout_filter *filter)
{
	if (!filter->audio_showing_ref || !filter->local_audio.root)
		return;

	obs_source_t *source = filter->local_audio.root;
	if (deckout_parent_is_scene(source))
		obs_source_dec_active(source);
	else
		obs_source_dec_showing(source);

	filter->audio_showing_ref = false;
}

static void deckout_filter_add_audio_showing(struct deckout_filter *filter, obs_source_t *source)
{
	if (!filter || !source || filter->audio_showing_ref)
		return;

	/* Video parent showing already keeps this source alive/producing. */
	if (source == filter->parent && filter->showing_ref)
		return;

	if (deckout_parent_is_scene(source))
		obs_source_inc_active(source);
	else
		obs_source_inc_showing(source);

	filter->audio_showing_ref = true;
}

static audio_t *deckout_filter_get_audio(struct deckout_filter *filter, obs_source_t *capture_source)
{
	/* DeckLink output is OBS_OUTPUT_AV — NULL audio fails start. Mute = silence bus. */
	if (filter->audio_routing == DECKOUT_AUDIO_ROUTE_MUTE) {
		if (!filter->local_audio.audio && !deckout_audio_init(filter, NULL, false))
			obs_log(LOG_WARNING, "[deck-out] Silent audio init failed");
		return filter->local_audio.audio;
	}

	if (filter->audio_routing == DECKOUT_AUDIO_ROUTE_MASTER)
		return obs_get_audio();

	if (filter->audio_routing != DECKOUT_AUDIO_ROUTE_LOCAL &&
	    filter->audio_routing != DECKOUT_AUDIO_ROUTE_CUSTOM)
		return obs_get_audio();

	if (!capture_source) {
		obs_log(LOG_WARNING, "[deck-out] No audio capture source; falling back to master audio");
		return obs_get_audio();
	}

	if (!filter->local_audio.audio) {
		/* Activate composites before enumerating their audio tree. */
		deckout_filter_add_audio_showing(filter, capture_source);
		if (!deckout_audio_init(filter, capture_source, true)) {
			obs_log(LOG_WARNING, "[deck-out] Local audio init failed; falling back to master audio");
			deckout_filter_release_audio_showing(filter);
			return obs_get_audio();
		}
	}

	return filter->local_audio.audio ? filter->local_audio.audio : obs_get_audio();
}

static const char *deckout_audio_routing_name(enum deckout_audio_routing routing)
{
	switch (routing) {
	case DECKOUT_AUDIO_ROUTE_MUTE:
		return "mute";
	case DECKOUT_AUDIO_ROUTE_LOCAL:
		return "local";
	case DECKOUT_AUDIO_ROUTE_CUSTOM:
		return "custom";
	case DECKOUT_AUDIO_ROUTE_MASTER:
	default:
		return "master";
	}
}

static void deckout_migrate_legacy_mute(obs_data_t *settings)
{
	if (!obs_data_has_user_value(settings, PROP_MUTE_AUDIO_LEGACY))
		return;

	if (obs_data_get_bool(settings, PROP_MUTE_AUDIO_LEGACY))
		obs_data_set_int(settings, PROP_AUDIO_ROUTING, AUDIO_ROUTE_MUTE);

	obs_data_unset_user_value(settings, PROP_MUTE_AUDIO_LEGACY);
}

static void deckout_filter_release_showing(struct deckout_filter *filter)
{
	if (!filter->showing_ref || !filter->parent)
		return;

	if (deckout_parent_is_scene(filter->parent))
		obs_source_dec_active(filter->parent);
	else
		obs_source_dec_showing(filter->parent);

	filter->showing_ref = false;
}

static void deckout_filter_add_showing(struct deckout_filter *filter, obs_source_t *parent)
{
	if (filter->showing_ref || !parent)
		return;

	if (deckout_parent_is_scene(parent))
		obs_source_inc_active(parent);
	else
		obs_source_inc_showing(parent);

	filter->showing_ref = true;
}

static void deckout_filter_stop(void *data);

static void deckout_filter_request_properties_refresh(struct deckout_filter *filter)
{
	if (filter)
		filter->properties_refresh_pending = true;
}

static void deckout_filter_refresh_properties(struct deckout_filter *filter)
{
	if (filter && filter->context)
		obs_source_update_properties(filter->context);
}

static void deckout_filter_set_props_locked(obs_properties_t *props, bool locked, const char *const *names,
					    size_t count)
{
	if (!props || !names)
		return;

	const char *hint = obs_module_text("Deckout.ModeLockedHint");
	for (size_t i = 0; i < count; i++) {
		obs_property_t *prop = obs_properties_get(props, names[i]);
		if (!prop)
			continue;

		obs_property_set_enabled(prop, !locked);
		if (locked)
			obs_property_set_long_description(prop, hint);
	}
}

static void deckout_filter_stop_internal(struct deckout_filter *filter, bool preserve_run_intent)
{
	obs_output_t *output = NULL;
	obs_canvas_t *canvas = NULL;
	obs_scene_t *scale_scene = NULL;
	obs_source_t *parent = NULL;
	char *device_hash = NULL;
	bool showing_ref = false;
	bool was_active = false;

	deckout_filter_lock(filter);
	if (filter->stopping) {
		deckout_filter_unlock(filter);
		return;
	}

	/* Clean up partial starts too — failed starts leave output/canvas without active=true. */
	if (!filter->active && !filter->output && !filter->canvas && !filter->scale_scene && !filter->showing_ref &&
	    !filter->device_hash && !filter->local_audio.audio && !filter->audio_showing_ref) {
		filter->starting = false;
		if (!preserve_run_intent) {
			filter->waiting_hotplug = false;
			filter->recover_manual_start = false;
			filter->manual_start = false;
		}
		deckout_filter_unlock(filter);
		return;
	}

	filter->stopping = true;
	filter->starting = false;
	was_active = filter->active;
	output = filter->output;
	canvas = filter->canvas;
	scale_scene = filter->scale_scene;
	parent = filter->parent;
	showing_ref = filter->showing_ref;
	device_hash = filter->device_hash;

	filter->active = false;
	filter->output = NULL;
	filter->canvas = NULL;
	filter->scale_scene = NULL;
	filter->device_hash = NULL;
	filter->showing_ref = false;
	if (!preserve_run_intent) {
		filter->manual_start = false;
		filter->waiting_hotplug = false;
		filter->recover_manual_start = false;
	}
	filter->active_mode_id = 0;
	filter->active_keyer = 0;
	filter->active_force_sdr = false;
	deckout_filter_unlock(filter);
	obs_log(LOG_INFO, "[deck-out] Stopping dedicated output (source='%s', device='%s')",
		parent ? obs_source_get_name(parent) : "(null)", device_hash ? device_hash : "(null)");

	if (output) {
		/* Disconnect media before stop so the canvas video thread is not blocked in callbacks. */
		obs_output_set_media(output, NULL, NULL);
		obs_log(LOG_INFO, "[deck-out] Output media cleared");

		if (g_shutting_down)
			obs_output_force_stop(output);
		else
			obs_output_stop(output);
		obs_log(LOG_INFO, "[deck-out] Output stop requested");

		obs_output_release(output);
		obs_log(LOG_INFO, "[deck-out] Output released");
	}

	if (canvas) {
		obs_canvas_set_channel(canvas, 0, NULL);
		obs_log(LOG_INFO, "[deck-out] Canvas channel cleared, releasing canvas...");
		obs_canvas_release(canvas);
		obs_log(LOG_INFO, "[deck-out] Canvas released");
	}

	if (scale_scene) {
		obs_scene_release(scale_scene);
		obs_log(LOG_INFO, "[deck-out] Scale scene released");
	}

	if (showing_ref && parent) {
		if (deckout_parent_is_scene(parent))
			obs_source_dec_active(parent);
		else
			obs_source_dec_showing(parent);
		obs_log(LOG_INFO, "[deck-out] Showing/active ref released");
	}

	if (device_hash) {
		deckout_registry_release(device_hash, filter->owner_id);
		bfree(device_hash);
	}

	deckout_filter_release_audio_showing(filter);
	deckout_audio_free(&filter->local_audio);
	deckout_filter_lock(filter);
	filter->stopping = false;
	const bool waiting = filter->waiting_hotplug;
	deckout_filter_unlock(filter);

	if (waiting)
		deckout_filter_set_status(filter, DECKOUT_STATUS_WAITING_DEVICE);
	else
		deckout_filter_set_status(filter, DECKOUT_STATUS_IDLE);
	if (was_active && !g_shutting_down)
		deckout_filter_request_properties_refresh(filter);
}

static bool deckout_filter_validate_settings(struct deckout_filter *filter, obs_data_t *settings)
{
	const char *device_hash = obs_data_get_string(settings, PROP_DEVICE_HASH);
	obs_source_t *parent = obs_filter_get_parent(filter->context);

	if (!parent) {
		deckout_filter_set_status(filter, DECKOUT_STATUS_INVALID_CONFIG);
		return false;
	}

	if (!device_hash || !*device_hash) {
		deckout_filter_set_status(filter, DECKOUT_STATUS_INVALID_CONFIG);
		return false;
	}

	const uint32_t width = obs_source_get_base_width(parent);
	const uint32_t height = obs_source_get_base_height(parent);
	if (!width || !height) {
		deckout_filter_set_status(filter, DECKOUT_STATUS_INVALID_CONFIG);
		return false;
	}

	return true;
}

static void deckout_filter_start(void *data, obs_data_t *settings)
{
	struct deckout_filter *filter = data;
	bool release_settings = false;
	long long mode_id = 0;
	long long keyer = 0;
	bool force_sdr = false;

	if (g_shutting_down)
		return;

	deckout_filter_lock(filter);
	if (filter->active || filter->starting || filter->stopping) {
		deckout_filter_unlock(filter);
		return;
	}
	filter->starting = true;
	deckout_filter_unlock(filter);

	if (!g_decklink_output_available) {
		deckout_filter_lock(filter);
		filter->starting = false;
		deckout_filter_unlock(filter);
		deckout_filter_set_status(filter, DECKOUT_STATUS_DEVICE_UNAVAILABLE);
		return;
	}

	if (!settings) {
		settings = obs_source_get_settings(filter->context);
		release_settings = true;
	}

	if (!deckout_filter_validate_settings(filter, settings)) {
		deckout_filter_lock(filter);
		filter->starting = false;
		deckout_filter_unlock(filter);
		if (release_settings)
			obs_data_release(settings);
		return;
	}

	const char *device_hash = obs_data_get_string(settings, PROP_DEVICE_HASH);
	const char *mode_name = obs_data_get_string(settings, PROP_MODE_NAME);
	mode_id = obs_data_get_int(settings, PROP_MODE_ID);
	keyer = obs_data_get_int(settings, PROP_KEYER);
	force_sdr = obs_data_get_bool(settings, PROP_FORCE_SDR);
	deckout_log_demanded_mode(filter, settings, mode_id);
	if (deckout_registry_is_claimed(device_hash, filter->owner_id)) {
		obs_log(LOG_WARNING, "DeckLink device '%s' already in use by another DeckLink Output filter",
			device_hash);
		deckout_filter_lock(filter);
		filter->starting = false;
		deckout_filter_unlock(filter);
		deckout_filter_set_status(filter, DECKOUT_STATUS_CONFLICT);
		if (release_settings)
			obs_data_release(settings);
		return;
	}

	if (deckout_device_has_active_output(device_hash, filter->output)) {
		obs_log(LOG_WARNING,
			"DeckLink device '%s' already in use by Tools → DeckLink Output or another active output",
			device_hash);
		deckout_filter_lock(filter);
		filter->starting = false;
		deckout_filter_unlock(filter);
		deckout_filter_set_status(filter, DECKOUT_STATUS_CONFLICT);
		if (release_settings)
			obs_data_release(settings);
		return;
	}

	if (deckout_device_has_active_input(device_hash)) {
		obs_log(LOG_WARNING,
			"DeckLink device '%s' has active input capture — output and input cannot share the same hardware",
			device_hash);
		deckout_filter_lock(filter);
		filter->starting = false;
		deckout_filter_unlock(filter);
		deckout_filter_set_status(filter, DECKOUT_STATUS_INPUT_CONFLICT);
		if (release_settings)
			obs_data_release(settings);
		return;
	}

	obs_source_t *parent = obs_filter_get_parent(filter->context);
	filter->parent = parent;
	obs_log(LOG_INFO,
		"[deck-out] Attempting start (filter='%s', parent='%s', device='%s', mode='%s', mode_id=%lld, keyer=%lld, force_sdr=%s, audio_routing=%s)",
		obs_source_get_name(filter->context), parent ? obs_source_get_name(parent) : "(null)", device_hash,
		mode_name && *mode_name ? mode_name : "(unset)", mode_id, keyer, force_sdr ? "true" : "false",
		deckout_audio_routing_name(filter->audio_routing));

	deckout_filter_set_status(filter, DECKOUT_STATUS_STARTING);

	char output_name[128];
	snprintf(output_name, sizeof(output_name), "deckout_%p", (void *)filter->context);

	obs_data_t *output_settings = deckout_build_output_settings(settings);
	filter->output = obs_output_create(DECKLINK_OUTPUT_ID, output_name, output_settings, NULL);
	if (!filter->output) {
		obs_log(LOG_ERROR, "Failed to create DeckLink output");
		obs_data_release(output_settings);
		deckout_filter_lock(filter);
		filter->starting = false;
		deckout_filter_unlock(filter);
		deckout_filter_set_status(filter, DECKOUT_STATUS_DEVICE_UNAVAILABLE);
		if (release_settings)
			obs_data_release(settings);
		return;
	}

	const struct video_scale_info *conversion = obs_output_get_video_conversion(filter->output);
	if (!conversion || !conversion->width || !conversion->height) {
		obs_log(LOG_ERROR, "DeckLink output has no valid video conversion (check device and mode)");
		obs_output_release(filter->output);
		filter->output = NULL;
		obs_data_release(output_settings);
		deckout_filter_lock(filter);
		filter->starting = false;
		deckout_filter_unlock(filter);
		deckout_filter_set_status(filter, DECKOUT_STATUS_UNSUPPORTED_MODE);
		if (release_settings)
			obs_data_release(settings);
		return;
	}

	obs_output_update(filter->output, output_settings);
	obs_data_release(output_settings);

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		obs_log(LOG_ERROR, "Failed to get program video info");
		obs_output_release(filter->output);
		filter->output = NULL;
		deckout_filter_lock(filter);
		filter->starting = false;
		deckout_filter_unlock(filter);
		deckout_filter_set_status(filter, DECKOUT_STATUS_DEVICE_UNAVAILABLE);
		if (release_settings)
			obs_data_release(settings);
		return;
	}

	ovi.base_width = conversion->width;
	ovi.base_height = conversion->height;
	ovi.output_width = conversion->width;
	ovi.output_height = conversion->height;

	obs_log(LOG_INFO, "[deck-out] Output video queue: %ux%u @ %u/%u (mode-matched, format=%d)",
		conversion->width, conversion->height, ovi.fps_num, ovi.fps_den, (int)ovi.output_format);

	filter->canvas = obs_canvas_create_private(output_name, &ovi, EPHEMERAL);
	if (!filter->canvas) {
		obs_log(LOG_ERROR, "Failed to create private canvas");
		obs_output_release(filter->output);
		filter->output = NULL;
		deckout_filter_lock(filter);
		filter->starting = false;
		deckout_filter_unlock(filter);
		deckout_filter_set_status(filter, DECKOUT_STATUS_DEVICE_UNAVAILABLE);
		if (release_settings)
			obs_data_release(settings);
		return;
	}

	const uint32_t src_w = obs_source_get_base_width(parent);
	const uint32_t src_h = obs_source_get_base_height(parent);
	const bool needs_scale = src_w != conversion->width || src_h != conversion->height;

	if (needs_scale) {
		filter->scale_scene = obs_scene_create_private(output_name);
		if (!filter->scale_scene) {
			obs_log(LOG_ERROR, "Failed to create scale scene");
			deckout_filter_stop_internal(filter, false);
			deckout_filter_set_status(filter, DECKOUT_STATUS_DEVICE_UNAVAILABLE);
			if (release_settings)
				obs_data_release(settings);
			return;
		}

		obs_sceneitem_t *item = obs_scene_add(filter->scale_scene, parent);
		if (!item) {
			obs_log(LOG_ERROR, "Failed to add source to scale scene");
			deckout_filter_stop_internal(filter, false);
			deckout_filter_set_status(filter, DECKOUT_STATUS_DEVICE_UNAVAILABLE);
			if (release_settings)
				obs_data_release(settings);
			return;
		}

		struct vec2 bounds;
		vec2_set(&bounds, (float)conversion->width, (float)conversion->height);
		obs_sceneitem_set_bounds(item, &bounds);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
		obs_sceneitem_set_scale_filter(item, OBS_SCALE_BICUBIC);

		obs_log(LOG_INFO, "[deck-out] Scaling source '%s' (%ux%u) to fit DeckLink mode %ux%u",
			obs_source_get_name(parent), src_w, src_h, conversion->width, conversion->height);

		obs_canvas_set_channel(filter->canvas, 0, obs_scene_get_source(filter->scale_scene));
	} else {
		obs_log(LOG_INFO, "[deck-out] Source '%s' matches DeckLink mode %ux%u — no scale pass",
			obs_source_get_name(parent), conversion->width, conversion->height);
		obs_canvas_set_channel(filter->canvas, 0, parent);
	}

	deckout_filter_add_showing(filter, parent);

	obs_source_t *audio_cap = NULL;
	if (filter->audio_routing == DECKOUT_AUDIO_ROUTE_LOCAL) {
		audio_cap = parent;
	} else if (filter->audio_routing == DECKOUT_AUDIO_ROUTE_CUSTOM) {
		const char *uuid = filter->audio_source_uuid;
		if (!uuid || !*uuid)
			uuid = obs_data_get_string(settings, PROP_AUDIO_SOURCE);
		audio_cap = (uuid && *uuid) ? obs_get_source_by_uuid(uuid) : NULL;
		if (!audio_cap) {
			obs_log(LOG_ERROR, "[deck-out] Selected audio source not found");
			deckout_filter_stop_internal(filter, false);
			deckout_filter_set_status(filter, DECKOUT_STATUS_INVALID_CONFIG);
			if (release_settings)
				obs_data_release(settings);
			return;
		}
	}

	audio_t *audio = deckout_filter_get_audio(filter, audio_cap);
	if (filter->audio_routing == DECKOUT_AUDIO_ROUTE_CUSTOM && audio_cap)
		obs_source_release(audio_cap);

	if (!audio) {
		obs_log(LOG_ERROR, "[deck-out] No audio bus available for DeckLink output");
		deckout_filter_stop_internal(filter, false);
		deckout_filter_set_status(filter, DECKOUT_STATUS_DEVICE_UNAVAILABLE);
		if (release_settings)
			obs_data_release(settings);
		return;
	}
	obs_output_set_media(filter->output, obs_canvas_get_video(filter->canvas), audio);

	const bool started = obs_output_start(filter->output);
	if (!started) {
		const char *last_error = obs_output_get_last_error(filter->output);
		obs_log(LOG_ERROR, "DeckLink output failed to start: %s", last_error ? last_error : "unknown error");

		if (last_error && strstr(last_error, "FPS"))
			deckout_filter_set_status(filter, DECKOUT_STATUS_UNSUPPORTED_MODE);
		else
			deckout_filter_set_status(filter, DECKOUT_STATUS_DEVICE_UNAVAILABLE);

		deckout_filter_stop_internal(filter, false);
		if (release_settings)
			obs_data_release(settings);
		return;
	}

	if (!deckout_registry_claim(device_hash, filter->owner_id)) {
		obs_log(LOG_WARNING, "DeckLink device conflict after start");
		deckout_filter_set_status(filter, DECKOUT_STATUS_CONFLICT);
		deckout_filter_stop_internal(filter, false);
		if (release_settings)
			obs_data_release(settings);
		return;
	}

	deckout_filter_lock(filter);
	filter->device_hash = bstrdup(device_hash);
	filter->active = true;
	filter->starting = false;
	filter->waiting_hotplug = false;
	filter->recover_manual_start = false;
	filter->hotplug_check_elapsed = 0.0f;
	filter->active_mode_id = mode_id;
	filter->active_keyer = keyer;
	filter->active_force_sdr = force_sdr;
	deckout_filter_unlock(filter);
	deckout_filter_set_status(filter, DECKOUT_STATUS_ACTIVE);
	deckout_filter_request_properties_refresh(filter);

	obs_log(LOG_INFO, "DeckLink filter output started for '%s' on device '%s'", obs_source_get_name(parent),
		device_hash);
	deckout_log_active_output_mode(filter);

	if (release_settings)
		obs_data_release(settings);
}

static void deckout_filter_stop(void *data)
{
	struct deckout_filter *filter = data;
	deckout_filter_stop_internal(filter, false);
}

static void deckout_filter_stop_for_hotplug(struct deckout_filter *filter)
{
	deckout_filter_lock(filter);
	filter->waiting_hotplug = true;
	filter->recover_manual_start = filter->manual_start;
	deckout_filter_unlock(filter);

	obs_log(LOG_WARNING, "[deck-out] Device disconnected — stopping output and waiting for reconnect");
	deckout_filter_stop_internal(filter, true);
}

static bool deckout_filter_should_run(struct deckout_filter *filter)
{
	if (!obs_source_enabled(filter->context))
		return false;

	if (filter->manual_start)
		return true;

	return filter->auto_start && !filter->user_stopped;
}

static void deckout_filter_try_start(struct deckout_filter *filter)
{
	if (g_shutting_down || filter->active || filter->starting || !deckout_filter_should_run(filter))
		return;

	if (!g_frontend_ready)
		return;

	obs_data_t *settings = obs_source_get_settings(filter->context);
	deckout_filter_start(filter, settings);
	obs_data_release(settings);
}

static void deckout_filter_frontend_event(enum obs_frontend_event event, void *data)
{
	struct deckout_filter *filter = data;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		g_frontend_ready = true;
		deckout_filter_try_start(filter);
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		/* SCRIPTING_SHUTDOWN fires before ClearSceneData; EXIT is too late. */
		if (g_shutting_down)
			break;
		g_shutting_down = true;
		g_frontend_ready = false;
		filter->pending_start = false;
		filter->user_stopped = false;
		filter->properties_refresh_pending = false;
		filter->auto_start = false;
		filter->manual_start = false;
		filter->waiting_hotplug = false;
		filter->recover_manual_start = false;
		obs_log(LOG_INFO, "[deck-out] Frontend shutdown — stopping dedicated output");
		deckout_filter_stop(filter);
		break;
	default:
		break;
	}
}

static const char *deckout_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Deckout.FilterName");
}

static void deckout_filter_update(void *data, obs_data_t *settings)
{
	struct deckout_filter *filter = data;

	deckout_migrate_legacy_mute(settings);

	const bool auto_start = obs_data_get_bool(settings, PROP_AUTO_START);
	const int audio_routing = (int)obs_data_get_int(settings, PROP_AUDIO_ROUTING);
	const int audio_tap = (int)obs_data_get_int(settings, PROP_AUDIO_TAP);
	const bool audio_pre_fader = audio_tap == AUDIO_TAP_PRE;
	const char *audio_source_uuid = obs_data_get_string(settings, PROP_AUDIO_SOURCE);
	const char *device_hash = obs_data_get_string(settings, PROP_DEVICE_HASH);
	const long long mode_id = obs_data_get_int(settings, PROP_MODE_ID);
	const long long keyer = obs_data_get_int(settings, PROP_KEYER);
	const bool force_sdr = obs_data_get_bool(settings, PROP_FORCE_SDR);

	deckout_filter_lock(filter);
	const bool auto_start_changed = filter->auto_start != auto_start;
	const bool audio_changed = filter->active && (int)filter->audio_routing != audio_routing;
	const bool audio_tap_changed = filter->active && filter->audio_pre_fader != audio_pre_fader;
	const bool audio_source_changed =
		filter->active && filter->audio_routing == DECKOUT_AUDIO_ROUTE_CUSTOM &&
		((!filter->audio_source_uuid && audio_source_uuid && *audio_source_uuid) ||
		 (filter->audio_source_uuid &&
		  (!audio_source_uuid || strcmp(filter->audio_source_uuid, audio_source_uuid) != 0)));
	const bool device_changed = filter->active && filter->device_hash &&
				    strcmp(filter->device_hash, device_hash) != 0;
	const bool mode_changed = filter->active && filter->active_mode_id != mode_id;
	const bool keyer_changed = filter->active && filter->active_keyer != keyer;
	const bool force_sdr_changed = filter->active && filter->active_force_sdr != force_sdr;

	if (device_changed || mode_changed || keyer_changed || force_sdr_changed) {
		obs_log(LOG_WARNING, "[deck-out] DeckLink settings changed while output is active; stop output to apply");
		deckout_filter_unlock(filter);
		return;
	}

	filter->auto_start = auto_start;
	if (!filter->active) {
		filter->audio_routing = (enum deckout_audio_routing)audio_routing;
		filter->audio_pre_fader = audio_pre_fader;
		bfree(filter->audio_source_uuid);
		filter->audio_source_uuid =
			(audio_source_uuid && *audio_source_uuid) ? bstrdup(audio_source_uuid) : NULL;
	}

	if (audio_changed || audio_source_changed || audio_tap_changed) {
		obs_log(LOG_WARNING, "[deck-out] Audio routing changed while output is active; stop output to apply");
		deckout_filter_unlock(filter);
		return;
	}

	if (auto_start_changed && filter->auto_start) {
		filter->user_stopped = false;
		filter->pending_start = true;
	}
	deckout_filter_unlock(filter);
}

static void *deckout_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct deckout_filter *filter = bzalloc(sizeof(*filter));
	filter->context = source;
	filter->status = DECKOUT_STATUS_IDLE;
	filter->audio_routing = DECKOUT_AUDIO_ROUTE_MASTER;
	if (pthread_mutex_init(&filter->state_mutex, NULL) == 0)
		filter->state_mutex_initialized = true;

	const char *uuid = obs_source_get_uuid(source);
	snprintf(filter->owner_id, sizeof(filter->owner_id), "%s", uuid ? uuid : "deckout");

	deckout_filter_update(filter, settings);

	obs_frontend_add_event_callback(deckout_filter_frontend_event, filter);

	obs_log(LOG_INFO, "DeckLink output filter created: '%s'", obs_source_get_name(source));
	return filter;
}

static void deckout_filter_destroy(void *data)
{
	struct deckout_filter *filter = data;

	obs_frontend_remove_event_callback(deckout_filter_frontend_event, filter);
	filter->pending_start = false;
	filter->properties_refresh_pending = false;
	filter->auto_start = false;
	filter->manual_start = false;
	filter->user_stopped = false;
	deckout_filter_stop(filter);
	bfree(filter->device_hash);
	bfree(filter->audio_source_uuid);
	if (filter->state_mutex_initialized)
		pthread_mutex_destroy(&filter->state_mutex);
	bfree(filter);

	obs_log(LOG_INFO, "DeckLink output filter destroyed");
}

static void deckout_filter_filter_add(void *data, obs_source_t *parent)
{
	struct deckout_filter *filter = data;
	UNUSED_PARAMETER(parent);

	if (deckout_filter_should_run(filter) || filter->pending_start)
		deckout_filter_try_start(filter);
}

static void deckout_filter_filter_remove(void *data, obs_source_t *parent)
{
	struct deckout_filter *filter = data;
	UNUSED_PARAMETER(parent);

	deckout_filter_stop(filter);
}

static bool deckout_enum_audio_sources(void *data, obs_source_t *source)
{
	obs_property_t *list = data;
	const uint32_t flags = obs_source_get_output_flags(source);

	if (!(flags & OBS_SOURCE_AUDIO))
		return true;

	const char *name = obs_source_get_name(source);
	const char *uuid = obs_source_get_uuid(source);
	if (!name || !*name || !uuid || !*uuid)
		return true;

	obs_property_list_add_string(list, name, uuid);
	return true;
}

static void deckout_fill_audio_source_list(obs_property_t *list)
{
	if (!list)
		return;

	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("Deckout.AudioSource.None"), "");
	obs_enum_sources(deckout_enum_audio_sources, list);
}

static bool deckout_audio_routing_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const int routing = (int)obs_data_get_int(settings, PROP_AUDIO_ROUTING);
	const bool source_tap = routing == AUDIO_ROUTE_LOCAL || routing == AUDIO_ROUTE_CUSTOM;

	obs_property_t *audio_source = obs_properties_get(props, PROP_AUDIO_SOURCE);
	if (audio_source)
		obs_property_set_visible(audio_source, routing == AUDIO_ROUTE_CUSTOM);

	obs_property_t *audio_tap = obs_properties_get(props, PROP_AUDIO_TAP);
	if (audio_tap)
		obs_property_set_visible(audio_tap, source_tap);

	return true;
}

static bool deckout_filter_start_stop_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);

	struct deckout_filter *filter = data;

	obs_property_set_enabled(property, false);

	if (!filter->active && !filter->waiting_hotplug) {
		filter->user_stopped = false;
		filter->manual_start = true;
		filter->waiting_hotplug = false;
		obs_data_t *settings = obs_source_get_settings(filter->context);
		deckout_filter_start(filter, settings);
		obs_data_release(settings);
	} else {
		filter->user_stopped = true;
		filter->manual_start = false;
		filter->waiting_hotplug = false;
		filter->recover_manual_start = false;
		deckout_filter_stop(filter);
	}

	obs_property_set_enabled(property, true);
	return true;
}

static obs_properties_t *deckout_filter_properties(void *data)
{
	struct deckout_filter *filter = data;
	const bool locked = filter && (filter->active || filter->starting);
	const int routing = filter ? (int)filter->audio_routing : AUDIO_ROUTE_MASTER;
	const bool source_tap = routing == AUDIO_ROUTE_LOCAL || routing == AUDIO_ROUTE_CUSTOM;

	obs_properties_t *props = obs_properties_create();

	/*
	 * OBS properties are single-column. Keep primary controls first so
	 * Start/Status stay visible, then Device / Video / Audio groups.
	 */
	obs_property_t *status = obs_properties_add_text(props, PROP_STATUS, obs_module_text("Deckout.Status"),
							 OBS_TEXT_INFO);
	obs_property_set_long_description(status, status_to_string(filter ? filter->status : DECKOUT_STATUS_IDLE));
	obs_property_text_set_info_type(status, status_to_info_type(filter ? filter->status : DECKOUT_STATUS_IDLE));

	obs_properties_add_button2(props, PROP_START_STOP,
				   filter && (filter->active || filter->waiting_hotplug)
					   ? obs_module_text("Deckout.Stop")
					   : obs_module_text("Deckout.Start"),
				   deckout_filter_start_stop_clicked, filter);

	obs_properties_add_bool(props, PROP_AUTO_START, obs_module_text("Deckout.AutoStart"));

	/*
	 * Split DeckLink output props into Device vs Video groups. Fetch twice
	 * and strip the other group's fields so property names stay unique.
	 * Device-change callbacks search the root tree recursively, so mode/keyer
	 * still update when they live in the sibling Video group.
	 */
	obs_properties_t *device_props = obs_get_output_properties(DECKLINK_OUTPUT_ID);
	if (device_props) {
		obs_properties_remove_by_name(device_props, PROP_AUTO_START);
		obs_properties_remove_by_name(device_props, PROP_MODE_ID);
		obs_properties_remove_by_name(device_props, PROP_MODE_NAME);
		obs_properties_remove_by_name(device_props, PROP_KEYER);
		obs_properties_remove_by_name(device_props, PROP_FORCE_SDR);
		static const char *device_lock[] = {PROP_DEVICE_HASH};
		deckout_filter_set_props_locked(device_props, locked, device_lock,
						sizeof(device_lock) / sizeof(device_lock[0]));
		obs_properties_add_group(props, "deckout_device_group", obs_module_text("Deckout.DeckLinkSettings"),
					 OBS_GROUP_NORMAL, device_props);
	}

	obs_properties_t *video_props = obs_get_output_properties(DECKLINK_OUTPUT_ID);
	if (video_props) {
		obs_properties_remove_by_name(video_props, PROP_AUTO_START);
		obs_properties_remove_by_name(video_props, PROP_DEVICE_HASH);
		static const char *video_lock[] = {PROP_MODE_ID, PROP_KEYER, PROP_FORCE_SDR};
		deckout_filter_set_props_locked(video_props, locked, video_lock,
						sizeof(video_lock) / sizeof(video_lock[0]));
		obs_properties_add_group(props, "deckout_video_group", obs_module_text("Deckout.VideoSettings"),
					 OBS_GROUP_NORMAL, video_props);
	}

	obs_properties_t *audio_props = obs_properties_create();
	obs_property_t *audio = obs_properties_add_list(audio_props, PROP_AUDIO_ROUTING,
							obs_module_text("Deckout.AudioRouting"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(audio, obs_module_text("Deckout.AudioRouting.Master"), AUDIO_ROUTE_MASTER);

	obs_source_t *parent = filter ? obs_filter_get_parent(filter->context) : NULL;
	if (deckout_parent_is_scene(parent))
		obs_property_list_add_int(audio, obs_module_text("Deckout.AudioRouting.Scene"), AUDIO_ROUTE_LOCAL);
	else
		obs_property_list_add_int(audio, obs_module_text("Deckout.AudioRouting.Source"), AUDIO_ROUTE_LOCAL);

	obs_property_list_add_int(audio, obs_module_text("Deckout.AudioRouting.Custom"), AUDIO_ROUTE_CUSTOM);
	obs_property_list_add_int(audio, obs_module_text("Deckout.AudioRouting.Mute"), AUDIO_ROUTE_MUTE);
	obs_property_set_long_description(audio, obs_module_text("Deckout.AudioRouting.Desc"));
	obs_property_set_modified_callback(audio, deckout_audio_routing_modified);

	obs_property_t *audio_source = obs_properties_add_list(audio_props, PROP_AUDIO_SOURCE,
							       obs_module_text("Deckout.AudioSource"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	deckout_fill_audio_source_list(audio_source);
	obs_property_set_long_description(audio_source, obs_module_text("Deckout.AudioSource.Desc"));
	obs_property_set_visible(audio_source, routing == AUDIO_ROUTE_CUSTOM);

	obs_property_t *audio_tap = obs_properties_add_list(audio_props, PROP_AUDIO_TAP,
							    obs_module_text("Deckout.AudioTap"), OBS_COMBO_TYPE_LIST,
							    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(audio_tap, obs_module_text("Deckout.AudioTap.Post"), AUDIO_TAP_POST);
	obs_property_list_add_int(audio_tap, obs_module_text("Deckout.AudioTap.Pre"), AUDIO_TAP_PRE);
	obs_property_set_long_description(audio_tap, obs_module_text("Deckout.AudioTap.Desc"));
	obs_property_set_visible(audio_tap, source_tap);

	if (locked) {
		obs_property_set_enabled(audio, false);
		obs_property_set_long_description(audio, obs_module_text("Deckout.ModeLockedHint"));
		obs_property_set_enabled(audio_source, false);
		obs_property_set_long_description(audio_source, obs_module_text("Deckout.ModeLockedHint"));
		obs_property_set_enabled(audio_tap, false);
		obs_property_set_long_description(audio_tap, obs_module_text("Deckout.ModeLockedHint"));
	}

	obs_properties_add_group(props, "deckout_audio_group", obs_module_text("Deckout.AudioSettings"),
				 OBS_GROUP_NORMAL, audio_props);

	return props;
}

static void deckout_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, PROP_AUDIO_ROUTING, AUDIO_ROUTE_MASTER);
	obs_data_set_default_string(settings, PROP_AUDIO_SOURCE, "");
	obs_data_set_default_int(settings, PROP_AUDIO_TAP, AUDIO_TAP_POST);
	obs_data_set_default_bool(settings, PROP_AUTO_START, false);
}

static void deckout_filter_check_hotplug(struct deckout_filter *filter, float seconds)
{
	filter->hotplug_check_elapsed += seconds;
	if (filter->hotplug_check_elapsed < HOTPLUG_CHECK_INTERVAL_SEC)
		return;
	filter->hotplug_check_elapsed = 0.0f;

	obs_data_t *settings = obs_source_get_settings(filter->context);
	const char *device_hash = obs_data_get_string(settings, PROP_DEVICE_HASH);
	const bool present = deckout_device_is_present(device_hash);
	obs_data_release(settings);

	if (filter->active) {
		if (present)
			return;

		deckout_filter_stop_for_hotplug(filter);
		return;
	}

	if (!filter->waiting_hotplug)
		return;

	if (!deckout_filter_should_run(filter)) {
		filter->waiting_hotplug = false;
		deckout_filter_set_status(filter, DECKOUT_STATUS_IDLE);
		deckout_filter_request_properties_refresh(filter);
		return;
	}

	if (!present)
		return;

	obs_log(LOG_INFO, "[deck-out] Device reconnected — restarting output");
	if (filter->recover_manual_start)
		filter->manual_start = true;
	deckout_filter_try_start(filter);
}

static void deckout_filter_video_tick(void *data, float seconds)
{
	struct deckout_filter *filter = data;

	if (g_shutting_down)
		return;

	if (filter->properties_refresh_pending) {
		filter->properties_refresh_pending = false;
		deckout_filter_refresh_properties(filter);
	}

	deckout_filter_check_hotplug(filter, seconds);

	if (!filter->active) {
		if (filter->waiting_hotplug)
			return;

		if (filter->pending_start || (filter->auto_start && !filter->user_stopped && g_frontend_ready))
			deckout_filter_try_start(filter);
		filter->pending_start = false;
		return;
	}

	if (!deckout_filter_should_run(filter)) {
		obs_log(LOG_INFO,
			"[deck-out] Stopping because filter should not run (enabled=%s, manual_start=%s, auto_start=%s)",
			obs_source_enabled(filter->context) ? "true" : "false", filter->manual_start ? "true" : "false",
			filter->auto_start ? "true" : "false");
		deckout_filter_stop(filter);
	}
}

bool deckout_is_decklink_output_available(void)
{
	return g_decklink_output_available;
}

void deckout_set_decklink_output_available(bool available)
{
	g_decklink_output_available = available;
}

struct obs_source_info deckout_filter_info = {
	.id = "deckout_decklink_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = deckout_filter_get_name,
	.create = deckout_filter_create,
	.destroy = deckout_filter_destroy,
	.update = deckout_filter_update,
	.get_properties = deckout_filter_properties,
	.get_defaults = deckout_filter_defaults,
	.filter_add = deckout_filter_filter_add,
	.filter_remove = deckout_filter_filter_remove,
	.video_tick = deckout_filter_video_tick,
};
