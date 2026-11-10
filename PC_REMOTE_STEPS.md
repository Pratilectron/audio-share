# PC Remote feature plan (Monect-like subset)

## Goals by step
1. **Touchpad** — relative move, left/right/middle click, wheel. Pass when a local test moves the real cursor.
2. **Keyboard** — Unicode text and virtual keys. Pass when Notepad receives typed characters.
3. **Files** — list / download / upload inside the Windows user profile only. Pass when a round-trip file matches.
4. **Task view** — CPU%, memory, process list, end process (non-protected). Pass when stats return and a test process can be ended.
5. **Camera** — one JPEG frame from the first camera, or a clear "no camera" error. Pass when capture returns bytes or that error. ✅ PASS (`jpeg=8504`)
6. **Encrypted session** — after auth, control and file payloads are AES-GCM with a key derived from the machine token. Pass when a wrong key cannot decrypt a control command. ✅ PASS (`wrong-key-rejected`)
7. **Phone UI + ship** — Remote tab with touchpad, keyboard, files, tasks, camera. Wire cmds 7–12. Full regression. Push to GitHub.

### Step status
1–7 ✅ shipped (`f09f0d7` on `origin/main`)
- Unit tests 1–6 PASS; Android `:app:assembleDebug` PASS
- Local MSVC lacks MFC; CI `build-fork` produces `AudioShareServer.exe`)

### Network commands (after auth)
| Cmd | Id | Purpose |
|---|---|---|
| cmd_fs_list | 7 | List profile-relative folder |
| cmd_fs_get | 8 | Download file ≤8MB |
| cmd_fs_put | 9 | Upload file into profile |
| cmd_stats | 10 | CPU / memory / processes |
| cmd_end_process | 11 | End non-protected process |
| cmd_camera | 12 | One JPEG frame |

When a token is configured, request/response bodies for control and cmds 7–12 are AES-256-GCM (`nonce\|\|cipher\|\|tag`) with key `SHA-256("audio-share/1/session/"+token)`.

## Out of scope (needs drivers / video stack)
Live desktop mirror, virtual webcam, virtual monitors, gamepad/Xbox emulation.
