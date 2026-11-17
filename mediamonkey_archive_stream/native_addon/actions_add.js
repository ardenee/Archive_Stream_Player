// MediaMonkey 5/2024 native Tools addon. Audio decoding stays in the shared
// statically linked archive bridge; native JS queues its authenticated URLs.
(function () {
    'use strict';

    const folderName = 'archiveStreamPlayerNative';
    const title = 'Archive Stream Player';
    // A session stays active only while its authenticated bridge playlist is
    // accessible. The map belongs to this host surface, not the user library.
    const queuedArchiveSessions = new Map();
    // Only paths that this addon validated and queued may receive transient
    // playback-duration metadata. No persistent library tracks or tags.
    const queuedNativePaths = new Set();
    const synchronizedNativeLengths = new Map();
    const nativeDurationSessions = new Map();
    const attemptedDroppedArchives = new Set();
    let updatingArchiveDurations = false;
    let expandingDroppedArchive = false;
    let activeNativeTrackPath = '';
    let lastNativePlaybackPosition = -1;
    let activeNativeProgressTicks = 0;
    let refreshingTitles = false;

    function notice(message) {
        uitools.toastMessage.show(title + ': ' + message, { disableUndo: true });
    }

    function applicationExe() {
        const location = app.filesystem.getApplicationPath();
        if (!location)
            return '';
        return /\.exe$/i.test(location) ? location :
            location.replace(/[\\/]+$/, '') + '\\MediaMonkey.exe';
    }

    async function addonFile(name) {
        // Installed addons can use the user-profile Scripts folder or the
        // application-local Scripts folder used by portable installations.
        const roots = [];
        const userScripts = app.filesystem.getScriptsPath();
        if (userScripts)
            roots.push(userScripts);
        const executable = applicationExe();
        if (executable)
            roots.push(executable.replace(/[\\/][^\\/]+$/, '') + '\\Scripts');

        for (const root of roots) {
            const folder = root.replace(/[\\/]+$/, '') + '\\' + folderName + '\\';
            const file = folder + name;
            if ((await app.filesystem.fileExistsAsync(file)) &&
                (await app.filesystem.fileExistsAsync(folder + 'archive_player_bridge.exe')))
                return file;
        }
        throw new Error('Archive Stream bridge or launcher is missing from the installed addon.');
    }

    function quote(value) {
        if (!value || value.includes('"') || /[\r\n]/.test(value))
            throw new Error('Invalid archive or launcher path.');
        return '"' + value + '"';
    }

    async function spawn(script, argumentsList) {
        const file = await addonFile(script);
        const command = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ' +
            quote(file) + ' ' + argumentsList;
        const result = app.utils.shellExecute('powershell.exe', command);
        if (typeof result === 'number' && result <= 32)
            throw new Error('Windows could not start the Archive Stream launcher (code ' + result + ').');
    }

    async function awaitReady(path) {
        // The PowerShell launcher publishes a tiny manifest atomically once the
        // bridge has initialized. No shell process, archive bytes or tokens are
        // read through a network service from this JavaScript addon.
        for (let attempt = 0; attempt < 120; ++attempt) {
            if (await app.filesystem.fileExistsAsync(path))
                return JSON.parse(await app.filesystem.loadTextFromFileAsync(path));
            await new Promise(resolve => setTimeout(resolve, 250));
        }
        throw new Error('Timed out waiting for archive decoding to initialize.');
    }

    function archiveTrackList(text, expectedCount, mode, nativeRoot) {
        const rows = text.split(/\r?\n/);
        const list = app.utils.createTracklist(true);
        const useNative = mode === 'native';
        const nativePaths = [];
        if (mode !== 'native' && mode !== 'http')
            throw new Error('Unsupported archive playback handoff.');
        const nativePrefix = useNative && typeof nativeRoot === 'string' &&
            /^[a-z]:[\\/]/i.test(nativeRoot) ?
            nativeRoot.replace(/[\\/]+$/, '').replace(/\//g, '\\').toLowerCase() + '\\' : '';
        if (useNative && !nativePrefix)
            throw new Error('Native archive references have no validated session directory.');
        let label = '';
        let durationSeconds = -1;
        let count = 0;
        for (const raw of rows) {
            const row = raw.trim();
            if (row.startsWith('#EXTINF:')) {
                const comma = row.indexOf(',');
                label = comma >= 0 ? row.slice(comma + 1).trim() : '';
                const duration = Number(row.slice(8, comma >= 0 ? comma : undefined));
                durationSeconds = Number.isInteger(duration) && duration > 0 && duration <= 86400 ?
                    duration : -1;
                continue;
            }
            if (!row || row.startsWith('#'))
                continue;
            if (useNative) {
                const file = row.replace(/\//g, '\\').toLowerCase();
                if (!file.startsWith(nativePrefix) ||
                    !/^entry-[0-9]+\.ast$/i.test(file.slice(nativePrefix.length)))
                    throw new Error('Native archive playlist contains an unexpected reference path.');
                nativePaths.push(file);
            }
            else {
                let url;
                try { url = new URL(row); }
                catch (error) { throw new Error('Invalid archive bridge playlist URL.'); }
                if (url.protocol !== 'http:' || url.hostname !== '127.0.0.1' ||
                    !/^\/[0-9a-f]{32}\/entry\/[0-9]+\.[a-z0-9]+$/i.test(url.pathname))
                    throw new Error('Archive bridge playlist has an untrusted URL.');
            }
            const track = app.utils.createEmptyTrack();
            track.path = row;
            if (label)
                track.title = label;
            // Shared backend supplies a bounded duration for supported OGG
            // streams. MediaMonkey needs it to seek rather than ending the
            // track when its HTTP Vorbis decoder reports an unknown length.
            if (durationSeconds > 0)
                track.songLength = durationSeconds * 1000;
            list.add(track);
            ++count;
            label = '';
            durationSeconds = -1;
            if (count > 10000)
                throw new Error('Archive playlist exceeds the safe entry limit.');
        }
        if (!count || count !== expectedCount)
            throw new Error('Archive playlist entry count does not match the bridge.');
        return {list, count, nativePaths};
    }

    async function refreshNativeArchiveDurations() {
        if (updatingArchiveDurations || !nativeDurationSessions.size)
            return;
        updatingArchiveDurations = true;
        try {
            const verified = new Map();
            for (const [directory, session] of nativeDurationSessions) {
                let content;
                try {
                    content = await app.filesystem.loadTextFromFileAsync(
                        directory + '\\archive-stream-durations.tsv');
                }
                catch (error) {
                    continue;
                }
                const lines = content.split(/\r?\n/);
                if (lines[0] !== 'ARCHIVE_STREAM_DURATIONS_V1')
                    continue;
                let complete = false;
                for (const line of lines.slice(1)) {
                    if (line === 'DONE') {
                        complete = true;
                        break;
                    }
                    const match = /^([0-9]+)\t([0-9]+)$/.exec(line);
                    if (!match)
                        continue;
                    const index = Number(match[1]);
                    const duration = Number(match[2]);
                    if (!Number.isSafeInteger(index) || index < 0 ||
                        !Number.isSafeInteger(duration) || duration < 1000 ||
                        duration > 86400000)
                        continue;
                    const path = (directory + '\\entry-' + index + '.ast').toLowerCase();
                    if (queuedNativePaths.has(path) && session.get(path) !== duration) {
                        verified.set(path, duration);
                        session.set(path, duration);
                    }
                }
                if (complete)
                    nativeDurationSessions.delete(directory);
            }
            if (!verified.size)
                return;
            const list = app.player.getSongList().getTracklist();
            await list.whenLoaded();
            let changes = 0;
            list.forEach(track => {
                if (!track || typeof track.path !== 'string')
                    return;
                const path = track.path.replace(/\//g, '\\').toLowerCase();
                const duration = verified.get(path);
                if (!duration || (track.songLength > 0 &&
                    !synchronizedNativeLengths.has(path)))
                    return;
                if (track.songLength !== duration) {
                    track.songLength = duration;
                    track.notifyChanged();
                    ++changes;
                }
                synchronizedNativeLengths.set(path, duration);
            });
            if (changes)
                list.notifyChanged();
        }
        finally {
            updatingArchiveDurations = false;
        }
    }

    async function expandDroppedArchives() {
        if (expandingDroppedArchive)
            return;
        const list = app.player.getSongList().getTracklist();
        await list.whenLoaded();
        const found = new Set();
        const candidates = [];
        list.forEach(track => {
            if (!track || typeof track.path !== 'string')
                return;
            const path = track.path;
            if (!/^[a-z]:[\\/].+\.(zip|7z|rar|001)$/i.test(path))
                return;
            found.add(path.toLowerCase());
            if (!attemptedDroppedArchives.has(path.toLowerCase()))
                candidates.push({path, track});
        });
        for (const path of attemptedDroppedArchives) {
            if (!found.has(path))
                attemptedDroppedArchives.delete(path);
        }
        if (!candidates.length)
            return;
        const {path, track} = candidates[0];
        attemptedDroppedArchives.add(path.toLowerCase());
        expandingDroppedArchive = true;
        try {
            // The host's regular drop inserts an unplayable archive placeholder.
            // Expand it with the existing native route, then remove only that
            // specific placeholder after insertion succeeds.
            await addArchive(path);
            const placeholder = app.utils.createTracklist(true);
            placeholder.add(track);
            await app.player.removeSelectedTracksAsync(placeholder);
        }
        catch (error) {
            notice(error && error.message ? error.message : 'Cannot expand dropped archive.');
        }
        finally {
            expandingDroppedArchive = false;
        }
    }

    function playlistTitles(text) {
        const rows = text.split(/\r?\n/);
        const titles = new Map();
        let label = '';
        for (const raw of rows) {
            const row = raw.trim();
            if (row.startsWith('#EXTINF:')) {
                const comma = row.indexOf(',');
                label = comma >= 0 ? row.slice(comma + 1).trim() : '';
            }
            else if (row && !row.startsWith('#')) {
                if (/^http:\/\/127\.0\.0\.1:\d+\/[0-9a-f]{32}\/entry\/\d+\.[a-z0-9]+$/i.test(row) && label)
                    titles.set(row, label);
                label = '';
            }
        }
        return titles;
    }

    function synchronizeNativePlayingDuration() {
        if (!queuedNativePaths.size || typeof app.player.getCurrentTrack !== 'function')
            return false;

        const track = app.player.getCurrentTrack();
        if (!track || typeof track.path !== 'string')
            return false;

        // Paths come only from archiveTrackList()'s validated native session
        // root; never update unrelated music, HTTP URLs or foreign .ast paths.
        const path = track.path.replace(/\//g, '\\').toLowerCase();
        if (!queuedNativePaths.has(path)) {
            activeNativeTrackPath = '';
            lastNativePlaybackPosition = -1;
            activeNativeProgressTicks = 0;
            return false;
        }

        if (path !== activeNativeTrackPath) {
            activeNativeTrackPath = path;
            lastNativePlaybackPosition = -1;
            activeNativeProgressTicks = 0;
        }

        // The player may change CurrentTrack before the new input decoder has
        // opened; during the native password dialog it can still report the
        // previous track's duration and playback position. Require real,
        // steadily advancing playback across three 3-second polls before
        // trusting TrackLengthMS to describe the new archive entry.
        const positionMs = app.player.trackPositionMS;
        if (app.player.isPlaying !== true ||
            !Number.isFinite(positionMs) || positionMs < 0) {
            lastNativePlaybackPosition = -1;
            activeNativeProgressTicks = 0;
            return false;
        }

        if (lastNativePlaybackPosition >= 0 &&
            positionMs > lastNativePlaybackPosition + 200 &&
            positionMs <= lastNativePlaybackPosition + 15000)
            ++activeNativeProgressTicks;
        else
            activeNativeProgressTicks = 0;
        lastNativePlaybackPosition = positionMs;

        if (activeNativeProgressTicks < 2)
            return false;

        // The engine can initially expose the *previous* track's duration on
        // a rapid multi-track switch. Remember only values that we injected;
        // replace those if the decoder subsequently reports a different
        // verified duration, but never overwrite playlist/user metadata.
        const previouslySynchronized = synchronizedNativeLengths.get(path);
        if (track.songLength > 0 &&
            (previouslySynchronized === undefined || track.songLength !== previouslySynchronized))
            return false;

        // The native decoder exposes its verified duration after opening; MM5
        // can know it via the player while the transient queued Track still
        // says -1, leaving the stock GUI seek bar with a negative maximum.
        const durationMs = app.player.trackLengthMS;
        if (!Number.isFinite(durationMs) || durationMs < 1000 ||
            durationMs > 24 * 60 * 60 * 1000)
            return false;

        if (track.songLength === durationMs)
            return false;

        // In-memory notification only: no commitAsync, DB update, or tag writes.
        track.songLength = durationMs;
        synchronizedNativeLengths.set(path, durationMs);
        track.notifyChanged();
        return true;
    }

    async function refreshQueuedArchiveTitles() {
        if (refreshingTitles || !queuedArchiveSessions.size)
            return 0;
        refreshingTitles = true;
        try {
            const updated = new Map();
            for (const [playlist, permittedUrls] of queuedArchiveSessions) {
                // Real host multi-entry title repaint has not passed playback parity;
                // do not mutate any queued album until this case is certified.
                if (permittedUrls.size !== 1)
                    continue;
                try {
                    const current = playlistTitles(await app.filesystem.loadTextFromFileAsync(playlist));
                    for (const [url, label] of current) {
                        if (permittedUrls.has(url))
                            updated.set(url, label);
                    }
                }
                catch (error) {
                    // Bridge is gone: do not attempt to reach its stale URLs.
                    queuedArchiveSessions.delete(playlist);
                }
            }
            if (!updated.size)
                return 0;
            const songs = app.player.getSongList().getTracklist();
            await songs.whenLoaded();
            let changed = 0;
            // Official MM5 SDK: forEach gives safe, transient objects, and
            // notifyChanged repaints in-memory metadata without tag writes.
            songs.forEach(track => {
                const expected = track && updated.get(track.path);
                if (!expected || track.title === expected)
                    return;
                track.title = expected;
                track.notifyChanged();
                ++changed;
            });
            if (changed)
                songs.notifyChanged();
            return changed;
        }
        finally {
            refreshingTitles = false;
        }
    }

    async function addArchive(archive, useHttpBridge = false) {
        const tempFile = await app.utils.getTempFilePathAsync();
        const ready = tempFile + '.archive-stream-ready.json';
        const acknowledge = ready + '.ack';
        const player = applicationExe();
        if (!player)
            throw new Error('MediaMonkey application location is unavailable.');
        try {
            await spawn('launch-mediamonkey.ps1',
                '-Archive ' + quote(archive) +
                ' -PlayerExe ' + quote(player) +
                ' -ReadyFile ' + quote(ready) +
                (useHttpBridge ? ' -UseHttpBridge' : ''));
            const manifest = await awaitReady(ready);
            if (!manifest || !manifest.ok || !manifest.playlist ||
                !Number.isInteger(manifest.entries) || manifest.entries < 1)
                throw new Error('Archive Stream could not open the archive or prepare media.');
            const playlist = await app.filesystem.loadTextFromFileAsync(manifest.playlist);
            const tracks = archiveTrackList(playlist, manifest.entries, manifest.mode || 'http', manifest.nativeRoot);
            // Native MediaMonkey API, not a second MediaMonkey.exe /Add process.
            // Keep existing Now Playing entries and do not interrupt playback.
            await app.player.addTracksAsync(tracks.list, {withClear: false, startPlayback: false});
            // The launcher terminates its bridge unless the player acknowledged
            // successful queue insertion. This avoids orphaned bridge sessions.
            await app.filesystem.saveTextToFileAsync(acknowledge, 'QUEUED=' + tracks.count);
            if (manifest.mode === 'native') {
                for (const path of tracks.nativePaths)
                    queuedNativePaths.add(path);
                if (typeof manifest.nativeRoot === 'string' && manifest.nativeRoot)
                    nativeDurationSessions.set(
                        manifest.nativeRoot.replace(/\//g, '\\').toLowerCase(), new Map());
                while (queuedNativePaths.size > 10000) {
                    const oldPath = queuedNativePaths.values().next().value;
                    queuedNativePaths.delete(oldPath);
                    synchronizedNativeLengths.delete(oldPath);
                }
            }
            else {
                queuedArchiveSessions.set(manifest.playlist, new Set(playlistTitles(playlist).keys()));
            }
            notice('Added ' + tracks.count + ' archive tracks to Now Playing (' +
                (manifest.mode === 'native' ? 'native decoder, no HTTP' : 'HTTP compatibility') + ').');
        }
        finally {
            try { await app.filesystem.deleteFileAsync(ready); } catch (error) {}
        }
    }

    actions.archiveStreamNativeAdd = {
        title: 'Archive Stream: Add Archive Audio...',
        hotkeyAble: true,
        execute: async function () {
            try {
                const archive = await app.utils.dialogOpenFile('',
                    'zip;7z;rar;001', 'Archives (*.zip;*.7z;*.rar;*.001)|*.zip;*.7z;*.rar;*.001',
                    'Choose audio archive', false);
                if (!archive)
                    return;
                await addArchive(quote(archive).slice(1, -1));
            }
            catch (error) {
                notice(error && error.message ? error.message : 'Archive selection or playback failed.');
            }
        }
    };

    actions.archiveStreamNativeAddHttp = {
        title: 'Archive Stream: Add Archive Audio (HTTP compatibility)...',
        hotkeyAble: true,
        execute: async function () {
            try {
                const archive = await app.utils.dialogOpenFile('',
                    'zip;7z;rar;001', 'Archives (*.zip;*.7z;*.rar;*.001)|*.zip;*.7z;*.rar;*.001',
                    'Choose audio archive for HTTP compatibility', false);
                if (!archive)
                    return;
                await addArchive(quote(archive).slice(1, -1), true);
            }
            catch (error) {
                notice(error && error.message ? error.message : 'Archive selection or playback failed.');
            }
        }
    };

    actions.archiveStreamNativeSettings = {
        title: 'Archive Stream: Settings...',
        hotkeyAble: true,
        execute: async function () {
            try {
                await spawn('configure-adapters.ps1', '');
                notice('Opening shared settings.');
            }
            catch (error) {
                notice(error && error.message ? error.message : 'Could not open shared settings.');
            }
        }
    };

    actions.archiveStreamNativeRefreshTitles = {
        title: 'Archive Stream: Refresh Queued Titles',
        hotkeyAble: true,
        execute: async function () {
            try {
                const changed = await refreshQueuedArchiveTitles();
                notice('Refreshed ' + changed + ' queued archive titles.');
            }
            catch (error) {
                notice(error && error.message ? error.message : 'Could not refresh archive titles.');
            }
        }
    };

    // Reload only authenticated playlists belonging to archive sessions
    // queued through this addon. Never touch embedded audio tags or unrelated
    // user tracks. The companion bridge rewrites its M3U8 on template change.
    setInterval(function () {
        try { synchronizeNativePlayingDuration(); } catch (error) {}
        refreshNativeArchiveDurations().catch(function () {});
        expandDroppedArchives().catch(function () {});
        refreshQueuedArchiveTitles().catch(function () {});
    }, 3000);

    window._menuItems.tools.action.submenu.push({
        action: actions.archiveStreamNativeAdd,
        order: 10,
        grouporder: 91
    });
    window._menuItems.tools.action.submenu.push({
        action: actions.archiveStreamNativeAddHttp,
        order: 15,
        grouporder: 91
    });
    window._menuItems.tools.action.submenu.push({
        action: actions.archiveStreamNativeSettings,
        order: 20,
        grouporder: 91
    });
    window._menuItems.tools.action.submenu.push({
        action: actions.archiveStreamNativeRefreshTitles,
        order: 30,
        grouporder: 91
    });
}());
