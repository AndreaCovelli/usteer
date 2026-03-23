/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 */

/*
 * Host tests for the list options handled by set_config and for the station
 * policy exemption list.
 *
 * The daemon sources are compiled into this translation unit, so the tests
 * run the real configuration, policy and ubus request code, including the
 * static handlers. Only the ubus transport, the timers and the remote node
 * code are replaced. Run with tests/run.sh, which also builds a variant with
 * AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer.
 *
 * All station addresses are locally administered test addresses.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../usteer.h"
#include "../node.h"
#include "../event.h"

#undef __usteer_init
#define __usteer_init

/* main.c provides the globals and the real usteer_init_defaults() */
#define main usteerd_main
#define debug_msg usteerd_debug_msg
#define debug_msg_cont usteerd_debug_msg_cont
#define usteer_update_time usteerd_update_time
#include "../main.c"
#undef main
#undef debug_msg
#undef debug_msg_cont
#undef usteer_update_time

#include "../sta.c"
#include "../policy.c"
#define b local_node_blob
#include "../local_node.c"
#undef b
#include "../ubus.c"
#include "../band_steering.c"

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		exit(1); \
	} \
} while (0)

#define NSTA 2
#define EXEMPT 0
#define CONTROL 1

static const char *const addresses[NSTA] = {
	"02:00:00:00:00:0a",
	"02:00:00:00:00:0b",
};

LIST_HEAD(remote_nodes);
struct avl_tree remote_hosts;

static struct usteer_local_node ap, ap5;
static struct sta_info *infos[NSTA];
static unsigned int calls[NSTA], kicks[NSTA], tm[NSTA], lm[NSTA], scans[NSTA];
static unsigned int n_exempt_log;
static struct blob_attr *reply;
static struct blob_buf in;

/* Logging: count the messages about rejected exemption list entries */
void debug_msg(int level, const char *func, int line, const char *format, ...)
{
	if (config.debug_level < level)
		return;

	if (strstr(format, "policy_exempt_sta_list"))
		n_exempt_log++;
}

void debug_msg_cont(int level, const char *format, ...) {}

/* Time only moves when a test moves it */
void usteer_update_time(void) {}

/* Timers, events, measurements and remote nodes are not under test */
void usteer_event(struct uevent *ev) {}
void usteer_timeout_init(struct usteer_timeout_queue *q) {}
void usteer_timeout_set(struct usteer_timeout_queue *q, struct usteer_timeout *t, int msecs) {}
void usteer_timeout_cancel(struct usteer_timeout_queue *q, struct usteer_timeout *t) {}
void usteer_send_sta_update(struct sta_info *si) {}
void usteer_measurement_report_sta_cleanup(struct sta *sta) {}
void usteer_measurement_report_node_cleanup(struct usteer_node *node) {}
int usteer_measurement_get_rssi(struct usteer_measurement_report *report) { abort(); }
struct usteer_measurement_report *
usteer_measurement_report_add(struct sta *sta, struct usteer_node *node,
			      uint8_t rcpi, uint8_t rsni, uint64_t timestamp) { abort(); }
struct usteer_node *usteer_node_by_bssid(uint8_t *bssid) { abort(); }
struct usteer_node *
usteer_node_get_next_neighbor(struct usteer_node *current_node, struct usteer_node *last) { return NULL; }
void usteer_node_set_blob(struct blob_attr **dest, struct blob_attr *val) { abort(); }
int usteer_interface_init(void) { return 0; }
void usteer_interface_add(const char *name) {}
void config_set_interfaces(struct blob_attr *data) {}
void config_get_interfaces(struct blob_buf *buf) {}
void config_set_event_log_types(struct blob_attr *attr) {}
void config_get_event_log_types(struct blob_buf *buf) {}

/* ubus transport: the daemon never reaches a bus in these tests */
struct ubus_context *ubus_connect(const char *path) { abort(); }
int ubus_register_event_handler(struct ubus_context *ctx, struct ubus_event_handler *ev,
				const char *pattern) { abort(); }
int ubus_lookup(struct ubus_context *ctx, const char *path, ubus_lookup_handler_t cb,
		void *priv) { abort(); }
int ubus_add_object(struct ubus_context *ctx, struct ubus_object *obj) { abort(); }
int ubus_remove_object(struct ubus_context *ctx, struct ubus_object *obj) { abort(); }
int ubus_register_subscriber(struct ubus_context *ctx, struct ubus_subscriber *obj) { abort(); }
int ubus_subscribe(struct ubus_context *ctx, struct ubus_subscriber *obj, uint32_t id) { abort(); }
int ubus_unsubscribe(struct ubus_context *ctx, struct ubus_subscriber *obj, uint32_t id) { abort(); }
void ubus_abort_request(struct ubus_context *ctx, struct ubus_request *req) {}
void ubus_complete_request_async(struct ubus_context *ctx, struct ubus_request *req) {}
int ubus_invoke_async_fd(struct ubus_context *ctx, uint32_t obj, const char *method,
			 struct blob_attr *msg, struct ubus_request *req, int fd) { return 0; }

static struct blob_attr *field(struct blob_attr *table, const char *name)
{
	struct blob_attr *cur;
	int rem;

	blobmsg_for_each_attr(cur, table, rem) {
		if (!strcmp(blobmsg_name(cur), name))
			return cur;
	}

	return NULL;
}

/* Every request to hostapd ends here: count it per station and method */
int ubus_invoke_fd(struct ubus_context *ctx, uint32_t obj, const char *method,
		   struct blob_attr *msg, ubus_data_handler_t cb, void *priv,
		   int timeout, int fd)
{
	struct blob_attr *addr = field(msg, "addr");
	int i;

	CHECK(addr);
	for (i = 0; i < NSTA; i++) {
		if (strcmp(blobmsg_get_string(addr), addresses[i]))
			continue;

		calls[i]++;
		if (!strcmp(method, "del_client"))
			kicks[i]++;
		else if (!strcmp(method, "bss_transition_request"))
			tm[i]++;
		else if (!strcmp(method, "link_measurement_req"))
			lm[i]++;
		else if (!strcmp(method, "rrm_beacon_req"))
			scans[i]++;
		else
			CHECK(!"unexpected hostapd method");

		return 0;
	}

	CHECK(!"request for an unknown station");
	return 0;
}

int ubus_send_reply(struct ubus_context *ctx, struct ubus_request_data *req,
		    struct blob_attr *msg)
{
	free(reply);
	reply = blob_memdup(msg);
	CHECK(reply);

	return 0;
}

/*
 * Build a config message the way the init script does. The entries of a list
 * are passed as NULL-terminated arguments.
 */
static void cfg_begin(void)
{
	void *a;

	blob_buf_init(&in, 0);
	blobmsg_add_u32(&in, "debug_level", MSG_INFO);
	a = blobmsg_open_array(&in, "ssid_list");
	blobmsg_add_string(&in, NULL, "test");
	blobmsg_close_array(&in, a);
}

static void cfg_list(const char *name, ...)
{
	const char *entry;
	va_list args;
	void *a;

	a = blobmsg_open_array(&in, name);
	va_start(args, name);
	while ((entry = va_arg(args, const char *)) != NULL)
		blobmsg_add_string(&in, NULL, entry);
	va_end(args);
	blobmsg_close_array(&in, a);
}

static void cfg_apply(const char *method)
{
	CHECK(!usteer_ubus_set_config(NULL, NULL, NULL, method, in.head));
}

static void get_config(void)
{
	CHECK(!usteer_ubus_get_config(NULL, NULL, NULL, "get_config", NULL));
}

static void reset_counters(void)
{
	memset(calls, 0, sizeof(calls));
	memset(kicks, 0, sizeof(kicks));
	memset(tm, 0, sizeof(tm));
	memset(lm, 0, sizeof(lm));
	memset(scans, 0, sizeof(scans));
}

static void node_init(struct usteer_local_node *ln, const char *name, int freq)
{
	memset(ln, 0, sizeof(*ln));
	ln->node.avl.key = name;
	ln->node.freq = freq;
	ln->node.noise = -95;
	ln->node.n_assoc = NSTA;
	ln->beacon_interval = 100;
	strcpy(ln->node.ssid, "test");
	INIT_LIST_HEAD(&ln->node.sta_info);
	INIT_LIST_HEAD(&ln->node.measurements);
	INIT_LIST_HEAD(&ln->bss_tm_queries);
	avl_insert(&local_nodes, &ln->node.avl);
}

static void teardown(void)
{
	struct usteer_bss_tm_query *query, *tmp;

	if (!ap.node.avl.key)
		return;

	list_for_each_entry_safe(query, tmp, &ap.bss_tm_queries, list) {
		list_del(&query->list);
		free(query);
	}
	usteer_sta_node_cleanup(&ap.node);
	usteer_sta_node_cleanup(&ap5.node);
}

static uint8_t *station_addr(int i)
{
	uint8_t *addr = (uint8_t *) ether_aton(addresses[i]);

	CHECK(addr);

	return addr;
}

/* Timings short enough for the periodic passes to act within a few rounds */
static void fast_timings(void)
{
	config.local_sta_update = 1;
	config.roam_scan_interval = 1000;
	config.roam_trigger_interval = 1;
	config.link_measurement_interval = 1;
	config.band_steering_interval = 0;
}

/*
 * A 2.4 GHz node with two connected stations and an empty 5 GHz node. The
 * first station is exempt and configured for the most aggressive steering,
 * the second one is the control and uses the defaults.
 */
static void fixture(void)
{
	bool create;
	int i;

	teardown();
	avl_init(&local_nodes, avl_strcmp, false, NULL);
	node_init(&ap, "hostapd.test24", 2412);
	node_init(&ap5, "hostapd.test5", 5180);

	cfg_begin();
	cfg_list("policy_exempt_sta_list", "02:00:00:00:00:0A", NULL);
	cfg_list("aggressiveness_mac_list", "02:00:00:00:00:0a,4", NULL);
	cfg_apply("set_config");
	fast_timings();
	current_time = 1000000;
	reset_counters();

	for (i = 0; i < NSTA; i++) {
		struct sta *sta = usteer_sta_get(station_addr(i), true);
		struct sta_info *si = usteer_sta_info_get(sta, &ap.node, &create);

		si->connected = STA_CONNECTED;
		si->connected_since = 1;
		si->signal = -80;
		si->seen = current_time;
		/* link measurement and active beacon measurement */
		si->rrm = (1 << 0) | (1 << 5);
		si->bss_transition = true;
		infos[i] = si;
	}

	CHECK(infos[EXEMPT]->sta->aggressiveness == 4);
	CHECK(infos[CONTROL]->sta->aggressiveness == config.aggressiveness);
}

static bool exempt(int i)
{
	return usteer_sta_is_policy_exempt(infos[i]->sta->addr);
}

static void test_exempt_list(void)
{
	void *a;

	fixture();
	CHECK(exempt(EXEMPT) && !exempt(CONTROL));
	CHECK(!usteer_sta_is_policy_exempt(NULL));
	CHECK(!ap.node.disabled && !ap5.node.disabled);
	get_config();
	CHECK(field(reply, "policy_exempt_sta_list"));
	CHECK(field(reply, "ssid_list"));

	/* A reload replaces the list */
	cfg_begin();
	cfg_list("policy_exempt_sta_list", addresses[CONTROL], NULL);
	cfg_apply("set_config");
	CHECK(!exempt(EXEMPT) && exempt(CONTROL));

	/* A reload without the option clears it */
	cfg_begin();
	cfg_apply("set_config");
	CHECK(!exempt(EXEMPT) && !exempt(CONTROL));
	get_config();
	CHECK(!field(reply, "policy_exempt_sta_list"));

	/* The init script sends an empty array when the option is unset */
	cfg_begin();
	cfg_list("policy_exempt_sta_list", addresses[EXEMPT], addresses[CONTROL], NULL);
	cfg_apply("set_config");
	CHECK(exempt(EXEMPT) && exempt(CONTROL));
	cfg_begin();
	cfg_list("policy_exempt_sta_list", NULL);
	cfg_apply("set_config");
	CHECK(!exempt(EXEMPT) && !exempt(CONTROL));

	/* Invalid entries are logged and skipped, the valid one still applies */
	n_exempt_log = 0;
	cfg_begin();
	cfg_list("policy_exempt_sta_list", "invalid", "02-00-00-00-00-0a", "",
		 addresses[CONTROL], "zz:00:00:00:00:0a", NULL);
	cfg_apply("set_config");
	CHECK(!exempt(EXEMPT) && exempt(CONTROL));
	CHECK(n_exempt_log == 4);

	/* So are entries that are not strings */
	n_exempt_log = 0;
	cfg_begin();
	a = blobmsg_open_array(&in, "policy_exempt_sta_list");
	blobmsg_add_u32(&in, NULL, 7);
	blobmsg_add_string(&in, NULL, addresses[EXEMPT]);
	blobmsg_close_array(&in, a);
	cfg_apply("set_config");
	CHECK(exempt(EXEMPT) && !exempt(CONTROL));
	CHECK(n_exempt_log == 1);

	/* ... and string entries without a terminating NUL */
	n_exempt_log = 0;
	cfg_begin();
	a = blobmsg_open_array(&in, "policy_exempt_sta_list");
	blobmsg_add_field(&in, BLOBMSG_TYPE_STRING, NULL, addresses[EXEMPT],
			  strlen(addresses[EXEMPT]));
	blobmsg_close_array(&in, a);
	cfg_apply("set_config");
	CHECK(!exempt(EXEMPT) && !exempt(CONTROL));
	CHECK(n_exempt_log == 1);

	/*
	 * update_config replaces every list option, like it does for
	 * ssid_list: a message that does not carry the list clears it.
	 */
	cfg_begin();
	cfg_list("policy_exempt_sta_list", addresses[EXEMPT], NULL);
	cfg_apply("update_config");
	CHECK(exempt(EXEMPT));
	cfg_begin();
	cfg_apply("update_config");
	CHECK(!exempt(EXEMPT));

	puts("ok - exemption list: parse, replace, clear, invalid entries");
}

static uint32_t aggressiveness_of(const char *addr_str)
{
	uint8_t *addr = (uint8_t *) ether_aton(addr_str);
	struct sta_info *si;
	struct sta *sta;
	bool create;

	CHECK(addr);
	sta = usteer_sta_get(addr, true);
	CHECK(sta);
	si = usteer_sta_info_get(sta, &ap5.node, &create);
	CHECK(si && create);

	return sta->aggressiveness;
}

/* A station that is already known: no entry is created for it */
static uint32_t known_aggressiveness_of(const char *addr_str)
{
	uint8_t *addr = (uint8_t *) ether_aton(addr_str);
	struct sta *sta;

	CHECK(addr);
	sta = usteer_sta_get(addr, false);
	CHECK(sta);

	return sta->aggressiveness;
}

static void test_aggressiveness_list(void)
{
	void *a;

	fixture();
	get_config();
	CHECK(field(reply, "aggressiveness_mac_list"));

	/*
	 * Entries that are too long, not strings or not terminated must be
	 * skipped without stopping the lookup. The long entry used to be
	 * copied into a 20-byte stack buffer.
	 */
	cfg_begin();
	a = blobmsg_open_array(&in, "aggressiveness_mac_list");
	blobmsg_add_string(&in, NULL,
			   "02:00:00:00:00:1a,4 followed by far more text than the "
			   "nineteen characters of a well-formed entry can hold");
	blobmsg_add_u32(&in, NULL, 7);
	blobmsg_add_field(&in, BLOBMSG_TYPE_STRING, NULL, "02:00:00:00:00:1a,4", 19);
	blobmsg_add_string(&in, NULL, "02:00:00:00:00:1a");
	blobmsg_add_string(&in, NULL, "");
	blobmsg_add_string(&in, NULL, "02:00:00:00:00:1a,1");
	blobmsg_add_string(&in, NULL, "02:00:00:00:00:1a,2");
	blobmsg_add_string(&in, NULL, "02:00:00:00:00:1b,0");
	blobmsg_close_array(&in, a);
	cfg_apply("set_config");

	/* first well-formed match wins, other stations use the default */
	CHECK(aggressiveness_of("02:00:00:00:00:1a") == 1);
	CHECK(aggressiveness_of("02:00:00:00:00:1b") == 0);
	CHECK(config.aggressiveness == 3);
	CHECK(aggressiveness_of("02:00:00:00:00:1c") == 3);

	/* A reload without the option clears the list */
	cfg_begin();
	cfg_apply("set_config");
	get_config();
	CHECK(!field(reply, "aggressiveness_mac_list"));
	CHECK(aggressiveness_of("02:00:00:00:00:1d") == 3);

	/* ... also for the stations that were created while it was set */
	CHECK(known_aggressiveness_of("02:00:00:00:00:1a") == 3);
	CHECK(known_aggressiveness_of("02:00:00:00:00:1b") == 3);

	/*
	 * A reload reaches the stations that are already known: a new entry,
	 * a changed entry, a removed entry and a new default.
	 */
	cfg_begin();
	cfg_list("aggressiveness_mac_list", "02:00:00:00:00:1a,2",
		 "02:00:00:00:00:1c,4", NULL);
	cfg_apply("set_config");
	CHECK(known_aggressiveness_of("02:00:00:00:00:1a") == 2);
	CHECK(known_aggressiveness_of("02:00:00:00:00:1b") == 3);
	CHECK(known_aggressiveness_of("02:00:00:00:00:1c") == 4);

	cfg_begin();
	blobmsg_add_u32(&in, "aggressiveness", 1);
	cfg_list("aggressiveness_mac_list", "02:00:00:00:00:1a,0", NULL);
	cfg_apply("set_config");
	CHECK(known_aggressiveness_of("02:00:00:00:00:1a") == 0);
	CHECK(known_aggressiveness_of("02:00:00:00:00:1b") == 1);
	CHECK(known_aggressiveness_of("02:00:00:00:00:1c") == 1);
	CHECK(known_aggressiveness_of("02:00:00:00:00:1d") == 1);

	/* update_config keeps the default it does not mention */
	cfg_begin();
	cfg_list("aggressiveness_mac_list", "02:00:00:00:00:1b,4", NULL);
	cfg_apply("update_config");
	CHECK(config.aggressiveness == 1);
	CHECK(known_aggressiveness_of("02:00:00:00:00:1a") == 1);
	CHECK(known_aggressiveness_of("02:00:00:00:00:1b") == 4);

	puts("ok - aggressiveness list: malformed entries, lookup, clear, reload");
}

/*
 * The init script calls set_config on every reload, including the one
 * triggered by each interface event. Every list must be released when it is
 * replaced; the sanitizer build fails on any copy that is left behind.
 */
static void test_reload(void)
{
	int i;

	fixture();
	for (i = 0; i < 50; i++) {
		cfg_begin();
		cfg_list("policy_exempt_sta_list", addresses[EXEMPT], addresses[CONTROL], NULL);
		cfg_list("aggressiveness_mac_list", "02:00:00:00:00:0a,1", NULL);
		cfg_apply(i % 5 ? "set_config" : "update_config");
	}
	CHECK(exempt(EXEMPT) && exempt(CONTROL));
	get_config();
	CHECK(field(reply, "ssid_list"));
	CHECK(field(reply, "policy_exempt_sta_list"));
	CHECK(field(reply, "aggressiveness_mac_list"));

	puts("ok - repeated reloads keep exactly one copy of each list");
}

static void test_requests(void)
{
	fixture();
	/* 50 dB above the noise floor is -45 dBm, both stations are at -80 */
	config.min_snr = config.min_connect_snr = 50;
	config.probe_steering = config.assoc_steering = true;

	CHECK(usteer_check_request(infos[EXEMPT], EVENT_TYPE_PROBE));
	CHECK(usteer_check_request(infos[EXEMPT], EVENT_TYPE_ASSOC));
	CHECK(usteer_check_request(infos[EXEMPT], EVENT_TYPE_AUTH));
	CHECK(!usteer_check_request(infos[CONTROL], EVENT_TYPE_PROBE));
	CHECK(!usteer_check_request(infos[CONTROL], EVENT_TYPE_ASSOC));
	CHECK(usteer_check_request(infos[CONTROL], EVENT_TYPE_AUTH));

	/* The event path, with stations a node sees for the first time */
	CHECK(usteer_handle_sta_event(&ap5.node, infos[EXEMPT]->sta->addr,
				      EVENT_TYPE_ASSOC, 5180, -90));
	CHECK(!usteer_handle_sta_event(&ap5.node, infos[CONTROL]->sta->addr,
				       EVENT_TYPE_ASSOC, 5180, -90));

	CHECK(!usteer_policy_can_perform_roam(infos[EXEMPT]));
	CHECK(usteer_policy_can_perform_roam(infos[CONTROL]));

	puts("ok - probe and association admission, roam eligibility");
}

static void run_kick_passes(int n)
{
	while (n--)
		usteer_local_node_kick(&ap);
}

static void test_kicks(void)
{
	/* Signal below min_snr */
	fixture();
	config.min_snr = 50;
	config.min_snr_kick_delay = 1;
	run_kick_passes(5);
	CHECK(!calls[EXEMPT] && kicks[CONTROL] == 1);
	CHECK(infos[EXEMPT]->connected == STA_CONNECTED);
	CHECK(!infos[EXEMPT]->below_min_snr && !infos[EXEMPT]->kick_count);

	/*
	 * Node above the load threshold. The exempt station has the weaker
	 * signal, which would make it the first choice: it must not shadow
	 * the station that can be kicked.
	 */
	fixture();
	infos[EXEMPT]->signal = -90;
	config.load_kick_enabled = true;
	config.load_kick_threshold = 50;
	config.load_kick_delay = 1;
	config.load_kick_min_clients = 1;
	ap.node.load = 90;
	run_kick_passes(5);
	CHECK(!calls[EXEMPT] && kicks[CONTROL] == 1);
	CHECK(infos[EXEMPT]->connected == STA_CONNECTED);
	CHECK(!infos[EXEMPT]->kick_count);

	/* Only the exempt station is left on the loaded node */
	infos[CONTROL]->connected = STA_NOT_CONNECTED;
	reset_counters();
	run_kick_passes(5);
	CHECK(!calls[EXEMPT] && !calls[CONTROL]);

	/* A station becomes exempt while its kick is already scheduled */
	fixture();
	cfg_begin();
	cfg_apply("set_config");
	fast_timings();
	CHECK(!exempt(EXEMPT));
	infos[EXEMPT]->kick_time = infos[CONTROL]->kick_time = current_time - 1;
	cfg_begin();
	cfg_list("policy_exempt_sta_list", addresses[EXEMPT], NULL);
	cfg_apply("update_config");
	run_kick_passes(1);
	CHECK(!calls[EXEMPT] && kicks[CONTROL] == 1);
	CHECK(!infos[EXEMPT]->kick_time);
	CHECK(infos[EXEMPT]->connected == STA_CONNECTED);

	puts("ok - low signal, load and scheduled kicks");
}

/* The station is heard by the 5 GHz node of the same router */
static struct sta_info *hear_on_5ghz(int i)
{
	struct sta_info *si;
	bool create;

	si = usteer_sta_info_get(infos[i]->sta, &ap5.node, &create);
	CHECK(si);
	usteer_sta_info_update(si, -50, false);

	return si;
}

static void test_roaming(void)
{
	struct sta_info *other[NSTA];
	bool create;
	int i, round;

	/* Both stations hear the 5 GHz node much better than their own */
	fixture();
	config.roam_scan_snr = 30;
	config.roam_trigger_snr = 25;
	config.signal_diff_threshold = 5;
	config.roam_kick_delay = 1;
	for (i = 0; i < NSTA; i++) {
		other[i] = usteer_sta_info_get(infos[i]->sta, &ap5.node, &create);
		other[i]->signal = -50;
	}
	for (round = 0; round < 40; round++) {
		current_time += 2000;
		for (i = 0; i < NSTA; i++)
			other[i]->seen = current_time;
		run_kick_passes(1);
	}
	CHECK(!calls[EXEMPT]);
	CHECK(infos[EXEMPT]->roam_state == ROAM_TRIGGER_IDLE);
	CHECK(infos[EXEMPT]->connected == STA_CONNECTED);
	CHECK(tm[CONTROL] >= 1 && kicks[CONTROL] >= 1);

	/* Band steering from 2.4 GHz to 5 GHz */
	fixture();
	config.band_steering_interval = 1;
	config.band_steering_min_snr = -90;
	for (i = 0; i < NSTA; i++)
		other[i] = hear_on_5ghz(i);
	for (round = 0; round < 4; round++)
		usteer_band_steering_perform_steer(&ap);
	CHECK(!calls[EXEMPT] && tm[CONTROL] >= 1);

	puts("ok - roam state machine and band steering");
}

/*
 * Band steering on the 2.4 GHz node with a signal threshold. The control
 * station is first heard while it is not connected, like a station that
 * probes before it associates, so that its reference signal is taken when
 * it connects.
 */
static void band_steering_setup(uint32_t signal_threshold)
{
	struct sta_info *si;

	fixture();
	cfg_begin();
	blobmsg_add_u32(&in, "band_steering_interval", 1);
	blobmsg_add_u32(&in, "band_steering_min_snr", (uint32_t) -90);
	blobmsg_add_u32(&in, "band_steering_signal_threshold", signal_threshold);
	cfg_apply("update_config");

	si = infos[CONTROL];
	si->connected = STA_NOT_CONNECTED;
	usteer_sta_info_update(si, -80, false);
	si->connected = STA_CONNECTED;
}

/* The station reports a signal, then the node looks for stations to steer */
static void band_steering_rounds(int signal, int n)
{
	while (n--) {
		usteer_sta_info_update(infos[CONTROL], signal, true);
		usteer_band_steering_perform_steer(&ap);
	}
}

static void test_band_steering(void)
{
	struct blob_attr *attr;

	/* The signal threshold reaches the daemon and is reported back */
	fixture();
	cfg_begin();
	blobmsg_add_u32(&in, "band_steering_signal_threshold", 7);
	cfg_apply("set_config");
	CHECK(config.band_steering_signal_threshold == 7);
	get_config();
	attr = field(reply, "band_steering_signal_threshold");
	CHECK(attr && blobmsg_get_u32(attr) == 7);

	/*
	 * With a threshold, a station whose signal stays where it was when it
	 * connected is left alone ...
	 */
	band_steering_setup(5);
	hear_on_5ghz(CONTROL);
	band_steering_rounds(-80, 6);
	CHECK(!calls[CONTROL] && !infos[CONTROL]->kick_time);

	/* ... until the signal is better by the threshold */
	band_steering_rounds(-75, 4);
	CHECK(tm[CONTROL] == 1 && calls[CONTROL] == 1);

	/* Without a threshold it is steered in the first interval */
	band_steering_setup(0);
	hear_on_5ghz(CONTROL);
	band_steering_rounds(-80, 2);
	CHECK(tm[CONTROL] == 1 && calls[CONTROL] == 1);

	/*
	 * A station that no 5 GHz node of this router has heard may only
	 * support 2.4 GHz: it gets no request and no kick is scheduled ...
	 */
	band_steering_setup(0);
	CHECK(!infos[CONTROL]->sta->seen_5ghz);
	band_steering_rounds(-80, 6);
	CHECK(!calls[CONTROL] && !infos[CONTROL]->kick_time);

	/* ... until a 5 GHz node hears it */
	hear_on_5ghz(CONTROL);
	band_steering_rounds(-80, 2);
	CHECK(tm[CONTROL] == 1 && calls[CONTROL] == 1);

	puts("ok - band steering signal threshold, stations not heard on 5 GHz");
}

/*
 * Load comparison between two nodes. A node counts as loaded when it has at
 * least load_kick_min_clients stations and its channel load is above
 * load_kick_threshold.
 */
static void test_load(void)
{
	struct sta_info *other;

	fixture();
	cfg_begin();
	cfg_apply("update_config");
	CHECK(!exempt(EXEMPT));
	config.assoc_steering = true;
	config.load_kick_threshold = 50;
	config.load_kick_min_clients = 1;
	other = hear_on_5ghz(CONTROL);

	/* Neither node is loaded, or both are: no reason to prefer one */
	ap.node.load = ap5.node.load = 10;
	CHECK(usteer_check_request(infos[CONTROL], EVENT_TYPE_ASSOC));
	CHECK(usteer_check_request(other, EVENT_TYPE_ASSOC));
	ap.node.load = ap5.node.load = 90;
	CHECK(usteer_check_request(infos[CONTROL], EVENT_TYPE_ASSOC));
	CHECK(usteer_check_request(other, EVENT_TYPE_ASSOC));

	/*
	 * Only the 5 GHz node is loaded: the association to it is refused,
	 * the one to the idle node is accepted.
	 */
	ap.node.load = 10;
	CHECK(usteer_check_request(infos[CONTROL], EVENT_TYPE_ASSOC));
	CHECK(!usteer_check_request(other, EVENT_TYPE_ASSOC));

	/* A node with fewer stations than the minimum is not loaded */
	config.load_kick_min_clients = NSTA + 1;
	CHECK(usteer_check_request(other, EVENT_TYPE_ASSOC));
	config.load_kick_min_clients = 1;

	/*
	 * Load kick on the loaded 2.4 GHz node. The first station has the
	 * weaker signal, but only the second one is heard by the idle 5 GHz
	 * node: it has somewhere to go and is the one to kick.
	 */
	ap.node.load = 90;
	ap5.node.load = 10;
	infos[EXEMPT]->signal = -90;
	infos[CONTROL]->signal = -60;
	config.load_kick_enabled = true;
	config.load_kick_delay = 1;
	reset_counters();
	run_kick_passes(2);
	CHECK(kicks[CONTROL] == 1 && !calls[EXEMPT]);

	puts("ok - load comparison for association requests and load kicks");
}

static struct blob_attr *bss_tm_query(const char *addr)
{
	blob_buf_init(&in, 0);
	blobmsg_add_string(&in, "address", addr);
	blobmsg_add_u8(&in, "dialog-token", 3);

	return in.head;
}

static void test_requests_to_hostapd(void)
{
	struct blob_attr *node, *client, *flag;
	int i;

	/* The request helpers refuse an exempt station on their own */
	fixture();
	for (i = 0; i < NSTA; i++) {
		CHECK(!usteer_ubus_bss_transition_request(infos[i], 1, false, 0, true, 100, NULL));
		CHECK(!usteer_ubus_band_steering_request(infos[i], 0, false, 0, true, 100));
		CHECK(!usteer_ubus_trigger_link_measurement(infos[i]));
		CHECK(!usteer_ubus_trigger_client_scan(infos[i]));
		usteer_ubus_kick_client(infos[i], KICK_REASON_UNSPECIFIED);
	}
	CHECK(!calls[EXEMPT] && calls[CONTROL] == 5);

	fixture();
	ap.link_measurement_tries = 1;
	usteer_local_node_request_link_measurement(&ap);
	CHECK(!lm[EXEMPT] && lm[CONTROL] == 1);

	/*
	 * BSS transition queries: 0 leaves the answer to hostapd, 1 means
	 * usteer answers with its own candidate list.
	 */
	CHECK(usteer_handle_bss_tm_query(&ap, bss_tm_query(addresses[EXEMPT])) == 0);
	CHECK(list_empty(&ap.bss_tm_queries));
	CHECK(usteer_handle_bss_tm_query(&ap, bss_tm_query("02:00:00:00:00:0A")) == 0);
	CHECK(usteer_handle_bss_tm_query(&ap, bss_tm_query(addresses[CONTROL])) == 1);
	usteer_local_node_process_bss_tm_queries(&ap.bss_tm_queries_timeout);
	CHECK(!tm[EXEMPT] && tm[CONTROL] == 1);
	CHECK(list_empty(&ap.bss_tm_queries));

	/* Status output marks the exempt station */
	CHECK(!usteer_ubus_get_connected_clients(NULL, NULL, NULL, "connected_clients", NULL));
	node = field(reply, "hostapd.test24");
	CHECK(node);
	for (i = 0; i < NSTA; i++) {
		client = field(node, addresses[i]);
		CHECK(client);
		flag = field(client, "policy_exempt");
		CHECK(flag && blobmsg_get_bool(flag) == (i == EXEMPT));
	}

	CHECK(!usteer_ubus_get_clients(NULL, NULL, NULL, "get_clients", NULL));
	for (i = 0; i < NSTA; i++) {
		client = field(reply, addresses[i]);
		CHECK(client);
		node = field(client, "hostapd.test24");
		CHECK(node);
		flag = field(node, "policy_exempt");
		CHECK(flag && blobmsg_get_bool(flag) == (i == EXEMPT));
	}

	puts("ok - requests to hostapd, BSS transition queries, status output");
}

int main(void)
{
	avl_init(&stations, avl_macaddr_cmp, false, NULL);

	test_exempt_list();
	test_aggressiveness_list();
	test_reload();
	test_requests();
	test_kicks();
	test_roaming();
	test_band_steering();
	test_load();
	test_requests_to_hostapd();

	teardown();
	config_set_policy_exempt_sta_list(NULL);
	config_set_aggressiveness_mac_list(NULL);
	config_set_ssid_list(NULL);
	free(reply);
	blob_buf_free(&in);
	blob_buf_free(&b);
	blob_buf_free(&local_node_blob);

	return 0;
}
