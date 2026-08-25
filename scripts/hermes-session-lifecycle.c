// hermes-session-lifecycle: exercise the UAPI v13 session capability lifecycle.
//
// The driver promises that an output owner can cut a capture consumer off
// without tearing down its stream: ROTATE_TOKEN stops a leaked token from
// granting new binds while running consumers continue, and REVOKE_BINDINGS
// additionally drops every bound descriptor at once -- including waking a
// blocked WAIT_FRAME with EACCES -- while ownership, the session ID and the
// scanout survive. None of that is observable from a single ioctl, so it is
// checked here end to end from one process holding several descriptors.
//
/* Build: cc -O2 -pthread -I/usr/include/libdrm -o hermes-session-lifecycle
 *            hermes-session-lifecycle.c -ldrm
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <xf86drm.h>
#include <drm/hermes_kms_drm.h>

static int failures;

#define CHECK(condition, ...)                                                  \
	do {                                                                   \
		if (!(condition)) {                                            \
			failures++;                                            \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
		} else {                                                       \
			printf("ok: ");                                        \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

struct token {
	uint64_t value[2];
	uint64_t session_id;
	uint32_t output_index;
};

static int open_hermes(void)
{
	for (int i = 0; i < 16; i++) {
		char path[64];
		int fd;
		drmVersionPtr version;
		int is_hermes;

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		version = drmGetVersion(fd);
		is_hermes = version && !strcmp(version->name, "hermes-kms");
		if (version)
			drmFreeVersion(version);
		if (is_hermes)
			return fd;
		close(fd);
	}
	return -1;
}

static int session_op(int fd, uint32_t operation, struct token *out)
{
	struct drm_hermes_kms_session_access request;

	memset(&request, 0, sizeof(request));
	request.operation = operation;
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_SESSION_ACCESS, &request) < 0)
		return -1;
	if (out) {
		out->value[0] = request.token[0];
		out->value[1] = request.token[1];
		out->session_id = request.session_id;
		out->output_index = request.output_index;
	}
	if (operation == HERMES_KMS_SESSION_ACCESS_REVOKE_BINDINGS &&
	    !(request.result_flags &
	      HERMES_KMS_SESSION_ACCESS_RESULT_REVOKED)) {
		errno = EPROTO;
		return -1;
	}
	return 0;
}

static int session_bind(int fd, const struct token *token)
{
	struct drm_hermes_kms_session_access request;

	memset(&request, 0, sizeof(request));
	request.operation = HERMES_KMS_SESSION_ACCESS_BIND;
	request.token[0] = token->value[0];
	request.token[1] = token->value[1];
	request.session_id = token->session_id;
	request.output_index = token->output_index;
	return ioctl(fd, DRM_IOCTL_HERMES_KMS_SESSION_ACCESS, &request);
}

static int get_status(int fd, struct drm_hermes_kms_status *status)
{
	memset(status, 0, sizeof(*status));
	return ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_STATUS, status);
}

static uint64_t bound_fd_count(int owner_fd)
{
	struct drm_hermes_kms_status status;

	if (get_status(owner_fd, &status) < 0)
		return UINT64_MAX;
	return status.bound_fd_count;
}

static int get_metrics(int fd, struct drm_hermes_kms_metrics *metrics)
{
	memset(metrics, 0, sizeof(*metrics));
	return ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_METRICS, metrics);
}

struct waiter {
	int fd;
	int ret;
	int err;
	atomic_int started;
};

/* A bound descriptor blocked in WAIT_FRAME must be woken by a revocation. */
static void *wait_thread(void *argument)
{
	struct waiter *waiter = argument;
	struct drm_hermes_kms_wait_frame wait;

	memset(&wait, 0, sizeof(wait));
	wait.after_sequence = UINT64_MAX - 1;
	wait.timeout_ms = 10000;
	atomic_store(&waiter->started, 1);
	waiter->ret = ioctl(waiter->fd, DRM_IOCTL_HERMES_KMS_WAIT_FRAME, &wait);
	waiter->err = errno;
	return NULL;
}

int main(void)
{
	struct drm_hermes_kms_set_output output;
	struct drm_hermes_kms_metrics metrics;
	struct drm_hermes_kms_status status;
	struct token first, rotated, revoked;
	struct waiter waiter;
	pthread_t wait_worker;
	int owner, a, b, c;
	uint64_t session_id;

	owner = open_hermes();
	if (owner < 0) {
		fprintf(stderr, "no Hermes-KMS card found\n");
		return 2;
	}
	a = open_hermes();
	b = open_hermes();
	c = open_hermes();
	if (a < 0 || b < 0 || c < 0) {
		fprintf(stderr, "could not open extra descriptors\n");
		return 2;
	}

	memset(&output, 0, sizeof(output));
	output.enabled = 1;
	output.width = 1920;
	output.height = 1080;
	output.refresh_hz = 60;
	if (ioctl(owner, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &output) < 0) {
		perror("SET_OUTPUT");
		return 2;
	}
	session_id = output.session_id;
	CHECK(session_id != 0, "owner claimed a session");

	CHECK(session_op(owner, HERMES_KMS_SESSION_ACCESS_GET_TOKEN,
			 &first) == 0,
	      "owner retrieved its token");
	CHECK(bound_fd_count(owner) == 0, "no descriptor is bound yet");

	CHECK(session_bind(a, &first) == 0, "first consumer bound");
	CHECK(bound_fd_count(owner) == 1, "one descriptor is bound");
	CHECK(session_bind(b, &first) == 0, "second consumer bound");
	CHECK(bound_fd_count(owner) == 2, "two descriptors are bound");
	CHECK(get_status(a, &status) == 0, "a bound consumer can read status");

	/* Rotating leaves running consumers alone but retires the old token. */
	CHECK(session_op(owner, HERMES_KMS_SESSION_ACCESS_ROTATE_TOKEN,
			 &rotated) == 0,
	      "owner rotated the token");
	CHECK(rotated.session_id == session_id,
	      "rotation kept the session id");
	CHECK(memcmp(rotated.value, first.value, sizeof(first.value)) != 0,
	      "rotation produced a different token");
	CHECK(get_status(a, &status) == 0,
	      "a consumer bound before the rotation keeps access");
	CHECK(bound_fd_count(owner) == 2,
	      "rotation did not drop existing bindings");
	CHECK(session_bind(c, &first) < 0 && errno == EACCES,
	      "the retired token no longer binds");
	CHECK(session_bind(c, &rotated) == 0, "the new token binds");
	CHECK(bound_fd_count(owner) == 3, "three descriptors are bound");

	/* A blocked wait must be woken by the revocation, not left hanging. */
	memset(&waiter, 0, sizeof(waiter));
	waiter.fd = b;
	atomic_store(&waiter.started, 0);
	if (pthread_create(&wait_worker, NULL, wait_thread, &waiter) != 0) {
		fprintf(stderr, "could not start the wait thread\n");
		return 2;
	}
	while (!atomic_load(&waiter.started))
		;
	usleep(200000);

	CHECK(session_op(owner, HERMES_KMS_SESSION_ACCESS_REVOKE_BINDINGS,
			 &revoked) == 0,
	      "owner revoked every binding");
	pthread_join(wait_worker, NULL);
	CHECK(waiter.ret < 0 && waiter.err == EACCES,
	      "the blocked wait failed with EACCES instead of timing out");

	CHECK(revoked.session_id == session_id,
	      "revocation kept the session id");
	CHECK(memcmp(revoked.value, rotated.value, sizeof(rotated.value)) != 0,
	      "revocation also rotated the token");
	CHECK(bound_fd_count(owner) == 0, "no descriptor is bound after revoke");
	CHECK(get_status(a, &status) < 0 && errno == EACCES,
	      "a revoked consumer loses status access");
	CHECK(get_status(c, &status) < 0 && errno == EACCES,
	      "every revoked consumer loses access");
	CHECK(session_bind(a, &rotated) < 0 && errno == EACCES,
	      "the pre-revocation token cannot rebind");

	/* Ownership and the scanout survive a revocation. */
	CHECK(get_status(owner, &status) == 0, "the owner still has access");
	CHECK(status.session_id == session_id, "the session survived");
	CHECK(status.flags & HERMES_KMS_STATUS_OUTPUT_ENABLED,
	      "the output stayed enabled");

	CHECK(session_bind(a, &revoked) == 0, "the post-revocation token binds");
	CHECK(bound_fd_count(owner) == 1, "the rebound descriptor is counted");
	CHECK(session_op(a, HERMES_KMS_SESSION_ACCESS_UNBIND, NULL) == 0,
	      "a consumer can drop its own binding");
	CHECK(bound_fd_count(owner) == 0, "unbinding decremented the count");

	/* Closing a bound descriptor must release its binding too. */
	CHECK(session_bind(a, &revoked) == 0, "rebound for the close test");
	CHECK(bound_fd_count(owner) == 1, "bound again before closing");
	close(a);
	a = -1;
	CHECK(bound_fd_count(owner) == 0, "closing released the binding");

	CHECK(get_metrics(owner, &metrics) == 0, "metrics readable");
	printf("counters: bind=%llu reject=%llu unbind=%llu revoke=%llu\n",
	       (unsigned long long)metrics.bind_count,
	       (unsigned long long)metrics.bind_reject_count,
	       (unsigned long long)metrics.unbind_count,
	       (unsigned long long)metrics.binding_revoke_count);
	/*
	 * Five binds succeed above (a, b, c, then a twice after the
	 * revocation), two are refused (the retired token, then the
	 * pre-revocation one), one descriptor unbinds itself and the owner
	 * revokes once.
	 */
	CHECK(metrics.bind_count == 5, "bind_count is %llu, expected 5",
	      (unsigned long long)metrics.bind_count);
	CHECK(metrics.bind_reject_count == 2,
	      "bind_reject_count is %llu, expected 2",
	      (unsigned long long)metrics.bind_reject_count);
	CHECK(metrics.unbind_count == 1, "unbind_count is %llu, expected 1",
	      (unsigned long long)metrics.unbind_count);
	CHECK(metrics.binding_revoke_count == 1,
	      "binding_revoke_count is %llu, expected 1",
	      (unsigned long long)metrics.binding_revoke_count);

	if (a >= 0)
		close(a);
	close(b);
	close(c);
	memset(&output, 0, sizeof(output));
	if (ioctl(owner, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &output) < 0)
		perror("disable SET_OUTPUT");
	close(owner);

	printf("%s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
