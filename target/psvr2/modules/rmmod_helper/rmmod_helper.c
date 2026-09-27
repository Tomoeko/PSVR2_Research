/*
 * rmmod_helper.c - Module unloading for supported PSVR2 kernels.
 *
 * The target kernel has CONFIG_MODULE_UNLOAD disabled, so delete_module(2)
 * and the normal module reference graph are absent. This helper reproduces
 * the 4.4.139 teardown sequence with functions resolved from the exact kernel
 * image and exposes a root-only /proc/rmmod_helper control file.
 *
 * Dependency information is captured from SHN_UNDEF symbols while modules
 * are in MODULE_STATE_COMING. The normal mode fails closed if the target was
 * already present when this helper loaded or if tracking ever overflowed.
 *
 *   echo name   > /proc/rmmod_helper  verified dependency-aware teardown
 *   echo !name  > /proc/rmmod_helper  allow incomplete dependency history
 *   echo !!name > /proc/rmmod_helper  raw teardown without module_exit()
 */

#include <linux/async.h>
#include <linux/capability.h>
#include <linux/completion.h>
#include <linux/elf.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#if defined(PSVR2_SOURCE_FAMILY_0110)
#define RMMOD_FIRMWARE_LABEL "01.10"
#elif defined(PSVR2_SOURCE_FAMILY_0600)
#define RMMOD_FIRMWARE_LABEL "06.00"
#else
#error "rmmod_helper requires an exact supported PSVR2 source family"
#endif

#define RMMOD_STATUS_SIZE 512
#define RMMOD_READ_SIZE 2048
#define RMMOD_MAX_TRACKED 64

extern struct mutex module_mutex;

struct module_sect_attr {
	struct module_attribute mattr;
	char *name;
	unsigned long address;
};

struct module_sect_attrs {
	struct attribute_group grp;
	unsigned int nsections;
	struct module_sect_attr attrs[0];
};

struct module_notes_attrs {
	struct kobject *dir;
	unsigned int notes;
	struct bin_attribute attrs[0];
};

typedef unsigned long (*mod_kallsyms_fn_t)(const char *name);
typedef int (*blocking_notifier_fn_t)(struct blocking_notifier_head *,
				      unsigned long, void *);
typedef void (*void_mod_fn_t)(struct module *);
typedef void (*void_ptr_fn_t)(void *);
typedef void (*void_kvp_fn_t)(struct kernel_param *, unsigned int);
typedef void (*sync_fn_t)(void);
typedef void (*lockdep_fn_t)(void *, unsigned int);
typedef void (*ddebug_fn_t)(const char *);
typedef void (*mod_tree_fn_t)(struct module *);
typedef void (*free_notes_fn_t)(struct module_notes_attrs *, unsigned int);
typedef void (*sysfs_remove_group_fn_t)(struct kobject *,
					const struct attribute_group *);
typedef void (*sysfs_remove_file_ns_fn_t)(struct kobject *,
					  const struct attribute *,
					  const void *);

static struct list_head *kp_modules_list;
static struct blocking_notifier_head *kp_module_notify_list;
static mod_kallsyms_fn_t kfn_mod_kallsyms;
static blocking_notifier_fn_t kfn_blocking_notifier;
static void_mod_fn_t kfn_module_arch_cleanup;
static void_mod_fn_t kfn_module_arch_freeing_init;
static void_mod_fn_t kfn_module_bug_cleanup;
static void_mod_fn_t kfn_unset_module_init_ro_nx;
static void_mod_fn_t kfn_unset_module_core_ro_nx;
static void_mod_fn_t kfn_module_param_sysfs_remove;
static void_mod_fn_t kfn_mod_kobject_put;
static void_ptr_fn_t kfn_module_memfree;
static void_ptr_fn_t kfn_free_percpu;
static void_kvp_fn_t kfn_destroy_params;
static sync_fn_t kfn_synchronize_sched;
static sync_fn_t kfn_async_synchronize_full;
static lockdep_fn_t kfn_lockdep_free_key_range;
static ddebug_fn_t kfn_ddebug_remove_module;
static mod_tree_fn_t kfn_mod_tree_remove;
static free_notes_fn_t kfn_free_notes_attrs;
static sysfs_remove_group_fn_t kfn_sysfs_remove_group;
static sysfs_remove_file_ns_fn_t kfn_sysfs_remove_file_ns;

static char status_buf[RMMOD_STATUS_SIZE];
static struct proc_dir_entry *proc_entry;
static DEFINE_MUTEX(status_lock);
static DEFINE_MUTEX(unload_lock);

struct tracked_module {
	struct module *mod;
	char name[MODULE_NAME_LEN];
	unsigned long core_start;
	unsigned long core_end;
	unsigned long init_start;
	unsigned long init_end;
	u64 depends_on;
	bool active;
	bool preexisting;
};

static struct tracked_module tracked[RMMOD_MAX_TRACKED];
static unsigned int tracked_count;
static bool tracking_complete = true;
static DEFINE_MUTEX(tracked_lock);

static void set_status(const char *format, ...)
{
	va_list args;

	mutex_lock(&status_lock);
	va_start(args, format);
	vsnprintf(status_buf, sizeof(status_buf), format, args);
	va_end(args);
	mutex_unlock(&status_lock);
}

static bool address_in_record(unsigned long address,
			      const struct tracked_module *record)
{
	return (address >= record->core_start && address < record->core_end) ||
	       (record->init_start &&
		address >= record->init_start && address < record->init_end);
}

static int find_tracked_locked(struct module *mod)
{
	unsigned int i;

	for (i = 0; i < tracked_count; ++i)
		if (tracked[i].mod == mod)
			return (int)i;
	return -1;
}

static int add_tracked_locked(struct module *mod, bool preexisting)
{
	struct tracked_module *record;
	int existing;

	if (!mod || mod == THIS_MODULE)
		return -1;
	existing = find_tracked_locked(mod);
	if (existing >= 0) {
		tracked[existing].active = true;
		return existing;
	}
	if (tracked_count >= RMMOD_MAX_TRACKED) {
		tracking_complete = false;
		pr_err("rmmod_helper: dependency tracker capacity exhausted\n");
		return -ENOSPC;
	}

	record = &tracked[tracked_count];
	memset(record, 0, sizeof(*record));
	record->mod = mod;
	strlcpy(record->name, mod->name, sizeof(record->name));
	record->core_start = (unsigned long)mod->module_core;
	record->core_end = record->core_start + mod->core_size;
	record->init_start = (unsigned long)mod->module_init;
	record->init_end = record->init_start + mod->init_size;
	record->active = true;
	record->preexisting = preexisting;
	return (int)tracked_count++;
}

static void capture_imports_locked(struct module *mod, int index)
{
	struct mod_kallsyms *kallsyms;
	unsigned int i;
	unsigned int target;

	if (index < 0 || index >= (int)RMMOD_MAX_TRACKED)
		return;
	kallsyms = rcu_dereference_sched(mod->kallsyms);
	if (!kallsyms || !kallsyms->symtab) {
		tracking_complete = false;
		pr_err("rmmod_helper: no loading kallsyms for '%s'\n", mod->name);
		return;
	}

	for (i = 0; i < kallsyms->num_symtab; ++i) {
		Elf_Sym *symbol = &kallsyms->symtab[i];
		unsigned long address;

		if (symbol->st_shndx != SHN_UNDEF || !symbol->st_value)
			continue;
		address = (unsigned long)symbol->st_value;
		for (target = 0; target < tracked_count; ++target) {
			if ((int)target == index || !tracked[target].active)
				continue;
			if (address_in_record(address, &tracked[target])) {
				tracked[index].depends_on |= 1ULL << target;
				break;
			}
		}
	}
}

static void mark_untracked(struct module *mod)
{
	int index;

	mutex_lock(&tracked_lock);
	index = find_tracked_locked(mod);
	if (index >= 0)
		tracked[index].active = false;
	mutex_unlock(&tracked_lock);
}

static int rmmod_module_notify(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	struct module *mod = data;
	int index;

	(void)nb;
	if (!mod || mod == THIS_MODULE)
		return NOTIFY_OK;

	mutex_lock(&tracked_lock);
	if (action == MODULE_STATE_COMING) {
		index = add_tracked_locked(mod, false);
		if (index >= 0)
			capture_imports_locked(mod, index);
	} else if (action == MODULE_STATE_LIVE) {
		index = find_tracked_locked(mod);
		if (index < 0) {
			/*
			 * Missing COMING means imported symbols are no longer
			 * available in core kallsyms. Preserve safety by making
			 * dependency-aware unload unavailable.
			 */
			(void)add_tracked_locked(mod, true);
			tracking_complete = false;
			pr_err("rmmod_helper: missed COMING event for '%s'\n",
			       mod->name);
		}
	} else if (action == MODULE_STATE_GOING) {
		index = find_tracked_locked(mod);
		if (index >= 0)
			tracked[index].active = false;
	}
	mutex_unlock(&tracked_lock);
	return NOTIFY_OK;
}

static struct notifier_block rmmod_nb = {
	.notifier_call = rmmod_module_notify,
};

enum dependency_result {
	DEPENDENCY_CLEAR,
	DEPENDENCY_FOUND,
	DEPENDENCY_UNKNOWN,
};

static enum dependency_result dependency_check(struct module *target,
						char dependent[MODULE_NAME_LEN])
{
	int target_index;
	unsigned int i;
	enum dependency_result result = DEPENDENCY_CLEAR;

	dependent[0] = '\0';
	mutex_lock(&tracked_lock);
	target_index = find_tracked_locked(target);
	if (target_index < 0) {
		result = DEPENDENCY_UNKNOWN;
		goto out;
	}
	for (i = 0; i < tracked_count; ++i) {
		if (!tracked[i].active || (int)i == target_index)
			continue;
		if (tracked[i].depends_on & (1ULL << target_index)) {
			strlcpy(dependent, tracked[i].name, MODULE_NAME_LEN);
			result = DEPENDENCY_FOUND;
			break;
		}
	}
	if (result == DEPENDENCY_CLEAR &&
	    (!tracking_complete || tracked[target_index].preexisting))
		result = DEPENDENCY_UNKNOWN;
out:
	mutex_unlock(&tracked_lock);
	return result;
}

static void remove_modinfo_attrs(struct module *mod)
{
	struct module_attribute *attr;
	unsigned int i;

	if (!mod->modinfo_attrs)
		return;
	for (i = 0; ; ++i) {
		attr = &mod->modinfo_attrs[i];
		if (!attr->attr.name)
			break;
		kfn_sysfs_remove_file_ns(
			&mod->mkobj.kobj, &attr->attr, NULL);
		if (attr->free)
			attr->free(mod);
	}
	kfree(mod->modinfo_attrs);
	mod->modinfo_attrs = NULL;
}

static void remove_notes_attrs(struct module *mod)
{
	if (!mod->notes_attrs)
		return;
	kfn_free_notes_attrs(mod->notes_attrs, mod->notes_attrs->notes);
	mod->notes_attrs = NULL;
}

static void remove_sect_attrs(struct module *mod)
{
	unsigned int i;

	if (!mod->sect_attrs)
		return;
	kfn_sysfs_remove_group(&mod->mkobj.kobj, &mod->sect_attrs->grp);
	for (i = 0; i < mod->sect_attrs->nsections; ++i)
		kfree(mod->sect_attrs->attrs[i].name);
	kfree(mod->sect_attrs);
	mod->sect_attrs = NULL;
}

static void teardown_sysfs(struct module *mod)
{
	remove_modinfo_attrs(mod);
	kfn_module_param_sysfs_remove(mod);
	if (mod->mkobj.drivers_dir) {
		kobject_put(mod->mkobj.drivers_dir);
		mod->mkobj.drivers_dir = NULL;
	}
	if (mod->holders_dir) {
		kobject_put(mod->holders_dir);
		mod->holders_dir = NULL;
	}
	remove_notes_attrs(mod);
	remove_sect_attrs(mod);
	kfn_mod_kobject_put(mod);
}

/*
 * Reproduce free_module() from the exact supported 4.4.139 source trees.
 * The 01.10 and 06.00 module teardown sources are byte-identical. Optional
 * calls below correspond only to features disabled in those configurations.
 */
static void free_module_exact(struct module *mod)
{
	teardown_sysfs(mod);

	mutex_lock(&module_mutex);
	mod->state = MODULE_STATE_UNFORMED;
	mutex_unlock(&module_mutex);

	if (kfn_ddebug_remove_module)
		kfn_ddebug_remove_module(mod->name);
	kfn_module_arch_cleanup(mod);
	kfn_destroy_params(mod->kp, mod->num_kp);

	mutex_lock(&module_mutex);
	list_del_rcu(&mod->list);
	if (kfn_mod_tree_remove)
		kfn_mod_tree_remove(mod);
	kfn_module_bug_cleanup(mod);
	kfn_synchronize_sched();
	mutex_unlock(&module_mutex);

	kfn_unset_module_init_ro_nx(mod);
	kfn_module_arch_freeing_init(mod);
	kfn_module_memfree(mod->module_init);
	kfree(mod->args);
#ifdef CONFIG_SMP
	if (mod->percpu)
		kfn_free_percpu(mod->percpu);
#endif
	if (kfn_lockdep_free_key_range)
		kfn_lockdep_free_key_range(mod->module_core, mod->core_size);
	kfn_unset_module_core_ro_nx(mod);
	kfn_module_memfree(mod->module_core);
}

static int find_unload_target(const char *name, struct module **target)
{
	struct module *mod;

	if (mutex_lock_interruptible(&module_mutex))
		return -EINTR;
	mod = find_module(name);
	if (!mod) {
		mutex_unlock(&module_mutex);
		set_status("ERR: module '%s' not found", name);
		return -ENOENT;
	}
	if (mod == THIS_MODULE) {
		mutex_unlock(&module_mutex);
		set_status("ERR: cannot unload rmmod_helper itself");
		return -EINVAL;
	}
	if (mod->state != MODULE_STATE_LIVE) {
		mutex_unlock(&module_mutex);
		set_status("ERR: module '%s' is not LIVE (state=%d)",
			   name, mod->state);
		return -EBUSY;
	}
	*target = mod;
	return 0;
}

static void notify_and_free(struct module *mod)
{
	kfn_blocking_notifier(kp_module_notify_list, MODULE_STATE_GOING, mod);
	kfn_async_synchronize_full();
	free_module_exact(mod);
}

static int do_rmmod(const char *name, bool allow_incomplete)
{
	struct module *mod;
	void (*exit_fn)(void);
	unsigned long exit_address;
	char symbol_name[MODULE_NAME_LEN + 20];
	char dependent[MODULE_NAME_LEN];
	enum dependency_result dependency;
	int ret;

	ret = find_unload_target(name, &mod);
	if (ret)
		return ret;

	snprintf(symbol_name, sizeof(symbol_name), "%s:cleanup_module", name);
	exit_address = kfn_mod_kallsyms(symbol_name);
	if (!exit_address) {
		mutex_unlock(&module_mutex);
		set_status("ERR: '%s' has no retained cleanup_module", name);
		return -EOPNOTSUPP;
	}
	exit_fn = (void (*)(void))exit_address;

	dependency = dependency_check(mod, dependent);
	if (dependency == DEPENDENCY_FOUND) {
		mutex_unlock(&module_mutex);
		set_status("ERR: '%s' is used by '%s'", name, dependent);
		return -EWOULDBLOCK;
	}
	if (dependency == DEPENDENCY_UNKNOWN && !allow_incomplete) {
		mutex_unlock(&module_mutex);
		set_status("ERR: dependency proof unavailable for '%s'; "
			   "use !%s only after manual verification",
			   name, name);
		return -EOPNOTSUPP;
	}

	mod->state = MODULE_STATE_GOING;
	mutex_unlock(&module_mutex);

	pr_info("rmmod_helper: unloading '%s'%s\n", name,
		allow_incomplete ? " [incomplete history accepted]" : "");
	exit_fn();
	notify_and_free(mod);
	mark_untracked(mod);
	set_status("OK: '%s' unloaded%s", name,
		   allow_incomplete ? " (incomplete history accepted)" : "");
	pr_info("rmmod_helper: '%s' unloaded\n", name);
	return 0;
}

static int do_rmmod_raw(const char *name)
{
	struct module *mod;
	int ret;

	ret = find_unload_target(name, &mod);
	if (ret)
		return ret;
	mod->state = MODULE_STATE_GOING;
	mutex_unlock(&module_mutex);

	pr_warn("rmmod_helper: raw teardown of '%s'; cleanup not called\n",
		name);
	notify_and_free(mod);
	mark_untracked(mod);
	set_status("OK: '%s' raw-removed without cleanup", name);
	return 0;
}

static bool valid_module_name(const char *name)
{
	const unsigned char *cursor = (const unsigned char *)name;
	size_t length = strlen(name);

	if (!length || length >= MODULE_NAME_LEN)
		return false;
	for (; *cursor; ++cursor)
		if (!((*cursor >= 'a' && *cursor <= 'z') ||
		      (*cursor >= 'A' && *cursor <= 'Z') ||
		      (*cursor >= '0' && *cursor <= '9') ||
		      *cursor == '_' || *cursor == '-'))
			return false;
	return true;
}

static ssize_t rmmod_proc_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *position)
{
	char input[MODULE_NAME_LEN + 3];
	char *name;
	size_t length;
	bool allow_incomplete = false;
	bool raw = false;
	int ret;

	(void)file;
	(void)position;
	if (!capable(CAP_SYS_MODULE))
		return -EPERM;
	if (!count)
		return 0;
	if (count >= sizeof(input)) {
		set_status("ERR: request is too long");
		return -ENAMETOOLONG;
	}
	if (copy_from_user(input, buffer, count))
		return -EFAULT;
	input[count] = '\0';
	length = count;
	while (length && (input[length - 1] == '\n' ||
			  input[length - 1] == '\r' ||
			  input[length - 1] == ' ' ||
			  input[length - 1] == '\t'))
		input[--length] = '\0';

	name = input;
	if (name[0] == '!' && name[1] == '!') {
		raw = true;
		name += 2;
	} else if (name[0] == '!') {
		allow_incomplete = true;
		++name;
	}
	if (!valid_module_name(name)) {
		set_status("ERR: invalid module name");
		return -EINVAL;
	}
	if (mutex_lock_interruptible(&unload_lock))
		return -EINTR;
	ret = raw ? do_rmmod_raw(name) : do_rmmod(name, allow_incomplete);
	mutex_unlock(&unload_lock);
	return ret ? ret : (ssize_t)count;
}

static ssize_t rmmod_proc_read(struct file *file, char __user *buffer,
			       size_t count, loff_t *position)
{
	char *output;
	char status[RMMOD_STATUS_SIZE];
	struct module *mod;
	int length;
	ssize_t result;
	unsigned int slots_consumed;
	bool dependency_tracking_complete;

	(void)file;
	output = kzalloc(RMMOD_READ_SIZE, GFP_KERNEL);
	if (!output)
		return -ENOMEM;
	mutex_lock(&status_lock);
	strlcpy(status, status_buf[0] ? status_buf : "ready", sizeof(status));
	mutex_unlock(&status_lock);
	mutex_lock(&tracked_lock);
	slots_consumed = tracked_count;
	dependency_tracking_complete = tracking_complete;
	mutex_unlock(&tracked_lock);

	length = scnprintf(
		output, RMMOD_READ_SIZE,
		"rmmod_helper v3.1 (PSVR2 firmware %s)\n"
		"Status: %s\n"
		"Dependency tracking: %s (%u/%u slots consumed)\n\n"
		"  echo name   > /proc/rmmod_helper  verified teardown\n"
		"  echo !name  > /proc/rmmod_helper  allow incomplete history\n"
		"  echo !!name > /proc/rmmod_helper  raw, no cleanup\n\n",
		RMMOD_FIRMWARE_LABEL, status,
		dependency_tracking_complete ? "complete" : "incomplete",
		slots_consumed, RMMOD_MAX_TRACKED);

	mutex_lock(&module_mutex);
	list_for_each_entry(mod, kp_modules_list, list) {
		if (mod->state == MODULE_STATE_UNFORMED)
			continue;
		if (length >= RMMOD_READ_SIZE - 80)
			break;
		length += scnprintf(output + length, RMMOD_READ_SIZE - length,
				    "  %-20s core=%px+0x%x state=%d\n",
				    mod->name, mod->module_core,
				    mod->core_size, mod->state);
	}
	mutex_unlock(&module_mutex);
	result = simple_read_from_buffer(
		buffer, count, position, output, length);
	kfree(output);
	return result;
}

static const struct file_operations rmmod_proc_fops = {
	.owner = THIS_MODULE,
	.write = rmmod_proc_write,
	.read = rmmod_proc_read,
};

#define RESOLVE(variable, symbol, type) do {				\
	(variable) = (type)kallsyms_lookup_name(symbol);			\
	pr_info("rmmod_helper: %-31s = %px\n", symbol, (variable));	\
} while (0)

#define REQUIRE(variable, symbol) do {					\
	if (!(variable)) {						\
		pr_err("rmmod_helper: required symbol '%s' is absent\n",	\
		       symbol);						\
		return -ENOSYS;						\
	}								\
} while (0)

static int resolve_kernel_contract(void)
{
	RESOLVE(kp_modules_list, "modules", struct list_head *);
	RESOLVE(kp_module_notify_list, "module_notify_list",
		struct blocking_notifier_head *);
	RESOLVE(kfn_mod_kallsyms, "module_kallsyms_lookup_name",
		mod_kallsyms_fn_t);
	RESOLVE(kfn_blocking_notifier, "blocking_notifier_call_chain",
		blocking_notifier_fn_t);
	RESOLVE(kfn_async_synchronize_full, "async_synchronize_full", sync_fn_t);
	RESOLVE(kfn_synchronize_sched, "synchronize_sched", sync_fn_t);
	RESOLVE(kfn_module_arch_cleanup, "module_arch_cleanup", void_mod_fn_t);
	RESOLVE(kfn_module_arch_freeing_init, "module_arch_freeing_init",
		void_mod_fn_t);
	RESOLVE(kfn_module_bug_cleanup, "module_bug_cleanup", void_mod_fn_t);
	RESOLVE(kfn_unset_module_init_ro_nx, "unset_module_init_ro_nx",
		void_mod_fn_t);
	RESOLVE(kfn_unset_module_core_ro_nx, "unset_module_core_ro_nx",
		void_mod_fn_t);
	RESOLVE(kfn_module_memfree, "module_memfree", void_ptr_fn_t);
	RESOLVE(kfn_destroy_params, "destroy_params", void_kvp_fn_t);
	RESOLVE(kfn_module_param_sysfs_remove, "module_param_sysfs_remove",
		void_mod_fn_t);
	RESOLVE(kfn_mod_kobject_put, "mod_kobject_put", void_mod_fn_t);
	RESOLVE(kfn_free_notes_attrs, "free_notes_attrs", free_notes_fn_t);
	RESOLVE(kfn_sysfs_remove_group, "sysfs_remove_group",
		sysfs_remove_group_fn_t);
	RESOLVE(kfn_sysfs_remove_file_ns, "sysfs_remove_file_ns",
		sysfs_remove_file_ns_fn_t);
	RESOLVE(kfn_free_percpu, "free_percpu", void_ptr_fn_t);

	/* Optional because these exact supported kernel features are disabled. */
	RESOLVE(kfn_mod_tree_remove, "mod_tree_remove", mod_tree_fn_t);
	RESOLVE(kfn_ddebug_remove_module, "ddebug_remove_module", ddebug_fn_t);
	RESOLVE(kfn_lockdep_free_key_range, "lockdep_free_key_range",
		lockdep_fn_t);

	REQUIRE(kp_modules_list, "modules");
	REQUIRE(kp_module_notify_list, "module_notify_list");
	REQUIRE(kfn_mod_kallsyms, "module_kallsyms_lookup_name");
	REQUIRE(kfn_blocking_notifier, "blocking_notifier_call_chain");
	REQUIRE(kfn_async_synchronize_full, "async_synchronize_full");
	REQUIRE(kfn_synchronize_sched, "synchronize_sched");
	REQUIRE(kfn_module_arch_cleanup, "module_arch_cleanup");
	REQUIRE(kfn_module_arch_freeing_init, "module_arch_freeing_init");
	REQUIRE(kfn_module_bug_cleanup, "module_bug_cleanup");
	REQUIRE(kfn_unset_module_init_ro_nx, "unset_module_init_ro_nx");
	REQUIRE(kfn_unset_module_core_ro_nx, "unset_module_core_ro_nx");
	REQUIRE(kfn_module_memfree, "module_memfree");
	REQUIRE(kfn_destroy_params, "destroy_params");
	REQUIRE(kfn_module_param_sysfs_remove, "module_param_sysfs_remove");
	REQUIRE(kfn_mod_kobject_put, "mod_kobject_put");
	REQUIRE(kfn_free_notes_attrs, "free_notes_attrs");
	REQUIRE(kfn_sysfs_remove_group, "sysfs_remove_group");
	REQUIRE(kfn_sysfs_remove_file_ns, "sysfs_remove_file_ns");
	REQUIRE(kfn_free_percpu, "free_percpu");
	return 0;
}

static int __init rmmod_helper_init(void)
{
	struct module *mod;
	int ret;

	pr_info("rmmod_helper: validating %s kernel contract\n",
		RMMOD_FIRMWARE_LABEL);
	ret = resolve_kernel_contract();
	if (ret)
		return ret;

	memset(tracked, 0, sizeof(tracked));
	tracked_count = 0;
	tracking_complete = true;

	/*
	 * Register before enumeration. Any concurrent loader is then either
	 * observed through COMING or found under module_mutex below. Stable slot
	 * identity makes duplicate observations harmless.
	 */
	ret = register_module_notifier(&rmmod_nb);
	if (ret) {
		pr_err("rmmod_helper: notifier registration failed: %d\n", ret);
		return ret;
	}
	mutex_lock(&module_mutex);
	list_for_each_entry(mod, kp_modules_list, list)
		if (mod->state == MODULE_STATE_LIVE && mod != THIS_MODULE) {
			mutex_lock(&tracked_lock);
			(void)add_tracked_locked(mod, true);
			mutex_unlock(&tracked_lock);
		}
	mutex_unlock(&module_mutex);

	proc_entry = proc_create("rmmod_helper", 0600, NULL,
				 &rmmod_proc_fops);
	if (!proc_entry) {
		unregister_module_notifier(&rmmod_nb);
		return -ENOMEM;
	}
	set_status("ready");
	pr_info("rmmod_helper: loaded; preexisting modules fail closed\n");
	return 0;
}

static void rmmod_helper_exit(void)
{
	if (proc_entry)
		proc_remove(proc_entry);
	unregister_module_notifier(&rmmod_nb);
	pr_info("rmmod_helper: unloaded\n");
}

module_init(rmmod_helper_init);
module_exit(rmmod_helper_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("PSVR2 Dev");
MODULE_DESCRIPTION("Exact-image module teardown helper for PSVR2 01.10/06.00");
