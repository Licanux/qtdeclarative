/****************************************************************************
**
** Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/
**
** This file is part of the test suite of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/
#include <qtest.h>

#include <QtQml/qqmlengine.h>
#include <QtQml/qqmlcomponent.h>
#include <private/qquickrectangle_p.h>

#include "../../shared/util.h"

class tst_qquickrectangle : public QQmlDataTest
{
    Q_OBJECT
public:
    tst_qquickrectangle();

private slots:
    void gradient();

private:
    QQmlEngine engine;
};

tst_qquickrectangle::tst_qquickrectangle()
{
}

void tst_qquickrectangle::gradient()
{
    QQmlComponent component(&engine, testFileUrl("gradient.qml"));
    QQuickRectangle *rect = qobject_cast<QQuickRectangle*>(component.create());
    QVERIFY(rect);

    QQuickGradient *grad = rect->gradient();
    QVERIFY(grad);

    QQmlListProperty<QQuickGradientStop> stops = grad->stops();
    QCOMPARE(stops.count(&stops), 2);
    QCOMPARE(stops.at(&stops, 0)->position(), 0.0);
    QCOMPARE(stops.at(&stops, 0)->color(), QColor("gray"));
    QCOMPARE(stops.at(&stops, 1)->position(), 1.0);
    QCOMPARE(stops.at(&stops, 1)->color(), QColor("white"));

    QMetaObject::invokeMethod(rect, "resetGradient");

    grad = rect->gradient();
    QVERIFY(!grad);

    delete rect;
}


QTEST_MAIN(tst_qquickrectangle)

#include "tst_qquickrectangle.moc"
