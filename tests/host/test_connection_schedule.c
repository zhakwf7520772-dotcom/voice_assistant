#include <assert.h>
#include <stdio.h>
#include "connection_schedule.h"

int main(void)
{
    connection_schedule_t state = {0};
    assert(connection_schedule_due(&state, 0));
    connection_schedule_failed(&state, 1000000);
    assert(!state.ready);
    assert(!connection_schedule_due(&state, 3999999));
    assert(connection_schedule_due(&state, 4000000));
    const unsigned delays[] = {6000, 12000, 24000, 30000, 30000};
    for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); ++i) {
        int64_t now = state.next_check_us;
        connection_schedule_failed(&state, now);
        assert(state.next_check_us - now == (int64_t)delays[i] * 1000);
    }
    int64_t now = state.next_check_us;
    connection_schedule_ready(&state, now);
    assert(state.ready && !connection_schedule_due(&state, now + 29999999));
    assert(connection_schedule_due(&state, now + 30000000));
    // Successful recovery resets backoff; a later disconnect retries promptly.
    connection_schedule_failed(&state, now + 1);
    assert(!state.ready && state.next_check_us == now + 1 + 3000000);
    puts("Connection heartbeat timing, capped reconnect backoff and recovery tests passed");
}
