/*
 * Minimal stand-in for the NDK's <GLES2/gl2.h>. See EGL/egl.h's file header: these bodies
 * (android_stub.c) are link-only no-ops and are never reached at runtime by any test here,
 * since the render thread never gets a real EGL surface. Not a stand-in for the real GPU
 * blend path's pixel correctness.
 */
#ifndef ANDROID_STUB_GLES2_GL2_H
#define ANDROID_STUB_GLES2_GL2_H

typedef unsigned int GLuint, GLenum;
typedef int          GLint, GLsizei;
typedef float        GLfloat;

enum {
    GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, GL_COMPILE_STATUS, GL_LINK_STATUS,
    GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_LINEAR,
    GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE, GL_COLOR_BUFFER_BIT,
    GL_BLEND, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_UNPACK_ALIGNMENT,
    GL_RGBA, GL_UNSIGNED_BYTE, GL_FLOAT, GL_FALSE, GL_TRIANGLE_STRIP
};

GLuint glCreateShader(GLenum type);
void   glShaderSource(GLuint shader, GLsizei count, const char *const *string, const GLint *length);
void   glCompileShader(GLuint shader);
void   glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
void   glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, char *infoLog);
GLuint glCreateProgram(void);
void   glAttachShader(GLuint program, GLuint shader);
void   glLinkProgram(GLuint program);
void   glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void   glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, char *infoLog);
void   glDeleteShader(GLuint shader);
void   glDeleteProgram(GLuint program);
GLint  glGetAttribLocation(GLuint program, const char *name);
void   glGenTextures(GLsizei n, GLuint *textures);
void   glBindTexture(GLenum target, GLuint texture);
void   glDeleteTextures(GLsizei n, const GLuint *textures);
void   glTexParameteri(GLenum target, GLenum pname, GLint param);
void   glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void   glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void   glClear(unsigned mask);
void   glUseProgram(GLuint program);
void   glEnable(GLenum cap);
void   glDisable(GLenum cap);
void   glBlendFunc(GLenum sfactor, GLenum dfactor);
void   glPixelStorei(GLenum pname, GLint param);
void   glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                    GLint border, GLenum format, GLenum type, const void *pixels);
void   glVertexAttribPointer(GLuint index, GLint size, GLenum type, int normalized, GLsizei stride, const void *pointer);
void   glEnableVertexAttribArray(GLuint index);
void   glDisableVertexAttribArray(GLuint index);
void   glDrawArrays(GLenum mode, GLint first, GLsizei count);
#endif
