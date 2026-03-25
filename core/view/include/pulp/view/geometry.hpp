#pragma once

#include <algorithm>
#include <cmath>

namespace pulp::view {

struct Point {
    float x = 0, y = 0;

    Point operator+(const Point& p) const { return {x + p.x, y + p.y}; }
    Point operator-(const Point& p) const { return {x - p.x, y - p.y}; }
    bool operator==(const Point& p) const { return x == p.x && y == p.y; }
};

struct Size {
    float width = 0, height = 0;

    bool operator==(const Size& s) const { return width == s.width && height == s.height; }
    bool is_empty() const { return width <= 0 || height <= 0; }
};

struct Rect {
    float x = 0, y = 0, width = 0, height = 0;

    float right() const { return x + width; }
    float bottom() const { return y + height; }
    Point origin() const { return {x, y}; }
    Size size() const { return {width, height}; }
    Point center() const { return {x + width * 0.5f, y + height * 0.5f}; }

    bool contains(Point p) const {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }

    bool is_empty() const { return width <= 0 || height <= 0; }

    Rect inset(float amount) const {
        return {x + amount, y + amount,
                std::max(0.0f, width - 2 * amount),
                std::max(0.0f, height - 2 * amount)};
    }

    Rect inset(float h, float v) const {
        return {x + h, y + v,
                std::max(0.0f, width - 2 * h),
                std::max(0.0f, height - 2 * v)};
    }

    bool operator==(const Rect& r) const {
        return x == r.x && y == r.y && width == r.width && height == r.height;
    }
};

// Flex layout direction
enum class FlexDirection { row, column };

// Flex alignment
enum class FlexAlign { start, center, end, stretch };

// Flex layout properties for a view
struct FlexStyle {
    FlexDirection direction = FlexDirection::column;
    FlexAlign align_items = FlexAlign::stretch;
    FlexAlign justify_content = FlexAlign::start;
    float gap = 0;
    float padding = 0;
    float flex_grow = 0;    // 0 = fixed size, >0 = share remaining space
    float min_width = 0;
    float min_height = 0;
    float preferred_width = 0;
    float preferred_height = 0;
};

} // namespace pulp::view
