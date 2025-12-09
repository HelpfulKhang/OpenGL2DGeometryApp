#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>

#include "shader.h"
#include "geometry.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <fstream>
#include <cstring>
#include <algorithm>

#include <filesystem>
namespace fs = std::filesystem;

// ---- forward callbacks (canvas window) ----
void canvas_framebuffer_size_callback(GLFWwindow* window, int width, int height);
void canvas_processInput(GLFWwindow *window);
void canvas_scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void canvas_mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
void canvas_cursor_position_callback(GLFWwindow* window, double xpos, double ypos);
void canvas_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);

// ---- helpers ----
static void screenToWorld(GLFWwindow* window, double sx, double sy, float &wx, float &wy);

// ---- app state shared by both windows ----
enum AppMode { MODE_NAV = 0, MODE_POINT = 1 };

// ---- shapes stored in the document ----
enum ShapeKind {
    SH_POINT = 0,
    SH_LINE,
    SH_CIRCLE,
    SH_ELLIPSE,
    SH_PARABOLA,
    SH_HYPERBOLA,
    SH_POLYLINE
};

struct Shape {
    ShapeKind kind = SH_POINT;
    Color color{0.0f, 0.4f, 1.0f};

    // common params
    Vec2 p1{0.0f,0.0f}, p2{0.0f,0.0f};
    float pointSize = 6.0f;

    // circle/ellipse/parabola/hyperbola params
    float radius = 0.0f;      // circle
    float a = 0.0f, b = 0.0f; // ellipse/hyperbola params / ellipse radii
    float angle = 0.0f;       // ellipse rotation (rad)
    // parabola
    float parab_k = 0.0f;
    float parab_xmin = -1.0f, parab_xmax = 1.0f;
    // hyperbola
    float hyper_a = 1.0f, hyper_b = 0.5f, hyper_tmin = 0.0f, hyper_tmax = 1.0f;

    // polyline
    std::vector<Vec2> poly;

    // sampling/segments (optional)
    int segments = 64;
};

static void drawShape(const Shape &s, GeometryRenderer &geom) {
    switch (s.kind) {
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
            geom.drawParabola(s.parab_k, s.parab_xmin, s.parab_xmax, s.color, std::max(8, s.segments));
            break;
        case SH_HYPERBOLA:
            geom.drawHyperbola(s.hyper_a, s.hyper_b, s.hyper_tmin, s.hyper_tmax, s.color, std::max(8, s.segments));
            break;
        case SH_POLYLINE:
            geom.drawPolyline(s.poly, s.color);
            break;
        default:
            break;
    }
}

// App state (replace the existing struct AppState with this)
enum Tool {
    TOOL_POINT = 0,
    TOOL_LINE,
    TOOL_CIRCLE,
    TOOL_ELLIPSE,
    TOOL_PARABOLA,
    TOOL_HYPERBOLA,
    TOOL_POLYLINE
};

struct AppState {
    GeometryRenderer* geom = nullptr; // operates on canvas context

    // legacy points (not used much now) - kept for compatibility
    std::vector<Vec2> points;

    // document shapes
    std::vector<Shape> shapes;

    // interaction mode + current tool
    AppMode mode = MODE_NAV;
    Tool currentTool = TOOL_POINT;

    // UI draw color (editable) & point size used in UI
    Color drawColor{0.0f, 0.4f, 1.0f};
    float pointSize = 6.0f;

    // committed paint color used for future shapes (set by "Apply color")
    Color paintColor{0.0f, 0.4f, 1.0f};

    // temporary interaction state for two-click tools
    bool awaitingSecond = false;
    Vec2 tempP1{0.0f, 0.0f};

    // polyline building
    bool polylineActive = false;
    std::vector<Vec2> tempPoly;

    // UI parameters for shapes
    int circleSegments = 96;
    int ellipseSegments = 128;
    float ellipse_b = 0.4f;
    float ellipse_angle = 0.0f;

    // parabola params
    float parab_k = 0.3f;
    float parab_xmin = -1.5f;
    float parab_xmax = 1.5f;
    int parab_segments = 300;

    // hyperbola params
    float hyper_a = 0.4f, hyper_b = 0.25f, hyper_tmin = 0.2f, hyper_tmax = 1.6f;
    int hyper_segments = 200;

    // undo/redo: simple snapshot stacks of the shapes vector
    std::vector<std::vector<Shape>> undoStack;
    std::vector<std::vector<Shape>> redoStack;
    size_t maxUndo = 60;
};

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

// prototypes for save/load
bool saveDrawing(const AppState& app, const char* path);
bool loadDrawing(AppState& app, const char* path);

// global small helpers for panning
static bool dragging = false;
static double lastX = 0.0, lastY = 0.0;

// format ticks as string
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

int main()
{
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return -1;
    }

    // Create canvas window (main drawing)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* canvasWindow = glfwCreateWindow(1024, 720, "Canvas - Geometry", NULL, NULL);
    if (!canvasWindow) { std::cerr << "Failed to create canvas window\n"; glfwTerminate(); return -1; }
    glfwMakeContextCurrent(canvasWindow);

    // Load GL function pointers for canvas context
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD for canvas\n";
        return -1;
    }

    // Set canvas callbacks (attach user pointer later after creating AppState)
    glfwSetFramebufferSizeCallback(canvasWindow, canvas_framebuffer_size_callback);
    glfwSetScrollCallback(canvasWindow, canvas_scroll_callback);
    glfwSetMouseButtonCallback(canvasWindow, canvas_mouse_button_callback);
    glfwSetCursorPosCallback(canvasWindow, canvas_cursor_position_callback);
    glfwSetKeyCallback(canvasWindow, canvas_key_callback);

    // Setup shader + geometry on canvas context
    Shader shader("shaders/vertex.glsl", "shaders/fragment.glsl");
    GeometryRenderer geom(shader);
    geom.setView(-2.0f, 2.0f, -1.5f, 1.5f);

    // Create UI window (separate context)
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    GLFWwindow* uiWindow = glfwCreateWindow(360, 520, "UI - Controls", NULL, NULL);
    if (!uiWindow) { std::cerr << "Failed to create UI window\n"; glfwDestroyWindow(canvasWindow); glfwTerminate(); return -1; }

    // Make UI context current and init ImGui there
    glfwMakeContextCurrent(uiWindow);

    // Re-load GL functions for this context (glad uses glfwGetProcAddress)
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD for UI\n";
        glfwDestroyWindow(uiWindow);
        glfwDestroyWindow(canvasWindow);
        glfwTerminate();
        return -1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    io.FontGlobalScale = 1.2f; // make UI larger

    ImGui_ImplGlfw_InitForOpenGL(uiWindow, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    // Create shared AppState and attach to canvas window
    AppState app;
    app.geom = &geom;
    app.mode = MODE_NAV;
    glfwSetWindowUserPointer(canvasWindow, &app);

    // Colors for grid/axis/labels and shape override
    Color gridCol{1.0f, 1.0f, 1.0f};
    Color axisCol{1.0f, 1.0f, 1.0f};
    Color labelCol{1.0f, 1.0f, 1.0f};
    // enforced blue for shapes
    const float blueR = 0.0f, blueG = 0.4f, blueB = 1.0f;

    // Main loop: render both windows
    while (!glfwWindowShouldClose(canvasWindow) && !glfwWindowShouldClose(uiWindow)) {
        // Process events for all windows
        glfwPollEvents();

        // --------------------
        // Render Canvas Window
        // --------------------
        glfwMakeContextCurrent(canvasWindow);

        // set viewport to canvas framebuffer size and clear
        int fbw, fbh;
        glfwGetFramebufferSize(canvasWindow, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // compute spacing based on view
        float l, r, b, t;
        geom.getView(l, r, b, t);
        float worldWidth = r - l;
        float baseSpacing = 0.25f;
        float spacing = baseSpacing;
        while (spacing * 10.0f < worldWidth) spacing *= 2.0f;
        while (spacing * 2.0f > worldWidth && spacing > 1e-6f) spacing *= 0.5f;

        // draw grid & axes on canvas
        geom.drawGrid(spacing, gridCol, axisCol);

        // draw axis labels (white) - ensure shader override off
        shader.use();
        shader.setInt("u_useOverride", 0);

        float labelCharHeight = (t - b) * 0.035f; // tweak if needed
        float labelOffset = (t - b) * 0.02f;

        // X axis labels
        float startX = std::floor(l / spacing) * spacing;
        float endX = std::ceil(r / spacing) * spacing;
        for (float x = startX; x <= endX + 1e-6f; x += spacing) {
            if (x < l - 1e-6f || x > r + 1e-6f) continue;
            float labelY = (b <= 0.0f && t >= 0.0f) ? (0.0f - labelOffset) : (b + labelOffset);
            geom.drawText({ x, labelY }, fmtTick(x), labelCharHeight, labelCol);
        }

        // Y axis labels
        float startY = std::floor(b / spacing) * spacing;
        float endY = std::ceil(t / spacing) * spacing;
        for (float y = startY; y <= endY + 1e-6f; y += spacing) {
            if (y < b - 1e-6f || y > t + 1e-6f) continue;
            float labelX = (l <= 0.0f && r >= 0.0f) ? (0.02f * (r - l)) : (l + 0.02f * (r - l));
            geom.drawText({ labelX, y }, fmtTick(y), labelCharHeight, labelCol);
        }

        // draw all stored shapes (use current shader override color if desired)
        shader.use();
        shader.setInt("u_useOverride", 0);
        // we let each shape carry its own color (and point size)
        for (const Shape &s : app.shapes) {
            // For point we pass its specific size
            if (s.kind == SH_POINT) {
                geom.drawPoint(s.p1, s.color, s.pointSize);
            } else {
                // other shapes drawn by helper
                drawShape(s, geom);
            }
        }

        glfwSwapBuffers(canvasWindow);

        // ----------------
        // Render UI Window
        // ----------------
        glfwMakeContextCurrent(uiWindow);

        // set viewport and clear UI framebuffer to avoid artifacts
        int ufbw, ufbh;
        glfwGetFramebufferSize(uiWindow, &ufbw, &ufbh);
        glViewport(0, 0, ufbw, ufbh);
        glClearColor(0.09f, 0.09f, 0.09f, 1.0f); // UI background
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Build UI (Controls + Tools)
        ImGui::Begin("Controls");

        // Mode: Nav / Draw
        ImGui::Text("Mode:");
        if (ImGui::RadioButton("Nav", app.mode == MODE_NAV)) app.mode = MODE_NAV;
        ImGui::SameLine();
        if (ImGui::RadioButton("Draw", app.mode == MODE_POINT)) app.mode = MODE_POINT;

        ImGui::Separator();

        ImGui::Separator();

        // Color editor (editable) and Apply for future shapes
        ImGui::ColorEdit3("Color", &app.drawColor.r);
        ImGui::SameLine();
        if (ImGui::Button("Apply color")) {
            app.paintColor = app.drawColor;
        }

        ImGui::Separator();

        // Only show tool-specific instructions/params when in Draw mode
        if (app.mode == MODE_POINT) {
            // Tool selection
            const char* toolNames[] = { "Point", "Line", "Circle", "Ellipse", "Parabola", "Hyperbola", "Polyline" };
            int curTool = (int)app.currentTool;
            if (ImGui::Combo("Tool", &curTool, toolNames, IM_ARRAYSIZE(toolNames))) {
                app.currentTool = (Tool)curTool;
                // cancel any pending interaction when tool changes
                app.awaitingSecond = false;
                app.polylineActive = false;
                app.tempPoly.clear();
            }
            switch (app.currentTool) {
                case TOOL_POINT:
                    ImGui::TextWrapped("Click on canvas to add a point.");
                    ImGui::SliderFloat("Point size", &app.pointSize, 1.0f, 20.0f);
                    break;

                case TOOL_LINE:
                    ImGui::TextWrapped("Click two points on canvas to create a line.");
                    if (app.awaitingSecond) ImGui::Text("Awaiting second click (first at %.3f, %.3f)", app.tempP1.x, app.tempP1.y);
                    break;

                case TOOL_CIRCLE:
                    ImGui::TextWrapped("Click center then click a second point to set radius.");
                    ImGui::SliderInt("Segments", &app.circleSegments, 8, 512);
                    if (app.awaitingSecond) ImGui::Text("Awaiting radius point (center at %.3f, %.3f)", app.tempP1.x, app.tempP1.y);
                    break;

                case TOOL_ELLIPSE:
                    ImGui::TextWrapped("Click center then click a second point to set 'a' (major radius). 'b' and angle controlled below.");
                    ImGui::SliderInt("Segments", &app.ellipseSegments, 8, 512);
                    ImGui::InputFloat("b (minor radius)", &app.ellipse_b, 0.01f, 1.0f, "%.3f");
                    ImGui::InputFloat("Angle (rad)", &app.ellipse_angle, 0.01f, 0.1f, "%.3f");
                    if (app.awaitingSecond) ImGui::Text("Awaiting 'a' point (center at %.3f, %.3f)", app.tempP1.x, app.tempP1.y);
                    break;

                case TOOL_PARABOLA:
                    ImGui::TextWrapped("Parabola: y = k * x^2 over [xmin, xmax]. Click 'Add Parabola' to insert.");
                    ImGui::InputFloat("k", &app.parab_k, 0.01f, 0.1f, "%.4f");
                    ImGui::InputFloat("xmin", &app.parab_xmin, 0.1f, 1.0f, "%.3f");
                    ImGui::InputFloat("xmax", &app.parab_xmax, 0.1f, 1.0f, "%.3f");
                    ImGui::SliderInt("Segments", &app.parab_segments, 8, 2000);
                    if (ImGui::Button("Add Parabola")) {
                        pushUndo(app);
                        Shape s; s.kind = SH_PARABOLA;
                        s.parab_k = app.parab_k; s.parab_xmin = app.parab_xmin; s.parab_xmax = app.parab_xmax;
                        s.color = app.paintColor; s.segments = app.parab_segments; app.shapes.push_back(s);
                    }
                    break;

                case TOOL_HYPERBOLA:
                    ImGui::TextWrapped("Hyperbola: parameters a, b, t-range. Click 'Add Hyperbola'.");
                    ImGui::InputFloat("a", &app.hyper_a, 0.01f, 0.1f, "%.3f");
                    ImGui::InputFloat("b", &app.hyper_b, 0.01f, 0.1f, "%.3f");
                    ImGui::InputFloat("tmin", &app.hyper_tmin, 0.01f, 0.1f, "%.3f");
                    ImGui::InputFloat("tmax", &app.hyper_tmax, 0.01f, 0.1f, "%.3f");
                    ImGui::SliderInt("Segments", &app.hyper_segments, 8, 2000);
                    if (ImGui::Button("Add Hyperbola")) {
                        pushUndo(app);
                        Shape s; s.kind = SH_HYPERBOLA;
                        s.hyper_a = app.hyper_a; s.hyper_b = app.hyper_b; s.hyper_tmin = app.hyper_tmin; s.hyper_tmax = app.hyper_tmax;
                        s.color = app.paintColor; s.segments = app.hyper_segments; app.shapes.push_back(s);
                    }
                    break;

                case TOOL_POLYLINE:
                    ImGui::TextWrapped("Click points on canvas to add vertices. Use 'Start/End polyline' or right-click to finish.");
                    if (!app.polylineActive) {
                        if (ImGui::Button("Start Polyline")) { app.polylineActive = true; app.tempPoly.clear(); }
                    } else {
                        ImGui::Text("Polyline active: %zu points", app.tempPoly.size());
                        if (ImGui::Button("End Polyline")) {
                            if (app.tempPoly.size() >= 2) {
                                pushUndo(app);
                                Shape s; s.kind = SH_POLYLINE; s.poly = app.tempPoly; s.color = app.paintColor;
                                app.shapes.push_back(s);
                            }
                            app.polylineActive = false;
                            app.tempPoly.clear();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel Polyline")) { app.polylineActive = false; app.tempPoly.clear(); }
                    }
                    break;
            } // end switch
        } // end if mode==DRAW

        ImGui::Separator();

        // File save/load controls
        static char filePathBuf[260] = "";
        ImGui::InputText("File path", filePathBuf, sizeof(filePathBuf));
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            saveDrawing(app, filePathBuf);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            loadDrawing(app, filePathBuf);
        }

        ImGui::Separator();

        ImGui::BeginDisabled(app.undoStack.empty());
        if (ImGui::Button("Undo")) {
            doUndo(app);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(app.redoStack.empty());
        if (ImGui::Button("Redo")) {
            doRedo(app);
        }
        ImGui::EndDisabled();

        ImGui::Separator();

        if (ImGui::Button("Close App")) {
            glfwSetWindowShouldClose(canvasWindow, 1);
            glfwSetWindowShouldClose(uiWindow, 1);
        }

        ImGui::End();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(uiWindow);
        // end main loop
    }

    // cleanup ImGui
    glfwMakeContextCurrent(uiWindow);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(uiWindow);
    glfwDestroyWindow(canvasWindow);
    glfwTerminate();
    return 0;
}

// ---------------- callbacks for canvas window ----------------

void canvas_framebuffer_size_callback(GLFWwindow* /*window*/, int width, int height) {
    glViewport(0, 0, width, height);
}

void canvas_processInput(GLFWwindow *window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, true);
    }
}

void canvas_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    AppState* g = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!g) return;
    if (key == GLFW_KEY_P && action == GLFW_PRESS) {
        g->mode = (g->mode == MODE_POINT) ? MODE_NAV : MODE_POINT;
        std::cerr << "Mode: " << (g->mode == MODE_POINT ? "POINT" : "NAV") << std::endl;
    }
}

void canvas_scroll_callback(GLFWwindow* window, double /*xoffset*/, double yoffset) {
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
    AppState* g = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!g || !g->geom) return;

    // Helper to get world pos at mouse
    auto getWorldPos = [&](float &wx, float &wy) {
        double mx,my; glfwGetCursorPos(window, &mx, &my);
        screenToWorld(window, mx, my, wx, wy);
    };

    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        float wx, wy;
        getWorldPos(wx, wy);

        // If in draw mode
        if (g->mode == MODE_POINT) {
            switch (g->currentTool) {
                case TOOL_POINT: {
                    pushUndo(*g);
                    Shape pshape;
                    pshape.kind = SH_POINT;
                    pshape.p1 = {wx, wy};
                    pshape.pointSize = g->pointSize;
                    pshape.color = g->paintColor;
                    g->shapes.push_back(pshape);
                    std::cerr << "Added point at (" << wx << ", " << wy << ")\n";
                } break;

                case TOOL_LINE: {
                    if (!g->awaitingSecond) {
                        g->tempP1 = {wx, wy};
                        g->awaitingSecond = true;
                        std::cerr << "Line: first point at (" << wx << ", " << wy << "), awaiting second click\n";
                    } else {
                        pushUndo(*g);
                        Shape s; s.kind = SH_LINE;
                        s.p1 = g->tempP1;
                        s.p2 = {wx, wy};
                        s.color = g->paintColor;
                        g->shapes.push_back(s);
                        g->awaitingSecond = false;
                        std::cerr << "Added line from (" << s.p1.x << "," << s.p1.y << ") to (" << s.p2.x << "," << s.p2.y << ")\n";
                    }
                } break;

                case TOOL_POLYLINE: {
                    if (!g->polylineActive) { g->polylineActive = true; g->tempPoly.clear(); }
                    g->tempPoly.push_back({wx, wy});
                    std::cerr << "Polyline: added vertex (" << wx << ", " << wy << "), total " << g->tempPoly.size() << "\n";
                } break;

                case TOOL_CIRCLE: {
                    if (!g->awaitingSecond) {
                        g->tempP1 = {wx, wy};
                        g->awaitingSecond = true;
                        std::cerr << "Circle: center at (" << wx << ", " << wy << "), awaiting radius point\n";
                    } else {
                        pushUndo(*g);
                        float dx = wx - g->tempP1.x;
                        float dy = wy - g->tempP1.y;
                        Shape s; s.kind = SH_CIRCLE;
                        s.p1 = g->tempP1;
                        s.radius = std::sqrt(dx*dx + dy*dy);
                        s.segments = g->circleSegments;
                        s.color = g->paintColor;
                        g->shapes.push_back(s);
                        g->awaitingSecond = false;
                        std::cerr << "Added circle center(" << s.p1.x << "," << s.p1.y << ") r=" << s.radius << "\n";
                    }
                } break;

                case TOOL_ELLIPSE: {
                    if (!g->awaitingSecond) {
                        g->tempP1 = {wx, wy};
                        g->awaitingSecond = true;
                        std::cerr << "Ellipse: center at (" << wx << ", " << wy << "), awaiting 'a' point\n";
                    } else {
                        pushUndo(*g);
                        float dx = wx - g->tempP1.x;
                        float dy = wy - g->tempP1.y;
                        Shape s; s.kind = SH_ELLIPSE;
                        s.p1 = g->tempP1;
                        s.a = std::sqrt(dx*dx + dy*dy); // a from click
                        s.b = g->ellipse_b;
                        s.angle = g->ellipse_angle;
                        s.segments = g->ellipseSegments;
                        s.color = g->paintColor;
                        g->shapes.push_back(s);
                        g->awaitingSecond = false;
                        std::cerr << "Added ellipse center(" << s.p1.x << "," << s.p1.y << ") a=" << s.a << "\n";
                    }
                } break;

                // parabola/hyperbola are added via UI buttons already handled
                default:
                    break;
            }
            return;
        }

        // Not in draw mode -> start panning
        dragging = true;
        glfwGetCursorPos(window, &lastX, &lastY);
        return;
    }

    // End panning on left release
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        dragging = false;
        return;
    }

    // Right-click: finish polyline or cancel awaiting second click
    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
        if (g->polylineActive) {
            if (g->tempPoly.size() >= 2) {
                pushUndo(*g);
                Shape s; s.kind = SH_POLYLINE;
                s.poly = g->tempPoly;
                s.color = g->paintColor;
                g->shapes.push_back(s);
                std::cerr << "Finished polyline with " << s.poly.size() << " vertices\n";
            }
            g->polylineActive = false;
            g->tempPoly.clear();
            return;
        }
        if (g->awaitingSecond) {
            g->awaitingSecond = false;
            std::cerr << "Cancelled awaiting second point\n";
            return;
        }
    }
}

void canvas_cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    if (!dragging) return;
    AppState* g = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!g || !g->geom) return;

    int width, height; glfwGetWindowSize(window, &width, &height);
    float l, r, b, t; g->geom->getView(l, r, b, t);

    double dx = xpos - lastX;
    double dy = ypos - lastY;
    float worldDX = (float)(-dx / (double)width * (r - l));
    float worldDY = (float)( dy / (double)height * (t - b));
    g->geom->setView(l + worldDX, r + worldDX, b + worldDY, t + worldDY);

    lastX = xpos;
    lastY = ypos;
}

// convert screen coords (window space) to world coords (based on geometry view)
static void screenToWorld(GLFWwindow* window, double sx, double sy, float &wx, float &wy) {
    AppState* app = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!app || !app->geom) { wx = wy = 0.0f; return; }
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    float l, r, b, t;
    app->geom->getView(l, r, b, t);
    wx = l + (float)(sx / (double)width) * (r - l);
    wy = b + (float)((height - sy) / (double)height) * (t - b);
}

// compute project root at runtime using compile-time source path (__FILE__)
// __FILE__ is like ".../test/src/main.cpp" so parent().parent() => project root
static fs::path getProjectRoot() {
    fs::path p = fs::path(__FILE__).parent_path().parent_path();
    return p;
}

// resolve a user-supplied path: if relative, place under <project-root>/save/
// returns absolute path we will use for file IO
static fs::path resolveSavePath(const char* userPath) {
    fs::path up(userPath ? userPath : "");
    if (up.empty() || up.is_relative()) {
        fs::path root = getProjectRoot();
        fs::path saveDir = root / "save";
        // ensure directory exists
        std::error_code ec;
        fs::create_directories(saveDir, ec);
        // if user passed a relative path that includes subfolders, honor them:
        fs::path out = saveDir / up;
        return fs::absolute(out);
    } else {
        return fs::absolute(up);
    }
}

bool saveDrawing(const AppState& app, const char* path) {
    if (!app.geom) return false;
    fs::path full = resolveSavePath(path);
    std::error_code ec; fs::create_directories(full.parent_path(), ec);
    std::ofstream ofs(full.string());
    if (!ofs) return false;

    float l,r,b,t; app.geom->getView(l,r,b,t);
    ofs << l << ' ' << r << ' ' << b << ' ' << t << '\n';
    ofs << app.pointSize << ' ' << app.drawColor.r << ' ' << app.drawColor.g << ' ' << app.drawColor.b << '\n';

    ofs << app.shapes.size() << '\n';
    for (const Shape &s : app.shapes) {
        switch (s.kind) {
            case SH_POINT:
                ofs << "POINT " << s.p1.x << ' ' << s.p1.y << ' ' << s.pointSize << ' '
                    << s.color.r << ' ' << s.color.g << ' ' << s.color.b << '\n';
                break;
            case SH_LINE:
                ofs << "LINE " << s.p1.x << ' ' << s.p1.y << ' '
                    << s.p2.x << ' ' << s.p2.y << ' '
                    << s.color.r << ' ' << s.color.g << ' ' << s.color.b << '\n';
                break;
            case SH_CIRCLE:
                ofs << "CIRCLE " << s.p1.x << ' ' << s.p1.y << ' ' << s.radius << ' ' << s.segments << ' '
                    << s.color.r << ' ' << s.color.g << ' ' << s.color.b << '\n';
                break;
            case SH_ELLIPSE:
                ofs << "ELLIPSE " << s.p1.x << ' ' << s.p1.y << ' ' << s.a << ' ' << s.b << ' ' << s.angle << ' ' << s.segments << ' '
                    << s.color.r << ' ' << s.color.g << ' ' << s.color.b << '\n';
                break;
            case SH_PARABOLA:
                ofs << "PARABOLA " << s.parab_k << ' ' << s.parab_xmin << ' ' << s.parab_xmax << ' ' << s.segments << ' '
                    << s.color.r << ' ' << s.color.g << ' ' << s.color.b << '\n';
                break;
            case SH_HYPERBOLA:
                ofs << "HYPERBOLA " << s.hyper_a << ' ' << s.hyper_b << ' ' << s.hyper_tmin << ' ' << s.hyper_tmax << ' ' << s.segments << ' '
                    << s.color.r << ' ' << s.color.g << ' ' << s.color.b << '\n';
                break;
            case SH_POLYLINE:
                ofs << "POLYLINE " << s.poly.size();
                for (auto &pt : s.poly) ofs << ' ' << pt.x << ' ' << pt.y;
                ofs << ' ' << s.color.r << ' ' << s.color.g << ' ' << s.color.b << '\n';
                break;
            default: break;
        }
    }

    ofs.close();
    std::cerr << "Saved drawing to " << full.string() << std::endl;
    return true;
}

bool loadDrawing(AppState& app, const char* path) {
    if (!app.geom) return false;
    fs::path full = resolveSavePath(path);
    std::ifstream ifs(full.string());
    if (!ifs.is_open()) return false;

    float l,r,b,t;
    ifs >> l >> r >> b >> t;
    if (!ifs) return false;
    app.geom->setView(l,r,b,t);

    float ps, cr,cg,cb;
    ifs >> ps >> cr >> cg >> cb;
    app.pointSize = ps; app.drawColor = {cr,cg,cb}; app.paintColor = app.drawColor;

    size_t n;
    ifs >> n;
    app.shapes.clear();
    for (size_t i=0;i<n;++i) {
        std::string kind;
        ifs >> kind;
        if (kind == "POINT") {
            Shape s; s.kind = SH_POINT;
            ifs >> s.p1.x >> s.p1.y >> s.pointSize >> s.color.r >> s.color.g >> s.color.b;
            app.shapes.push_back(s);
        } else if (kind == "LINE") {
            Shape s; s.kind = SH_LINE;
            ifs >> s.p1.x >> s.p1.y >> s.p2.x >> s.p2.y >> s.color.r >> s.color.g >> s.color.b;
            app.shapes.push_back(s);
        } else if (kind == "CIRCLE") {
            Shape s; s.kind = SH_CIRCLE;
            ifs >> s.p1.x >> s.p1.y >> s.radius >> s.segments >> s.color.r >> s.color.g >> s.color.b;
            app.shapes.push_back(s);
        } else if (kind == "ELLIPSE") {
            Shape s; s.kind = SH_ELLIPSE;
            ifs >> s.p1.x >> s.p1.y >> s.a >> s.b >> s.angle >> s.segments >> s.color.r >> s.color.g >> s.color.b;
            app.shapes.push_back(s);
        } else if (kind == "PARABOLA") {
            Shape s; s.kind = SH_PARABOLA;
            ifs >> s.parab_k >> s.parab_xmin >> s.parab_xmax >> s.segments >> s.color.r >> s.color.g >> s.color.b;
            app.shapes.push_back(s);
        } else if (kind == "HYPERBOLA") {
            Shape s; s.kind = SH_HYPERBOLA;
            ifs >> s.hyper_a >> s.hyper_b >> s.hyper_tmin >> s.hyper_tmax >> s.segments >> s.color.r >> s.color.g >> s.color.b;
            app.shapes.push_back(s);
        } else if (kind == "POLYLINE") {
            size_t m; ifs >> m;
            Shape s; s.kind = SH_POLYLINE;
            s.poly.resize(m);
            for (size_t j=0;j<m;++j) ifs >> s.poly[j].x >> s.poly[j].y;
            ifs >> s.color.r >> s.color.g >> s.color.b;
            app.shapes.push_back(s);
        } else {
            // unknown kind: try to skip line
            std::string rest;
            std::getline(ifs, rest);
        }
    }

    std::cerr << "Loaded drawing from " << full.string() << std::endl;
    return true;
}