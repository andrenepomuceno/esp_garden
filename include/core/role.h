#pragma once
#include <stdint.h>

// Ordered: a route guarded by OPERATOR also admits ADMIN.
enum class Role : uint8_t
{
    OPERATOR = 1,
    ADMIN = 2,
};
