#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/utils/result.h>

#include "fir.h"

#define MAX_FILTER_ORDER 16383
#define NUM_CHANNELS 2
#define CHANNEL_LEFT 0
#define CHANNEL_RIGHT 1

struct channel {
    struct pw_filter_port *in_port;
    struct pw_filter_port *out_port;
    struct delay_line *delay_line;
    const struct fir_filter *current;
};

struct channel_config {
    const char *input_name;
    const char *output_name;
    const char *channel_name;
};

struct data {
    struct pw_main_loop *loop;
    struct pw_filter *filter;

    struct channel channels[NUM_CHANNELS];

    int current_rate;
};

static const struct channel_config channel_configs[NUM_CHANNELS] = {
    {"input_FL", "output_FL", "FL"},
    {"input_FR", "output_FR", "FR"}
};

static void do_quit(void *userdata, int signal_number) {
    struct data *data = userdata;
    pw_main_loop_quit(data->loop);
}

static int init_channel(struct channel *channel, int delay_size) {
    channel->delay_line = delay_line_init(delay_size);
    if (!channel->delay_line) {
        fprintf(stderr, "Failed to initialize delay line\n");
        return -1;
    }

    channel->current = fir_filter_clone(&FIR_FILTERS[0]);
    if (!channel->current) {
        fprintf(stderr, "Failed to clone initial FIR filter\n");
        delay_line_free(channel->delay_line);
        channel->delay_line = NULL;
        return -1;
    }

    return 0;
}

static int init_fir_filters(struct data *data) {
    const int delay_size = MAX_FILTER_ORDER * 4;

    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        if (init_channel(&data->channels[ch], delay_size) != 0) {
            for (int i = 0; i < ch; i++) {
                if (data->channels[i].current) {
                    fir_filter_free(data->channels[i].current);
                }
                if (data->channels[i].delay_line) {
                    delay_line_free(data->channels[i].delay_line);
                }
            }
            return -1;
        }
    }

    data->current_rate = 44100;
    printf("FIR filters initialized for %d channels\n", NUM_CHANNELS);
    return 0;
}

static void cleanup_channel(struct channel *channel) {
    if (channel->current) {
        fir_filter_free(channel->current);
        channel->current = NULL;
    }
    if (channel->delay_line) {
        delay_line_free(channel->delay_line);
        channel->delay_line = NULL;
    }
}

static void cleanup_fir_filters(struct data *data) {
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        cleanup_channel(&data->channels[ch]);
    }
}

static int update_channel_filter(struct channel *channel, int rate) {
    const struct fir_filter *new_filter = NULL;

    for (int i = 0; i < 6; i++) {
        if (rate == FIR_FILTERS[i].rate) {
            new_filter = fir_filter_clone(&FIR_FILTERS[i]);
            break;
        }
    }

    if (!new_filter) {
        new_filter = fir_filter_clone(&FIR_FILTERS[5]);
    }

    if (!new_filter) {
        fprintf(stderr, "Failed to clone filter for rate %d\n", rate);
        return -1;
    }

    const struct fir_filter *old_filter = channel->current;
    channel->current = new_filter;
    fir_filter_free(old_filter);

    return 0;
}

static void select_filter_for_rate(struct data *data, int rate) {
    int actual_rate = rate;

    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        if (update_channel_filter(&data->channels[ch], rate) != 0) {
            update_channel_filter(&data->channels[ch], FIR_FILTERS[5].rate);
            actual_rate = FIR_FILTERS[5].rate;
        }
    }

    data->current_rate = actual_rate;
    printf("Selected FIR filter for rate=%d Hz (order=%d)\n",
           data->current_rate, data->channels[0].current->order);
}

static void on_filter_process(void *userdata, struct spa_io_position *position) {
    struct data *data = userdata;

    if (position == NULL) {
        return;
    }

    int n_samples = (int) position->clock.duration;

    if (position->clock.duration ^ n_samples) {
        printf("Warning: too many samples (%d) in one process call\n", n_samples);
    }

    int rate = position->clock.rate.denom;
    if (rate > 0 && rate != data->current_rate) {
        select_filter_for_rate(data, rate);
    }

    float *input_buffers[NUM_CHANNELS];
    float *output_buffers[NUM_CHANNELS];
    bool all_buffers_valid = true;

    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        input_buffers[ch] = pw_filter_get_dsp_buffer(data->channels[ch].in_port, n_samples);
        output_buffers[ch] = pw_filter_get_dsp_buffer(data->channels[ch].out_port, n_samples);

        if (!input_buffers[ch] || !output_buffers[ch]) {
            all_buffers_valid = false;
        }
    }

    if (!all_buffers_valid) {
        for (int ch = 0; ch < NUM_CHANNELS; ch++) {
            if (output_buffers[ch]) {
                memset(output_buffers[ch], 0, n_samples * sizeof(float));
            }
        }
        return;
    }

    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        struct channel *channel = &data->channels[ch];

        delay_line_append_samples(channel->current, channel->delay_line,
                                  input_buffers[ch], n_samples);

        fir_filter_apply(channel->current, channel->delay_line,
                         n_samples, output_buffers[ch]);
    }
}

static void on_filter_state_changed(void *userdata, enum pw_filter_state old,
                                    enum pw_filter_state state, const char *error) {
    struct data *data = userdata;
    printf("filter state: \"%s\"\n", pw_filter_state_as_string(state));

    switch (state) {
        case PW_FILTER_STATE_ERROR:
        case PW_FILTER_STATE_UNCONNECTED:
            pw_main_loop_quit(data->loop);
            break;
        default:
            break;
    }
}

static void on_filter_param_changed(void *userdata, void *user_data, uint32_t id, const struct spa_pod *param) {
    struct data *data = userdata;

    if (param == NULL || id != SPA_PARAM_Format)
        return;

    struct spa_audio_info info = {0};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_type != SPA_MEDIA_TYPE_audio || info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_audio_raw_parse(param, &info.info.raw) < 0)
        return;

    int rate = info.info.raw.rate;
    select_filter_for_rate(data, rate);
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = on_filter_state_changed,
    .param_changed = on_filter_param_changed,
    .process = on_filter_process,
};

static int create_channel_ports(struct data *data, int channel_idx) {
    const struct channel_config *config = &channel_configs[channel_idx];
    struct channel *channel = &data->channels[channel_idx];

    channel->in_port = pw_filter_add_port(data->filter,
                                          PW_DIRECTION_INPUT,
                                          PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                          PW_ID_ANY,
                                          pw_properties_new(
                                              PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                              PW_KEY_PORT_NAME, config->input_name,
                                              PW_KEY_AUDIO_CHANNEL, config->channel_name,
                                              NULL),
                                          NULL, 0);

    channel->out_port = pw_filter_add_port(data->filter,
                                           PW_DIRECTION_OUTPUT,
                                           PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                           PW_ID_ANY,
                                           pw_properties_new(
                                               PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                               PW_KEY_PORT_NAME, config->output_name,
                                               PW_KEY_AUDIO_CHANNEL, config->channel_name,
                                               NULL),
                                           NULL, 0);

    if (!channel->in_port || !channel->out_port) {
        fprintf(stderr, "Failed to create ports for channel %d\n", channel_idx);
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    struct data data = {0};

    pw_init(&argc, &argv);

    data.loop = pw_main_loop_new(NULL);

    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

    if (init_fir_filters(&data) != 0) {
        fprintf(stderr, "Failed to load FIR coefficients\n");
        return -1;
    }

    data.filter = pw_filter_new_simple(
        pw_main_loop_get_loop(data.loop),
        "JRX215 Comp Filter",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_NODE_DESCRIPTION, "FIR JRX215 Compensation Filter",
            NULL),
        &filter_events,
        &data);

    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        if (create_channel_ports(&data, ch) != 0) {
            fprintf(stderr, "Failed to create ports for channel %d\n", ch);
            pw_filter_destroy(data.filter);
            pw_main_loop_destroy(data.loop);
            cleanup_fir_filters(&data);
            pw_deinit();
            return -1;
        }
    }

    if (pw_filter_connect(data.filter,
                          PW_FILTER_FLAG_RT_PROCESS,
                          NULL, 0) < 0) {
        fprintf(stderr, "Failed to connect filter\n");
        return -1;
    }

    printf("PipeWire FIR filter started (Initial rate: %d Hz)\n", data.current_rate);
    printf("Use qpwgraph, pw-link, or other tools to connect audio sources and destinations.\n");

    pw_main_loop_run(data.loop);

    pw_filter_destroy(data.filter);
    pw_main_loop_destroy(data.loop);
    cleanup_fir_filters(&data);
    pw_deinit();

    return 0;
}
