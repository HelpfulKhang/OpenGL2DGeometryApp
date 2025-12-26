#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <filesystem>

#include "shader.h"
#include "geometry.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "imgui_stdlib.h"
#include <set>

namespace fs = std::filesystem;

// Biến toàn cục quản lý chuột
static bool dragging = false;     // Panning màn hình
static int draggingPointIdx = -1; // Index điểm đang bị kéo (nếu có)
static double lastX = 0.0, lastY = 0.0;

// ---- Forward declarations ----
void canvas_framebuffer_size_callback(GLFWwindow *window, int width, int height);
void canvas_processInput(GLFWwindow *window);
void canvas_scroll_callback(GLFWwindow *window, double xoffset, double yoffset);
void canvas_mouse_button_callback(GLFWwindow *window, int button, int action, int mods);
void canvas_cursor_position_callback(GLFWwindow *window, double xpos, double ypos);
void canvas_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods);

// ---- Math Helpers ----
float distSq(Vec2 p1, Vec2 p2)
{
    return (p1.x - p2.x) * (p1.x - p2.x) + (p1.y - p2.y) * (p1.y - p2.y);
}

float dist(Vec2 p1, Vec2 p2)
{
    return std::sqrt(distSq(p1, p2));
}

// Tính góc giữa 2 đường thẳng (0 đến 180 độ)
float getAngleBetweenLines(Vec2 a1, Vec2 b1, Vec2 a2, Vec2 b2) {
    Vec2 v1 = { b1.x - a1.x, b1.y - a1.y };
    Vec2 v2 = { b2.x - a2.x, b2.y - a2.y };
    float dot = v1.x * v2.x + v1.y * v2.y;
    float mag1 = std::sqrt(v1.x * v1.x + v1.y * v1.y);
    float mag2 = std::sqrt(v2.x * v2.x + v2.y * v2.y);
    
    if (mag1 < 1e-6f || mag2 < 1e-6f) return 0.0f;
    
    // cos(theta) = |v1.v2| / (|v1|.|v2|) -> Lấy trị tuyệt đối để luôn có góc nhọn/vuông (0-90) 
    // Nhưng đề bài yêu cầu 0-180, thường trong hình học phẳng ta lấy góc không tù:
    float cosTheta = std::abs(dot) / (mag1 * mag2);
    if (cosTheta > 1.0f) cosTheta = 1.0f;
    return std::acos(cosTheta) * 180.0f / 3.14159265f;
}

// Quay điểm quanh tâm
Vec2 rotatePoint(Vec2 p, Vec2 center, float angleDeg) {
    float rad = angleDeg * 3.14159265f / 180.0f;
    float s = std::sin(rad);
    float c = std::cos(rad);
    // Tịnh tiến về tâm O(0,0)
    float x = p.x - center.x;
    float y = p.y - center.y;
    // Áp dụng ma trận quay và tịnh tiến ngược lại
    return { x * c - y * s + center.x, x * s + y * c + center.y };
}

// Khoảng cách từ điểm p đến đoạn thẳng ab
float distToSegment(Vec2 p, Vec2 a, Vec2 b)
{
    Vec2 ab = {b.x - a.x, b.y - a.y};
    Vec2 ap = {p.x - a.x, p.y - a.y};
    float l2 = ab.x * ab.x + ab.y * ab.y;
    if (l2 == 0.0f)
        return std::sqrt(distSq(p, a));
    float t = (ap.x * ab.x + ap.y * ab.y) / l2;
    t = std::max(0.0f, std::min(1.0f, t));
    Vec2 projection = {a.x + t * ab.x, a.y + t * ab.y};
    return std::sqrt(distSq(p, projection));
}

// Tính đường tròn ngoại tiếp qua 3 điểm
bool calculateCircumcircle(Vec2 p1, Vec2 p2, Vec2 p3, Vec2 &center, float &radius)
{
    float x1 = p1.x, y1 = p1.y;
    float x2 = p2.x, y2 = p2.y;
    float x3 = p3.x, y3 = p3.y;

    float D = 2 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2));
    if (std::abs(D) < 1e-6f)
        return false; // 3 điểm thẳng hàng

    center.x = ((x1 * x1 + y1 * y1) * (y2 - y3) + (x2 * x2 + y2 * y2) * (y3 - y1) + (x3 * x3 + y3 * y3) * (y1 - y2)) / D;
    center.y = ((x1 * x1 + y1 * y1) * (x3 - x2) + (x2 * x2 + y2 * y2) * (x1 - x3) + (x3 * x3 + y3 * y3) * (x2 - x1)) / D;
    radius = std::sqrt((center.x - x1) * (center.x - x1) + (center.y - y1) * (center.y - y1));
    return true;
}

// Tính trung điểm
Vec2 getMidpoint(Vec2 a, Vec2 b)
{
    return {(a.x + b.x) / 2.0f, (a.y + b.y) / 2.0f};
}

// Đối xứng điểm qua điểm: P' = 2*I - P
Vec2 reflectPointPoint(Vec2 p, Vec2 center)
{
    return {2.0f * center.x - p.x, 2.0f * center.y - p.y};
}

// Đối xứng điểm qua đường thẳng
Vec2 reflectPointLine(Vec2 p, Vec2 a, Vec2 b)
{
    Vec2 ab = {b.x - a.x, b.y - a.y};
    Vec2 ap = {p.x - a.x, p.y - a.y};
    float l2 = ab.x * ab.x + ab.y * ab.y;
    if (l2 == 0.0f)
        return p;
    float t = (ap.x * ab.x + ap.y * ab.y) / l2;
    Vec2 projection = {a.x + t * ab.x, a.y + t * ab.y};
    // P' = P + 2*(Projection - P) = 2*Projection - P
    return {2.0f * projection.x - p.x, 2.0f * projection.y - p.y};
}

// ---- Data Structures ----
enum AppMode
{
    MODE_NAV = 0,
    MODE_POINT = 1
};

enum PointMode
{
    PT_CURSOR,
    PT_INPUT,
    PT_MIDPOINT,
    PT_REFLECT_PT,
    PT_REFLECT_LINE,
    PT_ROTATE
};

enum LineMode {
    LN_SEGMENT,
    LN_INFINITE,  
    LN_RAY,
    LN_ANGLE 
};

enum CircleMode
{
    CIR_CENTER_PT,
    CIR_CENTER_RAD,
    CIR_3PTS
};

enum ShapeKind
{
    SH_POINT = 0,
    SH_LINE,        // Giữ nguyên làm Đoạn thẳng
    SH_INFINITE_LINE, // Mới
    SH_RAY,           // Mới
    SH_CIRCLE,
    SH_ELLIPSE,
    SH_PARABOLA,
    SH_HYPERBOLA,
    SH_POLYLINE
};

struct Shape
{
    ShapeKind kind = SH_POINT;
    Color color{0.0f, 0.4f, 1.0f};
    Vec2 p1{0.0f, 0.0f}, p2{0.0f, 0.0f};
    float pointSize = 6.0f;
    float radius = 0.0f;
    float a = 0.0f, b = 0.0f;
    float angle = 0.0f;
    float paramA = 0.0f;
    bool isVertical = true;
    float parab_xmin = -1.0f, parab_xmax = 1.0f;
    float hyper_a = 1.0f, hyper_b = 0.5f;
    std::vector<Vec2> poly;
    int segments = 64;
    std::string name = ""; // Tên hiển thị (VD: "A", "B")
    bool showName = true;  // Mặc định là hiện tên
};

std::string getNextPointName(const std::vector<Shape> &shapes)
{
    std::set<std::string> usedNames;
    for (const auto &s : shapes)
    {
        if (s.kind == SH_POINT && !s.name.empty())
        {
            usedNames.insert(s.name);
        }
    }

    // Thử A -> Z
    for (char c = 'A'; c <= 'Z'; ++c)
    {
        std::string name(1, c);
        if (usedNames.find(name) == usedNames.end())
            return name;
    }

    // Thử A1 -> Z99 (Dự phòng)
    for (int i = 1; i < 100; ++i)
    {
        for (char c = 'A'; c <= 'Z'; ++c)
        {
            std::string name = std::string(1, c) + std::to_string(i);
            if (usedNames.find(name) == usedNames.end())
                return name;
        }
    }

    return "P?"; // Fallback cuối cùng
}

// Hàm tính khoảng cách từ chuột đến hình (cho chức năng Selection)
float getDistToShape(const Shape &s, Vec2 p)
{
    switch (s.kind)
    {
    case SH_POINT:
        return std::sqrt(distSq(s.p1, p));
    case SH_LINE:
        return distToSegment(p, s.p1, s.p2);
    case SH_CIRCLE:
        // Khoảng cách tới đường viền tròn
        return std::abs(std::sqrt(distSq(s.p1, p)) - s.radius);
    case SH_POLYLINE:
    {
        float minDist = 1e9;
        if (s.poly.size() < 2)
            return 1e9;
        for (size_t i = 0; i < s.poly.size() - 1; ++i)
        {
            float d = distToSegment(p, s.poly[i], s.poly[i + 1]);
            if (d < minDist)
                minDist = d;
        }
        return minDist;
    }
    case SH_ELLIPSE:
    {
        // Lấy mẫu xấp xỉ để tính khoảng cách
        int checkSegments = 500;
        float minDist = 1e9;
        Vec2 prev;
        for (int i = 0; i <= checkSegments; ++i)
        {
            float theta = 2.0f * 3.14159f * i / float(checkSegments);
            float x0 = s.a * cos(theta);
            float y0 = s.b * sin(theta);
            Vec2 curr = {
                s.p1.x + x0 * cosf(s.angle) - y0 * sinf(s.angle),
                s.p1.y + x0 * sinf(s.angle) + y0 * cosf(s.angle)};
            if (i > 0)
                minDist = std::min(minDist, distToSegment(p, prev, curr));
            prev = curr;
        }
        return minDist;
    }
    case SH_PARABOLA:
    {
        float minDist = 1e9f;
        float range = 10.0f;  // Phạm vi kiểm tra (nên khớp với range lúc vẽ)
        int checkSegs = 2000; // Số lượng đoạn thẳng dùng để xấp xỉ khoảng cách
        Vec2 prev;

        for (int i = 0; i <= checkSegs; ++i)
        {
            // Biến chạy t từ -range đến range
            float t = -range + (float)i * (2.0f * range / (float)checkSegs);
            float dx, dy;

            if (s.isVertical)
            {
                // x^2 = 4ay => y = x^2 / 4a. Biến chạy là x (t)
                dx = t;
                dy = (t * t) / (4.0f * s.paramA);
            }
            else
            {
                // y^2 = 4ax => x = y^2 / 4a. Biến chạy là y (t)
                dy = t;
                dx = (t * t) / (4.0f * s.paramA);
            }

            // Tọa độ điểm hiện tại trên đường cong (đã cộng offset Đỉnh p1)
            Vec2 curr = {s.p1.x + dx, s.p1.y + dy};

            if (i > 0)
            {
                // Tính khoảng cách từ chuột tới đoạn thẳng nối từ điểm trước tới điểm này
                minDist = std::min(minDist, distToSegment(p, prev, curr));
            }
            prev = curr;
        }
        return minDist;
    }
    break;
    case SH_HYPERBOLA:
    {
        float minDist = 1e9f;
        auto checkBranch = [&](float sign)
        {
            Vec2 prev;
            float t_range = 5.0f; // cosh(5) ~ 74, đủ bao phủ màn hình
            int steps = 50;
            for (int i = 0; i <= steps; ++i)
            {
                float t = -t_range + (float)i * (2.0f * t_range / (float)steps);
                float dx, dy;
                if (s.isVertical)
                {
                    // Dùng hyper_a, hyper_b theo yêu cầu của bạn
                    dx = s.hyper_a * sinhf(t);
                    dy = sign * s.hyper_b * coshf(t);
                }
                else
                {
                    dx = sign * s.hyper_a * coshf(t);
                    dy = s.hyper_b * sinhf(t);
                }
                Vec2 curr = {s.p1.x + dx, s.p1.y + dy};
                if (i > 0)
                    minDist = std::min(minDist, distToSegment(p, prev, curr));
                prev = curr;
            }
        };
        checkBranch(1.0f);  // Nhánh 1
        checkBranch(-1.0f); // Nhánh 2
        return minDist;
    }
    break;

    case SH_INFINITE_LINE: {
        // Khoảng cách từ điểm p đến đường thẳng đi qua s.p1, s.p2 (không giới hạn đầu mút)
        Vec2 ab = {s.p2.x - s.p1.x, s.p2.y - s.p1.y};
        Vec2 ap = {p.x - s.p1.x, p.y - s.p1.y};
        float l2 = ab.x * ab.x + ab.y * ab.y;
        if (l2 == 0.0f) return std::sqrt(distSq(p, s.p1));
        float t = (ap.x * ab.x + ap.y * ab.y) / l2;
        // Không ép t vào khoảng [0, 1] vì là đường thẳng vô hạn
        Vec2 projection = {s.p1.x + t * ab.x, s.p1.y + t * ab.y};
        return std::sqrt(distSq(p, projection));
    }
    case SH_RAY: {
        Vec2 ab = {s.p2.x - s.p1.x, s.p2.y - s.p1.y};
        Vec2 ap = {p.x - s.p1.x, p.y - s.p1.y};
        float l2 = ab.x * ab.x + ab.y * ab.y;
        if (l2 == 0.0f) return std::sqrt(distSq(p, s.p1));
        float t = (ap.x * ab.x + ap.y * ab.y) / l2;
        t = std::max(0.0f, t); // t >= 0 để tạo thành Tia xuất phát từ p1
        Vec2 projection = {s.p1.x + t * ab.x, s.p1.y + t * ab.y};
        return std::sqrt(distSq(p, projection));
    }
        default:
            return 1e9;
        }
    }

static void drawShape(const Shape &s, GeometryRenderer &geom)
{
    switch (s.kind)
    {
    case SH_POINT:
        geom.drawPoint(s.p1, s.color, s.pointSize);
        break;
    case SH_LINE:
        geom.drawLine(s.p1, s.p2, s.color);
        break;
    case SH_CIRCLE:
        geom.drawCircle(s.p1, s.radius, s.color, s.segments);
        break;
    case SH_ELLIPSE:
        geom.drawEllipse(s.p1, s.a, s.b, s.angle, s.color, s.segments);
        break;
    case SH_PARABOLA:
    {
        float l, r, b, t;
        geom.getView(l, r, b, t);

        // Tính toán tầm nhìn hiện tại của người dùng
        float viewWidth = (r - l);
        float viewHeight = (t - b);

        // Range tự động bằng 2 lần tầm nhìn để đảm bảo luôn tràn màn hình
        float dynamicRange = std::max(viewWidth, viewHeight) * 2.0f;

        // Luôn dùng ít nhất 2000 điểm để cực mịn kể cả khi zoom xa
        geom.drawParabola(s.p1, s.paramA, s.isVertical, dynamicRange, 2000, s.color);
    }
    break;
    case SH_HYPERBOLA:
    {
        float l, r, b, t;
        geom.getView(l, r, b, t);
        float dynamicRange = std::max(r - l, t - b); // Lấy phạm vi nhìn thấy

        geom.drawHyperbola(s.p1, s.hyper_a, s.hyper_b, s.isVertical, dynamicRange, 2000, s.color);
    }
    break;
    case SH_POLYLINE:
        geom.drawPolyline(s.poly, s.color);
        break;
    case SH_INFINITE_LINE: {
        float l, r, b, t; geom.getView(l, r, b, t);
        float dynamicRange = std::max(r - l, t - b) * 5.0f; // Kéo dài gấp 5 lần tầm nhìn
        Vec2 dir = {s.p2.x - s.p1.x, s.p2.y - s.p1.y};
        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (len > 1e-6f) {
            dir.x /= len; dir.y /= len;
            Vec2 start = {s.p1.x - dir.x * dynamicRange, s.p1.y - dir.y * dynamicRange};
            Vec2 end = {s.p1.x + dir.x * dynamicRange, s.p1.y + dir.y * dynamicRange};
            geom.drawLine(start, end, s.color);
        }
    } break;

    case SH_RAY: {
        float l, r, b, t; geom.getView(l, r, b, t);
        float dynamicRange = std::max(r - l, t - b) * 5.0f;
        Vec2 dir = {s.p2.x - s.p1.x, s.p2.y - s.p1.y};
        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (len > 1e-6f) {
            dir.x /= len; dir.y /= len;
            Vec2 end = {s.p1.x + dir.x * dynamicRange, s.p1.y + dir.y * dynamicRange};
            geom.drawLine(s.p1, end, s.color); // Gốc tại p1
        }
    } break;
    default:
        break;
    }
}

enum Tool
{
    TOOL_POINT = 0,
    TOOL_LINE,
    TOOL_CIRCLE,
    TOOL_ELLIPSE,
    TOOL_PARABOLA,
    TOOL_HYPERBOLA,
    TOOL_POLYLINE
};

struct AppState
{
    GeometryRenderer *geom = nullptr;
    std::vector<Shape> shapes;
    AppMode mode = MODE_NAV;
    Tool currentTool = TOOL_POINT;

    Color drawColor{0.0f, 0.4f, 1.0f};
    float pointSize = 6.0f;
    Color paintColor{0.0f, 0.4f, 1.0f};

    bool awaitingSecond = false;
    Vec2 tempP1{0.0f, 0.0f};

    bool polylineActive = false;
    std::vector<Vec2> tempPoly;

    // Selection & Snapping
    bool isHoveringAny = false; // Có đang bắt dính điểm nào không (cho Snapping)
    Vec2 hoverPos{0.0f, 0.0f};  // Tọa độ điểm bắt dính

    int hoveredShapeIndex = -1;  // Hình đang trỏ chuột vào (Hover)
    int selectedShapeIndex = -1; // Hình đã click chọn (Selected)

    // Params
    int circleSegments = 500;
    int ellipseSegments = 500;
    float ellipse_a = 0.4f;
    float ellipse_b = 0.4f;
    float ellipse_angle = 0.0f;
    bool ellipseCenterSet = false;
    int parab_segments = 500;
    float ui_parabola_a = 1.0f;
    bool ui_parabola_vertical = true;
    bool parabolaVertexSet = false; // Đánh dấu đã chọn đỉnh
    float hyper_a = 0.4f, hyper_b = 0.25f;
    int hyper_segments = 500;
    float ui_hyper_a = 1.0f;
    float ui_hyper_b = 1.0f;
    bool ui_hyper_vertical = false;
    bool hyperbolaCenterSet = false;

    std::vector<std::vector<Shape>> undoStack;
    std::vector<std::vector<Shape>> redoStack;
    size_t maxUndo = 60;

    bool showGrid = true;
    bool showAxis = true;

    float inputX = 0.0f; // Biến lưu giá trị nhập X
    float inputY = 0.0f; // Biến lưu giá trị nhập Y

    PointMode pointMode = PT_CURSOR;
    int pointStep = 0;  // Bước thực hiện (chọn điểm 1, điểm 2...)
    int savedIdx1 = -1; // Lưu index hình thứ nhất được chọn
    int savedIdx2 = -1; // Lưu index hình thứ hai được chọn

    LineMode lineMode = LN_SEGMENT;

    CircleMode circleMode = CIR_CENTER_PT;
    int circlePointStep = 0;       // Đếm số điểm đã click
    Vec2 circlePoints[3];          // Lưu tạm 3 tọa độ click
    float ui_circle_radius = 1.0f; // Bán kính nhập từ UI
    float ui_rotation_angle = 90.0f; // Góc quay mặc định
    float calculatedAngle = -1.0f;   // Lưu kết quả tính góc
};

// Undo/Redo Helpers
static void pushUndo(AppState &app)
{
    app.undoStack.push_back(app.shapes);
    if (app.undoStack.size() > app.maxUndo)
        app.undoStack.erase(app.undoStack.begin());
    app.redoStack.clear();
}
static void doUndo(AppState &app)
{
    if (app.undoStack.empty())
        return;
    app.redoStack.push_back(app.shapes);
    app.shapes = std::move(app.undoStack.back());
    app.undoStack.pop_back();
}
static void doRedo(AppState &app)
{
    if (app.redoStack.empty())
        return;
    app.undoStack.push_back(app.shapes);
    app.shapes = std::move(app.redoStack.back());
    app.redoStack.pop_back();
}

// ---- Helper Functions ----
void tryLoadFont(ImGuiIO &io, const char *path, float size)
{
    std::ifstream f(path);
    if (!f.good())
    {
        std::cerr << "Warning: Font not found at " << path << ". Using default.\n";
        return;
    }
    io.Fonts->AddFontFromFileTTF(path, size, NULL, io.Fonts->GetGlyphRangesVietnamese());
}

void drawLabel(const std::string &text, float wx, float wy, float l, float r, float b, float t, int winW, int winH, const Color &col)
{
    float ndcX = 2.0f * (wx - l) / (r - l) - 1.0f;
    float ndcY = 2.0f * (wy - b) / (t - b) - 1.0f;
    float screenX = (ndcX + 1.0f) * 0.5f * winW;
    float screenY = (1.0f - ndcY) * 0.5f * winH;
    ImU32 col32 = IM_COL32((int)(col.r * 255), (int)(col.g * 255), (int)(col.b * 255), 255);
    ImGui::GetBackgroundDrawList()->AddText(ImVec2(screenX, screenY), col32, text.c_str());
}

static std::string fmtTick(float v)
{
    std::ostringstream ss;
    if (std::fabs(v - std::round(v)) < 1e-4f)
        ss << int(std::round(v));
    else
    {
        ss << std::fixed << std::setprecision(2) << v;
        std::string s = ss.str();
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
        return s;
    }
    return ss.str();
}

static void screenToWorld(GLFWwindow *window, double sx, double sy, float &wx, float &wy)
{
    AppState *app = static_cast<AppState *>(glfwGetWindowUserPointer(window));
    if (!app || !app->geom)
    {
        wx = wy = 0.0f;
        return;
    }
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    float l, r, b, t;
    app->geom->getView(l, r, b, t);
    wx = l + (float)(sx / (double)width) * (r - l);
    wy = b + (float)((height - sy) / (double)height) * (t - b);
}

// Logic tìm điểm Snap (Bắt dính)
bool getClosestSnapPoint(AppState *app, GLFWwindow *window, double mx, double my, Vec2 &outPos)
{
    if (!app || !app->geom)
        return false;
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    float l, r, b, t;
    app->geom->getView(l, r, b, t);

    double pxToWorld = (r - l) / (double)w;
    float threshold = 12.0f * (float)pxToWorld; // 12px threshold
    float minDst2 = threshold * threshold;

    float wx = l + (float)(mx / w) * (r - l);
    float wy = b + (float)((h - my) / h) * (t - b);
    Vec2 mouseWorld = {wx, wy};

    bool found = false;
    Vec2 bestPos = {0, 0};

    auto checkPoint = [&](Vec2 p)
    {
        float d2 = distSq(p, mouseWorld);
        if (d2 < minDst2)
        {
            minDst2 = d2;
            bestPos = p;
            found = true;
        }
    };

    for (const Shape &s : app->shapes)
    {
        switch (s.kind)
        {
        case SH_POINT:
            checkPoint(s.p1);
            break;
        case SH_LINE:
            checkPoint(s.p1);
            checkPoint(s.p2);
            break;
        case SH_CIRCLE:
        case SH_ELLIPSE:
            checkPoint(s.p1);
            break;
        case SH_POLYLINE:
            for (const auto &v : s.poly)
                checkPoint(v);
            break;
        case SH_PARABOLA:
            checkPoint(s.p1);
            break;
        default:
            break;
        }
    }
    if (found)
        outPos = bestPos;
    return found;
}

// Logic Save/Load
static fs::path resolveSavePath(const char *userPath)
{
    fs::path p(userPath);
    if (p.is_absolute())
        return p;
    return fs::absolute(p);
}
bool saveDrawing(const AppState &app, const char *path)
{
    if (!app.geom)
        return false;
    std::ofstream ofs(resolveSavePath(path).string());
    if (!ofs)
        return false;

    // Lưu vùng nhìn (View)
    float l, r, b, t;
    app.geom->getView(l, r, b, t);
    ofs << l << " " << r << " " << b << " " << t << "\n";

    // Số lượng hình
    ofs << app.shapes.size() << "\n";

    for (const Shape &s : app.shapes)
    {
        // [Kind] [R G B]
        ofs << (int)s.kind << " " << s.color.r << " " << s.color.g << " " << s.color.b << " ";

        switch (s.kind)
        {
        case SH_POINT:
            // Tọa độ, Size, isFixed, showName, Name (Thay khoảng trắng bằng gạch dưới để tránh lỗi đọc file)
            {
                std::string safeName = s.name;
                std::replace(safeName.begin(), safeName.end(), ' ', '_');
                if (safeName.empty())
                    safeName = "null";
                ofs << s.p1.x << " " << s.p1.y << " " << s.pointSize << " " << " " << s.showName << " " << safeName;
            }
            break;
        case SH_LINE:
            ofs << s.p1.x << " " << s.p1.y << " " << s.p2.x << " " << s.p2.y;
            break;
        case SH_INFINITE_LINE:
        case SH_RAY:
            ofs << s.p1.x << " " << s.p1.y << " " << s.p2.x << " " << s.p2.y;
            break;
        case SH_CIRCLE:
            ofs << s.p1.x << " " << s.p1.y << " " << s.radius << " " << s.segments;
            break;
        case SH_ELLIPSE:
            ofs << s.p1.x << " " << s.p1.y << " " << s.a << " " << s.b << " " << s.angle << " " << s.segments;
            break;
        case SH_PARABOLA:
            ofs << s.p1.x << " " << s.p1.y << " " << s.paramA << " " << s.isVertical;
            break;
        case SH_HYPERBOLA:
            ofs << s.p1.x << " " << s.p1.y << " " << s.hyper_a << " " << s.hyper_b << " " << s.isVertical;
            break;
        case SH_POLYLINE:
            ofs << s.poly.size();
            for (auto &p : s.poly)
                ofs << " " << p.x << " " << p.y;
            break;
        }
        ofs << "\n";
    }
    return true;
}

bool loadDrawing(AppState &app, const char *path)
{
    std::ifstream ifs(resolveSavePath(path).string());
    if (!ifs)
        return false;

    float l, r, b, t;
    if (!(ifs >> l >> r >> b >> t))
        return false;
    if (app.geom)
        app.geom->setView(l, r, b, t);

    size_t count;
    if (!(ifs >> count))
        return false;

    app.shapes.clear();
    for (size_t i = 0; i < count; ++i)
    {
        int k;
        float cr, cg, cb;
        ifs >> k >> cr >> cg >> cb;
        Shape s;
        s.kind = (ShapeKind)k;
        s.color = {cr, cg, cb};

        switch (s.kind)
        {
        case SH_POINT:
            ifs >> s.p1.x >> s.p1.y >> s.pointSize >> s.showName >> s.name;
            if (s.name == "null")
                s.name = "";
            std::replace(s.name.begin(), s.name.end(), '_', ' ');
            break;
        case SH_LINE:
            ifs >> s.p1.x >> s.p1.y >> s.p2.x >> s.p2.y;
            break;
        case SH_INFINITE_LINE:
        case SH_RAY:
            ifs >> s.p1.x >> s.p1.y >> s.p2.x >> s.p2.y;
            break;
        case SH_CIRCLE:
            ifs >> s.p1.x >> s.p1.y >> s.radius >> s.segments;
            break;
        case SH_ELLIPSE:
            ifs >> s.p1.x >> s.p1.y >> s.a >> s.b >> s.angle >> s.segments;
            break;
        case SH_PARABOLA:
            ifs >> s.p1.x >> s.p1.y >> s.paramA >> s.isVertical;
            break;
        case SH_HYPERBOLA:
            ifs >> s.p1.x >> s.p1.y >> s.hyper_a >> s.hyper_b >> s.isVertical;
            break;
        case SH_POLYLINE:
            size_t n;
            ifs >> n;
            s.poly.resize(n);
            for (size_t j = 0; j < n; ++j)
                ifs >> s.poly[j].x >> s.poly[j].y;
            break;
        }
        app.shapes.push_back(s);
    }
    return true;
}

// ================= MAIN =================
int main()
{
    if (!glfwInit())
        return -1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(1280, 720, "OpenGL Geometry App", NULL, NULL);
    if (!window)
    {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
        return -1;

    glfwSetFramebufferSizeCallback(window, canvas_framebuffer_size_callback);
    glfwSetScrollCallback(window, canvas_scroll_callback);
    glfwSetMouseButtonCallback(window, canvas_mouse_button_callback);
    glfwSetCursorPosCallback(window, canvas_cursor_position_callback);
    glfwSetKeyCallback(window, canvas_key_callback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    ImGui::StyleColorsDark();

    // Tải font lớn (24.0f)
    tryLoadFont(io, "C:\\Windows\\Fonts\\segoeui.ttf", 24.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    Shader shader("shaders/vertex.glsl", "shaders/fragment.glsl");
    GeometryRenderer geom(shader);
    geom.setView(-2.0f, 2.0f, -1.5f, 1.5f);

    AppState app;
    app.geom = &geom;
    glfwSetWindowUserPointer(window, &app);

    Color gridCol{0.3f, 0.3f, 0.3f};
    Color axisCol{0.6f, 0.6f, 0.6f};
    Color labelCol{0.9f, 0.9f, 0.9f};

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        float l, r, b, t;
        geom.getView(l, r, b, t);

        float worldWidth = r - l;
        float spacing = 0.25f;
        while (spacing * 10.0f < worldWidth)
            spacing *= 2.0f;
        while (spacing * 2.0f > worldWidth && spacing > 1e-6f)
            spacing *= 0.5f;

        geom.drawGrid(spacing, gridCol, axisCol, app.showGrid, app.showAxis);

        shader.use();
        shader.setInt("u_useOverride", 0);

        // Vẽ các hình chính
        for (size_t i = 0; i < app.shapes.size(); ++i)
        {
            // Nếu hình này đang được Select, bỏ qua để vẽ sau (cho nó nổi lên trên)
            if ((int)i == app.selectedShapeIndex)
                continue;
            drawShape(app.shapes[i], geom);

            // --- SỬA ĐOẠN VẼ TÊN ĐIỂM ---
            if (app.shapes[i].kind == SH_POINT && !app.shapes[i].name.empty() && app.shapes[i].showName)
            {
                Vec2 p = app.shapes[i].p1;

                // Chuyển đổi World -> NDC (Normalized Device Coordinates)
                // Công thức: (val - min) / (max - min) * 2 - 1
                float ndcX = (2.0f * (p.x - l) / (r - l)) - 1.0f;
                float ndcY = (2.0f * (p.y - b) / (t - b)) - 1.0f;

                // Chuyển đổi NDC -> Screen Coordinates (Pixel)
                // Lưu ý: ImGui gốc tọa độ (0,0) là góc TRÊN-TRÁI
                // OpenGL gốc tọa độ (-1,-1) là góc DƯỚI-TRÁI
                float screenX = (ndcX + 1.0f) * 0.5f * display_w;
                float screenY = (1.0f - ndcY) * 0.5f * display_h;

                // Offset text lên trên và sang phải một chút để không đè vào điểm
                geom.drawText(app.shapes[i].name, screenX + 10.0f, screenY - 20.0f, app.shapes[i].color);
            }
            // -----------------------------
        }

        // --- 1. HIGHLIGHT SELECTED SHAPE (Click Selection) ---
        // Vẽ hình đang chọn với màu Đậm (Đỏ Cam) và nét to hơn
        if (app.selectedShapeIndex != -1 && app.selectedShapeIndex < (int)app.shapes.size())
        {
            Shape s = app.shapes[app.selectedShapeIndex];
            s.color = {1.0f, 0.4f, 0.0f}; // Màu cam đậm
            if (s.kind == SH_POINT)
                s.pointSize *= 1.5f; // Point to hơn

            // Vẽ đè lên
            drawShape(s, geom);
        }

        // --- 2. HIGHLIGHT HOVERED SHAPE (Mouse Over) ---
        // Chỉ highlight nếu hình đó CHƯA được chọn (tránh bị trùng màu)
        if (app.hoveredShapeIndex != -1 && app.hoveredShapeIndex != app.selectedShapeIndex && app.hoveredShapeIndex < (int)app.shapes.size())
        {
            Shape s = app.shapes[app.hoveredShapeIndex];
            s.color = {1.0f, 1.0f, 0.6f}; // Màu vàng nhạt (Preview)
            drawShape(s, geom);
        }

        // --- 3. HIGHLIGHT SNAP POINT (Khi vẽ) ---
        // Dấu chấm đỏ để biết chuột đang bắt dính vào đâu
        if (app.isHoveringAny)
        {
            float pxSize = 10.0f;
            float worldSize = pxSize * (r - l) / (float)display_w;
            geom.drawCircle(app.hoverPos, worldSize, {1.0f, 0.2f, 0.2f}, 24);
            geom.drawPoint(app.hoverPos, {1.0f, 1.0f, 0.0f}, 5.0f);
        }

        // --- VẼ NHÃN & TRỤC ---
        float labelOffset = (t - b) * 0.02f;
        float startX = std::floor(l / spacing) * spacing;
        float endX = std::ceil(r / spacing) * spacing;

        if (app.showGrid)
        {
            // Số trên trục X
            for (float x = startX; x <= endX + 1e-6f; x += spacing)
            {
                float worldLabelY = (b <= 0.0f && t >= 0.0f) ? (-labelOffset) : (b + labelOffset);
                drawLabel(fmtTick(x), x, worldLabelY, l, r, b, t, display_w, display_h, labelCol);
            }

            // Số trên trục Y
            float startY = std::floor(b / spacing) * spacing;
            float endY = std::ceil(t / spacing) * spacing;
            for (float y = startY; y <= endY + 1e-6f; y += spacing)
            {
                if (std::abs(y) < 1e-5f)
                    continue;
                float worldLabelX = (l <= 0.0f && r >= 0.0f) ? (labelOffset) : (l + labelOffset);
                drawLabel(fmtTick(y), worldLabelX, y, l, r, b, t, display_w, display_h, labelCol);
            }
        }

        float menuWidth = 320.0f;
        // 3. Vẽ tên trục x, y (Chỉ khi showAxis = true)
        if (app.showAxis)
        {

            float visibleRight = l + (r - l) * ((float)(display_w - menuWidth) / display_w);
            drawLabel("x", visibleRight - 0.2f * spacing, 0.2f * spacing, l, r, b, t, display_w, display_h, labelCol);
            drawLabel("y", -0.2f * spacing, t - 0.05f * spacing, l, r, b, t, display_w, display_h, labelCol);
        }

        // --- UI ---
        ImGui::SetNextWindowPos(ImVec2((float)display_w - menuWidth, 0));
        ImGui::SetNextWindowSize(ImVec2(menuWidth, (float)display_h));

        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        ImGui::Text("Mode:");
        if (ImGui::RadioButton("Navigate", app.mode == MODE_NAV))
            app.mode = MODE_NAV;
        ImGui::SameLine();
        if (ImGui::RadioButton("Draw", app.mode == MODE_POINT))
            app.mode = MODE_POINT;

        ImGui::Separator();
        ImGui::Text("View Options:");
        ImGui::Checkbox("Show Grid (Lines & Coords)", &app.showGrid);
        ImGui::Checkbox("Show Axis (Lines & Labels)", &app.showAxis);

        ImGui::Separator();
        ImGui::ColorEdit3("Color", &app.drawColor.r);
        ImGui::SameLine();
        if (ImGui::Button("Apply"))
        {
            // 1. Cập nhật màu vẽ cho các hình vẽ sau này (như cũ)
            app.paintColor = app.drawColor;

            // 2. [MỚI] Nếu đang chọn 1 hình, đổi màu hình đó ngay lập tức
            if (app.selectedShapeIndex != -1 && app.selectedShapeIndex < (int)app.shapes.size())
            {
                pushUndo(app); // Quan trọng: Lưu Undo để có thể quay lại màu cũ
                app.shapes[app.selectedShapeIndex].color = app.drawColor;
            }
        }

        ImGui::Separator();

        // Hiển thị thông tin Selection
        if (app.selectedShapeIndex != -1)
        {
            if (app.selectedShapeIndex < (int)app.shapes.size())
            {
                Shape &selShape = app.shapes[app.selectedShapeIndex];

                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Object Details:");

                switch (selShape.kind)
                {
                case SH_POINT:
                    ImGui::Text("Type: Point");
                    ImGui::BulletText("Position: (%.2f, %.2f)", selShape.p1.x, selShape.p1.y);
                    break;

                case SH_LINE:
                    ImGui::Text("Type: Line");
                    ImGui::BulletText("P1: (%.2f, %.2f)", selShape.p1.x, selShape.p1.y);
                    ImGui::BulletText("P2: (%.2f, %.2f)", selShape.p2.x, selShape.p2.y);
                    {
                        float dx = selShape.p2.x - selShape.p1.x;
                        float dy = selShape.p2.y - selShape.p1.y;
                        if (std::abs(dx) < 1e-6f)
                            ImGui::BulletText("Slope: Vertical (Infinite)");
                        else
                            ImGui::BulletText("Slope: %.4f", dy / dx);
                    }
                    break;

                case SH_CIRCLE:
                    ImGui::Text("Type: Circle");
                    ImGui::BulletText("Center: (%.2f, %.2f)", selShape.p1.x, selShape.p1.y);
                    ImGui::BulletText("Radius: %.2f", selShape.radius);
                    break;

                case SH_ELLIPSE:
                    ImGui::Text("Type: Ellipse");
                    ImGui::BulletText("Center: (%.2f, %.2f)", selShape.p1.x, selShape.p1.y);
                    ImGui::BulletText("Semi-axis a: %.2f", selShape.a);
                    ImGui::BulletText("Semi-axis b: %.2f", selShape.b);
                    break;

                case SH_PARABOLA:
                    ImGui::Text("Type: Parabola");
                    ImGui::BulletText("Vertex: (%.2f, %.2f)", selShape.p1.x, selShape.p1.y);
                    ImGui::BulletText("Param a: %.2f", selShape.paramA);
                    ImGui::BulletText("Orientation: %s", selShape.isVertical ? "Vertical (x^2=4ay)" : "Horizontal (y^2=4ax)");
                    break;

                case SH_HYPERBOLA:
                    ImGui::Text("Type: Hyperbola");
                    ImGui::BulletText("Center: (%.2f, %.2f)", selShape.p1.x, selShape.p1.y);
                    ImGui::BulletText("a: %.2f", selShape.hyper_a);
                    ImGui::BulletText("b: %.2f", selShape.hyper_b);
                    ImGui::BulletText("Orientation: %s", selShape.isVertical ? "Vertical" : "Horizontal");
                    break;

                case SH_POLYLINE:
                    ImGui::Text("Type: Polyline");
                    ImGui::BulletText("Vertices: %d", (int)selShape.poly.size());
                    break;

                default:
                    break;
                }

                // Nếu là ĐIỂM thì hiện ô nhập tên và checkbox Fixed
                if (selShape.kind == SH_POINT)
                {
                    ImGui::InputText("Name", &selShape.name); // Cần include imgui_stdlib.h
                    ImGui::Checkbox("Show Name", &selShape.showName);
                }

                ImGui::Separator();

                // --- THÊM NÚT XÓA (DELETE BUTTON) ---
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0.0f, 0.7f, 0.7f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0.0f, 0.8f, 0.8f));

                if (ImGui::Button("Delete Shape", ImVec2(-1.0f, 0.0f)))
                {                  // -1.0f là full chiều rộng
                    pushUndo(app); // Lưu trạng thái trước khi xóa

                    // Xóa phần tử khỏi vector
                    app.shapes.erase(app.shapes.begin() + app.selectedShapeIndex);

                    // Reset các index vì vector đã thay đổi kích thước
                    app.selectedShapeIndex = -1;
                    app.hoveredShapeIndex = -1;
                    draggingPointIdx = -1; // Đề phòng đang kéo hình đó thì xóa
                }
                ImGui::PopStyleColor(3);
                // ------------------------------------
            }
            if (ImGui::Button("Deselect"))
                app.selectedShapeIndex = -1;
        }
        else
        {
            ImGui::TextDisabled("No shape selected");
        }
        ImGui::Separator();

        if (app.mode == MODE_POINT)
        {
            const char *toolNames[] = {
                "Point", "Line", "Circle", "Ellipse", "Parabola", "Hyperbola", "Polyline"};

            int curTool = (int)app.currentTool;
            if (ImGui::Combo("Tool", &curTool, toolNames, IM_ARRAYSIZE(toolNames)))
            {
                app.currentTool = (Tool)curTool;
                app.awaitingSecond = false;
                app.polylineActive = false;
                app.tempPoly.clear();
            }

            if (app.currentTool == TOOL_POINT)
            {
                const char* pModes[] = { "Cursor", "Input", "Midpoint", "Reflect (Pt)", "Reflect (Line)", "Rotate" };
                int currentPMode = (int)app.pointMode;
                if (ImGui::Combo("Point Mode", &currentPMode, pModes, 6))
                {
                    app.pointMode = (PointMode)currentPMode;
                    app.pointStep = 0; // Reset bước khi đổi mode
                }

                ImGui::Separator();

                if (app.pointMode == PT_CURSOR)
                {
                    ImGui::SliderFloat("Size", &app.pointSize, 1.0f, 20.0f);
                }
                else if (app.pointMode == PT_INPUT)
                {
                    ImGui::InputFloat("X", &app.inputX, 0.5f, 1.0f, "%.2f");
                    ImGui::InputFloat("Y", &app.inputY, 0.5f, 1.0f, "%.2f");
                    if (ImGui::Button("Add Point", ImVec2(-1, 0)))
                    {
                        pushUndo(app);
                        Shape s;
                        s.kind = SH_POINT;
                        s.p1 = {app.inputX, app.inputY};
                        s.pointSize = app.pointSize;
                        s.color = app.paintColor;
                        s.name = getNextPointName(app.shapes);
                        app.shapes.push_back(s);
                    }
                }
                else
                {
                    // Hướng dẫn cho các chế độ dựng hình
                    if (app.pointMode == PT_MIDPOINT)
                        ImGui::Text("Step: %s", app.pointStep == 0 ? "Select 1st Point" : "Select 2nd Point");
                    else if (app.pointMode == PT_REFLECT_PT)
                        ImGui::Text("Step: %s", app.pointStep == 0 ? "Select Point to reflect" : "Select Center Point");
                    else if (app.pointMode == PT_REFLECT_LINE)
                        ImGui::Text("Step: %s", app.pointStep == 0 ? "Select Point" : "Select Mirror Line");
                    if (app.pointMode == PT_ROTATE) {
                        ImGui::InputFloat("Angle (Deg)", &app.ui_rotation_angle, 1.0f, 5.0f, "%.1f");
                        ImGui::Text("Step: %s", app.pointStep == 0 ? "Select Point" : "Select Center");
                    }
                    if (app.pointStep > 0 && ImGui::Button("Cancel Selection"))
                        app.pointStep = 0;
                }
            }
            if (app.currentTool == TOOL_LINE) {
                const char* lModes[] = { "Segment", "Infinite Line", "Ray", "Angle Calculator" };
                int currentLMode = (int)app.lineMode;
                
                if (ImGui::Combo("Line Mode", &currentLMode, lModes, 4)) {
                    app.lineMode = (LineMode)currentLMode;
                    
                    // Reset toàn bộ trạng thái khi đổi Mode để tránh xung đột logic
                    app.awaitingSecond = false; 
                    app.pointStep = 0;
                    app.calculatedAngle = -1.0f; // Reset kết quả tính toán cũ
                }

                ImGui::Separator();

                // CHỈ hiển thị logic vẽ nếu KHÔNG PHẢI là Mode tính góc
                if (app.lineMode != LN_ANGLE) {
                    if (!app.awaitingSecond) {
                        if (app.lineMode == LN_RAY) 
                            ImGui::Text("Click to set Origin point");
                        else 
                            ImGui::Text("Click to set 1st point");
                    } else {
                        ImGui::Text("Click to set 2nd point");
                        if (ImGui::Button("Cancel")) app.awaitingSecond = false;
                    }
                } 
                // CHỈ hiển thị logic tính góc khi chọn Angle Calculator
                else {
                    if (app.calculatedAngle >= 0) {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Result: %.2f deg", app.calculatedAngle);
                    }
                    ImGui::Text("Step: %s", app.pointStep == 0 ? "Select 1st Line" : "Select 2nd Line");
                    
                    if (app.pointStep > 0) {
                        if (ImGui::Button("Reset Selection")) {
                            app.pointStep = 0;
                            app.calculatedAngle = -1.0f;
                        }
                    }
                }
            }
            if (app.currentTool == TOOL_CIRCLE)
            {
                const char *cModes[] = {"Circle (center, point)", "Circle (center, radius)", "Circle (3 points)"};
                int currentMode = (int)app.circleMode;
                if (ImGui::Combo("Mode", &currentMode, cModes, 3))
                {
                    app.circleMode = (CircleMode)currentMode;
                    app.circlePointStep = 0; // Reset khi đổi mode
                }

                ImGui::Separator();

                if (app.circleMode == CIR_CENTER_RAD)
                {
                    ImGui::InputFloat("Radius", &app.ui_circle_radius, 0.1f, 1.0f, "%.2f");
                    if (app.circlePointStep >= 1)
                    {
                        if (ImGui::Button("Draw Circle", ImVec2(-1, 0)))
                        {
                            pushUndo(app);
                            Shape s;
                            s.kind = SH_CIRCLE;
                            s.p1 = app.circlePoints[0];
                            s.radius = app.ui_circle_radius;
                            s.color = app.paintColor;
                            s.segments = 200;
                            app.shapes.push_back(s);
                            app.circlePointStep = 0;
                        }
                    }
                }
                else if (app.circleMode == CIR_3PTS)
                {
                    ImGui::Text("Collected points: %d/3", app.circlePointStep);
                    if (app.circlePointStep > 0)
                    {
                        if (ImGui::Button("Cancel selection"))
                            app.circlePointStep = 0;
                    }
                }
                else
                { // CIR_CENTER_PT
                    ImGui::Text("Step: %s", app.circlePointStep == 0 ? "Click Center" : "Click point on boundary");
                }
            }
            if (app.currentTool == TOOL_ELLIPSE)
            {
                ImGui::Separator();
                ImGui::Text("Select a center point");

                if (app.ellipseCenterSet)
                {
                    // Nhập hệ số a và b
                    ImGui::InputFloat("rx", &app.ellipse_a, 0.1f, 0.5f, "%.2f");
                    ImGui::InputFloat("ry", &app.ellipse_b, 0.1f, 0.5f, "%.2f");

                    // Nút Vẽ
                    if (ImGui::Button("Draw Ellipse", ImVec2(-1.0f, 0.0f)))
                    {
                        pushUndo(app);
                        Shape s;
                        s.kind = SH_ELLIPSE;
                        s.p1 = app.tempP1; // Tâm đã chọn từ click
                        s.a = app.ellipse_a;
                        s.b = app.ellipse_b;
                        s.angle = 0.0f; // Bạn có thể thêm input cho góc nếu muốn
                        s.color = app.paintColor;
                        s.segments = app.ellipseSegments;

                        app.shapes.push_back(s);
                        app.ellipseCenterSet = false; // Reset sau khi vẽ xong
                    }
                }
            }
            if (app.currentTool == TOOL_PARABOLA)
            {
                ImGui::Separator();
                ImGui::Text("Select a center point");

                if (app.parabolaVertexSet)
                {
                    // Nhập hệ số a
                    ImGui::InputFloat("a", &app.ui_parabola_a, 0.1f, 0.5f, "%.2f");

                    // Chọn hướng
                    ImGui::Checkbox("Vertical?", &app.ui_parabola_vertical);

                    if (ImGui::Button("Draw Parabola", ImVec2(-1.0f, 0.0f)))
                    {
                        pushUndo(app);
                        Shape s;
                        s.kind = SH_PARABOLA;
                        s.p1 = app.tempP1; // Đỉnh
                        s.paramA = app.ui_parabola_a;
                        s.isVertical = (bool)app.ui_parabola_vertical;
                        s.color = app.paintColor;

                        app.shapes.push_back(s);
                        app.parabolaVertexSet = false; // Reset
                    }
                }
            }
            if (app.currentTool == TOOL_HYPERBOLA)
            {
                ImGui::Separator();
                ImGui::Text("Select a center point");

                if (app.hyperbolaCenterSet)
                {
                    ImGui::InputFloat("a", &app.ui_hyper_a, 0.1f, 0.5f, "%.2f");
                    ImGui::InputFloat("b", &app.ui_hyper_b, 0.1f, 0.5f, "%.2f");
                    ImGui::Checkbox("Vertical?", &app.ui_hyper_vertical);

                    if (ImGui::Button("Draw Hyperbola", ImVec2(-1.0f, 0.0f)))
                    {
                        pushUndo(app);
                        Shape s;
                        s.kind = SH_HYPERBOLA;
                        s.p1 = app.tempP1;
                        s.hyper_a = app.ui_hyper_a;
                        s.hyper_b = app.ui_hyper_b;
                        s.isVertical = app.ui_hyper_vertical;
                        s.color = app.paintColor;

                        app.shapes.push_back(s);
                        app.hyperbolaCenterSet = false; // Reset
                    }
                }
            }
            if (app.currentTool == TOOL_POLYLINE && app.polylineActive)
            {
                if (ImGui::Button("Finish Polyline"))
                {
                    if (app.tempPoly.size() >= 2)
                    {
                        pushUndo(app);
                        Shape s;
                        s.kind = SH_POLYLINE;
                        s.poly = app.tempPoly;
                        s.color = app.paintColor;
                        app.shapes.push_back(s);
                    }
                    app.polylineActive = false;
                    app.tempPoly.clear();
                }
            }
            else if (app.currentTool == TOOL_POLYLINE)
            {
                if (ImGui::Button("Start Polyline"))
                {
                    app.polylineActive = true;
                    app.tempPoly.clear();
                }
            }
        }
        else
        {
            ImGui::TextWrapped("Click to Select Shapes.\nMouse over to see Highlight.");
        }

        ImGui::Separator();
        static char filePathBuf[260] = "drawing.txt";
        ImGui::InputText("File", filePathBuf, sizeof(filePathBuf));
        if (ImGui::Button("Save"))
            saveDrawing(app, filePathBuf);
        ImGui::SameLine();
        if (ImGui::Button("Load"))
            loadDrawing(app, filePathBuf);

        ImGui::Separator();
        ImGui::BeginDisabled(app.undoStack.empty());
        if (ImGui::Button("Undo"))
            doUndo(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(app.redoStack.empty());
        if (ImGui::Button("Redo"))
            doRedo(app);
        ImGui::EndDisabled();

        ImGui::End();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

// ================= CALLBACKS =================

void canvas_framebuffer_size_callback(GLFWwindow * /*window*/, int width, int height)
{
    glViewport(0, 0, width, height);
}

void canvas_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    if (ImGui::GetIO().WantCaptureKeyboard)
        return;
    AppState *g = static_cast<AppState *>(glfwGetWindowUserPointer(window));
    if (!g)
        return;
    if (key == GLFW_KEY_Z && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL))
        doUndo(*g);
    if (key == GLFW_KEY_Y && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL))
        doRedo(*g);

    if (key == GLFW_KEY_DELETE && action == GLFW_PRESS)
    {
        if (g->selectedShapeIndex != -1 && g->selectedShapeIndex < (int)g->shapes.size())
        {
            pushUndo(*g);
            g->shapes.erase(g->shapes.begin() + g->selectedShapeIndex);
            g->selectedShapeIndex = -1;
            g->hoveredShapeIndex = -1;
            draggingPointIdx = -1;
        }
    }

    // Phím ESC để bỏ chọn
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
    {
        g->selectedShapeIndex = -1;
    }
}

void canvas_scroll_callback(GLFWwindow *window, double /*xoffset*/, double yoffset)
{
    if (ImGui::GetIO().WantCaptureMouse)
        return;
    AppState *g = static_cast<AppState *>(glfwGetWindowUserPointer(window));
    if (!g || !g->geom)
        return;

    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    float wx, wy;
    screenToWorld(window, mx, my, wx, wy);

    float l, r, b, t;
    g->geom->getView(l, r, b, t);
    const float zoomSpeed = 1.15f;
    float factor = (yoffset > 0.0) ? (1.0f / zoomSpeed) : zoomSpeed;
    float newL = wx - (wx - l) * factor;
    float newR = wx + (r - wx) * factor;
    float newB = wy - (wy - b) * factor;
    float newT = wy + (t - wy) * factor;
    g->geom->setView(newL, newR, newB, newT);
}

void canvas_cursor_position_callback(GLFWwindow *window, double xpos, double ypos)
{
    AppState *g = static_cast<AppState *>(glfwGetWindowUserPointer(window));
    if (!g || !g->geom)
        return;

    // --- LOGIC HOVER & SNAP (Khi không kéo chuột) ---
    if (!dragging)
    {
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        float l, r, b, t;
        g->geom->getView(l, r, b, t);

        // 1. Kiểm tra Snap Point (cho Drawing)
        Vec2 snapPos;
        if (getClosestSnapPoint(g, window, xpos, ypos, snapPos))
        {
            g->isHoveringAny = true;
            g->hoverPos = snapPos;
        }
        else
        {
            g->isHoveringAny = false;
        }

        // 2. Kiểm tra Hover Shape (cho Selection)
        // Chuyển chuột sang World
        float wx = l + (float)(xpos / w) * (r - l);
        float wy = b + (float)((h - ypos) / h) * (t - b);
        Vec2 mouseWorld = {wx, wy};

        // Tăng ngưỡng chọn lên 12 pixel cho dễ chọn
        float pixelScale = (r - l) / w;
        float threshold = 12.0f * pixelScale;

        // Biến lưu ứng viên là HÌNH KHÁC (Line, Circle...)
        int bestIdx = -1;
        float bestDist = threshold;

        // Biến lưu ứng viên là ĐIỂM (Point) - Được ưu tiên cao nhất
        int bestPointIdx = -1;
        float bestPointDist = threshold;

        // Duyệt ngược để ưu tiên hình vẽ sau (nằm trên)
        for (int i = (int)g->shapes.size() - 1; i >= 0; --i)
        {
            float d = getDistToShape(g->shapes[i], mouseWorld);

            // Chỉ xét nếu khoảng cách nhỏ hơn ngưỡng (threshold)
            if (d < threshold)
            {
                if (g->shapes[i].kind == SH_POINT)
                {
                    // Nếu là POINT: So sánh với các Point khác
                    if (d < bestPointDist)
                    {
                        bestPointDist = d;
                        bestPointIdx = i;
                    }
                }
                else
                {
                    // Nếu là HÌNH KHÁC: So sánh với các hình khác
                    if (d < bestDist)
                    {
                        bestDist = d;
                        bestIdx = i;
                    }
                }
            }
        }

        // QUYẾT ĐỊNH CUỐI CÙNG:
        // Nếu tìm thấy bất kỳ ĐIỂM nào hợp lệ, chọn nó ngay (Ưu tiên tuyệt đối).
        // Chỉ khi không có điểm nào, ta mới lấy hình khác gần nhất.
        if (bestPointIdx != -1)
        {
            g->hoveredShapeIndex = bestPointIdx;
        }
        else
        {
            g->hoveredShapeIndex = bestIdx;
        }
    }

    // --- LOGIC KÉO ĐIỂM (DYNAMIC POINT) ---
    if (draggingPointIdx != -1)
    {
        // Lấy tọa độ chuột hiện tại (đã convert sang World)
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);
        float wx, wy;
        screenToWorld(window, mx, my, wx, wy);

        // Cập nhật vị trí điểm
        if (draggingPointIdx < (int)g->shapes.size())
        {
            g->shapes[draggingPointIdx].p1 = {wx, wy};
        }
        return; // Đã kéo điểm thì không làm gì khác
    }

    // --- LOGIC PANNING ---
    if (!dragging)
        return;

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    float l, r, b, t;
    g->geom->getView(l, r, b, t);

    double dx = xpos - lastX;
    double dy = ypos - lastY;
    float worldDX = (float)(-dx / (double)width * (r - l));
    float worldDY = (float)(dy / (double)height * (t - b));
    g->geom->setView(l + worldDX, r + worldDX, b + worldDY, t + worldDY);

    lastX = xpos;
    lastY = ypos;
}

void canvas_mouse_button_callback(GLFWwindow *window, int button, int action, int mods)
{
    if (ImGui::GetIO().WantCaptureMouse)
        return;
    AppState *g = static_cast<AppState *>(glfwGetWindowUserPointer(window));
    if (!g || !g->geom)
        return;

    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    float wx, wy;
    screenToWorld(window, mx, my, wx, wy);
    Vec2 effectivePos = {wx, wy};

    // Nếu bắt dính được điểm snap, dùng tọa độ điểm đó
    if (g->isHoveringAny)
        effectivePos = g->hoverPos;

    // --- XỬ LÝ NHẤN CHUỘT TRÁI ---
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS)
    {

        // 1. Cập nhật Selection (Chọn hình)
        g->selectedShapeIndex = g->hoveredShapeIndex;
        if (g->selectedShapeIndex != -1)
        {
            std::cout << "Selected Shape: " << g->selectedShapeIndex << std::endl;
        }

        // 3. Nếu KHÔNG phải là kéo điểm thì mới xét tiếp chế độ vẽ
        // Trường hợp A: Đang ở chế độ VẼ (Draw Mode)
        if (g->mode == MODE_POINT)
        {
            switch (g->currentTool)
            {
            case TOOL_POINT:
            {
                if (g->pointMode == PT_CURSOR)
                {
                    if (g->hoveredShapeIndex != -1)
                        break; // Tránh vẽ đè
                    pushUndo(*g);
                    Shape s;
                    s.kind = SH_POINT;
                    s.p1 = effectivePos;
                    s.pointSize = g->pointSize;
                    s.color = g->paintColor;
                    s.name = getNextPointName(g->shapes);
                    g->shapes.push_back(s);
                }
                else if (g->pointMode == PT_MIDPOINT)
                {
                    // Chỉ chấp nhận nếu hover trúng một POINT
                    if (g->hoveredShapeIndex != -1 && g->shapes[g->hoveredShapeIndex].kind == SH_POINT)
                    {
                        if (g->pointStep == 0)
                        {
                            g->savedIdx1 = g->hoveredShapeIndex;
                            g->pointStep = 1;
                        }
                        else
                        {
                            pushUndo(*g);
                            Vec2 mid = getMidpoint(g->shapes[g->savedIdx1].p1, g->shapes[g->hoveredShapeIndex].p1);
                            Shape s;
                            s.kind = SH_POINT;
                            s.p1 = mid;
                            s.color = g->paintColor;
                            s.name = "Mid_" + g->shapes[g->savedIdx1].name + g->shapes[g->hoveredShapeIndex].name;
                            g->shapes.push_back(s);
                            g->pointStep = 0;
                        }
                    }
                }
                else if (g->pointMode == PT_REFLECT_PT)
                {
                    if (g->hoveredShapeIndex != -1 && g->shapes[g->hoveredShapeIndex].kind == SH_POINT)
                    {
                        if (g->pointStep == 0)
                        {
                            g->savedIdx1 = g->hoveredShapeIndex;
                            g->pointStep = 1;
                        }
                        else
                        {
                            pushUndo(*g);
                            Vec2 ref = reflectPointPoint(g->shapes[g->savedIdx1].p1, g->shapes[g->hoveredShapeIndex].p1);
                            Shape s;
                            s.kind = SH_POINT;
                            s.p1 = ref;
                            s.color = g->paintColor;
                            s.name = g->shapes[g->savedIdx1].name + "'";
                            g->shapes.push_back(s);
                            g->pointStep = 0;
                        }
                    }
                }
                else if (g->pointMode == PT_REFLECT_LINE)
                {
                    if (g->pointStep == 0)
                    {
                        // Bước 1: Chọn điểm
                        if (g->hoveredShapeIndex != -1 && g->shapes[g->hoveredShapeIndex].kind == SH_POINT)
                        {
                            g->savedIdx1 = g->hoveredShapeIndex;
                            g->pointStep = 1;
                        }
                    }
                    else
                    {
                        // Bước 2: Chọn đường thẳng
                        if (g->hoveredShapeIndex != -1 && g->shapes[g->hoveredShapeIndex].kind == SH_LINE)
                        {
                            pushUndo(*g);
                            Shape &line = g->shapes[g->hoveredShapeIndex];
                            Vec2 ref = reflectPointLine(g->shapes[g->savedIdx1].p1, line.p1, line.p2);
                            Shape s;
                            s.kind = SH_POINT;
                            s.p1 = ref;
                            s.color = g->paintColor;
                            s.name = g->shapes[g->savedIdx1].name + "_l";
                            g->shapes.push_back(s);
                            g->pointStep = 0;
                        }
                    }
                } else if (g->pointMode == PT_ROTATE) {
                    if (g->hoveredShapeIndex != -1 && g->shapes[g->hoveredShapeIndex].kind == SH_POINT) {
                        if (g->pointStep == 0) {
                            g->savedIdx1 = g->hoveredShapeIndex;
                            g->pointStep = 1;
                        } else {
                            pushUndo(*g);
                            Vec2 rotated = rotatePoint(g->shapes[g->savedIdx1].p1, g->shapes[g->hoveredShapeIndex].p1, g->ui_rotation_angle);
                            Shape s; s.kind = SH_POINT; s.p1 = rotated; s.color = g->paintColor;
                            s.name = g->shapes[g->savedIdx1].name + "_rot";
                            g->shapes.push_back(s);
                            g->pointStep = 0;
                        }
                    }
                }
            }
            break;

            case TOOL_LINE: {
                if (g->lineMode == LN_ANGLE) {
                    // Chỉ chọn các loại đường (Line, Infinite, Ray)
                    if (g->hoveredShapeIndex != -1) {
                        ShapeKind k = g->shapes[g->hoveredShapeIndex].kind;
                        if (k == SH_LINE || k == SH_INFINITE_LINE || k == SH_RAY) {
                            if (g->pointStep == 0) {
                                g->savedIdx1 = g->hoveredShapeIndex;
                                g->pointStep = 1;
                            } else {
                                Shape &s1 = g->shapes[g->savedIdx1];
                                Shape &s2 = g->shapes[g->hoveredShapeIndex];
                                g->calculatedAngle = getAngleBetweenLines(s1.p1, s1.p2, s2.p1, s2.p2);
                                g->pointStep = 0;
                            }
                        }
                    }
                } else {
                    if (!g->awaitingSecond) {
                        g->tempP1 = effectivePos;
                        g->awaitingSecond = true;
                    } else {
                        pushUndo(*g);
                        Shape s;
                        // Quyết định Kind dựa trên Mode đang chọn
                        if (g->lineMode == LN_SEGMENT) s.kind = SH_LINE;
                        else if (g->lineMode == LN_INFINITE) s.kind = SH_INFINITE_LINE;
                        else if (g->lineMode == LN_RAY) s.kind = SH_RAY;

                        s.p1 = g->tempP1;
                        s.p2 = effectivePos;
                        s.color = g->paintColor;
                        g->shapes.push_back(s);
                        g->awaitingSecond = false;
                    }
                }
            } break;

            case TOOL_CIRCLE:
            {
                // Lưu điểm vừa click vào mảng tạm
                g->circlePoints[g->circlePointStep] = effectivePos;
                g->circlePointStep++;

                if (g->circleMode == CIR_CENTER_PT)
                {
                    if (g->circlePointStep == 2)
                    {
                        pushUndo(*g);
                        Shape s;
                        s.kind = SH_CIRCLE;
                        s.p1 = g->circlePoints[0];                               // Tâm
                        s.radius = dist(g->circlePoints[0], g->circlePoints[1]); // Tính R
                        s.color = g->paintColor;
                        s.segments = 200;
                        g->shapes.push_back(s);
                        g->circlePointStep = 0; // Reset
                    }
                }
                else if (g->circleMode == CIR_CENTER_RAD)
                {
                    // Chỉ cần lấy tâm, việc vẽ sẽ kích hoạt qua nút "Draw" trên UI
                    // (Hoặc bạn có thể vẽ luôn nếu muốn)
                }
                else if (g->circleMode == CIR_3PTS)
                {
                    if (g->circlePointStep == 3)
                    {
                        Vec2 center;
                        float rad;
                        if (calculateCircumcircle(g->circlePoints[0], g->circlePoints[1], g->circlePoints[2], center, rad))
                        {
                            pushUndo(*g);
                            Shape s;
                            s.kind = SH_CIRCLE;
                            s.p1 = center;
                            s.radius = rad;
                            s.color = g->paintColor;
                            s.segments = 200;
                            g->shapes.push_back(s);
                        }
                        g->circlePointStep = 0; // Reset
                    }
                }
            }
            break;

            case TOOL_ELLIPSE:
            {
                g->tempP1 = effectivePos;
                g->ellipseCenterSet = true;
            }
            break;

            case TOOL_POLYLINE:
            {
                if (!g->polylineActive)
                {
                    g->polylineActive = true;
                    g->tempPoly.clear();
                }
                g->tempPoly.push_back(effectivePos);
            }
            break;

            case TOOL_PARABOLA:
            {
                g->tempP1 = effectivePos; // Lưu vị trí click làm Đỉnh
                g->parabolaVertexSet = true;
                // Chờ nhấn nút "Draw" trên UI
            }
            break;

            case TOOL_HYPERBOLA:
            {
                g->tempP1 = effectivePos; // Lưu vị trí click làm Tâm (Center)
                g->hyperbolaCenterSet = true;
            }
            break;

            default:
                break;
            }
            return;
        }

        // Trường hợp B: Đang ở chế độ NAVIGATE hoặc click ra vùng trống -> Panning màn hình
        dragging = true;
        glfwGetCursorPos(window, &lastX, &lastY);
    }
    return;

    // --- XỬ LÝ NHẢ CHUỘT TRÁI (FIX QUAN TRỌNG) ---
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE)
    {
        dragging = false;      // Dừng Panning
        draggingPointIdx = -1; // <--- BẮT BUỘC PHẢI CÓ: Dừng kéo điểm dynamic
    }

    // --- XỬ LÝ CHUỘT PHẢI (Hủy thao tác) ---
    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS)
    {
        g->awaitingSecond = false;
        if (g->polylineActive)
        {
            if (g->tempPoly.size() >= 2)
            {
                pushUndo(*g);
                Shape s;
                s.kind = SH_POLYLINE;
                s.poly = g->tempPoly;
                s.color = g->paintColor;
                g->shapes.push_back(s);
            }
            g->polylineActive = false;
            g->tempPoly.clear();
        }
    }
}