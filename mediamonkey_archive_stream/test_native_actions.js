// Host-independent contracts for MediaMonkey 5 native Tools actions and
// native in-process queue handoff. Real player UI needs separate acceptance.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

const code = fs.readFileSync(path.join(__dirname, 'native_addon', 'actions_add.js'), 'utf8');
const tempFile = 'C:\\Temp\\asp-unique.tmp';
const readyFile = tempFile + '.archive-stream-ready.json';
const ackFile = readyFile + '.ack';
const playlistFile = 'C:\\Temp\\ArchiveBridge-unique\\archive-stream.m3u8';
const secret = '0123456789abcdef0123456789abcdef';
const validPlaylist = '#EXTM3U\r\n#EXTINF:-1,Custom Archive Title 1\r\n' +
    'http://127.0.0.1:12345/' + secret + '/entry/0.mp3\r\n' +
    '#EXTINF:199,Custom Archive Title 2\r\n' +
    'http://127.0.0.1:12345/' + secret + '/entry/1.ogg\r\n';

async function exercise({choice = 'C:\\Audio\\album.zip', install = 'user',
                         available = true, exitCode = 42, playlist = validPlaylist,
                         manifest = {ok: true, playlist: playlistFile, entries: 2},
                         queueFailure = false, settings = false} = {}) {
    const calls = [];
    const messages = [];
    const files = {[playlistFile]: playlist};
    const actions = {};
    const menu = [];
    const toolbox = {
        actions,
        URL,
        setTimeout,
        window: {
            _menuItems: {tools: {action: {submenu: menu}}},
            localPromise: value => Promise.resolve(value)
        },
        app: {
            filesystem: {
                getScriptsPath: () => 'C:\\Users\\Test\\AppData\\Roaming\\MediaMonkey5\\Scripts\\',
                // Real MM2024 SDK reports the folder, not the .exe.
                getApplicationPath: () => 'C:\\MediaMonkey5\\',
                fileExistsAsync: async candidate => {
                    calls.push(['exists', candidate]);
                    if (candidate === readyFile)
                        return Object.hasOwn(files, readyFile);
                    return available && (install === 'user' ||
                        !candidate.startsWith('C:\\Users\\Test\\AppData\\Roaming\\MediaMonkey5\\Scripts\\'));
                },
                loadTextFromFileAsync: async candidate => {
                    calls.push(['read', candidate]);
                    if (!(candidate in files))
                        throw new Error('Absent handoff file');
                    return files[candidate];
                },
                saveTextToFileAsync: async (candidate, value) => {
                    files[candidate] = value;
                    calls.push(['save', candidate]);
                },
                deleteFileAsync: async candidate => {
                    delete files[candidate];
                    calls.push(['delete', candidate]);
                }
            },
            utils: {
                dialogOpenFile: (...args) => {
                    calls.push(['picker', args]);
                    return Promise.resolve(choice);
                },
                getTempFilePathAsync: async () => tempFile,
                shellExecute: (exe, args) => {
                    calls.push(['shell', exe, args]);
                    if (exitCode > 32 && args.includes('launch-mediamonkey.ps1'))
                        files[readyFile] = JSON.stringify(manifest);
                    return exitCode;
                },
                createTracklist: loaded => ({
                    tracks: [],
                    add(track) { this.tracks.push(track); },
                    loaded
                }),
                createEmptyTrack: () => ({})
            },
            player: {
                addTracksAsync: async (list, options) => {
                    calls.push(['queue', list, options]);
                    if (queueFailure)
                        throw new Error('Host rejected queue insertion');
                }
            }
        },
        uitools: {toastMessage: {show: (message, options) => messages.push({message, options})}}
    };
    vm.runInNewContext(code, toolbox, {filename: 'actions_add.js'});
    assert.equal(menu.length, 2);
    assert.equal(menu[0].action, actions.archiveStreamNativeAdd);
    assert.equal(menu[1].action, actions.archiveStreamNativeSettings);
    if (settings)
        await actions.archiveStreamNativeSettings.execute();
    else
        await actions.archiveStreamNativeAdd.execute();
    return {calls, files, messages, menu};
}

(async () => {
    const normal = await exercise();
    const picker = normal.calls.find(x => x[0] === 'picker');
    assert.equal(picker[1][4], false);
    const shell = normal.calls.find(x => x[0] === 'shell');
    assert.equal(shell[1], 'powershell.exe');
    assert(shell[2].includes('-Archive "C:\\Audio\\album.zip"'));
    assert(shell[2].includes('-PlayerExe "C:\\MediaMonkey5\\MediaMonkey.exe"'));
    assert(shell[2].includes('-ReadyFile "C:\\Temp\\asp-unique.tmp.archive-stream-ready.json"'));
    assert(!shell[2].includes('/Add'), 'Do not spawn second MediaMonkey exe');
    const queued = normal.calls.find(x => x[0] === 'queue');
    assert(queued && queued[1].tracks.length === 2, 'Two archive URLs inserted through native API');
    assert.equal(queued[1].tracks[0].title, 'Custom Archive Title 1');
    assert.equal(queued[1].tracks[1].title, 'Custom Archive Title 2');
    assert.equal(queued[1].tracks[1].songLength, 199000, 'Bridge duration is supplied to native player');
    assert.equal(queued[1].tracks[0].songLength, undefined, 'Unknown duration remains unset');
    assert(queued[1].tracks[1].path.includes('/entry/1.ogg'));
    assert.equal(queued[2].withClear, false);
    assert.equal(queued[2].startPlayback, false);
    assert.equal(normal.files[ackFile], 'QUEUED=2');
    assert(!Object.hasOwn(normal.files, readyFile), 'Delete manifest after queue handoff');
    assert(normal.messages.some(x => x.message.includes('Added 2 archive tracks')));
    console.log('MEDIAMONKEY_NATIVE_ADDON_IN_PROCESS_QUEUE_AND_TITLES_PASS');

    const portable = await exercise({install: 'portable'});
    assert(portable.calls.some(x => x[0] === 'queue'), 'Application-local addon dispatches native queue');
    assert(portable.calls.some(x => x[0] === 'exists' &&
        x[1] === 'C:\\MediaMonkey5\\Scripts\\archiveStreamPlayerNative\\launch-mediamonkey.ps1'));
    console.log('MEDIAMONKEY_NATIVE_ADDON_PORTABLE_PATH_FALLBACK_PASS');

    const canceled = await exercise({choice: ''});
    assert(!canceled.calls.some(x => x[0] === 'shell'));
    const invalid = await exercise({choice: 'C:\\Album\\bad"name.zip'});
    assert(!invalid.calls.some(x => x[0] === 'shell'));
    const missing = await exercise({available: false});
    assert(!missing.calls.some(x => x[0] === 'shell'));
    const processFailure = await exercise({exitCode: 2});
    assert(!processFailure.calls.some(x => x[0] === 'queue'));
    console.log('MEDIAMONKEY_NATIVE_ADDON_CANCEL_MALFORMED_MISSING_AND_LAUNCH_FAILURE_PASS');

    const remote = await exercise({playlist: validPlaylist.replace('127.0.0.1:12345', 'example.net')});
    assert(!remote.calls.some(x => x[0] === 'queue'), 'Reject nonloopback playlist URLs');
    assert(!Object.hasOwn(remote.files, ackFile), 'No bridge acknowledgement after rejection');
    const mismatched = await exercise({manifest: {ok: true, playlist: playlistFile, entries: 3}});
    assert(!mismatched.calls.some(x => x[0] === 'queue'), 'Reject playlist mismatch');
    const rejected = await exercise({queueFailure: true});
    assert(!Object.hasOwn(rejected.files, ackFile), 'No acknowledgement on player rejection');
    assert(rejected.messages.some(x => x.message.includes('Host rejected')));
    console.log('MEDIAMONKEY_NATIVE_ADDON_UNTRUSTED_PLAYLIST_AND_QUEUE_FAILURE_PASS');

    const settings = await exercise({settings: true});
    assert.equal(settings.calls.filter(x => x[0] === 'shell').length, 1);
    assert(settings.calls.find(x => x[0] === 'shell')[2].includes('configure-adapters.ps1'));
    assert(settings.calls.find(x => x[0] === 'shell')[2].includes('-ShowSettings'));
    assert(!settings.calls.some(x => x[0] === 'queue'));
    console.log('MEDIAMONKEY_NATIVE_ADDON_SHARED_SETTINGS_PASS');
})().catch(e => { console.error(e); process.exitCode = 1; });
