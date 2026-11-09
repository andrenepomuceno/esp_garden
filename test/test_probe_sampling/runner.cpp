#include <unity.h>

void run_probe_sampling_tests(void);

void
setUp(void)
{
}

void
tearDown(void)
{
}

int
main(int, char**)
{
    UNITY_BEGIN();
    run_probe_sampling_tests();
    return UNITY_END();
}
