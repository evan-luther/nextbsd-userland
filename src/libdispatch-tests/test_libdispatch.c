/*
 * test_libdispatch — basic libdispatch smoke check.
 *
 * Confirms libsystem_dispatch.so (vendored swift-corelibs-libdispatch
 * built in our build.sh chroot pipeline) is loadable, links cleanly,
 * and a serial queue executes a synchronous callback. Uses the
 * function-pointer dispatch API (dispatch_sync_f) instead of the block
 * variant (dispatch_sync) so we don't pull a -fblocks / BlocksRuntime
 * link dep into this test binary.
 *
 * Exit codes:
 *   0 — queue created + sync callback ran + counter mutation visible
 *   1 — dispatch_queue_create returned NULL
 *   2 — sync callback didn't run (counter unchanged)
 *   3 — a later one-shot dispatch timer never fired
 */
#include <stdio.h>
#include <dispatch/dispatch.h>

static void
set_counter_to_42(void *ctx)
{
	int *counter = (int *)ctx;
	*counter = 42;
}

static void
signal_semaphore(void *ctx)
{
	dispatch_semaphore_signal((dispatch_semaphore_t)ctx);
}

int
main(void)
{
	dispatch_queue_t q = dispatch_queue_create("test_libdispatch.serial",
	    DISPATCH_QUEUE_SERIAL);
	if (q == NULL) {
		printf("FAIL: dispatch_queue_create returned NULL\n");
		return 1;
	}

	int counter = 0;
	dispatch_sync_f(q, &counter, set_counter_to_42);

	if (counter != 42) {
		printf("FAIL: counter expected 42 got %d\n", counter);
		return 2;
	}

	dispatch_release(q);

	/*
	 * Sequential one-shot timers: each must fire. Before libmach requested
	 * EV_RECEIPT for KEVENT_FLAG_ERROR_EVENTS calls, a registration call
	 * consumed the manager thread's EVFILT_USER wakeup, so only the first
	 * timer in a process ever fired.
	 */
	for (int i = 0; i < 3; i++) {
		dispatch_semaphore_t fired = dispatch_semaphore_create(0);
		dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,
		    0, 0, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
		dispatch_set_context(t, fired);
		dispatch_source_set_event_handler_f(t, signal_semaphore);
		dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW,
		    50 * NSEC_PER_MSEC), DISPATCH_TIME_FOREVER, NSEC_PER_MSEC);
		dispatch_resume(t);
		long timedout = dispatch_semaphore_wait(fired,
		    dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
		dispatch_source_cancel(t);
		dispatch_release(t);
		dispatch_release(fired);
		if (timedout) {
			printf("FAIL: dispatch timer %d did not fire within 5 s\n", i + 1);
			return 3;
		}
	}

	printf("LIBDISPATCH-OK: dispatch_sync_f executed on serial queue; 3 sequential timers fired\n");
	return 0;
}
