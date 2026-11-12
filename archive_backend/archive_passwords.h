#pragma once

#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// ArchivePasswordList
//
// Shared password list loaded from keep.txt.
//
// The keep.txt file is deliberately plain and global: one password per line,
// no archive names, no CRCs, no hashes. This matches the intended plugin design:
// try all saved passwords quickly before asking the UI layer for a new one.
// -----------------------------------------------------------------------------
struct ArchivePasswordList
{
    std::vector<std::string> passwords;
};

// -----------------------------------------------------------------------------
// ArchivePasswordPromptCallback
//
// UI-owned password prompt callback.
//
// The backend never shows UI directly. in_archive_stream.dll registers a
// callback that can prompt the user, return a password, and say whether the
// password should be saved to keep.txt.
//
// Return true when the user supplied a password.
// Return false when the user cancelled.
// -----------------------------------------------------------------------------
typedef bool (*ArchivePasswordPromptCallback)(const char* archivePath, const char* entryName, char* outPassword, size_t outPasswordSize, bool* outSavePassword );

// -----------------------------------------------------------------------------
// ArchivePasswordAcceptedCallback
//
// Called after a password has actually proven valid by successfully reading
// encrypted data. This lets the input plugin queue background metadata refresh
// for remaining tracks in the same archive.
// -----------------------------------------------------------------------------
typedef void (*ArchivePasswordAcceptedCallback)(const char* archivePath);

// -----------------------------------------------------------------------------
// ArchivePasswordSetListPath
//
// Overrides the password-list path used by the backend.
//
// By default this is in the selected backend data folder.
// The Winamp adapters select %LOCALAPPDATA%\WinampArchiveStream\keeps.txt.
// Tests/tools may set an explicit path.
// -----------------------------------------------------------------------------
void ArchivePasswordSetListPath(const char* passwordListPath);

// -----------------------------------------------------------------------------
// ArchivePasswordGetListPath
//
// Returns the active keep.txt path, building the default plugin-data path when
// no explicit path has been set.
// -----------------------------------------------------------------------------
const char* ArchivePasswordGetListPath();

// -----------------------------------------------------------------------------
// ArchivePasswordLoadList
//
// Loads keep.txt into memory.
//
// Missing keep.txt is not an error; it simply means there are no saved
// passwords yet. Empty lines are ignored. Duplicate passwords are collapsed.
// -----------------------------------------------------------------------------
bool ArchivePasswordLoadList(ArchivePasswordList& outList, std::string& error);

// -----------------------------------------------------------------------------
// ArchivePasswordAppendIfMissing
//
// Appends a password to keep.txt only if it is not already present.
//
// This will be used by the UI prompt stage after the user enters a working
// password and chooses to save it.
// -----------------------------------------------------------------------------
bool ArchivePasswordAppendIfMissing(const char* password, std::string& error);

// -----------------------------------------------------------------------------
// ArchivePasswordSetPromptCallback
//
// Registers the UI-owned password prompt callback.
// -----------------------------------------------------------------------------
void ArchivePasswordSetPromptCallback(ArchivePasswordPromptCallback callback);

// -----------------------------------------------------------------------------
// ArchivePasswordPromptUser
//
// Calls the registered UI prompt callback, if one exists.
// -----------------------------------------------------------------------------
bool ArchivePasswordPromptUser(const char* archivePath, const char* entryName, char* outPassword, size_t outPasswordSize, bool* outSavePassword);
// -----------------------------------------------------------------------------
// ArchivePasswordAddRuntimePassword
//
// Remembers a password for one archive for the current process only.
//
// This is used after a user enters a working password, so the rest of the files
// in the same archive can play without prompting again. It does not write to
// keeps.txt.
// -----------------------------------------------------------------------------
void ArchivePasswordAddRuntimePassword(const char* archivePath, const char* password);

// -----------------------------------------------------------------------------
// ArchivePasswordGetRuntimePasswords
//
// Returns process-local passwords remembered for this archive.
// -----------------------------------------------------------------------------
void ArchivePasswordGetRuntimePasswords(const char* archivePath, std::vector<std::string>& outPasswords);

// -----------------------------------------------------------------------------
// ArchivePasswordMarkArchiveCancelled
//
// Marks an archive as cancelled for this process, so subsequent encrypted reads
// from the same archive do not keep prompting.
// -----------------------------------------------------------------------------
void ArchivePasswordMarkArchiveCancelled(const char* archivePath);

// -----------------------------------------------------------------------------
// ArchivePasswordIsArchiveCancelled
//
// Returns true when the user cancelled password entry for this archive.
// -----------------------------------------------------------------------------
bool ArchivePasswordIsArchiveCancelled(const char* archivePath);

// -----------------------------------------------------------------------------
// ArchivePasswordClearArchiveCancelled
//
// Clears the cancellation marker after a password succeeds.
// -----------------------------------------------------------------------------
void ArchivePasswordClearArchiveCancelled(const char* archivePath);

// -----------------------------------------------------------------------------
// ArchivePasswordSetAcceptedCallback
//
// Registers a callback fired after a runtime password is proven valid.
// -----------------------------------------------------------------------------
void ArchivePasswordSetAcceptedCallback(ArchivePasswordAcceptedCallback callback);

// -----------------------------------------------------------------------------
// ArchivePasswordNotifyAccepted
//
// Notifies the UI/input layer that an archive password has been accepted.
// -----------------------------------------------------------------------------
void ArchivePasswordNotifyAccepted(const char* archivePath);

// -----------------------------------------------------------------------------
// ArchivePasswordSetPromptAllowedForCurrentThread
//
// Enables/disables UI password prompts for the current thread.
// Metadata/background threads should disable this so they never show password UI.
// -----------------------------------------------------------------------------
void ArchivePasswordSetPromptAllowedForCurrentThread(bool allowed);

// -----------------------------------------------------------------------------
// ArchivePasswordGetPromptAllowedForCurrentThread
//
// Returns whether the current thread may show password UI.
// -----------------------------------------------------------------------------
bool ArchivePasswordGetPromptAllowedForCurrentThread();