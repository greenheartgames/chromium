// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/download/download_resource_throttle.h"

#include "base/bind.h"
#include "chrome/browser/download/download_util.h"
#include "content/public/browser/resource_controller.h"

DownloadResourceThrottle::DownloadResourceThrottle(
    DownloadRequestLimiter* limiter,
    int render_process_id,
    int render_view_id,
    int request_id,
    const std::string& request_method)
    : querying_limiter_(true),
      request_allowed_(false),
      request_deferred_(false) {
  limiter->CanDownloadOnIOThread(
      render_process_id,
      render_view_id,
      request_id,
      request_method,
      base::Bind(&DownloadResourceThrottle::ContinueDownload,
                 AsWeakPtr()));
}

DownloadResourceThrottle::~DownloadResourceThrottle() {
}

void DownloadResourceThrottle::WillStartRequest(bool* defer) {
  WillDownload(defer);
}

void DownloadResourceThrottle::WillRedirectRequest(const GURL& new_url,
                                                   bool* defer) {
  WillDownload(defer);
}

void DownloadResourceThrottle::WillProcessResponse(bool* defer) {
  WillDownload(defer);
}

void DownloadResourceThrottle::WillDownload(bool* defer) {
  DCHECK(!request_deferred_);

  // Defer the download until we have the DownloadRequestLimiter result.
  if (querying_limiter_) {
    request_deferred_ = true;
    *defer = true;
    return;
  }

  if (!request_allowed_)
    controller()->Cancel();
}

void DownloadResourceThrottle::ContinueDownload(bool allow) {
  querying_limiter_ = false;
  request_allowed_ = allow;

  if (allow) {
    // Presumes all downloads initiated by navigation use this throttle and
    // nothing else does.
    download_util::RecordDownloadSource(download_util::INITIATED_BY_NAVIGATION);
  } else {
    download_util::RecordDownloadCount(download_util::BLOCKED_BY_THROTTLING);
  }

  if (request_deferred_) {
    request_deferred_ = false;
    if (allow) {
      controller()->Resume();
    } else {
      controller()->Cancel();
    }
  }
}
