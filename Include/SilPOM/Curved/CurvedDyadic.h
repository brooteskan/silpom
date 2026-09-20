// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace SilPOM::Curved::Exact
{
// Exact binary64 arithmetic shared by the independent resultant oracle and
// the restricted rational-root certificate. No root-finding code is shared.
class Integer
{
public:
    Integer() = default;
    explicit Integer(std::int64_t value)
    {
        if (value < 0)
        {
            m_negative = true;
            const std::uint64_t magnitude = std::uint64_t(-(value + 1)) + 1;
            m_words = { std::uint32_t(magnitude), std::uint32_t(magnitude >> 32) };
        }
        else
        {
            const std::uint64_t magnitude = std::uint64_t(value);
            m_words = { std::uint32_t(magnitude), std::uint32_t(magnitude >> 32) };
        }
        Normalize();
    }

    static Integer Unsigned(std::uint64_t value)
    {
        Integer result;
        result.m_words = { std::uint32_t(value), std::uint32_t(value >> 32) };
        result.Normalize();
        return result;
    }

    bool IsZero() const { return m_words.empty(); }
    int Sign() const { return IsZero() ? 0 : (m_negative ? -1 : 1); }
    bool Negative() const { return m_negative && !IsZero(); }
    size_t WordCount() const { return m_words.size(); }

    Integer operator-() const
    {
        Integer result = *this;
        if (!result.IsZero())
        {
            result.m_negative = !result.m_negative;
        }
        return result;
    }

    friend Integer operator+(const Integer& a, const Integer& b)
    {
        if (a.m_negative == b.m_negative)
        {
            Integer result = AddMagnitude(a, b);
            result.m_negative = a.m_negative;
            return result;
        }
        const int comparison = CompareMagnitude(a, b);
        if (comparison == 0)
        {
            return {};
        }
        Integer result = comparison > 0 ? SubtractMagnitude(a, b) : SubtractMagnitude(b, a);
        result.m_negative = comparison > 0 ? a.m_negative : b.m_negative;
        return result;
    }

    friend Integer operator-(const Integer& a, const Integer& b) { return a + (-b); }

    friend Integer operator*(const Integer& a, const Integer& b)
    {
        if (a.IsZero() || b.IsZero())
        {
            return {};
        }
        Integer result;
        result.m_negative = a.m_negative != b.m_negative;
        result.m_words.assign(a.m_words.size() + b.m_words.size(), 0);
        for (size_t i = 0; i != a.m_words.size(); ++i)
        {
            std::uint64_t carry = 0;
            for (size_t j = 0; j != b.m_words.size(); ++j)
            {
                const size_t index = i + j;
                const std::uint64_t product = std::uint64_t(a.m_words[i]) * b.m_words[j] +
                    result.m_words[index] + carry;
                result.m_words[index] = std::uint32_t(product);
                carry = product >> 32;
            }
            size_t index = i + b.m_words.size();
            while (carry != 0)
            {
                if (index == result.m_words.size())
                {
                    result.m_words.push_back(0);
                }
                const std::uint64_t sum = std::uint64_t(result.m_words[index]) + carry;
                result.m_words[index++] = std::uint32_t(sum);
                carry = sum >> 32;
            }
        }
        result.Normalize();
        return result;
    }

    Integer ShiftedLeft(unsigned bits) const
    {
        if (IsZero() || bits == 0)
        {
            return *this;
        }
        Integer result;
        result.m_negative = m_negative;
        const size_t wordShift = bits / 32;
        const unsigned bitShift = bits % 32;
        result.m_words.assign(wordShift, 0);
        std::uint64_t carry = 0;
        for (std::uint32_t word : m_words)
        {
            const std::uint64_t value = (std::uint64_t(word) << bitShift) | carry;
            result.m_words.push_back(std::uint32_t(value));
            carry = value >> 32;
        }
        if (carry != 0)
        {
            result.m_words.push_back(std::uint32_t(carry));
        }
        return result;
    }

    void ShiftRightExact(unsigned bits)
    {
        if (IsZero() || bits == 0)
        {
            return;
        }
        const size_t wordShift = bits / 32;
        const unsigned bitShift = bits % 32;
        if (wordShift != 0)
        {
            m_words.erase(m_words.begin(), m_words.begin() + std::ptrdiff_t(wordShift));
        }
        if (bitShift != 0)
        {
            std::uint32_t carry = 0;
            for (size_t i = m_words.size(); i-- > 0;)
            {
                const std::uint32_t next = m_words[i] << (32 - bitShift);
                m_words[i] = (m_words[i] >> bitShift) | carry;
                carry = next;
            }
        }
        Normalize();
    }

    unsigned TrailingZeroBits() const
    {
        if (IsZero())
        {
            return 0;
        }
        unsigned result = 0;
        for (std::uint32_t word : m_words)
        {
            if (word == 0)
            {
                result += 32;
                continue;
            }
#if defined(_MSC_VER)
            unsigned long index = 0;
            _BitScanForward(&index, word);
            result += unsigned(index);
#else
            result += unsigned(__builtin_ctz(word));
#endif
            break;
        }
        return result;
    }

    long double ToLongDouble() const
    {
        long double result = 0.0L;
        for (size_t i = m_words.size(); i-- > 0;)
        {
            result = std::ldexp(result, 32) + m_words[i];
        }
        return m_negative ? -result : result;
    }

private:
    static int CompareMagnitude(const Integer& a, const Integer& b)
    {
        if (a.m_words.size() != b.m_words.size())
        {
            return a.m_words.size() < b.m_words.size() ? -1 : 1;
        }
        for (size_t i = a.m_words.size(); i-- > 0;)
        {
            if (a.m_words[i] != b.m_words[i])
            {
                return a.m_words[i] < b.m_words[i] ? -1 : 1;
            }
        }
        return 0;
    }

    static Integer AddMagnitude(const Integer& a, const Integer& b)
    {
        Integer result;
        const size_t count = std::max(a.m_words.size(), b.m_words.size());
        result.m_words.resize(count);
        std::uint64_t carry = 0;
        for (size_t i = 0; i != count; ++i)
        {
            const std::uint64_t sum = carry + (i < a.m_words.size() ? a.m_words[i] : 0) +
                (i < b.m_words.size() ? b.m_words[i] : 0);
            result.m_words[i] = std::uint32_t(sum);
            carry = sum >> 32;
        }
        if (carry != 0)
        {
            result.m_words.push_back(std::uint32_t(carry));
        }
        return result;
    }

    static Integer SubtractMagnitude(const Integer& larger, const Integer& smaller)
    {
        Integer result;
        result.m_words.resize(larger.m_words.size());
        std::uint64_t borrow = 0;
        for (size_t i = 0; i != larger.m_words.size(); ++i)
        {
            const std::uint64_t left = larger.m_words[i];
            const std::uint64_t right = (i < smaller.m_words.size() ? smaller.m_words[i] : 0) + borrow;
            result.m_words[i] = std::uint32_t(left - right);
            borrow = left < right ? 1 : 0;
        }
        result.Normalize();
        return result;
    }

    void Normalize()
    {
        while (!m_words.empty() && m_words.back() == 0)
        {
            m_words.pop_back();
        }
        if (m_words.empty())
        {
            m_negative = false;
        }
    }

    bool m_negative = false;
    std::vector<std::uint32_t> m_words;
};

struct Dyadic
{
    Integer numerator{};
    int exponent = 0;

    Dyadic() = default;
    Dyadic(Integer value, int power = 0) : numerator(std::move(value)), exponent(power) { Normalize(); }
    explicit Dyadic(double value)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const bool negative = (bits >> 63) != 0;
        const unsigned encodedExponent = unsigned((bits >> 52) & 0x7ffu);
        std::uint64_t significand = bits & ((std::uint64_t(1) << 52) - 1);
        if (encodedExponent == 0)
        {
            exponent = -1074;
        }
        else
        {
            significand |= std::uint64_t(1) << 52;
            exponent = int(encodedExponent) - 1023 - 52;
        }
        numerator = Integer::Unsigned(significand);
        if (negative)
        {
            numerator = -numerator;
        }
        Normalize();
    }

    int Sign() const { return numerator.Sign(); }
    bool IsZero() const { return numerator.IsZero(); }
    double ToDouble() const { return double(std::ldexp(numerator.ToLongDouble(), exponent)); }

    void Normalize()
    {
        if (numerator.IsZero())
        {
            exponent = 0;
            return;
        }
        const unsigned zeros = numerator.TrailingZeroBits();
        numerator.ShiftRightExact(zeros);
        exponent += int(zeros);
    }

    friend Dyadic operator+(const Dyadic& a, const Dyadic& b)
    {
        if (a.IsZero()) return b;
        if (b.IsZero()) return a;
        const int common = std::min(a.exponent, b.exponent);
        return { a.numerator.ShiftedLeft(unsigned(a.exponent - common)) +
            b.numerator.ShiftedLeft(unsigned(b.exponent - common)), common };
    }
    friend Dyadic operator-(const Dyadic& a, const Dyadic& b) { return a + Dyadic(-b.numerator, b.exponent); }
    friend Dyadic operator-(const Dyadic& value) { return { -value.numerator, value.exponent }; }
    friend Dyadic operator*(const Dyadic& a, const Dyadic& b)
    {
        return { a.numerator * b.numerator, a.exponent + b.exponent };
    }
};

}
