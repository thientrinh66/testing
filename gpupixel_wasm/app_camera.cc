// clang-format off
#include <glad/glad.h>
#include <GLFW/glfw3.h>
// clang-format on
#include <iostream>
#include <opencv2/opencv.hpp>
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "ghc/filesystem.hpp"
#include "camera_source.h"

namespace fs {
using namespace ghc::filesystem;
using ifstream = ghc::filesystem::ifstream;
using ofstream = ghc::filesystem::ofstream;
using fstream = ghc::filesystem::fstream;
}  // namespace fs

#ifdef _WIN32
#include <Shlwapi.h>
#include <delayimp.h>
#include <windows.h>
#pragma comment(lib, "Shlwapi.lib")
#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdlib.h>
#endif

#include "gpupixel/gpupixel.h"
#include "imgui.h"

using namespace gpupixel;

// Filters
std::shared_ptr<BeautyFaceFilter> beauty_filter_;
std::shared_ptr<FaceReshapeFilter> reshape_filter_;
std::shared_ptr<gpupixel::LipstickFilter> lipstick_filter_;
std::shared_ptr<gpupixel::BlusherFilter> blusher_filter_;
std::shared_ptr<SourceRawData> source_camera_;
std::shared_ptr<SinkRawData> sink_raw_data_;
#ifdef GPUPIXEL_ENABLE_FACE_DETECTOR
std::shared_ptr<FaceDetector> face_detector_;
#endif

// Camera
std::unique_ptr<CameraSource> camera_;
bool use_camera_ = true;

// Filter parameters
float beauty_strength_ = 0.0f;
float whitening_strength_ = 0.0f;
float face_slim_strength_ = 0.0f;
float eye_enlarge_strength_ = 0.0f;
float lipstick_strength_ = 0.0f;
float blusher_strength_ = 0.0f;

// GLFW window handle
GLFWwindow* main_window_ = nullptr;

// Camera frame buffer
cv::Mat current_camera_frame_;
std::mutex camera_frame_mutex_;

// Get executable path
std::string GetExecutablePath() {
  std::string path;
#ifdef _WIN32
  char buffer[MAX_PATH];
  GetModuleFileNameA(NULL, buffer, MAX_PATH);
  PathRemoveFileSpecA(buffer);
  path = buffer;
#elif defined(__APPLE__)
  char buffer[PATH_MAX];
  uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) == 0) {
    char realPath[PATH_MAX];
    if (realpath(buffer, realPath)) {
      path = realPath;
      size_t pos = path.find_last_of("/\\");
      if (pos != std::string::npos) {
        path = path.substr(0, pos);
      }
    }
  }
#elif defined(__linux__)
  char buffer[PATH_MAX];
  ssize_t count = readlink("/proc/self/exe", buffer, PATH_MAX);
  if (count != -1) {
    buffer[count] = '\0';
    path = buffer;
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
      path = path.substr(0, pos);
    }
  }
#endif
  return path;
}

// Check shader compilation/linking errors
bool CheckShaderErrors(GLuint shader, const char* type) {
  GLint success;
  GLchar infoLog[1024];

  if (strcmp(type, "PROGRAM") != 0) {
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
      glGetShaderInfoLog(shader, 1024, NULL, infoLog);
      std::cerr << "Shader compilation error: " << type << "\n"
                << infoLog << std::endl;
      return false;
    }
  } else {
    glGetProgramiv(shader, GL_LINK_STATUS, &success);
    if (!success) {
      glGetProgramInfoLog(shader, 1024, NULL, infoLog);
      std::cerr << "Program linking error: " << type << "\n"
                << infoLog << std::endl;
      return false;
    }
  }
  return true;
}

// GLFW framebuffer resize callback
void OnFramebufferResize(GLFWwindow* window, int width, int height) {
  glViewport(0, 0, width, height);
}

// GLFW error callback
void ErrorCallback(int error, const char* description) {
  std::cerr << "GLFW Error: " << description << std::endl;
}

// Initialize GLFW and create window
bool SetupGlfwWindow() {
  glfwSetErrorCallback(ErrorCallback);

  if (!glfwInit()) {
    std::cerr << "Failed to initialize GLFW" << std::endl;
    return false;
  }

#ifdef __APPLE__
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#else
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

  glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

  main_window_ = glfwCreateWindow(1280, 720, "GPUPixel Camera Demo", NULL, NULL);
  if (main_window_ == NULL) {
    std::cerr << "Failed to create GLFW window" << std::endl;
    glfwTerminate();
    return false;
  }

  glfwMakeContextCurrent(main_window_);

  if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
    std::cerr << "Failed to initialize GLAD" << std::endl;
    glfwDestroyWindow(main_window_);
    glfwTerminate();
    return false;
  }

  glfwSetFramebufferSizeCallback(main_window_, OnFramebufferResize);

  return true;
}

// Initialize ImGui
void SetupImGui() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(main_window_, true);

#ifdef __APPLE__
  const char* glsl_version = "#version 330 core";
#else
  const char* glsl_version = "#version 130";
#endif
  ImGui_ImplOpenGL3_Init(glsl_version);
}

// Camera frame callback
void OnCameraFrame(const cv::Mat& frame) {
  std::lock_guard<std::mutex> lock(camera_frame_mutex_);
  current_camera_frame_ = frame.clone();
}

// Initialize GPUPixel filters and pipeline
void SetupFilterPipeline() {
  auto resource_path = fs::path(GetExecutablePath()).parent_path();
  std::cout << "[App] Current resource path: " << resource_path << std::endl;
  GPUPixel::SetResourcePath(resource_path.string());

  // Create filters
  lipstick_filter_ = LipstickFilter::Create();
  blusher_filter_ = BlusherFilter::Create();
  reshape_filter_ = FaceReshapeFilter::Create();
  beauty_filter_ = BeautyFaceFilter::Create();

#ifdef GPUPIXEL_ENABLE_FACE_DETECTOR
  face_detector_ = FaceDetector::Create();
#endif

  // Create camera source and sink
  source_camera_ = SourceRawData::Create();
  sink_raw_data_ = SinkRawData::Create();

  // Build filter pipeline
  source_camera_->AddSink(lipstick_filter_)
      ->AddSink(blusher_filter_)
      ->AddSink(reshape_filter_)
      ->AddSink(beauty_filter_)
      ->AddSink(sink_raw_data_);
      
  // Setup camera
  camera_ = std::make_unique<CameraSource>(0);
  camera_->SetResolution(640, 480);
  camera_->SetFPS(30);
  camera_->SetFrameCallback(OnCameraFrame);
  
  if (!camera_->Start()) {
    std::cerr << "Failed to start camera" << std::endl;
    use_camera_ = false;
  }
}

// Update filter parameters from ImGui controls
void UpdateFilterParametersFromUI() {
  ImGui::Begin("Beauty Control Panel", nullptr,
               ImGuiWindowFlags_AlwaysAutoResize);

  // Camera controls
  if (ImGui::Button(use_camera_ ? "Stop Camera" : "Start Camera")) {
    if (use_camera_) {
      camera_->Stop();
      use_camera_ = false;
    } else {
      if (camera_->Start()) {
        use_camera_ = true;
      }
    }
  }
  
  ImGui::Text("Camera Status: %s", use_camera_ ? "Running" : "Stopped");
  ImGui::Separator();

  if (ImGui::SliderFloat("Smoothing", &beauty_strength_, 0.0f, 10.0f)) {
    beauty_filter_->SetBlurAlpha(beauty_strength_ / 10.0f);
  }

  if (ImGui::SliderFloat("Whitening", &whitening_strength_, 0.0f, 10.0f)) {
    beauty_filter_->SetWhite(whitening_strength_ / 20.0f);
  }

  if (ImGui::SliderFloat("Face Slimming", &face_slim_strength_, 0.0f, 10.0f)) {
    reshape_filter_->SetFaceSlimLevel(face_slim_strength_ / 200.0f);
  }

  if (ImGui::SliderFloat("Eye Enlarging", &eye_enlarge_strength_, 0.0f,
                         10.0f)) {
    reshape_filter_->SetEyeZoomLevel(eye_enlarge_strength_ / 100.0f);
  }

  if (ImGui::SliderFloat("Lipstick", &lipstick_strength_, 0.0f, 10.0f)) {
    lipstick_filter_->SetBlendLevel(lipstick_strength_ / 10.0f);
  }

  if (ImGui::SliderFloat("Blusher", &blusher_strength_, 0.0f, 10.0f)) {
    blusher_filter_->SetBlendLevel(blusher_strength_ / 10.0f);
  }

  ImGui::End();
}

// Convert OpenCV Mat to RGBA and feed to GPUPixel
void ProcessCameraFrame() {
  if (!use_camera_) {
    return;
  }
  
  cv::Mat frame;
  {
    std::lock_guard<std::mutex> lock(camera_frame_mutex_);
    if (current_camera_frame_.empty()) {
      return;
    }
    frame = current_camera_frame_.clone();
  }
  
  // Convert BGR to RGBA
  cv::Mat rgba_frame;
  cv::cvtColor(frame, rgba_frame, cv::COLOR_BGR2RGBA);
  
  // Feed to GPUPixel source
  source_camera_->SetRAWData(rgba_frame.data, 
                           rgba_frame.cols, 
                           rgba_frame.rows, 
                           rgba_frame.cols * 4);
}

// Render RGBA data to screen (same as original)
void RenderRGBAToScreen(const uint8_t* rgba_data, int width, int height) {
  if (!rgba_data || width <= 0 || height <= 0) {
    return;
  }

  int window_width, window_height;
  glfwGetFramebufferSize(main_window_, &window_width, &window_height);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, window_width, window_height);

  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba_data);

#ifdef __APPLE__
  const char* vertexShaderSource = R"(
    #version 330 core
    layout (location = 0) in vec3 aPos;
    layout (location = 1) in vec2 aTexCoord;
    out vec2 TexCoord;
    void main() {
      gl_Position = vec4(aPos, 1.0);
      TexCoord = aTexCoord;
    }
  )";

  const char* fragmentShaderSource = R"(
    #version 330 core
    in vec2 TexCoord;
    out vec4 FragColor;
    uniform sampler2D texture1;
    void main() {
      FragColor = texture(texture1, TexCoord);
    }
  )";
#else
  const char* vertexShaderSource = R"(
    #version 130
    attribute vec3 aPos;
    attribute vec2 aTexCoord;
    varying vec2 TexCoord;
    void main() {
      gl_Position = vec4(aPos, 1.0);
      TexCoord = aTexCoord;
    }
  )";

  const char* fragmentShaderSource = R"(
    #version 130
    varying vec2 TexCoord;
    uniform sampler2D texture1;
    void main() {
      gl_FragColor = texture2D(texture1, TexCoord);
    }
  )";
#endif

  GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
  glCompileShader(vertexShader);
  if (!CheckShaderErrors(vertexShader, "VERTEX")) {
    return;
  }

  GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
  glCompileShader(fragmentShader);
  if (!CheckShaderErrors(fragmentShader, "FRAGMENT")) {
    return;
  }

  GLuint shaderProgram = glCreateProgram();
  glAttachShader(shaderProgram, vertexShader);
  glAttachShader(shaderProgram, fragmentShader);
  glLinkProgram(shaderProgram);
  if (!CheckShaderErrors(shaderProgram, "PROGRAM")) {
    return;
  }

  glUseProgram(shaderProgram);

  float imageAspectRatio = (float)width / (float)height;
  float windowAspectRatio = 1280.0f / 720.0f;

  float scaleX = 1.0f;
  float scaleY = 1.0f;

  if (imageAspectRatio > windowAspectRatio) {
    scaleY = windowAspectRatio / imageAspectRatio;
  } else {
    scaleX = imageAspectRatio / windowAspectRatio;
  }

  float vertices[] = {
      -scaleX, -scaleY, 0.0f, 0.0f, 1.0f,
      scaleX,  -scaleY, 0.0f, 1.0f, 1.0f,
      scaleX,  scaleY,  0.0f, 1.0f, 0.0f,
      -scaleX, scaleY,  0.0f, 0.0f, 0.0f
  };

  unsigned int indices[] = {0, 1, 2, 2, 3, 0};

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);

  GLuint VAO, VBO, EBO;
  glGenVertexArrays(1, &VAO);
  glGenBuffers(1, &VBO);
  glGenBuffers(1, &EBO);

  glBindVertexArray(VAO);

  glBindBuffer(GL_ARRAY_BUFFER, VBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
               GL_STATIC_DRAW);

  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);

  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                        (void*)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);

  GLint texUniform = glGetUniformLocation(shaderProgram, "texture1");
  glUniform1i(texUniform, 0);

  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

  glDeleteVertexArrays(1, &VAO);
  glDeleteBuffers(1, &VBO);
  glDeleteBuffers(1, &EBO);
  glDeleteTextures(1, &texture);
  glDeleteProgram(shaderProgram);
  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);
}

// Render a single frame
void RenderFrame() {
  glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  UpdateFilterParametersFromUI();

  // Process camera frame
  ProcessCameraFrame();

  if (use_camera_) {
    // Process and render camera frame
    source_camera_->Render();

    const uint8_t* rgba_data = sink_raw_data_->GetRgbaBuffer();
    int width = sink_raw_data_->GetWidth();
    int height = sink_raw_data_->GetHeight();

    if (rgba_data && width > 0 && height > 0) {
      RenderRGBAToScreen(rgba_data, width, height);
    }
  }

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  glfwSwapBuffers(main_window_);
  glfwPollEvents();
}

// Clean up resources
void CleanupResources() {
  if (camera_) {
    camera_->Stop();
    camera_.reset();
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  if (main_window_) {
    glfwDestroyWindow(main_window_);
    main_window_ = nullptr;
  }
  glfwTerminate();
}

int main() {
#ifdef _WIN32
  std::string exePath = GetExecutablePath();
  char dllDir[MAX_PATH];
  sprintf_s(dllDir, MAX_PATH, "%s\\..\\lib", exePath.c_str());
  SetDllDirectoryA(dllDir);
#endif

  if (!SetupGlfwWindow()) {
    return -1;
  }

  SetupImGui();
  SetupFilterPipeline();

  while (!glfwWindowShouldClose(main_window_)) {
    RenderFrame();
  }

  CleanupResources();
  return 0;
} 