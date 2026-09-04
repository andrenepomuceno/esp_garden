#include <unity.h>

void run_session_slots_tests(void);
void reset_session_slots_state(void);

// The drop counter is shared state. Resetting it here rather than inside the
// tests that happen to look at it is what keeps the suite independent of the
// order Unity runs it in.
void
setUp(void)
{
    reset_session_slots_state();
}

void
tearDown(void)
{
}

int
main(int, char**)
{
    UNITY_BEGIN();
    run_session_slots_tests();
    return UNITY_END();
}
