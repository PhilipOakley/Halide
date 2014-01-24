#include "mini_stdint.h"
#include "../buffer_t.h"
#include "HalideRuntime.h"

#include "posix_allocator.cpp"
#include "posix_io.cpp"
#include "posix_error_handler.cpp"
#include "mini_string.h"
#include "mini_opengl.h"

// This function must be provided by the host environment to retrieve pointers
// to OpenGL API functions.
typedef void (*GLFUNCPTR)();
extern "C" WEAK GLFUNCPTR halide_opengl_get_proc_address(const char* name) {
    return NULL;
}

// List of all OpenGL functions used by the runtime. The list is used to
// declare and initialize the corresponding tables below.
#define USED_GL_FUNCTIONS                                               \
    GLFUNC(PFNGLDELETETEXTURESPROC, DeleteTextures);                    \
    GLFUNC(PFNGLGENTEXTURESPROC, GenTextures);                          \
    GLFUNC(PFNGLBINDTEXTUREPROC, BindTexture);                          \
    GLFUNC(PFNGLGETERRORPROC, GetError);                                \
    GLFUNC(PFNGLMATRIXMODEPROC, MatrixMode);                            \
    GLFUNC(PFNGLLOADIDENTITYPROC, LoadIdentity);                        \
    GLFUNC(PFNGLORTHOPROC, Ortho);                                      \
    GLFUNC(PFNGLVIEWPORTPROC, Viewport);                                \
    GLFUNC(PFNGLGENBUFFERSPROC, GenBuffers);                            \
    GLFUNC(PFNGLDELETEBUFFERSPROC, DeleteBuffers);                      \
    GLFUNC(PFNGLBINDBUFFERPROC, BindBuffer);                            \
    GLFUNC(PFNGLBUFFERDATAPROC, BufferData);                            \
    GLFUNC(PFNGLTEXPARAMETERIPROC, TexParameteri);                      \
    GLFUNC(PFNGLTEXIMAGE2DPROC, TexImage2D);                            \
    GLFUNC(PFNGLGETTEXIMAGEPROC, GetTexImage);                          \
    GLFUNC(PFNGLTEXSUBIMAGE2DPROC, TexSubImage2D);                      \
    GLFUNC(PFNGLDISABLEPROC, Disable);                                  \
    GLFUNC(PFNGLCREATESHADERPROC, CreateShader);                        \
    GLFUNC(PFNGLACTIVETEXTUREPROC, ActiveTexture);                      \
    GLFUNC(PFNGLSHADERSOURCEPROC, ShaderSource);                        \
    GLFUNC(PFNGLCOMPILESHADERPROC, CompileShader);                      \
    GLFUNC(PFNGLGETSHADERIVPROC, GetShaderiv);                          \
    GLFUNC(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog);                \
    GLFUNC(PFNGLDELETESHADERPROC, DeleteShader);                        \
    GLFUNC(PFNGLCREATEPROGRAMPROC, CreateProgram);                      \
    GLFUNC(PFNGLATTACHSHADERPROC, AttachShader);                        \
    GLFUNC(PFNGLLINKPROGRAMPROC, LinkProgram);                          \
    GLFUNC(PFNGLGETPROGRAMIVPROC, GetProgramiv);                        \
    GLFUNC(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog);              \
    GLFUNC(PFNGLUSEPROGRAMPROC, UseProgram);                            \
    GLFUNC(PFNGLDELETEPROGRAMPROC, DeleteProgram);                      \
    GLFUNC(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation);            \
    GLFUNC(PFNGLUNIFORM1IVPROC, Uniform1iv);                            \
    GLFUNC(PFNGLUNIFORM2IVPROC, Uniform2iv);                            \
    GLFUNC(PFNGLUNIFORM1FVPROC, Uniform1fv);                            \
    GLFUNC(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers);                  \
    GLFUNC(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers);            \
    GLFUNC(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus);    \
    GLFUNC(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer);                  \
    GLFUNC(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D);        \
    GLFUNC(PFNGLDRAWBUFFERSPROC, DrawBuffers);                          \
    GLFUNC(PFNGLGETATTRIBLOCATIONPROC, GetAttribLocation);              \
    GLFUNC(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer);          \
    GLFUNC(PFNGLDRAWELEMENTSPROC, DrawElements);                        \
    GLFUNC(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray);  \
    GLFUNC(PFNGLDISABLEVERTEXATTRIBARRAYPROC, DisableVertexAttribArray)



// ---------- Types ----------

#define ARG_NONE 0
#define ARG_BUFFER 1
#define ARG_FLOAT 2
#define ARG_INT 3
struct HalideOpenGLArgument {
    char* name;
    int type;
    bool is_output;
    HalideOpenGLArgument *next;
};

struct HalideOpenGLKernel {
    char* source;
    char* name;
    HalideOpenGLArgument *arguments;
    GLuint shader_id;
    GLuint program_id;
    HalideOpenGLKernel *next;
};

// Information about each texture accessed by any shader.
struct HalideOpenGLTexture {
    GLuint id;
    GLint min[4];
    GLint extent[4];
    GLenum format;                      // internal format: GL_RGBA32F, ...
    bool halide_allocated;              // allocated by us or host app?
    HalideOpenGLTexture* next;
};

// All persistant state maintained by the runtime.
struct HalideOpenGLState {
    bool initialized;

    // Various objects shared by all filter kernels
    GLuint vertex_shader_id;
    GLuint framebuffer_id;
    GLuint vertex_buffer;
    GLuint element_buffer;

    // A list of all defined kernels
    HalideOpenGLKernel* kernels;

    // A list of all textures that are still active
    HalideOpenGLTexture* textures;

    // Declare pointers used OpenGL functions
#define GLFUNC(PTYPE,VAR) PTYPE VAR
    USED_GL_FUNCTIONS;
#undef GLFUNC
};

// ---------- Static variables ----------

WEAK HalideOpenGLState halide_opengl_state;

static const char* vertex_shader_src =
    "#version 120\n"
    "attribute vec2 position;\n"
    "varying vec2 pixcoord;\n"
    "uniform ivec2 output_min;\n"
    "uniform ivec2 output_extent;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "    vec2 texcoord = 0.5 * position + 0.5;\n"
    "    pixcoord = floor(texcoord * output_extent) + output_min;\n"
    "}\n";

static const char kernel_marker[] = "/// KERNEL ";
static const char input_marker[] = "/// IN ";
static const char output_marker[] = "/// OUT ";

// Vertex coordinates for unit square
static const GLfloat square_vertices[] = {
    -1.0f, -1.0f,
    1.0f, -1.0f,
    -1.0f, 1.0f,
    1.0f, 1.0f
};

// Order of vertices in vertex_buffer_data for triangle strip forming the unit
// square.
static const GLuint square_indices[] = { 0, 1, 2, 3 };

// ---------- Macros ----------

// Convenience macro for accessing state of the OpenGL runtime
#define ST halide_opengl_state

// Ensure that OpenGL runtime is correctly initialized. Used in all public API
// functions.
#define ASSERT_INITIALIZED halide_assert(uctx, ST.initialized && "OpenGL runtime not initialized.")

// Macro for error checking.
#ifndef DEBUG
#  define CHECK_GLERROR()
#else
#  define CHECK_GLERROR() {                                     \
        GLenum err;                                             \
        if ((err = ST.GetError()) != GL_NO_ERROR) {             \
            halide_printf(uctx,                                 \
                          "%s:%d: OpenGL error 0x%04x\n",       \
                          __FILE__, __LINE__, err);             \
        }}
#endif // DEBUG


// ---------- Helper functions ----------

// Note: all function that directly or indirectly access the runtime state in
// halide_opengl_state must be declared as WEAK, otherwise the behavior at
// runtime is undefined.

static char* halide_strndup(void* uctx, const char* s, size_t n) {
    char* p = (char*)halide_malloc(uctx, n+1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static GLuint get_texture_id(buffer_t* buf) {
    return buf->dev & 0xffffffff;
}

static void set_texture_id(void* uctx, buffer_t* buf, GLuint texture) {
    buf->dev = texture & 0xffffffff;
    halide_assert(uctx, buf->dev == texture && "Texture ID larger than 32 bit");
}

WEAK GLuint halide_opengl_make_shader(void* uctx, GLenum type,
                                      const char* source, GLint* length) {
    GLuint shader = ST.CreateShader(type);
    ST.ShaderSource(shader, 1, (const GLchar **)&source, length);
    ST.CompileShader(shader);

    GLint shader_ok = 0;
    ST.GetShaderiv(shader, GL_COMPILE_STATUS, &shader_ok);
    if (!shader_ok) {
        halide_printf(uctx, "Could not compile shader:\n");
        GLint log_len;
        ST.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        char* log = (char*) halide_malloc(uctx, log_len);
        ST.GetShaderInfoLog(shader, log_len, NULL, log);
        halide_printf(uctx, "%s", log);
        halide_free(uctx, log);
        ST.DeleteShader(shader);
        return 0;
    }
    return shader;
}

static void check_buffer_properties(void* uctx, buffer_t* buf, int* w, int* h) {
    halide_assert(uctx, buf->extent[2] <= 4 &&
                  "Only up to 4 color channels are supported");
    halide_assert(uctx, buf->extent[3] <= 1 &&
                  "3D textures are not supported");
    // Minimum size of texture: 1x1
    *w = buf->extent[0];
    *h = buf->extent[1];
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

// Check whether string starts with a given prefix.
// Returns pointer to character after matched prefix if successful or NULL.
static const char* match_prefix(const char *s, const char* prefix) {
    if (0 == strncmp(s, prefix, strlen(prefix)))
        return s + strlen(prefix);
    return NULL;
}

// Parse declaration of the form "(float|int|buffer) name" and construct
// matching HalideOpenGLArgument.
static HalideOpenGLArgument* parse_argument(void* uctx, const char* src, const char* end) {
    const char *name;
    int type = ARG_NONE;
    if ((name = match_prefix(src, "float "))) {
        type = ARG_FLOAT;
    } else if ((name = match_prefix(src, "int "))) {
        type = ARG_INT;
    } else if ((name = match_prefix(src, "buffer "))) {
        type = ARG_BUFFER;
    }
    halide_assert(uctx, type != ARG_NONE && "Argument type not supported");

    HalideOpenGLArgument* arg = (HalideOpenGLArgument*)
        halide_malloc(uctx, sizeof(HalideOpenGLArgument));
    arg->name = halide_strndup(uctx, name, end-name);
    arg->type = type;
    arg->is_output = false;
    arg->next = 0;
    return arg;
}

// Create HalideOpenGLKernel for a piece of GLSL code
static HalideOpenGLKernel* create_kernel(void* uctx, const char* src, int size) {
    HalideOpenGLKernel* kernel =
        (HalideOpenGLKernel*)halide_malloc(uctx, sizeof(HalideOpenGLKernel));

    kernel->source = halide_strndup(uctx, src, size);
    kernel->name = NULL;
    kernel->arguments = NULL;
    kernel->shader_id = 0;
    kernel->program_id = 0;
    kernel->next = NULL;

    // Parse initial comment block
    const char *line = kernel->source;
    while (*line) {
        const char *next_line = strchr(line, '\n') + 1;
        if (!next_line)
            next_line = line + size;

        const char* args;
        if ((args = match_prefix(line, kernel_marker))) {
            kernel->name = halide_strndup(uctx, args, next_line - args - 1);
        } else if ((args = match_prefix(line, input_marker))) {
            HalideOpenGLArgument* arg = parse_argument(uctx, args, next_line-1);
            arg->next = kernel->arguments;
            kernel->arguments = arg;
        } else if ((args = match_prefix(line, output_marker))) {
            HalideOpenGLArgument* arg = parse_argument(uctx, args, next_line-1);
            arg->is_output = true;
            arg->next = kernel->arguments;
            kernel->arguments = arg;
        } else {
            // Stop parsing if we encounter something we don't recognize
            break;
        }
        line = next_line;
    }
    halide_assert(uctx, kernel->name && "Kernel name not found");

    // Arguments are currently in reverse order, flip the list.
    HalideOpenGLArgument* cur = kernel->arguments;
    kernel->arguments = NULL;
    while (cur) {
        HalideOpenGLArgument* next = cur->next;
        cur->next = kernel->arguments;
        kernel->arguments = cur;
        cur = next;
    }

    return kernel;
}

// Delete all data associated with a kernel. Also release associated OpenGL
// shader and program.
WEAK void halide_opengl_delete_kernel(void* uctx, HalideOpenGLKernel* kernel) {
    ST.DeleteProgram(kernel->program_id);
    ST.DeleteShader(kernel->shader_id);

    HalideOpenGLArgument *arg = kernel->arguments;
    while (arg) {
        HalideOpenGLArgument *next = arg->next;
        halide_free(uctx, arg);
        arg = next;
    }
    halide_free(uctx, kernel->name);
    halide_free(uctx, kernel);
}

// Find a kernel by name. Return NULL if not found.
WEAK HalideOpenGLKernel* halide_opengl_find_kernel(const char* name) {
    for (HalideOpenGLKernel* cur = ST.kernels; cur; cur = cur->next) {
        if (0 == strcmp(cur->name, name)) {
            return cur;
        }
    }
    return NULL;
}


extern "C" {

// Initialize the runtime, in particular all fields in halide_opengl_state.
WEAK void halide_opengl_init(void* uctx) {
    if (ST.initialized)
        return;

    // Initialize pointers to OpenGL functions.
#define GLFUNC(TYPE, VAR)                                               \
    ST.VAR = (TYPE)halide_opengl_get_proc_address("gl" #VAR);           \
    if (!ST.VAR) {                                                      \
        halide_printf(uctx, "Could not load function pointer for %s\n", "gl" #VAR); \
        return;                                                         \
    }
    USED_GL_FUNCTIONS;
#undef GLFUNC

    ST.kernels = NULL;
    ST.textures = NULL;

    // Initialize all OpenGL objects that are shared between kernels.
    ST.GenFramebuffers(1, &ST.framebuffer_id);
    CHECK_GLERROR();

    ST.vertex_shader_id = halide_opengl_make_shader(uctx,
        GL_VERTEX_SHADER, vertex_shader_src, NULL);
    halide_assert(uctx, ST.vertex_shader_id &&
                  "Failed to create vertex shader");

    GLuint buf;
    ST.GenBuffers(1, &buf);
    ST.BindBuffer(GL_ARRAY_BUFFER, buf);
    ST.BufferData(GL_ARRAY_BUFFER,
                  sizeof(square_vertices), square_vertices, GL_STATIC_DRAW);
    CHECK_GLERROR();
    ST.vertex_buffer = buf;

    ST.GenBuffers(1, &buf);
    ST.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf);
    ST.BufferData(GL_ELEMENT_ARRAY_BUFFER,
                  sizeof(square_indices), square_indices, GL_STATIC_DRAW);
    CHECK_GLERROR();
    ST.element_buffer = buf;

    ST.initialized = true;
}

// Release all data allocated by the runtime.
//
// The OpenGL context itself is generally managed by the host application, so
// we leave it untouched.
WEAK void halide_opengl_release(void* uctx) {
    ASSERT_INITIALIZED;
    ST.DeleteShader(ST.vertex_shader_id);
    ST.DeleteFramebuffers(1, &ST.framebuffer_id);

    HalideOpenGLKernel* cur = ST.kernels;
    while (cur) {
        HalideOpenGLKernel* next = cur->next;
        halide_opengl_delete_kernel(uctx, cur);
        cur = next;
    }
    halide_assert(uctx, ST.textures == NULL && "Not all textures have been freed");

    ST.DeleteBuffers(1, &ST.vertex_buffer);
    ST.DeleteBuffers(1, &ST.element_buffer);

    ST.vertex_shader_id = 0;
    ST.framebuffer_id = 0;
    ST.vertex_buffer = 0;
    ST.element_buffer = 0;
    ST.kernels = NULL;
    ST.initialized = false;
}

// Allocate a new texture matching the dimension and color format of the
// specified buffer.
WEAK void halide_opengl_dev_malloc(void* uctx, buffer_t* buf) {
    halide_opengl_init(uctx);

    halide_assert(uctx, buf && "Invalid buffer");

    // If the texture was already created by the host application, check that
    // it has the correct format. Otherwise, allocate and set up an
    // appropriate texture.
    GLuint tex = get_texture_id(buf);
    bool halide_allocated = false;
    GLint format = 0;
    if (tex != 0) {
        // TODO: check texture format
    } else {
        int w, h;
        check_buffer_properties(uctx, buf, &w, &h);

        // Generate texture ID
        ST.GenTextures(1, &tex);
        CHECK_GLERROR();

        // Set parameters for this texture: no interpolation and clamp to edges.
        ST.BindTexture(GL_TEXTURE_2D, tex);
        ST.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        ST.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        ST.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ST.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        CHECK_GLERROR();

        // Create empty texture here and fill it with glTexSubImage2D later.
        GLint type = GL_UNSIGNED_BYTE;
        if (buf->extent[2] <= 1) {
            format = GL_LUMINANCE;
        } else if (buf->extent[2] == 3) {
            format = GL_RGB;
        } else if (buf->extent[2] == 4) {
            format = GL_RGBA;
        } else {
            halide_assert(uctx, false && "Only 1, 3, or 4 color channels are supported");
        }

        if (buf->elem_size == 1) {
            type = GL_UNSIGNED_BYTE;
        } else if (buf->elem_size == 2) {
            type = GL_UNSIGNED_SHORT;
        } else {
            halide_printf(uctx, "%d\n", buf->elem_size);
            halide_assert(uctx, false && "Only uint8 and uint16 textures are supported");
        }

        ST.TexImage2D(GL_TEXTURE_2D, 0,
                      format,
                      w, h, 0,
                      format, type, NULL);
        CHECK_GLERROR();

        buf->dev = tex;
        halide_allocated = true;
#ifdef DEBUG
        halide_printf(uctx, "Allocated texture of size %dx%d\n", w, h);
#endif

        ST.BindTexture(GL_TEXTURE_2D, 0);
    }

    // Record main information about texture and remember it for later. In
    // halide_opengl_dev_run we are only given the texture ID and not the full
    // buffer_t, so we copy the interesting information here.
    HalideOpenGLTexture *texinfo = (HalideOpenGLTexture*)
        halide_malloc(uctx, sizeof(HalideOpenGLTexture));
    texinfo->id = tex;
    for (int i=0; i<3; i++) {
        texinfo->min[i] = buf->min[i];
        texinfo->extent[i] = buf->extent[i];
    }
    texinfo->format = format;
    texinfo->halide_allocated = halide_allocated;

    texinfo->next = ST.textures;
    ST.textures = texinfo;
}

WEAK HalideOpenGLTexture* halide_opengl_find_texture(GLuint tex) {
    HalideOpenGLTexture* texinfo = ST.textures;
    for (; texinfo; texinfo = texinfo->next)
        if (texinfo->id == tex)
            return texinfo;
    return NULL;
}

// Delete all texture information associated with a buffer. The OpenGL texture
// itself is only deleted if it was actually allocated by Halide and not
// provided by the host application.
WEAK void halide_opengl_dev_free(void* uctx, buffer_t* buf) {
    ASSERT_INITIALIZED;

    GLuint tex = get_texture_id(buf);
    if (tex == 0)
        return;

    // Look up corresponding HalideOpenGLTexture and unlink it from the list.
    HalideOpenGLTexture** ptr = &ST.textures;
    HalideOpenGLTexture* texinfo = *ptr;
    for (; texinfo != NULL; ptr = &texinfo->next, texinfo = *ptr) {
        if (texinfo->id == tex) {
            *ptr = texinfo->next;
            texinfo->next = NULL;
            break;
        }
    }
    halide_assert(uctx, texinfo && "Internal error: texture not found");

    // Delete texture if it was allocated by us.
    if (texinfo->halide_allocated) {
        ST.DeleteTextures(1, &tex);
        CHECK_GLERROR();
        buf->dev = 0;
    }

    halide_free(uctx, texinfo);
}

// Called at the beginning of a code block generated by Halide. This function
// is responsible for setting up the OpenGL environment and compiling the GLSL
// code into a fragment shader.
WEAK void halide_opengl_init_kernels(void* uctx, const char* src, int size) {
    if (!ST.initialized)
        halide_opengl_init(uctx);

    // Use '/// KERNEL' comments to split 'src' into discrete blocks, one for
    // each kernel contained in it.
    char* begin = strstr(src, kernel_marker);
    while (begin && begin[0]) {
        char* end = strstr(begin + sizeof(kernel_marker) - 1, kernel_marker);
        if (end == NULL)
            end = begin + strlen(begin);
        HalideOpenGLKernel *kernel = create_kernel(uctx, begin, end-begin);

        // Compile shader
        kernel->shader_id = halide_opengl_make_shader(uctx,
            GL_FRAGMENT_SHADER, kernel->source, NULL);

        // Link GLSL program
        GLuint program = ST.CreateProgram();
        ST.AttachShader(program, ST.vertex_shader_id);
        ST.AttachShader(program, kernel->shader_id);
        ST.LinkProgram(program);
        GLint status;
        ST.GetProgramiv(program, GL_LINK_STATUS, &status);
        if (!status) {
            halide_printf(uctx, "Could not link GLSL program:\n");
            GLint log_len;
            ST.GetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
            char* log = (char*) halide_malloc(uctx, log_len);
            ST.GetProgramInfoLog(program, log_len, NULL, log);
            halide_printf(uctx, "%s", log);
            halide_free(uctx, log);
            ST.DeleteProgram(program);
            program = 0;
        }
        kernel->program_id = program;

        if (halide_opengl_find_kernel(kernel->name)) {
            halide_printf(uctx, "Duplicate kernel name '%s'\n", kernel->name);
            halide_opengl_delete_kernel(uctx, kernel);
        } else {
            kernel->next = ST.kernels;
            ST.kernels = kernel;
        }

        begin = end;
    }
}

WEAK void halide_opengl_dev_sync(void* uctx) {
    ASSERT_INITIALIZED;
    // TODO: glFinish()
}

// Copy image data from host memory to texture. We assume that the texture has
// already been allocated using halide_opengl_dev_malloc.
WEAK void halide_opengl_copy_to_dev(void* uctx, buffer_t* buf) {
    ASSERT_INITIALIZED;
    if (buf->host_dirty) {
        halide_assert(uctx, buf->host && buf->dev);

        GLuint tex = get_texture_id(buf);
#ifdef DEBUG
        halide_printf(uctx, "halide_copy_to_dev: %d\n", tex);
#endif

        bool gl_compatible = (buf->stride[2] == 1 &&
                              buf->stride[0] == buf->extent[2]);

        halide_printf(uctx, "GL compatible? %d\n", gl_compatible);
        if (gl_compatible) {

            int w, h;
            check_buffer_properties(uctx, buf, &w, &h);
            ST.BindTexture(GL_TEXTURE_2D, tex);
            CHECK_GLERROR();

            // TODO: other formats than rgbaF
            // TODO: ensure that format of buf and texture match
            // TODO: GL_UNPACK_ROW_LENGTH
            // TODO: perform format conversions if necessary
            ST.TexSubImage2D(GL_TEXTURE_2D, 0,
                             0, 0, w, h,
                             GL_RGBA, GL_FLOAT, buf->host);
            CHECK_GLERROR();
        } else {
            halide_assert(uctx, false && "Unsupported image format");
        }
        ST.BindTexture(GL_TEXTURE_2D, 0);
        buf->host_dirty = false;
    }
}

// Copy image data from texture back to host memory.
WEAK void halide_opengl_copy_to_host(void* uctx, buffer_t* buf) {
    ASSERT_INITIALIZED;
    if (buf->dev_dirty) {
        halide_assert(uctx, buf->host && buf->dev);

        GLuint tex = get_texture_id(buf);
#ifdef DEBUG
        halide_printf(uctx, "halide_copy_to_host: %d\n", tex);
#endif

        // Is buffer in interleaved format?
        bool gl_compatible = (buf->stride[2] == 1 &&
                              buf->stride[0] == buf->extent[2]);

        if (gl_compatible) {
            ST.BindTexture(GL_TEXTURE_2D, tex);
            CHECK_GLERROR();

            // TODO: other formats than rgbaF
            // TODO: ensure that format of buf and texture match
            // TODO: GL_UNPACK_ROW_LENGTH
            // TODO: perform format conversions if necessary
            GLenum type = GL_FLOAT;
            GLenum format = GL_RGBA;
            ST.GetTexImage(GL_TEXTURE_2D, 0, format, type, buf->host);
            CHECK_GLERROR();
        } else {
            halide_assert(uctx, false && "Unsupported image format");
        }

        ST.BindTexture(GL_TEXTURE_2D, 0);
        buf->dev_dirty = false;
    }
}


WEAK void halide_opengl_dev_run(
    void* uctx,
    const char* entry_name,
    int blocksX, int blocksY, int blocksZ,
    int threadsX, int threadsY, int threadsZ,
    int shared_mem_bytes,
    size_t arg_sizes[],
    void* args[])
{
    ASSERT_INITIALIZED;

    HalideOpenGLKernel* kernel = halide_opengl_find_kernel(entry_name);
    if (!kernel) {
        halide_printf(uctx, "Could not find a kernel named '%s'\n",
                      entry_name);
        return;
    }

    ST.UseProgram(kernel->program_id);

    HalideOpenGLArgument* kernel_arg;

    // Copy input arguments to corresponding GLSL uniforms.
    GLint num_active_textures = 0;
    kernel_arg = kernel->arguments;
    for (int i=0; args[i]; i++, kernel_arg = kernel_arg->next) {
        halide_assert(uctx, kernel_arg != NULL &&
                      "Too many arguments passed to halide_opengl_dev_run");

        if (kernel_arg->is_output)
            continue;

        GLint loc = ST.GetUniformLocation(kernel->program_id,
                                          kernel_arg->name);
        if (loc != -1) {
            switch (kernel_arg->type) {
            case ARG_INT:
                halide_printf(uctx, "Int argument %d (%s): %d\n",
                              i, kernel_arg->name, *((int*)args[i]));
                ST.Uniform1iv(loc, 1, (GLint*)args[i]);
                break;
            case ARG_FLOAT:
                halide_printf(uctx, "Float argument %d (%s): %g\n",
                              i, kernel_arg->name, *((float*)args[i]));
                ST.Uniform1fv(loc, 1, (GLfloat*)args[i]);
                break;
            case ARG_BUFFER: {
                GLuint tex = *((GLuint*)args[i]);
                halide_printf(uctx, "Buffer argument %d (%s): %u\n",
                              i, kernel_arg->name, tex);
                ST.ActiveTexture(GL_TEXTURE0 + num_active_textures);
                ST.BindTexture(GL_TEXTURE_2D, tex);
                ST.Uniform1iv(loc, 1, &num_active_textures);
                num_active_textures++;
                // TODO: check maximum number of active textures
                break;
            }
            default:
                halide_printf(uctx, "Unexpected argument type for '%s'\n",
                              kernel_arg->name);
                halide_assert(uctx, false);
            }
        } else {
            // Argument was probably optimized away by GLSL compiler.
            halide_printf(uctx, "Ignoring argument '%s'\n", kernel_arg->name);
        }
    }
    halide_assert(uctx, kernel_arg == NULL &&
                  "Too few arguments passed to halide_opengl_dev_run");

    // Prepare framebuffer for rendering to output textures.
    GLint output_min[2] = { 0, 0 };
    GLint output_extent[2] = { 0, 0 };
    ST.BindFramebuffer(GL_FRAMEBUFFER, ST.framebuffer_id);
    ST.Disable(GL_CULL_FACE);
    ST.Disable(GL_DEPTH_TEST);

    GLint num_output_textures = 0;
    kernel_arg = kernel->arguments;
    for (int i=0; args[i]; i++, kernel_arg = kernel_arg->next) {
        if (!kernel_arg->is_output)
            continue;

        // TODO: GL_MAX_COLOR_ATTACHMENTS
        if (num_output_textures >= 1) {
            halide_assert(uctx, false &&
                          "OpenGL ES 2.0 only supports one single output texture");
        }

        GLuint tex = *((GLuint*)args[i]);
        halide_printf(uctx, "Output texture %d: %d\n", num_output_textures, tex);
        ST.FramebufferTexture2D(GL_FRAMEBUFFER,
                                GL_COLOR_ATTACHMENT0 + num_output_textures,
                                GL_TEXTURE_2D, tex, 0);
        CHECK_GLERROR();

        HalideOpenGLTexture* texinfo = halide_opengl_find_texture(tex);
        halide_assert(uctx, texinfo != NULL &&
                      "Undefined output texture");
        output_min[0] = texinfo->min[0];
        output_min[1] = texinfo->min[1];
        output_extent[0] = texinfo->extent[0];
        output_extent[1] = texinfo->extent[1];
        num_output_textures++;
    }
    // TODO: GL_MAX_DRAW_BUFFERS
    if (num_output_textures == 0) {
        halide_printf(uctx, "Warning: kernel '%s' has no output\n",
                      kernel->name);
        // TODO: cleanup
        return;
    } else {
        GLenum *draw_buffers = (GLenum*)
            halide_malloc(uctx, num_output_textures * sizeof(GLenum));
        for (int i=0; i<num_output_textures; i++)
            draw_buffers[i] = GL_COLOR_ATTACHMENT0 + i;
        ST.DrawBuffers(num_output_textures, draw_buffers);
        CHECK_GLERROR();
        halide_free(uctx, draw_buffers);
    }

    // Check that framebuffer is set up correctly
    GLenum status = ST.CheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_GLERROR();
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        halide_printf(uctx, "Setting up GL framebuffer %d failed (%x)\n",
                      ST.framebuffer_id, status);
        // TODO: cleanup
        return;
    }

    // Set vertex attributes
    GLint loc = ST.GetUniformLocation(kernel->program_id, "output_extent");
    ST.Uniform2iv(loc, 1, output_extent);
    CHECK_GLERROR();
    loc = ST.GetUniformLocation(kernel->program_id, "output_min");
    ST.Uniform2iv(loc, 1, output_min);
    CHECK_GLERROR();

    // Setup coordinate transformations
    ST.MatrixMode(GL_MODELVIEW);
    ST.LoadIdentity();
    ST.MatrixMode(GL_PROJECTION);
    ST.LoadIdentity();
    ST.Ortho(-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f);
    ST.Viewport(0, 0, output_extent[0], output_extent[1]);


    // Execute shader
    GLint position = ST.GetAttribLocation(kernel->program_id,
                                          "position");
    ST.BindBuffer(GL_ARRAY_BUFFER, ST.vertex_buffer);
    ST.VertexAttribPointer(position,
                           2,
                           GL_FLOAT,
                           GL_FALSE,    // normalized?
                           sizeof(GLfloat)*2,
                           NULL);
    ST.EnableVertexAttribArray(position);
    ST.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ST.element_buffer);
    ST.DrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_INT, NULL);
    CHECK_GLERROR();
    ST.DisableVertexAttribArray(position);

    // Cleanup
    for (int i=0; i<num_active_textures; i++) {
        ST.ActiveTexture(GL_TEXTURE0 + i);
        ST.BindTexture(GL_TEXTURE_2D, 0);
    }
    ST.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // extern "C"