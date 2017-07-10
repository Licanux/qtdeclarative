/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
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

#include "qv4vme_moth_p.h"
#include "qv4instr_moth_p.h"

#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>

#include <private/qv4value_p.h>
#include <private/qv4debugging_p.h>
#include <private/qv4function_p.h>
#include <private/qv4functionobject_p.h>
#include <private/qv4math_p.h>
#include <private/qv4scopedvalue_p.h>
#include <private/qv4lookup_p.h>
#include <private/qv4string_p.h>
#include <iostream>

#include "qv4alloca_p.h"

#undef DO_TRACE_INSTR // define to enable instruction tracing

#ifdef DO_TRACE_INSTR
#  define TRACE_INSTR(I) qDebug("executing a %s\n", #I);
#  define TRACE(n, str, ...) { char buf[4096]; snprintf(buf, 4096, str, __VA_ARGS__); qDebug("    %s : %s", #n, buf); }
#else
#  define TRACE_INSTR(I)
#  define TRACE(n, str, ...)
#endif // DO_TRACE_INSTR

extern "C" {

// This is the interface to Qt Creator's (new) QML debugger.

/*! \internal
    \since 5.5

    This function is called uncondionally from VME::run().

    An attached debugger can set a breakpoint here to
    intercept calls to VME::run().
 */

Q_QML_EXPORT void qt_v4ResolvePendingBreakpointsHook()
{
}

/*! \internal
    \since 5.5

    This function is called when a QML interpreter breakpoint
    is hit.

    An attached debugger can set a breakpoint here.
*/
Q_QML_EXPORT void qt_v4TriggeredBreakpointHook()
{
}

/*! \internal
    \since 5.5

    The main entry point into "Native Mixed" Debugging.

    Commands are passed as UTF-8 encoded JSON data.
    The data has two compulsory fields:
    \list
    \li \c version: Version of the protocol (currently 1)
    \li \c command: Name of the command
    \endlist

    Depending on \c command, more fields can be present.

    Error is indicated by negative return values,
    success by non-negative return values.

    \c protocolVersion:
    Returns version of implemented protocol.

    \c insertBreakpoint:
    Sets a breakpoint on a given file and line.
    \list
    \li \c fullName: Name of the QML/JS file
    \li \c lineNumber: Line number in the file
    \li \c condition: Breakpoint condition
    \endlist
    Returns a unique positive number as handle.

    \c removeBreakpoint:
    Removes a breakpoint from a given file and line.
    \list
    \li \c fullName: Name of the QML/JS file
    \li \c lineNumber: Line number in the file
    \li \c condition: Breakpoint condition
    \endlist
    Returns zero on success, a negative number on failure.

    \c prepareStep:
    Puts the interpreter in stepping mode.
    Returns zero.

*/
Q_QML_EXPORT int qt_v4DebuggerHook(const char *json);


} // extern "C"

#ifndef QT_NO_QML_DEBUGGER
static int qt_v4BreakpointCount = 0;
static bool qt_v4IsDebugging = true;
static bool qt_v4IsStepping = false;

class Breakpoint
{
public:
    Breakpoint() : bpNumber(0), lineNumber(-1) {}

    bool matches(const QString &file, int line) const
    {
        return fullName == file && lineNumber == line;
    }

    int bpNumber;
    int lineNumber;
    QString fullName;      // e.g. /opt/project/main.qml
    QString engineName;    // e.g. qrc:/main.qml
    QString condition;     // optional
};

static QVector<Breakpoint> qt_v4Breakpoints;
static Breakpoint qt_v4LastStop;

static QV4::Function *qt_v4ExtractFunction(QV4::ExecutionContext *context)
{
    if (QV4::Function *function = context->getFunction())
        return function;
    else
        return context->engine()->globalCode;
}

static void qt_v4TriggerBreakpoint(const Breakpoint &bp, QV4::Function *function)
{
    qt_v4LastStop = bp;

    // Set up some auxiliary data for informational purpose.
    // This is not part of the protocol.
    QV4::Heap::String *functionName = function->name();
    QByteArray functionNameUtf8;
    if (functionName)
        functionNameUtf8 = functionName->toQString().toUtf8();

    qt_v4TriggeredBreakpointHook(); // Trigger Breakpoint.
}

int qt_v4DebuggerHook(const char *json)
{
    const int ProtocolVersion = 1;

    enum {
        Success = 0,
        WrongProtocol,
        NoSuchCommand,
        NoSuchBreakpoint
    };

    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject ob = doc.object();
    QByteArray command = ob.value(QLatin1String("command")).toString().toUtf8();

    if (command == "protocolVersion") {
        return ProtocolVersion; // Version number.
    }

    int version = ob.value(QLatin1Literal("version")).toString().toInt();
    if (version != ProtocolVersion) {
        return -WrongProtocol;
    }

    if (command == "insertBreakpoint") {
        Breakpoint bp;
        bp.bpNumber = ++qt_v4BreakpointCount;
        bp.lineNumber = ob.value(QLatin1String("lineNumber")).toString().toInt();
        bp.engineName = ob.value(QLatin1String("engineName")).toString();
        bp.fullName = ob.value(QLatin1String("fullName")).toString();
        bp.condition = ob.value(QLatin1String("condition")).toString();
        qt_v4Breakpoints.append(bp);
        return bp.bpNumber;
    }

    if (command == "removeBreakpoint") {
        int lineNumber = ob.value(QLatin1String("lineNumber")).toString().toInt();
        QString fullName = ob.value(QLatin1String("fullName")).toString();
        if (qt_v4Breakpoints.last().matches(fullName, lineNumber)) {
            qt_v4Breakpoints.removeLast();
            return Success;
        }
        for (int i = 0; i + 1 < qt_v4Breakpoints.size(); ++i) {
            if (qt_v4Breakpoints.at(i).matches(fullName, lineNumber)) {
                qt_v4Breakpoints[i] = qt_v4Breakpoints.takeLast();
                return Success; // Ok.
            }
        }
        return -NoSuchBreakpoint; // Failure
    }

    if (command == "prepareStep") {
        qt_v4IsStepping = true;
        return Success; // Ok.
    }


    return -NoSuchCommand; // Failure.
}

Q_NEVER_INLINE static void qt_v4CheckForBreak(QV4::ExecutionContext *context)
{
    if (!qt_v4IsStepping && !qt_v4Breakpoints.size())
        return;

    const int lineNumber = context->d()->lineNumber;
    QV4::Function *function = qt_v4ExtractFunction(context);
    QString engineName = function->sourceFile();

    if (engineName.isEmpty())
        return;

    if (qt_v4IsStepping) {
        if (qt_v4LastStop.lineNumber != lineNumber
                || qt_v4LastStop.engineName != engineName) {
            qt_v4IsStepping = false;
            Breakpoint bp;
            bp.bpNumber = 0;
            bp.lineNumber = lineNumber;
            bp.engineName = engineName;
            qt_v4TriggerBreakpoint(bp, function);
            return;
        }
    }

    for (int i = qt_v4Breakpoints.size(); --i >= 0; ) {
        const Breakpoint &bp = qt_v4Breakpoints.at(i);
        if (bp.lineNumber != lineNumber)
            continue;
        if (bp.engineName != engineName)
            continue;

        qt_v4TriggerBreakpoint(bp, function);
    }
}

Q_NEVER_INLINE static void debug_slowPath(const QV4::Moth::Instr::instr_debug &instr,
                                          QV4::ExecutionEngine *engine)
{
    engine->current->lineNumber = instr.lineNumber;
    QV4::Debugging::Debugger *debugger = engine->debugger();
    if (debugger && debugger->pauseAtNextOpportunity())
        debugger->maybeBreakAtInstruction();
    if (qt_v4IsDebugging)
        qt_v4CheckForBreak(engine->currentContext);
}

#endif // QT_NO_QML_DEBUGGER
// End of debugger interface

using namespace QV4;
using namespace QV4::Moth;

#define MOTH_BEGIN_INSTR_COMMON(I) { \
    const InstrMeta<(int)Instr::I>::DataType &instr = InstrMeta<(int)Instr::I>::data(*genericInstr); \
    code += InstrMeta<(int)Instr::I>::Size; \
    Q_UNUSED(instr); \
    TRACE_INSTR(I)

#ifdef MOTH_THREADED_INTERPRETER

#  define MOTH_BEGIN_INSTR(I) op_##I: \
    MOTH_BEGIN_INSTR_COMMON(I)

#  define MOTH_END_INSTR(I) } \
    genericInstr = reinterpret_cast<const Instr *>(code); \
    goto *jumpTable[genericInstr->common.instructionType]; \

#else

#  define MOTH_BEGIN_INSTR(I) \
    case Instr::I: \
    MOTH_BEGIN_INSTR_COMMON(I)

#  define MOTH_END_INSTR(I) } \
    continue;

#endif

#ifdef DO_TRACE_INSTR
Param traceParam(const Param &param)
{
    if (param.isConstant()) {
        qDebug("    constant\n");
    } else if (param.isArgument()) {
        qDebug("    argument %d@%d\n", param.index, param.scope);
    } else if (param.isLocal()) {
        qDebug("    local %d\n", param.index);
    } else if (param.isTemp()) {
        qDebug("    temp %d\n", param.index);
    } else if (param.isScopedLocal()) {
        qDebug("    temp %d@%d\n", param.index, param.scope);
    } else {
        Q_ASSERT(!"INVALID");
    }
    return param;
}
# define VALUE(param) (*VALUEPTR(param))
# define VALUEPTR(param) (scopes[traceParam(param).scope].values + param.index)
#else
# define VALUE(param) (*VALUEPTR(param))
# define VALUEPTR(param) (scopes[param.scope].values + param.index)
#endif

// ### add write barrier here
#define STOREVALUE(param, value) { \
    QV4::ReturnedValue tmp = (value); \
    if (engine->hasException) \
        goto catchException; \
    if (Q_LIKELY(!engine->writeBarrierActive || !scopes[param.scope].base)) { \
        VALUE(param) = tmp; \
    } else { \
        QV4::WriteBarrier::write(engine, scopes[param.scope].base, VALUEPTR(param), QV4::Value::fromReturnedValue(tmp)); \
    } \
}

// qv4scopedvalue_p.h also defines a CHECK_EXCEPTION macro
#ifdef CHECK_EXCEPTION
#undef CHECK_EXCEPTION
#endif
#define CHECK_EXCEPTION \
    if (engine->hasException) \
        goto catchException

QV4::ReturnedValue VME::exec(ExecutionEngine *engine, const uchar *code)
{
#ifdef DO_TRACE_INSTR
    qDebug("Starting VME with context=%p and code=%p", context, code);
#endif // DO_TRACE_INSTR

    qt_v4ResolvePendingBreakpointsHook();

#ifdef MOTH_THREADED_INTERPRETER
#define MOTH_INSTR_ADDR(I, FMT) &&op_##I,
    static void *jumpTable[] = {
        FOR_EACH_MOTH_INSTR(MOTH_INSTR_ADDR)
    };
#undef MOTH_INSTR_ADDR
#endif

    QV4::ReturnedValue returnValue = Encode::undefined();
    QV4::Value *stack = 0;
    unsigned stackSize = 0;

    const uchar *exceptionHandler = 0;

    QV4::Scope scope(engine);
    engine->current->lineNumber = -1;

#ifdef DO_TRACE_INSTR
    qDebug("Starting VME with context=%p and code=%p", context, code);
#endif // DO_TRACE_INSTR

    // setup lookup scopes
    int scopeDepth = 0;
    {
        QV4::Heap::ExecutionContext *scope = engine->current;
        while (scope) {
            ++scopeDepth;
            scope = scope->outer;
        }
    }

    struct Scopes {
        QV4::Value *values;
        QV4::Heap::Base *base; // non 0 if a write barrier is required
    };
    Q_ALLOCA_VAR(Scopes, scopes, sizeof(Scopes)*(2 + 2*scopeDepth));
    {
        scopes[0] = { const_cast<QV4::Value *>(static_cast<CompiledData::CompilationUnit*>(engine->current->compilationUnit)->constants), 0 };
        // stack gets setup in push instruction
        scopes[1] = { 0, 0 };
        QV4::Heap::ExecutionContext *scope = engine->current;
        int i = 0;
        while (scope) {
            if (scope->type == QV4::Heap::ExecutionContext::Type_SimpleCallContext) {
                QV4::Heap::CallContext *cc = static_cast<QV4::Heap::CallContext *>(scope);
                scopes[2*i + 2] = { cc->callData->args, 0 };
                scopes[2*i + 3] = { 0, 0 };
            } else if (scope->type == QV4::Heap::ExecutionContext::Type_CallContext) {
                QV4::Heap::CallContext *cc = static_cast<QV4::Heap::CallContext *>(scope);
                scopes[2*i + 2] = { cc->callData->args, cc };
                scopes[2*i + 3] = { cc->locals.values, cc };
            } else {
                scopes[2*i + 2] = { 0, 0 };
                scopes[2*i + 3] = { 0, 0 };
            }
            ++i;
            scope = scope->outer;
        }
    }

    if (QV4::Debugging::Debugger *debugger = engine->debugger())
        debugger->enteringFunction();

    for (;;) {
        const Instr *genericInstr = reinterpret_cast<const Instr *>(code);
#ifdef MOTH_THREADED_INTERPRETER
        goto *jumpTable[genericInstr->common.instructionType];
#else
        switch (genericInstr->common.instructionType) {
#endif

    MOTH_BEGIN_INSTR(Move)
        VALUE(instr.result) = VALUE(instr.source);
    MOTH_END_INSTR(Move)

    MOTH_BEGIN_INSTR(LoadRuntimeString)
//        TRACE(value, "%s", instr.value.toString(context)->toQString().toUtf8().constData());
        VALUE(instr.result) = engine->current->compilationUnit->runtimeStrings[instr.stringId];
    MOTH_END_INSTR(LoadRuntimeString)

    MOTH_BEGIN_INSTR(LoadRegExp)
//        TRACE(value, "%s", instr.value.toString(context)->toQString().toUtf8().constData());
        VALUE(instr.result) = static_cast<CompiledData::CompilationUnit*>(engine->current->compilationUnit)->runtimeRegularExpressions[instr.regExpId];
    MOTH_END_INSTR(LoadRegExp)

    MOTH_BEGIN_INSTR(LoadClosure)
        STOREVALUE(instr.result, Runtime::method_closure(engine, instr.value));
    MOTH_END_INSTR(LoadClosure)

    MOTH_BEGIN_INSTR(LoadName)
        TRACE(inline, "property name = %s", runtimeStrings[instr.name]->toQString().toUtf8().constData());
        STOREVALUE(instr.result, Runtime::method_getActivationProperty(engine, instr.name));
    MOTH_END_INSTR(LoadName)

    MOTH_BEGIN_INSTR(GetGlobalLookup)
        QV4::Lookup *l = engine->current->lookups + instr.index;
        STOREVALUE(instr.result, l->globalGetter(l, engine));
    MOTH_END_INSTR(GetGlobalLookup)

    MOTH_BEGIN_INSTR(StoreName)
        TRACE(inline, "property name = %s", runtimeStrings[instr.name]->toQString().toUtf8().constData());
        Runtime::method_setActivationProperty(engine, instr.name, VALUE(instr.source));
        CHECK_EXCEPTION;
    MOTH_END_INSTR(StoreName)

    MOTH_BEGIN_INSTR(LoadElement)
        STOREVALUE(instr.result, Runtime::method_getElement(engine, VALUE(instr.base), VALUE(instr.index)));
    MOTH_END_INSTR(LoadElement)

    MOTH_BEGIN_INSTR(LoadElementLookup)
        QV4::Lookup *l = engine->current->lookups + instr.lookup;
        STOREVALUE(instr.result, l->indexedGetter(l, engine, VALUE(instr.base), VALUE(instr.index)));
    MOTH_END_INSTR(LoadElementLookup)

    MOTH_BEGIN_INSTR(StoreElement)
        Runtime::method_setElement(engine, VALUE(instr.base), VALUE(instr.index), VALUE(instr.source));
        CHECK_EXCEPTION;
    MOTH_END_INSTR(StoreElement)

    MOTH_BEGIN_INSTR(StoreElementLookup)
        QV4::Lookup *l = engine->current->lookups + instr.lookup;
        l->indexedSetter(l, engine, VALUE(instr.base), VALUE(instr.index), VALUE(instr.source));
        CHECK_EXCEPTION;
    MOTH_END_INSTR(StoreElementLookup)

    MOTH_BEGIN_INSTR(LoadProperty)
        STOREVALUE(instr.result, Runtime::method_getProperty(engine, VALUE(instr.base), instr.name));
    MOTH_END_INSTR(LoadProperty)

    MOTH_BEGIN_INSTR(GetLookup)
        QV4::Lookup *l = engine->current->lookups + instr.index;
        STOREVALUE(instr.result, l->getter(l, engine, VALUE(instr.base)));
    MOTH_END_INSTR(GetLookup)

    MOTH_BEGIN_INSTR(StoreProperty)
        Runtime::method_setProperty(engine, VALUE(instr.base), instr.name, VALUE(instr.source));
        CHECK_EXCEPTION;
    MOTH_END_INSTR(StoreProperty)

    MOTH_BEGIN_INSTR(SetLookup)
        QV4::Lookup *l = engine->current->lookups + instr.index;
        l->setter(l, engine, VALUE(instr.base), VALUE(instr.source));
        CHECK_EXCEPTION;
    MOTH_END_INSTR(SetLookup)

    MOTH_BEGIN_INSTR(StoreScopeObjectProperty)
        Runtime::method_setQmlScopeObjectProperty(engine, VALUE(instr.base), instr.propertyIndex, VALUE(instr.source));
        CHECK_EXCEPTION;
    MOTH_END_INSTR(StoreScopeObjectProperty)

    MOTH_BEGIN_INSTR(LoadScopeObjectProperty)
        STOREVALUE(instr.result, Runtime::method_getQmlScopeObjectProperty(engine, VALUE(instr.base), instr.propertyIndex, instr.captureRequired));
    MOTH_END_INSTR(LoadScopeObjectProperty)

    MOTH_BEGIN_INSTR(StoreContextObjectProperty)
        Runtime::method_setQmlContextObjectProperty(engine, VALUE(instr.base), instr.propertyIndex, VALUE(instr.source));
        CHECK_EXCEPTION;
    MOTH_END_INSTR(StoreContextObjectProperty)

    MOTH_BEGIN_INSTR(LoadContextObjectProperty)
        STOREVALUE(instr.result, Runtime::method_getQmlContextObjectProperty(engine, VALUE(instr.base), instr.propertyIndex, instr.captureRequired));
    MOTH_END_INSTR(LoadContextObjectProperty)

    MOTH_BEGIN_INSTR(LoadIdObject)
        STOREVALUE(instr.result, Runtime::method_getQmlIdObject(engine, VALUE(instr.base), instr.index));
    MOTH_END_INSTR(LoadIdObject)

    MOTH_BEGIN_INSTR(InitStackFrame)
        TRACE(inline, "stack size: %u", instr.value);
        stackSize = instr.value;
        stack = scope.alloc(stackSize);
        scopes[1].values = stack;
    MOTH_END_INSTR(InitStackFrame)

    MOTH_BEGIN_INSTR(CallValue)
        QV4::CallData *callData = reinterpret_cast<QV4::CallData *>(stack + instr.callData);
        STOREVALUE(instr.result, Runtime::method_callValue(engine, VALUE(instr.dest), callData));
        //### write barrier?
    MOTH_END_INSTR(CallValue)

    MOTH_BEGIN_INSTR(CallProperty)
        TRACE(property name, "%s, args=%u, argc=%u, this=%s", qPrintable(runtimeStrings[instr.name]->toQString()), instr.callData, instr.argc, (VALUE(instr.base)).toString(engine)->toQString().toUtf8().constData());
        QV4::CallData *callData = reinterpret_cast<QV4::CallData *>(stack + instr.callData);
        callData->thisObject = VALUE(instr.base);
        STOREVALUE(instr.result, Runtime::method_callProperty(engine, instr.name, callData));
        //### write barrier?
    MOTH_END_INSTR(CallProperty)

    MOTH_BEGIN_INSTR(CallPropertyLookup)
        Q_ASSERT(instr.callData + instr.argc + offsetof(QV4::CallData, args)/sizeof(QV4::Value) <= stackSize);
        QV4::CallData *callData = reinterpret_cast<QV4::CallData *>(stack + instr.callData);
        callData->tag = quint32(Value::ValueTypeInternal::Integer);
        callData->argc = instr.argc;
        callData->thisObject = VALUE(instr.base);
        STOREVALUE(instr.result, Runtime::method_callPropertyLookup(engine, instr.lookupIndex, callData));
    MOTH_END_INSTR(CallPropertyLookup)

    MOTH_BEGIN_INSTR(CallElement)
        QV4::CallData *callData = reinterpret_cast<QV4::CallData *>(stack + instr.callData);
        callData->thisObject = VALUE(instr.base);
        STOREVALUE(instr.result, Runtime::method_callElement(engine, VALUE(instr.index), callData));
        //### write barrier?
    MOTH_END_INSTR(CallElement)

    MOTH_BEGIN_INSTR(CallActivationProperty)
        QV4::CallData *callData = reinterpret_cast<QV4::CallData *>(stack + instr.callData);
        STOREVALUE(instr.result, Runtime::method_callActivationProperty(engine, instr.name, callData));
        //### write barrier?
    MOTH_END_INSTR(CallActivationProperty)

    MOTH_BEGIN_INSTR(CallGlobalLookup)
        QV4::CallData *callData = reinterpret_cast<QV4::CallData *>(stack + instr.callData);
        STOREVALUE(instr.result, Runtime::method_callGlobalLookup(engine, instr.index, callData));
        //### write barrier?
    MOTH_END_INSTR(CallGlobalLookup)

    MOTH_BEGIN_INSTR(SetExceptionHandler)
        exceptionHandler = instr.offset ? ((const uchar *)&instr.offset) + instr.offset : 0;
    MOTH_END_INSTR(SetExceptionHandler)

    MOTH_BEGIN_INSTR(CallBuiltinThrow)
        Runtime::method_throwException(engine, VALUE(instr.arg));
        CHECK_EXCEPTION;
    MOTH_END_INSTR(CallBuiltinThrow)

    MOTH_BEGIN_INSTR(GetException)
        *VALUEPTR(instr.result) = engine->hasException ? *engine->exceptionValue : Primitive::emptyValue();
        engine->hasException = false;
    MOTH_END_INSTR(HasException)

    MOTH_BEGIN_INSTR(SetException)
        *engine->exceptionValue = VALUE(instr.exception);
        engine->hasException = true;
    MOTH_END_INSTR(SetException)

    MOTH_BEGIN_INSTR(CallBuiltinUnwindException)
        STOREVALUE(instr.result, Runtime::method_unwindException(engine));
    MOTH_END_INSTR(CallBuiltinUnwindException)

    MOTH_BEGIN_INSTR(CallBuiltinPushCatchScope)
        Runtime::method_pushCatchScope(static_cast<QV4::NoThrowEngine*>(engine), instr.name);
    MOTH_END_INSTR(CallBuiltinPushCatchScope)

    MOTH_BEGIN_INSTR(CallBuiltinPushScope)
        Runtime::method_pushWithScope(VALUE(instr.arg), static_cast<QV4::NoThrowEngine*>(engine));
        CHECK_EXCEPTION;
    MOTH_END_INSTR(CallBuiltinPushScope)

    MOTH_BEGIN_INSTR(CallBuiltinPopScope)
        Runtime::method_popScope(static_cast<QV4::NoThrowEngine*>(engine));
    MOTH_END_INSTR(CallBuiltinPopScope)

    MOTH_BEGIN_INSTR(CallBuiltinForeachIteratorObject)
        STOREVALUE(instr.result, Runtime::method_foreachIterator(engine, VALUE(instr.arg)));
    MOTH_END_INSTR(CallBuiltinForeachIteratorObject)

    MOTH_BEGIN_INSTR(CallBuiltinForeachNextPropertyName)
        STOREVALUE(instr.result, Runtime::method_foreachNextPropertyName(VALUE(instr.arg)));
    MOTH_END_INSTR(CallBuiltinForeachNextPropertyName)

    MOTH_BEGIN_INSTR(CallBuiltinDeleteMember)
        STOREVALUE(instr.result, Runtime::method_deleteMember(engine, VALUE(instr.base), instr.member));
    MOTH_END_INSTR(CallBuiltinDeleteMember)

    MOTH_BEGIN_INSTR(CallBuiltinDeleteSubscript)
        STOREVALUE(instr.result, Runtime::method_deleteElement(engine, VALUE(instr.base), VALUE(instr.index)));
    MOTH_END_INSTR(CallBuiltinDeleteSubscript)

    MOTH_BEGIN_INSTR(CallBuiltinDeleteName)
        STOREVALUE(instr.result, Runtime::method_deleteName(engine, instr.name));
    MOTH_END_INSTR(CallBuiltinDeleteName)

    MOTH_BEGIN_INSTR(CallBuiltinTypeofName)
        STOREVALUE(instr.result, Runtime::method_typeofName(engine, instr.name));
    MOTH_END_INSTR(CallBuiltinTypeofName)

    MOTH_BEGIN_INSTR(CallBuiltinTypeofValue)
        STOREVALUE(instr.result, Runtime::method_typeofValue(engine, VALUE(instr.value)));
    MOTH_END_INSTR(CallBuiltinTypeofValue)

    MOTH_BEGIN_INSTR(CallBuiltinDeclareVar)
        Runtime::method_declareVar(engine, instr.isDeletable, instr.varName);
    MOTH_END_INSTR(CallBuiltinDeclareVar)

    MOTH_BEGIN_INSTR(CallBuiltinDefineArray)
        Q_ASSERT(instr.args + instr.argc <= stackSize);
        QV4::Value *args = stack + instr.args;
        STOREVALUE(instr.result, Runtime::method_arrayLiteral(engine, args, instr.argc));
    MOTH_END_INSTR(CallBuiltinDefineArray)

    MOTH_BEGIN_INSTR(CallBuiltinDefineObjectLiteral)
        QV4::Value *args = stack + instr.args;
    STOREVALUE(instr.result, Runtime::method_objectLiteral(engine, args, instr.internalClassId, instr.arrayValueCount, instr.arrayGetterSetterCountAndFlags));
    MOTH_END_INSTR(CallBuiltinDefineObjectLiteral)

    MOTH_BEGIN_INSTR(CallBuiltinSetupArgumentsObject)
        STOREVALUE(instr.result, Runtime::method_setupArgumentsObject(engine));
    MOTH_END_INSTR(CallBuiltinSetupArgumentsObject)

    MOTH_BEGIN_INSTR(CallBuiltinConvertThisToObject)
        Runtime::method_convertThisToObject(engine);
        CHECK_EXCEPTION;
    MOTH_END_INSTR(CallBuiltinConvertThisToObject)

    MOTH_BEGIN_INSTR(CreateValue)
        QV4::CallData *callData = reinterpret_cast<QV4::CallData *>(stack + instr.callData);
        STOREVALUE(instr.result, Runtime::method_constructValue(engine, VALUE(instr.func), callData));
        //### write barrier?
    MOTH_END_INSTR(CreateValue)

    MOTH_BEGIN_INSTR(CreateProperty)
        Q_ASSERT(instr.callData + instr.argc + offsetof(QV4::CallData, args)/sizeof(QV4::Value) <= stackSize);
        QV4::CallData *callData = reinterpret_cast<QV4::CallData *>(stack + instr.callData);
        callData->tag = quint32(Value::ValueTypeInternal::Integer);
        callData->argc = instr.argc;
        callData->thisObject = VALUE(instr.base);
        STOREVALUE(instr.result, Runtime::method_constructProperty(engine, instr.name, callData));
    MOTH_END_INSTR(CreateProperty)

    MOTH_BEGIN_INSTR(ConstructPropertyLookup)
        Q_ASSERT(instr.callData + instr.argc + offsetof(QV4::CallData, args)/sizeof(QV4::Value) <= stackSize);
        QV4::CallData *callData = reinterpret_cast<QV4::CallData *>(stack + instr.callData);
        callData->tag = quint32(Value::ValueTypeInternal::Integer);
        callData->argc = instr.argc;
        callData->thisObject = VALUE(instr.base);
        STOREVALUE(instr.result, Runtime::method_constructPropertyLookup(engine, instr.index, callData));
    MOTH_END_INSTR(ConstructPropertyLookup)

    MOTH_BEGIN_INSTR(CreateActivationProperty)
        Q_ASSERT(instr.callData + instr.argc + offsetof(QV4::CallData, args)/sizeof(QV4::Value) <= stackSize);
        QV4::CallData *callData = reinterpret_cast<QV4::CallData *>(stack + instr.callData);
        callData->tag = quint32(Value::ValueTypeInternal::Integer);
        callData->argc = instr.argc;
        callData->thisObject = QV4::Primitive::undefinedValue();
        STOREVALUE(instr.result, Runtime::method_constructActivationProperty(engine, instr.name, callData));
    MOTH_END_INSTR(CreateActivationProperty)

    MOTH_BEGIN_INSTR(ConstructGlobalLookup)
        Q_ASSERT(instr.callData + instr.argc + offsetof(QV4::CallData, args)/sizeof(QV4::Value) <= stackSize);
        QV4::CallData *callData = reinterpret_cast<QV4::CallData *>(stack + instr.callData);
        callData->tag = quint32(Value::ValueTypeInternal::Integer);
        callData->argc = instr.argc;
        callData->thisObject = QV4::Primitive::undefinedValue();
        STOREVALUE(instr.result, Runtime::method_constructGlobalLookup(engine, instr.index, callData));
    MOTH_END_INSTR(ConstructGlobalLookup)

    MOTH_BEGIN_INSTR(Jump)
        code = ((const uchar *)&instr.offset) + instr.offset;
    MOTH_END_INSTR(Jump)

    MOTH_BEGIN_INSTR(JumpEq)
        bool cond = VALUEPTR(instr.condition)->toBoolean();
        TRACE(condition, "%s", cond ? "TRUE" : "FALSE");
        if (cond)
            code = ((const uchar *)&instr.offset) + instr.offset;
    MOTH_END_INSTR(JumpEq)

    MOTH_BEGIN_INSTR(JumpNe)
        bool cond = VALUEPTR(instr.condition)->toBoolean();
        TRACE(condition, "%s", cond ? "TRUE" : "FALSE");
        if (!cond)
            code = ((const uchar *)&instr.offset) + instr.offset;
    MOTH_END_INSTR(JumpNe)

    MOTH_BEGIN_INSTR(JumpStrictEqual)
        bool cond = RuntimeHelpers::strictEqual(VALUE(instr.lhs), VALUE(instr.rhs));
        TRACE(condition, "%s", cond ? "TRUE" : "FALSE");
        if (cond)
            code = ((const uchar *)&instr.offset) + instr.offset;
    MOTH_END_INSTR(JumpStrictEqual)

    MOTH_BEGIN_INSTR(JumpStrictNotEqual)
        bool cond = RuntimeHelpers::strictEqual(VALUE(instr.lhs), VALUE(instr.rhs));
        TRACE(condition, "%s", cond ? "TRUE" : "FALSE");
        if (!cond)
            code = ((const uchar *)&instr.offset) + instr.offset;
    MOTH_END_INSTR(JumpStrictNotEqual)

    MOTH_BEGIN_INSTR(UNot)
        STOREVALUE(instr.result, Runtime::method_uNot(VALUE(instr.source)));
    MOTH_END_INSTR(UNot)

    MOTH_BEGIN_INSTR(UNotBool)
        bool b = VALUE(instr.source).booleanValue();
        VALUE(instr.result) = QV4::Encode(!b);
    MOTH_END_INSTR(UNotBool)

    MOTH_BEGIN_INSTR(UPlus)
        STOREVALUE(instr.result, Runtime::method_uPlus(VALUE(instr.source)));
    MOTH_END_INSTR(UPlus)

    MOTH_BEGIN_INSTR(UMinus)
        STOREVALUE(instr.result, Runtime::method_uMinus(VALUE(instr.source)));
    MOTH_END_INSTR(UMinus)

    MOTH_BEGIN_INSTR(UCompl)
        STOREVALUE(instr.result, Runtime::method_complement(VALUE(instr.source)));
    MOTH_END_INSTR(UCompl)

    MOTH_BEGIN_INSTR(UComplInt)
        VALUE(instr.result) = QV4::Encode((int)~VALUE(instr.source).integerValue());
    MOTH_END_INSTR(UComplInt)

    MOTH_BEGIN_INSTR(PreIncrement)
        STOREVALUE(instr.result, Runtime::method_preIncrement(VALUE(instr.source)));
    MOTH_END_INSTR(PreIncrement)

    MOTH_BEGIN_INSTR(PreDecrement)
        STOREVALUE(instr.result, Runtime::method_preDecrement(VALUE(instr.source)));
    MOTH_END_INSTR(PreDecrement)

    MOTH_BEGIN_INSTR(PostIncrement)
        //### we probably need a write-barrier for instr.source, because it will be written to
        STOREVALUE(instr.result, Runtime::method_postIncrement(VALUEPTR(instr.source)));
    MOTH_END_INSTR(PreIncrement)

    MOTH_BEGIN_INSTR(PostDecrement)
        //### we probably need a write-barrier for instr.source, because it will be written to
        STOREVALUE(instr.result, Runtime::method_postDecrement(VALUEPTR(instr.source)));
    MOTH_END_INSTR(PreDecrement)

    MOTH_BEGIN_INSTR(Binop)
        QV4::Runtime::BinaryOperation op = *reinterpret_cast<QV4::Runtime::BinaryOperation *>(reinterpret_cast<char *>(&engine->runtime.runtimeMethods[instr.alu]));
        STOREVALUE(instr.result, op(VALUE(instr.lhs), VALUE(instr.rhs)));
    MOTH_END_INSTR(Binop)

    MOTH_BEGIN_INSTR(Add)
        STOREVALUE(instr.result, Runtime::method_add(engine, VALUE(instr.lhs), VALUE(instr.rhs)));
    MOTH_END_INSTR(Add)

    MOTH_BEGIN_INSTR(BitAnd)
        STOREVALUE(instr.result, Runtime::method_bitAnd(VALUE(instr.lhs), VALUE(instr.rhs)));
    MOTH_END_INSTR(BitAnd)

    MOTH_BEGIN_INSTR(BitOr)
        STOREVALUE(instr.result, Runtime::method_bitOr(VALUE(instr.lhs), VALUE(instr.rhs)));
    MOTH_END_INSTR(BitOr)

    MOTH_BEGIN_INSTR(BitXor)
        STOREVALUE(instr.result, Runtime::method_bitXor(VALUE(instr.lhs), VALUE(instr.rhs)));
    MOTH_END_INSTR(BitXor)

    MOTH_BEGIN_INSTR(Shr)
        STOREVALUE(instr.result, QV4::Encode((int)(VALUEPTR(instr.lhs)->toInt32() >> (VALUEPTR(instr.rhs)->toInt32() & 0x1f))));
    MOTH_END_INSTR(Shr)

    MOTH_BEGIN_INSTR(Shl)
        STOREVALUE(instr.result, QV4::Encode((int)(VALUEPTR(instr.lhs)->toInt32() << (VALUEPTR(instr.rhs)->toInt32() & 0x1f))));
    MOTH_END_INSTR(Shl)

    MOTH_BEGIN_INSTR(BitAndConst)
        int lhs = VALUEPTR(instr.lhs)->toInt32();
        STOREVALUE(instr.result, QV4::Encode((int)(lhs & instr.rhs)));
    MOTH_END_INSTR(BitAnd)

    MOTH_BEGIN_INSTR(BitOrConst)
        int lhs = VALUEPTR(instr.lhs)->toInt32();
        STOREVALUE(instr.result, QV4::Encode((int)(lhs | instr.rhs)));
    MOTH_END_INSTR(BitOr)

    MOTH_BEGIN_INSTR(BitXorConst)
        int lhs = VALUEPTR(instr.lhs)->toInt32();
        STOREVALUE(instr.result, QV4::Encode((int)(lhs ^ instr.rhs)));
    MOTH_END_INSTR(BitXor)

    MOTH_BEGIN_INSTR(ShrConst)
        STOREVALUE(instr.result, QV4::Encode((int)(VALUEPTR(instr.lhs)->toInt32() >> instr.rhs)));
    MOTH_END_INSTR(ShrConst)

    MOTH_BEGIN_INSTR(ShlConst)
        STOREVALUE(instr.result, QV4::Encode((int)(VALUEPTR(instr.lhs)->toInt32() << instr.rhs)));
    MOTH_END_INSTR(ShlConst)

    MOTH_BEGIN_INSTR(Mul)
        STOREVALUE(instr.result, Runtime::method_mul(VALUE(instr.lhs), VALUE(instr.rhs)));
    MOTH_END_INSTR(Mul)

    MOTH_BEGIN_INSTR(Sub)
        STOREVALUE(instr.result, Runtime::method_sub(VALUE(instr.lhs), VALUE(instr.rhs)));
    MOTH_END_INSTR(Sub)

    MOTH_BEGIN_INSTR(BinopContext)
        QV4::Runtime::BinaryOperationContext op = *reinterpret_cast<QV4::Runtime::BinaryOperationContext *>(reinterpret_cast<char *>(&engine->runtime.runtimeMethods[instr.alu]));
        STOREVALUE(instr.result, op(engine, VALUE(instr.lhs), VALUE(instr.rhs)));
    MOTH_END_INSTR(BinopContext)

    MOTH_BEGIN_INSTR(Ret)
//        TRACE(Ret, "returning value %s", result.toString(context)->toQString().toUtf8().constData());
        returnValue = VALUE(instr.result).asReturnedValue();
        goto functionExit;
    MOTH_END_INSTR(Ret)

#ifndef QT_NO_QML_DEBUGGER
    MOTH_BEGIN_INSTR(Debug)
        debug_slowPath(instr, engine);
    MOTH_END_INSTR(Debug)

    MOTH_BEGIN_INSTR(Line)
        engine->current->lineNumber = instr.lineNumber;
        if (Q_UNLIKELY(qt_v4IsDebugging))
            qt_v4CheckForBreak(engine->currentContext);
    MOTH_END_INSTR(Line)
#endif // QT_NO_QML_DEBUGGER

    MOTH_BEGIN_INSTR(LoadThis)
        VALUE(instr.result) = engine->currentContext->thisObject();
    MOTH_END_INSTR(LoadThis)

    MOTH_BEGIN_INSTR(LoadQmlContext)
        VALUE(instr.result) = Runtime::method_getQmlContext(static_cast<QV4::NoThrowEngine*>(engine));
    MOTH_END_INSTR(LoadQmlContext)

    MOTH_BEGIN_INSTR(LoadQmlImportedScripts)
        VALUE(instr.result) = Runtime::method_getQmlImportedScripts(static_cast<QV4::NoThrowEngine*>(engine));
    MOTH_END_INSTR(LoadQmlImportedScripts)

    MOTH_BEGIN_INSTR(LoadQmlSingleton)
        VALUE(instr.result) = Runtime::method_getQmlSingleton(static_cast<QV4::NoThrowEngine*>(engine), instr.name);
    MOTH_END_INSTR(LoadQmlSingleton)

#ifdef MOTH_THREADED_INTERPRETER
    // nothing to do
#else
        default:
            qFatal("QQmlJS::Moth::VME: Internal error - unknown instruction %d", genericInstr->common.instructionType);
            break;
        }
#endif

        Q_ASSERT(false);
    catchException:
        Q_ASSERT(engine->hasException);
        if (!exceptionHandler) {
            returnValue = QV4::Encode::undefined();
            goto functionExit;
        }
        code = exceptionHandler;
    }

functionExit:
    if (QV4::Debugging::Debugger *debugger = engine->debugger())
        debugger->leavingFunction(returnValue);
    return returnValue;
}
