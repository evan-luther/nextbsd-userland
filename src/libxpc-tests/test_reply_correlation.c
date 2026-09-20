/* Run as a launchd job with MachServices: test.nextbsd.xpc.reply-correlation.
 * The server deliberately replies to request 2 before request 1.
 */
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <xpc/xpc.h>

static xpc_object_t first_reply;
static unsigned replies;

static void check_reply(xpc_object_t reply, int64_t expected)
{
    if (!reply || xpc_dictionary_get_int64(reply, "request") != expected) {
        fprintf(stderr, "FAIL: reply delivered to the wrong request handler\n");
        exit(1);
    }
    if (++replies == 2) {
        puts("XPC-REPLY-OK: out-of-order replies reached their request handlers");
        exit(0);
    }
}

int main(void)
{
    const char *service = "test.nextbsd.xpc.reply-correlation";
    /* The main queue avoids the independent first-peer initialization race. */
    xpc_connection_t listener = xpc_connection_create_mach_service(service,
        dispatch_get_main_queue(), XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) return 2;
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        xpc_connection_t connection = (xpc_connection_t)peer;
        xpc_connection_set_event_handler(connection, ^(xpc_object_t request) {
            xpc_object_t reply = xpc_dictionary_create_reply(request);
            if (!reply) exit(3);
            int64_t value = xpc_dictionary_get_int64(request, "request");
            xpc_dictionary_set_int64(reply, "request", value);
            if (value == 1) {
                first_reply = reply;
            } else {
                if (!first_reply) exit(4);
                xpc_connection_send_message(connection, reply);
                xpc_connection_send_message(connection, first_reply);
                xpc_release(reply);
                xpc_release(first_reply);
                first_reply = NULL;
            }
        });
        xpc_connection_resume(connection);
    });
    xpc_connection_resume(listener);

    dispatch_queue_t queue = dispatch_queue_create("test.xpc.replies", NULL);
    xpc_connection_t client = xpc_connection_create_mach_service(service, queue, 0);
    if (!client) return 5;
    xpc_connection_set_event_handler(client, ^(xpc_object_t event) {
        (void)event;
        fprintf(stderr, "FAIL: reply was not correlated to a pending request\n");
        exit(6);
    });
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_int64(request, "request", 1);
    xpc_connection_send_message_with_reply(client, request, queue,
        ^(xpc_object_t reply) { check_reply(reply, 1); });
    xpc_release(request);
    request = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_int64(request, "request", 2);
    xpc_connection_send_message_with_reply(client, request, queue,
        ^(xpc_object_t reply) { check_reply(reply, 2); });
    xpc_release(request);
    xpc_connection_resume(client);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC),
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            fprintf(stderr, "FAIL: timed out waiting for correlated replies\n");
            exit(7);
        });
    dispatch_main();
}
