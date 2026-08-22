obj-m += hermes_kms.o

ccflags-y += -Wall -I$(src)/../../include/uapi

# Linux 7.2 renamed struct drm_atomic_state to struct drm_atomic_commit and
# retyped every atomic helper callback with it. Ask the target kernel's own
# header which name it uses instead of trusting LINUX_VERSION_CODE, so a tree
# that backported the rename builds too. When the header is not readable from
# here the flag is left undefined and hermes_kms.c falls back to the version.
hermes-kms-atomic-h := $(srctree)/include/drm/drm_atomic.h
ifneq ($(wildcard $(hermes-kms-atomic-h)),)
hermes-kms-atomic-commit := $(shell grep -qs 'struct drm_atomic_commit {' \
	$(hermes-kms-atomic-h) && echo 1 || echo 0)
ccflags-y += -DHERMES_KMS_HAVE_DRM_ATOMIC_COMMIT=$(hermes-kms-atomic-commit)
endif
