/* Bounded OpenAL device close.
 *
 * openal-soft's SDL2 backend can block forever in alcCloseDevice on Horizon
 * (observed on 2026-10-25: the render thread stayed in ALC10.alcCloseDevice
 * after the world had been saved during a crash shutdown). The real close runs
 * on a helper thread; after a bounded wait the caller continues and the device
 * stays open until the process exits. Linked with -Wl,--wrap=alcCloseDevice. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>

typedef struct ALCdevice ALCdevice;
typedef char ALCboolean;
extern ALCboolean __real_alcCloseDevice(ALCdevice *device);

struct CloseRequest {
    ALCdevice *device;
    ALCboolean result;
    atomic_int done;
};

static void close_entry(void *argument) {
    struct CloseRequest *request = argument;
    request->result = __real_alcCloseDevice(request->device);
    atomic_store(&request->done, 1);
}

ALCboolean __wrap_alcCloseDevice(ALCdevice *device) {
    struct CloseRequest *request = calloc(1, sizeof(*request));
    if (!request)
        return __real_alcCloseDevice(device);
    request->device = device;
    Thread thread;
    if (R_FAILED(threadCreate(&thread, close_entry, request, NULL, 64 * 1024, 0x2C, -2))) {
        free(request);
        return __real_alcCloseDevice(device);
    }
    if (R_FAILED(threadStart(&thread))) {
        threadClose(&thread);
        free(request);
        return __real_alcCloseDevice(device);
    }
    for (int waited = 0; waited < 200 && !atomic_load(&request->done); ++waited)
        svcSleepThread(10000000ULL);
    if (!atomic_load(&request->done)) {
        /* The helper thread and its request are intentionally leaked. */
        printf("MINESWITCH_AUDIO_CLOSE_TIMEOUT device=%p waited_ms=2000\n", (void *)device);
        fflush(stdout);
        return 1;
    }
    threadWaitForExit(&thread);
    threadClose(&thread);
    ALCboolean result = request->result;
    free(request);
    return result;
}
