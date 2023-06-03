#ifndef __KSU_H_KSU
#define __KSU_H_KSU

#include "linux/capability.h"
#include "linux/types.h"
#include "linux/workqueue.h"
#include <../ksuversion>

#ifndef KSU_GIT_VERSION
#warning                                                                       \
	"KSU_GIT_VERSION not defined! It is better to make KernelSU a git submodule!"
#define KERNEL_SU_VERSION (16)
#else
#define KERNEL_SU_VERSION                                                      \
	(10000 + KSU_GIT_VERSION +                                             \
	 200) // major * 10000 + git version + 200 for historical reasons
#endif

#define KERNEL_SU_OPTION 0xDEADBEEF

#define CMD_GRANT_ROOT 0
#define CMD_BECOME_MANAGER 1
#define CMD_GET_VERSION 2
#define CMD_ALLOW_SU 3
#define CMD_DENY_SU 4
#define CMD_GET_ALLOW_LIST 5
#define CMD_GET_DENY_LIST 6
#define CMD_REPORT_EVENT 7
#define CMD_SET_SEPOLICY 8
#define CMD_CHECK_SAFEMODE 9
#define CMD_GET_APP_PROFILE 10
#define CMD_SET_APP_PROFILE 11
#define CMD_IS_UID_GRANTED_ROOT 12
#define CMD_IS_UID_SHOULD_UMOUNT 13

#define EVENT_POST_FS_DATA 1
#define EVENT_BOOT_COMPLETED 2

#define KSU_APP_PROFILE_VER 1
#define KSU_MAX_PACKAGE_NAME 256
// NGROUPS_MAX for Linux is 65535 generally, but we only supports 32 groups.
#define KSU_MAX_GROUPS 32
#define KSU_SELINUX_DOMAIN 64

struct root_profile {
	int32_t uid;
	int32_t gid;

	int32_t groups[KSU_MAX_GROUPS];
	int32_t groups_count;

	kernel_cap_t capabilities;
	char selinux_domain[KSU_SELINUX_DOMAIN];

	int32_t namespaces;
};

struct non_root_profile {
	bool umount_modules;
};

struct app_profile {
	// It may be utilized for backward compatibility, although we have never explicitly made any promises regarding this.
	u32 version;

	// this is usually the package of the app, but can be other value for special apps
	char key[KSU_MAX_PACKAGE_NAME];
	int32_t current_uid;
	bool allow_su;

	union {
		struct {
			bool use_default;
			char template_name[KSU_MAX_PACKAGE_NAME];

			struct root_profile profile;
		} rp_config;

		struct {
			bool use_default;

			struct non_root_profile profile;
		} nrp_config;
	};
};

bool ksu_queue_work(struct work_struct *work);

static inline int startswith(char *s, char *prefix)
{
	return strncmp(s, prefix, strlen(prefix));
}

static inline int endswith(const char *s, const char *t)
{
	size_t slen = strlen(s);
	size_t tlen = strlen(t);
	if (tlen > slen)
		return 1;
	return strcmp(s + slen - tlen, t);
}

#endif
