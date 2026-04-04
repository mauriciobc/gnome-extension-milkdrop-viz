# API mínima libprojectM para o renderer

Resumo da API C de libprojectM (projectM-4) usada pelo processo renderer da extensão.

## Inicialização e contexto OpenGL

- **projectm_create_with_opengl_load_proc(load_proc, user_data)**  
  Cria uma instância. O contexto OpenGL deve estar ativo e corrente.  
  `load_proc(name, user_data)` deve retornar o ponteiro da função GL (ex.: `eglGetProcAddress(name)` ou `epoxy_glGetProcAddress(name)`).

- **projectm_destroy(instance)**  
  Destrói a instância e libera recursos.

## Tamanho da janela

- **projectm_set_window_size(instance, width, height)**  
  Define o viewport em pixels. Deve ser chamado com tamanho não nulo para haver renderização.

- **projectm_get_window_size(instance, &width, &height)**  
  Retorna o tamanho atual.

## Áudio (PCM)

- **projectm_pcm_get_max_samples()**  
  Retorna o número máximo de amostras por canal que podem ser armazenadas.

- **projectm_pcm_add_float(instance, samples, count, channels)**  
  Adiciona amostras float em [-1, 1].  
  Estéreo: ordem LRLRLR.  
  `channels`: `PROJECTM_MONO` (1) ou `PROJECTM_STEREO` (2).

## Preset

- **projectm_load_preset_file(instance, filename, smooth_transition)**  
  Carrega preset por caminho (ou URL `file://`; `idle://` para preset idle).

- **projectm_load_preset_data(instance, data, smooth_transition)**  
  Carrega preset a partir de string (formato Milkdrop).

## Tempo e renderização

- **projectm_set_frame_time(instance, seconds_since_first_frame)**  
  Define o tempo do frame (>= 0). Valores &lt; 0 usam o relógio do sistema.

- **projectm_opengl_render_frame(instance)**  
  Renderiza um frame no framebuffer padrão (0).

- **projectm_opengl_render_frame_fbo(instance, framebuffer_object_id)**  
  Renderiza um frame em um FBO específico (recomendado para readback).

## Parâmetros opcionais

- **projectm_set_texture_search_paths(instance, paths, count)**  
  Lista de diretórios para texturas dos presets.

- **projectm_set_beat_sensitivity(instance, sensitivity)**  
- **projectm_set_fps(instance, fps)**  
- **projectm_set_preset_duration(instance, seconds)**  
- **projectm_set_preset_locked(instance, lock)**  
  Bloqueia troca automática de preset.

## Obtenção de pixels RGBA

A biblioteca não expõe “read pixels”; ela desenha no framebuffer atual (ou no FBO passado para `projectm_opengl_render_frame_fbo`). O fluxo para obter RGBA é:

1. Criar um FBO com textura anexada (formato RGBA8, tamanho = window size).
2. A cada frame: fazer bind do FBO, chamar **projectm_opengl_render_frame_fbo(instance, fbo_id)**.
3. **glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buffer)** a partir desse FBO.
4. Enviar o buffer via **SHM/FD** (`frame-pixels-fd`).

## Ordem típica por frame

1. `projectm_set_frame_time(instance, t)`
2. Intercalar `pcmLeft`/`pcmRight` e chamar `projectm_pcm_add_float(instance, samples, count, PROJECTM_STEREO)`
3. (Opcional) Se mudou o preset: `projectm_load_preset_file(instance, path, smooth)`
4. Bind do FBO de readback
5. `projectm_opengl_render_frame_fbo(instance, fbo_id)`
6. `glReadPixels(...)` do FBO para o buffer
7. Enviar `frame-pixels-fd` (metadata via stdout + FD via socket SHM).
