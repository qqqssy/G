#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <iostream>
#include <vector>
#include <string>
#include <random>

// --- 全局配置 ---
const unsigned int SCREEN_WIDTH = 1600;
const unsigned int SCREEN_HEIGHT = 900;
const unsigned int MAX_ELEMENTS = 1000000; // 最大支持一百万个元素

// --- 渲染模式 ---
enum RenderMode {
    MICRO_BATCH_INDIRECT = 0,
    INSTANCED_INDIRECT = 1
};

// --- GLSL着色器源码 ---

// 用于生成 "微批次" 指令的计算着色器 (模拟你的现状)
const char* cull_microbatch_cs_source = R"(
#version 450 core
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

struct InstanceData {
    vec2 position;
    vec2 size;
    vec4 color;
};

struct DrawElementsIndirectCommand {
    uint count;
    uint instanceCount;
    uint firstIndex;
    uint baseVertex;
    uint baseInstance;
};

layout(std430, binding = 0) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(std430, binding = 1) writeonly buffer DrawCommandBuffer {
    DrawElementsIndirectCommand commands[];
};

layout(binding = 2, offset = 0) uniform atomic_uint visible_count;

uniform uint total_element_count;
uniform mat4 projection; // 用于简单剔除

void main() {
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= total_element_count) {
        return;
    }

    InstanceData inst = instances[gid];
    
    // 简单的视锥剔除
    vec4 clip_pos = projection * vec4(inst.position, 0.0, 1.0);
    bool is_visible = (clip_pos.x >= -clip_pos.w && clip_pos.x <= clip_pos.w &&
                       clip_pos.y >= -clip_pos.w && clip_pos.y <= clip_pos.w);

    if (is_visible) {
        uint index = atomicCounterIncrement(visible_count);
        // 为每个可见元素生成一个独立的DrawCommand
        commands[index].count = 6; // Quad有6个索引
        commands[index].instanceCount = 1; // 每个DrawCall只画1个实例
        commands[index].firstIndex = 0;
        commands[index].baseVertex = 0;
        // 把当前元素的ID塞到baseInstance里，给VS用
        commands[index].baseInstance = gid;
    }
}
)";

// 用于生成 "实例化" 指令的计算着色器 (优化方案)
const char* cull_instanced_cs_source = R"(
#version 450 core
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

struct InstanceData {
    vec2 position;
    vec2 size;
    vec4 color;
};

struct DrawElementsIndirectCommand {
    uint count;
    uint instanceCount;
    uint firstIndex;
    uint baseVertex;
    uint baseInstance;
};

layout(std430, binding = 0) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

// 输出一个只包含可见元素ID的列表
layout(std430, binding = 1) writeonly buffer VisibleIDBuffer {
    uint visible_ids[];
};

layout(std430, binding = 2) writeonly buffer DrawCommandBuffer {
    DrawElementsIndirectCommand command; // 注意：不是数组，只有一个！
};

layout(binding = 3, offset = 0) uniform atomic_uint visible_count;

uniform uint total_element_count;
uniform mat4 projection;

void main() {
    // 在第一次调用时，由第一个线程来重置指令
    if (gl_GlobalInvocationID.x == 0) {
        command.count = 6;
        command.instanceCount = 0; // 先设为0，由原子计数器填充
        command.firstIndex = 0;
        command.baseVertex = 0;
        command.baseInstance = 0;
    }

    uint gid = gl_GlobalInvocationID.x;
    if (gid >= total_element_count) {
        return;
    }

    InstanceData inst = instances[gid];
    vec4 clip_pos = projection * vec4(inst.position, 0.0, 1.0);
    bool is_visible = (clip_pos.x >= -clip_pos.w && clip_pos.x <= clip_pos.w &&
                       clip_pos.y >= -clip_pos.w && clip_pos.y <= clip_pos.w);

    if (is_visible) {
        uint index = atomicCounterIncrement(visible_count);
        visible_ids[index] = gid;
    }
}
)";


// 顶点着色器
const char* render_vs_source = R"(
#version 450 core
layout (location = 0) in vec2 a_pos; // 基础Quad的顶点位置 (-0.5 to 0.5)

struct InstanceData {
    vec2 position;
    vec2 size;
    vec4 color;
};

layout(std430, binding = 0) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

// 仅在实例化模式下使用
layout(std430, binding = 1) readonly buffer VisibleIDBuffer {
    uint visible_ids[];
};

uniform mat4 projection;
uniform bool is_instanced_mode;

out vec4 v_color;

void main() {
    uint instance_id;
    if (is_instanced_mode) {
        // 实例化模式: 通过gl_InstanceID间接查找真正的元素ID
        instance_id = visible_ids[gl_InstanceID];
    } else {
        // 微批次模式: 直接从baseInstance获取元素ID
        instance_id = gl_BaseInstance;
    }
    
    InstanceData inst = instances[instance_id];
    
    v_color = inst.color;
    
    vec2 final_pos = a_pos * inst.size + inst.position;
    gl_Position = projection * vec4(final_pos, 0.0, 1.0);
}
)";

// 片元着色器
const char* render_fs_source = R"(
#version 450 core
in vec4 v_color;
out vec4 FragColor;

void main() {
    FragColor = v_color;
}
)";


// --- 主函数 ---
int main() {
    // ... (GLFW, GLAD, ImGui 初始化代码)
    // --- Boilerplate: Window, OpenGL, ImGui initialization ---
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Draw Call Performance Demo", NULL, NULL);
    if (window == NULL) { /*...*/ return -1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0); // VSync Off for performance measurement

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) { return -1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 450");
    
    // --- 数据准备 ---
    std::vector<glm::vec4> instance_positions_sizes;
    std::vector<glm::vec4> instance_colors;
    
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> pos_dist(-1.0f, 1.0f);
    std::uniform_real_distribution<float> size_dist(0.002f, 0.008f);
    std::uniform_real_distribution<float> color_dist(0.1f, 1.0f);

    struct InstanceData {
        glm::vec2 position;
        glm::vec2 size;
        glm::vec4 color;
    };
    std::vector<InstanceData> instance_cpu_data;
    instance_cpu_data.resize(MAX_ELEMENTS);
    for (int i = 0; i < MAX_ELEMENTS; ++i) {
        instance_cpu_data[i] = {
            {pos_dist(rng), pos_dist(rng)},
            {size_dist(rng), size_dist(rng)},
            {color_dist(rng), color_dist(rng), color_dist(rng), 1.0f}
        };
    }

    // --- OpenGL Buffer 设置 ---
    // 基础Quad VBO/EBO
    float quad_vertices[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };
    unsigned int quad_indices[] = { 0, 1, 2, 2, 3, 0 };
    GLuint quadVAO, quadVBO, quadEBO;
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glGenBuffers(1, &quadEBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // SSBOs
    GLuint instance_ssbo, visible_id_ssbo, command_buffer, counter_buffer;
    glGenBuffers(1, &instance_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, instance_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_ELEMENTS * sizeof(InstanceData), instance_cpu_data.data(), GL_DYNAMIC_DRAW);

    glGenBuffers(1, &visible_id_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, visible_id_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_ELEMENTS * sizeof(GLuint), nullptr, GL_DYNAMIC_DRAW);
    
    glGenBuffers(1, &command_buffer);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, command_buffer);
    glBufferData(GL_DRAW_INDIRECT_BUFFER, MAX_ELEMENTS * sizeof(GLuint) * 5, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &counter_buffer);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, counter_buffer);
    glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(GLuint), nullptr, GL_DYNAMIC_DRAW);

    // --- Shader编译 ---
    auto create_shader_program = [](const char* vs, const char* fs) {
        // ... (standard shader compilation code)
        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vertexShader, 1, &vs, NULL); glCompileShader(vertexShader);
        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fragmentShader, 1, &fs, NULL); glCompileShader(fragmentShader);
        GLuint shaderProgram = glCreateProgram(); glAttachShader(shaderProgram, vertexShader); glAttachShader(shaderProgram, fragmentShader); glLinkProgram(shaderProgram);
        glDeleteShader(vertexShader); glDeleteShader(fragmentShader); return shaderProgram;
    };
    auto create_compute_program = [](const char* cs) {
        // ... (standard compute shader compilation code)
        GLuint computeShader = glCreateShader(GL_COMPUTE_SHADER); glShaderSource(computeShader, 1, &cs, NULL); glCompileShader(computeShader);
        GLuint shaderProgram = glCreateProgram(); glAttachShader(shaderProgram, computeShader); glLinkProgram(shaderProgram);
        glDeleteShader(computeShader); return shaderProgram;
    };
    
    GLuint cull_microbatch_program = create_compute_program(cull_microbatch_cs_source);
    GLuint cull_instanced_program = create_compute_program(cull_instanced_cs_source);
    GLuint render_program = create_shader_program(render_vs_source, render_fs_source);

    // --- 主循环 ---
    RenderMode current_mode = MICRO_BATCH_INDIRECT;
    int element_count = 100000;
    float frame_time = 0.0f;
    unsigned int gpu_draw_calls = 0;

    while (!glfwWindowShouldClose(window)) {
        double current_time = glfwGetTime();

        glfwPollEvents();
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // --- UI ---
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::Begin("Performance Demo");
        ImGui::Text("Render Mode:");
        ImGui::RadioButton("Micro-Batch Indirect (现状)", (int*)&current_mode, MICRO_BATCH_INDIRECT);
        ImGui::RadioButton("Instanced Indirect (优化)", (int*)&current_mode, INSTANCED_INDIRECT);
        ImGui::SliderInt("Element Count", &element_count, 1000, MAX_ELEMENTS);
        ImGui::Separator();
        ImGui::Text("--- Stats ---");
        ImGui::Text("FPS: %.1f", 1.0f / frame_time);
        ImGui::Text("Frame Time: %.3f ms", frame_time * 1000.0f);
        ImGui::Text("GPU Draw Commands: %u", gpu_draw_calls);
        ImGui::End();

        // --- 剔除与指令生成 ---
        GLuint zero = 0;
        glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, counter_buffer);
        glBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(GLuint), &zero);

        glm::mat4 projection = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, instance_ssbo);
        glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, (current_mode == MICRO_BATCH_INDIRECT ? 2 : 3), counter_buffer);
        
        unsigned int num_groups = (element_count + 255) / 256;

        if (current_mode == MICRO_BATCH_INDIRECT) {
            glUseProgram(cull_microbatch_program);
            glUniform1i(glGetUniformLocation(cull_microbatch_program, "total_element_count"), element_count);
            glUniformMatrix4fv(glGetUniformLocation(cull_microbatch_program, "projection"), 1, GL_FALSE, glm::value_ptr(projection));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, command_buffer);
            glDispatchCompute(num_groups, 1, 1);
        } else { // INSTANCED_INDIRECT
            glUseProgram(cull_instanced_program);
            glUniform1i(glGetUniformLocation(cull_instanced_program, "total_element_count"), element_count);
            glUniformMatrix4fv(glGetUniformLocation(cull_instanced_program, "projection"), 1, GL_FALSE, glm::value_ptr(projection));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, visible_id_ssbo);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, command_buffer);
            glDispatchCompute(num_groups, 1, 1);
        }
        
        glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_ATOMIC_COUNTER_BARRIER_BIT);
        
        // --- 渲染 ---
        glUseProgram(render_program);
        glUniformMatrix4fv(glGetUniformLocation(render_program, "projection"), 1, GL_FALSE, glm::value_ptr(projection));
        glBindVertexArray(quadVAO);

        if (current_mode == MICRO_BATCH_INDIRECT) {
            glUniform1i(glGetUniformLocation(render_program, "is_instanced_mode"), 0);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, command_buffer);
            // 读取可见数量
            glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, counter_buffer);
            GLuint* count_ptr = (GLuint*)glMapBufferRange(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(GLuint), GL_MAP_READ_BIT);
            gpu_draw_calls = count_ptr[0];
            glUnmapBuffer(GL_ATOMIC_COUNTER_BUFFER);

            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, (void*)0, gpu_draw_calls, 0);
        } else { // INSTANCED_INDIRECT
            glUniform1i(glGetUniformLocation(render_program, "is__instanced_mode"), 1);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, visible_id_ssbo);

            // 关键：将原子计数器的值，写入到我们生成的唯一一个DrawCommand的instanceCount字段中
            // 这通常在CS的结尾做，或者用一个小的专用CS，这里为了简单直接用glCopyBufferSubData
            glBindBuffer(GL_COPY_READ_BUFFER, counter_buffer);
            glBindBuffer(GL_COPY_WRITE_BUFFER, command_buffer);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, sizeof(GLuint), sizeof(GLuint)); // a bit of a hack for demo

            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, command_buffer);
            glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, (void*)0);
            gpu_draw_calls = 1; // 只有一个间接绘制调用
        }

        // --- 渲染UI和交换缓冲 ---
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        frame_time = glfwGetTime() - current_time;
    }

    // --- 清理 ---
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}