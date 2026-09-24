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
#include <stdarg.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFRuntime.h>
#include <CoreFoundation/ForSwiftFoundationOnly.h>
#include <CoreFoundation/CFNumber_Private.h>

#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dispatch/dispatch.h>

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
/* Unregistered class addresses, one per type, so the fake _cfTypeID can
 * answer the right type ID for a foreign object (some functions validate
 * the type before dispatching). */
static int fake_date_isa, fake_error_isa, fake_url_isa, fake_locale_isa,
    fake_tz_isa, fake_cal_isa, fake_astr_isa, fake_timer_isa,
    fake_number_isa, fake_boolean_isa, fake_dict_isa, fake_cset_isa;

static int hook_cfTypeID, hook_hash, hook_isEqual, hook_retain,
    hook_release, hook_retainCount, hook_autorelease,
    hook_copyDescription, hook_deallocating;
static int hook_array_count, hook_array_objectAtIndex, hook_string_length;

static CFTypeID fake_cfTypeID(CFTypeRef o) {
	hook_cfTypeID++;
	if (((uintptr_t)o & 7) != 0) return CFStringGetTypeID(); /* tagged */
	uintptr_t isa = *(uintptr_t *)o;
	if (isa == (uintptr_t)&fake_date_isa) return CFDateGetTypeID();
	if (isa == (uintptr_t)&fake_error_isa) return CFErrorGetTypeID();
	if (isa == (uintptr_t)&fake_url_isa) return CFURLGetTypeID();
	if (isa == (uintptr_t)&fake_locale_isa) return CFLocaleGetTypeID();
	if (isa == (uintptr_t)&fake_tz_isa) return CFTimeZoneGetTypeID();
	if (isa == (uintptr_t)&fake_cal_isa) return CFCalendarGetTypeID();
	if (isa == (uintptr_t)&fake_astr_isa) return CFAttributedStringGetTypeID();
	if (isa == (uintptr_t)&fake_timer_isa) return CFRunLoopTimerGetTypeID();
	if (isa == (uintptr_t)&fake_number_isa) return CFNumberGetTypeID();
	if (isa == (uintptr_t)&fake_boolean_isa) return CFBooleanGetTypeID();
	if (isa == (uintptr_t)&fake_dict_isa) return CFDictionaryGetTypeID();
	if (isa == (uintptr_t)&fake_cset_isa) return CFCharacterSetGetTypeID();
	return CFStringGetTypeID();
}
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

/* Hooks for the 2b types: date, error, URL, locale, time zone, calendar,
 * attributed string, timer, plus the number/boolean/dictionary/charset
 * sites that were ObjC-only. Each fake counts its calls; hook_total sums
 * them so the native-object checks can assert silence in one line. */
static int hook_total, hook_who;

/* Shared return objects for hooks that hand back real CF objects. */
static CFURLRef fake_cfurl;	/* real CFURL, +0 */
static CFDictionaryRef fake_dict;	/* real CFDictionary, +0 */
static CFDataRef fake_data;	/* real CFData, +0 */
static CFLocaleRef fake_locale;	/* real CFLocale, +0 */
static CFTimeZoneRef fake_tz;	/* real CFTimeZone, +0 */
static CFDateRef fake_date;	/* real CFDate, +0 */
static CFCharacterSetRef fake_cset;	/* real CFCharacterSet, +0 */
static CFStringRef fake_str;	/* real CFString, +0 */

static CFTimeInterval fake_date_timeIntervalSinceReferenceDate(CFTypeRef o) { hook_total++; return 42.5; }
static CFTimeInterval fake_date_timeIntervalSinceDate(CFTypeRef o, CFTypeRef other) { hook_total++; return 1.5; }
static CFComparisonResult fake_date_compare(CFTypeRef o, CFTypeRef other) { hook_total++; return kCFCompareGreaterThan; }

static CFDictionaryRef fake_error_userInfo(CFTypeRef o) { hook_total++; return fake_dict; }
static CFStringRef fake_error_domain(CFTypeRef o) { hook_total++; return fake_str; }
static CFIndex fake_error_code(CFTypeRef o) { hook_total++; return 77; }
static CFStringRef fake_error_localizedDescription(CFTypeRef o) { hook_total++; return fake_str; }
static CFStringRef fake_error_localizedFailureReason(CFTypeRef o) { hook_total++; return fake_str; }
static CFStringRef fake_error_localizedRecoverySuggestion(CFTypeRef o) { hook_total++; return fake_str; }

static CFURLRef fake_url_cfurl(CFTypeRef o) { hook_total++; hook_who = 1; return fake_cfurl; }
static CFURLRef fake_url_absoluteURL(CFTypeRef o) { hook_total++; hook_who = 2; return fake_cfurl; }
static CFStringRef fake_url_relativeString(CFTypeRef o) { hook_total++; hook_who = 3; return fake_str; }
static CFURLRef fake_url_baseURL(CFTypeRef o) { hook_total++; hook_who = 4; return NULL; }
static CFStringRef fake_url_scheme(CFTypeRef o) { hook_total++; hook_who = 5; return fake_str; }
static CFStringRef fake_url_host(CFTypeRef o) { hook_total++; hook_who = 6; return fake_str; }
static CFNumberRef fake_url_port(CFTypeRef o) { hook_total++; hook_who = 7; return NULL; }
static CFStringRef fake_url_user(CFTypeRef o) { hook_total++; hook_who = 8; return fake_str; }
static CFStringRef fake_url_password(CFTypeRef o) { hook_total++; hook_who = 9; return fake_str; }
static CFStringRef fake_url_query(CFTypeRef o) { hook_total++; hook_who = 10; return fake_str; }
static CFStringRef fake_url_fragment(CFTypeRef o) { hook_total++; hook_who = 11; return fake_str; }
static Boolean fake_url_isFileReferenceURL(CFTypeRef o) { hook_total++; hook_who = 12; return true; }

static Boolean fake_locale_specialCase(CFTypeRef o) { hook_total++; return true; }
static void fake_locale_setSpecialCase(CFTypeRef o) { hook_total++; }
static CFDictionaryRef fake_locale_prefs(CFTypeRef o) { hook_total++; return fake_dict; }
static CFLocaleRef fake_locale_copy(CFTypeRef o) { hook_total++; return (CFLocaleRef)CFRetain(fake_locale); }
static CFStringRef fake_locale_identifier(CFTypeRef o) { hook_total++; return fake_str; }
static CFTypeRef fake_locale_objectForKey(CFTypeRef o, CFTypeRef key) { hook_total++; return fake_str; }
static CFStringRef fake_locale_displayName(CFTypeRef o, CFTypeRef key, CFTypeRef value) { hook_total++; return CFStringCreateWithCString(NULL, "fake-locale", kCFStringEncodingUTF8); }

static CFStringRef fake_tz_name(CFTypeRef o) { hook_total++; return fake_str; }
static CFDataRef fake_tz_data(CFTypeRef o) { hook_total++; return fake_data; }
static CFTimeInterval fake_tz_dstOffset(CFTypeRef o, CFAbsoluteTime at) { hook_total++; return 3600.0; }
static CFAbsoluteTime fake_tz_nextTransition(CFTypeRef o, CFAbsoluteTime at) { hook_total++; return 9999.0; }
static CFStringRef fake_tz_localizedName(CFTypeRef o, CFTimeZoneNameStyle style, CFTypeRef locale) { hook_total++; return CFStringCreateWithCString(NULL, "fake-tz", kCFStringEncodingUTF8); }
static CFTimeInterval fake_tz_secondsFromGMT(CFTypeRef o, CFAbsoluteTime at) { hook_total++; return -7200.0; }
static CFStringRef fake_tz_abbreviation(CFTypeRef o, CFAbsoluteTime at) { hook_total++; return fake_str; }
static Boolean fake_tz_isDST(CFTypeRef o, CFAbsoluteTime at) { hook_total++; return true; }

static CFStringRef fake_cal_identifier(CFTypeRef o) { hook_total++; return fake_str; }
static CFLocaleRef fake_cal_copyLocale(CFTypeRef o) { hook_total++; return (CFLocaleRef)CFRetain(fake_locale); }
static void fake_cal_setLocale(CFTypeRef o, CFTypeRef l) { hook_total++; }
static CFTimeZoneRef fake_cal_copyTimeZone(CFTypeRef o) { hook_total++; return (CFTimeZoneRef)CFRetain(fake_tz); }
static void fake_cal_setTimeZone(CFTypeRef o, CFTypeRef tz) { hook_total++; }
static CFIndex fake_cal_firstWeekday(CFTypeRef o) { hook_total++; return 2; }
static void fake_cal_setFirstWeekday(CFTypeRef o, CFIndex w) { hook_total++; }
static CFIndex fake_cal_minDays(CFTypeRef o) { hook_total++; return 3; }
static void fake_cal_setMinDays(CFTypeRef o, CFIndex m) { hook_total++; }
static CFDateRef fake_cal_copyGregorianStart(CFTypeRef o) { hook_total++; return (CFDateRef)CFRetain(fake_date); }
static void fake_cal_setGregorianStart(CFTypeRef o, CFTypeRef d) { hook_total++; }
static CFRange fake_cal_minRange(CFTypeRef o, CFCalendarUnit u) { hook_total++; return CFRangeMake(1, 2); }
static CFRange fake_cal_maxRange(CFTypeRef o, CFCalendarUnit u) { hook_total++; return CFRangeMake(3, 4); }
static Boolean fake_cal_compose(CFTypeRef o, CFAbsoluteTime *atp, const char *desc, va_list args) { hook_total++; *atp = 5.0; return true; }
static Boolean fake_cal_decompose(CFTypeRef o, CFAbsoluteTime at, const char *desc, va_list args) { hook_total++; return true; }
static Boolean fake_cal_addComponents(CFTypeRef o, CFAbsoluteTime *atp, CFOptionFlags opts, const char *desc, va_list args) { hook_total++; return true; }
static Boolean fake_cal_diffComponents(CFTypeRef o, CFAbsoluteTime s, CFAbsoluteTime r, CFOptionFlags opts, const char *desc, va_list args) { hook_total++; return true; }
static Boolean fake_cal_rangeOfUnit(CFTypeRef o, CFCalendarUnit u, CFAbsoluteTime *startp, CFTimeInterval *tip, CFAbsoluteTime at) { hook_total++; return true; }
static CFRange fake_cal_rangeOfUnitInUnit(CFTypeRef o, CFCalendarUnit s, CFCalendarUnit b, CFAbsoluteTime at) { hook_total++; return CFRangeMake(5, 6); }
static CFIndex fake_cal_ordinality(CFTypeRef o, CFCalendarUnit s, CFCalendarUnit b, CFAbsoluteTime at) { hook_total++; return 9; }

static CFStringRef fake_astr_string(CFTypeRef o) { hook_total++; return fake_str; }
static CFIndex fake_astr_length(CFTypeRef o) { hook_total++; return 4; }
static CFDictionaryRef fake_astr_attributesAtIndex(CFTypeRef o, CFIndex loc, CFRange *er) { hook_total++; if (er) *er = CFRangeMake(0, 4); return fake_dict; }
static CFTypeRef fake_astr_attributeAtIndex(CFTypeRef o, CFTypeRef name, CFIndex loc, CFRange *er) { hook_total++; if (er) *er = CFRangeMake(0, 4); return fake_str; }
static CFDictionaryRef fake_astr_attributesLongest(CFTypeRef o, CFIndex loc, CFRange *ler, CFRange limit) { hook_total++; if (ler) *ler = CFRangeMake(0, 4); return fake_dict; }
static CFTypeRef fake_astr_attributeLongest(CFTypeRef o, CFTypeRef name, CFIndex loc, CFRange *ler, CFRange limit) { hook_total++; if (ler) *ler = CFRangeMake(0, 4); return fake_str; }
static CFMutableStringRef fake_astr_mutableString(CFTypeRef o) { hook_total++; return NULL; }
static void fake_astr_replaceCharacters(CFTypeRef o, CFRange r, CFTypeRef s) { hook_total++; }
static void fake_astr_setAttributes(CFTypeRef o, CFTypeRef a, CFRange r) { hook_total++; }
static void fake_astr_addAttributes(CFTypeRef o, CFTypeRef a, CFRange r) { hook_total++; }
static void fake_astr_addAttribute(CFTypeRef o, CFTypeRef n, CFTypeRef v, CFRange r) { hook_total++; }
static void fake_astr_removeAttribute(CFTypeRef o, CFTypeRef n, CFRange r) { hook_total++; }
static void fake_astr_replaceWithAttrStr(CFTypeRef o, CFRange r, CFTypeRef s) { hook_total++; }
static void fake_astr_beginEditing(CFTypeRef o) { hook_total++; }
static void fake_astr_endEditing(CFTypeRef o) { hook_total++; }

static CFAbsoluteTime fake_timer_cffireTime(CFTypeRef o) { hook_total++; return 123.0; }
static CFTimeInterval fake_timer_timeInterval(CFTypeRef o) { hook_total++; return 2.5; }
static void fake_timer_invalidate(CFTypeRef o) { hook_total++; }
static Boolean fake_timer_isValid(CFTypeRef o) { hook_total++; return true; }
static CFTimeInterval fake_timer_tolerance(CFTypeRef o) { hook_total++; return 0.5; }
static void fake_timer_setTolerance(CFTypeRef o, CFTimeInterval t) { hook_total++; }
static void fake_timer_setFireDate(CFTypeRef o, CFAbsoluteTime d) { hook_total++; }

static CFNumberType fake_num_cfNumberGetType(CFTypeRef o) { hook_total++; return kCFNumberSInt64Type; }
static bool fake_num_boolValue(CFTypeRef o) { hook_total++; return true; }
static bool fake_num_getValue(CFTypeRef o, void *value, CFNumberType type) {
    hook_total++;
    if (type == kCFNumberSInt128Type) memset(value, 0, 16);
    else memset(value, 0, 8);
    return true;
}
static CFIndex fake_dict_countForObject(CFTypeRef o, CFTypeRef v) { hook_total++; return 6; }

static Boolean fake_cset_longCharacterIsMember(CFTypeRef o, UTF32Char ch) { hook_total++; return true; }
static CFCharacterSetRef fake_cset_expanded(CFTypeRef o) { hook_total++; return fake_cset; }
static CFDataRef fake_cset_bitmap(CFTypeRef o) { hook_total++; return (CFDataRef)CFRetain(fake_data); }
static CFMutableCharacterSetRef fake_cset_mutableCopy(CFTypeRef o) { hook_total++; return CFCharacterSetCreateMutableCopy(NULL, fake_cset); }
static Boolean fake_cset_characterIsMember(CFTypeRef o, UniChar ch) { hook_total++; return true; }
static CFCharacterSetRef fake_cset_invertedSet(CFTypeRef o) { hook_total++; return (CFCharacterSetRef)CFRetain(fake_cset); }
static Boolean fake_cset_hasMemberInPlane(CFTypeRef o, uint8_t plane) { hook_total++; return true; }
static void fake_cset_addCharactersInRange(CFTypeRef o, CFRange r) { hook_total++; }
static void fake_cset_removeCharactersInRange(CFTypeRef o, CFRange r) { hook_total++; }
static void fake_cset_addCharactersInString(CFTypeRef o, CFStringRef s) { hook_total++; }
static void fake_cset_removeCharactersInString(CFTypeRef o, CFStringRef s) { hook_total++; }
static void fake_cset_formUnion(CFTypeRef o, CFTypeRef s) { hook_total++; }
static void fake_cset_formIntersection(CFTypeRef o, CFTypeRef s) { hook_total++; }
static void fake_cset_invert(CFTypeRef o) { hook_total++; }

static int fake_date_class, fake_error_class, fake_url_class,
    fake_locale_class, fake_tz_class, fake_cal_class, fake_astr_class,
    fake_timer_class, fake_number_class, fake_boolean_class,
    fake_dict_class, fake_cset_class, fake_foreign_class;

/* A one-word foreign object at the end of a mapped page: any read past
 * the isa faults. Caller munmaps. */
static CFTypeRef
one_word_foreign(char **mapp, long page)
{
    char *map = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
	MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) return NULL;
    if (mprotect(map + page, page, PROT_NONE) != 0) {
	munmap(map, 2 * page);
	return NULL;
    }
    uintptr_t *one_word = (uintptr_t *)(map + page - sizeof(uintptr_t));
    *one_word = (uintptr_t)&fake_foreign_class;
    *mapp = map;
    return (CFTypeRef)one_word;
}

#define HOOK_CHECK(expr, want, msg) do { \
    int before = hook_total; \
    (void)(expr); \
    if (hook_total != before + (want)) { fprintf(stderr, "delta=%d want=%d who=%d\n", hook_total - before, want, hook_who); hook_who = 0; return fail(msg); } \
} while (0)

#define HOOK_FIRED(expr, msg) do { \
    int before = hook_total; \
    (void)(expr); \
    if (hook_total <= before) return fail(msg); \
} while (0)

/* Re-stamp the one-word object's isa so fake_cfTypeID answers the right
 * type ID for functions that validate before dispatching. */
#define SET_FOREIGN_ISA(isa) (*(uintptr_t *)f = (uintptr_t)&(isa))

static int
test_foreign_bridge_2b(void)
{
    long page = sysconf(_SC_PAGESIZE);
    char *map = NULL;
    CFTypeRef f = one_word_foreign(&map, page);
    if (!f) return fail("one-word foreign object setup failed");

    /* A CFDate created while CFDate has only the default class (from
     * test_foreign_bridge) is still native after CFDate gets its own
     * class: bridge classes only ever mark CF-native objects. */
    CFDateRef early_date = CFDateCreate(NULL, 7.0);
    if (!early_date) return fail("early CFDateCreate failed");

    /* Per-type classes. */
    _CFRuntimeBridgeTypeToClass(CFDateGetTypeID(), &fake_date_class);
    _CFRuntimeBridgeTypeToClass(CFErrorGetTypeID(), &fake_error_class);
    _CFRuntimeBridgeTypeToClass(CFURLGetTypeID(), &fake_url_class);
    _CFRuntimeBridgeTypeToClass(CFLocaleGetTypeID(), &fake_locale_class);
    _CFRuntimeBridgeTypeToClass(CFTimeZoneGetTypeID(), &fake_tz_class);
    _CFRuntimeBridgeTypeToClass(CFCalendarGetTypeID(), &fake_cal_class);
    _CFRuntimeBridgeTypeToClass(CFAttributedStringGetTypeID(), &fake_astr_class);
    _CFRuntimeBridgeTypeToClass(CFRunLoopTimerGetTypeID(), &fake_timer_class);
    _CFRuntimeBridgeTypeToClass(CFNumberGetTypeID(), &fake_number_class);
    _CFRuntimeBridgeTypeToClass(CFBooleanGetTypeID(), &fake_boolean_class);
    _CFRuntimeBridgeTypeToClass(CFDictionaryGetTypeID(), &fake_dict_class);
    _CFRuntimeBridgeTypeToClass(CFCharacterSetGetTypeID(), &fake_cset_class);

    /* Real CF objects the hooks hand back. */
    fake_cfurl = CFURLCreateWithString(NULL, CFSTR("file:///tmp/x"), NULL);
    fake_dict = CFDictionaryCreate(NULL, NULL, NULL, 0,
	&kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    fake_data = CFDataCreate(NULL, (const UInt8 *)"d", 1);
    fake_locale = CFLocaleCreate(NULL, CFSTR("en_US"));
    fake_tz = CFTimeZoneCreateWithTimeIntervalFromGMT(NULL, 3600.0);
    fake_date = CFDateCreate(NULL, 1.0);
    fake_cset = CFCharacterSetCreateWithCharactersInString(NULL, CFSTR("ab"));
    fake_str = CFStringCreateWithCString(NULL, "fake", kCFStringEncodingUTF8);
    if (!fake_cfurl || !fake_dict || !fake_data || !fake_locale || !fake_tz
	|| !fake_date || !fake_cset || !fake_str)
	return fail("shared fake return objects failed");

    __CFSwiftBridge.NSDate.timeIntervalSinceReferenceDate = fake_date_timeIntervalSinceReferenceDate;
    __CFSwiftBridge.NSDate.timeIntervalSinceDate = fake_date_timeIntervalSinceDate;
    __CFSwiftBridge.NSDate.compare = fake_date_compare;

    {
	int before = hook_total;
	if (CFDateGetAbsoluteTime(early_date) != 7.0 || hook_total != before)
	    return fail("CFDate stamped with the default class took the foreign path");
	CFRelease(early_date);
    }

    __CFSwiftBridge.NSError.userInfo = fake_error_userInfo;
    __CFSwiftBridge.NSError.domain = fake_error_domain;
    __CFSwiftBridge.NSError.code = fake_error_code;
    __CFSwiftBridge.NSError.localizedDescription = fake_error_localizedDescription;
    __CFSwiftBridge.NSError.localizedFailureReason = fake_error_localizedFailureReason;
    __CFSwiftBridge.NSError.localizedRecoverySuggestion = fake_error_localizedRecoverySuggestion;

    __CFSwiftBridge.NSURL._cfurl = fake_url_cfurl;
    __CFSwiftBridge.NSURL.absoluteURL = fake_url_absoluteURL;
    __CFSwiftBridge.NSURL.relativeString = fake_url_relativeString;
    __CFSwiftBridge.NSURL.baseURL = fake_url_baseURL;
    __CFSwiftBridge.NSURL.scheme = fake_url_scheme;
    __CFSwiftBridge.NSURL.host = fake_url_host;
    __CFSwiftBridge.NSURL.port = fake_url_port;
    __CFSwiftBridge.NSURL.user = fake_url_user;
    __CFSwiftBridge.NSURL.password = fake_url_password;
    __CFSwiftBridge.NSURL.query = fake_url_query;
    __CFSwiftBridge.NSURL.fragment = fake_url_fragment;
    __CFSwiftBridge.NSURL.isFileReferenceURL = fake_url_isFileReferenceURL;

    __CFSwiftBridge.NSLocale._doesNotRequireSpecialCaseHandling = fake_locale_specialCase;
    __CFSwiftBridge.NSLocale._setDoesNotRequireSpecialCaseHandling = fake_locale_setSpecialCase;
    __CFSwiftBridge.NSLocale._prefs = fake_locale_prefs;
    __CFSwiftBridge.NSLocale.copy = fake_locale_copy;
    __CFSwiftBridge.NSLocale.localeIdentifier = fake_locale_identifier;
    __CFSwiftBridge.NSLocale.objectForKey = fake_locale_objectForKey;
    __CFSwiftBridge.NSLocale._copyDisplayNameForKey = fake_locale_displayName;

    __CFSwiftBridge.NSTimeZone.name = fake_tz_name;
    __CFSwiftBridge.NSTimeZone.data = fake_tz_data;
    __CFSwiftBridge.NSTimeZone._daylightSavingTimeOffsetForAbsoluteTime = fake_tz_dstOffset;
    __CFSwiftBridge.NSTimeZone._nextDaylightSavingTimeTransitionAfterAbsoluteTime = fake_tz_nextTransition;
    __CFSwiftBridge.NSTimeZone.localizedName = fake_tz_localizedName;
    __CFSwiftBridge.NSTimeZone.secondsFromGMTForDate = fake_tz_secondsFromGMT;
    __CFSwiftBridge.NSTimeZone.abbreviationForDate = fake_tz_abbreviation;
    __CFSwiftBridge.NSTimeZone.isDaylightSavingTimeForDate = fake_tz_isDST;

    __CFSwiftBridge.NSCalendar.calendarIdentifier = fake_cal_identifier;
    __CFSwiftBridge.NSCalendar._copyLocale = fake_cal_copyLocale;
    __CFSwiftBridge.NSCalendar.setLocale = fake_cal_setLocale;
    __CFSwiftBridge.NSCalendar._copyTimeZone = fake_cal_copyTimeZone;
    __CFSwiftBridge.NSCalendar.setTimeZone = fake_cal_setTimeZone;
    __CFSwiftBridge.NSCalendar.firstWeekday = fake_cal_firstWeekday;
    __CFSwiftBridge.NSCalendar.setFirstWeekday = fake_cal_setFirstWeekday;
    __CFSwiftBridge.NSCalendar.minimumDaysInFirstWeek = fake_cal_minDays;
    __CFSwiftBridge.NSCalendar.setMinimumDaysInFirstWeek = fake_cal_setMinDays;
    __CFSwiftBridge.NSCalendar._copyGregorianStartDate = fake_cal_copyGregorianStart;
    __CFSwiftBridge.NSCalendar._setGregorianStartDate = fake_cal_setGregorianStart;
    __CFSwiftBridge.NSCalendar._minimumRangeOfUnit = fake_cal_minRange;
    __CFSwiftBridge.NSCalendar._maximumRangeOfUnit = fake_cal_maxRange;
    __CFSwiftBridge.NSCalendar._composeAbsoluteTime = fake_cal_compose;
    __CFSwiftBridge.NSCalendar._decomposeAbsoluteTime = fake_cal_decompose;
    __CFSwiftBridge.NSCalendar._addComponents = fake_cal_addComponents;
    __CFSwiftBridge.NSCalendar._diffComponents = fake_cal_diffComponents;
    __CFSwiftBridge.NSCalendar._rangeOfUnitStartTimeIntervalForAT = fake_cal_rangeOfUnit;
    __CFSwiftBridge.NSCalendar._rangeOfUnitInUnitForAT = fake_cal_rangeOfUnitInUnit;
    __CFSwiftBridge.NSCalendar._ordinalityOfUnitInUnitForAT = fake_cal_ordinality;

    __CFSwiftBridge.NSAttributedString.string = fake_astr_string;
    __CFSwiftBridge.NSAttributedString.length = fake_astr_length;
    __CFSwiftBridge.NSAttributedString.attributesAtIndexEffectiveRange = fake_astr_attributesAtIndex;
    __CFSwiftBridge.NSAttributedString.attributeAtIndexEffectiveRange = fake_astr_attributeAtIndex;
    __CFSwiftBridge.NSAttributedString.attributesAtIndexLongestEffectiveRangeInRange = fake_astr_attributesLongest;
    __CFSwiftBridge.NSAttributedString.attributeAtIndexLongestEffectiveRangeInRange = fake_astr_attributeLongest;
    __CFSwiftBridge.NSMutableAttributedString.mutableString = fake_astr_mutableString;
    __CFSwiftBridge.NSMutableAttributedString.replaceCharactersInRangeWithString = fake_astr_replaceCharacters;
    __CFSwiftBridge.NSMutableAttributedString.setAttributesRange = fake_astr_setAttributes;
    __CFSwiftBridge.NSMutableAttributedString.addAttributesRange = fake_astr_addAttributes;
    __CFSwiftBridge.NSMutableAttributedString.addAttributeValueRange = fake_astr_addAttribute;
    __CFSwiftBridge.NSMutableAttributedString.removeAttributeRange = fake_astr_removeAttribute;
    __CFSwiftBridge.NSMutableAttributedString.replaceCharactersInRangeWithAttributedString = fake_astr_replaceWithAttrStr;
    __CFSwiftBridge.NSMutableAttributedString.beginEditing = fake_astr_beginEditing;
    __CFSwiftBridge.NSMutableAttributedString.endEditing = fake_astr_endEditing;

    __CFSwiftBridge.NSTimer._cffireTime = fake_timer_cffireTime;
    __CFSwiftBridge.NSTimer.timeInterval = fake_timer_timeInterval;
    __CFSwiftBridge.NSTimer.invalidate = fake_timer_invalidate;
    __CFSwiftBridge.NSTimer.isValid = fake_timer_isValid;
    __CFSwiftBridge.NSTimer.tolerance = fake_timer_tolerance;
    __CFSwiftBridge.NSTimer.setTolerance = fake_timer_setTolerance;
    __CFSwiftBridge.NSTimer.setFireDate = fake_timer_setFireDate;

    __CFSwiftBridge.NSNumber._cfNumberGetType = fake_num_cfNumberGetType;
    __CFSwiftBridge.NSNumber.boolValue = fake_num_boolValue;
    __CFSwiftBridge.NSNumber._getValue = fake_num_getValue;
    __CFSwiftBridge.NSDictionary.countForObject = fake_dict_countForObject;

    __CFSwiftBridge.NSCharacterSet.longCharacterIsMember = fake_cset_longCharacterIsMember;
    __CFSwiftBridge.NSCharacterSet._expandedCFCharacterSet = fake_cset_expanded;
    __CFSwiftBridge.NSCharacterSet._retainedBitmapRepresentation = fake_cset_bitmap;
    __CFSwiftBridge.NSCharacterSet.mutableCopy = fake_cset_mutableCopy;
    __CFSwiftBridge.NSCharacterSet.characterIsMember = fake_cset_characterIsMember;
    __CFSwiftBridge.NSCharacterSet.invertedSet = fake_cset_invertedSet;
    __CFSwiftBridge.NSCharacterSet.hasMemberInPlane = fake_cset_hasMemberInPlane;
    __CFSwiftBridge.NSMutableCharacterSet.addCharactersInRange = fake_cset_addCharactersInRange;
    __CFSwiftBridge.NSMutableCharacterSet.removeCharactersInRange = fake_cset_removeCharactersInRange;
    __CFSwiftBridge.NSMutableCharacterSet.addCharactersInString = fake_cset_addCharactersInString;
    __CFSwiftBridge.NSMutableCharacterSet.removeCharactersInString = fake_cset_removeCharactersInString;
    __CFSwiftBridge.NSMutableCharacterSet.formUnionWithCharacterSet = fake_cset_formUnion;
    __CFSwiftBridge.NSMutableCharacterSet.formIntersectionWithCharacterSet = fake_cset_formIntersection;
    __CFSwiftBridge.NSMutableCharacterSet.invert = fake_cset_invert;

    /* Every public function with a new dispatch must reach its hook on
     * the one-word foreign object. */
    SET_FOREIGN_ISA(fake_date_isa);
    HOOK_CHECK(CFDateGetAbsoluteTime((CFDateRef)f), 1, "CFDateGetAbsoluteTime missed hook");
    HOOK_CHECK(CFDateGetTimeIntervalSinceDate((CFDateRef)f, fake_date), 1, "CFDateGetTimeIntervalSinceDate missed hook");
    HOOK_CHECK(CFDateCompare((CFDateRef)f, fake_date, NULL), 1, "CFDateCompare missed hook");
    /* A native date with a foreign other must not read the foreign
     * object's fields either. */
    HOOK_CHECK(CFDateGetTimeIntervalSinceDate(fake_date, (CFDateRef)f), 1, "CFDateGetTimeIntervalSinceDate foreign arg missed hook");
    HOOK_CHECK(CFDateCompare(fake_date, (CFDateRef)f, NULL), 1, "CFDateCompare foreign arg missed hook");

    SET_FOREIGN_ISA(fake_error_isa);
    HOOK_CHECK(CFErrorGetDomain((CFErrorRef)f), 1, "CFErrorGetDomain missed hook");
    HOOK_CHECK(CFErrorGetCode((CFErrorRef)f), 1, "CFErrorGetCode missed hook");
    HOOK_CHECK(CFErrorCopyUserInfo((CFErrorRef)f), 1, "CFErrorCopyUserInfo missed hook");
    HOOK_CHECK(CFErrorCopyDescription((CFErrorRef)f), 1, "CFErrorCopyDescription missed hook");
    HOOK_CHECK(CFErrorCopyFailureReason((CFErrorRef)f), 1, "CFErrorCopyFailureReason missed hook");
    HOOK_CHECK(CFErrorCopyRecoverySuggestion((CFErrorRef)f), 1, "CFErrorCopyRecoverySuggestion missed hook");

    SET_FOREIGN_ISA(fake_url_isa);
    HOOK_CHECK(CFURLGetString((CFURLRef)f), 1, "CFURLGetString missed hook");
    HOOK_CHECK(CFURLGetBaseURL((CFURLRef)f), 1, "CFURLGetBaseURL missed hook");
    HOOK_CHECK(CFURLCopyAbsoluteURL((CFURLRef)f), 1, "CFURLCopyAbsoluteURL missed hook");
    HOOK_CHECK(CFURLCopyScheme((CFURLRef)f), 1, "CFURLCopyScheme missed hook");
    HOOK_CHECK(CFURLCopyHostName((CFURLRef)f), 1, "CFURLCopyHostName missed hook");
    HOOK_CHECK(CFURLGetPortNumber((CFURLRef)f), 1, "CFURLGetPortNumber missed hook");
    HOOK_CHECK(CFURLCopyUserName((CFURLRef)f), 1, "CFURLCopyUserName missed hook");
    HOOK_CHECK(CFURLCopyPassword((CFURLRef)f), 1, "CFURLCopyPassword missed hook");
    HOOK_CHECK(CFURLCopyQueryString((CFURLRef)f, NULL), 1, "CFURLCopyQueryString missed hook");
    HOOK_CHECK(CFURLCopyFragment((CFURLRef)f, NULL), 1, "CFURLCopyFragment missed hook");
    /* _cfurl sites: the foreign object is converted to its backing CFURL. */
    HOOK_CHECK(CFURLCanBeDecomposed((CFURLRef)f), 1, "CFURLCanBeDecomposed missed _cfurl hook");
    HOOK_CHECK(CFURLCopyPath((CFURLRef)f), 1, "CFURLCopyPath missed _cfurl hook");
    HOOK_CHECK(CFURLCopyNetLocation((CFURLRef)f), 1, "CFURLCopyNetLocation missed _cfurl hook");
    HOOK_CHECK(CFURLHasDirectoryPath((CFURLRef)f), 1, "CFURLHasDirectoryPath missed _cfurl hook");
    HOOK_CHECK(CFURLCopyResourceSpecifier((CFURLRef)f), 1, "CFURLCopyResourceSpecifier missed _cfurl hook");
    HOOK_CHECK(_CFURLGetEncoding((CFURLRef)f), 1, "_CFURLGetEncoding missed _cfurl hook");
    /* Guarded field reads: foreign object must take the CFURLGetString
     * path (relativeString hook). */
    {
	uint8_t buf[64];
	HOOK_CHECK(CFURLGetBytes((CFURLRef)f, buf, sizeof buf), 1, "CFURLGetBytes missed relativeString hook");
	HOOK_CHECK(CFURLGetBytesUsingEncoding((CFURLRef)f, buf, sizeof buf, kCFStringEncodingUTF8), 1, "CFURLGetBytesUsingEncoding missed relativeString hook");
    }
    HOOK_CHECK(CFURLCopyLastPathComponent((CFURLRef)f), 1, "CFURLCopyLastPathComponent missed hooks");
    HOOK_CHECK(CFURLCopyPathExtension((CFURLRef)f), 1, "CFURLCopyPathExtension missed hooks");
    HOOK_FIRED(CFURLCopyFileSystemPath((CFURLRef)f, kCFURLPOSIXPathStyle), "CFURLCopyFileSystemPath missed hooks");
    HOOK_CHECK(CFURLGetFileSystemRepresentation((CFURLRef)f, false, NULL, 0), 1, "CFURLGetFileSystemRepresentation missed hooks");

    SET_FOREIGN_ISA(fake_locale_isa);
    HOOK_CHECK(CFLocaleGetIdentifier((CFLocaleRef)f), 1, "CFLocaleGetIdentifier missed hook");
    HOOK_CHECK(CFLocaleGetValue((CFLocaleRef)f, kCFLocaleCountryCodeKey), 1, "CFLocaleGetValue missed hook");
    HOOK_CHECK(CFLocaleCreateCopy(NULL, (CFLocaleRef)f), 1, "CFLocaleCreateCopy missed hook");
    HOOK_CHECK(CFLocaleCopyDisplayNameForPropertyValue((CFLocaleRef)f, kCFLocaleCountryCodeKey, fake_str), 1, "CFLocaleCopyDisplayNameForPropertyValue missed hook");

    SET_FOREIGN_ISA(fake_tz_isa);
    HOOK_CHECK(CFTimeZoneGetName((CFTimeZoneRef)f), 1, "CFTimeZoneGetName missed hook");
    HOOK_CHECK(CFTimeZoneGetData((CFTimeZoneRef)f), 1, "CFTimeZoneGetData missed hook");
    HOOK_CHECK(CFTimeZoneGetSecondsFromGMT((CFTimeZoneRef)f, 0.0), 1, "CFTimeZoneGetSecondsFromGMT missed hook");
    HOOK_CHECK(CFTimeZoneCopyAbbreviation((CFTimeZoneRef)f, 0.0), 1, "CFTimeZoneCopyAbbreviation missed hook");
    HOOK_CHECK(CFTimeZoneIsDaylightSavingTime((CFTimeZoneRef)f, 0.0), 1, "CFTimeZoneIsDaylightSavingTime missed hook");
    HOOK_CHECK(CFTimeZoneGetDaylightSavingTimeOffset((CFTimeZoneRef)f, 0.0), 1, "CFTimeZoneGetDaylightSavingTimeOffset missed hook");
    HOOK_CHECK(CFTimeZoneGetNextDaylightSavingTimeTransition((CFTimeZoneRef)f, 0.0), 1, "CFTimeZoneGetNextDaylightSavingTimeTransition missed hook");
    HOOK_CHECK(CFTimeZoneCopyLocalizedName((CFTimeZoneRef)f, kCFTimeZoneNameStyleStandard, fake_locale), 1, "CFTimeZoneCopyLocalizedName missed hook");

    SET_FOREIGN_ISA(fake_cal_isa);
    HOOK_CHECK(CFCalendarGetIdentifier((CFCalendarRef)f), 1, "CFCalendarGetIdentifier missed hook");
    HOOK_CHECK(CFCalendarCopyLocale((CFCalendarRef)f), 1, "CFCalendarCopyLocale missed hook");
    HOOK_CHECK(CFCalendarSetLocale((CFCalendarRef)f, fake_locale), 1, "CFCalendarSetLocale missed hook");
    HOOK_CHECK(CFCalendarCopyTimeZone((CFCalendarRef)f), 1, "CFCalendarCopyTimeZone missed hook");
    HOOK_CHECK(CFCalendarSetTimeZone((CFCalendarRef)f, fake_tz), 1, "CFCalendarSetTimeZone missed hook");
    HOOK_CHECK(CFCalendarGetFirstWeekday((CFCalendarRef)f), 1, "CFCalendarGetFirstWeekday missed hook");
    HOOK_CHECK(CFCalendarSetFirstWeekday((CFCalendarRef)f, 2), 1, "CFCalendarSetFirstWeekday missed hook");
    HOOK_CHECK(CFCalendarGetMinimumDaysInFirstWeek((CFCalendarRef)f), 1, "CFCalendarGetMinimumDaysInFirstWeek missed hook");
    HOOK_CHECK(CFCalendarSetMinimumDaysInFirstWeek((CFCalendarRef)f, 3), 1, "CFCalendarSetMinimumDaysInFirstWeek missed hook");
    HOOK_CHECK(CFCalendarCopyGregorianStartDate((CFCalendarRef)f), 1, "CFCalendarCopyGregorianStartDate missed hook");
    HOOK_CHECK(CFCalendarSetGregorianStartDate((CFCalendarRef)f, fake_date), 1, "CFCalendarSetGregorianStartDate missed hook");
    HOOK_CHECK(CFCalendarGetMinimumRangeOfUnit((CFCalendarRef)f, kCFCalendarUnitDay), 1, "CFCalendarGetMinimumRangeOfUnit missed hook");
    HOOK_CHECK(CFCalendarGetMaximumRangeOfUnit((CFCalendarRef)f, kCFCalendarUnitDay), 1, "CFCalendarGetMaximumRangeOfUnit missed hook");
    {
	CFAbsoluteTime at = 0.0;
	int32_t comp = 0;
	HOOK_CHECK(CFCalendarComposeAbsoluteTime((CFCalendarRef)f, &at, "y", 2020), 1, "CFCalendarComposeAbsoluteTime missed hook");
	HOOK_CHECK(CFCalendarDecomposeAbsoluteTime((CFCalendarRef)f, 0.0, "y", &comp), 1, "CFCalendarDecomposeAbsoluteTime missed hook");
	HOOK_CHECK(CFCalendarAddComponents((CFCalendarRef)f, &at, 0, "d", 1), 1, "CFCalendarAddComponents missed hook");
	HOOK_CHECK(CFCalendarGetComponentDifference((CFCalendarRef)f, 0.0, 1.0, 0, "d", &comp), 1, "CFCalendarGetComponentDifference missed hook");
	HOOK_CHECK(CFCalendarGetTimeRangeOfUnit((CFCalendarRef)f, kCFCalendarUnitDay, 0.0, &at, NULL), 1, "CFCalendarGetTimeRangeOfUnit missed hook");
    }
    HOOK_CHECK(CFCalendarGetRangeOfUnit((CFCalendarRef)f, kCFCalendarUnitDay, kCFCalendarUnitMonth, 0.0), 1, "CFCalendarGetRangeOfUnit missed hook");
    HOOK_CHECK(CFCalendarGetOrdinalityOfUnit((CFCalendarRef)f, kCFCalendarUnitDay, kCFCalendarUnitMonth, 0.0), 1, "CFCalendarGetOrdinalityOfUnit missed hook");
    /* The *V internals have no dispatch; foreign calendars fail cleanly. */
    {
	CFDateComponentsRef dc = CFDateComponentsCreate(NULL);
	CFDateComponentsSetValue(dc, kCFCalendarUnitDay, 1);
	if (CFCalendarCreateDateFromComponents(NULL, (CFCalendarRef)f, dc) != NULL)
	    return fail("CFCalendarCreateDateFromComponents did not fail cleanly on foreign calendar");
	CFRelease(dc);
	if (CFCalendarGetComponentFromDate((CFCalendarRef)f, kCFCalendarUnitDay, fake_date) != CFDateComponentUndefined)
	    return fail("CFCalendarGetComponentFromDate did not fail cleanly on foreign calendar");
	if (!CFCalendarCreateDateComponentsFromDate(NULL, (CFCalendarRef)f, kCFCalendarUnitDay, fake_date))
	    return fail("CFCalendarCreateDateComponentsFromDate failed on foreign calendar");
    }

    SET_FOREIGN_ISA(fake_astr_isa);
    HOOK_CHECK(CFAttributedStringGetString((CFAttributedStringRef)f), 1, "CFAttributedStringGetString missed hook");
    HOOK_CHECK(CFAttributedStringGetLength((CFAttributedStringRef)f), 1, "CFAttributedStringGetLength missed hook");
    {
	CFRange er;
	HOOK_CHECK(CFAttributedStringGetAttributes((CFAttributedStringRef)f, 0, &er), 1, "CFAttributedStringGetAttributes missed hook");
	HOOK_CHECK(CFAttributedStringGetAttribute((CFAttributedStringRef)f, 0, fake_str, &er), 1, "CFAttributedStringGetAttribute missed hook");
	HOOK_CHECK(CFAttributedStringGetAttributesAndLongestEffectiveRange((CFAttributedStringRef)f, 0, CFRangeMake(0, 4), &er), 1, "CFAttributedStringGetAttributesAndLongestEffectiveRange missed hook");
	HOOK_CHECK(CFAttributedStringGetAttributeAndLongestEffectiveRange((CFAttributedStringRef)f, 0, fake_str, CFRangeMake(0, 4), &er), 1, "CFAttributedStringGetAttributeAndLongestEffectiveRange missed hook");
    }
    HOOK_CHECK(CFAttributedStringGetMutableString((CFMutableAttributedStringRef)f), 1, "CFAttributedStringGetMutableString missed hook");
    HOOK_CHECK(CFAttributedStringReplaceString((CFMutableAttributedStringRef)f, CFRangeMake(0, 1), fake_str), 1, "CFAttributedStringReplaceString missed hook");
    HOOK_CHECK(CFAttributedStringSetAttributes((CFMutableAttributedStringRef)f, CFRangeMake(0, 1), fake_dict, true), 1, "CFAttributedStringSetAttributes missed hook");
    HOOK_CHECK(CFAttributedStringSetAttributes((CFMutableAttributedStringRef)f, CFRangeMake(0, 1), fake_dict, false), 1, "CFAttributedStringSetAttributes(addAttributes) missed hook");
    HOOK_CHECK(CFAttributedStringSetAttribute((CFMutableAttributedStringRef)f, CFRangeMake(0, 1), fake_str, fake_str), 1, "CFAttributedStringSetAttribute missed hook");
    HOOK_CHECK(CFAttributedStringRemoveAttribute((CFMutableAttributedStringRef)f, CFRangeMake(0, 1), fake_str), 1, "CFAttributedStringRemoveAttribute missed hook");
    HOOK_CHECK(CFAttributedStringReplaceAttributedString((CFMutableAttributedStringRef)f, CFRangeMake(0, 1), (CFAttributedStringRef)f), 1, "CFAttributedStringReplaceAttributedString missed hook");
    HOOK_CHECK(CFAttributedStringBeginEditing((CFMutableAttributedStringRef)f), 1, "CFAttributedStringBeginEditing missed hook");
    HOOK_CHECK(CFAttributedStringEndEditing((CFMutableAttributedStringRef)f), 1, "CFAttributedStringEndEditing missed hook");
    /* The create helpers must not read foreign fields. */
    HOOK_FIRED(CFAttributedStringCreateWithSubstring(NULL, (CFAttributedStringRef)f, CFRangeMake(0, 1)), "CFAttributedStringCreateWithSubstring missed hooks");
    HOOK_FIRED(CFAttributedStringCreateCopy(NULL, (CFAttributedStringRef)f), "CFAttributedStringCreateCopy missed hooks");
    HOOK_FIRED(CFAttributedStringCreateMutableCopy(NULL, 0, (CFAttributedStringRef)f), "CFAttributedStringCreateMutableCopy missed hooks");

    SET_FOREIGN_ISA(fake_timer_isa);
    HOOK_CHECK(CFRunLoopTimerGetNextFireDate((CFRunLoopTimerRef)f), 1, "CFRunLoopTimerGetNextFireDate missed hook");
    HOOK_CHECK(CFRunLoopTimerGetInterval((CFRunLoopTimerRef)f), 1, "CFRunLoopTimerGetInterval missed hook");
    HOOK_CHECK(CFRunLoopTimerIsValid((CFRunLoopTimerRef)f), 1, "CFRunLoopTimerIsValid missed hook");
    HOOK_CHECK(CFRunLoopTimerInvalidate((CFRunLoopTimerRef)f), 1, "CFRunLoopTimerInvalidate missed hook");
    HOOK_CHECK(CFRunLoopTimerDoesRepeat((CFRunLoopTimerRef)f), 1, "CFRunLoopTimerDoesRepeat missed hook");
    HOOK_CHECK(CFRunLoopTimerSetNextFireDate((CFRunLoopTimerRef)f, 1.0), 1, "CFRunLoopTimerSetNextFireDate missed hook");

    /* ObjC-only sites on the 2a types. */
    SET_FOREIGN_ISA(fake_boolean_isa);
    HOOK_CHECK(CFBooleanGetValue((CFBooleanRef)f), 1, "CFBooleanGetValue missed hook");
    SET_FOREIGN_ISA(fake_number_isa);
    HOOK_CHECK(CFNumberGetType((CFNumberRef)f), 1, "CFNumberGetType missed hook");
    HOOK_CHECK(_CFNumberGetType2((CFNumberRef)f), 1, "_CFNumberGetType2 missed hook");
    HOOK_CHECK(CFNumberIsFloatType((CFNumberRef)f), 1, "CFNumberIsFloatType missed hook");
    HOOK_CHECK(CFNumberCompare((CFNumberRef)f, (CFNumberRef)f, NULL), 4, "CFNumberCompare missed hooks");
    SET_FOREIGN_ISA(fake_dict_isa);
    HOOK_CHECK(CFDictionaryGetCountOfValue((CFDictionaryRef)f, fake_str), 1, "CFDictionaryGetCountOfValue missed hook");
    SET_FOREIGN_ISA(fake_cset_isa);
    HOOK_CHECK(CFCharacterSetIsCharacterMember((CFCharacterSetRef)f, 'a'), 1, "CFCharacterSetIsCharacterMember missed hook");
    HOOK_CHECK(CFCharacterSetIsLongCharacterMember((CFCharacterSetRef)f, 'a'), 1, "CFCharacterSetIsLongCharacterMember missed hook");
    HOOK_CHECK(CFCharacterSetCreateInvertedSet(NULL, (CFCharacterSetRef)f), 1, "CFCharacterSetCreateInvertedSet missed hook");
    HOOK_CHECK(CFCharacterSetHasMemberInPlane((CFCharacterSetRef)f, 0), 1, "CFCharacterSetHasMemberInPlane missed hook");
    HOOK_CHECK(CFCharacterSetCreateBitmapRepresentation(NULL, (CFCharacterSetRef)f), 1, "CFCharacterSetCreateBitmapRepresentation missed hook");
    HOOK_CHECK(CFCharacterSetCreateMutableCopy(NULL, (CFCharacterSetRef)f), 1, "CFCharacterSetCreateMutableCopy missed hook");
    HOOK_CHECK(CFCharacterSetIsSupersetOfSet((CFCharacterSetRef)f, (CFCharacterSetRef)f), 2, "CFCharacterSetIsSupersetOfSet missed hooks");
    HOOK_CHECK(CFCharacterSetAddCharactersInRange((CFMutableCharacterSetRef)f, CFRangeMake(0, 1)), 1, "CFCharacterSetAddCharactersInRange missed hook");
    HOOK_CHECK(CFCharacterSetRemoveCharactersInRange((CFMutableCharacterSetRef)f, CFRangeMake(0, 1)), 1, "CFCharacterSetRemoveCharactersInRange missed hook");
    HOOK_CHECK(CFCharacterSetAddCharactersInString((CFMutableCharacterSetRef)f, fake_str), 1, "CFCharacterSetAddCharactersInString missed hook");
    HOOK_CHECK(CFCharacterSetRemoveCharactersInString((CFMutableCharacterSetRef)f, fake_str), 1, "CFCharacterSetRemoveCharactersInString missed hook");
    HOOK_CHECK(CFCharacterSetUnion((CFMutableCharacterSetRef)f, fake_cset), 1, "CFCharacterSetUnion missed hook");
    HOOK_CHECK(CFCharacterSetIntersect((CFMutableCharacterSetRef)f, fake_cset), 1, "CFCharacterSetIntersect missed hook");
    HOOK_CHECK(CFCharacterSetInvert((CFMutableCharacterSetRef)f), 1, "CFCharacterSetInvert missed hook");

    /* Mutability SPI: foreign objects are not mutable, no field reads. */
    if (_CFDataIsMutable((CFDataRef)f)) return fail("_CFDataIsMutable read foreign fields");
    if (_CFCharacterSetIsMutable((CFCharacterSetRef)f)) return fail("_CFCharacterSetIsMutable read foreign fields");
    if (_CFStringIsMutable((CFStringRef)f)) return fail("_CFStringIsMutable read foreign fields");
    if (_CFArrayIsMutable((CFArrayRef)f)) return fail("_CFArrayIsMutable read foreign fields");

    munmap(map, 2 * page);

    /* Native objects of each type must NOT reach the hooks. */
    {
	int before = hook_total;
	CFDateRef d = CFDateCreate(NULL, 3.0);
	CFDateGetAbsoluteTime(d);
	CFDateGetTimeIntervalSinceDate(d, fake_date);
	CFDateCompare(d, fake_date, NULL);
	CFRelease(d);

	CFErrorRef e = CFErrorCreate(NULL, CFSTR("dom"), 1, NULL);
	CFErrorGetDomain(e);
	CFErrorGetCode(e);
	CFErrorCopyUserInfo(e);
	CFRelease(e);

	CFURLRef u = CFURLCreateWithString(NULL, CFSTR("http://u:p@host:9/p?q#f"), NULL);
	CFURLGetString(u);
	CFURLCopyScheme(u);
	CFURLCopyHostName(u);
	CFURLGetPortNumber(u);
	CFURLCopyUserName(u);
	CFURLCopyPassword(u);
	CFURLCopyQueryString(u, NULL);
	CFURLCopyFragment(u, NULL);
	CFURLCopyPath(u);
	CFURLCopyLastPathComponent(u);
	CFURLCopyPathExtension(u);
	CFURLCopyFileSystemPath(u, kCFURLPOSIXPathStyle);
	CFURLGetBaseURL(u);
	CFURLCanBeDecomposed(u);
	CFRelease(u);

	CFLocaleRef l = CFLocaleCreate(NULL, CFSTR("en_US"));
	CFLocaleGetIdentifier(l);
	CFLocaleGetValue(l, kCFLocaleCountryCodeKey);
	CFLocaleCreateCopy(NULL, l);
	CFRelease(l);

	CFTimeZoneRef t = CFTimeZoneCreateWithTimeIntervalFromGMT(NULL, 3600.0);
	CFTimeZoneGetName(t);
	CFTimeZoneGetData(t);
	CFTimeZoneGetSecondsFromGMT(t, 0.0);
	CFTimeZoneIsDaylightSavingTime(t, 0.0);
	CFTimeZoneGetDaylightSavingTimeOffset(t, 0.0);
	CFTimeZoneGetNextDaylightSavingTimeTransition(t, 0.0);
	CFRelease(t);

	CFCalendarRef c = CFCalendarCreateWithIdentifier(NULL, kCFCalendarIdentifierGregorian);
	CFCalendarGetIdentifier(c);
	CFCalendarCopyLocale(c);
	CFCalendarCopyTimeZone(c);
	CFCalendarGetFirstWeekday(c);
	CFCalendarGetMinimumDaysInFirstWeek(c);
	CFCalendarCopyGregorianStartDate(c);
	CFCalendarGetMinimumRangeOfUnit(c, kCFCalendarUnitDay);
	CFCalendarGetMaximumRangeOfUnit(c, kCFCalendarUnitDay);
	CFCalendarGetRangeOfUnit(c, kCFCalendarUnitDay, kCFCalendarUnitMonth, 0.0);
	CFCalendarGetOrdinalityOfUnit(c, kCFCalendarUnitDay, kCFCalendarUnitMonth, 0.0);
	CFRelease(c);

	CFAttributedStringRef as = CFAttributedStringCreate(NULL, fake_str, NULL);
	CFAttributedStringGetString(as);
	CFAttributedStringGetLength(as);
	CFAttributedStringGetAttributes(as, 0, NULL);
	CFAttributedStringCreateCopy(NULL, as);
	CFAttributedStringCreateWithSubstring(NULL, as, CFRangeMake(0, 1));
	CFMutableAttributedStringRef mas = CFAttributedStringCreateMutableCopy(NULL, 0, as);
	CFAttributedStringReplaceString(mas, CFRangeMake(0, 1), fake_str);
	CFAttributedStringSetAttribute(mas, CFRangeMake(0, 1), fake_str, fake_str);
	CFAttributedStringRemoveAttribute(mas, CFRangeMake(0, 1), fake_str);
	CFAttributedStringBeginEditing(mas);
	CFAttributedStringEndEditing(mas);
	CFRelease(mas);
	CFRelease(as);

	CFRunLoopTimerRef tm = CFRunLoopTimerCreate(NULL, 1.0, 0.5, 0, 0, timer_fired, NULL);
	CFRunLoopTimerGetNextFireDate(tm);
	CFRunLoopTimerGetInterval(tm);
	CFRunLoopTimerIsValid(tm);
	CFRunLoopTimerDoesRepeat(tm);
	CFRunLoopTimerInvalidate(tm);
	CFRelease(tm);

	CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt64Type, &(int64_t){7});
	CFNumberGetType(n);
	_CFNumberGetType2(n);
	CFNumberIsFloatType(n);
	CFNumberCompare(n, n, NULL);
	CFRelease(n);
	CFBooleanGetValue(kCFBooleanTrue);

	CFDictionaryRef dd = CFDictionaryCreate(NULL, (const void **)&fake_str, (const void **)&fake_str, 1, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionaryGetCountOfValue(dd, fake_str);
	CFRelease(dd);

	CFCharacterSetRef cs = CFCharacterSetCreateWithCharactersInString(NULL, CFSTR("ab"));
	CFCharacterSetIsCharacterMember(cs, 'a');
	CFCharacterSetIsLongCharacterMember(cs, 'a');
	CFCharacterSetHasMemberInPlane(cs, 0);
	CFCharacterSetCreateBitmapRepresentation(NULL, cs);
	CFCharacterSetCreateInvertedSet(NULL, cs);
	CFCharacterSetCreateMutableCopy(NULL, cs);
	CFMutableCharacterSetRef mcs = CFCharacterSetCreateMutableCopy(NULL, cs);
	CFCharacterSetAddCharactersInRange(mcs, CFRangeMake(0, 1));
	CFCharacterSetRemoveCharactersInRange(mcs, CFRangeMake(0, 1));
	CFCharacterSetAddCharactersInString(mcs, fake_str);
	CFCharacterSetRemoveCharactersInString(mcs, fake_str);
	CFCharacterSetUnion(mcs, cs);
	CFCharacterSetIntersect(mcs, cs);
	CFCharacterSetInvert(mcs);
	CFRelease(mcs);
	CFRelease(cs);

	if (hook_total != before)
	    return fail("native objects reached foreign hooks");
    }

    CFRelease(fake_cfurl);
    CFRelease(fake_dict);
    CFRelease(fake_data);
    CFRelease(fake_locale);
    CFRelease(fake_tz);
    CFRelease(fake_date);
    CFRelease(fake_cset);
    CFRelease(fake_str);
    return 0;
}

/* A fake foreign object: CFRuntimeBase-shaped header, isa pointing at a
 * class CF never registered. */
static struct {
	uintptr_t isa;
	uint64_t cfinfoa;
} fake_foreign_obj = { 0, 0 };

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

	/* (h) A foreign object may be a single word (an Objective-C object
	 * with no instance variables): the generic foreign test must read
	 * only its isa. The word after it is an inaccessible page, so any
	 * read past the isa faults. */
	{
		long page = sysconf(_SC_PAGESIZE);
		char *map = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANON, -1, 0);
		if (map == MAP_FAILED) return fail("mmap failed");
		if (mprotect(map + page, page, PROT_NONE) != 0)
			return fail("mprotect failed");
		uintptr_t *one_word = (uintptr_t *)(map + page - sizeof(uintptr_t));
		*one_word = (uintptr_t)&fake_foreign_class;
		CFTypeRef small = (CFTypeRef)one_word;
		int r = hook_retain, x = hook_release, t = hook_cfTypeID,
		    h = hook_hash, c = hook_retainCount;
		if (CFRetain(small) != small || hook_retain != r + 1)
			return fail("one-word object did not reach retain hook");
		CFRelease(small);
		if (hook_release != x + 1)
			return fail("one-word object did not reach release hook");
		if (CFGetTypeID(small) != CFStringGetTypeID() || hook_cfTypeID != t + 1)
			return fail("one-word object did not reach _cfTypeID hook");
		if (CFHash(small) != 4242 || hook_hash != h + 1)
			return fail("one-word object did not reach hash hook");
		if (CFGetRetainCount(small) != 7 || hook_retainCount != c + 1)
			return fail("one-word object did not reach retainCount hook");
		munmap(map, 2 * page);
	}

	CFRelease(s1);
	CFRelease(a1);
	return 0;
}

/* ---- CFFileDescriptor -------------------------------------------------
 * Pipe + version-1 source: read fires once per enable, write fires when
 * the pipe has room, closeOnInvalidate closes the fd, mode isolation,
 * common-mode fan-out, and no callback for data consumed before the
 * run loop got to it.
 */

static int fd_read_calls, fd_write_calls;

static void
fd_callback(CFFileDescriptorRef f, CFOptionFlags types, void *info)
{
	(void)f; (void)info;
	if (types & kCFFileDescriptorReadCallBack) fd_read_calls++;
	if (types & kCFFileDescriptorWriteCallBack) fd_write_calls++;
}

static int
test_file_descriptor(void)
{
	CFRunLoopRef rl = CFRunLoopGetCurrent();
	int p[2];
	char buf[512];

	if (pipe(p) != 0)
		return fail("pipe");

	/* Read side: one-shot semantics. */
	CFFileDescriptorRef f = CFFileDescriptorCreate(NULL, p[0], false,
	    fd_callback, NULL);
	if (f == NULL)
		return fail("CFFileDescriptorCreate returned NULL");
	if (CFFileDescriptorGetNativeDescriptor(f) != p[0])
		return fail("CFFileDescriptorGetNativeDescriptor");
	CFRunLoopSourceRef src = CFFileDescriptorCreateRunLoopSource(NULL,
	    f, 0);
	if (src == NULL)
		return fail("CFFileDescriptorCreateRunLoopSource returned NULL");
	CFRunLoopAddSource(rl, src, kCFRunLoopDefaultMode);
	CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);

	/* Nothing to read: must not fire. */
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
	if (fd_read_calls != 0)
		return fail("read callback fired with no data");

	write(p[1], "x", 1);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
	if (fd_read_calls != 1)
		return fail("read callback did not fire once");

	/* One-shot: data still unread, must not fire again until
	 * re-enabled. */
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
	if (fd_read_calls != 1)
		return fail("read callback fired again without re-enable");

	CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
	if (fd_read_calls != 2)
		return fail("read callback did not fire after re-enable");

	/* Write side: fires while the pipe has room. */
	CFFileDescriptorRef wf = CFFileDescriptorCreate(NULL, p[1], false,
	    fd_callback, NULL);
	CFRunLoopSourceRef wsrc = CFFileDescriptorCreateRunLoopSource(NULL,
	    wf, 0);
	CFRunLoopAddSource(rl, wsrc, kCFRunLoopDefaultMode);
	CFFileDescriptorEnableCallBacks(wf, kCFFileDescriptorWriteCallBack);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
	if (fd_write_calls != 1)
		return fail("write callback did not fire on writable pipe");
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
	if (fd_write_calls != 1)
		return fail("write callback fired again without re-enable");

	/* Fill the pipe, re-enable: must not fire until drained. */
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	fcntl(p[1], F_SETFL, O_NONBLOCK);
	memset(buf, 'w', sizeof(buf));
	while (write(p[1], buf, sizeof(buf)) > 0)
		;
	CFFileDescriptorEnableCallBacks(wf, kCFFileDescriptorWriteCallBack);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
	if (fd_write_calls != 1)
		return fail("write callback fired on a full pipe");
	while (read(p[0], buf, sizeof(buf)) > 0)
		;
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
	if (fd_write_calls != 2)
		return fail("write callback did not fire after drain");

	CFRunLoopRemoveSource(rl, src, kCFRunLoopDefaultMode);
	CFRunLoopRemoveSource(rl, wsrc, kCFRunLoopDefaultMode);
	CFRelease(src);
	CFRelease(wsrc);
	CFFileDescriptorInvalidate(f);
	CFFileDescriptorInvalidate(wf);
	CFRelease(f);
	CFRelease(wf);
	close(p[0]);
	close(p[1]);

	/* closeOnInvalidate closes the descriptor. */
	if (pipe(p) != 0)
		return fail("pipe");
	f = CFFileDescriptorCreate(NULL, p[0], true, fd_callback, NULL);
	CFFileDescriptorInvalidate(f);
	if (CFFileDescriptorIsValid(f))
		return fail("descriptor still valid after invalidate");
	if (fcntl(p[0], F_GETFD) != -1 || errno != EBADF)
		return fail("closeOnInvalidate did not close the fd");
	CFRelease(f);
	close(p[1]);

	/* Mode isolation: a source only in mode A must not fire while
	 * mode B runs (a timer keeps mode B occupied). */
	CFStringRef modeA = CFSTR("TestModeA");
	CFStringRef modeB = CFSTR("TestModeB");
	if (pipe(p) != 0)
		return fail("pipe");
	f = CFFileDescriptorCreate(NULL, p[0], false, fd_callback, NULL);
	src = CFFileDescriptorCreateRunLoopSource(NULL, f, 0);
	CFRunLoopAddSource(rl, src, modeA);
	CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);
	write(p[1], "x", 1);
	int before = fd_read_calls;
	double bfired = 0;
	CFRunLoopTimerContext tctx = { 0, &bfired, NULL, NULL, NULL };
	CFRunLoopTimerRef bt = CFRunLoopTimerCreate(NULL,
	    CFAbsoluteTimeGetCurrent() + 0.05, 0, 0, 0, timer_fired, &tctx);
	CFRunLoopAddTimer(rl, bt, modeB);
	CFRunLoopRunInMode(modeB, 1.0, true);
	CFRunLoopTimerInvalidate(bt);
	CFRelease(bt);
	if (fd_read_calls != before)
		return fail("mode-A source fired while running mode B");
	CFRunLoopRunInMode(modeA, 1.0, true);
	if (fd_read_calls != before + 1)
		return fail("mode-A source did not fire in mode A");
	CFRunLoopRemoveSource(rl, src, modeA);
	CFRelease(src);
	CFFileDescriptorInvalidate(f);
	CFRelease(f);
	close(p[0]);
	close(p[1]);

	/* Common modes: fires in the default mode and in another mode
	 * added to the common set. */
	CFStringRef modeC = CFSTR("TestModeC");
	CFRunLoopAddCommonMode(rl, modeC);
	if (pipe(p) != 0)
		return fail("pipe");
	f = CFFileDescriptorCreate(NULL, p[0], false, fd_callback, NULL);
	src = CFFileDescriptorCreateRunLoopSource(NULL, f, 0);
	CFRunLoopAddSource(rl, src, kCFRunLoopCommonModes);
	CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);
	write(p[1], "x", 1);
	before = fd_read_calls;
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
	if (fd_read_calls != before + 1)
		return fail("common-mode source did not fire in default mode");
	CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);
	write(p[1], "x", 1);
	CFRunLoopRunInMode(modeC, 1.0, true);
	if (fd_read_calls != before + 2)
		return fail("common-mode source did not fire in mode C");
	CFRunLoopRemoveSource(rl, src, kCFRunLoopCommonModes);
	CFRelease(src);
	CFFileDescriptorInvalidate(f);
	CFRelease(f);
	close(p[0]);
	close(p[1]);

	/* Stale event: data that arrives while the callback is enabled
	 * and is consumed before the run loop services the descriptor
	 * must not fire the callback (a client re-enables before reading
	 * so a nested run loop can service the descriptor). */
	if (pipe(p) != 0)
		return fail("pipe");
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	f = CFFileDescriptorCreate(NULL, p[0], false, fd_callback, NULL);
	src = CFFileDescriptorCreateRunLoopSource(NULL, f, 0);
	CFRunLoopAddSource(rl, src, kCFRunLoopDefaultMode);
	CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);
	write(p[1], "x", 1);
	while (read(p[0], buf, sizeof(buf)) > 0)
		;
	before = fd_read_calls;
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, false);
	if (fd_read_calls != before)
		return fail("read callback fired for data already consumed");
	write(p[1], "y", 1);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
	if (fd_read_calls != before + 1)
		return fail("read callback did not fire after a stale event");
	CFRunLoopRemoveSource(rl, src, kCFRunLoopDefaultMode);
	CFRelease(src);
	CFFileDescriptorInvalidate(f);
	CFRelease(f);
	close(p[0]);
	close(p[1]);

	/* Regular file: always ready. On Linux epoll rejects it (EPERM)
	 * and CFFileDescriptor falls back to a signalled stand-in; on BSD
	 * kqueue reports it readable directly. */
	char tmp[] = "/tmp/cffd-test.XXXXXX";
	int tfd = mkstemp(tmp);
	if (tfd < 0)
		return fail("mkstemp");
	write(tfd, "data", 4);
	lseek(tfd, 0, SEEK_SET);
	f = CFFileDescriptorCreate(NULL, tfd, true, fd_callback, NULL);
	src = CFFileDescriptorCreateRunLoopSource(NULL, f, 0);
	CFRunLoopAddSource(rl, src, kCFRunLoopDefaultMode);
	CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack |
	    kCFFileDescriptorWriteCallBack);
	before = fd_read_calls;
	int wbefore = fd_write_calls;
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
	if (fd_read_calls != before + 1)
		return fail("regular-file read callback did not fire");
#if defined(__linux__)
	if (fd_write_calls != wbefore + 1)
		return fail("regular-file write callback did not fire");
#endif
	/* One-shot still applies to the stand-in. */
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
	if (fd_read_calls != before + 1)
		return fail("regular-file callback fired again without re-enable");
	CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
	if (fd_read_calls != before + 2)
		return fail("regular-file read did not fire after re-enable");
	CFRunLoopRemoveSource(rl, src, kCFRunLoopDefaultMode);
	CFRelease(src);
	CFFileDescriptorInvalidate(f);
	CFRelease(f);
	unlink(tmp);

	return 0;
}

/* ---- v1-source port ownership -----------------------------------------
 * The run loop must not drain a version-1 source's port: it belongs to
 * the source. A source whose port is a pipe with a byte pending must
 * still find the byte in its perform — the old code read() it away
 * while acknowledging the wakeup.
 */

struct pipe_source {
	int rfd;
	int performed;
	int byte_ok;
};

static __CFPort
pipe_source_get_port(void *info)
{
	struct pipe_source *s = info;
#if defined(__FreeBSD__) || defined(__OpenBSD__)
	/* Same packing CFRunLoop.c uses for pipe ports. */
	return ((__CFPort)(uint32_t)s->rfd << 32) | (uint32_t)s->rfd;
#else
	return s->rfd;
#endif
}

static void
pipe_source_perform(void *info)
{
	struct pipe_source *s = info;
	char c;
	s->performed++;
	s->byte_ok = (read(s->rfd, &c, 1) == 1 && c == 'x');
}

static int
test_source1_port_ownership(void)
{
	int p[2];
	if (pipe(p) != 0)
		return fail("pipe");
	fcntl(p[0], F_SETFL, O_NONBLOCK);

	struct pipe_source s = { p[0], 0, 0 };
	CFRunLoopSourceContext1 ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.version = 1;
	ctx.info = &s;
	ctx.getPort = pipe_source_get_port;
	ctx.perform = pipe_source_perform;
	CFRunLoopSourceRef src = CFRunLoopSourceCreate(NULL, 0,
	    (CFRunLoopSourceContext *)&ctx);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);

	write(p[1], "x", 1);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);

	CFRunLoopRemoveSource(CFRunLoopGetCurrent(), src,
	    kCFRunLoopDefaultMode);
	CFRelease(src);
	close(p[0]);
	close(p[1]);

	if (!s.performed)
		return fail("version-1 source perform never ran");
	if (!s.byte_ok)
		return fail("run loop drained a version-1 source's port");
	return 0;
}

/* ---- many run-loop modes ----------------------------------------------
 * NextBSD's CF packed a global timer ident into each mode's kqueue port
 * and capped it at 16; a 17th mode aborted. Twenty modes with a timer
 * each must all work.
 */

static int
test_many_modes(void)
{
	CFRunLoopRef rl = CFRunLoopGetCurrent();
	for (int i = 0; i < 20; i++) {
		CFStringRef mode = CFStringCreateWithFormat(NULL, NULL,
		    CFSTR("ManyMode%d"), i);
		double fired = 0;
		CFRunLoopTimerContext tctx = { 0, &fired, NULL, NULL, NULL };
		CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL,
		    CFAbsoluteTimeGetCurrent() + 0.02, 0, 0, 0, timer_fired,
		    &tctx);
		CFRunLoopAddTimer(rl, t, mode);
		CFRunLoopRunInMode(mode, 1.0, true);
		CFRunLoopTimerInvalidate(t);
		CFRelease(t);
		CFRelease(mode);
		if (fired == 0)
			return fail("timer in a mode past the 16th did not fire");
	}
	return 0;
}

/* ---- main dispatch queue ----------------------------------------------
 * dispatch_async onto the main queue from a secondary thread must run
 * while the main thread is inside CFRunLoopRunInMode (DISPATCH_COCOA_COMPAT
 * main-queue servicing).
 */

static int main_queue_ran;

static void *
post_to_main(void *arg)
{
	(void)arg;
	usleep(20000);
	dispatch_async(dispatch_get_main_queue(), ^{
		main_queue_ran = 1;
		CFRunLoopStop(CFRunLoopGetMain());
	});
	return NULL;
}

static int
test_dispatch_main_queue(void)
{
	pthread_t thread;
	pthread_create(&thread, NULL, post_to_main, NULL);
	SInt32 r = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);
	pthread_join(thread, NULL);
	if (!main_queue_ran)
		return fail("dispatch_async to main queue never ran");
	if (r != kCFRunLoopRunStopped)
		return fail("main-queue run did not stop after the block");
	return 0;
}

/* ---- CFBundle ----------------------------------------------------------
 * A GNUstep bundle (executable at the root, Resources/Info-gnustep.plist
 * in OpenStep format) is read as a bundle, and Info-gnustep.plist takes
 * precedence over Info.plist on Linux and BSD.
 */

static int
write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fputs(text, f);
	return fclose(f);
}

static int
test_gnustep_bundle(void)
{
	char root[] = "/tmp/cfbundle-test.XXXXXX";
	char path[256];
	if (mkdtemp(root) == NULL)
		return fail("mkdtemp");
	snprintf(path, sizeof(path), "%s/Tool.app", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/Tool.app/Resources", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/Tool.app/Tool", root);
	write_file(path, "");
	snprintf(path, sizeof(path), "%s/Tool.app/Resources/Info-gnustep.plist", root);
	write_file(path, "{ NSExecutable = Tool; CFBundleIdentifier = \"local.test.gnustep\"; }\n");
	snprintf(path, sizeof(path), "%s/Tool.app/Resources/Info.plist", root);
	write_file(path, "{ CFBundleIdentifier = \"local.test.plain\"; }\n");

	snprintf(path, sizeof(path), "%s/Tool.app", root);
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL,
	    (const UInt8 *)path, strlen(path), true);
	CFBundleRef bundle = CFBundleCreate(NULL, url);
	CFRelease(url);
	if (bundle == NULL)
		return fail("CFBundleCreate on a GNUstep bundle returned NULL");
	CFStringRef ident = CFBundleGetIdentifier(bundle);
	if (ident == NULL || !CFEqual(ident, CFSTR("local.test.gnustep")))
		return fail("Info-gnustep.plist not preferred over Info.plist");
	CFURLRef exe = CFBundleCopyExecutableURL(bundle);
	if (exe == NULL)
		return fail("executable named by NSExecutable not found");
	CFStringRef exeName = CFURLCopyLastPathComponent(exe);
	if (!CFEqual(exeName, CFSTR("Tool")))
		return fail("wrong executable URL");
	CFRelease(exeName);
	CFRelease(exe);
	CFRelease(bundle);

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
	system(cmd);
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
	if (test_foreign_bridge_2b() != 0)
		return 1;

	/*
	 * 5. CFFileDescriptor, many modes, main dispatch queue.
	 */
	if (test_file_descriptor() != 0)
		return 1;
	if (test_many_modes() != 0)
		return 1;
	if (test_dispatch_main_queue() != 0)
		return 1;
	if (test_source1_port_ownership() != 0)
		return 1;
	if (test_gnustep_bundle() != 0)
		return 1;

	printf("COREFOUNDATION-OK: CFDictionary + XML/binary plist round-trip and run loop timing succeeded\n");
	return 0;
}
