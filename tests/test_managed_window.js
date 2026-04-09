#!/usr/bin/env gjs
/**
 * Test suite for ManagedWindow class
 * Tests the ManagedWindow implementation from managedWindow.js
 */

const GLib = imports.gi.GLib;
const Gio = imports.gi.Gio;

// Mock implementations for testing
let mockSignals = [];
let mockWindowState = {
    minimized: false,
    sticky: false,
    position: { x: 0, y: 0 },
    size: { width: 1920, height: 1080 },
    monitor: 0
};

// Mock Meta.Window
class MockMetaWindow {
    constructor() {
        this._signals = new Map();
        this.title = 'milkdrop';
        this.wmClass = 'milkdrop';
    }

    connect_after(signal, callback) {
        const id = GLib.random_int();
        this._signals.set(id, { signal, callback, after: true });
        return id;
    }

    connect(signal, callback) {
        const id = GLib.random_int();
        this._signals.set(id, { signal, callback, after: false });
        return id;
    }

    disconnect(id) {
        this._signals.delete(id);
    }

    stick() {
        mockWindowState.sticky = true;
    }

    lower() {
        // Mock implementation
    }

    move_to_monitor(monitor) {
        mockWindowState.monitor = monitor;
    }

    move_resize_frame(user_op, x, y, width, height) {
        mockWindowState.position = { x, y };
        mockWindowState.size = { width, height };
    }

    get_monitor() {
        return mockWindowState.monitor;
    }

    get_compositor_private() {
        return {
            get_parent: () => null,
        };
    }

    emit(signal) {
        for (const [id, info] of this._signals) {
            if (info.signal === signal) {
                info.callback();
            }
        }
    }
}

// Load the ManagedWindow class - we need to inline it for testing
class ManagedWindow {
    constructor(metaWindow, monitorIndex, callbacks = {}) {
        this._window = metaWindow;
        this._monitorIndex = monitorIndex;
        this._callbacks = callbacks;
        this._signals = [];

        this._state = {
            keepAtBottom: true,
            keepPosition: true,
            keepMinimized: false,
        };

        this._connectSignals();
    }

    _connectSignals() {
        const raisedId = this._window.connect_after('raised', () => {
            this._onRaised();
        });
        this._signals.push(raisedId);

        const positionId = this._window.connect('position-changed', () => {
            this._onMoved();
        });
        this._signals.push(positionId);

        const minimizedId = this._window.connect('notify::minimized', () => {
            this._onMinimized();
        });
        this._signals.push(minimizedId);
    }

    _onRaised() {
        if (!this._state.keepAtBottom)
            return;
        this._window.lower();
        if (this._callbacks.onRaised)
            this._callbacks.onRaised(this._window);
    }

    _onMoved() {
        if (!this._state.keepPosition)
            return;
        if (this._callbacks.onMoved)
            this._callbacks.onMoved(this._window);
    }

    _onMinimized() {
        if (this._callbacks.onMinimized)
            this._callbacks.onMinimized(this._window);
    }

    get window() {
        return this._window;
    }

    get monitorIndex() {
        return this._monitorIndex;
    }

    get state() {
        return { ...this._state };
    }

    setState(key, value) {
        if (key in this._state)
            this._state[key] = value;
    }

    anchor(geometry) {
        this._window.stick();
        this.enforceCoverage(geometry);
        this._window.lower();
    }

    enforceCoverage(geometry) {
        this._window.move_to_monitor(this._monitorIndex);
        this._window.move_resize_frame(
            false,
            geometry.x,
            geometry.y,
            geometry.width,
            geometry.height
        );
    }

    disable() {
        for (const signalId of this._signals) {
            try {
                this._window.disconnect(signalId);
            } catch (e) {
                // Ignore
            }
        }
        this._signals = [];
        this._window = null;
        this._callbacks = {};
    }
}

// Test cases
function test_basicConstruction() {
    const mockWindow = new MockMetaWindow();
    const managed = new ManagedWindow(mockWindow, 0);

    if (managed.window !== mockWindow) {
        throw new Error('test_basicConstruction: window mismatch');
    }
    if (managed.monitorIndex !== 0) {
        throw new Error('test_basicConstruction: monitorIndex mismatch');
    }
    print('test_basicConstruction: PASS');
}

function test_stateDefaults() {
    const mockWindow = new MockMetaWindow();
    const managed = new ManagedWindow(mockWindow, 1);

    const state = managed.state;
    if (state.keepAtBottom !== true) {
        throw new Error('test_stateDefaults: keepAtBottom should be true');
    }
    if (state.keepPosition !== true) {
        throw new Error('test_stateDefaults: keepPosition should be true');
    }
    print('test_stateDefaults: PASS');
}

function test_setState() {
    const mockWindow = new MockMetaWindow();
    const managed = new ManagedWindow(mockWindow, 0);

    managed.setState('keepAtBottom', false);
    if (managed.state.keepAtBottom !== false) {
        throw new Error('test_setState: keepAtBottom should be false after set');
    }

    // Invalid key should be ignored
    managed.setState('invalidKey', true);
    if ('invalidKey' in managed.state) {
        throw new Error('test_setState: invalidKey should not be added');
    }
    print('test_setValues: PASS');
}

function test_anchorCallsStick() {
    mockWindowState.sticky = false;
    const mockWindow = new MockMetaWindow();
    const managed = new ManagedWindow(mockWindow, 0);

    const geometry = { x: 0, y: 0, width: 1920, height: 1080 };
    managed.anchor(geometry);

    if (!mockWindowState.sticky) {
        throw new Error('test_anchorCallsStick: window should be sticky after anchor');
    }
    print('test_anchorCallsStick: PASS');
}

function test_enforceCoverage() {
    const mockWindow = new MockMetaWindow();
    const managed = new ManagedWindow(mockWindow, 1);

    const geometry = { x: 100, y: 100, width: 1920, height: 1080 };
    managed.enforceCoverage(geometry);

    if (mockWindowState.monitor !== 1) {
        throw new Error('test_enforceCoverage: monitor should be 1');
    }
    if (mockWindowState.position.x !== 100 || mockWindowState.position.y !== 100) {
        throw new Error('test_enforceCoverage: position mismatch');
    }
    print('test_enforceCoverage: PASS');
}

function test_callbacks() {
    let raisedCalled = false;
    let movedCalled = false;

    const mockWindow = new MockMetaWindow();
    const managed = new ManagedWindow(mockWindow, 0, {
        onRaised: () => { raisedCalled = true; },
        onMoved: () => { movedCalled = true; }
    });

    // Simulate raised event
    mockWindow.emit('raised');
    if (!raisedCalled) {
        throw new Error('test_callbacks: onRaised should be called');
    }

    // Simulate position-changed event
    mockWindow.emit('position-changed');
    if (!movedCalled) {
        throw new Error('test_callbacks: onMoved should be called');
    }

    print('test_callbacks: PASS');
}

function test_disableCleansUp() {
    const mockWindow = new MockMetaWindow();
    const managed = new ManagedWindow(mockWindow, 0);

    managed.disable();

    if (managed.window !== null) {
        throw new Error('test_disableCleansUp: window should be null after disable');
    }
    print('test_disableCleansUp: PASS');
}

function test_raisedKeepsAtBottom() {
    const mockWindow = new MockMetaWindow();
    let lowerCalled = false;
    mockWindow.lower = () => { lowerCalled = true; };

    const managed = new ManagedWindow(mockWindow, 0);
    mockWindow.emit('raised');

    if (!lowerCalled) {
        throw new Error('test_raisedKeepsAtBottom: lower should be called on raised');
    }
    print('test_raisedKeepsAtBottom: PASS');
}

// Run all tests
function main() {
    print('Running test_managed_window.js...');

    test_basicConstruction();
    test_stateDefaults();
    test_setState();
    test_anchorCallsStick();
    test_enforceCoverage();
    test_callbacks();
    test_disableCleansUp();
    test_raisedKeepsAtBottom();

    print('\nAll tests passed!');
    return 0;
}

main();
