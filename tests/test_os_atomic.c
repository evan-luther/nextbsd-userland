/* Build once for each freebsd-shims include directory (Libnotify and syslog). */
#include <assert.h>
#include <os/atomic_private.h>
#include <stdio.h>

int main(void)
{
    atomic_uint refs = 1;
    assert(os_atomic_dec(&refs, release) == 0);
    assert(atomic_load(&refs) == 0);
    assert(os_atomic_inc(&refs, relaxed) == 1);
    unsigned operand = 3;
    assert(os_atomic_add(&refs, operand++, relaxed) == 4);
    assert(operand == 4);
    assert(os_atomic_sub(&refs, 2, relaxed) == 2);
    assert(os_atomic_or(&refs, 4, relaxed) == 6);
    assert(os_atomic_and(&refs, 3, relaxed) == 2);
    assert(os_atomic_xor(&refs, 3, relaxed) == 1);
    assert(os_atomic_xchg(&refs, 7, relaxed) == 1);
    assert(atomic_load(&refs) == 7);
    puts("OS-ATOMIC-OK: updates return new values; exchange returns old value");
    return 0;
}
