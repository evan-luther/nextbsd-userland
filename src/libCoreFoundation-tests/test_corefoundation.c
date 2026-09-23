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
#include <CoreFoundation/CFRuntime.h>
#include <CoreFoundation/ForSwiftFoundationOnly.h>

#include <stdint.h>

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

/* ---- foreign-runtime bridge test ------------------------------------
 * A fake foreign runtime: fake class pointers (any distinct static
 * addresses), a fake foreign object whose isa differs from every
 * registered class, and __CFSwiftBridge hooks that count calls.
 */

static int fake_default_class;
static int fake_string_class;
static int fake_array_class;

static int hook_cfTypeID, hook_hash, hook_isEqual, hook_retain,
    hook_release, hook_retainCount, hook_autorelease,
    hook_copyDescription, hook_deallocating;
static int hook_array_count, hook_array_objectAtIndex, hook_string_length;

static CFTypeID fake_cfTypeID(CFTypeRef o) { hook_cfTypeID++; return CFStringGetTypeID(); }
static CFHashCode fake_hash(CFTypeRef o) { hook_hash++; return 4242; }
static bool fake_isEqual(CFTypeRef o, CFTypeRef other) { hook_isEqual++; return o == other; }
static CFTypeRef fake_copyWithZone(CFTypeRef o, CFTypeRef z) { return o; }
static CFTypeRef fake_retain(CFTypeRef o) { hook_retain++; return o; }
static void fake_release(CFTypeRef o) { hook_release++; }
static CFIndex fake_retainCount(CFTypeRef o) { hook_retainCount++; return 7; }
static CFTypeRef fake_autorelease(CFTypeRef o) { hook_autorelease++; return o; }
static CFStringRef fake_copyDescription(CFTypeRef o) {
	hook_copyDescription++;
	return CFStringCreateWithCString(NULL, "fake-foreign", kCFStringEncodingUTF8);
}
static void fake_deallocating(CFTypeRef o) { hook_deallocating++; }
static CFIndex fake_array_count(CFTypeRef o) { hook_array_count++; return 3; }
static CFTypeRef fake_array_objectAtIndex(CFTypeRef o, CFIndex i) { hook_array_objectAtIndex++; return NULL; }
static CFIndex fake_string_length(CFTypeRef o) { hook_string_length++; return 11; }

/* A fake foreign object: CFRuntimeBase-shaped header, isa pointing at a
 * class CF never registered. */
static struct {
	uintptr_t isa;
	uint64_t cfinfoa;
} fake_foreign_obj = { 0, 0 };
static int fake_foreign_class;

static int
test_foreign_bridge(void)
{
	fake_foreign_obj.isa = (uintptr_t)&fake_foreign_class;
	/* cfinfoa: type ID = CFString, uses-default-allocator bit, rc = 1 */
	fake_foreign_obj.cfinfoa =
	    (1ULL << 32) | (CFStringGetTypeID() << 8) | 0x80;

	/* (a) Nothing registered yet: CF objects work, no hooks fire. */
	CFStringRef s0 = CFStringCreateWithCString(NULL, "plain", kCFStringEncodingUTF8);
	if (!s0) return fail("CFStringCreateWithCString failed");
	if (CFStringGetLength(s0) != 5) return fail("native CFStringGetLength wrong");
	CFRetain(s0);
	CFRelease(s0);
	CFRelease(s0);
	if (hook_retain || hook_release || hook_string_length)
		return fail("bridge hooks fired before registration");

	/* Register the fake runtime: default class + per-type classes. */
	_CFRuntimeBridgeSetDefaultClass(&fake_default_class);
	_CFRuntimeBridgeTypeToClass(CFStringGetTypeID(), &fake_string_class);
	_CFRuntimeBridgeTypeToClass(CFArrayGetTypeID(), &fake_array_class);

	__CFSwiftBridge.NSObject._cfTypeID = fake_cfTypeID;
	__CFSwiftBridge.NSObject.hash = fake_hash;
	__CFSwiftBridge.NSObject.isEqual = fake_isEqual;
	__CFSwiftBridge.NSObject.copyWithZone = fake_copyWithZone;
	__CFSwiftBridge.NSObject.retain = fake_retain;
	__CFSwiftBridge.NSObject.release = fake_release;
	__CFSwiftBridge.NSObject.retainCount = fake_retainCount;
	__CFSwiftBridge.NSObject.autorelease = fake_autorelease;
	__CFSwiftBridge.NSObject.copyDescription = fake_copyDescription;
	__CFSwiftBridge.NSObject.deallocating = fake_deallocating;
	__CFSwiftBridge.NSArray.count = fake_array_count;
	__CFSwiftBridge.NSArray.objectAtIndex = fake_array_objectAtIndex;
	__CFSwiftBridge.NSString.length = fake_string_length;

	/* (b) New CF objects carry the registered isa; statics re-stamped. */
	CFStringRef s1 = CFStringCreateWithCString(NULL, "bridged", kCFStringEncodingUTF8);
	CFArrayRef a1 = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
	if (!s1 || !a1) return fail("post-registration create failed");
	if (((CFRuntimeBase *)s1)->_cfisa != (uintptr_t)&fake_string_class)
		return fail("new CFString lacks registered isa");
	if (((CFRuntimeBase *)a1)->_cfisa != (uintptr_t)&fake_array_class)
		return fail("new CFArray lacks registered isa");
	if (((CFRuntimeBase *)kCFBooleanTrue)->_cfisa != (uintptr_t)&fake_default_class)
		return fail("kCFBooleanTrue isa not re-stamped to default class");

	/* (c) Foreign object reaches the hooks. */
	CFTypeRef foreign = (CFTypeRef)&fake_foreign_obj;
	if (CFArrayGetCount((CFArrayRef)foreign) != 3 || hook_array_count != 1)
		return fail("CFArrayGetCount did not reach NSArray.count hook");
	if (CFStringGetLength((CFStringRef)foreign) != 11 || hook_string_length != 1)
		return fail("CFStringGetLength did not reach NSString.length hook");
	if (CFGetTypeID(foreign) != CFStringGetTypeID() || hook_cfTypeID != 1)
		return fail("CFGetTypeID did not reach _cfTypeID hook");
	if (CFEqual(foreign, foreign) != true) /* identical pointer short-circuits */
		return fail("CFEqual identity failed");
	{
		int other_isa;
		struct { uintptr_t isa; uint64_t cfinfoa; } other = {
		    (uintptr_t)&other_isa, fake_foreign_obj.cfinfoa };
		if (CFEqual(foreign, (CFTypeRef)&other) != false || hook_isEqual != 1)
			return fail("CFEqual did not reach isEqual hook");
	}
	if (CFHash(foreign) != 4242 || hook_hash != 1)
		return fail("CFHash did not reach hash hook");
	if (CFRetain(foreign) != foreign || hook_retain != 1)
		return fail("CFRetain did not reach retain hook");
	CFRelease(foreign);
	if (hook_release != 1)
		return fail("CFRelease did not reach release hook");
	if (CFGetRetainCount(foreign) != 7 || hook_retainCount != 1)
		return fail("CFGetRetainCount did not reach retainCount hook");
	{
		CFStringRef desc = CFCopyDescription(foreign);
		if (hook_copyDescription != 1)
			return fail("CFCopyDescription did not reach copyDescription hook");
		char buf[64];
		if (!desc || !CFStringGetCString(desc, buf, sizeof buf, kCFStringEncodingUTF8)
		    || strcmp(buf, "fake-foreign") != 0)
			return fail("copyDescription hook returned wrong string");
		CFRelease(desc);
		/* %@ formatting must reach the same hook */
		CFStringRef fmt = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@"), foreign);
		if (hook_copyDescription != 2)
			return fail("%%@ formatting did not reach copyDescription hook");
		if (!fmt || !CFStringGetCString(fmt, buf, sizeof buf, kCFStringEncodingUTF8)
		    || strcmp(buf, "fake-foreign") != 0)
			return fail("%%@ formatting produced wrong string");
		CFRelease(fmt);
	}

	/* (d) kCFTypeArrayCallBacks retains/releases foreign elements via hooks. */
	{
		CFArrayRef holder = CFArrayCreate(NULL, (const void **)&foreign, 1,
		    &kCFTypeArrayCallBacks);
		if (!holder) return fail("CFArrayCreate failed");
		if (hook_retain != 2)
			return fail("array did not retain foreign element via hook");
		CFRelease(holder);
		if (hook_release != 2)
			return fail("array did not release foreign element via hook");
	}

	/* (e) A tagged pointer is foreign without dereference. */
	{
		CFTypeRef tagged = (CFTypeRef)(uintptr_t)0xdead0001;
		if (CFGetTypeID(tagged) != CFStringGetTypeID() || hook_cfTypeID != 2)
			return fail("tagged pointer did not reach _cfTypeID hook");
		if (CFRetain(tagged) != tagged || hook_retain != 3)
			return fail("tagged pointer did not reach retain hook");
		CFRelease(tagged);
		if (hook_release != 3)
			return fail("tagged pointer did not reach release hook");
	}

	/* (f) deallocating fires once for a bridged native object's last release. */
	{
		int before = hook_deallocating;
		CFTypeRef native = _CFRuntimeCreateInstance(NULL, CFStringGetTypeID(), 0, NULL);
		if (!native) return fail("_CFRuntimeCreateInstance failed");
		if (((CFRuntimeBase *)native)->_cfisa != (uintptr_t)&fake_string_class)
			return fail("native instance lacks bridged isa");
		CFRelease(native);
		if (hook_deallocating != before + 1)
			return fail("deallocating hook did not fire exactly once");
	}

	/* (g) A CFSTR literal stays native. */
	{
		CFStringRef lit = CFSTR("literal");
		int r = hook_retain, x = hook_release, l = hook_string_length;
		CFRetain(lit);
		CFRelease(lit);
		if (CFStringGetLength(lit) != 7)
			return fail("CFSTR length wrong");
		if (hook_retain != r || hook_release != x || hook_string_length != l)
			return fail("CFSTR literal took the foreign path");
	}

	CFRelease(s1);
	CFRelease(a1);
	return 0;
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
	/*
	 * 4. Foreign-runtime bridge (CF_BRIDGE_FOREIGN_RUNTIME).
	 *    Register a fake foreign runtime from C: fake class pointers,
	 *    a fake foreign object, and __CFSwiftBridge hooks implemented
	 *    here. Without the bridge compiled in this test does not link
	 *    (_CFRuntimeBridgeSetDefaultClass, __CFSwiftBridge) — that is
	 *    the failure mode on an unpatched CF.
	 */
	if (test_foreign_bridge() != 0)
		return 1;

	printf("COREFOUNDATION-OK: CFDictionary + XML/binary plist round-trip and run loop timing succeeded\n");
	return 0;
}
