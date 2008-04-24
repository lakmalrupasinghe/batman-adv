/*
 * Copyright (C) 2007-2008 B.A.T.M.A.N. contributors:
 * Marek Lindner
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 *
 */


#include "main.h"
#include "proc.h"
#include "log.h"
#include "routing.h"
#include "send.h"
#include "soft-interface.h"
#include "device.h"
#include "translation-table.h"
#include "hard-interface.h"
#include "vis.h"
#include "types.h"
#include "hash.h"



struct list_head if_list;
struct hashtable_t *orig_hash;

DEFINE_MUTEX(if_list_lock);
DEFINE_SPINLOCK(orig_hash_lock);

atomic_t originator_interval;
atomic_t vis_interval;
int16_t num_hna = 0;
int16_t num_ifs = 0;

struct net_device *bat_device = NULL;

static struct task_struct *kthread_task = NULL;
static struct timer_list purge_timer;

unsigned char broadcastAddr[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
char hna_local_changed = 0;
char module_state = MODULE_INACTIVE;

struct workqueue_struct *bat_event_workqueue;



int init_module(void)
{
	int retval;

	INIT_LIST_HEAD(&if_list);
	atomic_set(&originator_interval, 1000);
	atomic_set(&vis_interval, 1000);	/* TODO: raise this later, this is only for debugging now. */

	bat_event_workqueue = create_singlethread_workqueue("bat_event_workqueue");

	if (!bat_event_workqueue)
		return -ENOMEM;

	if ((retval = setup_procfs()) < 0)
		return retval;

	orig_hash = hash_new(128, compare_orig, choose_orig);

	if (orig_hash == NULL)
		goto clean_proc;

	if (hna_local_init() < 0)
		goto free_orig_hash;

	if (hna_global_init() < 0)
		goto free_lhna_hash;

	start_hardif_check_timer();
	bat_device_init();

	debug_log(LOG_TYPE_CRIT, "B.A.T.M.A.N. Advanced %s%s (compability version %i) loaded \n", SOURCE_VERSION, (strlen(REVISION_VERSION) > 3 ? REVISION_VERSION : ""), COMPAT_VERSION);

	return 0;

free_lhna_hash:
	hna_local_free();

free_orig_hash:
	hash_delete(orig_hash, free_orig_node);

clean_proc:
	cleanup_procfs();
	return -ENOMEM;
}

void cleanup_module(void)
{
	module_state = MODULE_UNLOADING;

	shutdown_module();
	destroy_hardif_check_timer();

	spin_lock(&orig_hash_lock);
	hash_delete(orig_hash, free_orig_node);
	spin_unlock(&orig_hash_lock);

	hna_local_free();
	hna_global_free();
	cleanup_procfs();
	destroy_workqueue(bat_event_workqueue);
}

void start_purge_timer(void)
{
	init_timer(&purge_timer);

	purge_timer.expires = jiffies + (1 * HZ); /* one second */
	purge_timer.data = 0;
	purge_timer.function = purge_orig;

	add_timer(&purge_timer);
}

/* activates the module, creates bat device, starts timer ... */
void activate_module(void)
{
	int result;

	module_state = MODULE_ACTIVE;

	/* initialize layer 2 interface */
	if (bat_device == NULL) {

		bat_device = alloc_netdev(sizeof(struct bat_priv) , "bat%d", interface_setup);

		if (bat_device == NULL) {
			debug_log(LOG_TYPE_CRIT, "Unable to allocate the batman interface\n");
			return;
		}

		result = register_netdev(bat_device);

		if (result < 0) {
			debug_log(LOG_TYPE_CRIT, "Unable to register the batman interface: %i\n", result);
			free_netdev(bat_device);
			bat_device = NULL;
			return;
		}

		hna_local_add(bat_device->dev_addr);

	}

	/* (re)start kernel thread for packet processing */
	kthread_task = kthread_run(packet_recv_thread, NULL, "batman-adv");

	if (IS_ERR(kthread_task)) {
		debug_log(LOG_TYPE_CRIT, "Unable to start packet receive thread\n");
		kthread_task = NULL;
	}

	start_purge_timer();

	bat_device_setup();

	vis_init();
}
/* shuts down the whole module.*/
void shutdown_module(void)
{
	if (module_state == MODULE_ACTIVE)
		module_state = MODULE_INACTIVE;

	flush_workqueue(bat_event_workqueue);

	vis_quit();

	if (bat_device != NULL) {
		unregister_netdev(bat_device);
		bat_device = NULL;
	}

	/* deactivate kernel thread for packet processing (if running) */
	if (kthread_task) {
		atomic_set(&exit_cond, 1);
		wake_up_interruptible(&thread_wait);
		kthread_stop(kthread_task);

		kthread_task = NULL;
	}

	rcu_read_lock();
	if (!(list_empty(&if_list))) {
		rcu_read_unlock();
		del_timer_sync(&purge_timer);
	} else {
		rcu_read_unlock();
	}

	synchronize_net();
	bat_device_destroy();

	hardif_remove_interfaces();
	synchronize_rcu();


}

void inc_module_count(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	MOD_INC_USE_COUNT;
#else
	try_module_get(THIS_MODULE);
#endif
}

void dec_module_count(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	MOD_DEC_USE_COUNT;
#else
	module_put(THIS_MODULE);
#endif
}

int addr_to_string(char *buff, uint8_t *addr)
{
	return sprintf(buff, "%02x:%02x:%02x:%02x:%02x:%02x", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

int compare_orig(void *data1, void *data2)
{
	return (memcmp(data1, data2, ETH_ALEN) == 0 ? 1 : 0);
}

/* hashfunction to choose an entry in a hash table of given size */
/* hash algorithm from http://en.wikipedia.org/wiki/Hash_table */
int choose_orig(void *data, int32_t size)
{
	unsigned char *key= data;
	uint32_t hash = 0;
	size_t i;

	for (i = 0; i < 6; i++) {
		hash += key[i];
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return (hash%size);
}



int is_my_mac(uint8_t *addr)
{
	struct batman_if *batman_if;
	rcu_read_lock();
	list_for_each_entry_rcu(batman_if, &if_list, list) {
		if ((batman_if->net_dev != NULL) && (compare_orig(batman_if->net_dev->dev_addr, addr))) {
			rcu_read_unlock();
			return 1;
		}
	}
	rcu_read_unlock();
	return 0;

}

int is_bcast(uint8_t *addr)
{
	return ((addr[0] == (uint8_t)0xff) && (addr[1] == (uint8_t)0xff));
}

int is_mcast(uint8_t *addr)
{
	return (*addr & 0x01);
}



MODULE_LICENSE("GPL");

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_SUPPORTED_DEVICE(DRIVER_DEVICE);
