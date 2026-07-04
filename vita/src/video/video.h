#ifndef VITA_VIDEO_H
#define VITA_VIDEO_H

/*
 * Full-motion video via sceAvPlayer (hardware H.264 decode) drawn
 * through vitaGL. Used for the startup_Intro cutscene, whose source
 * .wmv the Vita cannot decode but which also ships as H.264 MP4.
 *
 * The decoder hands back a 2-plane YUV420 frame in CDRAM; the GXM
 * texture unit converts it to RGB in hardware (the YVU420P2_CSC1
 * format), so there is no CPU colour conversion or shader.
 */

/* Load the AvPlayer sysmodule once. Returns 1 on success, 0 if video
 * playback is unavailable (the caller should then just skip it). */
int videoInit(void);

/* Play one MP4 file (blocking) letterboxed to the screen, with its
 * audio on the BGM output port; any button or a touch skips it.
 * Returns 1 if it played, 0 if it could not be opened. Runs its own
 * render loop and calls inputUpdate() each frame. */
int videoPlayFile(const char *path);

#endif
