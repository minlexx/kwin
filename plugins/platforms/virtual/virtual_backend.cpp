/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2015 Martin Gräßlin <mgraesslin@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
#include "virtual_backend.h"
#include "virtual_output.h"
#include "scene_qpainter_virtual_backend.h"
#include "screens_virtual.h"
#include "wayland_server.h"
#include "egl_gbm_backend.h"
// Qt
#include <QTemporaryDir>
// KWayland
#include <KWayland/Server/seat_interface.h>
// system
#include <fcntl.h>
#include <unistd.h>
#include <config-kwin.h>

namespace KWin
{

VirtualBackend::VirtualBackend(QObject *parent)
    : Platform(parent)
{
    if (qEnvironmentVariableIsSet("KWIN_WAYLAND_VIRTUAL_SCREENSHOTS")) {
        m_screenshotDir.reset(new QTemporaryDir);
        if (!m_screenshotDir->isValid()) {
            m_screenshotDir.reset();
        }
        if (!m_screenshotDir.isNull()) {
            qDebug() << "Screenshots saved to: " << m_screenshotDir->path();
        }
    }
    setSupportsPointerWarping(true);
    setSupportsGammaControl(true);
}

VirtualBackend::~VirtualBackend()
{
}

void VirtualBackend::init()
{
    /*
     * Some tests currently expect one output present at start,
     * others set them explicitly.
     *
     * TODO: rewrite all tests to explicitly set the outputs.
     */
    if (!m_outputs.size()) {
        VirtualOutput *dummyOutput = new VirtualOutput(this);
        dummyOutput->init(QPoint(0, 0), initialWindowSize());
        m_outputs << dummyOutput ;
        m_enabledOutputs << dummyOutput ;
    }

    setSoftWareCursor(true);
    setReady(true);
    waylandServer()->seat()->setHasPointer(true);
    waylandServer()->seat()->setHasKeyboard(true);
    waylandServer()->seat()->setHasTouch(true);

    emit screensQueried();
}

QString VirtualBackend::screenshotDirPath() const
{
    if (m_screenshotDir.isNull()) {
        return QString();
    }
    return m_screenshotDir->path();
}

Screens *VirtualBackend::createScreens(QObject *parent)
{
    return new VirtualScreens(this, parent);
}

QPainterBackend *VirtualBackend::createQPainterBackend()
{
    return new VirtualQPainterBackend(this);
}

OpenGLBackend *VirtualBackend::createOpenGLBackend()
{
    return new EglGbmBackend(this);
}

Outputs VirtualBackend::outputs() const
{
    return m_outputs;
}

Outputs VirtualBackend::enabledOutputs() const
{
    return m_enabledOutputs;
}

bool VirtualBackend::canLoad()
{
    // our init() never fails
    return true;
}

void VirtualBackend::setVirtualOutputs(int count, QVector<QRect> geometries, QVector<int> scales)
{
    Q_ASSERT(geometries.size() == 0 || geometries.size() == count);
    Q_ASSERT(scales.size() == 0 || scales.size() == count);

    bool countChanged = m_outputs.size() != count;
    qDeleteAll(m_outputs.begin(), m_outputs.end());
    m_outputs.resize(count);
    m_enabledOutputs.resize(count);

    int sumWidth = 0;
    for (int i = 0; i < count; i++) {
        VirtualOutput *vo = new VirtualOutput(this);
        if (geometries.size()) {
            const QRect geo = geometries.at(i);
            vo->init(geo.topLeft(), geo.size());
        } else {
            vo->init(QPoint(sumWidth, 0), initialWindowSize());
            sumWidth += initialWindowSize().width();
        }
        if (scales.size()) {
            vo->setScale(scales.at(i));
        }
        m_outputs[i] = m_enabledOutputs[i] = vo;
    }

    emit virtualOutputsSet(countChanged);
}

}
