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

namespace fs = std::filesystem;

// ---- Forward declarations ----
void canvas_framebuffer_size_callback(GLFWwindow* window, int width, int height);
void canvas_processInput(GLFWwindow *window);
void canvas_scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void canvas_mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
void canvas_cursor_position_callback(GLFWwindow* window, double xpos, double ypos);
void canvas_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);

// ---- Data Structures (Giữ nguyên từ code cũ) ----
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
};

// Hàm vẽ shape (Helper)
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

enum Tool {
    TOOL_POINT = 0, TOOL_LINE, TOOL_CIRCLE, TOOL_ELLIPSE, TOOL_PARABOLA, TOOL_HYPERBOLA, TOOL_POLYLINE
};

struct AppState {
    GeometryRenderer* geom = nullptr;
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

// ---- Font Helper (New) ----
void tryLoadFont(ImGuiIO& io, const char* path, float size) {
    std::ifstream f(path);
    if (!f.good()) {
        std::cerr << "Warning: Font not found at " << path << ". Using default.\n";
        return;
    }
    io.Fonts->AddFontFromFileTTF(path, size, NULL, io.Fonts->GetGlyphRangesVietnamese());
}

// ---- Text Drawing Helper (New logic for Merged Window) ----
// Chuyển đổi từ tọa độ thế giới (Geometry) sang tọa độ màn hình (ImGui Pixel)
void drawLabel(const std::string& text, float wx, float wy, float l, float r, float b, float t, int winW, int winH, const Color& col) {
    // World to NDC
    float ndcX = 2.0f * (wx - l) / (r - l) - 1.0f;
    float ndcY = 2.0f * (wy - b) / (t - b) - 1.0f;

    // NDC to Screen (ImGui coordinates: Top-Left is 0,0)
    float screenX = (ndcX + 1.0f) * 0.5f * winW;
    float screenY = (1.0f - ndcY) * 0.5f * winH; // Đảo ngược Y vì ImGui Y chạy từ trên xuống

    ImU32 col32 = IM_COL32((int)(col.r*255), (int)(col.g*255), (int)(col.b*255), 255);
    ImGui::GetForegroundDrawList()->AddText(ImVec2(screenX, screenY), col32, text.c_str());
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

// Helpers for Mouse to World conversion
static void screenToWorld(GLFWwindow* window, double sx, double sy, float &wx, float &wy) {
    AppState* app = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!app || !app->geom) { wx = wy = 0.0f; return; }
    int width, height;
    glfwGetFramebufferSize(window, &width, &height); // Dùng FramebufferSize chính xác hơn WindowSize trên HighDPI
    float l, r, b, t;
    app->geom->getView(l, r, b, t);
    wx = l + (float)(sx / (double)width) * (r - l);
    wy = b + (float)((height - sy) / (double)height) * (t - b);
}

// Helpers Save/Load (Giữ nguyên logic cũ nhưng rút gọn để code ngắn hơn)
bool saveDrawing(const AppState& app, const char* path); // Implement ở cuối file
bool loadDrawing(AppState& app, const char* path); // Implement ở cuối file

// Global dragging state
static bool dragging = false;
static double lastX = 0.0, lastY = 0.0;

// ================= MAIN =================
int main()
{
    if (!glfwInit()) return -1;

    // 1. Tạo 1 cửa sổ duy nhất
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    // Tạo cửa sổ rộng để chứa cả Canvas và Menu
    GLFWwindow* window = glfwCreateWindow(1280, 720, "OpenGL Geometry App", NULL, NULL);
    if (!window) { glfwTerminate(); return -1; }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Bật V-Sync

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;

    // 2. Setup Callbacks
    glfwSetFramebufferSizeCallback(window, canvas_framebuffer_size_callback);
    glfwSetScrollCallback(window, canvas_scroll_callback);
    glfwSetMouseButtonCallback(window, canvas_mouse_button_callback);
    glfwSetCursorPosCallback(window, canvas_cursor_position_callback);
    glfwSetKeyCallback(window, canvas_key_callback);

    // 3. Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    
    // Tải font tiếng Việt (Segoe UI có sẵn trên Windows)
    tryLoadFont(io, "C:\\Windows\\Fonts\\arial.ttf", 24.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    // 4. Setup App State & Shader
    Shader shader("shaders/vertex.glsl", "shaders/fragment.glsl");
    GeometryRenderer geom(shader);
    geom.setView(-2.0f, 2.0f, -1.5f, 1.5f);

    AppState app;
    app.geom = &geom;
    glfwSetWindowUserPointer(window, &app); // Link AppState vào window

    Color gridCol{0.3f, 0.3f, 0.3f}; // Grid màu xám nhạt
    Color axisCol{0.6f, 0.6f, 0.6f}; // Trục màu sáng hơn
    Color labelCol{0.9f, 0.9f, 0.9f}; // Chữ màu trắng

    // --- VÒNG LẶP CHÍNH ---
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // A. Start ImGui Frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // B. Lấy kích thước hiển thị
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);

        // C. Clear màn hình
        glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // D. VẼ HÌNH HỌC (Geometry Layer)
        float l, r, b, t;
        geom.getView(l, r, b, t);
        
        // Tính toán khoảng cách grid
        float worldWidth = r - l;
        float spacing = 0.25f;
        while (spacing * 10.0f < worldWidth) spacing *= 2.0f;
        while (spacing * 2.0f > worldWidth && spacing > 1e-6f) spacing *= 0.5f;

        // Vẽ Grid & Axis bằng OpenGL
        geom.drawGrid(spacing, gridCol, axisCol);

        // Vẽ Shapes
        shader.use();
        shader.setInt("u_useOverride", 0);
        for (const Shape &s : app.shapes) {
            drawShape(s, geom);
        }

        // Vẽ Labels bằng ImGui (Overlay)
        // X Labels
        float startX = std::floor(l / spacing) * spacing;
        float endX = std::ceil(r / spacing) * spacing;
        float labelOffset = (t - b) * 0.02f;
        
        for (float x = startX; x <= endX + 1e-6f; x += spacing) {
            // Xác định vị trí Y cho label (bám trục X nếu nhìn thấy, hoặc bám cạnh dưới màn hình)
            float worldLabelY = (b <= 0.0f && t >= 0.0f) ? (-labelOffset) : (b + labelOffset);
            drawLabel(fmtTick(x), x, worldLabelY, l, r, b, t, display_w, display_h, labelCol);
        }
        // Y Labels
        float startY = std::floor(b / spacing) * spacing;
        float endY = std::ceil(t / spacing) * spacing;
        for (float y = startY; y <= endY + 1e-6f; y += spacing) {
            if (std::abs(y) < 1e-5f) continue; // Tránh vẽ trùng gốc tọa độ
            float worldLabelX = (l <= 0.0f && r >= 0.0f) ? (labelOffset) : (l + labelOffset);
            drawLabel(fmtTick(y), worldLabelX, y, l, r, b, t, display_w, display_h, labelCol);
        }

        // --- VẼ TÊN TRỤC X, Y (ĐÃ SỬA) ---
        // 1. Tính toán biên phải vùng nhìn thấy được (trừ đi bề rộng Menu 320px)
        //    Giúp chữ "x" không bao giờ chui tọt vào dưới Menu
        float menuWidth = 320.0f;
        float visibleRight = l + (r - l) * ((float)(display_w - menuWidth) / display_w);

        // 2. Vẽ chữ "x":
        //    - X: Ở mép phải vùng nhìn thấy (visibleRight) lùi lại 1 khoảng (0.6 * spacing)
        //    - Y: Ở dưới trục hoành (-0.4 * spacing) để né các con số
        drawLabel("x", visibleRight - 0.2f * spacing, 0.2f * spacing, l, r, b, t, display_w, display_h, labelCol);

        // 3. Vẽ chữ "y":
        //    - X: Bên trái trục tung (-0.4 * spacing) để né số
        //    - Y: Sát mép trên màn hình (t), trừ nhẹ một xíu cho đẹp
        drawLabel("y", -0.2f * spacing, t - 0.05f * spacing, l, r, b, t, display_w, display_h, labelCol);
        // ---------------------------------

        // E. VẼ GIAO DIỆN (UI Layer)
        // Menu bên phải cố định
        ImGui::SetNextWindowPos(ImVec2((float)display_w - menuWidth, 0));
        ImGui::SetNextWindowSize(ImVec2(menuWidth, (float)display_h));
        
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        
        ImGui::Text("Mode:");
        if (ImGui::RadioButton("Navigate (Pan/Zoom)", app.mode == MODE_NAV)) app.mode = MODE_NAV;
        ImGui::SameLine();
        if (ImGui::RadioButton("Draw", app.mode == MODE_POINT)) app.mode = MODE_POINT;

        ImGui::Separator();
        ImGui::ColorEdit3("Color", &app.drawColor.r);
        ImGui::SameLine();
        if (ImGui::Button("Apply Color")) app.paintColor = app.drawColor;

        ImGui::Separator();
        if (app.mode == MODE_POINT) {
            const char* toolNames[] = { "Point", "Line", "Circle", "Ellipse", "Parabola", "Hyperbola", "Polyline" };
            int curTool = (int)app.currentTool;
            if (ImGui::Combo("Tool", &curTool, toolNames, IM_ARRAYSIZE(toolNames))) {
                app.currentTool = (Tool)curTool;
                app.awaitingSecond = false;
                app.polylineActive = false;
                app.tempPoly.clear();
            }
            
            // Tool params UI
            if (app.currentTool == TOOL_POINT) ImGui::SliderFloat("Size", &app.pointSize, 1.0f, 20.0f);
            if (app.currentTool == TOOL_CIRCLE) ImGui::SliderInt("Segments", &app.circleSegments, 16, 128);
            if (app.currentTool == TOOL_ELLIPSE) {
                ImGui::InputFloat("b Radius", &app.ellipse_b, 0.1f);
                ImGui::SliderAngle("Angle", &app.ellipse_angle);
            }
            if (app.currentTool == TOOL_PARABOLA) {
                 ImGui::InputFloat("k", &app.parab_k, 0.1f);
                 if (ImGui::Button("Add Parabola")) {
                     pushUndo(app);
                     Shape s; s.kind = SH_PARABOLA; s.parab_k = app.parab_k; 
                     s.parab_xmin = app.parab_xmin; s.parab_xmax = app.parab_xmax;
                     s.color = app.paintColor; s.segments = app.parab_segments;
                     app.shapes.push_back(s);
                 }
            }
            if (app.currentTool == TOOL_POLYLINE) {
                if (app.polylineActive) {
                    ImGui::Text("Points: %zu", app.tempPoly.size());
                    if (ImGui::Button("Finish Polyline")) {
                        if (app.tempPoly.size() >= 2) {
                            pushUndo(app);
                            Shape s; s.kind = SH_POLYLINE; s.poly = app.tempPoly; s.color = app.paintColor;
                            app.shapes.push_back(s);
                        }
                        app.polylineActive = false; app.tempPoly.clear();
                    }
                } else {
                    if (ImGui::Button("Start Polyline")) { app.polylineActive = true; app.tempPoly.clear(); }
                }
            }
        } else {
             ImGui::TextWrapped("Use Mouse Drag to Pan.\nScroll to Zoom.");
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

        ImGui::End(); // End Controls Window

        // F. Kết thúc
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

// ================= CALLBACKS IMPLEMENTATION =================

void canvas_framebuffer_size_callback(GLFWwindow* /*window*/, int width, int height) {
    glViewport(0, 0, width, height);
}

void canvas_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    // Nếu ImGui đang nhập liệu (ví dụ gõ tên file), không xử lý phím tắt
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    AppState* g = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!g) return;
    if (key == GLFW_KEY_Z && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL)) {
        doUndo(*g);
    }
    if (key == GLFW_KEY_Y && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL)) {
        doRedo(*g);
    }
}

void canvas_scroll_callback(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    // QUAN TRỌNG: Nếu chuột đang nằm trên UI, không cho zoom Canvas
    if (ImGui::GetIO().WantCaptureMouse) return;

    AppState* g = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!g || !g->geom) return;

    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    float wx, wy;
    screenToWorld(window, mx, my, wx, wy);

    float l, r, b, t; g->geom->getView(l, r, b, t);
    const float zoomSpeed = 1.15f;
    float factor = (yoffset > 0.0) ? (1.0f/zoomSpeed) : zoomSpeed;
    float newL = wx - (wx - l) * factor;
    float newR = wx + (r - wx) * factor;
    float newB = wy - (wy - b) * factor;
    float newT = wy + (t - wy) * factor;
    g->geom->setView(newL, newR, newB, newT);
}

void canvas_mouse_button_callback(GLFWwindow* window, int button, int action, int /*mods*/) {
    // QUAN TRỌNG: Chặn click xuyên thấu qua UI
    if (ImGui::GetIO().WantCaptureMouse) return;

    AppState* g = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!g || !g->geom) return;

    // Helper: Lấy tọa độ thế giới tại chuột
    float wx, wy;
    double mx,my; glfwGetCursorPos(window, &mx, &my);
    screenToWorld(window, mx, my, wx, wy);

    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        if (g->mode == MODE_POINT) {
            // Logic vẽ hình
            switch (g->currentTool) {
                case TOOL_POINT: {
                    pushUndo(*g);
                    Shape s; s.kind = SH_POINT; s.p1 = {wx, wy}; 
                    s.pointSize = g->pointSize; s.color = g->paintColor;
                    g->shapes.push_back(s);
                } break;
                case TOOL_LINE: {
                    if (!g->awaitingSecond) {
                        g->tempP1 = {wx, wy}; g->awaitingSecond = true;
                    } else {
                        pushUndo(*g);
                        Shape s; s.kind = SH_LINE; s.p1 = g->tempP1; s.p2 = {wx, wy}; s.color = g->paintColor;
                        g->shapes.push_back(s);
                        g->awaitingSecond = false;
                    }
                } break;
                case TOOL_CIRCLE: {
                    if (!g->awaitingSecond) {
                        g->tempP1 = {wx, wy}; g->awaitingSecond = true;
                    } else {
                        pushUndo(*g);
                        float dx = wx - g->tempP1.x; float dy = wy - g->tempP1.y;
                        Shape s; s.kind = SH_CIRCLE; s.p1 = g->tempP1; 
                        s.radius = std::sqrt(dx*dx + dy*dy); s.segments = g->circleSegments; s.color = g->paintColor;
                        g->shapes.push_back(s);
                        g->awaitingSecond = false;
                    }
                } break;
                case TOOL_ELLIPSE: {
                     if (!g->awaitingSecond) {
                        g->tempP1 = {wx, wy}; g->awaitingSecond = true;
                    } else {
                        pushUndo(*g);
                        float dx = wx - g->tempP1.x; float dy = wy - g->tempP1.y;
                        Shape s; s.kind = SH_ELLIPSE; s.p1 = g->tempP1;
                        s.a = std::sqrt(dx*dx + dy*dy); s.b = g->ellipse_b; s.angle = g->ellipse_angle;
                        s.segments = g->ellipseSegments; s.color = g->paintColor;
                        g->shapes.push_back(s);
                        g->awaitingSecond = false;
                    }
                } break;
                case TOOL_POLYLINE: {
                    if (!g->polylineActive) { g->polylineActive = true; g->tempPoly.clear(); }
                    g->tempPoly.push_back({wx, wy});
                } break;
                default: break;
            }
            return;
        }

        // Nếu ở Mode NAV thì bắt đầu kéo
        dragging = true;
        glfwGetCursorPos(window, &lastX, &lastY);
        return;
    }

    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        dragging = false;
    }

    // Chuột phải: Hủy thao tác
    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
        g->awaitingSecond = false;
        if (g->polylineActive) {
            // Kết thúc polyline
            if (g->tempPoly.size() >= 2) {
                pushUndo(*g);
                Shape s; s.kind = SH_POLYLINE; s.poly = g->tempPoly; s.color = g->paintColor;
                g->shapes.push_back(s);
            }
            g->polylineActive = false; g->tempPoly.clear();
        }
    }
}

void canvas_cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    if (!dragging) return;
    AppState* g = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!g || !g->geom) return;

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

// ---- Implement Save/Load (Simplified) ----
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
        // ... (Thêm các loại khác tương tự nếu cần full save/load)
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
        // Các loại khác bạn tự thêm nốt nếu cần
        app.shapes.push_back(s);
    }
    return true;
}