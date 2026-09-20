/* Launchd MachServices job: test.nextbsd.xpc.peer-activation.
 * Incoming messages must wait for peer resume and use its selected queue.
 */
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <xpc/xpc.h>
#include <unistd.h>

static atomic_bool resumed;
static unsigned received;
static char queue_key;

int main(void)
{
    alarm(10);
    const char *service = "test.nextbsd.xpc.peer-activation";
    dispatch_queue_t listener_queue = dispatch_queue_create("test.xpc.listener", NULL);
    dispatch_queue_t peer_queue = dispatch_queue_create("test.xpc.peer", NULL);
    dispatch_queue_set_specific(peer_queue, &queue_key, &queue_key, NULL);
    xpc_connection_t listener = xpc_connection_create_mach_service(service,
        listener_queue, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) return 2;
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        xpc_connection_t connection = (xpc_connection_t)peer;
        xpc_connection_set_target_queue(connection, peer_queue);
        xpc_connection_set_event_handler(connection, ^(xpc_object_t request) {
            if (!atomic_load(&resumed) ||
                dispatch_get_specific(&queue_key) != &queue_key) {
                fprintf(stderr, "FAIL: resumed=%d target=%p expected=%p\n",
                    atomic_load(&resumed), dispatch_get_specific(&queue_key), &queue_key);
                exit(1);
            }
            if (xpc_dictionary_get_int64(request, "request") != ++received) {
                fprintf(stderr, "FAIL: peer messages arrived out of order\n");
                exit(3);
            }
            if (received == 2) {
                puts("XPC-PEER-OK: messages waited for resume and kept target queue order");
                exit(0);
            }
        });
        /* Delay activation while requests are already in flight. */
        usleep(100000);
        atomic_store(&resumed, true);
        xpc_connection_resume(connection);
    });
    xpc_connection_resume(listener);
    xpc_connection_t client = xpc_connection_create_mach_service(service, peer_queue, 0);
    if (!client) return 4;
    xpc_connection_set_event_handler(client, ^(xpc_object_t event) { (void)event; });
    for (int64_t i = 1; i <= 2; i++) {
        xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_int64(request, "request", i);
        xpc_connection_send_message_with_reply(client, request, peer_queue,
            ^(xpc_object_t unused) { (void)unused; });
        xpc_release(request);
    }
    xpc_connection_resume(client);
    dispatch_main();
}
