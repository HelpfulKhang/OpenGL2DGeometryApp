#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <vector>
#include <cmath>
#include <map>
#include <string>
#include <glad/glad.h>
#include "shader.h"

struct Vec2 { float x, y; };
struct Color { float r, g, b; };

class GeometryRenderer {
public:
    GeometryRenderer(Shader &shader, float left = -1.0f, float right = 1.0f, float bottom = -1.0f, float top = 1.0f)
        : shader(shader), left(left), right(right), bottom(bottom), top(top)
    {
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);

        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }

    ~GeometryRenderer() {
        glDeleteBuffers(1, &VBO);
        glDeleteVertexArrays(1, &VAO);
    }

    void setView(float l, float r, float b, float t) {
        left = l; right = r; bottom = b; top = t;
    }
    void getView(float &l, float &r, float &b, float &t) const {
        l = left; r = right; b = bottom; t = top;
    }

    void drawPoint(const Vec2 &p, const Color &c, float size = 5.0f) {
        shader.use();
        glPointSize(size);
        auto verts = buildVertexBuffer({p}, c);
        uploadAndDraw(verts, GL_POINTS);
        glPointSize(1.0f);
    }

    void drawLine(const Vec2 &a, const Vec2 &b, const Color &c) {
        shader.use();
        auto verts = buildVertexBuffer({a, b}, c);
        uploadAndDraw(verts, GL_LINES);
    }

    void drawPolyline(const std::vector<Vec2> &pts, const Color &c) {
        shader.use();
        auto verts = buildVertexBuffer(pts, c);
        uploadAndDraw(verts, GL_LINE_STRIP);
    }

    void drawCircle(const Vec2 &center, float radius, const Color &c, int segments = 64) {
        shader.use();
        std::vector<Vec2> pts;
        pts.reserve(segments);
        for (int i = 0; i < segments; ++i) {
            float theta = 2.0f * 3.14159265358979323846f * float(i) / float(segments);
            pts.push_back({ center.x + radius * cosf(theta), center.y + radius * sinf(theta) });
        }
        auto verts = buildVertexBuffer(pts, c);
        uploadAndDraw(verts, GL_LINE_LOOP);
    }

    void drawEllipse(const Vec2 &center, float a, float b, float angleRad, const Color &c, int segments = 128) {
        shader.use();
        std::vector<Vec2> pts; pts.reserve(segments);
        float cosA = cosf(angleRad), sinA = sinf(angleRad);
        for (int i = 0; i < segments; ++i) {
            float t = 2.0f * 3.14159265358979323846f * float(i) / float(segments);
            float x = a * cosf(t), y = b * sinf(t);
            float xr = x * cosA - y * sinA;
            float yr = x * sinA + y * cosA;
            pts.push_back({ center.x + xr, center.y + yr });
        }
        auto verts = buildVertexBuffer(pts, c);
        uploadAndDraw(verts, GL_LINE_LOOP);
    }

    void drawParabola(float k, float xmin, float xmax, const Color &c, int samples = 200) {
        shader.use();
        std::vector<Vec2> pts; pts.reserve(samples);
        for (int i = 0; i < samples; ++i) {
            float t = float(i) / float(samples - 1);
            float x = xmin + (xmax - xmin) * t;
            float y = k * x * x;
            pts.push_back({ x, y });
        }
        auto verts = buildVertexBuffer(pts, c);
        uploadAndDraw(verts, GL_LINE_STRIP);
    }

    void drawHyperbola(float a, float b, float tmin, float tmax, const Color &c, int samples = 200) {
        shader.use();
        std::vector<Vec2> pts; pts.reserve(samples);
        for (int i = 0; i < samples; ++i) {
            float s = float(i) / float(samples - 1);
            float t = tmin + (tmax - tmin) * s;
            float x = a * std::cosh(t);
            float y = b * std::sinh(t);
            pts.push_back({ x, y });
        }
        uploadAndDraw(buildVertexBuffer(pts, c), GL_LINE_STRIP);
        pts.clear();
        for (int i = 0; i < samples; ++i) {
            float s = float(i) / float(samples - 1);
            float t = tmin + (tmax - tmin) * s;
            float x = -a * std::cosh(t);
            float y = b * std::sinh(t);
            pts.push_back({ x, y });
        }
        uploadAndDraw(buildVertexBuffer(pts, c), GL_LINE_STRIP);
    }

    // draw grid + axes, with axis more pronounced
    void drawGrid(float spacing, const Color &colorGrid, const Color &colorAxis) {
        shader.use();

        glLineWidth(1.0f);
        std::vector<Vec2> lines;
        lines.clear();
        float startX = std::floor(left / spacing) * spacing;
        float endX = std::ceil(right / spacing) * spacing;
        for (float x = startX; x <= endX; x += spacing) {
            lines.push_back({ x, bottom });
            lines.push_back({ x, top });
        }
        uploadAndDraw(buildVertexBuffer(lines, colorGrid), GL_LINES);
        lines.clear();

        float startY = std::floor(bottom / spacing) * spacing;
        float endY = std::ceil(top / spacing) * spacing;
        for (float y = startY; y <= endY; y += spacing) {
            lines.push_back({ left, y });
            lines.push_back({ right, y });
        }
        uploadAndDraw(buildVertexBuffer(lines, colorGrid), GL_LINES);
        lines.clear();

        // draw axes thicker and brighter
        glLineWidth(3.5f);
        if (left <= 0.0f && right >= 0.0f) {
            lines.push_back({ 0.0f, bottom });
            lines.push_back({ 0.0f, top });
        }
        if (bottom <= 0.0f && top >= 0.0f) {
            lines.push_back({ left, 0.0f });
            lines.push_back({ right, 0.0f });
        }
        if (!lines.empty()) uploadAndDraw(buildVertexBuffer(lines, colorAxis), GL_LINES);
        glLineWidth(1.0f);
    }

    // simple stroke-text: supports digits 0-9, '-' and '.'
    void drawText(const Vec2 &pos, const std::string &text, float charHeight, const Color &c) {
        // build stroke definitions on first use
        static std::map<char, std::vector<std::vector<Vec2>>> strokes;
        if (strokes.empty()) {
            // coordinate system for chars: x in [0,1], y in [0,1]
            // '0'
            strokes['0'] = { {{0.1f,0.1f},{0.9f,0.1f},{0.9f,0.9f},{0.1f,0.9f},{0.1f,0.1f}} };
            strokes['1'] = { {{0.5f,0.1f},{0.5f,0.9f}} };
            strokes['2'] = { {{0.1f,0.9f},{0.9f,0.9f},{0.9f,0.5f},{0.1f,0.1f},{0.9f,0.1f}} };
            strokes['3'] = { {{0.1f,0.9f},{0.9f,0.9f},{0.5f,0.5f},{0.9f,0.5f},{0.9f,0.1f},{0.1f,0.1f}} };
            strokes['4'] = { {{0.1f,0.9f},{0.1f,0.5f},{0.9f,0.5f},{0.9f,0.9f}}, {{0.9f,0.1f},{0.9f,0.5f}} };
            strokes['5'] = { {{0.9f,0.9f},{0.1f,0.9f},{0.1f,0.5f},{0.9f,0.5f},{0.9f,0.1f},{0.1f,0.1f}} };
            strokes['6'] = { {{0.9f,0.9f},{0.1f,0.5f},{0.1f,0.1f},{0.9f,0.1f},{0.9f,0.5f},{0.1f,0.5f}} };
            strokes['7'] = { {{0.1f,0.9f},{0.9f,0.9f},{0.5f,0.1f}} };
            strokes['8'] = { {{0.5f,0.5f},{0.1f,0.9f},{0.9f,0.9f},{0.1f,0.1f},{0.9f,0.1f},{0.5f,0.5f}} };
            strokes['9'] = { {{0.9f,0.1f},{0.9f,0.9f},{0.1f,0.9f},{0.1f,0.5f},{0.9f,0.5f}} };
            strokes['-'] = { {{0.1f,0.5f},{0.9f,0.5f}} };
            strokes['.'] = { {{0.8f,0.1f},{0.85f,0.1f}} }; // small dot as tiny segment
        }

        // sizing: charHeight is world-space height; width ~ 0.6 * height
        float charW = charHeight * 0.6f;
        float spacing = charW * 0.18f;

        // for each character and each stroke, build stroke points in world coords and draw
        float xCursor = pos.x;
        for (char ch : text) {
            auto it = strokes.find(ch);
            if (it == strokes.end()) {
                xCursor += charW + spacing; // unknown char -> advance
                continue;
            }
            for (const auto &stroke : it->second) {
                std::vector<Vec2> strokeWorld;
                strokeWorld.reserve(stroke.size());
                for (const Vec2 &pt : stroke) {
                    float wx = xCursor + pt.x * charW;
                    float wy = pos.y + pt.y * charHeight;
                    strokeWorld.push_back({ wx, wy });
                }
                uploadAndDraw(buildVertexBuffer(strokeWorld, c), GL_LINE_STRIP);
            }
            xCursor += charW + spacing;
        }
    }

private:
    Shader &shader;
    GLuint VAO = 0;
    GLuint VBO = 0;
    float left, right, bottom, top;

    inline float worldToNDCx(float x) const {
        return (2.0f * (x - left) / (right - left) - 1.0f);
    }
    inline float worldToNDCy(float y) const {
        return (2.0f * (y - bottom) / (top - bottom) - 1.0f);
    }

    std::vector<float> buildVertexBuffer(const std::vector<Vec2> &pts, const Color &c) const {
        std::vector<float> verts;
        verts.reserve(pts.size() * 6);
        for (const auto &p : pts) {
            float nx = worldToNDCx(p.x);
            float ny = worldToNDCy(p.y);
            verts.push_back(nx);
            verts.push_back(ny);
            verts.push_back(0.0f);
            verts.push_back(c.r);
            verts.push_back(c.g);
            verts.push_back(c.b);
        }
        return verts;
    }

    void uploadAndDraw(const std::vector<float> &verts, GLenum mode) {
        if (verts.empty()) return;
        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(mode, 0, (GLsizei)(verts.size() / 6));
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }
};

#endif // GEOMETRY_H