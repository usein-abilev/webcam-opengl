#include <glad/glad.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

static char *read_file(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (f == NULL) {
        perror(filename);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) < 0) goto read_failure;

    long fsize = ftell(f);
    if (fsize < 0) goto read_failure;
    if (fseek(f, 0, SEEK_SET) < 0) goto read_failure;

    char *content = malloc(fsize + 1);
    if (content == NULL) goto read_failure;

    fread(content, fsize, 1, f);
    if (ferror(f)) goto read_failure;

    content[fsize] = '\0';

    fclose(f);
    errno = 0;

    return content;
read_failure:
    if (f) {
        int preclose_error = errno;
        fclose(f);
        errno = preclose_error;
        return NULL;
    }
    if (content) {
        free(content);
    }
    return NULL;
}

static GLint compile_shader_file(const char *path, GLenum type, GLuint *shader) {
    const char *src = read_file(path);
    *shader = glCreateShader(type);
    glShaderSource(*shader, 1, &src, NULL);
    glCompileShader(*shader);

    GLint status;
    glGetShaderiv(*shader, GL_COMPILE_STATUS, &status);

    free((void *)src);
    return status != 0;
}

// Returns zero in case of any error (e.g. if shader loading or creation program failed)
GLuint load_shaders(const char *vert_path, const char *frag_path) {
    int errmsg_size;
    int success;
    char info[512];

    GLuint vert;
    if (!compile_shader_file(vert_path, GL_VERTEX_SHADER, &vert)) {
        glGetShaderInfoLog(vert, 512, &errmsg_size, info);
        fprintf(stderr, "error: could not compile vertex shader: %.*s\n",errmsg_size, info);
        return 0;
    }
    GLuint frag;
    if (!compile_shader_file(frag_path, GL_FRAGMENT_SHADER, &frag)) {
        glGetShaderInfoLog(frag, 512, &errmsg_size, info);
        fprintf(stderr, "error: cloud not compile fragment shader: %.*s\n", errmsg_size, info);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);

    glDeleteShader(vert);
    glDeleteShader(frag);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(program, 512, &errmsg_size, info);
        fprintf(stderr, "failed to create a program: %.*s\n", errmsg_size, info);
        return 0;
    }

    return program;
}
