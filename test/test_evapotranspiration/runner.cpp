#include <unity.h>

void run_evapotranspiration_tests(void);

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
    run_evapotranspiration_tests();
    return UNITY_END();
}
