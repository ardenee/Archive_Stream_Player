// MediaMonkey 5/2024 native Tools addon. Audio decoding stays in the shared
// statically linked archive bridge; native JS queues its authenticated URLs.
(function () {
    'use strict';

    const folderName = 'archiveStreamPlayerNative';
    const title = 'Archive Stream Player';

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

    function archiveTrackList(text, expectedCount) {
        const rows = text.split(/\r?\n/);
        const list = app.utils.createTracklist(true);
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
            let url;
            try { url = new URL(row); }
            catch (error) { throw new Error('Invalid archive bridge playlist URL.'); }
            if (url.protocol !== 'http:' || url.hostname !== '127.0.0.1' ||
                !/^\/[0-9a-f]{32}\/entry\/[0-9]+\.[a-z0-9]+$/i.test(url.pathname))
                throw new Error('Archive bridge playlist has an untrusted URL.');
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
        return {list, count};
    }

    async function addArchive(archive) {
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
                ' -ReadyFile ' + quote(ready));
            const manifest = await awaitReady(ready);
            if (!manifest || !manifest.ok || !manifest.playlist ||
                !Number.isInteger(manifest.entries) || manifest.entries < 1)
                throw new Error('Archive Stream could not open the archive or prepare media.');
            const playlist = await app.filesystem.loadTextFromFileAsync(manifest.playlist);
            const tracks = archiveTrackList(playlist, manifest.entries);
            // Native MediaMonkey API, not a second MediaMonkey.exe /Add process.
            // Keep existing Now Playing entries and do not interrupt playback.
            await app.player.addTracksAsync(tracks.list, {withClear: false, startPlayback: false});
            // The launcher terminates its bridge unless the player acknowledged
            // successful queue insertion. This avoids orphaned bridge sessions.
            await app.filesystem.saveTextToFileAsync(acknowledge, 'QUEUED=' + tracks.count);
            notice('Added ' + tracks.count + ' archive tracks to Now Playing.');
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
                const archive = await window.localPromise(app.utils.dialogOpenFile('',
                    'zip;7z;rar;001', 'Archives (*.zip;*.7z;*.rar;*.001)|*.zip;*.7z;*.rar;*.001',
                    'Choose audio archive', false));
                if (!archive)
                    return;
                await addArchive(quote(archive).slice(1, -1));
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
                await spawn('configure-adapters.ps1', '-ShowSettings');
                notice('Opening shared settings.');
            }
            catch (error) {
                notice(error && error.message ? error.message : 'Could not open shared settings.');
            }
        }
    };

    window._menuItems.tools.action.submenu.push({
        action: actions.archiveStreamNativeAdd,
        order: 10,
        grouporder: 91
    });
    window._menuItems.tools.action.submenu.push({
        action: actions.archiveStreamNativeSettings,
        order: 20,
        grouporder: 91
    });
}());
