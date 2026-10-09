// SPDX-License-Identifier: LGPL-2.1-or-later
// Completion notifications: MTLSharedEvent listener blocks feed a queue that a
// front-end thread blocks on.
//
// The listener blocks run on a Metal dispatch thread. They only lock a mutex,
// push a record and notify a condition variable; everything else happens on the
// front-end's waiter thread.
#include "internal.h"

#include <condition_variable>
#include <deque>
#include <memory>

namespace {

using namespace mtlb;

struct NotifyQueue {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<mtlb_notification> pending;
    bool closed = false;
};

// A handle owns one reference; listener blocks hold their own, so a queue
// destroyed with registrations outstanding stays valid until they fire.
using NotifyRef = std::shared_ptr<NotifyQueue>;

} // namespace

extern "C" {

mtlb_result mtlb_notify_create(mtlb_notify *out)
{
    if (!out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    *out = to_handle(new NotifyRef(std::make_shared<NotifyQueue>()));
    return MTLB_OK;
}

void mtlb_notify_destroy(mtlb_notify handle)
{
    NotifyRef *ref = from_handle<NotifyRef>(handle);
    if (!ref)
        return;
    mtlb_notify_close(handle);
    delete ref;
}

mtlb_result mtlb_event_notify(mtlb_event event_handle, uint64_t value, mtlb_notify queue_handle, uint64_t cookie)
{
    Event *event = from_handle<Event>(event_handle);
    NotifyRef *ref = from_handle<NotifyRef>(queue_handle);
    if (!event || !ref)
        return MTLB_ERROR_INVALID_ARGUMENT;
    NotifyRef queue = *ref;
    [event->event notifyListener:event->device->listener
                         atValue:value
                           block:^(id<MTLSharedEvent>, uint64_t reached) {
        {
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->pending.push_back({cookie, reached});
        }
        queue->wake.notify_one();
    }];
    return MTLB_OK;
}

mtlb_result mtlb_notify_wait(mtlb_notify handle, mtlb_notification *out, uint32_t max, uint32_t *count)
{
    NotifyRef *ref = from_handle<NotifyRef>(handle);
    if (!ref || !out || !count || max == 0)
        return MTLB_ERROR_INVALID_ARGUMENT;
    NotifyQueue &queue = **ref;
    std::unique_lock<std::mutex> lock(queue.mutex);
    queue.wake.wait(lock, [&] { return queue.closed || !queue.pending.empty(); });
    *count = 0;
    while (*count < max && !queue.pending.empty() && !queue.closed) {
        out[(*count)++] = queue.pending.front();
        queue.pending.pop_front();
    }
    return MTLB_OK;
}

void mtlb_notify_close(mtlb_notify handle)
{
    NotifyRef *ref = from_handle<NotifyRef>(handle);
    if (!ref)
        return;
    {
        std::lock_guard<std::mutex> lock((*ref)->mutex);
        (*ref)->closed = true;
    }
    (*ref)->wake.notify_all();
}

} // extern "C"
