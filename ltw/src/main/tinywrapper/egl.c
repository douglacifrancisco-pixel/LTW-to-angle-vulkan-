/**
 * Created by: artDev
 * Copyright (c) 2025 artDev, SerpentSpirale, CADIndie.
 * For use under LGPL-3.0
 */
#include "egl.h"
#include "unordered_map/int_hash.h"
#include "string_utils.h"
#include "env.h"
#include <string.h>

thread_local context_t *current_context;
unordered_map* context_map;

EGLContext (*host_eglCreateContext)(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list);
EGLBoolean (*host_eglDestroyContext)(EGLDisplay dpy, EGLContext ctx);
EGLBoolean (*host_eglMakeCurrent) (EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);

void init_egl() {
    context_map = alloc_intmap();
    host_eglCreateContext = (EGLContext (*)(EGLDisplay, EGLConfig, EGLContext,
                                            const EGLint *)) host_eglGetProcAddress("eglCreateContext");
    host_eglDestroyContext = (EGLBoolean (*)(EGLDisplay, EGLContext)) host_eglGetProcAddress(
            "eglDestroyContext");
    host_eglMakeCurrent = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface,
                                          EGLContext)) host_eglGetProcAddress("eglMakeCurrent");
}

static bool init_context(context_t* tw_context) {
    tw_context->shader_map = alloc_intmap_safe();
    if(!tw_context->shader_map) goto fail;
    tw_context->framebuffer_map = alloc_intmap_safe();
    if(!tw_context->framebuffer_map) goto fail_dealloc;
    tw_context->program_map = alloc_intmap_safe();
    if(!tw_context->program_map) goto fail_dealloc;
    tw_context->texture_swztrack_map = alloc_intmap_safe();
    if(!tw_context->texture_swztrack_map) goto fail_dealloc;
    for(int i = 0; i < MAX_BOUND_BASEBUFFERS; i++) {
        unordered_map *map = alloc_intmap_safe();
        if(!map) goto fail_dealloc;
        tw_context->bound_basebuffers[i] = map;
    }
    return true;

    fail_dealloc:
    for(int i = 0; i < MAX_BOUND_BASEBUFFERS; i++) {
        unordered_map *map = tw_context->bound_basebuffers[i];
        if(map) unordered_map_free(map);
    }
    if(tw_context->shader_map)
        unordered_map_free(tw_context->shader_map);
    if(tw_context->framebuffer_map)
        unordered_map_free(tw_context->framebuffer_map);
    if(tw_context->program_map)
        unordered_map_free(tw_context->program_map);
    if(tw_context->texture_swztrack_map)
        unordered_map_free(tw_context->texture_swztrack_map);
    fail:
    return false;
}

static void free_context(context_t* tw_context) {
    unordered_map_free(tw_context->shader_map);
    unordered_map_free(tw_context->program_map);
    unordered_map_free(tw_context->framebuffer_map);
    unordered_map_free(tw_context->texture_swztrack_map);
    if(tw_context->extensions_string != NULL) free(tw_context->extensions_string);
    if(tw_context->nextras != 0 && tw_context->extra_extensions_array != NULL) {
        for(int i = 0; i < tw_context->nextras; i++) {
            free((tw_context->extra_extensions_array[i]));
        }
        free(tw_context->extra_extensions_array);
    }
}

void init_extra_extensions(context_t* context, int* length) {
    const char* es_extensions = (const char*)es3_functions.glGetString(GL_EXTENSIONS);
    *length = (int)strlen(es_extensions);
    context->extensions_string = malloc(*length + 1);
    memcpy(context->extensions_string, es_extensions, *length+1);
}

void add_extra_extension(context_t* context, int* length, const char* extension)  {
    size_t extension_len = strlen(extension);

    char str_append_extension[extension_len + 2];
    memcpy(str_append_extension, extension, extension_len);
    str_append_extension[extension_len] = ' ';
    str_append_extension[extension_len + 1] = 0;
    context->extensions_string = gl4es_append(context->extensions_string, length, str_append_extension);

    int extension_idx = context->nextras++;
    context->extra_extensions_array = realloc(context->extra_extensions_array, sizeof(char*)*context->nextras);
    char* extra_extension = malloc(extension_len + 1);
    strncpy(extra_extension, extension, extension_len + 1);
    context->extra_extensions_array[extension_idx] = extra_extension;
}

void fin_extra_extensions(context_t* context, int length) {
    if(context->extensions_string[length-2] != ' ') return;
    char* orig_string = context->extensions_string;
    context->extensions_string = realloc(context->extensions_string, length - 1);
    if(context->extensions_string == NULL) {
        free(orig_string);
        return;
    }
    context->extensions_string[length-2] = 0;
}

void build_extension_string(context_t* context) {
    int length;
    init_extra_extensions(context, &length);
    if(context->buffer_storage) {
        if(!env_istrue("LTW_HIDE_BUFFER_STORAGE"))
            add_extra_extension(context, &length, "GL_ARB_buffer_storage");
        else printf("LTW: The buffer storage extension is hidden.\n");
    }
    if(context->buffer_texture_ext || context->es32) {
        add_extra_extension(context, &length, "GL_ARB_texture_buffer_object");
    }
    add_extra_extension(context, &length, "GL_ARB_draw_elements_base_vertex");
    // Required by Iris. Indexed variants are available since ES3.2 or with OES/EXT_draw_buffers_indexed extensions
    if(context->blending.available)
        add_extra_extension(context, &length, "GL_ARB_draw_buffers_blend");
    // Used by Minecraft for the GPU usage counter (see Blaze3D TimerQuery)
    add_extra_extension(context, &length, "GL_ARB_timer_query");
    // More extensions are possible, but will need way more wraps and tracking.
    fin_extra_extensions(context, length);
}

static void find_esversion(context_t* context) {
    const char* version = (const char*) es3_functions.glGetString(GL_VERSION);
    const char* shader_version = (const char*) es3_functions.glGetString(GL_SHADING_LANGUAGE_VERSION);

    int esmajor = 0, esminor = 0, shadermajor = 3, shaderminor = 0;
    sscanf(version, " OpenGL ES %i.%i", &esmajor, &esminor);
    sscanf(shader_version, " OpenGL ES GLSL ES %i.%i", &shadermajor, &shaderminor);
    context->shader_version = shadermajor * 100 + shaderminor;
    printf("LTW: Running on OpenGL ES %i.%i with ESSL %i\n", esmajor, esminor, context->shader_version);
    if(esmajor == 0 && esminor == 0) goto fail;
    if(esmajor < 3 || context->shader_version < 300) {
        printf("Unsupported OpenGL ES version. This will cause you problems down the line.\n");
        return;
    }
    if(esmajor == 3) {
        context->es31 = esminor >= 1;
        context->es32 = esminor >= 2;
    }else if(esmajor > 3) {
        context->es32 = context->es31 = true;
    }

    const char* extensions = (const char*) es3_functions.glGetString(GL_EXTENSIONS);
    if(strstr(extensions, "GL_EXT_buffer_storage")) context->buffer_storage = true;
    if(strstr(extensions, "GL_EXT_texture_buffer")) context->buffer_texture_ext = true;
    if(strstr(extensions, "GL_EXT_multi_draw_indirect")) context->multidraw_indirect = true;

    // EXT_disjoint_timer_query provides accurate int64 timer queries
    // on Core Profile it's ARB_timer_query instead
    // This enables real time queries via mentioned extension, otherwise faked ones are used (see query.c)
    if(strstr(extensions, "GL_EXT_disjoint_timer_query") || env_istrue_d("LTW_ENABLE_TIMER_QUERY", false)) context->timer_query = true;

    bool basevertex_oes = strstr(extensions, "GL_OES_draw_elements_base_vertex");
    bool basevertex_ext = strstr(extensions, "GL_EXT_draw_elements_base_vertex");
    if(context->es32) context->drawelementsbasevertex = es3_functions.glDrawElementsBaseVertex;
    else if(basevertex_oes) context->drawelementsbasevertex = es3_functions.glDrawElementsBaseVertexOES;
    else if(basevertex_ext) context->drawelementsbasevertex = es3_functions.glDrawElementsBaseVertexEXT;
    else context->drawelementsbasevertex = NULL;

    bool drawbuffersi_oes = strstr(extensions, "GL_OES_draw_buffers_indexed");
    bool drawbuffersi_ext = strstr(extensions, "GL_EXT_draw_buffers_indexed");
    blending_functions_t* blend = &context->blending;
    blend->available = true;
#define SET_FUNC(type) \
    blend->blendequationi = es3_functions.glBlendEquationi ## type; \
    blend->blendequationseparatei = es3_functions.glBlendEquationSeparatei ## type; \
    blend->blendfunci = es3_functions.glBlendFunci ## type; \
    blend->blendfuncseparatei = es3_functions.glBlendFuncSeparatei ## type; \
    blend->colormaski = es3_functions.glColorMaski ## type; \

    if(context->es32){
        SET_FUNC()
    }
    else if(drawbuffersi_oes){
        SET_FUNC(OES)
    }
    else if(drawbuffersi_ext){
        SET_FUNC(EXT)
    }
    else {
        blend->available = false;
    }
#undef SET_FUNC
    build_extension_string(context);

    return;
    fail:
    printf("LTW: Failed to detect OpenGL ES version");
}

void basevertex_init(context_t* context);
void buffer_copier_init(context_t* context);
static void init_incontext(context_t* tw_context) {
    es3_functions.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &tw_context->maxTextureSize);
    es3_functions.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &tw_context->max_drawbuffers);
    es3_functions.glGetIntegerv(GL_NUM_EXTENSIONS, &tw_context->nextensions_es);
    if(tw_context->max_drawbuffers > MAX_DRAWBUFFERS) {
        tw_context->max_drawbuffers = MAX_DRAWBUFFERS;
    }

    find_esversion(tw_context);

    basevertex_init(tw_context);
    buffer_copier_init(tw_context);
    es3_functions.glGenBuffers(1, &tw_context->multidraw_element_buffer);
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list) {
    EGLContext phys_context = host_eglCreateContext(dpy, config, share_context, attrib_list);
    if(phys_context == EGL_NO_CONTEXT) return phys_context;
    context_t* tw_context = calloc(1, sizeof(context_t));
    if(tw_context == NULL || !init_context(tw_context)) {
        if(tw_context) free(tw_context);
        host_eglDestroyContext(dpy, phys_context);
        return EGL_NO_CONTEXT;
    }
    unordered_map_put(context_map, phys_context, tw_context);
    return phys_context;
}

EGLBoolean eglDestroyContext (EGLDisplay dpy, EGLContext ctx) {
    if(!host_eglDestroyContext(dpy, ctx)) return EGL_FALSE;
    context_t* old_ctx = unordered_map_remove(context_map, ctx);
    free_context(old_ctx);
    free(old_ctx);
    return EGL_TRUE;
}

EGLBoolean eglMakeCurrent (EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    if(!host_eglMakeCurrent(dpy, draw, read, ctx)) return EGL_FALSE;
    if(ctx == EGL_NO_CONTEXT) {
        current_context = NULL;
        return EGL_TRUE;
    }
    context_t* tw_context = unordered_map_get(context_map, ctx);
    if(tw_context == NULL) {
        printf("TinywrapperEGL: Failed to find context %p\n", ctx);
        abort();
    }
    if(!tw_context->context_rdy) {
        init_incontext(tw_context);
        tw_context->context_rdy = true;
    }
    current_context = tw_context;
    return EGL_TRUE;
}


// === EGL WRAPPERS FOR LWJGL/GLFW DLSYM COMPATIBILITY ===
#define EGL_ATTR __attribute__((visibility("default"), used))

static EGLDisplay (*host_eglGetDisplay)(EGLNativeDisplayType) = NULL;
EGL_ATTR EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
    if (!host_eglGetDisplay) host_eglGetDisplay = (EGLDisplay (*)(EGLNativeDisplayType))host_eglGetProcAddress("eglGetDisplay");
    return host_eglGetDisplay ? host_eglGetDisplay(display_id) : EGL_NO_DISPLAY;
}

static EGLBoolean (*host_eglInitialize)(EGLDisplay, EGLint*, EGLint*) = NULL;
EGL_ATTR EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    if (!host_eglInitialize) host_eglInitialize = (EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*))host_eglGetProcAddress("eglInitialize");
    return host_eglInitialize ? host_eglInitialize(dpy, major, minor) : EGL_FALSE;
}

static EGLBoolean (*host_eglTerminate)(EGLDisplay) = NULL;
EGL_ATTR EGLBoolean eglTerminate(EGLDisplay dpy) {
    if (!host_eglTerminate) host_eglTerminate = (EGLBoolean (*)(EGLDisplay))host_eglGetProcAddress("eglTerminate");
    return host_eglTerminate ? host_eglTerminate(dpy) : EGL_FALSE;
}

static EGLBoolean (*host_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = NULL;
EGL_ATTR EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    if (!host_eglChooseConfig) host_eglChooseConfig = (EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*))host_eglGetProcAddress("eglChooseConfig");
    return host_eglChooseConfig ? host_eglChooseConfig(dpy, attrib_list, configs, config_size, num_config) : EGL_FALSE;
}

static EGLint (*host_eglGetError)(void) = NULL;
EGL_ATTR EGLint eglGetError(void) {
    if (!host_eglGetError) host_eglGetError = (EGLint (*)(void))host_eglGetProcAddress("eglGetError");
    return host_eglGetError ? host_eglGetError() : 0x3000;
}

static const char* (*host_eglQueryString)(EGLDisplay, EGLint) = NULL;
EGL_ATTR const char* eglQueryString(EGLDisplay dpy, EGLint name) {
    if (!host_eglQueryString) host_eglQueryString = (const char* (*)(EGLDisplay, EGLint))host_eglGetProcAddress("eglQueryString");
    return host_eglQueryString ? host_eglQueryString(dpy, name) : NULL;
}

static EGLSurface (*host_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) = NULL;
EGL_ATTR EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list) {
    if (!host_eglCreateWindowSurface) host_eglCreateWindowSurface = (EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*))host_eglGetProcAddress("eglCreateWindowSurface");
    return host_eglCreateWindowSurface ? host_eglCreateWindowSurface(dpy, config, win, attrib_list) : EGL_NO_SURFACE;
}

static EGLSurface (*host_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*) = NULL;
EGL_ATTR EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) {
    if (!host_eglCreatePbufferSurface) host_eglCreatePbufferSurface = (EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint*))host_eglGetProcAddress("eglCreatePbufferSurface");
    return host_eglCreatePbufferSurface ? host_eglCreatePbufferSurface(dpy, config, attrib_list) : EGL_NO_SURFACE;
}

static EGLBoolean (*host_eglDestroySurface)(EGLDisplay, EGLSurface) = NULL;
EGL_ATTR EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    if (!host_eglDestroySurface) host_eglDestroySurface = (EGLBoolean (*)(EGLDisplay, EGLSurface))host_eglGetProcAddress("eglDestroySurface");
    return host_eglDestroySurface ? host_eglDestroySurface(dpy, surface) : EGL_FALSE;
}

static EGLBoolean (*host_eglSwapBuffers)(EGLDisplay, EGLSurface) = NULL;
EGL_ATTR EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!host_eglSwapBuffers) host_eglSwapBuffers = (EGLBoolean (*)(EGLDisplay, EGLSurface))host_eglGetProcAddress("eglSwapBuffers");
    return host_eglSwapBuffers ? host_eglSwapBuffers(dpy, surface) : EGL_FALSE;
}

static EGLContext (*host_eglGetCurrentContext)(void) = NULL;
EGL_ATTR EGLContext eglGetCurrentContext(void) {
    if (!host_eglGetCurrentContext) host_eglGetCurrentContext = (EGLContext (*)(void))host_eglGetProcAddress("eglGetCurrentContext");
    return host_eglGetCurrentContext ? host_eglGetCurrentContext() : EGL_NO_CONTEXT;
}

static EGLDisplay (*host_eglGetCurrentDisplay)(void) = NULL;
EGL_ATTR EGLDisplay eglGetCurrentDisplay(void) {
    if (!host_eglGetCurrentDisplay) host_eglGetCurrentDisplay = (EGLDisplay (*)(void))host_eglGetProcAddress("eglGetCurrentDisplay");
    return host_eglGetCurrentDisplay ? host_eglGetCurrentDisplay() : EGL_NO_DISPLAY;
}

static EGLSurface (*host_eglGetCurrentSurface)(EGLint) = NULL;
EGL_ATTR EGLSurface eglGetCurrentSurface(EGLint readdraw) {
    if (!host_eglGetCurrentSurface) host_eglGetCurrentSurface = (EGLSurface (*)(EGLint))host_eglGetProcAddress("eglGetCurrentSurface");
    return host_eglGetCurrentSurface ? host_eglGetCurrentSurface(readdraw) : EGL_NO_SURFACE;
}

// === FORCE SYMBOL RETENTION ===
// Esta função constructor cria referências a todas as funções EGL,
// impedindo que o LTO as elimine como "dead code".
__attribute__((constructor, used)) static void force_egl_symbol_retention(void) {
    volatile void* refs[] = {
        (void*)eglGetDisplay,
        (void*)eglInitialize,
        (void*)eglTerminate,
        (void*)eglChooseConfig,
        (void*)eglGetError,
        (void*)eglQueryString,
        (void*)eglCreateWindowSurface,
        (void*)eglCreatePbufferSurface,
        (void*)eglDestroySurface,
        (void*)eglSwapBuffers,
        (void*)eglGetCurrentContext,
        (void*)eglGetCurrentDisplay,
        (void*)eglGetCurrentSurface,
        (void*)eglCreateContext,
        (void*)eglDestroyContext,
        (void*)eglMakeCurrent
    };
    (void)refs; // Evita warning de variável não usada
}
