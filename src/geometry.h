#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <vector>
#include <cmath>
#include <map>
#include <string>
#include <glad/glad.h>
#include "shader.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_glfw.h"

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

    // [Trong file src/geometry.h]

    // Sửa lại signature hàm để nhận thêm 2 biến bool: showGridLines, showAxisLines
    void drawGrid(float spacing, const Color &colorGrid, const Color &colorAxis, bool showGridLines, bool showAxisLines) {
        shader.use();

        // 1. Vẽ lưới (Grid Lines)
        if (showGridLines) {
            glLineWidth(1.0f);
            std::vector<Vec2> lines;
            
            // Vẽ các đường dọc
            float startX = std::floor(left / spacing) * spacing;
            float endX = std::ceil(right / spacing) * spacing;
            for (float x = startX; x <= endX; x += spacing) {
                lines.push_back({ x, bottom });
                lines.push_back({ x, top });
            }
            uploadAndDraw(buildVertexBuffer(lines, colorGrid), GL_LINES);
            lines.clear();

            // Vẽ các đường ngang
            float startY = std::floor(bottom / spacing) * spacing;
            float endY = std::ceil(top / spacing) * spacing;
            for (float y = startY; y <= endY; y += spacing) {
                lines.push_back({ left, y });
                lines.push_back({ right, y });
            }
            uploadAndDraw(buildVertexBuffer(lines, colorGrid), GL_LINES);
            lines.clear();
        }

        // 2. Vẽ trục (Axis Lines)
        if (showAxisLines) {
            glLineWidth(3.5f); // Nét đậm cho trục
            std::vector<Vec2> axisLines;
            
            // Trục Y (x = 0)
            if (left <= 0.0f && right >= 0.0f) {
                axisLines.push_back({ 0.0f, bottom });
                axisLines.push_back({ 0.0f, top });
            }
            // Trục X (y = 0)
            if (bottom <= 0.0f && top >= 0.0f) {
                axisLines.push_back({ left, 0.0f });
                axisLines.push_back({ right, 0.0f });
            }
            if (!axisLines.empty()) uploadAndDraw(buildVertexBuffer(axisLines, colorAxis), GL_LINES);
            glLineWidth(1.0f); // Reset lại độ dày nét
        }
    }

    void drawText(const std::string& text, float x, float y, const Color& color) {
        // 1. An toàn: Nếu chưa load font, dùng font mặc định thay vì crash app
        ImFont* fontToUse = font ? font : ImGui::GetFont(); 
        
        // 2. Chuyển đổi màu từ float (0.0-1.0) sang U32 (0-255) cho ImGui
        ImU32 col32 = IM_COL32((int)(color.r * 255), (int)(color.g * 255), (int)(color.b * 255), 255);

        ImGui::PushFont(fontToUse);
        
        // 3. Dùng GetForegroundDrawList để vẽ đè lên mọi thứ tại toạ độ màn hình (Screen Coordinates)
        // Lưu ý: x, y ở đây phải là toạ độ PIXEL trên màn hình cửa sổ
        ImGui::GetForegroundDrawList()->AddText(ImVec2(x, y), col32, text.c_str());

        ImGui::PopFont();
    }

private:
    Shader &shader;
    float left, right, bottom, top;
    GLuint VAO, VBO;
    ImFont* font;

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