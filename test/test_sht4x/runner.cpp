#include <unity.h>

void run_sht4x_tests(void);

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
    run_sht4x_tests();
    return UNITY_END();
}
