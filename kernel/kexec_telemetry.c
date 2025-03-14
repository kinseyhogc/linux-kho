#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/sched/clock.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/libfdt.h>
#include <linux/kexec.h>
#include <linux/kexec_handover.h>
#include <linux/sort.h>
#include <linux/timer.h>
#include <asm/timer.h>

#include "kexec_internal.h"

struct timestamp_info {
	char *name;
	unsigned long ts;
	bool src_user : 1;
};

static struct dentry *kexec_telemetry_debugfs_dir;

static struct timestamp_info *ts_array;
static char *ts_events;
static atomic_t ts_index;
static atomic_t event_index;
/* offset variables */
unsigned long long tsc_new;
unsigned long long tsc_old;
unsigned long sclock_new;
unsigned long sclock_old;
unsigned long kexec_sched_clock_offset = 0;
bool reboot = false;

int kexec_timestamp(char *event_name);

struct kho_node telemetry_node = KHO_NODE_INIT;

static int add_timestamp_to_telemetry_array(struct timestamp_info ts_info, char *event_name)
{
	int index;
	int name_index;
	char *event_pos = ts_events;

	index = atomic_fetch_add(1, &ts_index);
	name_index = atomic_fetch_add(strlen(event_name) + 1, &event_index);
	event_pos += name_index;

	strcpy(event_pos, event_name);
	ts_info.name = event_pos;

	ts_info.ts += kexec_sched_clock_offset;
	ts_array[index] = ts_info;
	return 0;
}

static void set_sched_clock_offset(void)
{
	unsigned long long tsc_new = rdtsc_ordered();
	unsigned long sclock_new = native_sched_clock();
	if (tsc_old) {
		kexec_sched_clock_offset = native_sched_clock_from_tsc(tsc_new) -
	native_sched_clock_from_tsc(tsc_old) + (sclock_old - sclock_new);
	}
}

void kexec_pre_reboot_record(void)
{
	tsc_old = rdtsc_ordered();
	sclock_old = native_sched_clock() + kexec_sched_clock_offset;
}

/* For kernel use */
int kexec_timestamp(char *event_name)
{
	struct timestamp_info ts_info;

	ts_info.ts = native_sched_clock();
	ts_info.src_user = false;

	add_timestamp_to_telemetry_array(ts_info, event_name);

	return 0;
}

static int telemetry_cmp(const void *_a, const void *_b)
{
	struct timestamp_info *a = (struct timestamp_info *)_a;
	struct timestamp_info *b = (struct timestamp_info *)_b;
	unsigned long ts_a = a->ts;
	unsigned long ts_b = b->ts;

	if (ts_a < ts_b)
		return -1;
	if (ts_a > ts_b)
		return 1;
	return 0;
}

static void kexec_telemetry_sort(void)
{
	int index = atomic_read(&ts_index);

	sort(ts_array, index, sizeof(struct timestamp_info), telemetry_cmp, NULL);
}

static ssize_t kexec_telemetry_user_write(struct file *file,
				     const char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct timestamp_info ts_info;
	char buffer[NAME_MAX];
	int read_len = count < (sizeof(buffer) - 1) ? count-1 : (sizeof(buffer) - 1);

	ts_info.ts = native_sched_clock();
	ts_info.src_user = true;

	if (copy_from_user(buffer, buf, read_len)) {
		return -EFAULT;
	}

	buffer[read_len] = '\0';

	add_timestamp_to_telemetry_array(ts_info, buffer);

	return count;
}

static int kexec_telemetry_show(struct seq_file *m, void *v)
{
	int i = 0;
	int index = atomic_read(&ts_index);

	seq_printf(m, "%-25s%s\n", "Event", "Timestamp");
	seq_printf(m, "-------------------------------------\n");

	// RCU read lock
	kexec_telemetry_sort();

	for (; i < index ; i++) {
		struct timestamp_info t = ts_array[i];
		char src = 'K';
		if (t.src_user)
			src = 'U';
		seq_printf(m, "%c: %-22s%ld\n", src, t.name, t.ts);
	}
	return 0;
}

static int kexec_telemetry_open(struct inode *inode, struct file *file)
{
	return single_open(file, kexec_telemetry_show, NULL);
}

static ssize_t kexec_telemetry_reset(struct file *file, const char __user *data,
				 size_t count, loff_t *ppos)
{
	u64 val;
	int err;

	err = kstrtou64_from_user(data, count, 0, &val);
	if (err) {
		pr_err("%s, failed to parse string\n", __func__);
		return -EFAULT;
	}

	if (val != 1)
		return -EFAULT;

	atomic_set(&ts_index, 0);
	atomic_set(&event_index, 0);
	memset(ts_array, 0, PAGE_SIZE);
	memset(ts_events, 0, PAGE_SIZE);

	return count;
}

static const struct file_operations kexec_telemetry_fops = {
	.owner = THIS_MODULE,
	.open = kexec_telemetry_open,
	.write = kexec_telemetry_user_write,
	.read = seq_read,
};

static const struct file_operations kexec_telemetry_reset_fops = {
	.owner = THIS_MODULE,
	.write = kexec_telemetry_reset,
};

static int kho_telemetry_notifier(struct notifier_block *self, unsigned long cmd,
				  void *private)
{
	char *event_name_buf;
	unsigned long *ts_buf;
	bool *src_buf; //K:0, U:1, also TODO: use bitmap to save space
	unsigned long *sclock_old_buf;
	unsigned long long *tsc_old_buf;
	size_t names_total_size = 0;
	size_t event_count;
	int ts_iter = 0;
	int name_offset = 0;

	kexec_telemetry_sort();
	event_count = atomic_read(&ts_index);

	/* Calculate size of temp buffer for event names and timestamp */
	for (; ts_iter < event_count; ts_iter++) {
		names_total_size += strlen(ts_array[ts_iter].name) + 1;
	}

	event_name_buf = kzalloc(names_total_size, GFP_KERNEL);
	ts_buf = kzalloc((sizeof(unsigned long)*event_count), GFP_KERNEL);
	src_buf = kzalloc((sizeof(bool)*event_count), GFP_KERNEL);
	sclock_old_buf = kzalloc(sizeof(unsigned long), GFP_KERNEL);
	tsc_old_buf = kzalloc(sizeof(unsigned long long), GFP_KERNEL);

	if (!event_name_buf || !ts_buf || !src_buf || !sclock_old_buf || !tsc_old_buf)
		return NOTIFY_BAD;

	/* Serialize names and timestamp */
	for (ts_iter = 0; ts_iter < event_count; ts_iter++) {
		struct timestamp_info t = ts_array[ts_iter];
		size_t name_len = strlen(t.name) + 1;
		memcpy(event_name_buf + name_offset, t.name, name_len);
		ts_buf[ts_iter] = t.ts;
		src_buf[ts_iter] = t.src_user;

		name_offset += name_len;
	}

	/* Serialize preboot values */
	sclock_old_buf[0] = sclock_old;
	tsc_old_buf[0] = tsc_old;

	kho_add_node(NULL, "kexec_telemetry", &telemetry_node);
	kho_add_prop(&telemetry_node, "event_name", event_name_buf, names_total_size);
	kho_add_prop(&telemetry_node, "timestamp", ts_buf, sizeof(unsigned long)*event_count);
	kho_add_prop(&telemetry_node, "src", src_buf, event_count);
	kho_add_prop(&telemetry_node, "sclock_old", sclock_old_buf, sizeof(unsigned long));
	kho_add_prop(&telemetry_node, "tsc_old", tsc_old_buf, sizeof(unsigned long long));

	return NOTIFY_DONE;
}

/* Re-build telemetry file */
static int __restore_telemetry_from_kho(const char *name, size_t name_len, unsigned long ts, int src_user)
{
	struct timestamp_info ts_info;
	char name_buf[NAME_MAX];

	memcpy(name_buf, name, name_len);

	ts_info.ts = ts;
	ts_info.src_user = src_user;

	add_timestamp_to_telemetry_array(ts_info, name_buf);

	return 0;
}

static int restore_telemetry_from_kho(void)
{
	const void *fdt = kho_get_fdt();
	int offset, err, i = 0;
	int names_size = 0;
	int ts_buf_size = 0;
	int src_buf_size = 0;
	int sclock_old_buf_size = 0;
	int tsc_old_buf_size = 0;

	const char *names;
	const unsigned long *ts_buf;
	const bool *src_buf;
	const unsigned long *sclock_old_buf;
	const unsigned long long *tsc_old_buf;

	const char *event_name;

	if (!fdt)
		return 0;

	offset = fdt_path_offset(fdt, "/kexec_telemetry");
	if (offset < 0) {
		pr_err("cannot find /timestamps in KHO dt\n");
		return -ENOENT;
	}

	names = fdt_getprop(fdt, offset, "event_name", &names_size);
	if (!names) {
		pr_err("Looking for /kexec_telemetry.event_name, found p=%p\n", names);
		return -ENOENT;
	}

	ts_buf = fdt_getprop(fdt, offset, "timestamp", &ts_buf_size);
	if (!ts_buf) {
		pr_err("Looking for /kexec_telemetry.timestamp, found p=%p\n", ts_buf);
		return -ENOENT;
	}

	src_buf = fdt_getprop(fdt, offset, "src", &src_buf_size);
	if (!ts_buf) {
		pr_err("Looking for /kexec_telemetry.src, found p=%p\n", src_buf);
		return -ENOENT;
	}
	event_name = names;

	sclock_old_buf = fdt_getprop(fdt, offset, "sclock_old", &sclock_old_buf_size);
	if (!sclock_old_buf) {
		pr_err("Looking for /khodemo.sclock_old, found p=%p\n", sclock_old_buf);
		return -ENOENT;
	}

	tsc_old_buf = fdt_getprop(fdt, offset, "tsc_old", &tsc_old_buf_size);
	if (!tsc_old_buf) {
		pr_err("Looking for /khodemo.tsc_old, found p=%p\n", tsc_old_buf);
		return -ENOENT;
	}

	tsc_old = tsc_old_buf[0];
	sclock_old = sclock_old_buf[0];

	printk("%s, restoring\n", __func__);
	for (; i < src_buf_size; i += 1) {
		size_t name_len = strlen(event_name) + 1;

		err = __restore_telemetry_from_kho(event_name, name_len, ts_buf[i], src_buf[i]);
		if (err)
			return err;
		event_name += name_len;
	}

	return err;

}

static struct notifier_block kho_telemetry_nb = {
	.notifier_call = kho_telemetry_notifier,
};

static int __init kexec_telemetry_init(void)
{
	int err;

	kexec_telemetry_debugfs_dir = debugfs_create_dir("kexec", NULL);
	debugfs_create_file("timestamps", 0644, kexec_telemetry_debugfs_dir, NULL, &kexec_telemetry_fops);
	debugfs_create_file("timestamps_reset", 0200, kexec_telemetry_debugfs_dir, NULL, &kexec_telemetry_reset_fops);

	ts_array = (struct timestamp_info *)__get_free_page(GFP_KERNEL);
	if (!ts_array)
		return -ENOMEM;

	ts_events = (char *)__get_free_page(GFP_KERNEL);
	if (!ts_events)
		return -ENOMEM;

	atomic_set(&ts_index, 0);
	atomic_set(&event_index, 0);

	err = restore_telemetry_from_kho();
	set_sched_clock_offset();

	register_kho_notifier(&kho_telemetry_nb);

	return err;
}
subsys_initcall(kexec_telemetry_init);
