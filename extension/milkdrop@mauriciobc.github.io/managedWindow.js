/**
 * ManagedWindow - Gerencia uma janela do renderer ancorada no wallpaper.
 *
 * Baseado no padrão ManagedWindow do Hanabi (hanabi/src/windowManager.js).
 * Encapsula o estado e os sinais de uma janela do renderer para manutenção
 * mais simples e cleanup garantido.
 */

import GLib from 'gi://GLib';

/**
 * Callbacks opcionais para eventos da janela.
 * @typedef {Object} WindowCallbacks
 * @property {function(MetaWindow):void} [onRaised] - Chamado quando a janela é raised
 * @property {function(MetaWindow):void} [onMoved] - Chamado quando a janela muda de posição
 * @property {function(MetaWindow):void} [onMinimized] - Chamado quando a janela é minimizada
 * @property {function(MetaWindow):void} [onShown] - Chamado quando a janela é mostrada
 */

/**
 * Estado interno da janela gerenciada.
 * @typedef {Object} WindowState
 * @property {boolean} keepAtBottom - Mantém a janela sempre no fundo
 * @property {boolean} keepPosition - Mantém a janela na posição do monitor
 * @property {string} reparentState - 'window_group' | 'wallpaper'
 */

export class ManagedWindow {
    /**
     * Cria uma nova instância de ManagedWindow.
     * @param {Meta.Window} metaWindow - A janela Meta do renderer
     * @param {number} monitorIndex - Índice do monitor onde a janela deve ficar
     * @param {WindowCallbacks} [callbacks] - Callbacks opcionais para eventos
     */
    constructor(metaWindow, monitorIndex, callbacks = {}) {
        this._window = metaWindow;
        this._monitorIndex = monitorIndex;
        this._callbacks = callbacks;
        this._signals = [];
        this._disabled = false;

        /**
         * Estado interno da janela.
         * @type {WindowState}
         */
        this._state = {
            keepAtBottom: true,
            keepPosition: true,
            reparentState: null,
        };

        this._connectSignals();
    }

    /**
     * Conecta os sinais necessários da janela.
     * @private
     */
    _connectSignals() {
        // Intercepta 'raised' para manter a janela sempre no fundo
        const raisedId = this._window.connect_after('raised', () => {
            this._onRaised();
        });
        this._signals.push(raisedId);

        // Monitora mudanças de posição/tamanho
        const positionId = this._window.connect('position-changed', () => {
            this._onMoved();
        });
        this._signals.push(positionId);

        // Monitora minimização
        const minimizedId = this._window.connect('notify::minimized', () => {
            this._onMinimized();
        });
        this._signals.push(minimizedId);

        // Monitora quando a janela é mostrada
        const shownId = this._window.connect('shown', () => {
            this._onShown();
        });
        this._signals.push(shownId);
    }

    /**
     * Handler para quando a janela é raised (trazida para frente).
     * @private
     */
    _onRaised() {
        this.relower();

        if (this._callbacks.onRaised)
            this._callbacks.onRaised(this._window);
    }

    /**
     * Força a janela para o fundo da pilha, rebaixando tanto no
     * Meta.Window quanto no ClutterActor. Idempotente.
     */
    relower() {
        if (this._disabled || !this._state.keepAtBottom)
            return;

        this._window.lower();

        const actor = this._window.get_compositor_private();
        if (actor && global.window_group && this._state.reparentState === 'window_group')
            global.window_group.set_child_below_sibling(actor, null);
    }

    /**
     * Handler para quando a janela muda de posição.
     * @private
     */
    _onMoved() {
        if (this._disabled || !this._state.keepPosition)
            return;

        if (this._callbacks.onMoved)
            this._callbacks.onMoved(this._window);
    }

    /**
     * Handler para quando a janela é minimizada/desminimizada.
     * @private
     */
    _onMinimized() {
        if (this._disabled)
            return;

        if (this._callbacks.onMinimized)
            this._callbacks.onMinimized(this._window);
    }

    /**
     * Handler para quando a janela é mostrada.
     * @private
     */
    _onShown() {
        if (this._disabled)
            return;

        if (this._callbacks.onShown)
            this._callbacks.onShown(this._window);
    }

    /**
     * Retorna a janela Meta gerenciada.
     * @returns {Meta.Window}
     */
    get window() {
        return this._window;
    }

    /**
     * Retorna o índice do monitor associado.
     * @returns {number}
     */
    get monitorIndex() {
        return this._monitorIndex;
    }

    /**
     * Retorna o estado atual da janela.
     * @returns {WindowState}
     */
    get state() {
        return { ...this._state };
    }

    /**
     * Ancora a janela no wallpaper: move para o monitor correto,
     * aplica sticky, redimensiona para cobrir o monitor, e posiciona no fundo.
     * @param {Meta.Rectangle} geometry - Geometria do monitor (x, y, width, height)
     */
    anchor(geometry) {
        const actor = this._window.get_compositor_private();
        if (!actor) {
            log(`[milkdrop] ManagedWindow: No compositor actor on monitor ${this._monitorIndex}`);
            return;
        }

        const parent = actor.get_parent();

        if (parent && parent._milkdropWallpaper) {
            this._state.reparentState = 'wallpaper';
        } else {
            this._state.reparentState = 'window_group';
            if (parent && parent !== global.window_group)
                parent.remove_child(actor);
            if (actor.get_parent() !== global.window_group)
                global.window_group.add_child(actor);
            global.window_group.set_child_below_sibling(actor, null);
        }

        this._window.stick();
        this.enforceCoverage(geometry);
        this._window.lower();
    }

    /**
     * Força a janela a cobrir todo o monitor especificado.
     * @param {Meta.Rectangle} geometry - Geometria do monitor (x, y, width, height)
     */
    enforceCoverage(geometry) {
        if (!geometry || geometry.width <= 0 || geometry.height <= 0) {
            log(`[milkdrop] ManagedWindow: Invalid geometry for monitor ${this._monitorIndex}`);
            return;
        }

        this._window.move_to_monitor(this._monitorIndex);
        this._window.move_resize_frame(
            false,
            geometry.x,
            geometry.y,
            geometry.width,
            geometry.height
        );
    }

    /**
     * Desconecta todos os sinais e limpa referências.
     * Deve ser chamado quando a janela não precisa mais ser gerenciada.
     */
    disable() {
        this._disabled = true;
        for (const signalId of this._signals)
            this._window.disconnect(signalId);
        this._signals = [];
        this._window = null;
        this._callbacks = {};
    }
}
