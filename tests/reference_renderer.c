/* Minimal reference renderer for blur validation testing
 * 
 * This creates a simple GTK4+GLArea renderer identical to our extension
 * but without the compositor integration, to serve as ground truth for
 * comparing blur rendering output.
 */

#include <gtk/gtk.h>
#include <epoxy/gl.h>
#include <projectM-4/projectM.h>
#include <projectM-4/playlist.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    GtkWidget *window;
    GtkWidget *gl_area;
    projectm_handle projectm;
    projectm_playlist_handle playlist;
    int width;
    int height;
    int frame_count;
    char *preset_file;
    char *screenshot_path;
    int target_frame;
    bool deferred_preset;
} RefData;

static gboolean
on_render(GtkGLArea *area, GdkGLContext *context, gpointer user_data)
{
    RefData *data = user_data;
    
    gtk_gl_area_make_current(area);
    gtk_gl_area_attach_buffers(area);
    
    GLint draw_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    
    // Update projectM time
    static gint64 start_time = 0;
    if (start_time == 0) {
        start_time = g_get_monotonic_time();
    }
    double elapsed = (g_get_monotonic_time() - start_time) / 1000000.0;
    projectm_set_frame_time(data->projectm, elapsed);
    
    // Activate deferred preset on first render frame (like extension does)
    if (data->deferred_preset && data->playlist) {
        data->deferred_preset = false;
        projectm_playlist_set_position(data->playlist, 0, true);
        g_print("Deferred preset activated at frame %d\n", data->frame_count);
    }
    
    // Feed dummy audio data (projectM needs this for beat detection and reactivity)
    // Generate simple sine wave for visual testing
    float pcm_data[512 * 2]; // 512 stereo frames
    for (int i = 0; i < 512 * 2; i++) {
        float t = (data->frame_count * 512 + i / 2) / 44100.0;
        pcm_data[i] = 0.5f * sinf(2.0f * M_PI * 440.0f * t); // 440 Hz sine wave
    }
    projectm_pcm_add_float(data->projectm, pcm_data, 512, PROJECTM_STEREO);
    
    // Render with projectM (same as extension)
    if (draw_fbo != 0) {
        projectm_opengl_render_frame_fbo(data->projectm, (uint32_t)draw_fbo);
    } else {
        projectm_opengl_render_frame(data->projectm);
    }
    
    // CRITICAL: glFinish() for blur synchronization (same as extension)
    glFinish();
    
    // Restore GL state (same as extension)
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    
    data->frame_count++;
    
    if (data->frame_count % 30 == 0) {
        g_print("Frame %d rendered\n", data->frame_count);
    }
    
    // Take screenshot at target frame
    if (data->frame_count == data->target_frame && data->screenshot_path) {
        g_print("Capturing frame %d to %s\n", data->frame_count, data->screenshot_path);
        g_print("Current FBO: %d, Render size: %dx%d\n", draw_fbo, data->width, data->height);
        
        // Read from the FBO that was just rendered to
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        g_print("Viewport: %d,%d %dx%d\n", viewport[0], viewport[1], viewport[2], viewport[3]);
        
        int read_width = viewport[2] > 0 ? viewport[2] : data->width;
        int read_height = viewport[3] > 0 ? viewport[3] : data->height;
        
        // The render went to draw_fbo (the internal GtkGLArea FBO)
        // We need to read from that same FBO, not from framebuffer 0
        // First, ensure the draw and read fbos are the same
        glBindFramebuffer(GL_READ_FRAMEBUFFER, draw_fbo);
        
        GLubyte *pixels = malloc(read_width * read_height * 3);
        glReadPixels(0, 0, read_width, read_height, GL_RGB, GL_UNSIGNED_BYTE, pixels);
        
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            g_warning("glReadPixels error: 0x%x", err);
        }
        
        // Check if we got any non-zero pixels
        int non_zero = 0;
        for (int i = 0; i < read_width * read_height * 3; i++) {
            if (pixels[i] != 0) non_zero++;
        }
        g_print("Non-zero pixels: %d / %d (%.2f%%)\n", non_zero, read_width * read_height * 3,
                100.0 * non_zero / (read_width * read_height * 3));
        
        // Write PPM (simple format, no dependencies)
        FILE *f = fopen(data->screenshot_path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", read_width, read_height);
            // Flip vertically (OpenGL origin is bottom-left)
            for (int y = read_height - 1; y >= 0; y--) {
                fwrite(pixels + y * read_width * 3, 1, read_width * 3, f);
            }
            fclose(f);
            g_print("Screenshot saved successfully\n");
        }
        free(pixels);
        
        // Exit after screenshot
        g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(data->window))));
    }
    
    return TRUE;
}

static void
on_realize(GtkGLArea *area, gpointer user_data)
{
    RefData *data = user_data;
    
    gtk_gl_area_make_current(area);
    
    g_print("GL Context created\n");
    g_print("Initializing projectM with preset: %s\n", data->preset_file);
    
    // Initialize projectM (same settings as extension)
    data->projectm = projectm_create();
    if (!data->projectm) {
        g_error("Failed to create projectM instance");
        return;
    }
    
    // Create playlist (same as extension)
    data->playlist = projectm_playlist_create(data->projectm);
    if (!data->playlist) {
        g_error("Failed to create projectM playlist");
        projectm_destroy(data->projectm);
        data->projectm = NULL;
        return;
    }
    
    // Configure texture search paths (same as extension)
    if (data->preset_file) {
        char *dir = g_path_get_dirname(data->preset_file);
        if (dir && g_file_test(dir, G_FILE_TEST_IS_DIR)) {
            const char* tex_paths[] = {dir};
            projectm_set_texture_search_paths(data->projectm, tex_paths, 1);
            g_print("Texture search path: %s\n", dir);
        }
        g_free(dir);
    }
    
    // Set window size
    projectm_set_window_size(data->projectm, data->width, data->height);
    projectm_set_preset_duration(data->projectm, 300.0);
    
    // Add preset to playlist (like test does - uses playlist API, not direct load)
    if (data->preset_file && g_file_test(data->preset_file, G_FILE_TEST_IS_REGULAR)) {
        if (!projectm_playlist_add_preset(data->playlist, data->preset_file, true)) {
            g_warning("Failed to add preset to playlist");
        } else {
            g_print("Preset added to playlist\n");
        }
    }
    
    // CRITICAL: Load idle:// first (like extension + test do)
    projectm_load_preset_file(data->projectm, "idle://", false);
    g_print("Idle preset loaded\n");
    
    // Mark for deferred activation (like extension startup logic)
    // We'll activate it in the first render frame
    data->deferred_preset = true;
    
    g_print("ProjectM initialized\n");
}

static void
on_unrealize(GtkGLArea *area, gpointer user_data)
{
    RefData *data = user_data;
    
    gtk_gl_area_make_current(area);
    
    if (data->projectm) {
        projectm_destroy(data->projectm);
        data->projectm = NULL;
    }
}

static gboolean
on_resize(GtkGLArea *area, int width, int height, gpointer user_data)
{
    RefData *data = user_data;
    
    data->width = width;
    data->height = height;
    
    if (data->projectm) {
        gtk_gl_area_make_current(area);
        projectm_set_window_size(data->projectm, width, height);
    }
    
    return FALSE;
}

static gboolean
on_idle_render(gpointer user_data)
{
    RefData *data = user_data;
    
    if (data->gl_area) {
        gtk_widget_queue_draw(data->gl_area);
    }
    
    return G_SOURCE_CONTINUE;
}

static void
activate(GtkApplication *app, gpointer user_data)
{
    RefData *data = user_data;
    
    data->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(data->window), "ProjectM Reference Renderer");
    gtk_window_set_default_size(GTK_WINDOW(data->window), data->width, data->height);
    
    data->gl_area = gtk_gl_area_new();
    gtk_gl_area_set_required_version(GTK_GL_AREA(data->gl_area), 3, 3);
    gtk_window_set_child(GTK_WINDOW(data->window), data->gl_area);
    
    g_signal_connect(data->gl_area, "realize", G_CALLBACK(on_realize), data);
    g_signal_connect(data->gl_area, "unrealize", G_CALLBACK(on_unrealize), data);
    g_signal_connect(data->gl_area, "render", G_CALLBACK(on_render), data);
    g_signal_connect(data->gl_area, "resize", G_CALLBACK(on_resize), data);
    
    // Queue render updates (60 FPS)
    g_timeout_add(16, on_idle_render, data);
    
    gtk_window_present(GTK_WINDOW(data->window));
}

int
main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <preset_file> <output_screenshot.ppm> <frame_number> [width] [height]\n", argv[0]);
        return 1;
    }
    
    RefData data = {0};
    data.preset_file = argv[1];
    data.screenshot_path = argv[2];
    data.target_frame = atoi(argv[3]);
    data.width = argc > 4 ? atoi(argv[4]) : 1920;
    data.height = argc > 5 ? atoi(argv[5]) : 1080;
    
    g_print("Reference renderer starting:\n");
    g_print("  Preset: %s\n", data.preset_file);
    g_print("  Output: %s\n", data.screenshot_path);
    g_print("  Target frame: %d\n", data.target_frame);
    g_print("  Resolution: %dx%d\n", data.width, data.height);
    
    GtkApplication *app = gtk_application_new("com.milkdrop.reference", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &data);
    
    int status = g_application_run(G_APPLICATION(app), 1, &argv[0]);
    
    g_object_unref(app);
    return status;
}
