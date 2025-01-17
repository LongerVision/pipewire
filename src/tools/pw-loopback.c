/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>

#include <spa/utils/result.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/pipewire.h>

#define DEFAULT_RATE		48000
#define DEFAULT_CHANNELS	2
#define DEFAULT_CHANNEL_MAP	"[ FL, FR ]"

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	const char *opt_group_name;
	const char *opt_channel_map;

	uint32_t channels;
	uint32_t latency;

	struct pw_properties *capture_props;
	struct pw_stream *capture;
	struct spa_hook capture_listener;

	struct pw_properties *playback_props;
	struct pw_stream *playback;
	struct spa_hook playback_listener;
};

static void capture_process(void *d)
{
	struct data *data = d;
	struct pw_buffer *in, *out;

	if ((in = pw_stream_dequeue_buffer(data->capture)) == NULL)
		pw_log_warn("out of capture buffers: %m");

	if ((out = pw_stream_dequeue_buffer(data->playback)) == NULL)
		pw_log_warn("out of playback buffers: %m");

	if (in != NULL && out != NULL)
		*out->buffer = *in->buffer;

	if (in != NULL)
		pw_stream_queue_buffer(data->capture, in);
	if (out != NULL)
		pw_stream_queue_buffer(data->playback, out);
}

static const struct pw_stream_events in_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = capture_process
};

static const struct pw_stream_events out_stream_events = {
	PW_VERSION_STREAM_EVENTS,
};

static int setup_streams(struct data *data)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	data->capture = pw_stream_new(data->core,
			"loopback capture", data->capture_props);
	data->capture_props = NULL;
	if (data->capture == NULL)
		return -errno;

	pw_stream_add_listener(data->capture,
			&data->capture_listener,
			&in_stream_events, data);

	data->playback = pw_stream_new(data->core,
			"loopback playback", data->playback_props);
	data->playback_props = NULL;
	if (data->playback == NULL)
		return -errno;

	pw_stream_add_listener(data->playback,
			&data->playback_listener,
			&out_stream_events, data);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&SPA_AUDIO_INFO_RAW_INIT(
				.flags = SPA_AUDIO_FLAG_UNPOSITIONED,
				.format = SPA_AUDIO_FORMAT_F32P,
				.channels = data->channels));

	if ((res = pw_stream_connect(data->capture,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	if ((res = pw_stream_connect(data->playback,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct data *d = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(d->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void show_help(struct data *data, const char *name)
{
        fprintf(stdout, "%s [options]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -r, --remote                          Remote daemon name\n"
		"  -g, --group                           Node group (default '%s')\n"
		"  -c, --channels                        Number of channels (default %d)\n"
		"  -m, --channel-map                     Channel map (default '%s')\n"
		"  -l, --latency                         Desired latency in ms\n"
		"  -C  --capture                         Capture source to connect to\n"
		"      --capture-props                   Capture stream properties\n"
		"  -P  --playback                        Playback sink to connect to\n"
		"      --playback-props                  Playback stream properties\n",
		name,
		data->opt_group_name,
		data->channels,
		data->opt_channel_map);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	const char *opt_remote = NULL;
	char cname[256];
	static const struct option long_options[] = {
		{ "help",		no_argument,		NULL, 'h' },
		{ "version",		no_argument,		NULL, 'V' },
		{ "remote",		required_argument,	NULL, 'r' },
		{ "group",		required_argument,	NULL, 'g' },
		{ "channels",		required_argument,	NULL, 'c' },
		{ "latency",		required_argument,	NULL, 'l' },
		{ "capture",		required_argument,	NULL, 'C' },
		{ "playback",		required_argument,	NULL, 'P' },
		{ "capture-props",	required_argument,	NULL, 'i' },
		{ "playback-props",	required_argument,	NULL, 'o' },
		{ NULL, 0, NULL, 0}
	};
	int c, res = -1;

	pw_init(&argc, &argv);

	data.channels = DEFAULT_CHANNELS;
	data.opt_channel_map = DEFAULT_CHANNEL_MAP;
	data.opt_group_name = pw_get_client_name();
	if (snprintf(cname, sizeof(cname), "%s-%zd", argv[0], (size_t) getpid()) > 0)
		data.opt_group_name = cname;

	data.capture_props = pw_properties_new(NULL, NULL);
	data.playback_props = pw_properties_new(NULL, NULL);
	if (data.capture_props == NULL || data.playback_props == NULL) {
		fprintf(stderr, "can't create properties: %m\n");
		goto exit;
	}

	while ((c = getopt_long(argc, argv, "hVr:g:c:m:l:C:P:i:o:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(&data, argv[0]);
			return 0;
		case 'V':
			fprintf(stdout, "%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'r':
			opt_remote = optarg;
			break;
		case 'g':
			data.opt_group_name = optarg;
			break;
		case 'c':
			data.channels = atoi(optarg);
			break;
		case 'm':
			data.opt_channel_map = optarg;
			break;
		case 'l':
			data.latency = atoi(optarg) * DEFAULT_RATE / SPA_MSEC_PER_SEC;
			break;
		case 'C':
			pw_properties_set(data.capture_props, PW_KEY_NODE_TARGET, optarg);
			break;
		case 'P':
			pw_properties_set(data.playback_props, PW_KEY_NODE_TARGET, optarg);
			break;
		case 'i':
			pw_properties_update_string(data.capture_props, optarg, strlen(optarg));
			break;
		case 'o':
			pw_properties_update_string(data.playback_props, optarg, strlen(optarg));
			break;
		default:
			show_help(&data, argv[0]);
			return -1;
		}
	}

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "can't create main loop: %m\n");
		goto exit;
	}

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l, NULL, 0);
	if (data.context == NULL) {
		fprintf(stderr, "can't create context: %m\n");
		goto exit;
	}

	if (data.opt_group_name != NULL) {
		pw_properties_set(data.capture_props, PW_KEY_NODE_GROUP, data.opt_group_name);
		pw_properties_set(data.playback_props, PW_KEY_NODE_GROUP, data.opt_group_name);
	}
	if (data.latency != 0) {
		pw_properties_setf(data.capture_props, PW_KEY_NODE_LATENCY, "%u/%u",
				data.latency, DEFAULT_RATE);
		pw_properties_setf(data.playback_props, PW_KEY_NODE_LATENCY, "%u/%u",
				data.latency, DEFAULT_RATE);
	}

	data.core = pw_context_connect(data.context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, opt_remote,
				NULL),
			0);
	if (data.core == NULL) {
		fprintf(stderr, "can't connect: %m\n");
		goto exit;
	}

	pw_core_add_listener(data.core,
			&data.core_listener,
			&core_events, &data);

	setup_streams(&data);

	pw_main_loop_run(data.loop);

	res = 0;
exit:
	if (data.core)
		pw_core_disconnect(data.core);
	if (data.context)
		pw_context_destroy(data.context);
	if (data.loop)
		pw_main_loop_destroy(data.loop);
	if (data.capture_props)
		pw_properties_free(data.capture_props);
	if (data.playback_props)
		pw_properties_free(data.playback_props);
	pw_deinit();

	return res;
}
