#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <linux/videodev2.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define CAM_WIDTH 640
#define CAM_HEIGHT 480
#define CAM_FORMAT V4L2_PIX_FMT_YUYV
#define OUTPUT_FORMAT V4L2_PIX_FMT_RGB24

#define MAX_ZOOM_LEVEL 8.0

// TODO: move to shader header
GLuint load_shaders(const char *vert_path, const char *frag_path);

// AppContext stores parameters that meant to be passed into callbacks via glfwSetWindowUserPointer/glfwGetWindowUserPointer
typedef struct {
    float zoom_level;
    int rendering_mode;
    bool ctrl_pressed;
    bool shader_enabled;
} AppContext;

typedef struct {
    void *start;
    size_t length;
} CamBuffer;

int writecam(const char *file, int width, int height) {
    int wfd = open(file, O_WRONLY);
    if (wfd < 0) {
        perror("failed to open file write-only");
        exit(EXIT_FAILURE);
    }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = OUTPUT_FORMAT;
    if (ioctl(wfd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("v4l2 set format error");
        exit(EXIT_FAILURE);
    }

    return wfd;
}

int readcam(const char *file, int width, int height, int bufcount, CamBuffer *buffers) {
    int fd_in = open(file, O_RDWR);
    if (fd_in < 0) {
        perror("failed to open file");
        return -1;
    }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = CAM_FORMAT;

    if (ioctl(fd_in, VIDIOC_S_FMT, &fmt) == -1) {
        perror("v4l2 set format error");
        return -1;
    }

    struct v4l2_requestbuffers req = {0};
    req.count = bufcount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_in, VIDIOC_REQBUFS, &req) == -1) {
        perror("request buffer error");
        return -1;
    }

    for (unsigned int i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        // request offset of allocated memory from driver
        ioctl(fd_in, VIDIOC_QUERYBUF, &buf);

        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd_in, buf.m.offset);

        if (buffers[i].start == MAP_FAILED) {
            perror("mmap failure");
            return -1;
        }

        ioctl(fd_in, VIDIOC_QBUF, &buf);
    }

    return fd_in;
}

void yuyv_to_rgba(unsigned char *yuyv, unsigned char *rgba, int w, int h) {
    int pix_count = w * h;
    for (int i = 0, j = 0; i < pix_count; i += 2, j += 4) {
        int y0 = yuyv[j];
        int u = yuyv[j + 1] - 128;
        int y1 = yuyv[j + 2];
        int v = yuyv[j + 3] - 128;

#define CLIP(x) (unsigned char)((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))

        // pixel 1
        rgba[i * 4] = CLIP(y0 + 1.402 * v);
        rgba[i * 4 + 1] = CLIP(y0 - 0.344 * u - 0.714 * v);
        rgba[i * 4 + 2] = CLIP(y0 + 1.772 * u);
        rgba[i * 4 + 3] = 255;

        // pixel 2
        rgba[(i + 1) * 4] = CLIP(y1 + 1.402 * v);
        rgba[(i + 1) * 4 + 1] = CLIP(y1 - 0.344 * u - 0.714 * v);
        rgba[(i + 1) * 4 + 2] = CLIP(y1 + 1.772 * u);
        rgba[(i + 1) * 4 + 3] = 255;
#undef CLIP
    }
}

void flip_buffer_vertical(unsigned char *data, int width, int height, int channels) {
    int row_size = width * channels;
    unsigned char *temp_row = malloc(row_size);
    for (int i = 0; i < height / 2; i++) {
        unsigned char *top_row = data + (i * row_size);
        unsigned char *bottom_row = data + ((height - 1 - i) * row_size);

        memcpy(temp_row, top_row, row_size);
        memcpy(top_row, bottom_row, row_size);
        memcpy(bottom_row, temp_row, row_size);
    }
    free(temp_row);
}

GLuint create_opengl_texture(unsigned char *buffer, int width, int height) {
    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);
    if (!texture_id) {
        fprintf(stderr, "create_opengl_texture(): failed to generate texture\n");
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer);

    GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        fprintf(stderr, "create_opengl_texture(): glTexImage2D error\n");
        return 0;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    return texture_id;
}

void init_vertex_buffers(GLuint *VAO, GLuint* VBO, GLuint* EBO) {
    float vertices[] = {
        1.0f, 1.0f, 0.0f, 1.0f, 1.0f,   // top right
        1.0f, -1.0f, 0.0f, 1.0f, 0.0f,  // bottom right
        -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, // bottom left
        -1.0f, 1.0f, 0.0f, 0.0f, 1.0f   // top left
    };
    unsigned int indices[] = {
        0, 1, 3, // first triangle
        1, 2, 3  // second triangle
    };
    glGenVertexArrays(1, VAO);
    glGenBuffers(1, VBO);
    glGenBuffers(1, EBO);

    glBindVertexArray(*VAO);

    glBindBuffer(GL_ARRAY_BUFFER, *VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, *EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
}

void keyboard_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    AppContext* context = (AppContext*)glfwGetWindowUserPointer(window);

    if (key == GLFW_KEY_LEFT_CONTROL) {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            printf("Left control is pressed\n");
            context->ctrl_pressed = true;
        } else {
            context->ctrl_pressed = false;
        }
    }

    if (action == GLFW_PRESS) {
        switch (key) {
            case GLFW_KEY_R:
            case GLFW_KEY_0:
                context->rendering_mode = 0;
                break;
            case GLFW_KEY_1:
                context->rendering_mode = 1;
                break;
            case GLFW_KEY_2:
                context->rendering_mode = 2;
                break;
            case GLFW_KEY_3:
                context->rendering_mode = 3;
                break;
            case GLFW_KEY_4:
                context->rendering_mode = 4;
                break;
        }
    }
}

void scroll_callback(GLFWwindow*window, double x, double y) {
    AppContext* context = (AppContext*)glfwGetWindowUserPointer(window);
    if (!context) {
        printf("scroll_callback: AppContext is lost\n");
        return;
    }

    float zoom_offset = y / 3;
    if (context->ctrl_pressed) {
        zoom_offset *= 2;
    }
    if (context->zoom_level + zoom_offset >= 0.1 && context->zoom_level + zoom_offset <= MAX_ZOOM_LEVEL) {
        context->zoom_level += zoom_offset;
    }
    printf("scroll event (offset: %f, zoom: %f)\n", zoom_offset, context->zoom_level);
}

int main(void) {
    if (!glfwInit()) {
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(CAM_WIDTH, CAM_HEIGHT, "Webcam Shader", NULL, NULL);
    if (!window) {
        printf("failed to initialize GLFW window\n");
        glfwTerminate();
        return -1;
    }
    glfwSetWindowAttrib(window, GLFW_RESIZABLE, false);
    glfwMakeContextCurrent(window);

    AppContext* app_context = malloc(sizeof(AppContext));
    if (app_context == NULL) {
        fprintf(stderr, "seriously? okay, i don't blame you, the ram prices are insane\n");
        exit(EXIT_FAILURE);
    }
    app_context->zoom_level = 1.0;
    app_context->rendering_mode = 1;

    // the user pointer is needed to provide the access to our app context inside callbacks
    glfwSetWindowUserPointer(window, app_context);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetKeyCallback(window, keyboard_callback);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        printf("failed to initialize GLAD\n");
        return -1;
    }

    unsigned int fd_in_bufcount = 3;
    CamBuffer *fd_buffers = calloc(fd_in_bufcount, sizeof(*fd_buffers));

    int fd_in = readcam("/dev/video1", CAM_WIDTH, CAM_HEIGHT, fd_in_bufcount, fd_buffers);
    int fd_out = writecam("/dev/video0", CAM_WIDTH, CAM_HEIGHT); // get access to virtual cam
    assert(fd_in != -1);
    assert(fd_out != -1);
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    assert(ioctl(fd_in, VIDIOC_STREAMON, &type) != -1);

    // setup shaders
    GLuint shader_program = load_shaders("./shaders/vertex.glsl", "./shaders/ascii.frag");
    assert(shader_program != 0);

    // init vertices and relation buffers vao+vbo+ebo
    GLuint VAO, VBO, EBO;
    init_vertex_buffers(&VAO, &VBO, &EBO);

    // define a_pos attr
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);

    // define a_texcoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // setup main camera texture to be rendered
    GLuint cam_texture = create_opengl_texture(NULL, CAM_WIDTH, CAM_HEIGHT);

    int atlas_width, atlas_height, atlas_channels;
    stbi_uc* atlas_glyphs_buffer = stbi_load(
            "./assets/ascii.png", 
            &atlas_width, &atlas_height, 
            &atlas_channels, 0
    );
    if (!atlas_glyphs_buffer) {
        fprintf(stderr, "error: cannot open glyph image\n");
        exit(EXIT_FAILURE);
    }
    printf("atlas image loaded: (w: %i, h: %i, ch: %i)\n", atlas_width, atlas_height, atlas_channels);
    GLuint atlas_texture = create_opengl_texture(atlas_glyphs_buffer, atlas_width, atlas_height);

    int atlas_edges_w, atlas_edges_h, atlas_edges_channels;
    stbi_uc* atlas_edges_buffer = stbi_load(
            "./assets/ascii_edges.png", 
            &atlas_edges_w, &atlas_edges_h, 
            &atlas_edges_channels, 3
    );
    if (!atlas_edges_buffer) {
        fprintf(stderr, "error: cannot open atlas image\n");
        exit(EXIT_FAILURE);
    }
    printf("atlas edges image loaded: (w: %i, h: %i, ch: %i)\n", atlas_edges_w, atlas_edges_h, atlas_edges_channels);
    GLuint atlas_edges_texture = create_opengl_texture(atlas_edges_buffer, atlas_edges_w, atlas_edges_h);

    glUseProgram(shader_program);
    glUniform1i(glGetUniformLocation(shader_program, "cam_texture"), 0);
    glUniform1i(glGetUniformLocation(shader_program, "atlas_texture"), 1);
    glUniform1i(glGetUniformLocation(shader_program, "atlas_edges_texture"), 2);
    
    GLuint width_loc = glGetUniformLocation(shader_program, "width");
    GLuint height_loc = glGetUniformLocation(shader_program, "height");
    GLuint time_loc = glGetUniformLocation(shader_program, "time");
    GLuint zoom_loc = glGetUniformLocation(shader_program, "zoom");
    GLuint mode_loc = glGetUniformLocation(shader_program, "mode");

    float delta_time = 0.0f;
    float last_time = 0.0f;

    // setup RGBA buffers for camera frames and OpenGL pixels
    static const size_t rgba_buffer_size = CAM_WIDTH * CAM_HEIGHT * 4;
    unsigned char *rgba_buffer = malloc(rgba_buffer_size);

    while (!glfwWindowShouldClose(window)) {
        float current_time = (float)glfwGetTime();
        if (last_time == 0) {
            last_time = current_time;
            continue;
        }
        delta_time = current_time - last_time;
        last_time = current_time;

        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_in, VIDIOC_DQBUF, &buf) == -1) {
            break;
        }

        yuyv_to_rgba(fd_buffers[buf.index].start, rgba_buffer, CAM_WIDTH, CAM_HEIGHT);

        // upload frame to gpu
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, cam_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CAM_WIDTH, CAM_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, rgba_buffer);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, atlas_texture);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, atlas_edges_texture);

        // render
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(shader_program);

        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

        glUniform1f(width_loc, CAM_WIDTH);
        glUniform1f(height_loc, CAM_HEIGHT);
        glUniform1f(time_loc, delta_time);
        glUniform1f(zoom_loc, app_context->zoom_level);
        glUniform1ui(mode_loc, app_context->rendering_mode);

        glfwPollEvents();

        // TODO: currently, glfwSwapBuffers blocks main thread when minimized/hidden 
        // the one way to get around this is to use Wayland event callbacks 
        // Ref: https://emersion.fr/blog/2018/wayland-rendering-loop/
        glfwSwapBuffers(window);

        memset(rgba_buffer, 0, rgba_buffer_size);
        glReadPixels(0, 0, CAM_WIDTH, CAM_HEIGHT, GL_RGB, GL_UNSIGNED_BYTE, rgba_buffer);

        flip_buffer_vertical(rgba_buffer, CAM_WIDTH, CAM_HEIGHT, 4); // rgba

        if (write(fd_out, rgba_buffer, rgba_buffer_size) == -1) {
            perror("Failed to write data to virtual cam");
            break;
        }

        if (ioctl(fd_in, VIDIOC_QBUF, &buf) < 0) {
            printf("Failed to requeue buffer\n");
        }
    }

    // stop video capturing
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    assert(ioctl(fd_in, VIDIOC_STREAMOFF, &type) != -1);

    // unmap memory and close file descriptors
    for (unsigned int i = 0; i < fd_in_bufcount; i++) {
        assert(munmap(fd_buffers[i].start, fd_buffers[i].length) != -1);
    }
    close(fd_in);
    if (fd_out > 0)
        close(fd_out);

    free(app_context);
    free(rgba_buffer);
    stbi_image_free(atlas_glyphs_buffer);
    stbi_image_free(atlas_edges_buffer);

    // delete OpenGL objects before terminating the context
    glDeleteTextures(1, &cam_texture);
    glDeleteTextures(1, &atlas_texture);
    glDeleteTextures(1, &atlas_edges_texture);

    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteBuffers(1, &EBO);
    glDeleteProgram(shader_program);

    glfwTerminate();
    return 0;
}
