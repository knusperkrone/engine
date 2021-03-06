// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/content_handler/session_connection.h"
#include "lib/ui/scenic/fidl_helpers.h"

namespace flutter_runner {

SessionConnection::SessionConnection(ui::ScenicPtr scenic,
                                     zx::eventpair import_token)
    : session_(scenic.get()),
      root_node_(&session_),
      surface_producer_(std::make_unique<VulkanSurfaceProducer>(&session_)),
      scene_update_context_(&session_, surface_producer_.get()) {
  ASSERT_IS_GPU_THREAD;

  session_.set_error_handler(
      std::bind(&SessionConnection::OnSessionError, this));
  session_.set_event_handler(std::bind(&SessionConnection::OnSessionEvents,
                                       this, std::placeholders::_1));

  root_node_.Bind(std::move(import_token));
  root_node_.SetEventMask(ui::gfx::kMetricsEventMask);
  session_.Present(0, [](ui::PresentationInfoPtr info) {});

  present_callback_ =
      std::bind(&SessionConnection::OnPresent, this, std::placeholders::_1);
}

SessionConnection::~SessionConnection() {
  ASSERT_IS_GPU_THREAD;
}

void SessionConnection::OnSessionError() {
  ASSERT_IS_GPU_THREAD;
  // TODO: Not this.
  FXL_CHECK(false) << "Session connection was terminated.";
}

void SessionConnection::OnSessionEvents(f1dl::Array<ui::EventPtr> events) {
  ui::gfx::MetricsPtr new_metrics;
  for (const auto& event : *events) {
    if (event->is_gfx() && event->get_gfx()->is_metrics() &&
        event->get_gfx()->get_metrics()->node_id == root_node_.id()) {
      new_metrics = std::move(event->get_gfx()->get_metrics()->metrics);
    }
  }
  if (!new_metrics)
    return;

  scene_update_context_.set_metrics(std::move(new_metrics));

  if (metrics_changed_callback_)
    metrics_changed_callback_();
}

void SessionConnection::Present(flow::CompositorContext::ScopedFrame& frame,
                                fxl::Closure on_present_callback) {
  ASSERT_IS_GPU_THREAD;
  FXL_DCHECK(pending_on_present_callback_ == nullptr);
  FXL_DCHECK(on_present_callback != nullptr);
  pending_on_present_callback_ = on_present_callback;

  // Flush all session ops. Paint tasks have not yet executed but those are
  // fenced. The compositor can start processing ops while we finalize paint
  // tasks.
  session_.Present(0,                 // presentation_time. Placeholder for now.
                   present_callback_  // callback
  );

  // Execute paint tasks and signal fences.
  auto surfaces_to_submit = scene_update_context_.ExecutePaintTasks(frame);

  // Tell the surface producer that a present has occurred so it can perform
  // book-keeping on buffer caches.
  surface_producer_->OnSurfacesPresented(std::move(surfaces_to_submit));

  // Prepare for the next frame.
  EnqueueClearCommands();
}

void SessionConnection::OnPresent(ui::PresentationInfoPtr info) {
  ASSERT_IS_GPU_THREAD;
  auto callback = pending_on_present_callback_;
  pending_on_present_callback_ = nullptr;
  callback();
}

void SessionConnection::EnqueueClearCommands() {
  ASSERT_IS_GPU_THREAD;
  // We are going to be sending down a fresh node hierarchy every frame. So just
  // enqueue a detach command on the imported root node.
  session_.Enqueue(scenic_lib::NewDetachChildrenCommand(root_node_.id()));
}

}  // namespace flutter_runner
