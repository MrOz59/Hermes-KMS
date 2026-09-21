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

# The DRM-owned vblank timer helpers are absent from older LTS kernels (such
# as Ubuntu 24.04's 6.8). Select the implementation from the target headers,
# since distribution kernels can backport the helpers independently of version.
hermes-kms-vblank-helper-h := $(srctree)/include/drm/drm_vblank_helper.h
ccflags-y += -DHERMES_KMS_HAVE_DRM_VBLANK_TIMER=$(if $(wildcard $(hermes-kms-vblank-helper-h)),1,0)

# Older DRM shmem helpers import through the generic scatter/gather path.
hermes-kms-shmem-h := $(srctree)/include/drm/drm_gem_shmem_helper.h
hermes-kms-shmem-import-no-map := $(shell grep -qs 'drm_gem_shmem_prime_import_no_map' \
	$(hermes-kms-shmem-h) && echo 1 || echo 0)
ccflags-y += -DHERMES_KMS_HAVE_SHMEM_IMPORT_NO_MAP=$(hermes-kms-shmem-import-no-map)

# drm_gem_huge_mnt_create() lets a driver back its GEM objects with a
# huge=within_size tmpfs, so their pages come in 2 MiB folios. Only newer
# kernels have it; huge_gem is refused with a warning elsewhere.
hermes-kms-drm-gem-h := $(srctree)/include/drm/drm_gem.h
hermes-kms-gem-huge-mnt := $(shell grep -qs 'drm_gem_huge_mnt_create' \
	$(hermes-kms-drm-gem-h) && echo 1 || echo 0)
ccflags-y += -DHERMES_KMS_HAVE_GEM_HUGE_MNT=$(hermes-kms-gem-huge-mnt)

# The platform .remove callback returned int before the void conversion.
hermes-kms-platform-h := $(srctree)/include/linux/platform_device.h
hermes-kms-platform-remove-void := $(shell grep -qFs 'void (*remove)(struct platform_device *);' \
	$(hermes-kms-platform-h) && echo 1 || echo 0)
ccflags-y += -DHERMES_KMS_HAVE_PLATFORM_REMOVE_VOID=$(hermes-kms-platform-remove-void)

# MODULE_IMPORT_NS took an identifier on 6.8 and takes a string on newer
# kernels. Inspect the macro body to use the syntax expected by modpost.
hermes-kms-module-h := $(srctree)/include/linux/module.h
hermes-kms-module-ns-stringify := $(shell grep -qs 'MODULE_INFO(import_ns, __stringify(ns))' \
	$(hermes-kms-module-h) && echo 1 || echo 0)
ccflags-y += -DHERMES_KMS_MODULE_NS_STRINGIFY=$(hermes-kms-module-ns-stringify)
