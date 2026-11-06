/* SPDX-License-Identifier: MIT */
/*
 * geforce-sriov host CLI: configuration, sysfs inspection and lifecycle commands.
 *
 * Read-only commands (devices, doctor, status) accept a diagnostic sysfs root.
 * Every mutating function in this interface takes no root argument: it acts on
 * the live /sys only, so a fixture root can never reach a mutation path.
 */
#ifndef GS_LIFECYCLE_H
#define GS_LIFECYCLE_H

#include <stddef.h>
#include <sys/types.h>

#include "gs_devices.h"

/* Exit codes of the geforce-sriov binary. */
#define GS_EXIT_OK 0
#define GS_EXIT_FAIL 1         /* a step failed; the message names it */
#define GS_EXIT_USAGE 2        /* bad command line */
#define GS_EXIT_REFUSED 3      /* a precondition does not hold; nothing was changed */
#define GS_EXIT_LOCKED 4       /* another geforce-sriov process holds the lock */
#define GS_EXIT_COLD_CYCLE 5   /* guard set: a full power-off is required before anything else */

#define GS_MODULE_NAME "geforce_sriov"
#define GS_DKMS_NAME "geforce-sriov"
#define GS_UNIT_PREPARE "geforce-sriov-prepare.service"
#define GS_UNIT_DIR "/etc/systemd/system"
/* The link `systemctl enable` creates for the boot unit (WantedBy=sysinit.target). */
#define GS_UNIT_WANTS GS_UNIT_DIR "/sysinit.target.wants/" GS_UNIT_PREPARE

#define GS_CONFIG_PATH "/etc/geforce-sriov/config"
#define GS_CONFIG_ENV "GEFORCE_SRIOV_CONFIG"
#define GS_CONFIG_MAX_BYTES 65536
#define GS_STATE_DIR "/var/lib/geforce-sriov"
#define GS_ATTEMPT_NAME "prepare-attempt"
#define GS_ATTEMPT_MARKER GS_STATE_DIR "/" GS_ATTEMPT_NAME
#define GS_MANIFEST GS_STATE_DIR "/installed-files"
#define GS_RUN_DIR "/run/geforce-sriov"
#define GS_PREPARED_NAME "prepared"
#define GS_PREPARED_DIR GS_RUN_DIR "/" GS_PREPARED_NAME
/* This boot's deferral by the boot unit; nvidia-vgpud's drop-in asserts that it is absent. */
#define GS_DEFERRED_NAME "deferred"
#define GS_DEFERRED GS_RUN_DIR "/" GS_DEFERRED_NAME
#define GS_LOCK_PATH GS_RUN_DIR "/lock"
#define GS_LIVE_SYSFS "/sys"
#define GS_LIVE_PROC "/proc"
/* The only firmware framebuffer kind that `prepare --release-console` releases. */
#define GS_SIMPLEFB_PREFIX "simple-framebuffer."
#define GS_SRIOV_MANAGE_DEFAULT "/usr/lib/nvidia/sriov-manage"
#define GS_DEFAULT_PREFIX "/usr/local"

#define GS_CFG_MAX_BDFS GS_MAX_PFS
#define GS_PATH_MAX 4096

/* Canonical PCI address as sysfs names it: dddd:bb:ss.f, lowercase hex, 4 to 8 domain digits. */
struct gs_addr {
	unsigned int domain;
	unsigned int bus;
	unsigned int slot;
	unsigned int func;
	char str[GS_BDF_STR_SIZE];
};

/* 0 when s is exactly a canonical lowercase address; -1 otherwise. */
int gs_addr_parse(const char *s, struct gs_addr *out);

/* Tri-state switch values for configuration overrides. */
#define GS_SW_UNSET (-1)
#define GS_SW_OFF 0
#define GS_SW_ON 1

/*
 * /etc/geforce-sriov/config, shared with the identity hook. Syntax: at most
 * 65536 bytes; lines split on '\n' (the last may lack one); empty lines and
 * lines whose first byte is '#' are ignored; any other line is key=value with
 * key [a-z0-9_]+, a non-empty value and no whitespace or '\r' anywhere; a
 * duplicate or unknown key makes the whole file invalid.
 */
struct gs_config {
	struct gs_addr bdfs[GS_CFG_MAX_BDFS];
	unsigned int nbdfs;
	/* Boot display PFs whose console `prepare --release-console` may release; a subset of bdfs. */
	struct gs_addr boot_display_release[GS_CFG_MAX_BDFS];
	unsigned int nrelease;
	int ari;                 /* default on */
	int msix;                /* default on */
	int experimental_vfs;    /* default off */
	int twin_id;             /* default off */
	unsigned int vfs;        /* VFs per card, 1..GS_VFS_MAX; 0: not set, GS_DEFAULT_VFS applies */
	int identity;            /* hook only, default off */
	int identity_force_host; /* hook only, default off */
	char sriov_manage[GS_PATH_MAX];
	int present;             /* the file existed */
};

void gs_config_defaults(struct gs_config *cfg);
int gs_config_parse(const char *text, size_t len, struct gs_config *cfg, char *err, size_t errlen);
/* Missing file: defaults with present=0 and return 0. Invalid: -1 and err. */
int gs_config_load(const char *path, struct gs_config *cfg, char *err, size_t errlen);
/* Canonical text of cfg; returns length or -1 when buf is too small. */
int gs_config_render(const struct gs_config *cfg, char *buf, size_t len);
/* $GEFORCE_SRIOV_CONFIG when set and non-empty, else GS_CONFIG_PATH. */
const char *gs_config_path(void);
/* 1 when boot_display_release lists pf. */
int gs_config_grants_release(const struct gs_config *cfg, const char *pf);
/* Comma-separated list of 1..8 canonical function-0 addresses without duplicates; errors name key. */
int gs_parse_addr_list(const char *key, const char *value, struct gs_addr *out, unsigned int *n, char *err,
		       size_t errlen);
int gs_parse_switch(const char *value, int *out);
/* "a,b,c" of n addresses into buf (room for GS_BDF_LIST_STR_SIZE). */
void gs_addr_list_str(const struct gs_addr *list, unsigned int n, char *buf, size_t len);
/* The VF count the module is loaded with: vfs, or GS_DEFAULT_VFS when not set. */
unsigned int gs_config_vfs(const struct gs_config *cfg);
/* vfs=N value: decimal 1..GS_VFS_MAX without a leading zero. 0 or -1. */
int gs_parse_vfs(const char *value, unsigned int *out);

/* One PCI function of the inspected card or its bus. */
struct gs_fn {
	struct gs_addr bdf;
	unsigned int vendor;
	unsigned int device;
	unsigned int class_code;  /* 24-bit class */
	int is_vf;
	char driver[64];          /* "" when unbound */
};

#define GS_MAX_BUS_FNS 32

/* Everything read about one selected physical function and its bus. */
struct gs_card {
	struct gs_addr pf;
	int present;
	unsigned int vendor, device, subvendor, subdevice, class_code;
	int is_vf;
	const struct gs_device *entry; /* table entry, or NULL */
	char driver[64];
	int boot_vga;             /* -1 unknown */
	int has_sriov;            /* sriov_totalvfs exists */
	unsigned int totalvfs, numvfs;
	int ari_enabled;          /* -1 unknown */
	char iommu_group[32];
	/* From <pf>/resource: 1 assigned, 0 all-zero (unassigned), -1 unknown or absent. */
	int bar0_assigned;        /* line 0: BAR0 */
	int vf_bars_assigned;     /* lines 7-12: the SR-IOV VF BARs (CONFIG_PCI_IOV) */
	int bridge_is_pci;        /* the parent is a PCI bridge function */
	struct gs_addr bridge;
	char bridge_dir[GS_PATH_MAX];
	char rescan_path[GS_PATH_MAX];
	int rescan_present;
	char bridge_pm[16];       /* power/control */
	char bridge_rt[32];       /* power/runtime_status */
	int bridge_ari_fwd_supported; /* DevCap2 bit 5; -1 unknown */
	struct gs_fn fns[GS_MAX_BUS_FNS];   /* card functions other than the PF (same slot) */
	unsigned int nfns;
	struct gs_fn others[GS_MAX_BUS_FNS]; /* functions on the bus outside the card's slot */
	unsigned int nothers;
	unsigned int nvfs_on_bus;
};

/* Reads sysroot (e.g. "/sys") only. Returns 0; card->present tells whether the PF exists. */
int gs_card_inspect(const char *sysroot, const struct gs_addr *pf, struct gs_card *card);
/*
 * The configured VF count against one listed card: -1 with why when vfs is set above what the card
 * offers (its hardware TotalVFs, gs_vf_ceiling()); 1 with why when the card would be given more than
 * GS_DEFAULT_VFS VFs, which are expected not to work (the module refuses them on the measured
 * boards); 0 otherwise.
 */
int gs_vfs_check(const struct gs_device *e, const struct gs_config *cfg, char *why, size_t len);
/* "hardware default 16, geforce-sriov presents 4 (default)" for a listed card. */
void gs_vfs_describe(const struct gs_device *e, const struct gs_config *cfg, char *buf, size_t len);
/* SR-IOV presented with sriov_totalvfs 1..gs_config_vfs(cfg)? 0, or -1 with why. */
int gs_check_presented(const struct gs_card *c, const struct gs_config *cfg, char *why, size_t len);

enum gs_verdict { GS_PLAN_READY, GS_PLAN_REFUSE };

/*
 * Console users of a boot display PF, read by gs_display_inspect(). The only firmware framebuffer
 * it accepts is the kernel's simple-framebuffer.N platform device whose parent is this PF.
 */
enum gs_fw_state { GS_FW_NONE, GS_FW_RELEASED, GS_FW_BOUND };
struct gs_display {
	char fw[64];              /* the PF's own simple-framebuffer.N, "" when it has none */
	char fw_dir[GS_PATH_MAX]; /* its resolved sysfs directory */
	char fw_driver[64];       /* its driver; "" when unbound (released) */
	unsigned int nconsumers;  /* fb and DRM devices of that framebuffer */
	int proc_checked;         /* /proc/iomem and /proc/consoles were read */
	char refuse[384];         /* why releasing the console is not safe; "" when it is */
};

/*
 * Reads sysroot and, when procroot is not NULL, procroot/iomem and procroot/consoles. Any user
 * of the card's memory or console other than its own firmware framebuffer, an unreadable source,
 * or a firmware framebuffer that cannot be attributed to this PF sets refuse.
 */
int gs_display_inspect(const char *sysroot, const char *procroot, const struct gs_card *c, struct gs_display *d);
static inline enum gs_fw_state gs_display_fw_state(const struct gs_display *d)
{
	return !d->fw[0] ? GS_FW_NONE : d->fw_driver[0] ? GS_FW_BOUND : GS_FW_RELEASED;
}
/*
 * /proc/iomem text: the only resource inside a BAR of pf may be one BOOTFB, and only when the PF
 * has its firmware framebuffer; while that is released, nothing may remain inside BOOTFB.
 * 0 or -1 with why.
 */
int gs_iomem_check(const char *iomem, const char *pf, enum gs_fw_state fw, char *why, size_t len);
/* /proc/consoles text: -1 with why when an early EFI framebuffer console (efifb) is registered. */
int gs_consoles_check(const char *consoles, char *why, size_t len);

#define GS_PLAN_MAX_UNBIND 8
struct gs_plan {
	enum gs_verdict verdict;
	char reason[768];
	struct gs_addr unbind[GS_PLAN_MAX_UNBIND]; /* audio functions of this card to unbind */
	unsigned int nunbind;
	char release[64];         /* firmware framebuffer to unbind first; "" when none is bound */
	char release_driver[64];  /* its driver */
};

/*
 * Pure decision over an inspected card: may `prepare` remove and rescan it? display is NULL
 * unless the config grants a console release for the card; a boot display PF is refused without it.
 */
void gs_plan_prepare(const struct gs_card *card, const struct gs_display *display, struct gs_plan *plan);
/*
 * Install-time selection check (the PF may legitimately be bound while installing). A boot display
 * PF is accepted only with release_granted.
 */
int gs_check_selectable(const struct gs_card *card, int release_granted, char *reason, size_t len);

/* GS_RUN_DIR/prepared/<pf>, written by prepare for each card it prepared this boot. */
struct gs_prepared {
	int present;
	char boot_id[64];
	char console_release[64]; /* firmware framebuffer released for this card, "none", or "" */
};
int gs_prepared_read(const char *run_dir, const struct gs_addr *pf, struct gs_prepared *rec);
/*
 * May `enable` act on this card: prepared in the boot boot_id names, and, for a boot display PF or
 * one whose console was released, released this boot with the config's grant? 0 or -1 with why.
 */
int gs_check_enable(const struct gs_card *c, const struct gs_config *cfg, const struct gs_prepared *rec,
		    const char *boot_id, char *why, size_t len);

/* Read-only commands. sysroot NULL means the live /sys. */
struct gs_diag_opts {
	const char *sysroot;
	const char *config_path;   /* NULL: default rules (see gs_cmd_doctor) */
	struct gs_addr devices[GS_CFG_MAX_BDFS];
	unsigned int ndevices;
};
int gs_cmd_devices(void);
int gs_cmd_doctor(const struct gs_diag_opts *opts);
int gs_cmd_status(const struct gs_diag_opts *opts);

/*
 * Where prepare reads and writes: gs_host_live, or a fixture tree with a simulated kernel behind
 * write_attr and run (tests).
 */
struct gs_host {
	const char *sys;       /* sysfs root */
	const char *proc;      /* procfs root */
	const char *state_dir; /* the guard GS_ATTEMPT_NAME */
	const char *run_dir;   /* this boot: GS_PREPARED_NAME records, GS_DEFERRED_NAME */
	int (*write_attr)(const char *path, const char *value);
	int (*run)(const char *const argv[]);
};
extern const struct gs_host gs_host_live;

/* Mutating commands: live system only. */
struct gs_overrides {
	int ari, msix, experimental_vfs, twin_id; /* GS_SW_* */
};
struct gs_prepare_opts {
	int boot; /* run by the boot unit: with boot_display_release set, defer and change nothing */
	struct gs_addr release[GS_CFG_MAX_BDFS]; /* --release-console: must equal boot_display_release */
	unsigned int nrelease;
};
int gs_cmd_prepare(const struct gs_overrides *ov, const struct gs_prepare_opts *po, int clear_guard);
/* prepare after the root check, lock and config load (cfg with the overrides applied). */
int gs_prepare(const struct gs_host *h, struct gs_config *cfg, const struct gs_prepare_opts *po);
int gs_cmd_enable(void);

/*
 * Ada CPU-RM host stack (docs/ada.md), after `enable`. activate: restart nvidia-vgpud and
 * nvidia-vgpu-mgr, re-probe each selected Ada PF (driver unbind/bind, never remove/rescan),
 * optionally set the vGPU type, then power/control=on on each VF, gated on the readback.
 * check: read-only gate before every VM start. Other cards get no re-probe, type or power write,
 * but the host-wide daemon restart re-registers their VFs too; activate refuses while any vGPU exists.
 */
#define GS_VGPU_TYPE_MAX 999999u
struct gs_activate_opts {
	struct gs_addr devices[GS_CFG_MAX_BDFS]; /* --device; none: every selected Ada GPU */
	unsigned int ndevices;
	unsigned int type;       /* --type N; 0: leave current_vgpu_type alone */
	int dry_run;
	unsigned int settle_ms;  /* pause after nvidia-vgpu-mgr is active, before the re-probe */
	const char *plugin;      /* libnvidia-vgpu.so whose SHA-256 must be the H5 copy; NULL: not checked (tests) */
};
/* --type value: decimal 1..GS_VGPU_TYPE_MAX without a leading zero. 0 or -1. */
int gs_parse_vgpu_type(const char *value, unsigned int *out);
int gs_cmd_activate(const struct gs_activate_opts *o);
/* activate after the root check, lock and config load. */
int gs_activate(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o);
int gs_cmd_check(const struct gs_activate_opts *o);
/*
 * check against h->sys with cfg (tests use a fixture host); a point-in-time read with no lock, so
 * nothing stops a re-probe or daemon restart between it and a VM start. NOT READY while h->state_dir holds the
 * guard or h->run_dir holds GS_ACTIVATING_NAME, which activate writes before its first change and
 * removes only after every VF read back on/active.
 */
#define GS_ACTIVATING_NAME "activating"
int gs_check_vfs(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o);

struct gs_install_opts {
	const char *prefix;        /* absolute; default GS_DEFAULT_PREFIX */
	const char *destdir;       /* "" or NULL: live install */
	const char *source;        /* built tree; NULL: directory of the running binary */
	struct gs_addr devices[GS_CFG_MAX_BDFS];
	unsigned int ndevices;
	struct gs_addr release[GS_CFG_MAX_BDFS]; /* --boot-display-release, each also a --device */
	unsigned int nrelease;
	struct gs_overrides ov;
	unsigned int vfs;          /* --vfs N; 0: not given */
};
int gs_cmd_install(const struct gs_install_opts *opts);
int gs_cmd_uninstall(int unload);

/* The consent text shown wherever a console release is granted or performed. */
#define GS_CONSOLE_RELEASE_NOTE \
	"Console release: the Linux console on that GPU stays detached until the next boot (the kernel\n" \
	"switches to a dummy console; the monitor may keep the last frame or lose signal). Logs continue\n" \
	"in the journal and over SSH; starting and stopping guests does not touch the display."

/* What the experimental_vfs and twin_id switches do, wherever an option is described. */
#define GS_EXPVFS_OPT_MSG "experimental_vfs (default off): " GS_EXPVFS_BODY
#define GS_TWIN_ID_OPT_MSG "twin_id (default off): " GS_TWIN_ID_BODY

/* Utilities shared by the command implementations. */
int gs_read_attr(const char *path, char *buf, size_t len);
int gs_read_hex(const char *path, unsigned int *out);
int gs_read_uint(const char *path, unsigned int *out);
/* Copy src into dst[len], truncating; dst is always terminated. */
void gs_copy(char *dst, size_t len, const char *src);
int gs_write_attr(const char *path, const char *value);
int gs_pathf(char *out, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int gs_exists(const char *path);
int gs_fsync_dir(const char *dir);
int gs_write_file_atomic(const char *path, const void *data, size_t len, mode_t mode);
int gs_mkdir_p(const char *path, mode_t mode);
int gs_read_whole(const char *path, char **data, size_t *len, size_t max);
/* fork + execv of argv[0] (absolute, or looked up in a fixed system PATH); no shell. */
int gs_run(const char *const argv[]);
int gs_find_program(const char *name, char *out, size_t len);
void gs_sleep_ms(unsigned int ms);
int gs_boot_id(char *buf, size_t len);
int gs_validate_abs_path(const char *p);
int gs_lock(void);

/* SHA-256 of a file as 64 lowercase hex characters. */
int gs_sha256_file(const char *path, char hex[65]);
void gs_sha256_buf(const void *data, size_t len, char hex[65]);

#endif
