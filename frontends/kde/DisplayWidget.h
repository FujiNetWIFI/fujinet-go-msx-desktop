/*
 * The emulator display on the GPU: a QRhiWidget that keeps one 640x480
 * texture and draws one textured quad, only when the session has published
 * a new frame (see FrameFeed). Qt picks Vulkan, Metal, D3D or OpenGL for it;
 * the scaling and letterboxing happen on the GPU.
 *
 * If no GPU device can be had it emits renderFailed() and the main window
 * swaps in the software SoftDisplayWidget.
 *
 * Key events are translated by KeyForward (shared with the software display).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRhiWidget>
#include <memory>
#include <rhi/qrhi.h>
#include <vector>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "msxsession.h"

class DisplayWidget : public QRhiWidget, public DisplayView {
    Q_OBJECT
public:
    explicit DisplayWidget(msxsession *session, QWidget *parent = nullptr);
    ~DisplayWidget() override;

    QWidget *widget() override { return this; }

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool event(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    /* Tab is a key like any other here (it is an MSX key): it reaches the
     * key handler instead of moving the focus. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();

    msxsession *m_session;
    FrameFeed *m_feed;
    /* Tightly packed XRGB8888, filled by copy_frame (the upload source). */
    std::vector<uint32_t> m_frame;
    uint64_t m_serial = 0;
    bool m_have = false;       /* m_frame holds a real frame */
    bool m_dirty = false;      /* m_frame holds a frame not yet uploaded */

    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
};
