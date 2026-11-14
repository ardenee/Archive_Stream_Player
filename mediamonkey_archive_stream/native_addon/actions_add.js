// MediaMonkey 5/2024 general addon. Registers archive actions in its Tools menu.
// Uses the single shared archive bridge; does not decode or modify archive files.
(function () {
    'use strict';
    const folderName = 'archiveStreamPlayerNative';
    const title = 'Archive Stream Player';

    function notice(message) {
        uitools.toastMessage.show(title + ': ' + message, { disableUndo: true });
    }

    async function launch(script, extraArguments, confirmation) {
        // Installed addons may live under the per-user Scripts directory,
        // while portable and bundled addons can live beside MediaMonkey.exe.
        const roots = [];
        const userScripts = app.filesystem.getScriptsPath();
        if (userScripts)
            roots.push(userScripts);
        let applicationPath = app.filesystem.getApplicationPath();
        if (applicationPath) {
            if (/\.exe$/i.test(applicationPath))
                applicationPath = applicationPath.replace(/[\\/][^\\/]+$/, '');
            roots.push(applicationPath.replace(/[\\/]+$/, '') + '\\Scripts');
        }
        let launcher = '';
        for (const root of roots) {
            const directory = root.replace(/[\\/]+$/, '') + '\\' + folderName + '\\';
            const scriptPath = directory + script;
            const bridgePath = directory + 'archive_player_bridge.exe';
            if ((await app.filesystem.fileExistsAsync(scriptPath)) &&
                (await app.filesystem.fileExistsAsync(bridgePath))) {
                launcher = scriptPath;
                break;
            }
        }
        if (!launcher) {
            notice('The bridge or launcher is missing from the installed addon.');
            return;
        }

        if (script === 'launch-mediamonkey.ps1') {
            const exe = app.filesystem.getApplicationPath();
            if (exe && exe.indexOf('"') < 0)
                extraArguments += ' -PlayerExe "' + exe + '"';
        }
        const argumentsList = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' +
            launcher + '" ' + extraArguments;
        const result = app.utils.shellExecute('powershell.exe', argumentsList);
        if (result <= 32) {
            notice('Windows could not start the Archive Stream launcher (code ' + result + ').');
            return;
        }
        notice(confirmation);
    }

    actions.archiveStreamNativeAdd = {
        title: 'Archive Stream: Add Archive Audio...',
        hotkeyAble: true,
        execute: async function () {
            try {
                const choice = await window.localPromise(app.utils.dialogOpenFile('',
                    'zip;7z;rar;001', 'Archives (*.zip;*.7z;*.rar;*.001)|*.zip;*.7z;*.rar;*.001',
                    'Choose audio archive', false));
                if (!choice || typeof choice !== 'string')
                    return;
                // Windows file paths cannot contain a quote; reject it explicitly.
                if (choice.indexOf('"') >= 0 || /[\r\n]/.test(choice)) {
                    notice('The selected archive path is invalid.');
                    return;
                }
                await launch('launch-mediamonkey.ps1', '-Archive "' + choice + '"',
                    'Starting archive playback using the shared bridge.');
            }
            catch (error) {
                notice('Archive selection or launch failed.');
            }
        }
    };

    actions.archiveStreamNativeSettings = {
        title: 'Archive Stream: Settings...',
        hotkeyAble: true,
        execute: async function () {
            try {
                await launch('configure-adapters.ps1', '-ShowSettings', 'Opening shared settings.');
            }
            catch (error) {
                notice('Could not open shared settings.');
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
