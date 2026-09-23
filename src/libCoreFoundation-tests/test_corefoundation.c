/*
 * test_corefoundation.c — Phase libCoreFoundation smoke check.
 *
 * Exercises the bits of CoreFoundation launchctl will actually use:
 *   - CF runtime alive (CFRetain / CFRelease, legacy non-Swift path)
 *   - CFDictionary + CFString basics
 *   - CFPropertyList XML round-trip (the primary feature)
 *   - CFRunLoop timer and run timeout timing
 *
 * Prints COREFOUNDATION-OK on success, COREFOUNDATION-FAIL otherwise.
 * Exit 0 / 1 to match.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <CoreFoundation/CoreFoundation.h>

static int
fail(const char *msg)
{
	fprintf(stderr, "COREFOUNDATION-FAIL: %s\n", msg);
	return 1;
}

static double
monotonic(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double fired_at;

static void
timer_fired(CFRunLoopTimerRef timer, void *info)
{
	(void)timer;
	*(double *)info = monotonic();
}

int
main(void)
{
	/*
	 * 1. CFDictionary + CFString basics.
	 *    Construct a small dict with a string value; read it back.
	 */
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(
	    kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (d == NULL)
		return fail("CFDictionaryCreateMutable returned NULL");

	CFStringRef key = CFSTR("Label");
	CFStringRef val = CFSTR("com.example.test");
	CFDictionarySetValue(d, key, val);

	CFStringRef got = (CFStringRef)CFDictionaryGetValue(d, key);
	if (got == NULL)
		return fail("CFDictionaryGetValue returned NULL");
	if (CFStringCompare(got, val, 0) != kCFCompareEqualTo)
		return fail("CFDictionaryGetValue returned wrong value");

	/*
	 * 2. Plist XML round-trip — serialize the dict, parse it back,
	 *    confirm the parsed dict has the same key+value.
	 */
	CFErrorRef err = NULL;
	CFDataRef xml = CFPropertyListCreateData(
	    kCFAllocatorDefault, (CFPropertyListRef)d,
	    kCFPropertyListXMLFormat_v1_0, 0, &err);
	if (xml == NULL) {
		if (err != NULL) {
			CFStringRef desc = CFErrorCopyDescription(err);
			char buf[256];
			CFStringGetCString(desc, buf, sizeof(buf),
			    kCFStringEncodingUTF8);
			fprintf(stderr, "CFPropertyListCreateData error: %s\n", buf);
			CFRelease(desc);
			CFRelease(err);
		}
		return fail("CFPropertyListCreateData (XML) returned NULL");
	}
	if (CFDataGetLength(xml) <= 0)
		return fail("XML plist data is empty");

	CFPropertyListRef parsed = CFPropertyListCreateWithData(
	    kCFAllocatorDefault, xml,
	    kCFPropertyListMutableContainersAndLeaves, NULL, &err);
	if (parsed == NULL)
		return fail("CFPropertyListCreateWithData (XML) returned NULL");
	if (CFGetTypeID(parsed) != CFDictionaryGetTypeID())
		return fail("Parsed XML plist isn't a dictionary");

	CFStringRef parsed_val = (CFStringRef)CFDictionaryGetValue(
	    (CFDictionaryRef)parsed, key);
	if (parsed_val == NULL)
		return fail("XML round-trip lost the Label key");
	if (CFStringCompare(parsed_val, val, 0) != kCFCompareEqualTo)
		return fail("XML round-trip mutated the Label value");

	CFRelease(parsed);
	CFRelease(xml);

	/*
	 * 3. Plist binary round-trip — same dict, different format.
	 */
	err = NULL;
	CFDataRef bin = CFPropertyListCreateData(
	    kCFAllocatorDefault, (CFPropertyListRef)d,
	    kCFPropertyListBinaryFormat_v1_0, 0, &err);
	if (bin == NULL)
		return fail("CFPropertyListCreateData (binary) returned NULL");
	if (CFDataGetLength(bin) <= 0)
		return fail("Binary plist data is empty");

	CFPropertyListRef bparsed = CFPropertyListCreateWithData(
	    kCFAllocatorDefault, bin,
	    kCFPropertyListMutableContainersAndLeaves, NULL, &err);
	if (bparsed == NULL)
		return fail("CFPropertyListCreateWithData (binary) returned NULL");
	if (CFGetTypeID(bparsed) != CFDictionaryGetTypeID())
		return fail("Parsed binary plist isn't a dictionary");

	CFStringRef bparsed_val = (CFStringRef)CFDictionaryGetValue(
	    (CFDictionaryRef)bparsed, key);
	if (bparsed_val == NULL)
		return fail("Binary round-trip lost the Label key");
	if (CFStringCompare(bparsed_val, val, 0) != kCFCompareEqualTo)
		return fail("Binary round-trip mutated the Label value");

	CFRelease(bparsed);
	CFRelease(bin);
	CFRelease(d);

	/*
	 * 3. Run loop timing. A 50 ms timer must fire and a 0.2 s
	 *    CFRunLoopRunInMode must time out, each within a second.
	 *    With the TSR rate derived from clock_getres a 24 MHz arm64
	 *    timer stretched both 42-fold, and the run timeout depends on
	 *    a second dispatch timer, which libmach once never delivered.
	 */
	double t0 = monotonic();
	CFRunLoopTimerContext tctx = { 0, &fired_at, NULL, NULL, NULL };
	CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault,
	    CFAbsoluteTimeGetCurrent() + 0.05, 0, 0, 0, timer_fired, &tctx);
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
	CFRunLoopTimerInvalidate(timer);
	CFRelease(timer);
	if (fired_at == 0.0 || fired_at - t0 > 1.0)
		return fail("50 ms CFRunLoopTimer did not fire within 1 s");
	for (int i = 0; i < 2; i++) {
		double r0 = monotonic();
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, false);
		if (monotonic() - r0 > 1.0)
			return fail("0.2 s CFRunLoopRunInMode took over 1 s to time out");
	}

	printf("COREFOUNDATION-OK: CFDictionary + XML/binary plist round-trip and run loop timing succeeded\n");
	return 0;
}
