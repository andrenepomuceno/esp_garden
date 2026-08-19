#include <unity.h>

void run_accumulator_tests(void);

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
    run_accumulator_tests();
    return UNITY_END();
}
