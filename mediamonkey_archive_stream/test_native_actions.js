// Host-free MediaMonkey 5 native action API contract tests.
// This checks official action/menu wiring, Windows picker, package paths,
// safe argument composition and shared settings routing; not real host playback.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

const code = fs.readFileSync(path.join(__dirname, 'native_addon', 'actions_add.js'), 'utf8');

async function exercise(choice, available = true, exitCode = 33, installMode = 'user') {
    const calls = [];
    const messages = [];
    const actions = {};
    const submenu = [];
    const sandbox = {
        actions,
        window: {
            _menuItems: { tools: { action: { submenu } } },
            localPromise: value => Promise.resolve(value)
        },
        app: {
            filesystem: {
                getScriptsPath: () => 'C:\\Users\\Test\\AppData\\Roaming\\MediaMonkey5\\Scripts\\',
                getApplicationPath: () => 'C:\\MediaMonkey5\\MediaMonkey.exe',
                fileExistsAsync: async candidate => {
                    calls.push(['exists', candidate]);
                    return available && (installMode === 'user' || !candidate.startsWith('C:\\Users\\Test\\AppData\\Roaming\\MediaMonkey5\\Scripts\\'));
                }
            },
            utils: {
                dialogOpenFile: (...args) => {
                    calls.push(['dialog', args]);
                    return Promise.resolve(choice);
                },
                shellExecute: (file, params) => {
                    calls.push(['shell', file, params]);
                    return exitCode;
                }
            }
        },
        uitools: {
            toastMessage: {
                show: (message, options) => messages.push({message, options})
            }
        }
    };
    vm.runInNewContext(code, sandbox, {filename:'actions_add.js'});
    assert.equal(submenu.length, 2, 'Two Tools menu commands registered');
    assert.equal(submenu[0].action, actions.archiveStreamNativeAdd);
    assert.equal(submenu[1].action, actions.archiveStreamNativeSettings);
    await actions.archiveStreamNativeAdd.execute();
    return {actions, submenu, calls, messages};
}

(async function main() {
    const valid = await exercise('C:\\Audio\\album.zip');
    const picker = valid.calls.find(x => x[0] === 'dialog');
    assert(picker && picker[1][4] === false, 'Single-archive file picker');
    const launched = valid.calls.find(x => x[0] === 'shell');
    assert(launched && launched[1] === 'powershell.exe', 'Native Windows launcher');
    assert(launched[2].includes('launch-mediamonkey.ps1'), 'Addon launcher selected');
    assert(launched[2].includes('-Archive "C:\\Audio\\album.zip"'), 'Exact archive argument');
    assert(launched[2].includes('-PlayerExe "C:\\MediaMonkey5\\MediaMonkey.exe"'), 'Correct MediaMonkey executable');
    assert(!launched[2].includes('gen_archive_stream'), 'Unsupported Winamp general plugin excluded');
    assert(valid.messages.some(x => x.message.includes('Starting archive playback')), 'Nonterminal launch feedback');
    console.log('MEDIAMONKEY_NATIVE_ADDON_ACTION_AND_MENU_PASS');

    const portable = await exercise('D:\\Music\\collection.7z', true, 33, 'portable');
    const portableLaunch = portable.calls.find(x => x[0] === 'shell');
    assert(portableLaunch, 'App-local addon fallback found');
    assert(portableLaunch[2].includes('C:\\MediaMonkey5\\Scripts\\archiveStreamPlayerNative\\launch-mediamonkey.ps1'), 'Local Scripts path');
    console.log('MEDIAMONKEY_NATIVE_ADDON_PORTABLE_PATH_FALLBACK_PASS');

    const invalid = await exercise('C:\\Audio\\track"escape.zip');
    assert(!invalid.calls.some(x => x[0] === 'shell'), 'Invalid path must not launch');
    const cancelled = await exercise('');
    assert(!cancelled.calls.some(x => x[0] === 'shell'), 'Cancelled file picker must not launch');
    const missing = await exercise('C:\\Audio\\album.zip', false);
    assert(!missing.calls.some(x => x[0] === 'shell'), 'Missing installed assets must not launch');
    console.log('MEDIAMONKEY_NATIVE_ADDON_CANCEL_AND_MISSING_ASSETS_PASS');

    const settingsCalls = [];
    const settingsSandbox = {
        actions: {},
        window: {
            _menuItems: { tools: { action: { submenu: [] } } },
            localPromise: value => Promise.resolve(value)
        },
        app: {
            filesystem: {
                getScriptsPath: () => 'C:\\MediaMonkey5\\Scripts',
                getApplicationPath: () => 'C:\\MediaMonkey5\\MediaMonkey.exe',
                fileExistsAsync: async () => true
            },
            utils: {
                dialogOpenFile: () => Promise.resolve(''),
                shellExecute: (exe, args) => {settingsCalls.push([exe, args]); return 33;}
            }
        },
        uitools: {toastMessage: {show: () => {}}}
    };
    vm.runInNewContext(code, settingsSandbox);
    await settingsSandbox.actions.archiveStreamNativeSettings.execute();
    assert.equal(settingsCalls.length, 1, 'Settings must start exactly one process');
    assert(settingsCalls[0][1].includes('configure-adapters.ps1'), 'Existing shared settings reused');
    assert(settingsCalls[0][1].includes('-ShowSettings'), 'Settings opens the GUI');
    console.log('MEDIAMONKEY_NATIVE_ADDON_SHARED_SETTINGS_PASS');
})().catch(err => {console.error(err);process.exitCode=1;});
