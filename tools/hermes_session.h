/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Hermes-KMS contributors */
#ifndef HERMES_KMS_USERSPACE_SESSION_H
#define HERMES_KMS_USERSPACE_SESSION_H

/*
 * Small, header-only helper for the generic UAPI v11 session capability.
 *
 * This is deliberately application-neutral.  An output owner may transfer the
 * opaque credential over any trusted IPC channel.  The file helpers below are
 * only a convenient, same-UID mechanism for command-line diagnostics and test
 * scripts; applications should normally keep the token in memory.
 */

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm/hermes_kms_drm.h>
#include <linux/sync_file.h>

/* Linux exposes these flags even in strict ISO language modes. */
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 00400000
#endif

#ifdef __cplusplus
#define HERMES_SESSION_ZERO_INIT {}
#else
#define HERMES_SESSION_ZERO_INIT {0}
#endif

struct hermes_session_credentials {
	uint64_t token[2];
	uint64_t session_id;
	uint32_t output_index;
};

/*
 * Wait for a sync_file and verify its completion status.  poll(POLLIN) alone
 * is insufficient: a fence that completed with an error is also signalled and
 * must not be reported as a successfully produced frame.
 */
static inline int hermes_sync_file_wait(int fd, int timeout_ms)
{
	struct sync_file_info info = HERMES_SESSION_ZERO_INIT;
	struct pollfd pfd = HERMES_SESSION_ZERO_INIT;
	int ret;

	pfd.fd = fd;
	pfd.events = POLLIN;
	if (fd < 0 || timeout_ms < -1) {
		errno = EINVAL;
		return -1;
	}
	do {
		ret = poll(&pfd, 1, timeout_ms);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0)
		return -1;
	if (!ret) {
		errno = ETIMEDOUT;
		return -1;
	}
	if (pfd.revents & POLLNVAL) {
		errno = EBADF;
		return -1;
	}
	if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0)
		return -1;
	if (info.status < 0) {
		int64_t fence_errno = -(int64_t)info.status;

		errno = fence_errno > 0 && fence_errno <= 4095 ?
			(int)fence_errno : EIO;
		return -1;
	}
	if (info.status != 1 || !(pfd.revents & POLLIN)) {
		errno = EIO;
		return -1;
	}

	return 0;
}

static inline void
hermes_session_memzero(void *memory, size_t length)
{
	volatile unsigned char *byte;
	size_t i;

	if (!memory)
		return;
	byte = (volatile unsigned char *)memory;
	for (i = 0; i < length; i++)
		byte[i] = 0;
}

static inline void
hermes_session_forget(struct hermes_session_credentials *credentials)
{
	if (credentials)
		hermes_session_memzero(credentials, sizeof(*credentials));
}

/* Safely identify the driver before issuing any driver-private ioctl. */
static inline int hermes_session_require_driver(int fd)
{
	struct drm_version core_version = HERMES_SESSION_ZERO_INIT;
	char core_name[HERMES_KMS_NAME_LEN] = {0};
	int saved_errno;
	int ret = -1;

	core_version.name = core_name;
	core_version.name_len = sizeof(core_name) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &core_version) < 0)
		goto out;
	if (core_version.name_len != sizeof("hermes-kms") - 1 ||
	    memcmp(core_name, "hermes-kms", sizeof("hermes-kms") - 1) != 0) {
		errno = ENODEV;
		goto out;
	}
	ret = 0;

out:
	saved_errno = errno;
	hermes_session_memzero(&core_version, sizeof(core_version));
	hermes_session_memzero(core_name, sizeof(core_name));
	if (ret)
		errno = saved_errno;
	return ret;
}

/* Require the generic capability-token UAPI used by session consumers. */
static inline int hermes_session_require_token_uapi(int fd)
{
	struct drm_hermes_kms_version version = HERMES_SESSION_ZERO_INIT;
	struct drm_hermes_kms_caps caps = HERMES_SESSION_ZERO_INIT;
	int saved_errno;
	int ret = -1;

	if (hermes_session_require_driver(fd) < 0)
		goto out;
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_VERSION, &version) < 0)
		goto out;
	if (strncmp(version.driver_name, "hermes-kms",
		    sizeof(version.driver_name)) != 0) {
		errno = EPROTO;
		goto out;
	}
	if (version.uapi_version < 11) {
		errno = EOPNOTSUPP;
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_CAPS, &caps) < 0)
		goto out;
	if (!(caps.flags & HERMES_KMS_CAP_SESSION_TOKEN)) {
		errno = EOPNOTSUPP;
		goto out;
	}
	ret = 0;

out:
	saved_errno = errno;
	hermes_session_memzero(&version, sizeof(version));
	hermes_session_memzero(&caps, sizeof(caps));
	if (ret)
		errno = saved_errno;
	return ret;
}

static inline int
hermes_session_get_owner_token(int fd,
			       struct hermes_session_credentials *credentials)
{
	struct drm_hermes_kms_session_access request = HERMES_SESSION_ZERO_INIT;
	int saved_errno;
	int ret = -1;

	if (!credentials) {
		errno = EINVAL;
		return -1;
	}
	hermes_session_forget(credentials);
	if (hermes_session_require_token_uapi(fd) < 0)
		return -1;
	request.operation = HERMES_KMS_SESSION_ACCESS_GET_TOKEN;
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_SESSION_ACCESS, &request) < 0)
		goto out;
	if (!(request.result_flags &
	      HERMES_KMS_SESSION_ACCESS_RESULT_TOKEN_VALID) ||
	    !request.session_id || (!request.token[0] && !request.token[1]) ||
	    request.output_index == UINT32_MAX) {
		errno = EPROTO;
		goto out;
	}

	credentials->token[0] = request.token[0];
	credentials->token[1] = request.token[1];
	credentials->session_id = request.session_id;
	credentials->output_index = request.output_index;
	ret = 0;

out:
	saved_errno = errno;
	hermes_session_memzero(&request, sizeof(request));
	if (ret)
		errno = saved_errno;
	return ret;
}

/*
 * Replace the session's token, keeping the session and every existing binding
 * alive, so a token that may have been exposed stops granting new binds without
 * interrupting a running consumer. With @revoke_bindings set, every bound
 * descriptor also loses access at once: its next protected ioctl fails with
 * EACCES and a blocked wait is woken to the same error. Ownership, the session
 * ID and the scanout survive either way.
 *
 * The caller's own blocked WAIT_FRAME, if it has one on another thread, also
 * fails with EACCES across a revocation and should be reissued.
 */
static inline int
hermes_session_refresh_owner_token(int fd, int revoke_bindings,
				   struct hermes_session_credentials *credentials)
{
	struct drm_hermes_kms_session_access request = HERMES_SESSION_ZERO_INIT;
	int saved_errno;
	int ret = -1;

	if (!credentials) {
		errno = EINVAL;
		return -1;
	}
	hermes_session_forget(credentials);
	if (hermes_session_require_token_uapi(fd) < 0)
		return -1;
	request.operation = revoke_bindings ?
		HERMES_KMS_SESSION_ACCESS_REVOKE_BINDINGS :
		HERMES_KMS_SESSION_ACCESS_ROTATE_TOKEN;
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_SESSION_ACCESS, &request) < 0)
		goto out;
	if (!(request.result_flags &
	      HERMES_KMS_SESSION_ACCESS_RESULT_TOKEN_VALID) ||
	    !request.session_id || (!request.token[0] && !request.token[1]) ||
	    request.output_index == UINT32_MAX) {
		errno = EPROTO;
		goto out;
	}
	if (revoke_bindings &&
	    !(request.result_flags &
	      HERMES_KMS_SESSION_ACCESS_RESULT_REVOKED)) {
		errno = EPROTO;
		goto out;
	}

	credentials->token[0] = request.token[0];
	credentials->token[1] = request.token[1];
	credentials->session_id = request.session_id;
	credentials->output_index = request.output_index;
	ret = 0;

out:
	saved_errno = errno;
	hermes_session_memzero(&request, sizeof(request));
	if (ret)
		errno = saved_errno;
	return ret;
}

/* Drop this descriptor's own capture authorization. */
static inline int hermes_session_unbind(int fd)
{
	struct drm_hermes_kms_session_access request = HERMES_SESSION_ZERO_INIT;

	if (hermes_session_require_token_uapi(fd) < 0)
		return -1;
	request.operation = HERMES_KMS_SESSION_ACCESS_UNBIND;
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_SESSION_ACCESS, &request) < 0)
		return -1;
	return 0;
}

static inline int
hermes_session_bind(int fd,
		    const struct hermes_session_credentials *credentials)
{
	struct drm_hermes_kms_session_access request = HERMES_SESSION_ZERO_INIT;
	int saved_errno;
	int ret = -1;

	if (!credentials || !credentials->session_id ||
	    (!credentials->token[0] && !credentials->token[1])) {
		errno = EINVAL;
		return -1;
	}
	if (hermes_session_require_token_uapi(fd) < 0)
		return -1;

	request.token[0] = credentials->token[0];
	request.token[1] = credentials->token[1];
	request.session_id = credentials->session_id;
	request.operation = HERMES_KMS_SESSION_ACCESS_BIND;
	request.output_index = credentials->output_index;
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_SESSION_ACCESS, &request) < 0)
		goto out;
	if (!(request.result_flags & HERMES_KMS_SESSION_ACCESS_RESULT_BOUND) ||
	    request.session_id != credentials->session_id ||
	    request.output_index != credentials->output_index) {
		errno = EPROTO;
		goto out;
	}
	ret = 0;

out:
	saved_errno = errno;
	hermes_session_memzero(&request, sizeof(request));
	if (ret)
		errno = saved_errno;
	return ret;
}

static inline int
hermes_session_write_all(int fd, const char *buffer, size_t length)
{
	while (length) {
		ssize_t written = write(fd, buffer, length);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!written) {
			errno = EIO;
			return -1;
		}
		buffer += (size_t)written;
		length -= (size_t)written;
	}
	return 0;
}

static inline int
hermes_session_random_bytes(void *buffer, size_t length)
{
	unsigned char *bytes = (unsigned char *)buffer;
	int fd;
	int saved_errno;

	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	while (length) {
		ssize_t count = read(fd, bytes, length);

		if (count < 0) {
			if (errno == EINTR)
				continue;
			saved_errno = errno;
			close(fd);
			errno = saved_errno;
			return -1;
		}
		if (!count) {
			close(fd);
			errno = EIO;
			return -1;
		}
		bytes += (size_t)count;
		length -= (size_t)count;
	}
	if (close(fd) < 0)
		return -1;
	return 0;
}

/* Atomically publish a mode-0600 credential without replacing any file. */
static inline int
hermes_session_write_file_internal(
	const char *path,
	const struct hermes_session_credentials *credentials, int replace)
{
	char line[160] = {0};
	char *temporary = NULL;
	unsigned char nonce[16] = {0};
	size_t path_length;
	size_t i;
	unsigned int attempt;
	int fd = -1;
	int line_length;
	int saved_errno;
	int ret = -1;

	if (!path || !*path || !credentials || !credentials->session_id ||
	    (!credentials->token[0] && !credentials->token[1])) {
		errno = EINVAL;
		return -1;
	}
	line_length = snprintf(line, sizeof(line),
			       "HERMES_KMS_SESSION_V1 %" PRIu32 " %" PRIu64
			       " %016" PRIx64 " %016" PRIx64 "\n",
			       credentials->output_index, credentials->session_id,
			       credentials->token[0], credentials->token[1]);
	if (line_length < 0 || (size_t)line_length >= sizeof(line)) {
		errno = EOVERFLOW;
		goto out;
	}

	path_length = strlen(path);
	if (path_length > SIZE_MAX - sizeof(".tmp.") - sizeof(nonce) * 2) {
		errno = ENAMETOOLONG;
		goto out;
	}
	temporary = (char *)malloc(path_length + sizeof(".tmp.") +
				   sizeof(nonce) * 2);
	if (!temporary)
		goto out;

	for (attempt = 0; attempt < 16; attempt++) {
		static const char hex[] = "0123456789abcdef";
		char *suffix;

		if (hermes_session_random_bytes(nonce, sizeof(nonce)) < 0)
			goto out;
		memcpy(temporary, path, path_length);
		memcpy(temporary + path_length, ".tmp.", sizeof(".tmp.") - 1);
		suffix = temporary + path_length + sizeof(".tmp.") - 1;
		for (i = 0; i < sizeof(nonce); i++) {
			suffix[i * 2] = hex[nonce[i] >> 4];
			suffix[i * 2 + 1] = hex[nonce[i] & 0x0f];
		}
		suffix[sizeof(nonce) * 2] = '\0';
		fd = open(temporary,
			  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
			  S_IRUSR | S_IWUSR);
		if (fd >= 0 || errno != EEXIST)
			break;
	}
	if (fd < 0)
		goto out;
	if (hermes_session_write_all(fd, line, (size_t)line_length) < 0 ||
	    fsync(fd) < 0)
		goto out;
	if (close(fd) < 0) {
		fd = -1;
		goto out;
	}
	fd = -1;

	/*
	 * link() is an atomic no-replace publication on the same filesystem;
	 * rename() is the atomic replacing form, for rotating the credential in
	 * a file a consumer may already be reading. Either way no reader ever
	 * sees a partially written line.
	 */
	if (replace) {
		if (rename(temporary, path) < 0)
			goto out;
		temporary[0] = '\0';
	} else if (link(temporary, path) < 0) {
		goto out;
	}
	ret = 0;

out:
	saved_errno = errno;
	if (fd >= 0)
		close(fd);
	if (temporary) {
		/* A successful rename already consumed the temporary name. */
		if (temporary[0])
			unlink(temporary);
		free(temporary);
	}
	hermes_session_memzero(line, sizeof(line));
	hermes_session_memzero(nonce, sizeof(nonce));
	if (ret)
		errno = saved_errno;
	return ret;
}

/* Publish a credential, refusing to replace an existing file. */
static inline int
hermes_session_write_file(const char *path,
			  const struct hermes_session_credentials *credentials)
{
	return hermes_session_write_file_internal(path, credentials, 0);
}

/*
 * Replace a published credential in place, for republishing after a token
 * rotation. The file a consumer may be reading is swapped atomically, so it
 * either sees the old credential or the new one and never a torn line.
 */
static inline int
hermes_session_replace_file(
	const char *path, const struct hermes_session_credentials *credentials)
{
	return hermes_session_write_file_internal(path, credentials, 1);
}

static inline int
hermes_session_read_file(const char *path,
			 struct hermes_session_credentials *credentials)
{
	struct hermes_session_credentials parsed = HERMES_SESSION_ZERO_INIT;
	struct stat status;
	char magic[32] = {0};
	char buffer[256] = {0};
	char canonical[160] = {0};
	char extra = 0;
	char overflow = 0;
	size_t used = 0;
	ssize_t length = 0;
	int fd = -1;
	int canonical_length;
	int fields;
	int saved_errno;
	int ret = -1;

	if (!path || !*path || !credentials) {
		errno = EINVAL;
		return -1;
	}
	hermes_session_forget(credentials);
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		goto out;
	if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
	    status.st_uid != geteuid() || (status.st_mode & 077) != 0 ||
	    status.st_size <= 0 || status.st_size >= (off_t)sizeof(buffer)) {
		errno = EACCES;
		goto out;
	}
	for (;;) {
		do {
			length = read(fd, buffer + used,
				      sizeof(buffer) - 1 - used);
		} while (length < 0 && errno == EINTR);
		if (length < 0)
			break;
		if (!length)
			break;
		used += (size_t)length;
		if (used == sizeof(buffer) - 1) {
			do {
				length = read(fd, &overflow, 1);
			} while (length < 0 && errno == EINTR);
			if (length > 0) {
				errno = EFBIG;
				length = -1;
			}
			break;
		}
	}
	if (close(fd) < 0 && length >= 0)
		length = -1;
	fd = -1;
	if (length < 0)
		goto out;
	if (used != (size_t)status.st_size || memchr(buffer, '\0', used)) {
		errno = EINVAL;
		goto out;
	}
	buffer[used] = '\0';

	fields = sscanf(buffer,
			"%31s %" SCNu32 " %" SCNu64 " %" SCNx64 " %" SCNx64 " %c",
			magic, &parsed.output_index, &parsed.session_id,
			&parsed.token[0], &parsed.token[1], &extra);
	if (fields != 5 || strcmp(magic, "HERMES_KMS_SESSION_V1") != 0 ||
	    !parsed.session_id || (!parsed.token[0] && !parsed.token[1])) {
		errno = EINVAL;
		goto out;
	}
	canonical_length = snprintf(canonical, sizeof(canonical),
				    "HERMES_KMS_SESSION_V1 %" PRIu32 " %" PRIu64
				    " %016" PRIx64 " %016" PRIx64 "\n",
				    parsed.output_index, parsed.session_id,
				    parsed.token[0], parsed.token[1]);
	if (canonical_length < 0 || (size_t)canonical_length != used ||
	    memcmp(buffer, canonical, used) != 0) {
		errno = EINVAL;
		goto out;
	}
	*credentials = parsed;
	ret = 0;

out:
	saved_errno = errno;
	if (fd >= 0)
		close(fd);
	hermes_session_forget(&parsed);
	hermes_session_memzero(buffer, sizeof(buffer));
	hermes_session_memzero(canonical, sizeof(canonical));
	hermes_session_memzero(magic, sizeof(magic));
	hermes_session_memzero(&extra, sizeof(extra));
	hermes_session_memzero(&overflow, sizeof(overflow));
	if (ret)
		errno = saved_errno;
	return ret;
}

static inline int hermes_session_bind_file(int fd, const char *path,
					   uint32_t *output_index)
{
	struct hermes_session_credentials credentials;
	int ret;

	if (output_index)
		*output_index = UINT32_MAX;
	memset(&credentials, 0, sizeof(credentials));
	ret = hermes_session_read_file(path, &credentials);
	if (!ret)
		ret = hermes_session_bind(fd, &credentials);
	if (!ret && output_index)
		*output_index = credentials.output_index;
	hermes_session_forget(&credentials);
	return ret;
}

static inline int
hermes_session_open_bound_render_credentials(
	const struct hermes_session_credentials *credentials,
	char *device_path, size_t device_path_size, uint32_t *output_index)
{
	glob_t render_nodes;
	int bind_errno = 0;
	int probe_errno = 0;
	int open_errno = 0;
	int found_hermes = 0;
	int glob_called = 0;
	int glob_ret;
	int saved_errno;
	int fd = -1;

	if (!credentials || !credentials->session_id ||
	    (!credentials->token[0] && !credentials->token[1]) ||
	    (device_path && !device_path_size)) {
		errno = EINVAL;
		return -1;
	}
	if (device_path)
		device_path[0] = '\0';
	if (output_index)
		*output_index = UINT32_MAX;
	memset(&render_nodes, 0, sizeof(render_nodes));

	glob_called = 1;
	glob_ret = glob("/dev/dri/renderD*", 0, NULL, &render_nodes);
	if (glob_ret != 0) {
		if (glob_ret == GLOB_NOSPACE)
			errno = ENOMEM;
		else if (glob_ret == GLOB_NOMATCH)
			errno = ENOENT;
		else
			errno = EIO;
		goto out;
	}

	for (size_t i = 0; i < render_nodes.gl_pathc; i++) {
		const char *candidate = render_nodes.gl_pathv[i];
		int candidate_fd;

		candidate_fd = open(candidate, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
		if (candidate_fd < 0) {
			if (!open_errno)
				open_errno = errno;
			continue;
		}
		if (hermes_session_require_token_uapi(candidate_fd) < 0) {
			int require_errno = errno;

			close(candidate_fd);
			if (require_errno != ENODEV) {
				found_hermes = 1;
				probe_errno = require_errno;
			}
			continue;
		}
		found_hermes = 1;
		if (hermes_session_bind(candidate_fd, credentials) < 0) {
			bind_errno = errno;
			close(candidate_fd);
			continue;
		}
		if (device_path) {
			size_t candidate_length = strlen(candidate);

			if (candidate_length >= device_path_size) {
				close(candidate_fd);
				errno = ENAMETOOLONG;
				goto out;
			}
			memcpy(device_path, candidate, candidate_length + 1);
		}
		if (output_index)
			*output_index = credentials->output_index;
		fd = candidate_fd;
		break;
	}

	if (fd < 0) {
		if (found_hermes)
			errno = bind_errno ? bind_errno :
				(probe_errno ? probe_errno : EACCES);
		else
			errno = open_errno == EACCES ? EACCES : ENODEV;
	}

out:
	saved_errno = errno;
	if (glob_called)
		globfree(&render_nodes);
	if (fd < 0)
		errno = saved_errno;
	return fd;
}

/*
 * Open the render node that owns a file-backed session capability.  This is a
 * convenience for diagnostics in multi-device systems: the credential is read
 * exactly once, then each Hermes render node is tried until BIND succeeds.
 */
static inline int
hermes_session_open_bound_render(const char *path, char *device_path,
				 size_t device_path_size,
				 uint32_t *output_index)
{
	struct hermes_session_credentials credentials = HERMES_SESSION_ZERO_INIT;
	int saved_errno;
	int fd = -1;

	if (!path || !*path || (device_path && !device_path_size)) {
		errno = EINVAL;
		return -1;
	}
	if (device_path)
		device_path[0] = '\0';
	if (output_index)
		*output_index = UINT32_MAX;
	if (hermes_session_read_file(path, &credentials) < 0)
		goto out;
	fd = hermes_session_open_bound_render_credentials(
		&credentials, device_path, device_path_size, output_index);

out:
	saved_errno = errno;
	hermes_session_forget(&credentials);
	if (fd < 0)
		errno = saved_errno;
	return fd;
}

#undef HERMES_SESSION_ZERO_INIT

#endif /* HERMES_KMS_USERSPACE_SESSION_H */
