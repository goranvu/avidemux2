
/***************************************************************************
Custom slider
***************************************************************************/

/***************************************************************************
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
***************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include "ADM_inttype.h"
#include <QPainter>
#include <QColor>
#include <QPen>
#include <QSlider>
#include <QWheelEvent>
#include <QStyle>
#include <QStyleOptionSlider>
#include "ADM_mwNavSlider.h"
#include "ADM_assert.h"
#include "ADM_default.h"
/**
    \fn ADM_QSlider
*/
ADM_mwNavSlider::ADM_mwNavSlider(QWidget *parent) : ADM_flyNavSlider(parent)
{
    segments = NULL;
    numOfSegments = 0;
}
/**
    \fn dtor
*/
ADM_mwNavSlider::~ADM_mwNavSlider()
{
    if (segments)
        delete [] segments;
    segments = NULL;
}
/**
    \fn drawCutPoints
*/
void ADM_mwNavSlider::drawCutPoints(void)
{
    if (!segments || numOfSegments < 2 || !totalDuration)
        return;

    int pos, prevpos;
    prevpos = -1;
    QPainter painter(this);
    for (uint32_t i=1; i<numOfSegments; i++)
    {
        if (segments[i] == ADM_NO_PTS)
            continue;
        pos = (int)(((double)segments[i] * width()) / (double)totalDuration);
        if (pos < 1) pos = 1;
        if (pos > width() - 1) pos = width() - 1;
        if (pos == prevpos)
            painter.setPen(Qt::darkMagenta);
        else
            painter.setPen(Qt::red);
        prevpos = pos;

        if(layoutDirection() == Qt::LeftToRight)
            painter.drawLine(pos, 1, pos, height() - 3);
        else
            painter.drawLine(width() - pos, 1, width() - pos, height() - 3);
    }
}

void ADM_mwNavSlider::drawChapterPoints(void)
{
    if (!totalDuration || chapterMarkers.empty())
        return;

    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const int usable = qMax(0, groove.width() - handle.width());
    const int start = groove.left() + handle.width() / 2;

    QPainter painter(this);
    painter.setPen(QPen(QColor(255, 163, 35), 2));
    for (uint64_t timestamp : chapterMarkers)
    {
        if (timestamp > totalDuration)
            continue;
        const double fraction = static_cast<double>(timestamp) / static_cast<double>(totalDuration);
        int x = start + static_cast<int>(fraction * usable + 0.5);
        if (layoutDirection() == Qt::RightToLeft)
            x = width() - 1 - x;
        painter.drawLine(x, height() - 9, x, height() - 2);
    }
}

void ADM_mwNavSlider::setChapterMarkers(const std::vector<uint64_t> &markersUs)
{
    chapterMarkers = markersUs;
    update();
}

/**
    \fn paintEvent
*/
void ADM_mwNavSlider::paintEvent(QPaintEvent *event)
{
    drawCutPoints();

    QSlider::paintEvent(event);

    drawSelection();
    drawChapterPoints();
}
/**
    \fn setMarkerA
*/
void ADM_mwNavSlider::setMarkerA(uint64_t frameIndex)
{
    setMarkers(frameIndex, markerBTime);
}
/**
    \fn setMarkerB
*/
void ADM_mwNavSlider::setMarkerB(uint64_t frameIndex)
{
    setMarkers(markerATime, frameIndex);
}
/**
    \fn setMarkers
*/
void ADM_mwNavSlider::setMarkers(uint64_t frameIndexA, uint64_t frameIndexB)
{
    if (frameIndexA > totalDuration)
        printf("[ADM_QSlider] Marker A is out of bounds (%" PRIu64", %" PRIu64")\n", markerATime, totalDuration);
    else if (frameIndexB > totalDuration)
        printf("[ADM_QSlider] Marker B is out of bounds (%" PRIu64", %" PRIu64")\n", markerBTime, totalDuration);
    else
    {
        markerATime = frameIndexA;
        markerBTime = frameIndexB;

        repaint();
    }
}
/**
    \fn setTotalDuration
*/
void ADM_mwNavSlider::setTotalDuration(uint64_t duration)
{
    totalDuration = duration;
    repaint();
}
/**
    \fn setSegments
*/
void ADM_mwNavSlider::setSegments(uint32_t numOfSegs, uint64_t * segPts)
{
    if (segments)
        delete [] segments;
    segments = new uint64_t[numOfSegs];
    for (uint32_t i=0; i<numOfSegs; i++)
        segments[i] = segPts[i];
    numOfSegments = numOfSegs;
}
/*
    \fn wheelEvent
 */
void ADM_mwNavSlider::wheelEvent(QWheelEvent *e)
{
#if QT_VERSION < QT_VERSION_CHECK(5,0,0)
    //printf("Wheel : %d\n",e->delta());
    emit sliderAction(e->delta());
#else
    emit sliderAction(e->angleDelta().ry());
#endif
}
//EOF
