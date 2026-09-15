/**
 * Created by: artDev
 * Copyright (c) 2025 artDev, SerpentSpirale, CADIndie.
 * For use under LGPL-3.0
 */
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "proc.h"
#include "egl.h"
#include "libraryinternal.h"
#define GL_GLEXT_PROTOTYPES
#include "GL/gl.h"
#include "GL/glext.h"

INTERNAL eglMustCastToProperFunctionPointerType (*host_eglGetProcAddress)(const char *procname);
INTERNAL es3_functions_t es3_functions;

static void error_sysegl() {
    printf("LTWInit: Failed to load EGL: %s\n", dlerror());
    abort();
}

static void error_init(const char* functionName) {
    printf("LTWInit: Failed to load function \"%s\"\n", functionName);
    abort();
}

static void init_es3_proc() {
#define GLESFUNC(name, type) es3_functions.name = (type)host_eglGetProcAddress(#name); if(es3_functions.name == NULL) error_init(#name);
#include "es3_functions.h"
#undef GLESFUNC
#define GLESFUNC(name, type) es3_functions.name = (type)host_eglGetProcAddress(#name);
#include "es3_extended.h"
#undef GLESFUNC
}

static int find_ltw_native_dir(char *nativeDir, size_t nativeDirSize) {
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[PATH_MAX + 256];

    if(maps == NULL) {
        return 0;
    }

    while(fgets(line, sizeof(line), maps) != NULL) {
        char *libraryPath = strchr(line, '/');
        char *libraryName;
        size_t directoryLength;

        if(libraryPath == NULL || strstr(libraryPath, "/libltw.so") == NULL) {
            continue;
        }

        libraryName = strrchr(libraryPath, '/');
        if(libraryName == NULL) {
            continue;
        }

        directoryLength = (size_t)(libraryName - libraryPath);
        if(directoryLength + 1 > nativeDirSize) {
            continue;
        }

        memcpy(nativeDir, libraryPath, directoryLength);
        nativeDir[directoryLength] = '\0';
        fclose(maps);
        return 1;
    }

    fclose(maps);
    return 0;
}

static void angle_make_current_early() {
    EGLDisplay (*fn_eglGetDisplay)(EGLNativeDisplayType) =
        (EGLDisplay(*)(EGLNativeDisplayType))host_eglGetProcAddress("eglGetDisplay");
    EGLBoolean (*fn_eglInitialize)(EGLDisplay, EGLint*, EGLint*) =
        (EGLBoolean(*)(EGLDisplay, EGLint*, EGLint*))host_eglGetProcAddress("eglInitialize");
    EGLBoolean (*fn_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) =
        (EGLBoolean(*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*))host_eglGetProcAddress("eglChooseConfig");
    EGLSurface (*fn_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*) =
        (EGLSurface(*)(EGLDisplay, EGLConfig, const EGLint*))host_eglGetProcAddress("eglCreatePbufferSurface");
    EGLContext (*fn_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) =
        (EGLContext(*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*))host_eglGetProcAddress("eglCreateContext");
    EGLBoolean (*fn_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) =
        (EGLBoolean(*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))host_eglGetProcAddress("eglMakeCurrent");

    if(!fn_eglGetDisplay || !fn_eglInitialize || !fn_eglChooseConfig ||
       !fn_eglCreatePbufferSurface || !fn_eglCreateContext || !fn_eglMakeCurrent) {
        printf("LTWInit: angle_make_current_early: missing EGL functions\n");
        return;
    }

    EGLDisplay dpy = fn_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if(dpy == EGL_NO_DISPLAY) {
        printf("LTWInit: angle_make_current_early: eglGetDisplay failed\n");
        return;
    }

    EGLint major, minor;
    if(!fn_eglInitialize(dpy, &major, &minor)) {
        printf("LTWInit: angle_make_current_early: eglInitialize failed\n");
        return;
    }

    EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs;
    if(!fn_eglChooseConfig(dpy, config_attribs, &config, 1, &num_configs) || num_configs == 0) {
        printf("LTWInit: angle_make_current_early: eglChooseConfig failed\n");
        return;
    }

    EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surf = fn_eglCreatePbufferSurface(dpy, config, pbuf_attribs);
    if(surf == EGL_NO_SURFACE) {
        printf("LTWInit: angle_make_current_early: eglCreatePbufferSurface failed\n");
        return;
    }

    EGLint ctx_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    EGLContext ctx = fn_eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    if(ctx == EGL_NO_CONTEXT) {
        printf("LTWInit: angle_make_current_early: eglCreateContext failed\n");
        return;
    }

    if(!fn_eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("LTWInit: angle_make_current_early: eglMakeCurrent failed\n");
        return;
    }

    printf("LTWInit: ANGLE context made current early (pbuffer 1x1)\n");
}

__attribute__((constructor, used)) void proc_init(){
    const char* angleEglPath = "libEGL_angle.so";
    const char* eglPath = angleEglPath;
    char nativeDir[PATH_MAX];
    char launcherEglPath[PATH_MAX];
    int hasExplicitPath = 0;

    if(getenv("LIBGL_EGL") != NULL) {
        eglPath = getenv("LIBGL_EGL");
        printf("LTWInit: Using LIBGL_EGL override: %s\n", eglPath);
    }
    else if(getenv("LTW_ANGLE_LIB_DIR") != NULL) {
        snprintf(launcherEglPath, sizeof(launcherEglPath), "%s/%s",
                 getenv("LTW_ANGLE_LIB_DIR"), angleEglPath);
        eglPath = launcherEglPath;
        hasExplicitPath = 1;
        printf("LTWInit: Using LTW_ANGLE_LIB_DIR: %s\n", eglPath);
    }
    else if(find_ltw_native_dir(nativeDir, sizeof(nativeDir))) {
        snprintf(launcherEglPath, sizeof(launcherEglPath), "%s/%s",
                 nativeDir, angleEglPath);
        eglPath = launcherEglPath;
        hasExplicitPath = 1;
        printf("LTWInit: Resolved ANGLE from libltw.so directory: %s\n", eglPath);
    }

    int flags = RTLD_LAZY | RTLD_LOCAL;
    void* eglHandle = dlopen(eglPath, flags);

    if(eglHandle == NULL && hasExplicitPath) {
        printf("LTWInit: Failed to load explicit ANGLE path %s: %s\n", eglPath, dlerror());
        eglPath = angleEglPath;
        eglHandle = dlopen(eglPath, flags);
    }

    if(eglHandle == NULL) {
        printf("LTWInit: FATAL - Failed to load %s: %s\n", eglPath, dlerror());
        printf("LTWInit: Ensure libEGL_angle.so is packaged beside libltw.so or set LTW_ANGLE_LIB_DIR\n");
        error_sysegl();
    }

    host_eglGetProcAddress = dlsym(eglHandle, "eglGetProcAddress");
    if(host_eglGetProcAddress == NULL) error_sysegl();
    init_egl();
    init_es3_proc();
    angle_make_current_early();
}

__attribute__((used)) eglMustCastToProperFunctionPointerType glXGetProcAddress(const char *procname) {
    return eglGetProcAddress(procname);
}

extern void* resolve_stub(const char* procname);

eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname) {
    if(!strncmp(procname, "egl", 3)) {
        if(!strcmp("eglCreateContext", procname)) return (eglMustCastToProperFunctionPointerType) eglCreateContext;
        if(!strcmp("eglDestroyContext", procname)) return (eglMustCastToProperFunctionPointerType) eglDestroyContext;
        if(!strcmp("eglMakeCurrent", procname)) return (eglMustCastToProperFunctionPointerType) eglMakeCurrent;
        if(!strcmp("eglGetDisplay", procname)) return (eglMustCastToProperFunctionPointerType) eglGetDisplay;
        if(!strcmp("eglInitialize", procname)) return (eglMustCastToProperFunctionPointerType) eglInitialize;
        if(!strcmp("eglTerminate", procname)) return (eglMustCastToProperFunctionPointerType) eglTerminate;
        if(!strcmp("eglChooseConfig", procname)) return (eglMustCastToProperFunctionPointerType) eglChooseConfig;
        if(!strcmp("eglGetError", procname)) return (eglMustCastToProperFunctionPointerType) eglGetError;
        if(!strcmp("eglQueryString", procname)) return (eglMustCastToProperFunctionPointerType) eglQueryString;
        if(!strcmp("eglCreateWindowSurface", procname)) return (eglMustCastToProperFunctionPointerType) eglCreateWindowSurface;
        if(!strcmp("eglCreatePbufferSurface", procname)) return (eglMustCastToProperFunctionPointerType) eglCreatePbufferSurface;
        if(!strcmp("eglDestroySurface", procname)) return (eglMustCastToProperFunctionPointerType) eglDestroySurface;
        if(!strcmp("eglSwapBuffers", procname)) return (eglMustCastToProperFunctionPointerType) eglSwapBuffers;
        if(!strcmp("eglGetCurrentContext", procname)) return (eglMustCastToProperFunctionPointerType) eglGetCurrentContext;
        if(!strcmp("eglGetCurrentDisplay", procname)) return (eglMustCastToProperFunctionPointerType) eglGetCurrentDisplay;
        if(!strcmp("eglGetCurrentSurface", procname)) return (eglMustCastToProperFunctionPointerType) eglGetCurrentSurface;
    }
    if(strncmp(procname, "gl", 2) != 0) goto fallback;
#define GLESOVERRIDE(name)                                        \
    if(!strcmp(procname, #name)) {                                \
        printf("LTW: Overridden %s\n", #name);                   \
        return (eglMustCastToProperFunctionPointerType) name;     \
    }
#include "es3_overrides.h"
#undef GLESOVERRIDE
    eglMustCastToProperFunctionPointerType function;
fallback:
    function = host_eglGetProcAddress(procname);
    if(function == NULL) {
        function = resolve_stub(procname);
    }
    return function;
}
