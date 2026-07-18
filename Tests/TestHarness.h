#pragma once

#include <cmath>
#include <iostream>
#include <string>

class TestHarness
{
public:
    void expect(bool condition, const std::string& description)
    {
        if (condition)
            return;
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }

    template <typename Left, typename Right>
    void expectEqual(const Left& left, const Right& right, const std::string& description)
    {
        expect(left == right, description);
    }

    void expectNear(double left, double right, double tolerance, const std::string& description)
    {
        expect(std::abs(left - right) <= tolerance, description);
    }

    [[nodiscard]] int result() const
    {
        if (failures == 0)
            std::cout << "All tests passed\n";
        return failures == 0 ? 0 : 1;
    }

private:
    int failures {};
};
