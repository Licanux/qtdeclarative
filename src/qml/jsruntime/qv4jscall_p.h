/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/
#ifndef QV4JSCALL_H
#define QV4JSCALL_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include "qv4object_p.h"
#include "qv4function_p.h"
#include "qv4functionobject_p.h"
#include "qv4context_p.h"
#include "qv4scopedvalue_p.h"

QT_BEGIN_NAMESPACE

namespace QV4 {

struct JSCall {
    JSCall(const Scope &scope, std::nullptr_t, int argc = 0)
    {
        int size = int(offsetof(QV4::CallData, args)/sizeof(QV4::Value)) + qMax(argc , int(QV4::Global::ReservedArgumentCount));
        ptr = reinterpret_cast<CallData *>(scope.alloc(size));
        ptr->tag = quint32(QV4::Value::ValueTypeInternal::Integer);
        ptr->argc = argc;
    }
    JSCall(const Scope &scope, const FunctionObject *function, int argc = 0)
    {
        int size = int(offsetof(QV4::CallData, args)/sizeof(QV4::Value)) + qMax(argc , int(QV4::Global::ReservedArgumentCount));
        ptr = reinterpret_cast<CallData *>(scope.alloc(size));
        ptr->tag = quint32(QV4::Value::ValueTypeInternal::Integer);
        ptr->argc = argc;
        ptr->function = *function;
    }
    JSCall(const Scope &scope, Heap::FunctionObject *function, int argc = 0)
    {
        int size = int(offsetof(QV4::CallData, args)/sizeof(QV4::Value)) + qMax(argc , int(QV4::Global::ReservedArgumentCount));
        ptr = reinterpret_cast<CallData *>(scope.alloc(size));
        ptr->tag = quint32(QV4::Value::ValueTypeInternal::Integer);
        ptr->argc = argc;
        ptr->function = function;
    }

    CallData *operator->() {
        return ptr;
    }

    operator CallData *() const {
        return ptr;
    }

    ReturnedValue call() const {
        return static_cast<FunctionObject &>(ptr->function).call(ptr);
    }

    ReturnedValue callAsConstructor() const {
        return static_cast<FunctionObject &>(ptr->function).construct(ptr);
    }

    CallData *ptr;
};

struct ScopedStackFrame {
    Scope &scope;
    CppStackFrame frame;

    ScopedStackFrame(Scope &scope, Heap::ExecutionContext *context)
        : scope(scope)
    {
        frame.parent = scope.engine->currentStackFrame;
        if (!context)
            return;
        frame.jsFrame = reinterpret_cast<CallData *>(scope.alloc(sizeof(CallData)/sizeof(Value)));
        frame.jsFrame->context = context;
        frame.v4Function = frame.parent ? frame.parent->v4Function : 0;
        scope.engine->currentStackFrame = &frame;
    }
    ~ScopedStackFrame() {
        scope.engine->currentStackFrame = frame.parent;
    }
};

}

#endif