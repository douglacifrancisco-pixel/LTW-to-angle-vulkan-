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

/*
 * Android's /data/app path contains an install-specific "~~..." component, so
 * hard-coding a path such as /data/app/.../lib/arm64 is not reliable.  When
 * libltw.so is loaded from the launcher, its mapped path gives us the exact
 * native-library directory for that installation.
 */
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

__attribute__((constructor, used)) void proc_init(){
    // ANGLE-first strategy for Vulkan rendering.
    const char* angleEglPath = "libEGL_angle.so";
    const char* eglPath = angleEglPath;
    char nativeDir[PATH_MAX];
    char launcherEglPath[PATH_MAX];
    int hasExplicitPath = 0;

    // Priority 1: Explicit EGL override.
    if(getenv("LIBGL_EGL") != NULL) {
        eglPath = getenv("LIBGL_EGL");
        printf("LTWInit: Using LIBGL_EGL override: %s\n", eglPath);
    }
    // Priority 2: Explicit launcher native-library directory.
    else if(getenv("LTW_ANGLE_LIB_DIR") != NULL) {
        snprintf(launcherEglPath, sizeof(launcherEglPath), "%s/%s",
                 getenv("LTW_ANGLE_LIB_DIR"), angleEglPath);
        eglPath = launcherEglPath;
        hasExplicitPath = 1;
        printf("LTWInit: Using LTW_ANGLE_LIB_DIR: %s\n", eglPath);
    }
    // Priority 3: Resolve the directory that contains libltw.so.  This handles
    // randomized Android paths such as /data/app/~~.../lib/arm64.
    else if(find_ltw_native_dir(nativeDir, sizeof(nativeDir))) {
        snprintf(launcherEglPath, sizeof(launcherEglPath), "%s/%s",
                 nativeDir, angleEglPath);
        eglPath = launcherEglPath;
        hasExplicitPath = 1;
        printf("LTWInit: Resolved ANGLE from libltw.so directory: %s\n", eglPath);
    }

    int flags = RTLD_LAZY | RTLD_LOCAL;
    void* eglHandle = dlopen(eglPath, flags);

    // Keep the normal linker lookup as a fallback for launchers that expose
    // their native-library directory through the linker namespace only.
    if(eglHandle == NULL && hasExplicitPath) {
        printf("LTWInit: Failed to load explicit ANGLE path %s: %s\n", eglPath, dlerror());
        eglPath = angleEglPath;
        eglHandle = dlopen(eglPath, flags);
    }
    
    if(eglHandle == NULL) {
        printf("LTWInit: FATAL - Failed to load %s: %s\n", eglPath, dlerror());
        printf("LTWInit: Ensure libEGL_angle.so is packaged beside libltw.so or set "
               "LTW_ANGLE_LIB_DIR\n");
        error_sysegl();
    }
    
    host_eglGetProcAddress = dlsym(eglHandle, "eglGetProcAddress");
    if(host_eglGetProcAddress == NULL) error_sysegl();
    init_egl();
    init_es3_proc();
}

// This is exported for it to be automatically picked up by LWJGL's symbol resolver.
__attribute__((used)) eglMustCastToProperFunctionPointerType glXGetProcAddress(const char *procname) {
    return eglGetProcAddress(procname);
}

extern void* resolve_stub(const char* procname);

eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname) {
    // EGL functions that we implement.
    // All of the other platform EGL functions will be redirected into Android's default EGL implementation.
    if(!strncmp(procname, "egl", 3)) {
        if(!strcmp("eglCreateContext", procname)) return (eglMustCastToProperFunctionPointerType) eglCreateContext;
        if(!strcmp("eglDestroyContext", procname)) return (eglMustCastToProperFunctionPointerType) eglDestroyContext;
        if(!strcmp("eglMakeCurrent", procname)) return (eglMustCastToProperFunctionPointerType) eglMakeCurrent;
    }
    // If the function doesn't start with "gl", don't even bother, pass through immediately.
    if(strncmp(procname, "gl", 2) != 0) goto fallback;
#define GLESOVERRIDE(name)                                        \
    if(!strcmp(procname, #name)) {                                \
        printf("LTW: Overridden %s\n", #name);                        \
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
