/*	CFFileDescriptor.h
	Copyright (c) 2006-2019, Apple Inc. and the Swift project authors

	Portions Copyright (c) 2014-2019, Apple Inc. and the Swift project authors
	Licensed under Apache License v2.0 with Runtime Library Exception
	See http://swift.org/LICENSE.txt for license information
	See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
*/

#if !defined(__COREFOUNDATION_CFFILEDESCRIPTOR__)
#define __COREFOUNDATION_CFFILEDESCRIPTOR__ 1

#include "CFBase.h"
#include "CFRunLoop.h"

CF_IMPLICIT_BRIDGING_ENABLED
CF_EXTERN_C_BEGIN

typedef int CFFileDescriptorNativeDescriptor;

typedef struct __CFFileDescriptor * CFFileDescriptorRef;

/*	Reasons for a callout to be called */
enum {
	kCFFileDescriptorReadCallBack = 1UL << 0,
	kCFFileDescriptorWriteCallBack = 1UL << 1
};

typedef void (*CFFileDescriptorCallBack)(CFFileDescriptorRef f, CFOptionFlags callBackTypes, void *info);

typedef struct {
    CFIndex	version;
    void *	info;
    void *	(*retain)(void *info);
    void	(*release)(void *info);
    CFStringRef	(*copyDescription)(void *info);
} CFFileDescriptorContext;

CF_EXPORT CFTypeID	CFFileDescriptorGetTypeID(void);

CF_EXPORT CFFileDescriptorRef	CFFileDescriptorCreate(CFAllocatorRef allocator, CFFileDescriptorNativeDescriptor fd, Boolean closeOnInvalidate, CFFileDescriptorCallBack callout, const CFFileDescriptorContext *context);

CF_EXPORT CFFileDescriptorNativeDescriptor	CFFileDescriptorGetNativeDescriptor(CFFileDescriptorRef f);

CF_EXPORT void	CFFileDescriptorGetContext(CFFileDescriptorRef f, CFFileDescriptorContext *context);

/* Callbacks are one-shot: once a callback type has fired it is disabled
 * and must be re-enabled to fire again. On Linux the read callback also
 * fires for EPOLLPRI, EPOLLERR and EPOLLHUP; on BSD the read callback
 * fires when the descriptor reaches EOF or hangs up. */
CF_EXPORT void	CFFileDescriptorEnableCallBacks(CFFileDescriptorRef f, CFOptionFlags callBackTypes);
CF_EXPORT void	CFFileDescriptorDisableCallBacks(CFFileDescriptorRef f, CFOptionFlags callBackTypes);

CF_EXPORT void	CFFileDescriptorInvalidate(CFFileDescriptorRef f);
CF_EXPORT Boolean	CFFileDescriptorIsValid(CFFileDescriptorRef f);

CF_EXPORT CFRunLoopSourceRef	CFFileDescriptorCreateRunLoopSource(CFAllocatorRef allocator, CFFileDescriptorRef f, CFIndex order);

CF_EXTERN_C_END
CF_IMPLICIT_BRIDGING_DISABLED

#endif /* ! __COREFOUNDATION_CFFILEDESCRIPTOR__ */
