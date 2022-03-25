#define _GNU_SOURCE
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

#include <tox/tox.h>
#include <tox/toxav.h>
#include <sodium.h>

static uint64_t start_time;
static bool signal_exit = false;

static const int32_t audio_bitrate = 48;
static const int32_t video_bitrate = 5000;
static const char *data_filename = "data";

static Tox *g_tox = NULL;
static ToxAV *g_toxAV = NULL;

struct tox_node {
	const char *key;
	const char *hostname;
	uint16_t port;
};

static void friend_cleanup(Tox *tox) {
	uint32_t friend_count = tox_self_get_friend_list_size(tox);
	if (friend_count == 0) {
		return;
	}

	uint32_t friends[friend_count];
	tox_self_get_friend_list(tox, friends);

	uint64_t curr_time = time(NULL);
	for (uint32_t i = 0; i < friend_count; i++) {
		TOX_ERR_FRIEND_GET_LAST_ONLINE err;
		uint32_t friend = friends[i];
		uint64_t last_online = tox_friend_get_last_online(tox, friend, &err);

		if (err != TOX_ERR_FRIEND_GET_LAST_ONLINE_OK) {
			fprintf(stderr, "Couldn't obtain 'last online' (this should never happen)\n");
			continue;
		}

		if (curr_time - last_online > 2629743) {
			fprintf(stderr, "Removing friend %d\n", friend);
			tox_friend_delete(tox, friend, NULL);
		}
	}
}

static bool save_profile(Tox *tox) {
	uint32_t save_size = tox_get_savedata_size(tox);
	uint8_t save_data[save_size];

	tox_get_savedata(tox, save_data);

	FILE *file = fopen(data_filename, "wb");
	if (file) {
		fwrite(save_data, sizeof(uint8_t), save_size, file);
		fclose(file);
		return true;
	} else {
		fprintf(stderr, "Could not write profile to disk\n");
		return false;
	}
}

static void *run_toxav(void *arg) {
	ToxAV *toxav = (ToxAV *)arg;
	fprintf(stderr, "Starting toxav thread\n");

	while (!signal_exit) {
		toxav_iterate(toxav);

		long long time = toxav_iteration_interval(toxav) * 1000000L;
		nanosleep((const struct timespec[]){{0, time}}, NULL);
	}

	fprintf(stderr, "Shut down toxav thread\n");
	return NULL;
}

static void *run_tox(void *arg) {
	Tox *tox = (Tox *)arg;
	fprintf(stderr, "Starting tox thread\n");

	uint64_t last_purge = 0;
	while (!signal_exit) {
		tox_iterate(tox, NULL);

		uint64_t curr_time = time(NULL);
		if (curr_time - last_purge > 1800) {
			friend_cleanup(tox);
			save_profile(tox);

			last_purge = curr_time;
		}

		long long time = tox_iteration_interval(tox) * 1000000L;
		nanosleep((const struct timespec[]){{0, time}}, NULL);
	}

	fprintf(stderr, "Shut down tox thread\n");
	return NULL;
}

static TOX_ERR_BOOTSTRAP bootstrap(Tox *tox, struct tox_node *node) {
	uint8_t key[TOX_PUBLIC_KEY_SIZE];
	sodium_hex2bin(key, sizeof(key), node->key, strlen(node->key), NULL, NULL, NULL);

	TOX_ERR_BOOTSTRAP err;
	tox_bootstrap(tox, node->hostname, node->port, key, &err);
	return err;
}

static void get_elapsed_time_str(char *buf, int bufsize, uint64_t secs) {
	long unsigned int minutes = (secs % 3600) / 60;
	long unsigned int hours = (secs / 3600) % 24;
	long unsigned int days = (secs / 3600) / 24;

	snprintf(buf, bufsize, "%lud %luh %lum", days, hours, minutes);
}

static bool file_exists(const char *filename) {
	return access(filename, 0) != -1;
}

static bool load_profile(Tox **tox, struct Tox_Options *options) {
	FILE *file = fopen(data_filename, "rb");

	if (file) {
		fseek(file, 0, SEEK_END);
		long file_size = ftell(file);
		fseek(file, 0, SEEK_SET);

		uint8_t *save_data = (uint8_t *)malloc(file_size * sizeof(uint8_t));
		fread(save_data, sizeof(uint8_t), file_size, file);
		fclose(file);

		options->savedata_data = save_data;
		options->savedata_type = TOX_SAVEDATA_TYPE_TOX_SAVE;
		options->savedata_length = file_size;

		TOX_ERR_NEW err;
		*tox = tox_new(options, &err);
		free(save_data);

		return err == TOX_ERR_NEW_OK;
	}

	return false;
}

static uint32_t get_online_friend_count(Tox *tox) {
	uint32_t friend_count = tox_self_get_friend_list_size(tox);
	uint32_t friends[friend_count];
	tox_self_get_friend_list(tox, friends);

	uint32_t online_friend_count = 0u;
	for (uint32_t i = 0; i < friend_count; i++) {
		if (tox_friend_get_connection_status(tox, friends[i], NULL) != TOX_CONNECTION_NONE) {
			online_friend_count++;
		}
	}

	return online_friend_count;
}

static void self_connection_status(Tox *tox, TOX_CONNECTION status, void *userData) {
	if (status == TOX_CONNECTION_NONE) {
		fprintf(stderr, "Lost connection to the tox network\n");
	} else {
		fprintf(stderr, "Connected to the tox network, status: %d\n", status);
	}
}

static void friend_request(Tox *tox, const uint8_t *public_key, const uint8_t *message, size_t length, void *user_data) {
	TOX_ERR_FRIEND_ADD err;
	tox_friend_add_norequest(tox, public_key, &err);

	if (err != TOX_ERR_FRIEND_ADD_OK) {
		fprintf(stderr, "Could not add friend, error: %d\n", err);
	} else {
		fprintf(stderr, "Added to our friend list\n");
	}

	save_profile(tox);
}

static void friend_message(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message, size_t length, void *user_data) {
	char dest_msg[length + 1];
	dest_msg[length] = '\0';
	memcpy(dest_msg, message, length);

	if (!strcmp("!info", dest_msg)) {
		char res_msg[TOX_MAX_MESSAGE_LENGTH];
		char time_str[64];
		uint64_t cur_time = time(NULL);

		get_elapsed_time_str(time_str, sizeof(time_str), cur_time - start_time);
		snprintf(res_msg, sizeof(res_msg), "Uptime: %s", time_str);
		tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *)res_msg, strlen(res_msg), NULL);

		snprintf(res_msg, sizeof(res_msg), "Toxcore: %llu.%llu.%llu", (long long unsigned int)tox_version_major(), (long long unsigned int)tox_version_minor(), (long long unsigned int)tox_version_patch());
		tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *)res_msg, strlen(res_msg), NULL);

		const char *github_msg = "Source: https://github.com/alexbakker/EchoBot";
		tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *)github_msg, strlen(github_msg), NULL);

		snprintf(res_msg, sizeof(res_msg), "Friends: %zu (%d online)", tox_self_get_friend_list_size(tox), get_online_friend_count(tox));
		tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *)res_msg, strlen(res_msg), NULL);

		const char *friend_info_msg = "Friends are removed after 1 month of inactivity";
		tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *)friend_info_msg, strlen(friend_info_msg), NULL);

		const char *info_msg = "If you're experiencing issues, contact alexbakker in #tox at Libera Chat";
		tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *)info_msg, strlen(info_msg), NULL);
	} else if (!strcmp("!callme", dest_msg)) {
		toxav_call(g_toxAV, friend_number, audio_bitrate, 0, NULL);
	} else if (!strcmp ("!videocallme", dest_msg)) {
		toxav_call (g_toxAV, friend_number, audio_bitrate, video_bitrate, NULL);
	} else {
		tox_friend_send_message (tox, friend_number, TOX_MESSAGE_TYPE_NORMAL, message, length, NULL);

		static const char *help_msg = "EchoBot commands:\n!info: Show stats.\n!callme: Launch an audio call.\n!videocallme: Launch a video call.";
		tox_friend_send_message (tox, friend_number, TOX_MESSAGE_TYPE_NORMAL, (uint8_t*) help_msg, strlen (help_msg), NULL);
	}
}

static void file_recv(Tox *tox, uint32_t friend_number, uint32_t file_number, uint32_t kind, uint64_t file_size, const uint8_t *filename, size_t filename_length, void *user_data) {
	if (kind == TOX_FILE_KIND_AVATAR) {
		return;
	}

	tox_file_control(tox, friend_number, file_number, TOX_FILE_CONTROL_CANCEL, NULL);

	const char *msg = "Sorry, I don't support file transfers.";
	tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL, (uint8_t*)msg, strlen(msg), NULL);
}

static void call(ToxAV *toxAV, uint32_t friend_number, bool audio_enabled, bool video_enabled, void *user_data) {
	TOXAV_ERR_ANSWER err;
	toxav_answer(toxAV, friend_number, audio_enabled ? audio_bitrate : 0, video_enabled ? video_bitrate : 0, &err);

	if (err != TOXAV_ERR_ANSWER_OK) {
		fprintf(stderr, "Could not answer call, friend: %d, error: %d\n", friend_number, err);
	}
}

static void call_state(ToxAV *toxAV, uint32_t friend_number, uint32_t state, void *user_data) {
	if (state & TOXAV_FRIEND_CALL_STATE_FINISHED) {
		fprintf(stderr, "Call with friend %d finished\n", friend_number);
		return;
	} else if (state & TOXAV_FRIEND_CALL_STATE_ERROR) {
		fprintf(stderr, "Call with friend %d errored\n", friend_number);
		return;
	}

	bool send_audio = (state & TOXAV_FRIEND_CALL_STATE_SENDING_A) && (state & TOXAV_FRIEND_CALL_STATE_ACCEPTING_A);
	bool send_video = state & TOXAV_FRIEND_CALL_STATE_SENDING_V && (state & TOXAV_FRIEND_CALL_STATE_ACCEPTING_V);
	toxav_audio_set_bit_rate(toxAV, friend_number, send_audio ? audio_bitrate : 0, NULL);
	toxav_video_set_bit_rate(toxAV, friend_number, send_video ? video_bitrate : 0, NULL);

	fprintf(stderr, "Call state for friend %d changed to %d: audio: %d, video: %d\n", friend_number, state, send_audio, send_video);
}

static void audio_receive_frame(ToxAV *toxAV, uint32_t friend_number, const int16_t *pcm, size_t sample_count, uint8_t channels, uint32_t sampling_rate, void *user_data) {
	TOXAV_ERR_SEND_FRAME err;
	toxav_audio_send_frame(toxAV, friend_number, pcm, sample_count, channels, sampling_rate, &err);

	if (err != TOXAV_ERR_SEND_FRAME_OK) {
		fprintf(stderr, "Could not send audio frame to friend: %d, error: %d\n", friend_number, err);
	}
}

static void video_receive_frame(ToxAV *toxAV, uint32_t friend_number, uint16_t width, uint16_t height, const uint8_t *y, const uint8_t *u, const uint8_t *v, int32_t ystride, int32_t ustride, int32_t vstride, void *user_data) {
	ystride = abs(ystride);
	ustride = abs(ustride);
	vstride = abs(vstride);
	if (ystride < width || ustride < width / 2 || vstride < width / 2) {
		return;
	}

	uint8_t *y_dest = (uint8_t *)malloc(width * height);
	uint8_t *u_dest = (uint8_t *)malloc(width * height / 2);
	uint8_t *v_dest = (uint8_t *)malloc(width * height / 2);

	for (size_t h = 0; h < height; h++) {
		memcpy(&y_dest[h * width], &y[h * ystride], width);
	}

	for (size_t h = 0; h < height / 2; h++) {
		memcpy(&u_dest[h * width / 2], &u[h * ustride], width / 2);
		memcpy(&v_dest[h * width / 2], &v[h * vstride], width / 2);
	}

	TOXAV_ERR_SEND_FRAME err;
	toxav_video_send_frame(toxAV, friend_number, width, height, y_dest, u_dest, v_dest, &err);

	free(y_dest);
	free(u_dest);
	free(v_dest);

	if (err != TOXAV_ERR_SEND_FRAME_OK) {
		fprintf(stderr, "Could not send video frame to friend: %d, error: %d\n", friend_number, err);
	}
}

static const char *tox_log_level_name(Tox_Log_Level level)
{
    switch (level) {
        case TOX_LOG_LEVEL_TRACE:
            return "TRACE";
        case TOX_LOG_LEVEL_DEBUG:
            return "DEBUG";
        case TOX_LOG_LEVEL_INFO:
            return "INFO";
        case TOX_LOG_LEVEL_WARNING:
            return "WARNING";
        case TOX_LOG_LEVEL_ERROR:
            return "ERROR";
    }

    return "UNKNOWN";
}

static void tox_log(Tox *tox, Tox_Log_Level level, const char *file, uint32_t line, const char *func, const char *message, void *user_data) {
	const char *level_name = tox_log_level_name(level);
	fprintf(stderr, "[%s] [%s:%d] %s\n", level_name, file, line, message);
}

int main(int argc, char *argv[]) {
	start_time = time(NULL);

	TOX_ERR_NEW err = TOX_ERR_NEW_OK;
	struct Tox_Options options;
	tox_options_default(&options);
	tox_options_set_log_callback(&options, tox_log);

	if (file_exists(data_filename)) {
		if (load_profile(&g_tox, &options)) {
			fprintf(stderr, "Loaded profile from disk\n");
		} else {
			fprintf(stderr, "Failed to load profile from disk\n");
			return -1;
		}
	} else {
		fprintf(stderr, "Creating a new profile\n");

		g_tox = tox_new(&options, &err);
		save_profile(g_tox);
	}

	tox_callback_self_connection_status(g_tox, self_connection_status);
	tox_callback_friend_request(g_tox, friend_request);
	tox_callback_friend_message(g_tox, friend_message);
	tox_callback_file_recv(g_tox, file_recv);

	if (err != TOX_ERR_NEW_OK) {
		fprintf(stderr, "Error returned by tox_new: %d\n", err);
		return -1;
	}

	uint8_t address_bin[TOX_ADDRESS_SIZE];
	tox_self_get_address(g_tox, (uint8_t *)address_bin);
	char address_hex[TOX_ADDRESS_SIZE * 2 + 1];
	sodium_bin2hex(address_hex, sizeof(address_hex), address_bin, sizeof(address_bin));

	fprintf(stderr, "Our Tox ID: %s\n", address_hex);

	const char *name = "EchoBot";
	const char *status_msg = "Tox audio/video testing service. Send '!info' for stats.";

	tox_self_set_name(g_tox, (uint8_t *)name, strlen(name), NULL);
	tox_self_set_status_message(g_tox, (uint8_t *)status_msg, strlen(status_msg), NULL);

	struct tox_node nodes[] = {
		{"7A6098B590BDC73F9723FC59F82B3F9085A64D1B213AAF8E610FD351930D052D", "tox2.abilinski.com", 33445},
		{"3F0A45A268367C1BEA652F258C85F4A66DA76BCAA667A49E770BCC4917AB6A25", "tox.initramfs.io", 33445},
		{"DA4E4ED4B697F2E9B000EEFE3A34B554ACD3F45F5C96EAEA2516DD7FF9AF7B43", "85.143.221.42", 33445},
		{"1C5293AEF2114717547B39DA8EA6F1E331E5E358B35F9B6B5F19317911C5F976", "tox.verdict.gg", 33445},
		{"BEF0CFB37AF874BD17B9A8F9FE64C75521DB95A37D33C5BDB00E9CF58659C04F", "198.199.98.108", 33445},
		{"82EF82BA33445A1F91A7DB27189ECFC0C013E06E3DA71F588ED692BED625EC23", "tox.kurnevsky.net", 33445},
		{"B3E5FA80DC8EBD1149AD2AB35ED8B85BD546DEDE261CA593234C619249419506", "tox1.mf-net.eu", 33445}
	};

	bool bootstrap_success = false;
	for (size_t i = 0; i < sizeof(nodes) / sizeof(struct tox_node); i++) {
		struct tox_node *node = &nodes[i];
		fprintf(stderr, "Bootstrapping from node: %s\n", node->hostname);

		TOX_ERR_BOOTSTRAP err = bootstrap(g_tox, node);
		if (err != TOX_ERR_BOOTSTRAP_OK) {
			fprintf(stderr, "Could not bootstrap from %s: %d\n", node->hostname, err);
		} else {
			bootstrap_success = true;
		}
	}
	if (!bootstrap_success) {
		fprintf(stderr, "Could not bootstrap from any nodes");
		return -1;
	}

	TOXAV_ERR_NEW err2;
	g_toxAV = toxav_new(g_tox, &err2);
	toxav_callback_call(g_toxAV, call, NULL);
	toxav_callback_call_state(g_toxAV, call_state, NULL);
	toxav_callback_audio_receive_frame(g_toxAV, audio_receive_frame, NULL);
	toxav_callback_video_receive_frame(g_toxAV, video_receive_frame, NULL);

	if (err2 != TOXAV_ERR_NEW_OK) {
		fprintf(stderr, "Error returned by toxav_new: %d\n", err);
		return -1;
	}

	sigset_t sig_set;
	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGTERM);
	sigaddset(&sig_set, SIGINT);

	pthread_t tox_thread, toxav_thread;
	pthread_sigmask(SIG_BLOCK, &sig_set, NULL);
	pthread_create(&tox_thread, NULL, &run_tox, g_tox);
	pthread_setname_np(tox_thread, "echobot:tox");
	pthread_create(&toxav_thread, NULL, &run_toxav, g_toxAV);
	pthread_setname_np(toxav_thread, "echobot:toxav");

	int sig;
	sigwait(&sig_set, &sig);
	fprintf(stderr, "Shutdown signal received\n");
	signal_exit = true;
	fprintf(stderr, "Waiting for tox and toxav threads to finish\n");
	pthread_join(tox_thread, NULL);
	pthread_join(toxav_thread, NULL);

	fprintf(stderr, "Saving profile to disk and killing tox/toxav\n");
	save_profile(g_tox);
	toxav_kill(g_toxAV);
	tox_kill(g_tox);
	return 0;
}
