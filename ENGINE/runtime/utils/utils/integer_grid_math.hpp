#pragma once

namespace vibble::math {

inline int floor_div(int numerator, int denominator) {
    if (denominator == 0) {
        return 0;
    }
    int quotient = numerator / denominator;
    int remainder = numerator % denominator;
    if (remainder != 0 && ((remainder > 0) != (denominator > 0))) {
        --quotient;
    }
    return quotient;
}

} // namespace vibble::math

