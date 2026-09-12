#pragma once
#include "AudioPlayer.h"

namespace Ship {
/**
 * @brief AudioPlayer implementation backed by the original Xbox DirectSound (RXDK libdsound).
 *
 * Replaces SDLAudioPlayer on the Xbox port. Streams interleaved PCM into a looping
 * DirectSound secondary buffer, writing at the play cursor and tracking how many frames
 * are queued for the audio subsystem's pacing (Buffered()). Stereo and 5.1 are selected
 * from the AudioChannelsSetting in AudioPlayer.
 *
 * DirectSound handles are held as opaque pointers so this header pulls no dsound.h into
 * the libultraship include tree; the concrete types live in XboxAudioPlayer.cpp.
 */
class XboxAudioPlayer final : public AudioPlayer {
  public:
    XboxAudioPlayer(AudioSettings settings) : AudioPlayer(settings) {
    }
    ~XboxAudioPlayer();

    /** @brief Frames currently queued between the write and play cursors. */
    int Buffered() override;

  protected:
    /** @brief Creates the DirectSound device and streaming secondary buffer. */
    bool DoInit() override;

    /** @brief Stops and releases the DirectSound buffer and device. */
    void DoClose() override;

    /** @brief Writes interleaved PCM into the streaming buffer at the write cursor. */
    void DoPlay(const uint8_t* buf, size_t len) override;

  private:
    void* mDsound = nullptr;     ///< LPDIRECTSOUND (opaque).
    void* mBuffer = nullptr;     ///< LPDIRECTSOUNDBUFFER streaming buffer (opaque).
    uint32_t mBufferBytes = 0;   ///< Total byte size of the streaming buffer.
    uint32_t mWriteOffset = 0;   ///< Current write cursor into the streaming buffer.
    int32_t mNumChannels = 2;    ///< Output channels (2 stereo, 6 for 5.1).
    int32_t mBytesPerFrame = 4;  ///< Channels * sizeof(int16_t).
};
} // namespace Ship
