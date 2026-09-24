/*	CFFileDescriptor.c
	Copyright (c) 2006-2019, Apple Inc. and the Swift project authors

	Portions Copyright (c) 2014-2019, Apple Inc. and the Swift project authors
	Licensed under Apache License v2.0 with Runtime Library Exception
	See http://swift.org/LICENSE.txt for license information
	See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
*/

#include "CFBase.h"

#if TARGET_OS_BSD || TARGET_OS_LINUX

#include "CFFileDescriptor.h"
#include "CFArray.h"
#include "CFString.h"
#include "CFInternal.h"
#include "CFRuntime_Internal.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#if TARGET_OS_BSD
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#elif TARGET_OS_LINUX
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif

/* Each CFFileDescriptor owns a kernel queue holding only its watched
 * descriptor with the enabled filters, disabled after each delivery.
 * The kernel queue is itself readable when an event is pending, so it
 * is the version-1 run-loop source's port: on BSD CF ports are packed
 * pipes (__CFPORT_PACK(rfd, wfd), see CFRunLoop.c) and the run loop
 * watches rfd for read readiness — the kqueue fd serves as rfd and the
 * packed wfd is never used for a v1 source; on Linux the port is the fd
 * itself (epoll inside epoll). The source's perform drains the kernel
 * queue without blocking, disables the callback types that fired, and
 * calls the callout.
 *
 * A callback reports the descriptor's state when the perform runs, not
 * when the event was queued: a client may re-enable before consuming
 * the data (to service the descriptor from a nested run loop), and the
 * event queued then must not fire once the data is gone. epoll
 * re-polls each item when harvesting; kqueue re-checks the filter only
 * for events that are not EV_ONESHOT, so BSD uses EV_DISPATCH. */

struct __CFFileDescriptor {
    CFRuntimeBase _base;
    CFLock_t _lock;
    CFFileDescriptorNativeDescriptor _fd;	/* immutable */
    int _kqfd;				/* kernel queue; -1 once invalidated */
    CFOptionFlags _enabled;		/* armed callback types */
    Boolean _closeOnInvalidate;		/* immutable */
    Boolean _closed;			/* _fd already closed */
#if TARGET_OS_LINUX
    int _readyfd;			/* eventfd standing in for fds epoll rejects; -1 until needed */
#endif
    CFFileDescriptorCallBack _callout;	/* immutable */
    CFFileDescriptorContext _context;	/* immutable */
    CFMutableArrayRef _rlSources;
};

/* Bit 4 in the base reserved bits is used for invalid state (mutable),
 * as in CFSocket. */

CF_INLINE Boolean __CFFileDescriptorIsValid(CFFileDescriptorRef f) {
    return __CFRuntimeGetFlag(f, 4);
}

CF_INLINE void __CFFileDescriptorSetValid(CFFileDescriptorRef f) {
    __CFRuntimeSetFlag(f, 4, true);
}

CF_INLINE void __CFFileDescriptorUnsetValid(CFFileDescriptorRef f) {
    __CFRuntimeSetFlag(f, 4, false);
}

CF_INLINE void __CFFileDescriptorLock(CFFileDescriptorRef f) {
    __CFLock(&(f->_lock));
}

CF_INLINE void __CFFileDescriptorUnlock(CFFileDescriptorRef f) {
    __CFUnlock(&(f->_lock));
}

/* f locked. Re-arm the kernel queue to match _enabled. */
static void __CFFileDescriptorUpdateKernelQueue(CFFileDescriptorRef f) {
    if (f->_kqfd < 0) return;
#if TARGET_OS_BSD
    struct kevent changes[2], receipts[2];
    int n = 0;
    if (f->_enabled & kCFFileDescriptorReadCallBack) {
	EV_SET(&changes[n++], f->_fd, EVFILT_READ, EV_ADD | EV_DISPATCH | EV_RECEIPT, 0, 0, NULL);
    } else {
	EV_SET(&changes[n++], f->_fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    }
    if (f->_enabled & kCFFileDescriptorWriteCallBack) {
	EV_SET(&changes[n++], f->_fd, EVFILT_WRITE, EV_ADD | EV_DISPATCH | EV_RECEIPT, 0, 0, NULL);
    } else {
	EV_SET(&changes[n++], f->_fd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    }
    /* EV_RECEIPT: a NULL eventlist would abort the changelist on the
     * first failing change (EV_DELETE of a filter never registered). */
    kevent(f->_kqfd, changes, n, receipts, n, NULL);
#elif TARGET_OS_LINUX
    uint32_t events = 0;
    if (f->_enabled & kCFFileDescriptorReadCallBack) events |= EPOLLIN | EPOLLPRI;
    if (f->_enabled & kCFFileDescriptorWriteCallBack) events |= EPOLLOUT;
    if (events) {
	struct epoll_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.events = events | EPOLLONESHOT;
	ev.data.fd = f->_fd;
	/* EPOLLONESHOT disarms after each delivery but keeps the
	 * registration, so MOD re-arms; ADD covers the first enable. */
	if (epoll_ctl(f->_kqfd, EPOLL_CTL_MOD, f->_fd, &ev) < 0 &&
	    epoll_ctl(f->_kqfd, EPOLL_CTL_ADD, f->_fd, &ev) < 0 &&
	    errno == EPERM) {
	    /* epoll rejects fds that do not support polling (regular
	     * files, some device nodes). kqueue reports those
	     * always-ready, so stand in with an eventfd that is
	     * signalled once per enable — the one-shot semantics are
	     * unchanged. */
	    if (f->_readyfd < 0) {
		f->_readyfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (f->_readyfd >= 0) {
		    memset(&ev, 0, sizeof(ev));
		    ev.events = EPOLLIN;
		    ev.data.fd = f->_readyfd;
		    if (epoll_ctl(f->_kqfd, EPOLL_CTL_ADD, f->_readyfd, &ev) < 0) {
			close(f->_readyfd);
			f->_readyfd = -1;
		    }
		}
	    }
	    if (f->_readyfd >= 0) {
		uint64_t one = 1;
		write(f->_readyfd, &one, sizeof(one));
	    }
	}
    } else {
	epoll_ctl(f->_kqfd, EPOLL_CTL_DEL, f->_fd, NULL);
    }
#endif
}

static __CFPort __CFFileDescriptorGetPort(void *info) {
    CFFileDescriptorRef f = (CFFileDescriptorRef)info;
    __CFPort port;
    __CFFileDescriptorLock(f);
#if TARGET_OS_BSD
    /* Same packing as CFRunLoop.c's __CFPORT_PACK(rfd, wfd); the kqueue
     * fd is the read end and the write half is unused for v1 sources. */
    port = (((uint64_t)(uint32_t)f->_kqfd) << 32) | (uint32_t)f->_kqfd;
#else
    port = f->_kqfd;
#endif
    __CFFileDescriptorUnlock(f);
    return port;
}

static void __CFFileDescriptorPerform(void *info) {
    CFFileDescriptorRef f = (CFFileDescriptorRef)info;
    CFOptionFlags fired = 0;
    int kqfd;

    __CFFileDescriptorLock(f);
    kqfd = f->_kqfd;
    __CFFileDescriptorUnlock(f);
    if (kqfd < 0) return;

#if TARGET_OS_BSD
    struct kevent events[8];
    int n;
    do {
	n = kevent(kqfd, NULL, 0, events, 8, &(struct timespec){0, 0});
    } while (n < 0 && errno == EINTR);
    for (int i = 0; i < n; i++) {
	if (events[i].filter == EVFILT_READ) fired |= kCFFileDescriptorReadCallBack;
	if (events[i].filter == EVFILT_WRITE) fired |= kCFFileDescriptorWriteCallBack;
    }
#elif TARGET_OS_LINUX
    struct epoll_event events[8];
    int n;
    do {
	n = epoll_wait(kqfd, events, 8, 0);
    } while (n < 0 && errno == EINTR);
    for (int i = 0; i < n; i++) {
	if (events[i].data.fd == f->_readyfd) {
	    /* Always-ready stand-in: report every type; _enabled
	     * masks it down to what was armed. */
	    uint64_t ignored;
	    read(f->_readyfd, &ignored, sizeof(ignored));
	    fired |= kCFFileDescriptorReadCallBack | kCFFileDescriptorWriteCallBack;
	    continue;
	}
	if (events[i].events & (EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP)) fired |= kCFFileDescriptorReadCallBack;
	if (events[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) fired |= kCFFileDescriptorWriteCallBack;
    }
#endif

    __CFFileDescriptorLock(f);
    fired &= f->_enabled;
    f->_enabled &= ~fired;
    __CFFileDescriptorUpdateKernelQueue(f);
    Boolean valid = __CFFileDescriptorIsValid(f);
    __CFFileDescriptorUnlock(f);

    if (valid && fired) {
	f->_callout(f, fired, f->_context.info);
    }
}

static const void *__CFFileDescriptorRetain(const void *info) {
    return CFRetain((CFFileDescriptorRef)info);
}

static void __CFFileDescriptorRelease(const void *info) {
    CFRelease((CFFileDescriptorRef)info);
}

static CFStringRef __CFFileDescriptorContextCopyDescription(const void *info) {
    return CFCopyDescription((CFFileDescriptorRef)info);
}

static CFStringRef __CFFileDescriptorCopyDescription(CFTypeRef cf) {
    CFFileDescriptorRef f = (CFFileDescriptorRef)cf;
    CFMutableStringRef result;
    CFStringRef contextDesc = NULL;
    void *contextInfo = NULL;
    CFStringRef (*contextCopyDescription)(void *info) = NULL;
    result = CFStringCreateMutable(CFGetAllocator(f), 0);
    __CFFileDescriptorLock(f);
    CFStringAppendFormat(result, NULL, CFSTR("<CFFileDescriptor %p [%p]>{valid = %s, descriptor = %d, enabled = 0x%x, callout = %p, context = "), cf, CFGetAllocator(f), (__CFFileDescriptorIsValid(f) ? "Yes" : "No"), f->_fd, (unsigned)f->_enabled, f->_callout);
    contextInfo = f->_context.info;
    contextCopyDescription = f->_context.copyDescription;
    __CFFileDescriptorUnlock(f);
    if (NULL != contextInfo && NULL != contextCopyDescription) {
	contextDesc = (CFStringRef)contextCopyDescription(contextInfo);
    }
    if (NULL == contextDesc) {
	contextDesc = CFStringCreateWithFormat(CFGetAllocator(f), NULL, CFSTR("<CFFileDescriptor context %p>"), contextInfo);
    }
    CFStringAppend(result, contextDesc);
    CFStringAppend(result, CFSTR("}"));
    CFRelease(contextDesc);
    return result;
}

static void __CFFileDescriptorDeallocate(CFTypeRef cf) {
    CFFileDescriptorRef f = (CFFileDescriptorRef)cf;
    if (f->_kqfd >= 0) close(f->_kqfd);
#if TARGET_OS_LINUX
    if (f->_readyfd >= 0) close(f->_readyfd);
#endif
    if (f->_closeOnInvalidate && !f->_closed) close(f->_fd);
    if (f->_rlSources) CFRelease(f->_rlSources);
    if (f->_context.info && f->_context.release) f->_context.release(f->_context.info);
}

const CFRuntimeClass __CFFileDescriptorClass = {
    0,
    "CFFileDescriptor",
    NULL,      // init
    NULL,      // copy
    __CFFileDescriptorDeallocate,
    NULL,      // equal
    NULL,      // hash
    NULL,      //
    __CFFileDescriptorCopyDescription
};

CFTypeID CFFileDescriptorGetTypeID(void) {
    return _kCFRuntimeIDCFFileDescriptor;
}

CFFileDescriptorRef CFFileDescriptorCreate(CFAllocatorRef allocator, CFFileDescriptorNativeDescriptor fd, Boolean closeOnInvalidate, CFFileDescriptorCallBack callout, const CFFileDescriptorContext *context) {
    CHECK_FOR_FORK();
    if (fd < 0 || NULL == callout) return NULL;
    int kqfd;
#if TARGET_OS_BSD
    kqfd = kqueue();
    if (kqfd < 0) return NULL;
#elif TARGET_OS_LINUX
    kqfd = epoll_create1(EPOLL_CLOEXEC);
    if (kqfd < 0) return NULL;
#endif
    CFFileDescriptorRef memory = (CFFileDescriptorRef)_CFRuntimeCreateInstance(allocator, CFFileDescriptorGetTypeID(), sizeof(struct __CFFileDescriptor) - sizeof(CFRuntimeBase), NULL);
    if (NULL == memory) {
	close(kqfd);
	return NULL;
    }
    __CFFileDescriptorSetValid(memory);
    memory->_lock = CFLockInit;
    memory->_fd = fd;
    memory->_kqfd = kqfd;
    memory->_enabled = 0;
    memory->_closeOnInvalidate = closeOnInvalidate;
    memory->_closed = false;
#if TARGET_OS_LINUX
    memory->_readyfd = -1;
#endif
    memory->_callout = callout;
    memset(&memory->_context, 0, sizeof(memory->_context));
    if (NULL != context) {
	memory->_context.retain = context->retain;
	memory->_context.release = context->release;
	memory->_context.copyDescription = context->copyDescription;
	memory->_context.info = context->retain ? context->retain(context->info) : context->info;
    }
    memory->_rlSources = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    return memory;
}

CFFileDescriptorNativeDescriptor CFFileDescriptorGetNativeDescriptor(CFFileDescriptorRef f) {
    CF_ASSERT_TYPE(CFFileDescriptorGetTypeID(), f);
    return f->_fd;
}

void CFFileDescriptorGetContext(CFFileDescriptorRef f, CFFileDescriptorContext *context) {
    CF_ASSERT_TYPE(CFFileDescriptorGetTypeID(), f);
    CFAssert1(0 == context->version, __kCFLogAssertion, "%s(): context version not initialized to 0", __PRETTY_FUNCTION__);
    memcpy(context, &f->_context, sizeof(CFFileDescriptorContext));
}

void CFFileDescriptorEnableCallBacks(CFFileDescriptorRef f, CFOptionFlags callBackTypes) {
    CF_ASSERT_TYPE(CFFileDescriptorGetTypeID(), f);
    CHECK_FOR_FORK();
    __CFFileDescriptorLock(f);
    if (__CFFileDescriptorIsValid(f)) {
	f->_enabled |= callBackTypes & (kCFFileDescriptorReadCallBack | kCFFileDescriptorWriteCallBack);
	__CFFileDescriptorUpdateKernelQueue(f);
    }
    __CFFileDescriptorUnlock(f);
}

void CFFileDescriptorDisableCallBacks(CFFileDescriptorRef f, CFOptionFlags callBackTypes) {
    CF_ASSERT_TYPE(CFFileDescriptorGetTypeID(), f);
    CHECK_FOR_FORK();
    __CFFileDescriptorLock(f);
    if (__CFFileDescriptorIsValid(f)) {
	f->_enabled &= ~(callBackTypes & (kCFFileDescriptorReadCallBack | kCFFileDescriptorWriteCallBack));
	__CFFileDescriptorUpdateKernelQueue(f);
    }
    __CFFileDescriptorUnlock(f);
}

void CFFileDescriptorInvalidate(CFFileDescriptorRef f) {
    CF_ASSERT_TYPE(CFFileDescriptorGetTypeID(), f);
    CFRetain(f);
    __CFFileDescriptorLock(f);
    if (__CFFileDescriptorIsValid(f)) {
	/* The sources retain f and the array retains the sources; drop
	 * the array here so the cycle ends with invalidation. */
	CFArrayRef sources = (CFArrayRef)f->_rlSources;
	f->_rlSources = NULL;
	void *contextInfo = f->_context.info;
	void (*contextRelease)(void *info) = f->_context.release;
	__CFFileDescriptorUnsetValid(f);
	__CFFileDescriptorUnlock(f);
	/* Invalidate the sources first: that removes the kernel queue fd
	 * from every run loop's port set before the queue is closed. */
	if (sources) {
	    for (CFIndex idx = 0, cnt = CFArrayGetCount(sources); idx < cnt; idx++) {
		CFRunLoopSourceInvalidate((CFRunLoopSourceRef)CFArrayGetValueAtIndex(sources, idx));
	    }
	    CFRelease(sources);
	}
	__CFFileDescriptorLock(f);
	if (f->_kqfd >= 0) {
	    close(f->_kqfd);
	    f->_kqfd = -1;
	}
#if TARGET_OS_LINUX
	if (f->_readyfd >= 0) {
	    close(f->_readyfd);
	    f->_readyfd = -1;
	}
#endif
	if (f->_closeOnInvalidate && !f->_closed) {
	    close(f->_fd);
	    f->_closed = true;
	}
	f->_context.info = NULL;
	__CFFileDescriptorUnlock(f);
	if (NULL != contextInfo && NULL != contextRelease) {
	    contextRelease(contextInfo);
	}
    } else {
	__CFFileDescriptorUnlock(f);
    }
    CFRelease(f);
}

Boolean CFFileDescriptorIsValid(CFFileDescriptorRef f) {
    CF_ASSERT_TYPE(CFFileDescriptorGetTypeID(), f);
    __CFFileDescriptorLock(f);
    Boolean valid = __CFFileDescriptorIsValid(f);
    __CFFileDescriptorUnlock(f);
    return valid;
}

CFRunLoopSourceRef CFFileDescriptorCreateRunLoopSource(CFAllocatorRef allocator, CFFileDescriptorRef f, CFIndex order) {
    CF_ASSERT_TYPE(CFFileDescriptorGetTypeID(), f);
    CHECK_FOR_FORK();
    CFRunLoopSourceRef result = NULL;
    __CFFileDescriptorLock(f);
    if (__CFFileDescriptorIsValid(f)) {
	CFRunLoopSourceContext1 context;
	memset(&context, 0, sizeof(context));
	context.version = 1;
	context.info = f;
	context.retain = __CFFileDescriptorRetain;
	context.release = __CFFileDescriptorRelease;
	context.copyDescription = __CFFileDescriptorContextCopyDescription;
	context.getPort = __CFFileDescriptorGetPort;
	context.perform = __CFFileDescriptorPerform;
	result = CFRunLoopSourceCreate(allocator, order, (CFRunLoopSourceContext *)&context);
	if (result) CFArrayAppendValue(f->_rlSources, result);
    }
    __CFFileDescriptorUnlock(f);
    return result;
}

#endif /* TARGET_OS_BSD || TARGET_OS_LINUX */
