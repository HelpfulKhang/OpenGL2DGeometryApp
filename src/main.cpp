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
static bool dragging = false;         // Panning màn hình
static int draggingPointIdx = -1;     // Index điểm đang bị kéo (nếu có)
static double lastX = 0.0, lastY = 0.0;

// ---- Forward declarations ----
void canvas_framebuffer_size_callback(GLFWwindow* window, int width, int height);
void canvas_processInput(GLFWwindow *window);
void canvas_scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void canvas_mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
void canvas_cursor_position_callback(GLFWwindow* window, double xpos, double ypos);
void canvas_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);

// ---- Math Helpers ----
float distSq(Vec2 p1, Vec2 p2) {
    return (p1.x - p2.x)*(p1.x - p2.x) + (p1.y - p2.y)*(p1.y - p2.y);
}

// Khoảng cách từ điểm p đến đoạn thẳng ab
float distToSegment(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = {b.x - a.x, b.y - a.y};
    Vec2 ap = {p.x - a.x, p.y - a.y};
    float l2 = ab.x*ab.x + ab.y*ab.y;
    if (l2 == 0.0f) return std::sqrt(distSq(p, a));
    float t = (ap.x * ab.x + ap.y * ab.y) / l2;
    t = std::max(0.0f, std::min(1.0f, t));
    Vec2 projection = {a.x + t * ab.x, a.y + t * ab.y};
    return std::sqrt(distSq(p, projection));
}

// ---- Data Structures ----
enum AppMode { MODE_NAV = 0, MODE_POINT = 1 };

enum ShapeKind {
    SH_POINT = 0, SH_LINE, SH_CIRCLE, SH_ELLIPSE, SH_PARABOLA, SH_HYPERBOLA, SH_POLYLINE
};

struct Shape {
    ShapeKind kind = SH_POINT;
    Color color{0.0f, 0.4f, 1.0f};
    Vec2 p1{0.0f,0.0f}, p2{0.0f,0.0f};
    float pointSize = 6.0f;
    float radius = 0.0f;
    float a = 0.0f, b = 0.0f;
    float angle = 0.0f;
    float parab_k = 0.0f;
    float parab_xmin = -1.0f, parab_xmax = 1.0f;
    float hyper_a = 1.0f, hyper_b = 0.5f, hyper_tmin = 0.0f, hyper_tmax = 1.0f;
    std::vector<Vec2> poly;
    int segments = 64;
    std::string name = "";  // Tên hiển thị (VD: "A", "B")
    bool isFixed = true;    // Mặc định là Cố định (không kéo được)
};

std::string getNextPointName(const std::vector<Shape>& shapes) {
    std::set<std::string> usedNames;
    for (const auto& s : shapes) {
        if (s.kind == SH_POINT && !s.name.empty()) {
            usedNames.insert(s.name);
        }
    }

    // Thử A -> Z
    for (char c = 'A'; c <= 'Z'; ++c) {
        std::string name(1, c);
        if (usedNames.find(name) == usedNames.end()) return name;
    }

    // Thử A1 -> Z99 (Dự phòng)
    for (int i = 1; i < 100; ++i) {
        for (char c = 'A'; c <= 'Z'; ++c) {
            std::string name = std::string(1, c) + std::to_string(i);
            if (usedNames.find(name) == usedNames.end()) return name;
        }
    }
    
    return "P?"; // Fallback cuối cùng
}

// Hàm tính khoảng cách từ chuột đến hình (cho chức năng Selection)
float getDistToShape(const Shape& s, Vec2 p) {
    switch (s.kind) {
        case SH_POINT:
            return std::sqrt(distSq(s.p1, p));
        case SH_LINE:
            return distToSegment(p, s.p1, s.p2);
        case SH_CIRCLE:
            // Khoảng cách tới đường viền tròn
            return std::abs(std::sqrt(distSq(s.p1, p)) - s.radius);
        case SH_POLYLINE: {
            float minDist = 1e9;
            if (s.poly.size() < 2) return 1e9;
            for (size_t i = 0; i < s.poly.size() - 1; ++i) {
                float d = distToSegment(p, s.poly[i], s.poly[i+1]);
                if (d < minDist) minDist = d;
            }
            return minDist;
        }
        case SH_ELLIPSE: {
            // Lấy mẫu xấp xỉ để tính khoảng cách
            int checkSegments = 32; 
            float minDist = 1e9;
            Vec2 prev;
            for(int i=0; i<=checkSegments; ++i) {
                float theta = 2.0f * 3.14159f * i / float(checkSegments);
                float x0 = s.a * cos(theta);
                float y0 = s.b * sin(theta);
                Vec2 curr = {
                    s.p1.x + x0 * cosf(s.angle) - y0 * sinf(s.angle),
                    s.p1.y + x0 * sinf(s.angle) + y0 * cosf(s.angle)
                };
                if (i > 0) minDist = std::min(minDist, distToSegment(p, prev, curr));
                prev = curr;
            }
            return minDist;
        }
        case SH_PARABOLA: 
             // Xấp xỉ đơn giản: khoảng cách tới điểm đầu/cuối
             return std::min(std::sqrt(distSq({s.parab_xmin, s.parab_k*s.parab_xmin*s.parab_xmin}, p)),
                             std::sqrt(distSq({s.parab_xmax, s.parab_k*s.parab_xmax*s.parab_xmax}, p)));
        default: return 1e9;
    }
}

static void drawShape(const Shape &s, GeometryRenderer &geom) {
    switch (s.kind) {
        case SH_POINT: geom.drawPoint(s.p1, s.color, s.pointSize); break;
        case SH_LINE: geom.drawLine(s.p1, s.p2, s.color); break;
        case SH_CIRCLE: geom.drawCircle(s.p1, s.radius, s.color, s.segments); break;
        case SH_ELLIPSE: geom.drawEllipse(s.p1, s.a, s.b, s.angle, s.color, s.segments); break;
        case SH_PARABOLA: geom.drawParabola(s.parab_k, s.parab_xmin, s.parab_xmax, s.color, std::max(8, s.segments)); break;
        case SH_HYPERBOLA: geom.drawHyperbola(s.hyper_a, s.hyper_b, s.hyper_tmin, s.hyper_tmax, s.color, std::max(8, s.segments)); break;
        case SH_POLYLINE: geom.drawPolyline(s.poly, s.color); break;
        default: break;
    }
}

// [Trong file src/main.cpp]

enum Tool {
    TOOL_POINT_CURSOR = 0, // Đổi tên từ TOOL_POINT
    TOOL_POINT_INPUT,      // Thêm mới
    TOOL_LINE, 
    TOOL_CIRCLE, 
    TOOL_ELLIPSE, 
    TOOL_PARABOLA, 
    TOOL_HYPERBOLA, 
    TOOL_POLYLINE
};

struct AppState {
    GeometryRenderer* geom = nullptr;
    std::vector<Shape> shapes;
    AppMode mode = MODE_NAV;
    Tool currentTool = TOOL_POINT_CURSOR;

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
    int circleSegments = 96;
    int ellipseSegments = 128;
    float ellipse_b = 0.4f;
    float ellipse_angle = 0.0f;
    float parab_k = 0.3f;
    float parab_xmin = -1.5f; float parab_xmax = 1.5f;
    int parab_segments = 300;
    float hyper_a = 0.4f, hyper_b = 0.25f, hyper_tmin = 0.2f, hyper_tmax = 1.6f;
    int hyper_segments = 200;

    std::vector<std::vector<Shape>> undoStack;
    std::vector<std::vector<Shape>> redoStack;
    size_t maxUndo = 60;

    bool showGrid = true;
    bool showAxis = true;

    float inputX = 0.0f; // Biến lưu giá trị nhập X
    float inputY = 0.0f; // Biến lưu giá trị nhập Y
};

// Undo/Redo Helpers
static void pushUndo(AppState &app) {
    app.undoStack.push_back(app.shapes);
    if (app.undoStack.size() > app.maxUndo) app.undoStack.erase(app.undoStack.begin());
    app.redoStack.clear();
}
static void doUndo(AppState &app) {
    if (app.undoStack.empty()) return;
    app.redoStack.push_back(app.shapes);
    app.shapes = std::move(app.undoStack.back());
    app.undoStack.pop_back();
}
static void doRedo(AppState &app) {
    if (app.redoStack.empty()) return;
    app.undoStack.push_back(app.shapes);
    app.shapes = std::move(app.redoStack.back());
    app.redoStack.pop_back();
}

// ---- Helper Functions ----
void tryLoadFont(ImGuiIO& io, const char* path, float size) {
    std::ifstream f(path);
    if (!f.good()) {
        std::cerr << "Warning: Font not found at " << path << ". Using default.\n";
        return;
    }
    io.Fonts->AddFontFromFileTTF(path, size, NULL, io.Fonts->GetGlyphRangesVietnamese());
}

void drawLabel(const std::string& text, float wx, float wy, float l, float r, float b, float t, int winW, int winH, const Color& col) {
    float ndcX = 2.0f * (wx - l) / (r - l) - 1.0f;
    float ndcY = 2.0f * (wy - b) / (t - b) - 1.0f;
    float screenX = (ndcX + 1.0f) * 0.5f * winW;
    float screenY = (1.0f - ndcY) * 0.5f * winH;
    ImU32 col32 = IM_COL32((int)(col.r*255), (int)(col.g*255), (int)(col.b*255), 255);
    ImGui::GetBackgroundDrawList()->AddText(ImVec2(screenX, screenY), col32, text.c_str());
}

static std::string fmtTick(float v) {
    std::ostringstream ss;
    if (std::fabs(v - std::round(v)) < 1e-4f) ss << int(std::round(v));
    else {
        ss << std::fixed << std::setprecision(2) << v;
        std::string s = ss.str();
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }
    return ss.str();
}

static void screenToWorld(GLFWwindow* window, double sx, double sy, float &wx, float &wy) {
    AppState* app = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!app || !app->geom) { wx = wy = 0.0f; return; }
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    float l, r, b, t;
    app->geom->getView(l, r, b, t);
    wx = l + (float)(sx / (double)width) * (r - l);
    wy = b + (float)((height - sy) / (double)height) * (t - b);
}

// Logic tìm điểm Snap (Bắt dính)
bool getClosestSnapPoint(AppState* app, GLFWwindow* window, double mx, double my, Vec2& outPos) {
    if (!app || !app->geom) return false;
    int w, h; glfwGetFramebufferSize(window, &w, &h);
    float l, r, b, t; app->geom->getView(l, r, b, t);
    
    double pxToWorld = (r - l) / (double)w;
    float threshold = 12.0f * (float)pxToWorld; // 12px threshold
    float minDst2 = threshold * threshold;
    
    float wx = l + (float)(mx / w) * (r - l);
    float wy = b + (float)((h - my) / h) * (t - b);
    Vec2 mouseWorld = {wx, wy};

    bool found = false;
    Vec2 bestPos = {0,0};

    auto checkPoint = [&](Vec2 p) {
        float d2 = distSq(p, mouseWorld);
        if (d2 < minDst2) { minDst2 = d2; bestPos = p; found = true; }
    };

    for (const Shape& s : app->shapes) {
        switch (s.kind) {
            case SH_POINT: checkPoint(s.p1); break;
            case SH_LINE: checkPoint(s.p1); checkPoint(s.p2); break;
            case SH_CIRCLE: 
            case SH_ELLIPSE: checkPoint(s.p1); break;
            case SH_POLYLINE: for (const auto& v : s.poly) checkPoint(v); break;
            case SH_PARABOLA: 
                checkPoint({s.parab_xmin, s.parab_k * s.parab_xmin * s.parab_xmin});
                checkPoint({s.parab_xmax, s.parab_k * s.parab_xmax * s.parab_xmax});
                break;
            default: break;
        }
    }
    if (found) outPos = bestPos;
    return found;
}

// Logic Save/Load
static fs::path resolveSavePath(const char* userPath) {
    fs::path p(userPath);
    if (p.is_absolute()) return p;
    return fs::absolute(p);
}
bool saveDrawing(const AppState& app, const char* path) {
    if (!app.geom) return false;
    std::ofstream ofs(resolveSavePath(path).string());
    if (!ofs) return false;
    float l,r,b,t; app.geom->getView(l,r,b,t);
    ofs << l << ' ' << r << ' ' << b << ' ' << t << '\n';
    ofs << app.shapes.size() << '\n';
    for (const Shape &s : app.shapes) {
        ofs << (int)s.kind << ' ' << s.color.r << ' ' << s.color.g << ' ' << s.color.b << ' ';
        if (s.kind == SH_POINT) ofs << s.p1.x << ' ' << s.p1.y << ' ' << s.pointSize;
        else if (s.kind == SH_LINE) ofs << s.p1.x << ' ' << s.p1.y << ' ' << s.p2.x << ' ' << s.p2.y;
        else if (s.kind == SH_CIRCLE) ofs << s.p1.x << ' ' << s.p1.y << ' ' << s.radius << ' ' << s.segments;
        else if (s.kind == SH_POLYLINE) {
            ofs << s.poly.size();
            for (auto& p : s.poly) ofs << ' ' << p.x << ' ' << p.y;
        }
        ofs << '\n';
    }
    return true;
}
bool loadDrawing(AppState& app, const char* path) {
    std::ifstream ifs(resolveSavePath(path).string());
    if (!ifs) return false;
    float l,r,b,t; ifs >> l >> r >> b >> t;
    if (app.geom) app.geom->setView(l,r,b,t);
    size_t count; ifs >> count;
    app.shapes.clear();
    for (size_t i=0; i<count; ++i) {
        int k; float r,g,b;
        ifs >> k >> r >> g >> b;
        Shape s; s.kind = (ShapeKind)k; s.color = {r,g,b};
        if (s.kind == SH_POINT) ifs >> s.p1.x >> s.p1.y >> s.pointSize;
        else if (s.kind == SH_LINE) ifs >> s.p1.x >> s.p1.y >> s.p2.x >> s.p2.y;
        else if (s.kind == SH_CIRCLE) ifs >> s.p1.x >> s.p1.y >> s.radius >> s.segments;
        else if (s.kind == SH_POLYLINE) {
            size_t n; ifs >> n; s.poly.resize(n);
            for(size_t j=0; j<n; ++j) ifs >> s.poly[j].x >> s.poly[j].y;
        }
        app.shapes.push_back(s);
    }
    return true;
}

// ================= MAIN =================
int main()
{
    if (!glfwInit()) return -1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    GLFWwindow* window = glfwCreateWindow(1280, 720, "OpenGL Geometry App", NULL, NULL);
    if (!window) { glfwTerminate(); return -1; }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;

    glfwSetFramebufferSizeCallback(window, canvas_framebuffer_size_callback);
    glfwSetScrollCallback(window, canvas_scroll_callback);
    glfwSetMouseButtonCallback(window, canvas_mouse_button_callback);
    glfwSetCursorPosCallback(window, canvas_cursor_position_callback);
    glfwSetKeyCallback(window, canvas_key_callback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
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

    while (!glfwWindowShouldClose(window)) {
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
        while (spacing * 10.0f < worldWidth) spacing *= 2.0f;
        while (spacing * 2.0f > worldWidth && spacing > 1e-6f) spacing *= 0.5f;

        geom.drawGrid(spacing, gridCol, axisCol, app.showGrid, app.showAxis);

        shader.use();
        shader.setInt("u_useOverride", 0);
        
        // Vẽ các hình chính
        for (size_t i = 0; i < app.shapes.size(); ++i) {
            // Nếu hình này đang được Select, bỏ qua để vẽ sau (cho nó nổi lên trên)
            if ((int)i == app.selectedShapeIndex) continue;
            drawShape(app.shapes[i], geom);

            // --- SỬA ĐOẠN VẼ TÊN ĐIỂM ---
            if (app.shapes[i].kind == SH_POINT && !app.shapes[i].name.empty()) {
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
        if (app.selectedShapeIndex != -1 && app.selectedShapeIndex < (int)app.shapes.size()) {
            Shape s = app.shapes[app.selectedShapeIndex];
            s.color = {1.0f, 0.4f, 0.0f}; // Màu cam đậm
            if (s.kind == SH_POINT) s.pointSize *= 1.5f; // Point to hơn
            
            // Vẽ đè lên
            drawShape(s, geom);
        }

        // --- 2. HIGHLIGHT HOVERED SHAPE (Mouse Over) ---
        // Chỉ highlight nếu hình đó CHƯA được chọn (tránh bị trùng màu)
        if (app.hoveredShapeIndex != -1 && app.hoveredShapeIndex != app.selectedShapeIndex && app.hoveredShapeIndex < (int)app.shapes.size()) {
            Shape s = app.shapes[app.hoveredShapeIndex];
            s.color = {1.0f, 1.0f, 0.6f}; // Màu vàng nhạt (Preview)
            drawShape(s, geom);
        }

        // --- 3. HIGHLIGHT SNAP POINT (Khi vẽ) ---
        // Dấu chấm đỏ để biết chuột đang bắt dính vào đâu
        if (app.isHoveringAny) {
            float pxSize = 10.0f; 
            float worldSize = pxSize * (r - l) / (float)display_w; 
            geom.drawCircle(app.hoverPos, worldSize, {1.0f, 0.2f, 0.2f}, 24); 
            geom.drawPoint(app.hoverPos, {1.0f, 1.0f, 0.0f}, 5.0f);
        }

        // --- VẼ NHÃN & TRỤC ---
        float labelOffset = (t - b) * 0.02f;
        float startX = std::floor(l / spacing) * spacing;
        float endX = std::ceil(r / spacing) * spacing;
        
        if (app.showGrid) {
            // Số trên trục X
            for (float x = startX; x <= endX + 1e-6f; x += spacing) {
                float worldLabelY = (b <= 0.0f && t >= 0.0f) ? (-labelOffset) : (b + labelOffset);
                drawLabel(fmtTick(x), x, worldLabelY, l, r, b, t, display_w, display_h, labelCol);
            }
            
            // Số trên trục Y
            float startY = std::floor(b / spacing) * spacing;
            float endY = std::ceil(t / spacing) * spacing;
            for (float y = startY; y <= endY + 1e-6f; y += spacing) {
                if (std::abs(y) < 1e-5f) continue;
                float worldLabelX = (l <= 0.0f && r >= 0.0f) ? (labelOffset) : (l + labelOffset);
                drawLabel(fmtTick(y), worldLabelX, y, l, r, b, t, display_w, display_h, labelCol);
            }
        }

        float menuWidth = 320.0f; 
        // 3. Vẽ tên trục x, y (Chỉ khi showAxis = true)
        if (app.showAxis) {
            
            float visibleRight = l + (r - l) * ((float)(display_w - menuWidth) / display_w);
            drawLabel("x", visibleRight - 0.2f * spacing, 0.2f * spacing, l, r, b, t, display_w, display_h, labelCol);
            drawLabel("y", -0.2f * spacing, t - 0.05f * spacing, l, r, b, t, display_w, display_h, labelCol);
        }

        // --- UI ---
        ImGui::SetNextWindowPos(ImVec2((float)display_w - menuWidth, 0));
        ImGui::SetNextWindowSize(ImVec2(menuWidth, (float)display_h));
        
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        
        ImGui::Text("Mode:");
        if (ImGui::RadioButton("Navigate", app.mode == MODE_NAV)) app.mode = MODE_NAV;
        ImGui::SameLine();
        if (ImGui::RadioButton("Draw", app.mode == MODE_POINT)) app.mode = MODE_POINT;

        ImGui::Separator();
        ImGui::Text("View Options:");
        ImGui::Checkbox("Show Grid (Lines & Coords)", &app.showGrid);
        ImGui::Checkbox("Show Axis (Lines & Labels)", &app.showAxis);

        ImGui::Separator();
        ImGui::ColorEdit3("Color", &app.drawColor.r);
        ImGui::SameLine();
        if (ImGui::Button("Apply")) {
            // 1. Cập nhật màu vẽ cho các hình vẽ sau này (như cũ)
            app.paintColor = app.drawColor;

            // 2. [MỚI] Nếu đang chọn 1 hình, đổi màu hình đó ngay lập tức
            if (app.selectedShapeIndex != -1 && app.selectedShapeIndex < (int)app.shapes.size()) {
                pushUndo(app); // Quan trọng: Lưu Undo để có thể quay lại màu cũ
                app.shapes[app.selectedShapeIndex].color = app.drawColor;
            }
        }

        ImGui::Separator();
        
        // Hiển thị thông tin Selection
        if (app.selectedShapeIndex != -1) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Selected Shape #%d", app.selectedShapeIndex);
            if (app.selectedShapeIndex < (int)app.shapes.size()) {
                Shape& selShape = app.shapes[app.selectedShapeIndex];
                
                // Nếu là ĐIỂM thì hiện ô nhập tên và checkbox Fixed
                if (selShape.kind == SH_POINT) {
                    ImGui::InputText("Name", &selShape.name); // Cần include imgui_stdlib.h
                    
                    // Checkbox Fixed
                    // Logic: Nếu Fixed = false (Dynamic) thì có thể kéo chuột
                    if (ImGui::Checkbox("Fixed Position", &selShape.isFixed)) {
                        // Có thể thêm logic gì đó nếu cần khi toggle
                    }
                    if (!selShape.isFixed) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(Dynamic)");
                    }
                }
            }
            if (ImGui::Button("Deselect")) app.selectedShapeIndex = -1;
        } else {
            ImGui::TextDisabled("No shape selected");
        }
        ImGui::Separator();

        if (app.mode == MODE_POINT) {
            const char* toolNames[] = { 
                "Point (Cursor)", // Tên mới
                "Point (Input)",  // Tên mới
                "Line", "Circle", "Ellipse", "Parabola", "Hyperbola", "Polyline" 
            };
            
            int curTool = (int)app.currentTool;
            if (ImGui::Combo("Tool", &curTool, toolNames, IM_ARRAYSIZE(toolNames))) {
                app.currentTool = (Tool)curTool;
                app.awaitingSecond = false;
                app.polylineActive = false; app.tempPoly.clear();
            }

            // 2. Hiện giao diện nhập liệu CHỈ KHI chọn Tool "Point (Input)"
            if (app.currentTool == TOOL_POINT_INPUT) {
                ImGui::Text("Coordinates:");
                ImGui::InputFloat("X", &app.inputX, 0.5f, 1.0f, "%.2f");
                ImGui::InputFloat("Y", &app.inputY, 0.5f, 1.0f, "%.2f");
                if (ImGui::Button("Add Point")) {
                    pushUndo(app);
                    Shape s;
                    s.kind = SH_POINT;
                    s.p1 = { app.inputX, app.inputY };
                    s.pointSize = app.pointSize;
                    s.color = app.paintColor;
                    app.shapes.push_back(s);
                    s.name = getNextPointName(app.shapes);
                    s.isFixed = true;
                }
            }
            if (app.currentTool == TOOL_POINT_CURSOR) ImGui::SliderFloat("Size", &app.pointSize, 1.0f, 20.0f);
            if (app.currentTool == TOOL_CIRCLE) ImGui::SliderInt("Segs", &app.circleSegments, 16, 128);
            if (app.currentTool == TOOL_POLYLINE && app.polylineActive) {
                if (ImGui::Button("Finish Polyline")) {
                    if (app.tempPoly.size() >= 2) {
                        pushUndo(app);
                        Shape s; s.kind = SH_POLYLINE; s.poly = app.tempPoly; s.color = app.paintColor;
                        app.shapes.push_back(s);
                    }
                    app.polylineActive = false; app.tempPoly.clear();
                }
            } else if (app.currentTool == TOOL_POLYLINE) {
                if (ImGui::Button("Start Polyline")) { app.polylineActive = true; app.tempPoly.clear(); }
            }
        } else {
             ImGui::TextWrapped("Click to Select Shapes.\nMouse over to see Highlight.");
        }

        ImGui::Separator();
        static char filePathBuf[260] = "drawing.txt";
        ImGui::InputText("File", filePathBuf, sizeof(filePathBuf));
        if (ImGui::Button("Save")) saveDrawing(app, filePathBuf);
        ImGui::SameLine();
        if (ImGui::Button("Load")) loadDrawing(app, filePathBuf);

        ImGui::Separator();
        ImGui::BeginDisabled(app.undoStack.empty());
        if (ImGui::Button("Undo")) doUndo(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(app.redoStack.empty());
        if (ImGui::Button("Redo")) doRedo(app);
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

void canvas_framebuffer_size_callback(GLFWwindow* /*window*/, int width, int height) {
    glViewport(0, 0, width, height);
}

void canvas_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (ImGui::GetIO().WantCaptureKeyboard) return;
    AppState* g = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!g) return;
    if (key == GLFW_KEY_Z && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL)) doUndo(*g);
    if (key == GLFW_KEY_Y && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL)) doRedo(*g);
    
    // Phím ESC để bỏ chọn
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        g->selectedShapeIndex = -1;
    }
}

void canvas_scroll_callback(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    AppState* g = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!g || !g->geom) return;

    double mx, my; glfwGetCursorPos(window, &mx, &my);
    float wx, wy; screenToWorld(window, mx, my, wx, wy);

    float l, r, b, t; g->geom->getView(l, r, b, t);
    const float zoomSpeed = 1.15f;
    float factor = (yoffset > 0.0) ? (1.0f/zoomSpeed) : zoomSpeed;
    float newL = wx - (wx - l) * factor;
    float newR = wx + (r - wx) * factor;
    float newB = wy - (wy - b) * factor;
    float newT = wy + (t - wy) * factor;
    g->geom->setView(newL, newR, newB, newT);
}

void canvas_cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    AppState* g = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!g || !g->geom) return;

    // --- LOGIC HOVER & SNAP (Khi không kéo chuột) ---
    if (!dragging) {
        int w, h; glfwGetFramebufferSize(window, &w, &h);
        float l, r, b, t; g->geom->getView(l, r, b, t);
        
        // 1. Kiểm tra Snap Point (cho Drawing)
        Vec2 snapPos;
        if (getClosestSnapPoint(g, window, xpos, ypos, snapPos)) {
            g->isHoveringAny = true;
            g->hoverPos = snapPos;
        } else {
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
        for (int i = (int)g->shapes.size() - 1; i >= 0; --i) {
            float d = getDistToShape(g->shapes[i], mouseWorld);
            
            // Chỉ xét nếu khoảng cách nhỏ hơn ngưỡng (threshold)
            if (d < threshold) {
                if (g->shapes[i].kind == SH_POINT) {
                    // Nếu là POINT: So sánh với các Point khác
                    if (d < bestPointDist) {
                        bestPointDist = d;
                        bestPointIdx = i;
                    }
                } else {
                    // Nếu là HÌNH KHÁC: So sánh với các hình khác
                    if (d < bestDist) {
                        bestDist = d;
                        bestIdx = i;
                    }
                }
            }
        }

        // QUYẾT ĐỊNH CUỐI CÙNG:
        // Nếu tìm thấy bất kỳ ĐIỂM nào hợp lệ, chọn nó ngay (Ưu tiên tuyệt đối).
        // Chỉ khi không có điểm nào, ta mới lấy hình khác gần nhất.
        if (bestPointIdx != -1) {
            g->hoveredShapeIndex = bestPointIdx;
        } else {
            g->hoveredShapeIndex = bestIdx;
        }
    }

    // --- LOGIC KÉO ĐIỂM (DYNAMIC POINT) ---
    if (draggingPointIdx != -1) {
        // Lấy tọa độ chuột hiện tại (đã convert sang World)
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float wx, wy; screenToWorld(window, mx, my, wx, wy);
        
        // Cập nhật vị trí điểm
        if (draggingPointIdx < (int)g->shapes.size()) {
            g->shapes[draggingPointIdx].p1 = {wx, wy};
        }
        return; // Đã kéo điểm thì không làm gì khác
    }

    // --- LOGIC PANNING ---
    if (!dragging) return;

    int width, height; glfwGetFramebufferSize(window, &width, &height);
    float l, r, b, t; g->geom->getView(l, r, b, t);

    double dx = xpos - lastX;
    double dy = ypos - lastY;
    float worldDX = (float)(-dx / (double)width * (r - l));
    float worldDY = (float)( dy / (double)height * (t - b));
    g->geom->setView(l + worldDX, r + worldDX, b + worldDY, t + worldDY);

    lastX = xpos;
    lastY = ypos;
}

// [Thay thế toàn bộ hàm canvas_mouse_button_callback cũ bằng hàm này]
void canvas_mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    AppState* g = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!g || !g->geom) return;

    double mx, my; glfwGetCursorPos(window, &mx, &my);
    float wx, wy; screenToWorld(window, mx, my, wx, wy);
    Vec2 effectivePos = {wx, wy};

    // Nếu bắt dính được điểm snap, dùng tọa độ điểm đó
    if (g->isHoveringAny) effectivePos = g->hoverPos;

    // --- XỬ LÝ NHẤN CHUỘT TRÁI ---
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        
        // 1. Cập nhật Selection (Chọn hình)
        g->selectedShapeIndex = g->hoveredShapeIndex;
        if (g->selectedShapeIndex != -1) {
            std::cout << "Selected Shape: " << g->selectedShapeIndex << std::endl;
        }
        
        // 2. Kiểm tra Logic Kéo Điểm (Dynamic Point)
        bool startDragPoint = false;
        if (g->hoveredShapeIndex != -1) {
            Shape& s = g->shapes[g->hoveredShapeIndex];
            // Nếu click trúng ĐIỂM và điểm đó KHÔNG FIXED -> Bắt đầu kéo
            if (s.kind == SH_POINT && !s.isFixed) {
                draggingPointIdx = g->hoveredShapeIndex;
                startDragPoint = true;
                pushUndo(*g); // Lưu trạng thái trước khi di chuyển
            }
        }
        
        // 3. Nếu KHÔNG phải là kéo điểm thì mới xét tiếp chế độ vẽ
        if (!startDragPoint) {
            
            // Trường hợp A: Đang ở chế độ VẼ (Draw Mode)
            if (g->mode == MODE_POINT) {
                switch (g->currentTool) {
                    case TOOL_POINT_CURSOR: { 
                        // FIX QUAN TRỌNG: 
                        // Nếu click trúng hình cũ (Select) thì KHÔNG vẽ điểm mới đè lên
                        if (g->hoveredShapeIndex != -1) break;

                        pushUndo(*g);
                        Shape s; s.kind = SH_POINT; s.p1 = effectivePos; 
                        s.pointSize = g->pointSize; s.color = g->paintColor;
                        
                        // Thuộc tính mặc định
                        s.name = getNextPointName(g->shapes);
                        s.isFixed = true; 

                        g->shapes.push_back(s);
                    } break;

                    case TOOL_LINE: {
                        if (!g->awaitingSecond) {
                            g->tempP1 = effectivePos; g->awaitingSecond = true;
                        } else {
                            pushUndo(*g);
                            Shape s; s.kind = SH_LINE; s.p1 = g->tempP1; s.p2 = effectivePos; 
                            s.color = g->paintColor; g->shapes.push_back(s);
                            g->awaitingSecond = false;
                        }
                    } break;

                    case TOOL_CIRCLE: {
                        if (!g->awaitingSecond) {
                            g->tempP1 = effectivePos; g->awaitingSecond = true;
                        } else {
                            pushUndo(*g);
                            float dx = effectivePos.x - g->tempP1.x; float dy = effectivePos.y - g->tempP1.y;
                            Shape s; s.kind = SH_CIRCLE; s.p1 = g->tempP1; 
                            s.radius = std::sqrt(dx*dx + dy*dy); s.segments = g->circleSegments; s.color = g->paintColor;
                            g->shapes.push_back(s);
                            g->awaitingSecond = false;
                        }
                    } break;

                    case TOOL_ELLIPSE: {
                         if (!g->awaitingSecond) {
                            g->tempP1 = effectivePos; g->awaitingSecond = true;
                        } else {
                            pushUndo(*g);
                            float dx = effectivePos.x - g->tempP1.x; float dy = effectivePos.y - g->tempP1.y;
                            Shape s; s.kind = SH_ELLIPSE; s.p1 = g->tempP1;
                            s.a = std::sqrt(dx*dx + dy*dy); s.b = g->ellipse_b; s.angle = g->ellipse_angle;
                            s.segments = g->ellipseSegments; s.color = g->paintColor;
                            g->shapes.push_back(s);
                            g->awaitingSecond = false;
                        }
                    } break;

                    case TOOL_POLYLINE: {
                        if (!g->polylineActive) { g->polylineActive = true; g->tempPoly.clear(); }
                        g->tempPoly.push_back(effectivePos);
                    } break;
                    
                    default: break; // Parabola, Hyperbola... (giữ nguyên nếu có)
                }
                return; // Đã xử lý vẽ xong, thoát luôn (không Pan)
            }
            
            // Trường hợp B: Đang ở chế độ NAVIGATE hoặc click ra vùng trống -> Panning màn hình
            dragging = true;
            glfwGetCursorPos(window, &lastX, &lastY);
        }
        return;
    }

    // --- XỬ LÝ NHẢ CHUỘT TRÁI (FIX QUAN TRỌNG) ---
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        dragging = false;        // Dừng Panning
        draggingPointIdx = -1;   // <--- BẮT BUỘC PHẢI CÓ: Dừng kéo điểm dynamic
    }

    // --- XỬ LÝ CHUỘT PHẢI (Hủy thao tác) ---
    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
        g->awaitingSecond = false;
        if (g->polylineActive) {
            if (g->tempPoly.size() >= 2) {
                pushUndo(*g);
                Shape s; s.kind = SH_POLYLINE; s.poly = g->tempPoly; s.color = g->paintColor;
                g->shapes.push_back(s);
            }
            g->polylineActive = false; g->tempPoly.clear();
        }
    }
}