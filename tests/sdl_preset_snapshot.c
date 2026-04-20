/*
 * Minimal SDL2 + OpenGL snapshot of a single .milk preset (same PCM + playlist
 * pattern as tests/reference_renderer.c). Intended as the “standalone-style”
 * baseline that uses the same libprojectM as the system/GTK build, comparable
 * to the upstream SDL test UI render path.
 *
 * Usage: sdl_preset_snapshot <preset.milk> <out.ppm> <target_frame> [width] [height]
 */

#include <SDL2/SDL.h>
#include <epoxy/gl.h>
#include <glib.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <projectM-4/audio.h>
#include <projectM-4/core.h>
#include <projectM-4/parameters.h>
#include <projectM-4/playlist.h>
#include <projectM-4/render_opengl.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    projectm_handle           pm;
    projectm_playlist_handle  playlist;
    int                       width;
    int                       height;
    int                       frame_count;
    const char*               preset_file;
    const char*               out_path;
    int                       target_frame;
    gboolean                  deferred_preset;
} SnapCtx;

static void APIENTRY
gl_debug_log_cb(GLenum source,
                GLenum type,
                GLuint id,
                GLenum severity,
                GLsizei length,
                const GLchar* message,
                const void* user_param)
{
    (void)source;
    (void)type;
    (void)id;
    (void)severity;
    (void)length;
    const char* tag = user_param ? (const char*)user_param : "gl";
    fprintf(stderr, "%s debug: %s\n", tag, message ? message : "(null)");
}

static void
maybe_enable_gl_debug(const char* tag)
{
    const char* enabled = g_getenv("MILKDROP_GL_DEBUG_TRACE");
    if (!enabled || strcmp(enabled, "1") != 0)
        return;

    if (epoxy_has_gl_extension("GL_KHR_debug") || epoxy_gl_version() >= 43) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(gl_debug_log_cb, tag);
    }
}

static void
log_gl_context_info(const char* tag)
{
    const char* enabled = g_getenv("MILKDROP_GL_DEBUG_TRACE");
    if (!enabled || strcmp(enabled, "1") != 0)
        return;

    GLint profile = 0;
    GLint flags = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    fprintf(stderr,
            "%s context: vendor=%s renderer=%s version=%s profile=0x%x flags=0x%x\n",
            tag,
            (const char*)glGetString(GL_VENDOR),
            (const char*)glGetString(GL_RENDERER),
            (const char*)glGetString(GL_VERSION),
            (unsigned int)profile,
            (unsigned int)flags);
}

static void
log_fbo_state(const char* tag,
              GLuint      fbo)
{
    const char* enabled = g_getenv("MILKDROP_GL_DEBUG_TRACE");
    if (!enabled || strcmp(enabled, "1") != 0)
        return;

    GLint prev_draw = 0;
    GLint prev_read = 0;
    GLint draw_buf0 = 0;
    GLint read_buf = 0;
    GLint type = 0;
    GLint name = 0;
    GLint enc = 0;

    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glGetIntegerv(GL_DRAW_BUFFER0, &draw_buf0);
    glGetIntegerv(GL_READ_BUFFER, &read_buf);
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                          GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                          &type);
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                          GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                          &name);
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                          GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,
                                          &enc);
    fprintf(stderr,
            "%s fbo=%u status_draw=0x%x status_read=0x%x draw_buf0=0x%x read_buf=0x%x att_type=0x%x att_name=%d encoding=0x%x\n",
            tag,
            (unsigned int)fbo,
            (unsigned int)glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER),
            (unsigned int)glCheckFramebufferStatus(GL_READ_FRAMEBUFFER),
            (unsigned int)draw_buf0,
            (unsigned int)read_buf,
            (unsigned int)type,
            name,
            (unsigned int)enc);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
}

static void*
sdl_gl_load(const char* name,
            void*       user_data)
{
    (void)user_data;
    return (void*)SDL_GL_GetProcAddress(name);
}

static void
snap_configure_textures(projectm_handle pm,
                        const char*     preset_path)
{
    if (!preset_path)
        return;
    char*       dir = g_path_get_dirname(preset_path);
    const char* paths[3] = {0};
    size_t      n        = 0;

    if (dir && g_file_test(dir, G_FILE_TEST_IS_DIR))
        paths[n++] = dir;

    const char* extra = g_getenv("MILKDROP_TEXTURE_SEARCH_EXTRA");
    if (extra && extra[0] && g_file_test(extra, G_FILE_TEST_IS_DIR))
        paths[n++] = extra;

    if (n > 0)
        projectm_set_texture_search_paths(pm, paths, (uint32_t)n);
    g_free(dir);
}

static gboolean
snap_init(SnapCtx* ctx)
{
    ctx->pm = projectm_create_with_opengl_load_proc(sdl_gl_load, NULL);
    if (!ctx->pm) {
        fprintf(stderr, "sdl_preset_snapshot: projectm_create_with_opengl_load_proc failed\n");
        return FALSE;
    }

    ctx->playlist = projectm_playlist_create(ctx->pm);
    if (!ctx->playlist) {
        fprintf(stderr, "sdl_preset_snapshot: playlist create failed\n");
        projectm_destroy(ctx->pm);
        ctx->pm = NULL;
        return FALSE;
    }

    snap_configure_textures(ctx->pm, ctx->preset_file);

    projectm_set_window_size(ctx->pm, (size_t)ctx->width, (size_t)ctx->height);
    projectm_set_preset_duration(ctx->pm, 300.0);

    if (ctx->preset_file && g_file_test(ctx->preset_file, G_FILE_TEST_IS_REGULAR)) {
        if (!projectm_playlist_add_preset(ctx->playlist, ctx->preset_file, true))
            fprintf(stderr, "sdl_preset_snapshot: warning: add_preset failed\n");
    }

    projectm_load_preset_file(ctx->pm, "idle://", false);
    ctx->deferred_preset = TRUE;
    projectm_playlist_set_shuffle(ctx->playlist, false);
    return TRUE;
}

static void
snap_feed_pcm(SnapCtx* ctx,
              float*   pcm,
              int      nfloats)
{
    int f = ctx->frame_count;
    for (int i = 0; i < nfloats; i++) {
        float t = (float)(f * 512 + i / 2) / 44100.0f;
        pcm[i]  = 0.5f * sinf(2.0f * (float)M_PI * 440.0f * t);
    }
}

static gboolean
snap_write_ppm(const char* path,
               int         w,
               int         h)
{
    GLubyte* pixels = calloc((size_t)w * (size_t)h, 4);
    if (!pixels)
        return FALSE;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    while (glGetError() != GL_NO_ERROR) {}

    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    FILE* f = fopen(path, "wb");
    if (!f) {
        free(pixels);
        return FALSE;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) {
        const GLubyte* row = pixels + y * w * 4;
        for (int x = 0; x < w; x++) {
            const unsigned char rgb[3] = {row[x * 4], row[x * 4 + 1], row[x * 4 + 2]};
            size_t nw = fwrite(rgb, 1, 3, f);
            if (nw != 3) {
                fprintf(stderr,
                        "sdl_preset_snapshot: fwrite wrote %zu of 3 bytes to %s\n",
                        nw,
                        path);
                perror("fwrite");
                fclose(f);
                remove(path);
                free(pixels);
                return FALSE;
            }
        }
    }
    fclose(f);
    free(pixels);
    return TRUE;
}

static void
snap_cleanup(SnapCtx* ctx)
{
    if (ctx->playlist) {
        projectm_playlist_destroy(ctx->playlist);
        ctx->playlist = NULL;
    }
    if (ctx->pm) {
        projectm_destroy(ctx->pm);
        ctx->pm = NULL;
    }
}

int
main(int argc,
     char** argv)
{
    if (argc < 4) {
        fprintf(stderr,
                "Usage: %s <preset.milk> <out.ppm> <target_frame> [width] [height]\n",
                argv[0]);
        return 1;
    }

    SnapCtx ctx = {0};
    ctx.preset_file   = argv[1];
    ctx.out_path      = argv[2];
    ctx.target_frame  = atoi(argv[3]);
    ctx.width         = argc > 4 ? atoi(argv[4]) : 320;
    ctx.height        = argc > 5 ? atoi(argv[5]) : 240;
    ctx.frame_count   = 0;

    if (ctx.width < 32 || ctx.height < 32 || ctx.target_frame < 1) {
        fprintf(stderr, "sdl_preset_snapshot: invalid size or frame\n");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow("milkdrop sdl snapshot",
                                            SDL_WINDOWPOS_UNDEFINED,
                                            SDL_WINDOWPOS_UNDEFINED,
                                            ctx.width,
                                            ctx.height,
                                            SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext glctx = SDL_GL_CreateContext(window);
    if (!glctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_MakeCurrent(window, glctx);
    glViewport(0, 0, ctx.width, ctx.height);
    maybe_enable_gl_debug("sdl-preset-snapshot");
    log_gl_context_info("sdl-preset-snapshot");

    if (!snap_init(&ctx)) {
        SDL_GL_DeleteContext(glctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    float pcm[512 * 2];

    for (;;) {
        if (g_getenv("MILKDROP_FIXED_FRAME_TIME"))
            projectm_set_frame_time(ctx.pm, (double)ctx.frame_count / 60.0);
        else
            projectm_set_frame_time(ctx.pm, SDL_GetTicks() / 1000.0);

        if (ctx.deferred_preset && ctx.playlist) {
            ctx.deferred_preset = FALSE;
            projectm_playlist_set_position(ctx.playlist, 0u, true);
        }

        snap_feed_pcm(&ctx, pcm, 512 * 2);
        projectm_pcm_add_float(ctx.pm, pcm, 512, PROJECTM_STEREO);

        GLuint fbo = 0, tex = 0;
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ctx.width, ctx.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        log_fbo_state("sdl-before-render", fbo);
        GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr,
                    "sdl_preset_snapshot: FBO incomplete before render: 0x%x\n",
                    (unsigned int)fbo_status);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            snap_cleanup(&ctx);
            SDL_GL_DeleteContext(glctx);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 2;
        }

        while (glGetError() != GL_NO_ERROR) {}

        projectm_opengl_render_frame_fbo(ctx.pm, fbo);
        glFinish();
        log_fbo_state("sdl-after-render", fbo);
        GLenum render_err = glGetError();
        if (render_err != GL_NO_ERROR) {
            fprintf(stderr,
                    "sdl_preset_snapshot: post-render GL error 0x%x at frame %d for %s\n",
                    (unsigned int)render_err,
                    ctx.frame_count + 1,
                    ctx.preset_file ? ctx.preset_file : "(null)");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            snap_cleanup(&ctx);
            SDL_GL_DeleteContext(glctx);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 3;
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo); // Read from fbo for saving

        ctx.frame_count++;
        gboolean wrote_snapshot = FALSE;
        if (ctx.frame_count == ctx.target_frame) {
            if (!snap_write_ppm(ctx.out_path, ctx.width, ctx.height)) {
                fprintf(stderr, "sdl_preset_snapshot: failed to write %s\n", ctx.out_path);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &fbo);
                glDeleteTextures(1, &tex);
                snap_cleanup(&ctx);
                SDL_GL_DeleteContext(glctx);
                SDL_DestroyWindow(window);
                SDL_Quit();
                return 1;
            }
            printf("sdl_preset_snapshot: wrote frame %d -> %s\n", ctx.target_frame, ctx.out_path);
            wrote_snapshot = TRUE;
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);

        if (wrote_snapshot)
            break;
    }

    snap_cleanup(&ctx);
    SDL_GL_DeleteContext(glctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
