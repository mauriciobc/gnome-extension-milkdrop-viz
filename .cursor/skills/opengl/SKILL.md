---
name: opengl
description: Debug, diagnose, and optimize OpenGL/GLSL rendering pipelines (EGL/GLX, GtkGLArea, FBO/PBO, shader errors). Use when the user mentions OpenGL, GLSL, EGL, GLX, GtkGLArea, GL errors, framebuffers, readback, RenderDoc, apitrace, or performance issues.
---

# OpenGL

## Escopo

Use esta skill quando o pedido envolver **OpenGL/GLSL** (incluindo **EGL/GLX**, **GtkGLArea/GTK**, FBO/PBO, readback) e o objetivo for **debug**, **diagnóstico**, **performance** ou **reprodutor mínimo**.

## Princípios (especialmente em GNOME Shell / GJS)

- **Não travar o loop**: evite chamadas GL bloqueantes no hot path (ex.: `glFinish`, `glGetTexImage` grande, `glReadPixels` sem PBO).
- **Minimizar spam**: não logar por-frame (principalmente dentro do processo do Shell).
- **Consertar com evidência**: reproduzir → medir → mudar 1 coisa → medir de novo.

## Checklist — Debug rápido (GL “não desenha” / tela preta)

- **Contexto válido**: confirmar que o contexto está corrente no thread certo antes de qualquer call GL.
- **Versão/Extensões**: coletar `glGetString(GL_VERSION)`, `GL_RENDERER`, `GL_VENDOR`, `GL_SHADING_LANGUAGE_VERSION`.
- **Erros imediatos**: inserir checkpoints curtos com `glGetError()` (em pontos-chave, não em loop).
- **Shaders**:
  - checar compile/link e imprimir `infoLog`
  - validar que `glUseProgram()` foi chamado
  - confirmar binding de atributos/locations e uniforms (incluindo matrizes e samplers)
- **Estado mínimo**:
  - viewport correto (`glViewport`) após resize/HiDPI
  - `glBindVertexArray`, `glBindBuffer`, `glEnableVertexAttribArray`, `glVertexAttribPointer`
  - `glClearColor`/`glClear` e `glDisable(GL_CULL_FACE)`/`glDisable(GL_DEPTH_TEST)` temporariamente pra isolar
- **Texturas/FBO**:
  - checar `glCheckFramebufferStatus(GL_FRAMEBUFFER)` quando usar FBO
  - confirmar formatos internos/attachments e tamanho não-zero

## Checklist — Debug “de verdade” (quando é intermitente)

- **KHR_debug** (se disponível):
  - habilitar debug output e filtrar severidade (não “logar tudo”)
  - rotular objetos (programs/textures/FBOs) pra facilitar no RenderDoc
- **Captura**:
  - usar **RenderDoc** quando possível (pipeline/estado/recursos)
  - usar **apitrace** para reproduzir e comparar entre máquinas/drivers
- **Isolar**:
  - criar um **reprodutor mínimo**: 1 triângulo, 1 shader, 1 textura (se preciso)
  - desligar features: blending → FBO → ping-pong → postprocess, até achar o ponto de quebra

## Checklist — Performance (stutter, alta CPU/GPU, “cai FPS”)

- **Medir primeiro**:
  - separar **CPU vs GPU** (tempo no loop vs tempo no swap/present)
  - confirmar se há **stall** (ex.: readback, sync, buffer orphaning mal feito)
- **Hot spots comuns**:
  - estado: reduzir `glBind*`, `glUseProgram`, `glUniform*` redundantes
  - alocação: não recriar buffers/texturas por-frame
  - upload: preferir atualização incremental (`glBufferSubData`/mapeamento) ao invés de reupload total
  - readback: preferir **PBO async** + double-buffer (evitar `glReadPixels` direto)
- **Sincronização**:
  - evitar `glFinish`; se precisar sincronizar, usar fences com parcimônia
  - cuidado com `glFlush` em excesso (geralmente não é necessário)

## Checklist — Interop com UI/loop (GtkGLArea/GTK, resize, HiDPI)

- **Thread/loop**: garantir que draw ocorre no callback certo e com contexto corrente.
- **Resize**: recalcular viewport + tamanhos de FBO/attachments quando o widget mudar.
- **HiDPI**: usar o tamanho em pixels reais (scale factor) para viewport e FBO.
- **Swap/present**: checar se o “present” está bloqueando (vsync) e se isso é esperado.

## Checklist — Teste e verificação (sem overengineering)

- **Repro**: um caso mínimo que falha de forma determinística.
- **Assertivo**:
  - validar `glCheckFramebufferStatus` em FBO setup
  - validar compile/link dos shaders e falhar cedo com log útil
- **Regressão**:
  - adicionar um teste pequeno (quando der) que exercite o setup e falhe antes de “desenhar nada”

## Como eu devo responder (formato)

Quando você pedir ajuda de OpenGL, eu devo devolver:
- **Hipótese principal** (1 frase)
- **3–8 passos do checklist** (os mais prováveis pro caso)
- **O que medir/observar** em cada passo (ex.: “se o FBO não fica COMPLETE, o bug está no setup”)

## Referências

- **GLSL Specification (Khronos OpenGL Wiki)**: https://www.khronos.org/opengl/wiki/Core_Language_(GLSL)
