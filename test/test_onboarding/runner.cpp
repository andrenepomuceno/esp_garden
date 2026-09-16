#include <unity.h>

void run_onboarding_tests(void);

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
    run_onboarding_tests();
    return UNITY_END();
}
