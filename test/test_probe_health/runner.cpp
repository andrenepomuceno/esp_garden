#include <unity.h>

void run_probe_health_tests(void);

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
    run_probe_health_tests();
    return UNITY_END();
}
