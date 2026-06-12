#ifndef VITA_AUDIO_AUDIO_H
#define VITA_AUDIO_AUDIO_H

/*
 * Small mixer on sceAudio: OGG files (stb_vorbis) decode fully to PCM
 * on load and play on one-shot channels with distance attenuation and
 * pan, plus one looping ambience channel. The mixer runs on its own
 * thread, paced by the audio output.
 */

/* Start the output port and mixer thread. Returns 0 on failure. */
int audioInit(void);

/* Load (and cache) an OGG; returns a sound handle or -1. */
int audioLoad(const char *path);

/* One-shot playback, vol 0..1, pan -1..1. */
void audioPlay(int sound, float vol, float pan);

/* One-shot with distance attenuation and pan relative to a listener
 * at `listener` facing `yaw` (radians); `range` in the same units. */
void audioPlay3D(int sound, const float pos[3], const float listener[3],
                 float yaw, float range);

/* Loop a sound on the dedicated ambience channel (replaces previous). */
void audioLoopAmbience(int sound, float vol);

#endif
