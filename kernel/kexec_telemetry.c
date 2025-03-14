#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/sched/clock.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/kexec.h>
#include <linux/sort.h>

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

int kexec_timestamp(char *event_name);

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

	ts_array[index] = ts_info;
	return 0;
}

/* For kernel use */
int kexec_timestamp(char *event_name)
{
	struct timestamp_info ts_info;

	ts_info.ts = sched_clock();
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

	ts_info.ts = sched_clock();
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

static int __init kexec_telemetry_init(void)
{
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

	return 0;
}
subsys_initcall(kexec_telemetry_init);
