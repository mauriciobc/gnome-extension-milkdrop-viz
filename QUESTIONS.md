# QUESTIONS.md — Revisão técnica e dúvidas para o time

Este arquivo lista **perguntas independentes** levantadas numa passagem de revisão de código e arquitetura do repositório **gnome-extension-milkdrop-viz** (binário C `milkdrop` + extensão GNOME Shell em GJS). O objetivo é você responder **abaixo de cada pergunta** (ou numerar respostas num apêndice) para separar o que é **bug**, **dívida técnica aceita**, **comportamento intencional** ou **ideia futura**.

> **Nota de escopo:** `reference_codebases/` foi tratado como material de referência (read-only conforme regras do repo); as perguntas focam no código “de produto”, extensão, renderer, testes e docs de primeiro nível.

---

## 1. Visão de produto e limites

1. O PRD e vários docs dizem **“Wayland only”**, mas `extension.js` implementa caminho **X11** (`launcher.spawnv` e matching por `wm_class`/`title`). Isso é **suporte real e desejado** (documentação desatualizada) ou **código legado / fallback** que deveria ser removido ou desativado em build de produção?

2. `metadata.json` declara **shell-version até `"50"`**, enquanto documentação em `CLAUDE.md` / `AGENTS.md` cita **47–49**. O alvo oficial de suporte é **47–49**, **47–50**, ou **apenas o que for testado em CI**? Há matriz de testes manuais por versão?

3. A extensão descreve-se como controladora de um renderer “wallpaper-layer”. Há requisito explícito de **funcionar em sessões aninhadas** (nested shell), **KVM**, ou **múltiplos usuários** — e isso influencia decisões (ex.: paths de socket, `XDG_RUNTIME_DIR`)?

---

## 2. Arquitetura geral (dois processos)

4. O modelo “**sem IPC por frame**” está claro; porém há **polling de status nas prefs** (2 s) e possíveis leituras de socket em outros fluxos. Qual é a **política oficial** de quanto tráfego de controle é aceitável (prefs aberta, debug, futuras features)?

5. Quando `all-monitors` está ativo, há **N instâncias** do renderer. Como deve ser o comportamento de **GSettings “globais”** (`opacity`, `shuffle`, `preset-dir`, `fps`) — sempre **broadcast** idêntico (como hoje), ou existe caso de **por monitor** no roadmap?

6. Os keys `last-preset` e `was-paused` existem no schema mas **não aparecem** em buscas rápidas no código da extensão/renderer. Qual era o **plano de persistência** (save-state/restore-state via socket, snapshot por monitor, migração futura)? Estão **reservados** ou são **órfãos**?

---

## 3. Renderer C — GTK / GL / projectM (`src/main.c`, `src/renderer.c`)

7. Há muitos `g_message` / `g_warning` no caminho de renderização (ex.: `on_render` a cada ~60 frames, logs de startup gate). Isso é **estratégia consciente de diagnóstico** ou deveria estar **100% atrás de `verbose`** para não degradar performance / encher journald?

8. Comentários e mensagens misturam **inglês e português** no mesmo arquivo (`main.c`, `managedWindow.js`). Qual é o **padrão oficial** de idioma para logs e comentários (EN-only, PT-BR-only, ou “logs EN / comentários livres”)?

9. `startup_hidden` e `milkdrop_mark_startup_hidden` passaram por mudanças (“nunca esconder”, warmup). A máquina de estados **startup gate** ainda reflete o desenho original do PRD ou merece **diagrama único** (documento curto) para onboarding?

10. `glFinish()` após cada frame renderizado com projectM foi documentado como necessário para blur/readback. Foi medido o impacto em **GPU sync**, **latência**, e consumo em **144 Hz** com presets pesados? Existe alternativa aceitável (barriers mais finas) ou é **não negociável** com GTK atual?

11. `milkdrop_probe_startup_pixels` faz `glReadPixels` em vários pontos durante warmup. Isso ainda é **essencial** com o gate atual ou virou **custo fixo** que poderia ser reduzido?

12. `build_window` inicializa `render_width`/`render_height` com o tamanho **lógico** do monitor, mas `on_resize` usa **backing store / scale**. Pode haver **um frame** com dimensões inconsistentes ou mismatch entre `gtk_window_set_default_size` e o primeiro `resize`? Isso já foi observado em bug real?

13. `MILKDROP_FORCE_GL_API` é forçado na extensão ao spawnar. Qual é o plano se futuras versões GTK **mudarem defaults** ou se **GLES** voltar a ser requisito em algum hardware?

14. Screenshot via comando de controle grava **PPM RGB** via `fopen`/`malloc` full-frame. Isso é **feature suportada** para usuários, só para testes, ou experimental? Há limite de tamanho / risco de **OOM** em 4K?

---

## 4. Controle / protocolo / concorrência (`src/control.c`, `src/control.h`, `controlClient.js`)

15. `docs/SECURITY.md` menciona socket default `milkdrop.sock`, mas o código usa **`milkdrop-<monitor>.sock`**. A documentação de segurança deve ser **atualizada** ou existe modo legado ainda suportado?

16. O socket é `0600`, mas **qualquer processo do mesmo UID** pode conectar (`docs/SECURITY.md`). Há intenção de implementar **`SO_PEERCRED`** / **`getpeereid`** ou isso foi **explicitamente descartado** por complexidade?

17. O comando `screenshot <path>` aceita caminho arbitrário do cliente de socket. Qual é o **modelo de ameaça**: só processos do usuário (OK), ou também **flatpak** / **sandbox** onde paths podem ser sensíveis? Deveria haver **restrição** a um subdir (ex.: `XDG_PICTURES`)?

18. `restore-state` aplica `preset-dir` com string vinda do cliente. Há validação desejada de **path exists**, **symlinks**, ou **permissões** antes de trocar diretório?

19. `queryMilkdropStatus` em `controlClient.js` lê **apenas 4096 bytes** da resposta, enquanto `_readResponseFully` existe para outros fluxos. O `status` pode crescer (preset path longo) e **truncar** parsing — isso é aceitável?

20. `CONTROL_MAX_RESPONSE_BYTES` no JS usa `5 * 4096` com comentário “alinhado a PATH_MAX * 5”; no Linux `PATH_MAX` costuma ser **4096**, mas não é garantido universalmente. Deve o cliente **consultar** o mesmo limite do C (constante compartilhada / teste de contrato)?

21. Há **múltiplas conexões** simultâneas ao socket (listen backlog 8). O servidor trata **um cliente por vez** por design? Há cenário de prefs + extensão + script concorrendo e **respostas intercaladas**?

---

## 5. Áudio / PipeWire / ring buffer (`src/audio.c`, `src/app.h`, `audio_recovery`)

22. O stream PipeWire está fixo em **48 kHz estéreo F32**. Presets e projectM assumem taxa específica? Há plano para **resampling** se o sink for outra taxa, ou isso é **garantido pelo PipeWire** no caminho atual?

23. `MILKDROP_RING_CAPACITY` é **16384 floats**. Qual é a latência máxima tolerada e o comportamento esperado quando o produtor **ultrapassa** o consumidor (drop silencioso)? Há métrica/telemetria de **underrun**?

24. `audio_reprobe_cb` faz teardown/init no **main loop GLib**, enquanto o stream vive em **thread loop** PipeWire. Está provado que não há **use-after-free** ou corrida entre `audio_cleanup` e callbacks tardios?

25. Após `AUDIO_MAX_RESTARTS`, o status vira `audio=failed`. Qual é a **experiência de usuário** esperada (notificação shell, fallback silencioso, botão “retry” nas prefs)?

---

## 6. Presets / quarentena (`src/presets.c`, `src/quarantine.c`, playlist)

26. `presets_reload` faz varredura **recursiva** de subdiretórios. Isso é desejado para coleções grandes? Há limite de arquivos / **tempo de startup** documentado?

27. Arquivos em diretórios cujo nome começa com `!` são ignorados. Isso é compatibilidade com **convenções MilkDrop** — documentar para usuários?

28. A quarentena usa lista fixa e `QUARANTINE_FAILURE_THRESHOLD`. O que acontece quando **todos** falham (`quarantine_all_failed`) — rotação para preset vazio, mensagem ao usuário, ou apenas logs?

29. `shuffle` existe como flag de CLI e runtime via socket; o schema GSettings também tem `shuffle`. A semântica **startup vs runtime** está totalmente alinhada entre extensão e renderer?

---

## 7. Extensão GNOME Shell (`extension.js`)

30. `_isWayland()` pode retornar true com `Meta.is_wayland_compositor` mas a extensão ainda cai em ramos que chamam `spawnv` “X11” se `isWayland` for false. Em qual cenário real isso acontece (Xwayland session type, SSH, etc.) e isso é **suportado**?

31. `DRI_PRIME=1` é definido **sempre** no launcher. Isso é intencional para **forçar GPU dedicada**, ou deveria ser **opt-in** (laptops híbridos podem piorar bateria / quebrar GL)?

32. `GSK_RENDERER=gl` e comentário sobre deadlock **Mesa/vkps** — há **issue upstream** referenciável? Em qual stack isso foi reproduzido (distro, driver)?

33. O spawn usa `Meta.WaylandClient.new` vs `new_subprocess` conforme `shellMajor < 49`. Houve validação em **49 e 50** com ambos os caminhos? `new_subprocess` retornando `null` — qual é o plano de fallback além de log?

34. `_onWindowMapped` usa `window-created` via `window_manager map` (comentário Hanabi). Ainda há casos em que **outra janela** spoofa `wm_class`/`title` `milkdrop` e seria ancorada erroneamente? O `owns_window` é a **única** fonte de verdade desejada?

35. O título inicial inclui JSON `@milkdrop!{...}` mas a extensão **não parseia** esse JSON nas buscas atuais. Esse payload é **para futuro**, para outras ferramentas, ou **código morto** no renderer?

36. `disable()` é síncrono e avisa sobre `wait_async` / `force_exit` escalonado possivelmente **sobreviver** ao disable. Qual é o invariante desejado: “nunca vaza sinais após disable” ou “aceitável por X ms”? Há testes de **stress enable/disable**?

37. `_stopProcess` e `RENDERER_STOP_ESCALATION_MS`: em quais condições o processo fica **zumbi** / **D-state** observado na prática? O segundo `force_exit` resolve todos os casos conhecidos?

38. `PausePolicy` para maximizado percorre **todos** `get_window_actors` a cada mudança. Em máquinas com muitas janelas, isso é **OK** ou deveria haver **debounce** / cache por monitor?

39. `MprisWatcher` trata “nenhum player = pausar” via `!isAnyPlaying`. Isso pausa quando **não há players** no bus — intencional? Usuários com áudio que **não expõe MPRIS** (navegador, app sem integração) ficariam sempre pausados se `media-aware` estiver on?

40. Agregação de pause (`fullscreen OR maximized OR mpris`) é **única** para todos os monitores (`_broadcastControlCommand`). Em `all-monitors`, deveria ser possível **monitor A pausado e B não** (ex.: fullscreen só num monitor)?

---

## 8. `ManagedWindow` / compositor / clones

41. `ManagedWindow` mistura comentários PT e nomes de API em EN. Há guia de estilo para módulos “Hanabi-like”?

42. `keepMinimized` existe no estado mas está “não usado ainda”. Há roadmap ou deve ser removido até existir feature?

43. Injeções em `Background`, `Workspace`, `WorkspaceThumbnail`, overview, etc.: qual é a **lista canônica** de superfícies que **devem** mostrar o clone vs onde é **opcional**? Como detectar regressões visuais (checklist manual)?

44. `_reloadBackgrounds` e debounce: qual é o pior caso de **flicker** ou **CPU spike** ao plugar/desplugar monitor repetidamente?

---

## 9. Preferências (`prefs.js`)

45. O poll de status a cada **2 s** multiplicado por `numMonitors` sockets: há preocupação com **wakeups** / bateria em laptops? Já se considerou **só poll quando a página está visível** ou backoff?

46. `monitor` SpinRow vai até **16** enquanto o schema não fixa upper bound além do tipo `i`. Deveria refletir **monitores reais** (`Gdk.Display.get_monitors().get_n_items()`)?

47. Há intenção de expor nas prefs comandos avançados (`save-state`/`restore-state`, screenshot) ou isso fica **só para devs**?

---

## 10. Cliente de controle JS (`controlClient.js`)

48. `_readResponseFully` concatena com loop `reduce` para tamanho a cada chunk — aceitável ou prefere acumular **tamanho incremental**?

49. `queryMilkdropStatus` encerra conexão com `close_async` sem `await` da callback em alguns erros. Há preocupação com **fd leak** em stress, ou o GC/fechamento do Gio cobre?

50. Funções “fire-and-forget” (`sendMilkdropControlCommand`) não propagam falhas para UI. Deveria existir **sinalização** nas prefs quando o renderer está down?

---

## 11. Build, empacotamento, versionamento

51. Versão do projeto em `meson.build` é **0.1.0** enquanto `metadata.json` tem `"version": 2`. Qual número é o **canônico** para releases (semver do binário vs versão da extensão)?

52. Quando `projectm` ou `pipewire` estão **disabled** no build, a extensão ainda tenta spawnar o binário “placeholder”? O UX esperado é **mensagem clara** ou falha silenciosa?

---

## 12. Testes (`tests/`)

53. A suíte Meson cobre protocolo, presets, GLArea FBO, etc. Há lacuna conhecida de **testes de integração** entre **extensão + renderer** além do script opcional de compositor?

54. `validate_scaffold.py` valida chaves do schema de forma **exata** (`key_names == expected`). Qual processo garante que **novas keys** no XML não esqueçam de atualizar o validador?

55. Testes que precisam de **DISPLAY** — rodam no CI atual ou só localmente?

---

## 13. Documentação e consistência

56. `CLAUDE.md` / `AGENTS.md` ainda listam testes/nomes que não batem 1:1 com `tests/meson.build` (ex.: nomes adicionais como `preset_quarantine`). Qual arquivo é a **fonte da verdade** para a lista de testes?

57. `docs/SECURITY.md` vs implementação de paths: além da atualização de path, há outros **desvios doc↔código** que o time já conhece?

---

## 14. Scripts e artefatos (`tools/`, `artifacts/`)

58. Scripts como `profile_gpu_milkdrop.sh`, `nested_devkit.sh`, `analyze_on_render_journal.py` — são **oficiais** para contribuidores ou internos? Devem entrar no README principal?

59. A pasta `artifacts/gpu_profile/...` versionada no repo é **intencional** (baseline de perf) ou deveria estar em **.gitignore** / releases separadas?

---

## 15. “Páginas” e endpoints

60. Este projeto **não** expõe HTTP REST tradicional; o “endpoint” principal é o **socket Unix texto**. Há planos de **D-Bus** service (substituto ou complemento) para descoberta, auth, ou integração com `gnome-settings`?

61. Se um dia houver **interface web local** para controle, como isso se encaixaria no **modelo de ameaça** atual (UID session trust boundary)?

---

## 16. Bugs potenciais observados (para você classificar: bug vs intencional)

62. `extension.js` fallback de matching usa `window.title === 'milkdrop'`, mas o renderer define título como **`@milkdrop!{...}`** via `milkdrop_set_initial_title`. Esse fallback **nunca dispara** por igualdade de string — é bug, ou o fallback depende só de `get_wm_class() === 'milkdrop'` e o check de `title` é legado?

63. `PausePolicy` usa `'notify::maximized-vertically'` / `'notify::maximized-horizontally'`, enquanto em outros trechos usa-se `maximized_vertically` (underscore) via MetaWindow JS bindings. Está garantido que **ambos** funcionam no GJS do Shell alvo, ou um deles é **no-op**?

64. `MprisWatcher._removePlayer` itera `this._players` com `delete` durante loop `for...of` no Map — em JS isso é seguro, mas o `disable()` faz unsubscribe e `delete` em sequência. Há cenário de **double unsubscribe** ou id **0** tratado incorretamente?

---

## 17. Performance de longo prazo

65. `glFinish` + possíveis readbacks + `g_idle_add` para `queue_render` cada frame com projectM: qual é o **orçamento de frame** alvo (ms) e como medir regressão entre versões de **Mutter**, **GTK**, **projectM**?

66. Varredura recursiva de presets + ordenação lexicográfica em diretórios enormes: aceitável mover para **thread** ou cache com `mtime`?

---

## 18. Acessibilidade e UX do produto

67. Visualizador “wallpaper” pode interferir em **reduced motion** / preferências de acessibilidade do GNOME. Existe requisito de respeitar **`prefers-reduced-motion`** (via settings próprios ou portal)?

68. Logs com prefixo `[milkdrop]` podem conter **caminhos locais** ou estado. Alguma política de **privacidade** para attachments de bug reports?

---

## 19. Licenças e distribuição

69. Presets `.milk` de terceiros: o projeto pretende **distribuir presets** ou apenas o motor? Há implicações de **licença** documentadas para usuários finais?

---

## 20. Ordem sugerida para suas respostas

Para facilitar o próximo passo (implementação guiada pelas respostas), você pode marcar cada resposta com uma etiqueta curta:

- **`[INTENCIONAL]`** — comportamento desejado; talvez só falte doc.
- **`[BUG]`** — precisa correção.
- **`[DÍVIDA]`** — conhecido; aceito por ora.
- **`[ROADMAP]`** — não existe ainda; planejado.
- **`[INCERTO]`** — precisa investigação/medição.

---

### Espaço para respostas (template opcional)

Copie/cole blocos abaixo e preencha.

```
## Resposta Q4
[INTENCIONAL] ...

## Resposta Q62
[BUG] ...
```
