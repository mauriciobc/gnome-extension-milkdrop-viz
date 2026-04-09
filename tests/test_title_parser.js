#!/usr/bin/env gjs
/**
 * Test suite for title parser functionality
 * Tests the _parseRendererTitle function from extension.js
 */

const GLib = imports.gi.GLib;
const Gio = imports.gi.Gio;

// Simple mock log function for testing
let logMessages = [];
function log(msg) {
    logMessages.push(msg);
}

/**
 * Parse renderer title - mimics extension.js implementation
 * @param {string} title - Window title to parse
 * @returns {object|null} Parsed state or null
 */
function parseRendererTitle(title) {
    if (!title || !title.startsWith('@milkdrop!'))
        return null;

    const parts = title.replace('@milkdrop!', '').split('|');
    const jsonPart = parts[0];
    const monitorIndex = parts[1] ? parseInt(parts[1], 10) : null;

    try {
        const state = JSON.parse(jsonPart);
        if (monitorIndex !== null && !isNaN(monitorIndex))
            state.monitorIndex = monitorIndex;
        log(`[milkdrop] Parsed renderer state: monitor=${state.monitor}, overlay=${state.overlay}, opacity=${state.opacity}`);
        return state;
    } catch (e) {
        log(`[milkdrop] Failed to parse title: ${e}`);
        return null;
    }
}

// Test cases
function test_validTitle() {
    const title = '@milkdrop!{"monitor":0,"overlay":false,"opacity":1.00}|0';
    const result = parseRendererTitle(title);

    if (!result) {
        throw new Error('test_validTitle: Expected valid result, got null');
    }
    if (result.monitor !== 0) {
        throw new Error(`test_validTitle: Expected monitor=0, got ${result.monitor}`);
    }
    if (result.overlay !== false) {
        throw new Error(`test_validTitle: Expected overlay=false, got ${result.overlay}`);
    }
    if (result.opacity !== 1.0) {
        throw new Error(`test_validTitle: Expected opacity=1.0, got ${result.opacity}`);
    }
    if (result.monitorIndex !== 0) {
        throw new Error(`test_validTitle: Expected monitorIndex=0, got ${result.monitorIndex}`);
    }
    print('test_validTitle: PASS');
}

function test_titleWithDifferentMonitor() {
    const title = '@milkdrop!{"monitor":1,"overlay":true,"opacity":0.50}|2';
    const result = parseRendererTitle(title);

    if (!result) {
        throw new Error('test_titleWithDifferentMonitor: Expected valid result');
    }
    if (result.monitor !== 1) {
        throw new Error(`test_titleWithDifferentMonitor: Expected monitor=1, got ${result.monitor}`);
    }
    if (result.monitorIndex !== 2) {
        throw new Error(`test_titleWithDifferentMonitor: Expected monitorIndex=2, got ${result.monitorIndex}`);
    }
    print('test_titleWithDifferentMonitor: PASS');
}

function test_invalidPrefix() {
    const result = parseRendererTitle('milkdrop');
    if (result !== null) {
        throw new Error('test_invalidPrefix: Expected null for plain title');
    }
    print('test_invalidPrefix: PASS');
}

function test_nullTitle() {
    const result = parseRendererTitle(null);
    if (result !== null) {
        throw new Error('test_nullTitle: Expected null for null title');
    }
    print('test_nullTitle: PASS');
}

function test_emptyTitle() {
    const result = parseRendererTitle('');
    if (result !== null) {
        throw new Error('test_emptyTitle: Expected null for empty title');
    }
    print('test_emptyTitle: PASS');
}

function test_malformedJson() {
    const title = '@milkdrop!{invalid json}|0';
    const result = parseRendererTitle(title);
    if (result !== null) {
        throw new Error('test_malformedJson: Expected null for malformed JSON');
    }
    // Check that error was logged
    const hasErrorLog = logMessages.some(m => m.includes('Failed to parse title'));
    if (!hasErrorLog) {
        throw new Error('test_malformedJson: Expected error log message');
    }
    print('test_malformedJson: PASS');
}

function test_noMonitorSuffix() {
    const title = '@milkdrop!{"monitor":0,"overlay":false,"opacity":1.00}';
    const result = parseRendererTitle(title);
    if (!result) {
        throw new Error('test_noMonitorSuffix: Expected valid result');
    }
    if (result.monitorIndex !== undefined) {
        throw new Error('test_noMonitorSuffix: Expected no monitorIndex when no suffix');
    }
    print('test_noMonitorSuffix: PASS');
}

// Run all tests
function main() {
    print('Running test_title_parser.js...');

    test_validTitle();
    test_titleWithDifferentMonitor();
    test_invalidPrefix();
    test_nullTitle();
    test_emptyTitle();
    test_malformedJson();
    test_noMonitorSuffix();

    print('\nAll tests passed!');
    return 0;
}

main();
