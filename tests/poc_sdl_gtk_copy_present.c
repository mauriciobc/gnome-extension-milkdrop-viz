/*
 * POC: renderiza projectM inteiro em um contexto SDL/OpenGL separado e copia
 * somente o frame final para o GTK via GdkMemoryTexture.
 *
 * Uso:
 *   poc-sdl-gtk-copy-present <preset.milk> [width] [height] [auto_exit_frames]
 *
 * Se auto_exit_frames > 0, o processo termina sozinho depois de N frames e
 * retorna erro se nenhum frame visível tiver sido produzido.
 */

#include <SDL2/SDL.h>
#include <epoxy/gl.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <projectM-4/audio.h>
#include <projectM-4/core.h>
#include <projectM-4/logging.h>
#include <projectM-4/parameters.h>
#include <projectM-4/render_opengl.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    GtkWidget*      window;
    GtkWidget*      picture;
    GMainLoop*      loop;
    SDL_Window*     sdl_window;
    SDL_GLContext   sdl_gl;
    projectm_handle pm;
    GLuint          render_fbo;
    GLuint          render_tex;
    guint           tick_id;
    guint64         frame_count;
    gboolean        saw_visible_frame;
    gboolean        deferred_preset;
    gboolean        failed;
    int             width;
    int             height;
    int             auto_exit_frames;
    char*           preset_file;
} PocCtx;

static void
poc_projectm_log(const char*        message,
                 projectm_log_level level,
                 void*              user_data)
{
    (void)user_data;
    if (!message)
        return;

    if (level >= PROJECTM_LOG_LEVEL_WARN)
        g_warning("poc-projectM: %s", message);
    else
        g_message("poc-projectM: %s", message);
}

static void*
poc_sdl_gl_load(const char* name,
                void*       user_data)
{
    (void)user_data;
    return (void*)SDL_GL_GetProcAddress(name);
}

static gboolean
poc_buffer_has_visible_content(const guint8* pixels,
                               gsize         len)
{
    if (!pixels)
        return FALSE;

    for (gsize i = 0; i + 3 < len; i += 4) {
        if (pixels[i] || pixels[i + 1] || pixels[i + 2])
            return TRUE;
    }
    return FALSE;
}

static void
poc_feed_pcm(float*  pcm,
             int     nfloats,
             guint64 frame_count)
{
    for (int i = 0; i < nfloats; i++) {
        float t = (float)(frame_count * 512u + (guint64)(i / 2)) / 44100.0f;
        pcm[i]  = 0.5f * sinf(2.0f * (float)M_PI * 440.0f * t);
    }
}

static gboolean
poc_copy_pixels_top_down(guint8** out_pixels,
                         int      width,
                         int      height)
{
    gsize stride = (gsize)width * 4u;
    gsize total  = stride * (gsize)height;
    guint8* src  = g_malloc(total);
    guint8* dst  = g_malloc(total);

    if (!src || !dst) {
        g_free(src);
        g_free(dst);
        return FALSE;
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, src);
    if (glGetError() != GL_NO_ERROR) {
        g_free(src);
        g_free(dst);
        return FALSE;
    }

    for (int y = 0; y < height; y++) {
        memcpy(dst + (gsize)y * stride,
               src + (gsize)(height - 1 - y) * stride,
               stride);
    }

    g_free(src);
    *out_pixels = dst;
    return TRUE;
}

static void
poc_configure_texture_paths(projectm_handle pm,
                            const char*     preset_file)
{
    g_autofree char* preset_dir = NULL;
    g_autofree char* texture_dir = NULL;
    const char* paths[2] = {0};
    guint n_paths = 0;

    if (!preset_file || !preset_file[0])
        return;

    preset_dir = g_path_get_dirname(preset_file);
    if (preset_dir && g_file_test(preset_dir, G_FILE_TEST_IS_DIR))
        paths[n_paths++] = preset_dir;

    texture_dir = g_build_filename(preset_dir, "textures", NULL);
    if (texture_dir && g_file_test(texture_dir, G_FILE_TEST_IS_DIR))
        paths[n_paths++] = texture_dir;

    if (n_paths > 0)
        projectm_set_texture_search_paths(pm, paths, n_paths);
}

static gboolean
poc_create_render_target(PocCtx* ctx)
{
    glGenFramebuffers(1, &ctx->render_fbo);
    glGenTextures(1, &ctx->render_tex);

    glBindTexture(GL_TEXTURE_2D, ctx->render_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ctx->width, ctx->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->render_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx->render_tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        g_warning("poc: render target FBO incompleto");
        return FALSE;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return TRUE;
}

static gboolean
poc_init_renderer(PocCtx* ctx)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        g_warning("poc: SDL_Init falhou: %s", SDL_GetError());
        return FALSE;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    ctx->sdl_window = SDL_CreateWindow("milkdrop poc offscreen",
                                       SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED,
                                       ctx->width,
                                       ctx->height,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!ctx->sdl_window) {
        g_warning("poc: SDL_CreateWindow falhou: %s", SDL_GetError());
        SDL_Quit();
        return FALSE;
    }

    ctx->sdl_gl = SDL_GL_CreateContext(ctx->sdl_window);
    if (!ctx->sdl_gl) {
        g_warning("poc: SDL_GL_CreateContext falhou: %s", SDL_GetError());
        SDL_DestroyWindow(ctx->sdl_window);
        ctx->sdl_window = NULL;
        SDL_Quit();
        return FALSE;
    }

    SDL_GL_MakeCurrent(ctx->sdl_window, ctx->sdl_gl);
    glViewport(0, 0, ctx->width, ctx->height);

    projectm_set_log_callback(poc_projectm_log, true, NULL);
    projectm_set_log_level(PROJECTM_LOG_LEVEL_INFO, true);

    ctx->pm = projectm_create_with_opengl_load_proc(poc_sdl_gl_load, NULL);
    if (!ctx->pm) {
        g_warning("poc: projectm_create_with_opengl_load_proc retornou NULL");
        return FALSE;
    }

    poc_configure_texture_paths(ctx->pm, ctx->preset_file);
    projectm_set_window_size(ctx->pm, (size_t)ctx->width, (size_t)ctx->height);
    projectm_set_preset_duration(ctx->pm, 300.0);

    projectm_load_preset_file(ctx->pm, "idle://", false);
    if (ctx->preset_file && ctx->preset_file[0])
        ctx->deferred_preset = TRUE;

    if (!poc_create_render_target(ctx))
        return FALSE;

    return TRUE;
}

static void
poc_cleanup_renderer(PocCtx* ctx)
{
    if (ctx->sdl_window && ctx->sdl_gl)
        SDL_GL_MakeCurrent(ctx->sdl_window, ctx->sdl_gl);

    if (ctx->render_fbo) {
        glDeleteFramebuffers(1, &ctx->render_fbo);
        ctx->render_fbo = 0;
    }

    if (ctx->render_tex) {
        glDeleteTextures(1, &ctx->render_tex);
        ctx->render_tex = 0;
    }

    if (ctx->pm) {
        projectm_destroy(ctx->pm);
        ctx->pm = NULL;
    }

    projectm_set_log_callback(NULL, true, NULL);

    if (ctx->sdl_gl) {
        SDL_GL_DeleteContext(ctx->sdl_gl);
        ctx->sdl_gl = NULL;
    }

    if (ctx->sdl_window) {
        SDL_DestroyWindow(ctx->sdl_window);
        ctx->sdl_window = NULL;
    }

    if (SDL_WasInit(SDL_INIT_VIDEO))
        SDL_Quit();
}

static gboolean
poc_present_frame(PocCtx* ctx)
{
    float pcm[512 * 2];
    guint8* pixels = NULL;
    gsize stride = (gsize)ctx->width * 4u;
    gsize total  = stride * (gsize)ctx->height;

    SDL_GL_MakeCurrent(ctx->sdl_window, ctx->sdl_gl);

    poc_feed_pcm(pcm, G_N_ELEMENTS(pcm), ctx->frame_count);
    projectm_set_frame_time(ctx->pm, (double)ctx->frame_count / 60.0);

    if (ctx->deferred_preset && ctx->preset_file && ctx->preset_file[0]) {
        ctx->deferred_preset = FALSE;
        projectm_load_preset_file(ctx->pm, ctx->preset_file, false);
    }

    projectm_pcm_add_float(ctx->pm, pcm, 512, PROJECTM_STEREO);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->render_fbo);
    glViewport(0, 0, ctx->width, ctx->height);
    while (glGetError() != GL_NO_ERROR) {}

    projectm_opengl_render_frame_fbo(ctx->pm, ctx->render_fbo);
    glFinish();
    if (glGetError() != GL_NO_ERROR) {
        g_warning("poc: projectM falhou ao renderizar no contexto SDL");
        return FALSE;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->render_fbo);
    if (!poc_copy_pixels_top_down(&pixels, ctx->width, ctx->height)) {
        g_warning("poc: glReadPixels falhou");
        return FALSE;
    }

    if (!ctx->saw_visible_frame && poc_buffer_has_visible_content(pixels, total)) {
        ctx->saw_visible_frame = TRUE;
        g_message("poc: primeiro frame visivel detectado no frame %" G_GUINT64_FORMAT, ctx->frame_count + 1u);
    }

    GBytes* bytes = g_bytes_new_take(pixels, total);
    GdkTexture* texture = gdk_memory_texture_new(ctx->width,
                                                 ctx->height,
                                                 GDK_MEMORY_R8G8B8A8,
                                                 bytes,
                                                 stride);
    gtk_picture_set_paintable(GTK_PICTURE(ctx->picture), GDK_PAINTABLE(texture));
    g_object_unref(texture);
    g_bytes_unref(bytes);

    ctx->frame_count++;
    return TRUE;
}

static gboolean
poc_on_tick(gpointer user_data)
{
    PocCtx* ctx = user_data;

    if (!poc_present_frame(ctx)) {
        ctx->failed = TRUE;
        ctx->tick_id = 0;
        g_main_loop_quit(ctx->loop);
        return G_SOURCE_REMOVE;
    }

    if (ctx->auto_exit_frames > 0 && (int)ctx->frame_count >= ctx->auto_exit_frames) {
        ctx->tick_id = 0;
        g_main_loop_quit(ctx->loop);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
poc_on_close_request(GtkWindow* window,
                     gpointer   user_data)
{
    PocCtx* ctx = user_data;
    (void)window;
    g_main_loop_quit(ctx->loop);
    return FALSE;
}

static gboolean
poc_build_ui(PocCtx* ctx)
{
    ctx->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(ctx->window), "Milkdrop POC - SDL offscreen -> GTK");
    gtk_window_set_default_size(GTK_WINDOW(ctx->window), ctx->width, ctx->height);

    ctx->picture = gtk_picture_new();
    gtk_picture_set_can_shrink(GTK_PICTURE(ctx->picture), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(ctx->picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_window_set_child(GTK_WINDOW(ctx->window), ctx->picture);

    g_signal_connect(ctx->window, "close-request", G_CALLBACK(poc_on_close_request), ctx);
    gtk_window_present(GTK_WINDOW(ctx->window));
    return TRUE;
}

int
main(int argc,
     char** argv)
{
    PocCtx ctx = {0};

    ctx.width  = argc > 2 ? atoi(argv[2]) : 640;
    ctx.height = argc > 3 ? atoi(argv[3]) : 360;
    ctx.auto_exit_frames = argc > 4 ? atoi(argv[4]) : 0;
    ctx.preset_file = argc > 1 ? g_strdup(argv[1]) : NULL;

    if (ctx.width < 32 || ctx.height < 32 || ctx.auto_exit_frames < 0) {
        g_printerr("Uso: %s <preset.milk> [width] [height] [auto_exit_frames]\n", argv[0]);
        g_free(ctx.preset_file);
        return 1;
    }

    if (!poc_init_renderer(&ctx)) {
        g_free(ctx.preset_file);
        return 1;
    }

    if (!gtk_init_check()) {
        g_printerr("poc: nao foi possivel iniciar o GTK\n");
        poc_cleanup_renderer(&ctx);
        g_free(ctx.preset_file);
        return 77;
    }

    ctx.loop = g_main_loop_new(NULL, FALSE);

    if (!poc_build_ui(&ctx)) {
        poc_cleanup_renderer(&ctx);
        g_main_loop_unref(ctx.loop);
        g_free(ctx.preset_file);
        return 1;
    }

    ctx.tick_id = g_timeout_add(16, poc_on_tick, &ctx);
    g_main_loop_run(ctx.loop);

    if (ctx.tick_id != 0)
        g_source_remove(ctx.tick_id);

    poc_cleanup_renderer(&ctx);
    if (ctx.window)
        gtk_window_destroy(GTK_WINDOW(ctx.window));
    g_main_loop_unref(ctx.loop);

    gboolean success = !ctx.failed && (ctx.auto_exit_frames == 0 || ctx.saw_visible_frame);
    if (!success && !ctx.failed && ctx.auto_exit_frames > 0)
        g_warning("poc: encerrou sem detectar frame visivel");

    g_free(ctx.preset_file);
    return success ? 0 : 1;
}
