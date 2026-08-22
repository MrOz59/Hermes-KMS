/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Hermes-KMS contributors */
/* Userspace-only regression tests for tools/hermes_session.h. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../tools/hermes_session.h"

static int bytes_are_zero(const void *memory, size_t length)
{
	const unsigned char *bytes = memory;

	for (size_t i = 0; i < length; i++)
		if (bytes[i] != 0)
			return 0;
	return 1;
}

static int credentials_equal(const struct hermes_session_credentials *left,
			     const struct hermes_session_credentials *right)
{
	return left->token[0] == right->token[0] &&
	       left->token[1] == right->token[1] &&
	       left->session_id == right->session_id &&
	       left->output_index == right->output_index;
}

static int make_path(char *destination, size_t destination_size,
		     const char *directory, const char *name)
{
	int length = snprintf(destination, destination_size, "%s/%s", directory,
			      name);

	if (length < 0 || (size_t)length >= destination_size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int append_trailing_garbage(const char *path)
{
	static const char garbage[] = "trailing-garbage\n";
	int saved_errno;
	int fd;

	fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (hermes_session_write_all(fd, garbage, sizeof(garbage) - 1) < 0) {
		saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
	return close(fd);
}

static void report_failure(unsigned int line, const char *message)
{
	fprintf(stderr, "session-file:%u: %s (errno=%d: %s)\n", line, message,
		errno, strerror(errno));
}

#define REQUIRE(condition, message) \
	do { \
		if (!(condition)) { \
			report_failure(__LINE__, (message)); \
			goto out; \
		} \
	} while (0)

int main(void)
{
	char directory[] = "/tmp/hermes-session-file.XXXXXX";
	char session_path[PATH_MAX] = {0};
	char symlink_path[PATH_MAX] = {0};
	char trailing_path[PATH_MAX] = {0};
	char missing_path[PATH_MAX] = {0};
	char device_path[1] = {0};
	unsigned char scratch[48];
	struct hermes_session_credentials expected = {
		.token = {
			UINT64_C(0x0123456789abcdef),
			UINT64_C(0xfedcba9876543210),
		},
		.session_id = UINT64_C(0x1122334455667788),
		.output_index = UINT32_C(7),
	};
	struct hermes_session_credentials actual = {0};
	struct stat status;
	int directory_created = 0;
	int non_drm_fd = -1;
	int result = EXIT_FAILURE;

	non_drm_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	REQUIRE(non_drm_fd >= 0, "could not open the non-DRM test device");
	errno = 0;
	REQUIRE(hermes_session_require_driver(non_drm_fd) < 0 && errno == ENOTTY,
		"driver guard accepted a non-DRM file or changed its ioctl error");
	errno = 0;
	REQUIRE(hermes_session_require_token_uapi(non_drm_fd) < 0 &&
		errno == ENOTTY,
		"token-UAPI guard accepted a non-DRM file or changed its ioctl error");
	actual = expected;
	errno = 0;
	REQUIRE(hermes_session_get_owner_token(non_drm_fd, &actual) < 0 &&
		errno == ENOTTY,
		"owner-token helper skipped the DRM driver guard");
	REQUIRE(bytes_are_zero(&actual, sizeof(actual)),
		"failed owner-token lookup retained stale credentials");
	REQUIRE(close(non_drm_fd) == 0, "could not close the non-DRM test device");
	non_drm_fd = -1;

	REQUIRE(mkdtemp(directory) != NULL, "mkdtemp failed");
	directory_created = 1;
	REQUIRE(make_path(session_path, sizeof(session_path), directory,
			  "session.auth") == 0,
		"session path is too long");
	REQUIRE(make_path(symlink_path, sizeof(symlink_path), directory,
			  "session.link") == 0,
		"symlink path is too long");
	REQUIRE(make_path(trailing_path, sizeof(trailing_path), directory,
			  "session.trailing") == 0,
		"trailing-data path is too long");
	REQUIRE(make_path(missing_path, sizeof(missing_path), directory,
			  "session.missing") == 0,
		"missing-file path is too long");
	errno = 0;
	REQUIRE(hermes_session_open_bound_render(missing_path, NULL, 0, NULL) < 0 &&
		errno == ENOENT,
		"auto-open did not preserve a credential read error");
	errno = 0;
	REQUIRE(hermes_session_open_bound_render(missing_path, device_path, 0,
						 NULL) < 0 && errno == EINVAL,
		"auto-open accepted a path buffer with zero size");
	actual = expected;
	errno = 0;
	REQUIRE(hermes_session_read_file(missing_path, &actual) < 0 &&
		errno == ENOENT,
		"missing credential read changed its error");
	REQUIRE(bytes_are_zero(&actual, sizeof(actual)),
		"failed credential read retained stale credentials");

	REQUIRE(hermes_session_write_file(session_path, &expected) == 0,
		"credential publication failed");
	REQUIRE(lstat(session_path, &status) == 0 && S_ISREG(status.st_mode),
		"published credential is not a regular file");
	REQUIRE((status.st_mode & 0777) == 0600,
		"published credential permissions are not 0600");
	REQUIRE(status.st_uid == geteuid(),
		"published credential has the wrong owner");
	REQUIRE(hermes_session_read_file(session_path, &actual) == 0,
		"credential read failed");
	REQUIRE(credentials_equal(&actual, &expected),
		"credential round-trip changed a field");

	hermes_session_forget(&actual);
	REQUIRE(bytes_are_zero(&actual, sizeof(actual)),
		"credential forget did not clear the complete object");
	memset(scratch, 0xa5, sizeof(scratch));
	hermes_session_memzero(scratch, sizeof(scratch));
	REQUIRE(bytes_are_zero(scratch, sizeof(scratch)),
		"generic secret-memory clearing failed");

	errno = 0;
	REQUIRE(hermes_session_write_file(session_path, &expected) < 0 &&
		errno == EEXIST,
		"credential publication replaced an existing file");
	REQUIRE(hermes_session_read_file(session_path, &actual) == 0 &&
		credentials_equal(&actual, &expected),
		"failed no-replace publication modified the original credential");
	hermes_session_forget(&actual);

	REQUIRE(chmod(session_path, 0640) == 0,
		"could not broaden credential permissions for the rejection test");
	errno = 0;
	REQUIRE(hermes_session_read_file(session_path, &actual) < 0 &&
		errno == EACCES,
		"credential with group permissions was accepted");
	REQUIRE(chmod(session_path, 0600) == 0,
		"could not restore credential permissions");

	REQUIRE(symlink(session_path, symlink_path) == 0,
		"could not create credential symlink");
	errno = 0;
	REQUIRE(hermes_session_read_file(symlink_path, &actual) < 0 &&
		errno == ELOOP,
		"credential symlink was accepted");

	REQUIRE(hermes_session_write_file(trailing_path, &expected) == 0,
		"could not publish trailing-data fixture");
	REQUIRE(append_trailing_garbage(trailing_path) == 0,
		"could not append trailing-data fixture");
	errno = 0;
	REQUIRE(hermes_session_read_file(trailing_path, &actual) < 0 &&
		errno == EINVAL,
		"credential with trailing garbage was accepted");

	hermes_session_forget(&expected);
	REQUIRE(bytes_are_zero(&expected, sizeof(expected)),
		"source credential was not logically erased");
	result = EXIT_SUCCESS;
	printf("session file helper: PASS\n");

out:
	hermes_session_forget(&actual);
	hermes_session_forget(&expected);
	hermes_session_memzero(scratch, sizeof(scratch));
	if (non_drm_fd >= 0)
		close(non_drm_fd);
	if (directory_created && symlink_path[0])
		unlink(symlink_path);
	if (directory_created && trailing_path[0])
		unlink(trailing_path);
	if (directory_created && session_path[0])
		unlink(session_path);
	if (directory_created)
		rmdir(directory);
	return result;
}
