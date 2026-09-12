#include "libultraship/libultraship.h"

#if defined(LUS_XBOX)
#include <thread>
#include <chrono>
#include <atomic>
#endif

extern "C" {

static void __lusViPost(void) {
    __OSEventState* es = &__osEventStateTab[OS_EVENT_VI];
    if (es && es->queue) {
        osSendMesg(es->queue, es->msg, OS_MESG_NOBLOCK);
    }
}

#if defined(LUS_XBOX)
// No SDL timer subsystem on Xbox; drive the ~60 Hz VI retrace event from a dedicated
// thread. The game's graph thread blocks on this message, so it paces frame advance.
// TODO(Phase 2): retire this in favour of the D3D8 device vsync / present cadence.
static std::atomic_bool sViThreadRunning{ false };

static void __lusViThread(void) {
    while (sViThreadRunning.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::microseconds(16667)); // ~59.94 Hz
        __lusViPost();
    }
}

void osCreateViManager(OSPri pri) {
    if (sViThreadRunning.exchange(true)) {
        return; // already running
    }
    std::thread(__lusViThread).detach();
}
#else
Uint32 __lusViCallback(Uint32 interval, void* param) {
    __lusViPost();
    return interval;
}

void osCreateViManager(OSPri pri) {
    SDL_AddTimer(16, &__lusViCallback, NULL);
}
#endif

void osViSetEvent(OSMesgQueue* queue, OSMesg mesg, uint32_t c) {

    __OSEventState* es = &__osEventStateTab[OS_EVENT_VI];

    es->queue = queue;
    es->msg = mesg;
}

void osViSwapBuffer(void* a) {
}

void osViSetSpecialFeatures(uint32_t a) {
}

void osViSetMode(OSViMode* a) {
}

void osViBlack(uint8_t a) {
}

void* osViGetNextFramebuffer() {
    return nullptr;
}

void* osViGetCurrentFramebuffer() {
    return nullptr;
}

void osViSetXScale(float a) {
}

void osViSetYScale(float a) {
}
}