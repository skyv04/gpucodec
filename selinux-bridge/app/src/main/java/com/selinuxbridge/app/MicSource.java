package com.selinuxbridge.app;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;

import java.io.IOException;

/**
 * Microphone capture, handed to the Debian side as raw little-endian S16.
 *
 * The container has no route to audio input at all: /dev/snd/* is denied, so
 * there is no ALSA device to open, and the PulseAudio server it can reach
 * belongs to Termux and exposes only a sink monitor. Capture therefore has to
 * happen in the app process, same as camera and codec.
 *
 * Whatever is selected as Android's input -- the built-in mics, a USB audio
 * interface, a Bluetooth headset -- is what AudioRecord returns, so external
 * microphones are covered without any extra work here.
 *
 * Note the audio source really does matter. VOICE_COMMUNICATION runs the
 * platform's echo canceller and noise suppressor, which is what you want when
 * the far end's audio is coming out of this phone's speaker; MIC is the raw
 * path and is what you want when something downstream is already doing that.
 * Picking one silently would be wrong for half the callers, so it is a
 * protocol field.
 */
final class MicSource {

    private MicSource() {}

    /** Read timeout equivalent: how long silence may last before we bail. */
    private static final int DEFAULT_RATE = 48000;

    interface Logger { void log(String msg); }

    /** Where captured audio goes. Mirrors CameraSource.FrameSink. */
    interface AudioSink {
        void format(int sampleRate, int channels) throws IOException;
        void chunk(byte[] data, int len) throws IOException;
    }

    static String sourceName(int id) {
        switch (id) {
            case 0: return "MIC";
            case 1: return "VOICE_COMMUNICATION";
            case 2: return "CAMCORDER";
            case 3: return "UNPROCESSED";
            default: return "unknown";
        }
    }

    static int androidSource(int id) {
        switch (id) {
            case 0: return MediaRecorder.AudioSource.MIC;
            case 1: return MediaRecorder.AudioSource.VOICE_COMMUNICATION;
            case 2: return MediaRecorder.AudioSource.CAMCORDER;
            case 3: return MediaRecorder.AudioSource.UNPROCESSED;
            default: return -1;
        }
    }

    /**
     * Streams S16LE PCM into {@code sink}.
     *
     * @param sampleRate 0 selects 48000, which is what WebRTC wants anyway.
     * @param channels   1 or 2; 0 selects mono.
     * @param maxSeconds 0 means "until the client disconnects".
     * @return bytes delivered.
     */
    static long stream(int sampleRate, int channels, int sourceId, int maxSeconds,
                       AudioSink sink, Logger logger) throws IOException {

        if (sampleRate <= 0) sampleRate = DEFAULT_RATE;
        if (channels <= 0) channels = 1;
        if (channels != 1 && channels != 2) {
            throw new IOException("channels must be 1 or 2, got " + channels);
        }
        int androidSource = androidSource(sourceId);
        if (androidSource < 0) {
            throw new IOException("unknown audio source " + sourceId
                    + " (expected 0=mic, 1=voice_communication, 2=camcorder, 3=unprocessed)");
        }

        int channelMask = channels == 2
                ? AudioFormat.CHANNEL_IN_STEREO : AudioFormat.CHANNEL_IN_MONO;

        int minBuf = AudioRecord.getMinBufferSize(
                sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf == AudioRecord.ERROR || minBuf == AudioRecord.ERROR_BAD_VALUE) {
            throw new IOException("device rejects " + sampleRate + "Hz x" + channels
                    + " S16 capture");
        }
        /*
         * Four times the minimum. The minimum is the point at which overrun
         * becomes certain under any scheduling hiccup, and this container is
         * routinely descheduled onto four little cores, so the floor is not
         * a safe working size.
         */
        int bufBytes = minBuf * 4;

        AudioRecord rec;
        try {
            rec = new AudioRecord(androidSource, sampleRate, channelMask,
                    AudioFormat.ENCODING_PCM_16BIT, bufBytes);
        } catch (IllegalArgumentException e) {
            throw new IOException("AudioRecord rejected the requested format: " + e.getMessage(), e);
        } catch (SecurityException e) {
            throw new IOException("RECORD_AUDIO permission not granted to the bridge app"
                    + " -- open the app and allow microphone access", e);
        }

        if (rec.getState() != AudioRecord.STATE_INITIALIZED) {
            rec.release();
            throw new IOException("AudioRecord failed to initialise -- this is what a"
                    + " missing RECORD_AUDIO grant looks like at runtime");
        }

        long bytesPerSecond = (long) sampleRate * channels * 2;
        long limit = maxSeconds > 0 ? bytesPerSecond * maxSeconds : 0;
        long total = 0;
        // A tenth of a second per chunk: small enough not to add audible
        // latency, large enough not to syscall-thrash.
        byte[] buf = new byte[(int) Math.max(2048, bytesPerSecond / 10)];

        try {
            rec.startRecording();
            if (rec.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
                throw new IOException("microphone did not start -- it may be held by"
                        + " another app, or the grant was revoked");
            }
            logger.log("microphone " + sourceName(sourceId) + " " + sampleRate + "Hz x"
                    + channels + " s16le, buffer " + bufBytes + "B");

            sink.format(sampleRate, channels);

            while (limit == 0 || total < limit) {
                int want = buf.length;
                if (limit > 0 && total + want > limit) want = (int) (limit - total);
                int n = rec.read(buf, 0, want);
                if (n == AudioRecord.ERROR_INVALID_OPERATION || n == AudioRecord.ERROR_BAD_VALUE) {
                    throw new IOException("AudioRecord.read failed with " + n);
                }
                if (n <= 0) break;
                sink.chunk(buf, n);
                total += n;
            }
        } finally {
            try { rec.stop(); } catch (Throwable ignored) {}
            rec.release();
        }
        return total;
    }
}
