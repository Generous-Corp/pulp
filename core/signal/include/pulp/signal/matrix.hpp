#pragma once

// Small matrix operations for spatial audio and graphics.
// Supports 2x2, 3x3, and 4x4 float matrices with common operations.

#include <array>
#include <cmath>
#include <cstring>

namespace pulp::signal {

/// NxN matrix stored in row-major order
template<int N>
struct Matrix {
    std::array<float, N * N> data{};

    float& at(int row, int col) { return data[row * N + col]; }
    float at(int row, int col) const { return data[row * N + col]; }

    float& operator()(int row, int col) { return at(row, col); }
    float operator()(int row, int col) const { return at(row, col); }

    /// Identity matrix
    static Matrix identity() {
        Matrix m{};
        for (int i = 0; i < N; ++i)
            m.at(i, i) = 1.0f;
        return m;
    }

    /// Zero matrix
    static Matrix zero() { return Matrix{}; }

    /// Matrix multiply
    Matrix operator*(const Matrix& other) const {
        Matrix result{};
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j)
                for (int k = 0; k < N; ++k)
                    result.at(i, j) += at(i, k) * other.at(k, j);
        return result;
    }

    /// Scalar multiply
    Matrix operator*(float scalar) const {
        Matrix result{};
        for (int i = 0; i < N * N; ++i)
            result.data[i] = data[i] * scalar;
        return result;
    }

    /// Matrix add
    Matrix operator+(const Matrix& other) const {
        Matrix result{};
        for (int i = 0; i < N * N; ++i)
            result.data[i] = data[i] + other.data[i];
        return result;
    }

    /// Transpose
    Matrix transposed() const {
        Matrix result{};
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j)
                result.at(j, i) = at(i, j);
        return result;
    }

    bool operator==(const Matrix& other) const { return data == other.data; }
};

using Matrix2 = Matrix<2>;
using Matrix3 = Matrix<3>;
using Matrix4 = Matrix<4>;

/// Determinant of a 2x2 matrix
inline float determinant(const Matrix2& m) {
    return m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0);
}

/// Determinant of a 3x3 matrix
inline float determinant(const Matrix3& m) {
    return m(0, 0) * (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1))
         - m(0, 1) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0))
         + m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0));
}

/// 4x4 translation matrix
inline Matrix4 translation_matrix(float x, float y, float z) {
    auto m = Matrix4::identity();
    m(0, 3) = x;
    m(1, 3) = y;
    m(2, 3) = z;
    return m;
}

/// 4x4 scale matrix
inline Matrix4 scale_matrix(float sx, float sy, float sz) {
    auto m = Matrix4::identity();
    m(0, 0) = sx;
    m(1, 1) = sy;
    m(2, 2) = sz;
    return m;
}

/// 4x4 rotation around X axis
inline Matrix4 rotation_x(float radians) {
    auto m = Matrix4::identity();
    float c = std::cos(radians), s = std::sin(radians);
    m(1, 1) = c;  m(1, 2) = -s;
    m(2, 1) = s;  m(2, 2) = c;
    return m;
}

/// 4x4 rotation around Y axis
inline Matrix4 rotation_y(float radians) {
    auto m = Matrix4::identity();
    float c = std::cos(radians), s = std::sin(radians);
    m(0, 0) = c;  m(0, 2) = s;
    m(2, 0) = -s; m(2, 2) = c;
    return m;
}

/// 4x4 rotation around Z axis
inline Matrix4 rotation_z(float radians) {
    auto m = Matrix4::identity();
    float c = std::cos(radians), s = std::sin(radians);
    m(0, 0) = c;  m(0, 1) = -s;
    m(1, 0) = s;  m(1, 1) = c;
    return m;
}

/// Transform a 3D vector by a 4x4 matrix (assumes w=1)
struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 transform(const Matrix4& m, const Vec3& v) {
    return {
        m(0, 0) * v.x + m(0, 1) * v.y + m(0, 2) * v.z + m(0, 3),
        m(1, 0) * v.x + m(1, 1) * v.y + m(1, 2) * v.z + m(1, 3),
        m(2, 0) * v.x + m(2, 1) * v.y + m(2, 2) * v.z + m(2, 3)
    };
}

}  // namespace pulp::signal
