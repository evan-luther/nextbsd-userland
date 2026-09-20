/*
 * test_libxpc — Phase H2 smoke check for libxpc.
 *
 * Validates the vendored libxpc (src/libxpc, ported from ravynOS
 * lib/libxpc/) actually links + works at runtime. Stays inside the
 * type-system layer — no connections, no bootstrap, no Mach IPC — so
 * a failure here means the libxpc.so itself is broken, not the lower
 * Mach stack. Connection + bootstrap surface lands in a follow-up
 * test once xpc_connection_create over our libbootstrap is wired.
 *
 * Checks:
 *   1. xpc_dictionary_create with no initial keys returns non-NULL.
 *   2. xpc_dictionary_set_string / _get_string round-trips a value.
 *   3. xpc_dictionary_set_int64 / _get_int64 round-trips an integer.
 *   4. xpc_release on the dictionary doesn't crash.
 *   5. Type tokens are distinct, so error/type validation is meaningful.
 *
 * Exit codes:
 *   0 — all checks pass
 *   1 — xpc_dictionary_create returned NULL
 *   2 — string round-trip mismatch
 *   3 — int64 round-trip mismatch
 *   4 — type identity mismatch
 */
#include <stdio.h>
#include <string.h>
#include <xpc/xpc.h>

int
main(void)
{
	xpc_object_t d = xpc_dictionary_create(NULL, NULL, 0);
	if (d == NULL) {
		printf("FAIL: xpc_dictionary_create returned NULL\n");
		return 1;
	}
	xpc_type_t types[] = {
		XPC_TYPE_ARRAY, XPC_TYPE_BOOL, XPC_TYPE_CONNECTION, XPC_TYPE_DATA,
		XPC_TYPE_DATE, XPC_TYPE_DICTIONARY, XPC_TYPE_ENDPOINT, XPC_TYPE_NULL,
		XPC_TYPE_ERROR, XPC_TYPE_FD, XPC_TYPE_INT64, XPC_TYPE_UINT64,
		XPC_TYPE_SHMEM, XPC_TYPE_STRING, XPC_TYPE_UUID, XPC_TYPE_DOUBLE
	};
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		for (size_t j = i + 1; j < sizeof(types) / sizeof(types[0]); j++) {
			if (types[i] == types[j]) {
				printf("FAIL: XPC type tokens %zu and %zu alias\n", i, j);
				xpc_release(d);
				return 4;
			}
		}
	}
	if (xpc_get_type(d) != XPC_TYPE_DICTIONARY ||
	    xpc_get_type(d) == XPC_TYPE_ERROR) {
		printf("FAIL: dictionary cannot be distinguished from an error\n");
		xpc_release(d);
		return 4;
	}

	xpc_dictionary_set_string(d, "k_str", "hello-xpc");
	const char *s = xpc_dictionary_get_string(d, "k_str");
	if (s == NULL || strcmp(s, "hello-xpc") != 0) {
		printf("FAIL: string round-trip: got '%s' want 'hello-xpc'\n",
		    s ? s : "(null)");
		xpc_release(d);
		return 2;
	}

	xpc_dictionary_set_int64(d, "k_int", 0x7fffffffabcd1234LL);
	int64_t v = xpc_dictionary_get_int64(d, "k_int");
	if (v != 0x7fffffffabcd1234LL) {
		printf("FAIL: int64 round-trip: got 0x%llx want 0x7fffffffabcd1234\n",
		    (unsigned long long)v);
		xpc_release(d);
		return 3;
	}

	xpc_release(d);
	printf("LIBXPC-OK: dictionary string + int64 round-trip succeeded\n");
	return 0;
}
