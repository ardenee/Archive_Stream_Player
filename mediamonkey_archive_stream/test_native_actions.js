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
                         queueFailure = false, settings = false, refreshTitle = false} = {}) {
    const calls = [];
    const messages = [];
    const files = {[playlistFile]: playlist};
    const actions = {};
    const menu = [];
    const hostTracks = [];
    const timerCallbacks = [];
    const toolbox = {
        actions,
        URL,
        setTimeout,
        setInterval: callback => { timerCallbacks.push(callback); return timerCallbacks.length; },
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
                getSongList: () => ({
                    getTracklist: () => ({
                        whenLoaded: async () => {},
                        forEach: callback => hostTracks.forEach(callback),
                        notifyChanged: () => calls.push(['listChanged'])
                    })
                }),
                addTracksAsync: async (list, options) => {
                    calls.push(['queue', list, options]);
                    if (queueFailure)
                        throw new Error('Host rejected queue insertion');
                    for (const track of list.tracks) {
                        track.notifyChanged = () => calls.push(['trackChanged', track.path]);
                        hostTracks.push(track);
                    }
                }
            }
        },
        uitools: {toastMessage: {show: (message, options) => messages.push({message, options})}}
    };
    vm.runInNewContext(code, toolbox, {filename: 'actions_add.js'});
    assert.equal(menu.length, 4);
    assert.equal(menu[0].action, actions.archiveStreamNativeAdd);
    assert.equal(menu[1].action, actions.archiveStreamNativeAddHttp);
    assert.equal(menu[2].action, actions.archiveStreamNativeSettings);
    assert.equal(menu[3].action, actions.archiveStreamNativeRefreshTitles);
    if (settings)
        await actions.archiveStreamNativeSettings.execute();
    else
        await actions.archiveStreamNativeAdd.execute();
    if (refreshTitle) {
        files[playlistFile] = files[playlistFile].replace('Custom Archive Title 1', 'Updated Archive Title 1');
        await actions.archiveStreamNativeRefreshTitles.execute();
    }
    return {calls, files, messages, menu, actions, hostTracks, timerCallbacks, player: toolbox.app.player};
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

    const nativeRoot = 'C:\\Temp\\ArchiveStreamMM5Native-abc123';
    const nativeList = '#EXTM3U\r\n#EXTINF:-1,Native One\r\n' + nativeRoot + '\\entry-0.ast\r\n' +
        '#EXTINF:-1,Native Two\r\n' + nativeRoot + '\\entry-1.ast\r\n';
    const nativeManifest = {ok: true, playlist: playlistFile, entries: 2, mode: 'native', nativeRoot};
    const native = await exercise({playlist: nativeList, manifest: nativeManifest});
    const nativeQueue = native.calls.find(x => x[0] === 'queue');
    assert(nativeQueue && nativeQueue[1].tracks.length === 2);
    assert.equal(nativeQueue[1].tracks[0].path, nativeRoot + '\\entry-0.ast');
    assert.equal(nativeQueue[1].tracks[1].title, 'Native Two');
    assert.equal(native.files[ackFile], 'QUEUED=2');
    assert(native.messages.some(x => x.message.includes('native decoder, no HTTP')));
    // A real encrypted RAR5 AAC source can be 251785ms long after the decoder
    // opens, yet its temporary Now Playing track still reports -1. Test that
    // only the addon's exact validated .ast references are synchronized.
    native.player.trackLengthMS = 251785;
    native.player.getCurrentTrack = () => native.hostTracks[0];
    native.hostTracks[0].songLength = -1;
    native.timerCallbacks[0]();
    assert.equal(native.hostTracks[0].songLength, 251785);
    assert.equal(native.calls.filter(x => x[0] === 'trackChanged').length, 1);
    native.timerCallbacks[0]();
    assert.equal(native.calls.filter(x => x[0] === 'trackChanged').length, 1,
        'Duration sync must be idempotent after an accepted value');
    native.player.trackLengthMS = 62000;
    native.player.getCurrentTrack = () => native.hostTracks[1];
    native.hostTracks[1].songLength = -1;
    native.timerCallbacks[0]();
    assert.equal(native.hostTracks[1].songLength, 62000, 'Next native track also gets playback duration');
    native.player.trackLengthMS = 85000;
    const unrelated = {path: 'C:\\Other\\entry-9.ast', songLength: -1,
        notifyChanged: () => native.calls.push(['foreignTrackChanged'])};
    native.player.getCurrentTrack = () => unrelated;
    native.timerCallbacks[0]();
    assert.equal(unrelated.songLength, -1, 'Foreign .ast track must not be changed');
    assert(!native.calls.some(x => x[0] === 'foreignTrackChanged'));
    native.player.getCurrentTrack = () => native.hostTracks[1];
    native.player.trackLengthMS = 0;
    native.hostTracks[1].songLength = -1;
    native.timerCallbacks[0]();
    assert.equal(native.hostTracks[1].songLength, -1, 'Unknown player duration must not be injected');
    native.player.trackLengthMS = 87000;
    native.hostTracks[1].songLength = 99000;
    native.timerCallbacks[0]();
    assert.equal(native.hostTracks[1].songLength, 99000, 'Known playlist duration must be preserved');
    normal.player.trackLengthMS = 251785;
    normal.player.getCurrentTrack = () => normal.hostTracks[0];
    normal.timerCallbacks[0]();
    assert.equal(normal.hostTracks[0].songLength, undefined, 'Never sync HTTP bridge or unrelated tracks');
    console.log('MEDIAMONKEY_NATIVE_ONLY_VERIFIED_PLAYBACK_DURATION_SEEKBAR_PASS');
    const forged = await exercise({playlist: nativeList.replace('entry-1.ast', '..\\foreign.ast'), manifest: nativeManifest});
    assert(!forged.calls.some(x => x[0] === 'queue'), 'Reject escaped native manifest paths');
    const foreignRoot = await exercise({playlist: nativeList, manifest: {...nativeManifest, nativeRoot: 'C:\\Elsewhere'}});
    assert(!foreignRoot.calls.some(x => x[0] === 'queue'), 'Reject paths outside native session directory');
    console.log('MEDIAMONKEY_NATIVE_ADDON_LOCAL_MANIFEST_SCOPE_AND_QUEUE_PASS');

    const singlePlaylist = validPlaylist.split('#EXTINF:199,')[0];
    const refreshed = await exercise({playlist: singlePlaylist,
        manifest: {ok: true, playlist: playlistFile, entries: 1}, refreshTitle: true});
    assert.equal(refreshed.timerCallbacks.length, 1, 'Only one background title refresh poll');
    assert.equal(refreshed.hostTracks[0].title, 'Updated Archive Title 1');
    assert.equal(refreshed.hostTracks.length, 1);
    assert.equal(refreshed.calls.filter(x => x[0] === 'trackChanged').length, 1);
    assert.equal(refreshed.calls.filter(x => x[0] === 'listChanged').length, 1);
    assert(refreshed.messages.some(x => x.message.includes('Refreshed 1 queued archive titles')));
    const multi = await exercise({refreshTitle: true});
    assert.equal(multi.hostTracks[0].title, 'Custom Archive Title 1',
        'Multi-entry title changes must not disturb playable queued tracks');
    assert(!multi.calls.some(x => x[0] === 'trackChanged'));
    assert(!multi.calls.some(x => x[0] === 'listChanged'));
    const foreign = await exercise({playlist: singlePlaylist,
        manifest: {ok: true, playlist: playlistFile, entries: 1}});
    foreign.files[playlistFile] = singlePlaylist.replace('127.0.0.1:12345', 'example.net')
        .replace('Custom Archive Title 1', 'Malicious Foreign Title');
    await foreign.actions.archiveStreamNativeRefreshTitles.execute();
    assert.equal(foreign.hostTracks[0].title, 'Custom Archive Title 1');
    assert(!foreign.calls.some(x => x[0] === 'trackChanged'), 'Never update from foreign playlist URLs');
    console.log('MEDIAMONKEY_NATIVE_ADDON_LIVE_TITLE_REFRESH_AND_URL_SCOPE_PASS');

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
