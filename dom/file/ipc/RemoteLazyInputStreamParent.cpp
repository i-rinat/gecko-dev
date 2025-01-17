/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RemoteLazyInputStreamParent.h"
#include "mozilla/ipc/IPCStreamUtils.h"
#include "mozilla/InputStreamLengthHelper.h"
#include "nsContentUtils.h"
#include "RemoteLazyInputStreamStorage.h"

namespace mozilla {

template <typename M>
/* static */
already_AddRefed<RemoteLazyInputStreamParent>
RemoteLazyInputStreamParent::Create(nsIInputStream* aInputStream,
                                    uint64_t aSize, uint64_t aChildID,
                                    nsresult* aRv, M* aManager) {
  MOZ_ASSERT(aInputStream);
  MOZ_ASSERT(aRv);

  nsID id;
  *aRv = nsContentUtils::GenerateUUIDInPlace(id);
  if (NS_WARN_IF(NS_FAILED(*aRv))) {
    return nullptr;
  }

  RemoteLazyInputStreamStorage::Get()->AddStream(aInputStream, id, aSize,
                                                 aChildID);

  RefPtr<RemoteLazyInputStreamParent> parent =
      new RemoteLazyInputStreamParent(id, aSize, aManager);
  return parent.forget();
}

/* static */
already_AddRefed<RemoteLazyInputStreamParent>
RemoteLazyInputStreamParent::Create(const nsID& aID, uint64_t aSize,
                                    PBackgroundParent* aManager) {
  RefPtr<RemoteLazyInputStreamParent> actor =
      new RemoteLazyInputStreamParent(aID, aSize, aManager);

  actor->mCallback = RemoteLazyInputStreamStorage::Get()->TakeCallback(aID);

  return actor.forget();
}

template already_AddRefed<RemoteLazyInputStreamParent>
RemoteLazyInputStreamParent::Create<mozilla::ipc::PBackgroundParent>(
    nsIInputStream*, uint64_t, uint64_t, nsresult*,
    mozilla::ipc::PBackgroundParent*);

/* static */
already_AddRefed<RemoteLazyInputStreamParent>
RemoteLazyInputStreamParent::Create(const nsID& aID, uint64_t aSize,
                                    SocketProcessParent* aManager) {
  RefPtr<RemoteLazyInputStreamParent> actor =
      new RemoteLazyInputStreamParent(aID, aSize, aManager);

  actor->mCallback = RemoteLazyInputStreamStorage::Get()->TakeCallback(aID);

  return actor.forget();
}

template already_AddRefed<RemoteLazyInputStreamParent>
RemoteLazyInputStreamParent::Create<mozilla::net::SocketProcessParent>(
    nsIInputStream*, uint64_t, uint64_t, nsresult*,
    mozilla::net::SocketProcessParent*);

template already_AddRefed<RemoteLazyInputStreamParent>
RemoteLazyInputStreamParent::Create<ContentParent>(nsIInputStream*, uint64_t,
                                                   uint64_t, nsresult*,
                                                   ContentParent*);

RemoteLazyInputStreamParent::RemoteLazyInputStreamParent(
    const nsID& aID, uint64_t aSize, ContentParent* aManager)
    : mID(aID),
      mSize(aSize),
      mContentManager(aManager),
      mPBackgroundManager(nullptr),
      mSocketProcessManager(nullptr),
      mMigrating(false) {}

RemoteLazyInputStreamParent::RemoteLazyInputStreamParent(
    const nsID& aID, uint64_t aSize, PBackgroundParent* aManager)
    : mID(aID),
      mSize(aSize),
      mContentManager(nullptr),
      mPBackgroundManager(aManager),
      mSocketProcessManager(nullptr),
      mMigrating(false) {}

RemoteLazyInputStreamParent::RemoteLazyInputStreamParent(
    const nsID& aID, uint64_t aSize, SocketProcessParent* aManager)
    : mID(aID),
      mSize(aSize),
      mContentManager(nullptr),
      mPBackgroundManager(nullptr),
      mSocketProcessManager(aManager),
      mMigrating(false) {}

void RemoteLazyInputStreamParent::ActorDestroy(
    IProtocol::ActorDestroyReason aReason) {
  MOZ_ASSERT(mContentManager || mPBackgroundManager || mSocketProcessManager);

  mContentManager = nullptr;
  mPBackgroundManager = nullptr;
  mSocketProcessManager = nullptr;

  RefPtr<RemoteLazyInputStreamParentCallback> callback;
  mCallback.swap(callback);

  RefPtr<RemoteLazyInputStreamStorage> storage =
      RemoteLazyInputStreamStorage::Get();

  if (mMigrating) {
    if (callback && storage) {
      // We need to assign this callback to the next parent.
      storage->StoreCallback(mID, callback);
    }
    return;
  }

  if (storage) {
    storage->ForgetStream(mID);
  }

  if (callback) {
    callback->ActorDestroyed(mID);
  }
}

void RemoteLazyInputStreamParent::SetCallback(
    RemoteLazyInputStreamParentCallback* aCallback) {
  MOZ_ASSERT(aCallback);
  MOZ_ASSERT(!mCallback);

  mCallback = aCallback;
}

mozilla::ipc::IPCResult RemoteLazyInputStreamParent::RecvStreamNeeded() {
  MOZ_ASSERT(mContentManager || mPBackgroundManager || mSocketProcessManager);

  nsCOMPtr<nsIInputStream> stream;
  RemoteLazyInputStreamStorage::Get()->GetStream(mID, 0, mSize,
                                                 getter_AddRefs(stream));
  if (!stream) {
    if (!SendStreamReady(Nothing())) {
      return IPC_FAIL(this, "SendStreamReady failed");
    }

    return IPC_OK();
  }

  mozilla::ipc::AutoIPCStream ipcStream;
  bool ok = false;

  if (mContentManager) {
    MOZ_ASSERT(NS_IsMainThread());
    ok = ipcStream.Serialize(stream, mContentManager);
  } else if (mPBackgroundManager) {
    ok = ipcStream.Serialize(stream, mPBackgroundManager);
  } else {
    MOZ_ASSERT(mSocketProcessManager);
    ok = ipcStream.Serialize(stream, mSocketProcessManager);
  }

  if (NS_WARN_IF(!ok)) {
    return IPC_FAIL(this, "SendStreamReady failed");
  }

  if (!SendStreamReady(Some(ipcStream.TakeValue()))) {
    return IPC_FAIL(this, "SendStreamReady failed");
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult RemoteLazyInputStreamParent::RecvLengthNeeded() {
  MOZ_ASSERT(mContentManager || mPBackgroundManager || mSocketProcessManager);

  nsCOMPtr<nsIInputStream> stream;
  RemoteLazyInputStreamStorage::Get()->GetStream(mID, 0, mSize,
                                                 getter_AddRefs(stream));
  if (!stream) {
    if (!SendLengthReady(-1)) {
      return IPC_FAIL(this, "SendLengthReady failed");
    }

    return IPC_OK();
  }

  int64_t length = -1;
  if (InputStreamLengthHelper::GetSyncLength(stream, &length)) {
    Unused << SendLengthReady(length);
    return IPC_OK();
  }

  RefPtr<RemoteLazyInputStreamParent> self = this;
  InputStreamLengthHelper::GetAsyncLength(stream, [self](int64_t aLength) {
    if (self->mContentManager || self->mPBackgroundManager ||
        self->mSocketProcessManager) {
      Unused << self->SendLengthReady(aLength);
    }
  });

  return IPC_OK();
}

mozilla::ipc::IPCResult RemoteLazyInputStreamParent::RecvClose() {
  MOZ_ASSERT(mContentManager || mPBackgroundManager || mSocketProcessManager);

  Unused << Send__delete__(this);
  return IPC_OK();
}

mozilla::ipc::IPCResult RemoteLazyInputStreamParent::Recv__delete__() {
  MOZ_ASSERT(mContentManager || mPBackgroundManager || mSocketProcessManager);
  mMigrating = true;
  return IPC_OK();
}

bool RemoteLazyInputStreamParent::HasValidStream() const {
  return RemoteLazyInputStreamStorage::Get()->HasStream(mID);
}

}  // namespace mozilla
