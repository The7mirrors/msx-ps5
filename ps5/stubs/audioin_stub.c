// MSX PS5 (2026-10-11, AI-assisted): the link stub for libSceAudioIn.sprx, which the payload SDK has no stub for.
// Built as libSceAudioIn.so (SONAME libSceAudioIn.sprx); ps5-native-tool takes the names from it and the console's own
// libSceAudioIn is bound with the app's imports. By name the app may neither load it (0x80020063) nor look its
// functions up (sceKernelDlsym 0x80020003, measured), although the system has it loaded in the app (handle 109).
// The bodies are never run. SPDX-License-Identifier: MIT
int sceAudioInOpen(void) { return 0; }
int sceAudioInInput(void) { return 0; }
int sceAudioInClose(void) { return 0; }
int sceAudioInGetSilentState(void) { return 0; }
int sceAudioInSetUserMute(void) { return 0; }
