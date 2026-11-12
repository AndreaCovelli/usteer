/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 *
 *   Copyright (C) 2020 embedd.ch 
 *   Copyright (C) 2020 Felix Fietkau <nbd@nbd.name> 
 *   Copyright (C) 2020 John Crispin <john@phrozen.org> 
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <net/ethernet.h>
#ifdef linux
#include <netinet/ether.h>
#endif

#include "usteer.h"

static int
avl_macaddr_cmp(const void *k1, const void *k2, void *ptr)
{
	return memcmp(k1, k2, 6);
}

AVL_TREE(stations, avl_macaddr_cmp, false, NULL);
static struct usteer_timeout_queue tq;

/*
 * Kept outside struct usteer_config: set_config clears that struct with
 * usteer_init_defaults() before calling the setters, which would drop the
 * only reference to the previous lists.
 */
static struct blob_attr *aggressiveness_mac_list;
static struct blob_attr *policy_exempt_sta_list;
static struct ether_addr *policy_exempt_addrs;
static unsigned int n_policy_exempt_addrs;

void config_set_aggressiveness_mac_list(struct blob_attr *data)
{
	free(aggressiveness_mac_list);

	if (data && blobmsg_len(data))
		aggressiveness_mac_list = blob_memdup(data);
	else
		aggressiveness_mac_list = NULL;
}

void config_get_aggressiveness_mac_list(struct blob_buf *buf)
{
	if (aggressiveness_mac_list)
		blobmsg_add_blob(buf, aggressiveness_mac_list);
}

void config_set_policy_exempt_sta_list(struct blob_attr *data)
{
	struct blob_attr *cur;
	unsigned int n = 0;
	int rem;

	free(policy_exempt_sta_list);
	policy_exempt_sta_list = NULL;
	free(policy_exempt_addrs);
	policy_exempt_addrs = NULL;
	n_policy_exempt_addrs = 0;

	if (!data || !blobmsg_len(data))
		return;

	policy_exempt_sta_list = blob_memdup(data);
	if (!policy_exempt_sta_list)
		return;

	blobmsg_for_each_attr(cur, policy_exempt_sta_list, rem)
		n++;

	policy_exempt_addrs = calloc(n, sizeof(*policy_exempt_addrs));
	if (!policy_exempt_addrs)
		return;

	blobmsg_for_each_attr(cur, policy_exempt_sta_list, rem) {
		struct ether_addr *addr = &policy_exempt_addrs[n_policy_exempt_addrs];

		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING ||
		    !blobmsg_check_attr(cur, false)) {
			MSG(INFO, "Ignoring malformed policy_exempt_sta_list entry\n");
			continue;
		}

		if (!ether_aton_r(blobmsg_get_string(cur), addr)) {
			MSG(INFO, "Ignoring invalid policy_exempt_sta_list entry '%s'\n",
			    blobmsg_get_string(cur));
			continue;
		}

		n_policy_exempt_addrs++;
	}
}

void config_get_policy_exempt_sta_list(struct blob_buf *buf)
{
	if (policy_exempt_sta_list)
		blobmsg_add_blob(buf, policy_exempt_sta_list);
}

bool usteer_sta_is_policy_exempt(const uint8_t *addr)
{
	unsigned int i;

	if (!addr)
		return false;

	for (i = 0; i < n_policy_exempt_addrs; i++) {
		if (!memcmp(&policy_exempt_addrs[i], addr, 6))
			return true;
	}

	return false;
}

static void
usteer_sta_del(struct sta *sta)
{
	MSG(DEBUG, "Delete station " MAC_ADDR_FMT "\n",
	    MAC_ADDR_DATA(sta->addr));

	avl_delete(&stations, &sta->avl);
	usteer_measurement_report_sta_cleanup(sta);
	free(sta);
}

static void
usteer_sta_info_del(struct sta_info *si)
{
	struct sta *sta = si->sta;

	MSG(DEBUG, "Delete station " MAC_ADDR_FMT " entry for node %s\n",
	    MAC_ADDR_DATA(sta->addr), usteer_node_name(si->node));

	usteer_timeout_cancel(&tq, &si->timeout);
	list_del(&si->list);
	list_del(&si->node_list);
	free(si);

	if (list_empty(&sta->nodes))
		usteer_sta_del(sta);
}

void
usteer_sta_node_cleanup(struct usteer_node *node)
{
	struct sta_info *si, *tmp;

	free(node->rrm_nr);
	node->rrm_nr = NULL;

	list_for_each_entry_safe(si, tmp, &node->sta_info, node_list)
		usteer_sta_info_del(si);
}

static void
usteer_sta_info_timeout(struct usteer_timeout_queue *q, struct usteer_timeout *t)
{
	struct sta_info *si = container_of(t, struct sta_info, timeout);

	usteer_sta_info_del(si);
}

static void
usteer_sta_update_aggressiveness(struct sta *sta)
{
	struct blob_attr *cur;
	const char *entry;
	char sta_mac[18];
	int rem;

	snprintf(sta_mac, sizeof(sta_mac), MAC_ADDR_FMT, MAC_ADDR_DATA(sta->addr));
	sta->aggressiveness = config.aggressiveness;
	blobmsg_for_each_attr(cur, aggressiveness_mac_list, rem) {
		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING ||
		    !blobmsg_check_attr(cur, false))
			continue;

		/* "xx:xx:xx:xx:xx:xx,N" */
		entry = blobmsg_get_string(cur);
		if (strlen(entry) != 19)
			continue;
		if (strncmp(entry, sta_mac, 17) != 0)
			continue;
		sta->aggressiveness = entry[18] - '0';
		break;
	}
}

struct sta_info *
usteer_sta_info_get(struct sta *sta, struct usteer_node *node, bool *create)
{
	struct sta_info *si;

	list_for_each_entry(si, &sta->nodes, list) {
		if (si->node != node)
			continue;

		if (create)
			*create = false;

		return si;
	}

	if (!create)
		return NULL;

	MSG(DEBUG, "Create station " MAC_ADDR_FMT " entry for node %s\n",
	    MAC_ADDR_DATA(sta->addr), usteer_node_name(node));

	si = calloc(1, sizeof(*si));
	si->node = node;
	si->sta = sta;
	list_add(&si->list, &sta->nodes);
	list_add(&si->node_list, &node->sta_info);
	si->created = current_time;
	*create = true;

	usteer_sta_update_aggressiveness(sta);

	/* Node is by default not connected. */
	usteer_sta_disconnected(si);

	return si;
}


void
usteer_sta_info_update_timeout(struct sta_info *si, int timeout)
{
	if (si->connected == STA_CONNECTED)
		usteer_timeout_cancel(&tq, &si->timeout);
	else if (timeout > 0)
		usteer_timeout_set(&tq, &si->timeout, timeout);
	else
		usteer_sta_info_del(si);
}

struct sta *
usteer_sta_get(const uint8_t *addr, bool create)
{
	struct sta *sta;

	sta = avl_find_element(&stations, addr, sta, avl);
	if (sta)
		return sta;

	if (!create)
		return NULL;

	MSG(DEBUG, "Create station entry " MAC_ADDR_FMT "\n", MAC_ADDR_DATA(addr));
	sta = calloc(1, sizeof(*sta));
	memcpy(sta->addr, addr, sizeof(sta->addr));
	sta->avl.key = sta->addr;
	avl_insert(&stations, &sta->avl);
	INIT_LIST_HEAD(&sta->nodes);
	INIT_LIST_HEAD(&sta->measurements);

	return sta;
}

void usteer_sta_disconnected(struct sta_info *si)
{
	si->connected = STA_NOT_CONNECTED;
	si->kick_time = 0;
	si->connected_since = 0;
	usteer_sta_info_update_timeout(si, config.local_sta_timeout);
}

void
usteer_sta_info_update(struct sta_info *si, int signal, bool avg)
{
	/* ignore probe request signal when connected */
	if (si->connected == STA_CONNECTED && si->signal != NO_SIGNAL && !avg)
		signal = NO_SIGNAL;

	if (signal != NO_SIGNAL) {
		si->signal = signal;
		usteer_band_steering_sta_update(si);
	}

	si->seen = current_time;

	if (si->node->freq < 4000)
		si->sta->seen_2ghz = 1;
	else
		si->sta->seen_5ghz = 1;

	usteer_sta_info_update_timeout(si, config.local_sta_timeout);
}

bool
usteer_handle_sta_event(struct usteer_node *node, const uint8_t *addr,
		       enum usteer_event_type type, int freq, int signal)
{
	struct sta *sta;
	struct sta_info *si;
	uint32_t diff;
	bool ret;
	bool create;

	sta = usteer_sta_get(addr, true);
	if (!sta)
		return -1;

	si = usteer_sta_info_get(sta, node, &create);
	usteer_sta_info_update(si, signal, false);
	si->stats[type].requests++;

	diff = si->stats[type].blocked_last_time - current_time;
	if (diff > config.sta_block_timeout)
		si->stats[type].blocked_cur = 0;

	ret = usteer_check_request(si, type);
	if (!ret) {
		si->stats[type].blocked_cur++;
		si->stats[type].blocked_total++;
		si->stats[type].blocked_last_time = current_time;
	} else {
		si->stats[type].blocked_cur = 0;
	}

	if (create)
		usteer_send_sta_update(si);

	return ret;
}

bool
usteer_sta_supports_beacon_measurement_mode(struct sta_info *si, enum usteer_beacon_measurement_mode mode)
{
	switch (mode) {
		case BEACON_MEASUREMENT_PASSIVE:
			return si->rrm & (1 << 4);
		case BEACON_MEASUREMENT_ACTIVE:
			return si->rrm & (1 << 5);
		case BEACON_MEASUREMENT_TABLE:
			return si->rrm & (1 << 6);
	}

	return false;
}

bool
usteer_sta_supports_link_measurement(struct sta_info *si)
{
	return si->rrm & (1 << 0);
}

static void __usteer_init usteer_sta_init(void)
{
	usteer_timeout_init(&tq);
	tq.cb = usteer_sta_info_timeout;
}
