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
 * @property {boolean} keepMinimized - Mantém a janela minimizada (não usado ainda)
 * @property {string} reparentState - 'window_group' | 'wallpaper' | null
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
            keepMinimized: false,
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
        if (this._disabled || !this._state.keepAtBottom)
            return;

        // Mantém a janela no fundo
        this._window.lower();

        const actor = this._window.get_compositor_private();
        if (actor && global.window_group && this._state.reparentState === 'window_group') {
            try {
                global.window_group.set_child_below_sibling(actor, null);
            } catch (e) {
                log(`[milkdrop] ManagedWindow: Error setting child position: ${e}`);
            }
        }

        if (this._callbacks.onRaised)
            this._callbacks.onRaised(this._window);
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
     * Define uma propriedade de estado.
     * @param {string} key - Chave do estado
     * @param {any} value - Valor a definir
     */
    setState(key, value) {
        if (key in this._state)
            this._state[key] = value;
    }

    /**
     * Checks if an actor is valid and can be operated on.
     * @param {Clutter.Actor} actor
     * @returns {boolean}
     */
    _isActorValid(actor) {
        if (!actor)
            return false;
        if (typeof actor.is_finalized === 'function' && actor.is_finalized())
            return false;
        if (typeof actor.is_destroyed === 'function' && actor.is_destroyed())
            return false;
        if (typeof actor.get_stage === 'function' && !actor.get_stage())
            return false;
        return typeof actor.get_parent === 'function';
    }

    /**
     * Ancora a janela no wallpaper: move para o monitor correto,
     * aplica sticky, redimensiona para cobrir o monitor, e posiciona no fundo.
     * @param {Meta.Rectangle} geometry - Geometria do monitor (x, y, width, height)
     */
    anchor(geometry) {
        const actor = this._window.get_compositor_private();
        if (!this._isActorValid(actor)) {
            log(`[milkdrop] ManagedWindow: Invalid actor for anchoring on monitor ${this._monitorIndex}`);
            return;
        }

        const parent = actor.get_parent();
        const parentName = parent ? (parent._milkdropWallpaper ? `wallpaper(StWidget)` : String(parent)) : 'null';
        log(`[milkdrop] ManagedWindow: Actor parent: ${parentName}`);

        // If already in wallpaper (parent has _milkdropWallpaper === true), skip window_group anchoring
        if (parent && parent._milkdropWallpaper === true) {
            this._state.reparentState = 'wallpaper';
            log('[milkdrop] ManagedWindow: Actor already in wallpaper, skipping window_group anchoring');
        } else {
            this._state.reparentState = 'window_group';
            // Move actor to the bottom of the window group
            try {
                if (parent && parent !== global.window_group)
                    parent.remove_child(actor);
                if (actor.get_parent() !== global.window_group)
                    global.window_group.add_child(actor);
                global.window_group.set_child_below_sibling(actor, null);
                log('[milkdrop] ManagedWindow: Actor moved to bottom of window_group');
            } catch (e) {
                log(`[milkdrop] ManagedWindow: Error moving actor to window_group: ${e}`);
                return;
            }
        }

        // Aplica sticky (visível em todos os workspaces)
        this._window.stick();
        log('[milkdrop] ManagedWindow: Window stuck to all workspaces');

        // Move para o monitor correto e redimensiona
        this.enforceCoverage(geometry);

        // Garante que fica no fundo
        this._window.lower();
        log('[milkdrop] ManagedWindow: Window lowered in Meta stack');
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

        try {
            this._window.move_to_monitor(this._monitorIndex);
            log(`[milkdrop] ManagedWindow: move_to_monitor(${this._monitorIndex}) succeeded`);
        } catch (e) {
            log(`[milkdrop] ManagedWindow: move_to_monitor failed: ${e}`);
        }

        try {
            this._window.move_resize_frame(
                false,
                geometry.x,
                geometry.y,
                geometry.width,
                geometry.height
            );
            log(`[milkdrop] ManagedWindow: move_resize_frame to ${geometry.width}x${geometry.height} succeeded`);
        } catch (e) {
            log(`[milkdrop] ManagedWindow: move_resize_frame failed: ${e}`);
        }
    }

    /**
     * Desconecta todos os sinais e limpa referências.
     * Deve ser chamado quando a janela não precisa mais ser gerenciada.
     */
    disable() {
        this._disabled = true;
        
        for (const signalId of this._signals) {
            try {
                this._window.disconnect(signalId);
            } catch (e) {
                // Ignora erros de desconexão (janela pode já ter sido destruída)
            }
        }
        this._signals = [];
        this._window = null;
        this._callbacks = {};
    }
}
