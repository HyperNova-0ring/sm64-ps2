#ifdef TARGET_PS2

#include <stdio.h>
#include <kernel.h>
#include <ps2_audio_driver.h>

#include "audsrv.h"

#include "macros.h"
#include "audio_api.h"

static bool audio_ps2_init(void) {
    if (init_audio_driver() != 0) return false;

    audsrv_fmt_t fmt;

    fmt.freq = 32000;
    fmt.bits = 16;
    fmt.channels = 2;

    if (audsrv_set_format(&fmt)) {
        printf("audio_ps2: unsupported sound format\n");
        audsrv_quit();
        return false;
    }

    return true;
}

static int audio_ps2_buffered(void) {
    return audsrv_queued() / 4;
}

static int audio_ps2_get_desired_buffered(void) {
    return 1100;
}

static void audio_ps2_play(const uint8_t *buf, size_t len) {
    if (audio_ps2_buffered() < 6000)
        audsrv_play_audio(buf, len);
}

// fills all the free space of audsrv's ring buffer with silence; when the ring
// underruns it replays its old contents, so this keeps it silent
void audio_ps2_fill_silence(void) {
    static const uint8_t zeros[4096] __attribute__((aligned(64)));

    int avail = audsrv_available() & ~3;
    while (avail > 0) {
        const int len = avail < (int)sizeof(zeros) ? avail : (int)sizeof(zeros);
        const int sent = audsrv_play_audio((const char *)zeros, len);
        if (sent <= 0) break;
        avail -= sent;
    }
}

struct AudioAPI audio_ps2 = {
    audio_ps2_init,
    audio_ps2_buffered,
    audio_ps2_get_desired_buffered,
    audio_ps2_play
};

#endif // TARGET_PS2
