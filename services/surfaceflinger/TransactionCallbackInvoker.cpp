/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "TransactionCallbackInvoker"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "TransactionCallbackInvoker.h"

#include <cinttypes>

#include <binder/IInterface.h>
#include <utils/RefBase.h>

namespace android {

// Returns 0 if they are equal
//         <0 if the first id that doesn't match is lower in c2 or all ids match but c2 is shorter
//         >0 if the first id that doesn't match is greater in c2 or all ids match but c2 is longer
//
// See CallbackIdsHash for a explanation of why this works
static int compareCallbackIds(const std::vector<CallbackId>& c1,
                              const std::vector<CallbackId>& c2) {
    if (c1.empty()) {
        return !c2.empty();
    }
    return c1.front().id - c2.front().id;
}

static bool containsOnCommitCallbacks(const std::vector<CallbackId>& callbacks) {
    return !callbacks.empty() && callbacks.front().type == CallbackId::Type::ON_COMMIT;
}

TransactionCallbackInvoker::~TransactionCallbackInvoker() {
    {
        std::lock_guard lock(mMutex);
        for (const auto& [listener, transactionStats] : mCompletedTransactions) {
            listener->unlinkToDeath(mDeathRecipient);
        }
    }
}

status_t TransactionCallbackInvoker::startRegistration(const ListenerCallbacks& listenerCallbacks) {
    std::lock_guard lock(mMutex);

    auto [itr, inserted] = mRegisteringTransactions.insert(listenerCallbacks);
    auto& [listener, callbackIds] = listenerCallbacks;

    if (inserted) {
        if (mCompletedTransactions.count(listener) == 0) {
            status_t err = listener->linkToDeath(mDeathRecipient);
            if (err != NO_ERROR) {
                ALOGE("cannot add callback because linkToDeath failed, err: %d", err);
                return err;
            }
        }
        auto& transactionStatsDeque = mCompletedTransactions[listener];
        transactionStatsDeque.emplace_back(callbackIds);
    }

    return NO_ERROR;
}

status_t TransactionCallbackInvoker::endRegistration(const ListenerCallbacks& listenerCallbacks) {
    std::lock_guard lock(mMutex);

    auto itr = mRegisteringTransactions.find(listenerCallbacks);
    if (itr == mRegisteringTransactions.end()) {
        ALOGE("cannot end a registration that does not exist");
        return BAD_VALUE;
    }

    mRegisteringTransactions.erase(itr);

    return NO_ERROR;
}

bool TransactionCallbackInvoker::isRegisteringTransaction(
        const sp<IBinder>& transactionListener, const std::vector<CallbackId>& callbackIds) {
    ListenerCallbacks listenerCallbacks(transactionListener, callbackIds);

    auto itr = mRegisteringTransactions.find(listenerCallbacks);
    return itr != mRegisteringTransactions.end();
}

status_t TransactionCallbackInvoker::registerPendingCallbackHandle(
        const sp<CallbackHandle>& handle) {
    std::lock_guard lock(mMutex);

    // If we can't find the transaction stats something has gone wrong. The client should call
    // startRegistration before trying to register a pending callback handle.
    TransactionStats* transactionStats;
    status_t err = findTransactionStats(handle->listener, handle->callbackIds, &transactionStats);
    if (err != NO_ERROR) {
        ALOGE("cannot find transaction stats");
        return err;
    }

    mPendingTransactions[handle->listener][handle->callbackIds]++;
    return NO_ERROR;
}

status_t TransactionCallbackInvoker::finalizeCallbackHandle(const sp<CallbackHandle>& handle,
                                                            const std::vector<JankData>& jankData) {
    auto listener = mPendingTransactions.find(handle->listener);
    if (listener != mPendingTransactions.end()) {
        auto& pendingCallbacks = listener->second;
        auto pendingCallback = pendingCallbacks.find(handle->callbackIds);

        if (pendingCallback != pendingCallbacks.end()) {
            auto& pendingCount = pendingCallback->second;

            // Decrease the pending count for this listener
            if (--pendingCount == 0) {
                pendingCallbacks.erase(pendingCallback);
            }
        } else {
            ALOGW("there are more latched callbacks than there were registered callbacks");
        }
        if (listener->second.size() == 0) {
            mPendingTransactions.erase(listener);
        }
    } else {
        ALOGW("cannot find listener in mPendingTransactions");
    }

    status_t err = addCallbackHandle(handle, jankData);
    if (err != NO_ERROR) {
        ALOGE("could not add callback handle");
        return err;
    }
    return NO_ERROR;
}

status_t TransactionCallbackInvoker::finalizeOnCommitCallbackHandles(
        const std::deque<sp<CallbackHandle>>& handles,
        std::deque<sp<CallbackHandle>>& outRemainingHandles) {
    if (handles.empty()) {
        return NO_ERROR;
    }
    std::lock_guard lock(mMutex);
    const std::vector<JankData>& jankData = std::vector<JankData>();
    for (const auto& handle : handles) {
        if (!containsOnCommitCallbacks(handle->callbackIds)) {
            outRemainingHandles.push_back(handle);
            continue;
        }
        status_t err = finalizeCallbackHandle(handle, jankData);
        if (err != NO_ERROR) {
            return err;
        }
    }

    return NO_ERROR;
}

status_t TransactionCallbackInvoker::finalizePendingCallbackHandles(
        const std::deque<sp<CallbackHandle>>& handles, const std::vector<JankData>& jankData) {
    if (handles.empty()) {
        return NO_ERROR;
    }
    std::lock_guard lock(mMutex);
    for (const auto& handle : handles) {
        status_t err = finalizeCallbackHandle(handle, jankData);
        if (err != NO_ERROR) {
            return err;
        }
    }

    return NO_ERROR;
}

status_t TransactionCallbackInvoker::registerUnpresentedCallbackHandle(
        const sp<CallbackHandle>& handle) {
    std::lock_guard lock(mMutex);

    return addCallbackHandle(handle, std::vector<JankData>());
}

status_t TransactionCallbackInvoker::findTransactionStats(
        const sp<IBinder>& listener, const std::vector<CallbackId>& callbackIds,
        TransactionStats** outTransactionStats) {
    auto& transactionStatsDeque = mCompletedTransactions[listener];

    // Search back to front because the most recent transactions are at the back of the deque
    auto itr = transactionStatsDeque.rbegin();
    for (; itr != transactionStatsDeque.rend(); itr++) {
        if (compareCallbackIds(itr->callbackIds, callbackIds) == 0) {
            *outTransactionStats = &(*itr);
            return NO_ERROR;
        }
    }

    ALOGE("could not find transaction stats");
    return BAD_VALUE;
}

status_t TransactionCallbackInvoker::addCallbackHandle(const sp<CallbackHandle>& handle,
        const std::vector<JankData>& jankData) {
    // If we can't find the transaction stats something has gone wrong. The client should call
    // startRegistration before trying to add a callback handle.
    TransactionStats* transactionStats;
    status_t err = findTransactionStats(handle->listener, handle->callbackIds, &transactionStats);
    if (err != NO_ERROR) {
        return err;
    }

    transactionStats->latchTime = handle->latchTime;
    // If the layer has already been destroyed, don't add the SurfaceControl to the callback.
    // The client side keeps a sp<> to the SurfaceControl so if the SurfaceControl has been
    // destroyed the client side is dead and there won't be anyone to send the callback to.
    sp<IBinder> surfaceControl = handle->surfaceControl.promote();
    if (surfaceControl) {
        FrameEventHistoryStats eventStats(handle->frameNumber,
                                          handle->gpuCompositionDoneFence->getSnapshot().fence,
                                          handle->compositorTiming, handle->refreshStartTime,
                                          handle->dequeueReadyTime);
        transactionStats->surfaceStats.emplace_back(surfaceControl, handle->acquireTime,
                                                    handle->previousReleaseFence,
                                                    handle->transformHint,
                                                    handle->currentMaxAcquiredBufferCount,
                                                    eventStats, jankData,
                                                    handle->previousReleaseCallbackId);
    }
    return NO_ERROR;
}

void TransactionCallbackInvoker::addPresentFence(const sp<Fence>& presentFence) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPresentFence = presentFence;
}

void TransactionCallbackInvoker::sendCallbacks() {
    std::lock_guard lock(mMutex);

    // For each listener
    auto completedTransactionsItr = mCompletedTransactions.begin();
    while (completedTransactionsItr != mCompletedTransactions.end()) {
        auto& [listener, transactionStatsDeque] = *completedTransactionsItr;
        ListenerStats listenerStats;
        listenerStats.listener = listener;

        // For each transaction
        auto transactionStatsItr = transactionStatsDeque.begin();
        while (transactionStatsItr != transactionStatsDeque.end()) {
            auto& transactionStats = *transactionStatsItr;

            // If this transaction is still registering, it is not safe to send a callback
            // because there could be surface controls that haven't been added to
            // transaction stats or mPendingTransactions.
            if (isRegisteringTransaction(listener, transactionStats.callbackIds)) {
                break;
            }

            // If we are still waiting on the callback handles for this transaction, stop
            // here because all transaction callbacks for the same listener must come in order
            auto pendingTransactions = mPendingTransactions.find(listener);
            if (pendingTransactions != mPendingTransactions.end() &&
                pendingTransactions->second.count(transactionStats.callbackIds) != 0) {
                break;
            }

            // If the transaction has been latched
            if (transactionStats.latchTime >= 0 &&
                !containsOnCommitCallbacks(transactionStats.callbackIds)) {
                if (!mPresentFence) {
                    break;
                }
                transactionStats.presentFence = mPresentFence;
            }

            // Remove the transaction from completed to the callback
            listenerStats.transactionStats.push_back(std::move(transactionStats));
            transactionStatsItr = transactionStatsDeque.erase(transactionStatsItr);
        }
        // If the listener has completed transactions
        if (!listenerStats.transactionStats.empty()) {
            // If the listener is still alive
            if (listener->isBinderAlive()) {
                // Send callback.  The listener stored in listenerStats
                // comes from the cross-process setTransactionState call to
                // SF.  This MUST be an ITransactionCompletedListener.  We
                // keep it as an IBinder due to consistency reasons: if we
                // interface_cast at the IPC boundary when reading a Parcel,
                // we get pointers that compare unequal in the SF process.
                interface_cast<ITransactionCompletedListener>(listenerStats.listener)
                        ->onTransactionCompleted(listenerStats);
                if (transactionStatsDeque.empty()) {
                    listener->unlinkToDeath(mDeathRecipient);
                    completedTransactionsItr =
                            mCompletedTransactions.erase(completedTransactionsItr);
                } else {
                    completedTransactionsItr++;
                }
            } else {
                completedTransactionsItr =
                        mCompletedTransactions.erase(completedTransactionsItr);
            }
        } else {
            completedTransactionsItr++;
        }
    }

    if (mPresentFence) {
        mPresentFence.clear();
    }
}

// -----------------------------------------------------------------------

CallbackHandle::CallbackHandle(const sp<IBinder>& transactionListener,
                               const std::vector<CallbackId>& ids, const sp<IBinder>& sc)
      : listener(transactionListener), callbackIds(ids), surfaceControl(sc) {}

} // namespace android

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion"
