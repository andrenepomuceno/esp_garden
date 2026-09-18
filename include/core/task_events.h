#pragma once

// The asynchronous half of the io tick, and the two relay seams that feed it.
//
// Everything here answers the same question: a transition happened — a relay
// switched, a reservoir emptied, a cloud edge passed, a day closed — and the
// periodic payload is built once every five minutes, so sampling it would miss
// most of them. See "Sampling vs events" in CLAUDE.md. Each of these turns a
// bit that switching or a model recorded into a message on tbPublishEvent()'s
// outbox, which tbLoop() drains on every loop() iteration.
//
// It lives beside src/tasks.cpp rather than inside it because none of it is
// scheduling. tasks.cpp owns the DECLARE_TASK set, the registration order in
// tasksSetup() and every reference to a g_*Task object; this owns what one of
// those handlers does when it runs. The split was forced by the 1000-line gate
// and chosen here because it moves no handler body and no registration.
//
// EVERY function below runs on the io task (1 Hz) or the checkMoisture task,
// except the two relay seams, which run on whichever thread asked for a relay.
// The critical runner must never call any of them: building a String under a
// 50 ms deadline is the mistake that panicked this board with an interrupt
// watchdog, which is why relaysTick() only sets bits.

#include <Arduino.h>

// Called in this order from ioTaskHandler(), after the accumulators have been
// fed and /data.json has been rendered from them. The order is load-bearing:
// publishCloudEvents() takes the tick's return value, and the step publisher
// that runs after them all reads the state those ticks produced.
void
publishRelayEvents();

void
publishFloatEvents();

// `events` is cloudModelTick()'s return value. The model is ticked by the
// caller, not here, so ioTaskHandler() still reads as the literal list of what
// happens in one second.
void
publishCloudEvents(int events);

void
publishEt0Event();

// The post-watering check, run once by the checkMoisture task four hours after
// a zone was watered. Reads the baseline relayStartedHook() took; the task's own
// disable() stays with the task, in tasks.cpp.
void
reportWateringResponse();
