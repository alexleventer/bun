/*
 * Copyright (C) 2008-2017 Apple Inc. All rights reserved.
 * Copyright (C) 2009 Google Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "Worker.h"

#include "InternalModuleRegistry.h"
#include "ErrorCode.h"
#include "ErrorEvent.h"
#include "Event.h"
#include "EventNames.h"
#include "StructuredSerializeOptions.h"
#include <JavaScriptCore/IteratorOperations.h>
#include <JavaScriptCore/ScriptCallStack.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Scope.h>
#include "SerializedScriptValue.h"
#include "ScriptExecutionContext.h"
#include <JavaScriptCore/JSMap.h>
#include <JavaScriptCore/JSModuleLoader.h>
#include "MessageEvent.h"
#include "GlobalEventScope.h"
#include "CloseEvent.h"
#include "JSDOMConvertObject.h"
#include "JSDOMConvertSequences.h"
#include "JSMessagePort.h"
#include "JSMessageChannel.h"
#include "JSWorker.h"
#include "MessagePortPipe.h"
#include "JSBroadcastChannel.h"
#include "JSStructuredSerializeOptions.h"
#include "BunClientData.h"
#include "NodeWorkerState.h"

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Worker);

Worker::Worker(ScriptExecutionContext& context, WorkerOptions&& options)
    : ActiveDOMObject(&context)
    , m_name(options.name)
    , m_contextProxy(WorkerMessagingProxy::create(*this, context, WTF::move(options)))
{
}

ExceptionOr<Ref<Worker>> Worker::create(ScriptExecutionContext& context, const String& urlInit, WorkerOptions&& options)
{
    ASSERT(context.isContextThread());

    String url = urlInit;
    if (url.startsWith("file://"_s)) {
        WTF::URL urlObject { url };
        if (!urlObject.isValid())
            return Exception { TypeError, makeString("Invalid file URL: \""_s, urlInit, '"') };
        url = urlObject.fileSystemPath();
    }

    auto worker = adoptRef(*new Worker(context, WTF::move(options)));
    worker->suspendIfNeeded();

    auto started = worker->m_contextProxy->startWorkerGlobalScope(url);
    if (started.hasException())
        return started.releaseException();
    return worker;
}

Worker::~Worker()
{
    m_contextProxy->workerObjectDestroyed();
}

// As in WebCore and Node: a message for a worker that has terminated is serialized (transfer
// side effects still happen) and then dropped by the proxy; it is not an error.
ExceptionOr<void> Worker::postMessage(JSC::JSGlobalObject& state, JSC::JSValue messageValue, StructuredSerializeOptions&& options)
{
    Vector<RefPtr<MessagePort>> ports;
    auto serialized = SerializedScriptValue::create(state, messageValue, WTF::move(options.transfer), ports, SerializationForStorage::No, SerializationContext::WorkerPostMessage);
    if (serialized.hasException())
        return serialized.releaseException();

    auto disentangledPorts = MessagePort::disentanglePorts(WTF::move(ports));
    if (disentangledPorts.hasException())
        return disentangledPorts.releaseException();

    m_contextProxy->postMessageToWorkerGlobalScope(MessageWithMessagePorts { serialized.releaseReturnValue(), disentangledPorts.releaseReturnValue() });
    return {};
}

void Worker::terminate()
{
    m_wasTerminated = true;
    m_contextProxy->terminateWorkerGlobalScope();
}

void Worker::stop()
{
    terminate();
}

bool Worker::virtualHasPendingActivity() const
{
    return m_contextProxy->hasPendingActivity();
}

void Worker::setKeepAlive(bool keepAlive)
{
    m_contextProxy->setKeepAlive(keepAlive);
}

void Worker::dispatchEvent(Event& event)
{
    if (m_wasTerminated || !m_contextProxy->hasPendingActivity())
        return;
    EventTargetWithInlineData::dispatchEvent(event);
}

void Worker::dispatchCloseEvent(Event& event)
{
    EventTargetWithInlineData::dispatchEvent(event);
}

// ---- Worker-thread side: hooks the native thread object calls, and the script-facing functions that
//      run inside a worker (parentPort.postMessage, workerData, receiveMessageOnPort, ...).

// The proxy of the worker whose global scope runs on `bunVM`'s thread, or null on the main thread.
extern "C" WorkerMessagingProxy* WebWorker__getMessagingProxy(void* bunVM);

// The entry module just finished (or failed) its top-level evaluation. node's bootstrap runs the
// synchronous CJS main before any port delivery: a node:worker_threads worker's parentPort starts
// delivering now, and so does its port to the postMessageToThread hub, so a routed message does not
// observe "no listeners" while the entry that registers them is still loading. Runs on every
// post-evaluation path, so a message that arrived meanwhile never leaves its sender's
// Atomics.waitAsync unresolved.
extern "C" void WebWorker__entrySettled(Zig::GlobalObject* globalObject)
{
    if (auto* nodeWorker = globalObject->nodeWorkerState())
        nodeWorker->entryDidSettle(*globalObject);
}

extern "C" void WebWorker__workerThreadStarted(WorkerMessagingProxy* proxy)
{
    proxy->workerThreadStarted();
}

extern "C" void WebWorker__workerGlobalScopeStarted(WorkerMessagingProxy* proxy, Zig::GlobalObject* globalObject)
{
    WebWorker__entrySettled(globalObject);
    proxy->workerGlobalScopeStarted(*globalObject);
}

extern "C" void WebWorker__workerGlobalScopeDestroyed(WorkerMessagingProxy* proxy, int32_t exitCode, bool stoppedByParent)
{
    proxy->workerGlobalScopeDestroyed(exitCode, stoppedByParent);
}

extern "C" void WebWorker__parentContextWillDestroy(WorkerMessagingProxy* proxy)
{
    proxy->parentContextWillDestroy();
}

// An uncaught error inside the worker: dispatch 'error' on the worker's own global scope, then report
// it to the Worker object.
extern "C" void WebWorker__dispatchError(Zig::GlobalObject* globalObject, WorkerMessagingProxy* proxy, BunString message, JSC::EncodedJSValue errorValue)
{
    JSC::JSValue error = JSC::JSValue::decode(errorValue);
    String messageStr = message.transferToWTFString();
    ErrorEvent::Init init;
    init.message = messageStr;
    init.error = error;
    init.cancelable = false;
    init.bubbles = false;
    globalObject->globalEventScope->dispatchEvent(ErrorEvent::create(eventNames().errorEvent, init, EventIsTrusted::Yes));
    proxy->postErrorToWorkerObject(*globalObject, messageStr, error);
}

JSC_DEFINE_HOST_FUNCTION(jsReceiveMessageOnPort, (JSGlobalObject * lexicalGlobalObject, CallFrame* callFrame))
{
    auto& vm = JSC::getVM(lexicalGlobalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (callFrame->argumentCount() < 1) {
        throwTypeError(lexicalGlobalObject, scope, "receiveMessageOnPort needs 1 argument"_s);
        return {};
    }

    auto port = callFrame->argument(0);

    if (!port.isObject()) {
        return Bun::throwError(lexicalGlobalObject, scope, Bun::ErrorCode::ERR_INVALID_ARG_TYPE, "The \"port\" argument must be a MessagePort instance"_s);
    }

    if (auto* messagePort = dynamicDowncast<JSMessagePort>(port)) {
        // node: `undefined` when the queue is empty, otherwise `{ message }` — built
        // here so a posted `undefined`/falsy value is distinguishable from "empty".
        bool hadMessage = false;
        JSValue message = messagePort->wrapped().tryTakeMessage(lexicalGlobalObject, hadMessage);
        RETURN_IF_EXCEPTION(scope, {});
        if (!hadMessage)
            return JSC::JSValue::encode(jsUndefined());
        auto* result = JSC::constructEmptyObject(lexicalGlobalObject, lexicalGlobalObject->objectPrototype(), 1);
        result->putDirect(vm, JSC::Identifier::fromString(vm, "message"_s), message);
        return JSC::JSValue::encode(result);
    } else if (dynamicDowncast<JSBroadcastChannel>(port)) {
        // TODO: support broadcast channels
        return JSC::JSValue::encode(jsUndefined());
    }

    return Bun::throwError(lexicalGlobalObject, scope, Bun::ErrorCode::ERR_INVALID_ARG_TYPE, "The \"port\" argument must be a MessagePort instance"_s);
}

JSC_DEFINE_HOST_FUNCTION(jsMessagePortIsActive, (JSGlobalObject * lexicalGlobalObject, CallFrame* callFrame))
{
    auto port = callFrame->argument(0);
    if (auto* messagePort = dynamicDowncast<JSMessagePort>(port)) {
        auto& wrapped = messagePort->wrapped();
        bool active = (wrapped.isDetached() == false) && wrapped.pipe()->isOtherSideOpen(wrapped.side());
        return JSC::JSValue::encode(jsBoolean(active));
    }
    return JSC::JSValue::encode(jsBoolean(false));
}

// Primitives are a documented no-op for both.
JSC_DEFINE_HOST_FUNCTION(jsFunctionMarkAsUncloneable, (JSGlobalObject * lexicalGlobalObject, CallFrame* callFrame))
{
    if (auto* object = callFrame->argument(0).getObject())
        markAsUncloneable(lexicalGlobalObject->vm(), *object);
    return JSC::JSValue::encode(JSC::jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(jsFunctionMarkAsUntransferable, (JSGlobalObject * lexicalGlobalObject, CallFrame* callFrame))
{
    if (auto* object = callFrame->argument(0).getObject())
        markAsUntransferable(lexicalGlobalObject->vm(), *object);
    return JSC::JSValue::encode(JSC::jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(jsFunctionIsMarkedAsUntransferable, (JSGlobalObject * lexicalGlobalObject, CallFrame* callFrame))
{
    auto& vm = lexicalGlobalObject->vm();
    auto* object = callFrame->argument(0).getObject();
    return JSC::JSValue::encode(jsBoolean(object && !!object->getDirect(vm, builtinNames(vm).isUntransferablePrivateName())));
}

// Main thread starts at 1
static JSValue threadIdOf(WorkerMessagingProxy* proxy)
{
    return jsNumber(proxy ? proxy->workerContextIdentifier() - 1 : 0);
}

JSValue createNodeWorkerThreadsBinding(Zig::GlobalObject* globalObject)
{
    VM& vm = globalObject->vm();

    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue workerData = jsNull();
    JSValue threadName = jsEmptyString(vm);
    JSMap* environmentData = nullptr;
    JSValue parentPort = jsUndefined();
    JSValue messagePortEmitterPrototype = jsUndefined();

    // The module is about to define MessagePort's emitter methods, close(callback) and inspect hook.
    JSValue messagePortConstructor = JSMessagePort::getConstructor(vm, globalObject);
    RETURN_IF_EXCEPTION(scope, {});

    auto* proxy = WebWorker__getMessagingProxy(globalObject->bunVM());
    auto* nodeWorker = globalObject->nodeWorkerState();
    if (nodeWorker) {
        // Already taken from the parent by the start sequence.
        if (JSValue taken = nodeWorker->workerData())
            workerData = taken;
        environmentData = globalObject->nodeWorkerEnvironmentData();
        if (auto* port = nodeWorker->port(WorkerOptions::Parent)) {
            parentPort = toJS(globalObject, globalObject, *port);
            RETURN_IF_EXCEPTION(scope, {});
        }
        if (auto* emitterPrototype = JSMessagePort::retireNodeWorkerMemberSlots(vm, *globalObject))
            messagePortEmitterPrototype = emitterPrototype;
    } else if (proxy) {
        auto& options = proxy->options();
        RefPtr<WebCore::SerializedScriptValue> serialized = WTF::move(options.workerDataAndEnvironmentData);
        // `workerDataAndEnvironmentData` is moved-from on the first call. If
        // this binding is created twice (lazy-init re-entry), `serialized` is
        // null and `->deserialize` would UB → garbage → SIGTRAP at the
        // uncheckedDowncast below (#53748 darwin). Guard both: skip the
        // deserialize on second call (workerData stays jsUndefined), and use a
        // checked cast so a non-Array deserialize result doesn't trap.
        if (serialized) {
            auto ports = MessagePort::entanglePorts(*globalObject->scriptExecutionContext(), WTF::move(options.dataMessagePorts));
            JSValue deserialized = serialized->deserialize(*globalObject, globalObject, WTF::move(ports));
            RETURN_IF_EXCEPTION(scope, {});
            // Should always be set to an Array of length 2 in the constructor in JSWorker.cpp
            if (auto* pair = dynamicDowncast<JSArray>(deserialized)) {
                ASSERT(pair->length() == 2);
                ASSERT(pair->canGetIndexQuickly(0u));
                ASSERT(pair->canGetIndexQuickly(1u));
                workerData = pair->getIndexQuickly(0);
                RETURN_IF_EXCEPTION(scope, {});
                auto environmentDataValue = pair->getIndexQuickly(1);
                // it might not be a Map if the parent had not set up environmentData yet
                environmentData = environmentDataValue ? dynamicDowncast<JSMap>(environmentDataValue) : nullptr;
                RETURN_IF_EXCEPTION(scope, {});
            } else {
                ASSERT_NOT_REACHED_WITH_MESSAGE("createNodeWorkerThreadsBinding: deserialized is not JSArray");
            }
        }
    }
    if (proxy) {
        // isolatedCopy: this JSString lives in the worker heap; it must own a
        // worker-local impl so its GC deref never races m_options.name's
        // (non-atomic) refcount on the parent thread.
        threadName = jsString(vm, proxy->options().name.isolatedCopy());
    }
    if (!environmentData) {
        environmentData = JSMap::create(vm, globalObject->mapStructure());
        RETURN_IF_EXCEPTION(scope, {});
    }
    ASSERT(environmentData);
    globalObject->setNodeWorkerEnvironmentData(environmentData);

    JSObject* array = constructEmptyArray(globalObject, nullptr, 15);
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 0, workerData);
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 1, threadIdOf(proxy));
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 2, JSFunction::create(vm, globalObject, 1, "receiveMessageOnPort"_s, jsReceiveMessageOnPort, ImplementationVisibility::Public, NoIntrinsic));
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 3, environmentData);
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 4, threadName);
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 5, JSFunction::create(vm, globalObject, 1, "isMessagePortActive"_s, jsMessagePortIsActive, ImplementationVisibility::Public, NoIntrinsic));
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 6, JSFunction::create(vm, globalObject, 1, "markAsUntransferable"_s, jsFunctionMarkAsUntransferable, ImplementationVisibility::Public, NoIntrinsic));
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 7, JSFunction::create(vm, globalObject, 1, "isMarkedAsUntransferable"_s, jsFunctionIsMarkedAsUntransferable, ImplementationVisibility::Public, NoIntrinsic));
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 8, JSFunction::create(vm, globalObject, 1, "markAsUncloneable"_s, jsFunctionMarkAsUncloneable, ImplementationVisibility::Public, NoIntrinsic));
    RETURN_IF_EXCEPTION(scope, {});
    // A worker of node:worker_threads' Worker: the port entangled with that Worker's public port.
    // The start sequence took and started it (NodeWorkerState::takeFromParent).
    array->putDirectIndex(globalObject, 9, parentPort);
    RETURN_IF_EXCEPTION(scope, {});
    // A node worker: the object between MessagePort.prototype and EventTarget.prototype, where
    // the emitter methods go.
    array->putDirectIndex(globalObject, 10, messagePortEmitterPrototype);
    RETURN_IF_EXCEPTION(scope, {});
    // The intrinsic constructors, so worker_threads keeps working when user code
    // replaces globalThis.MessagePort etc. before the module loads (#40268).
    array->putDirectIndex(globalObject, 11, messagePortConstructor);
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 12, JSMessageChannel::getConstructor(vm, globalObject));
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 13, JSBroadcastChannel::getConstructor(vm, globalObject));
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 14, JSWorker::getConstructor(vm, globalObject));
    RETURN_IF_EXCEPTION(scope, {});
    return array;
}

// internal/worker/messaging (postMessageToThread): [threadId, isMainThread, MessageChannel, and in
// a worker of node:worker_threads' Worker its port to the main thread's hub].
JSValue createNodeWorkerMessagingBinding(Zig::GlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* proxy = WebWorker__getMessagingProxy(globalObject->bunVM());
    JSValue hubPort = jsUndefined();
    if (auto* nodeWorker = globalObject->nodeWorkerState()) {
        if (auto* port = nodeWorker->port(WorkerOptions::Hub)) {
            hubPort = toJS(globalObject, globalObject, *port);
            RETURN_IF_EXCEPTION(scope, {});
            // The module listens on it from here on.
            nodeWorker->stopListeningForArrival(WorkerOptions::Hub);
        }
    }

    JSObject* array = constructEmptyArray(globalObject, nullptr, 4);
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 0, threadIdOf(proxy));
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 1, jsBoolean(globalObject->scriptExecutionContext()->isMainThread()));
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 2, JSMessageChannel::getConstructor(vm, globalObject));
    RETURN_IF_EXCEPTION(scope, {});
    array->putDirectIndex(globalObject, 3, hubPort);
    RETURN_IF_EXCEPTION(scope, {});
    return array;
}

// A node:worker_threads worker's start, before preloads and the entry point, as Node runs its worker
// bootstrap before user code. It takes what the parent's Worker sent and runs no builtin JS, except
// for workerData that only node:worker_threads can unpack. Throws like require() would.
extern "C" void Bun__Worker__startNodeWorker(Zig::GlobalObject* globalObject)
{
    auto* proxy = WebWorker__getMessagingProxy(globalObject->bunVM());
    auto* nodeWorker = globalObject->nodeWorkerState();
    ASSERT(proxy && nodeWorker);
    nodeWorker->takeFromParent(*globalObject, proxy->options());
}

JSC_DEFINE_HOST_FUNCTION(jsFunctionPostMessage,
    (JSC::JSGlobalObject * leixcalGlobalObject, JSC::CallFrame* callFrame))
{
    JSC::VM& vm = leixcalGlobalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Zig::GlobalObject* globalObject = dynamicDowncast<Zig::GlobalObject>(leixcalGlobalObject);
    if (!globalObject) [[unlikely]]
        return JSValue::encode(jsUndefined());

    auto* proxy = WebWorker__getMessagingProxy(globalObject->bunVM());
    if (!proxy)
        return JSValue::encode(jsUndefined());

    JSC::JSValue value = callFrame->argument(0);
    JSC::JSValue options = callFrame->argument(1);

    Vector<JSC::Strong<JSC::JSObject>> transferList;

    // postMessage(message, sequence<object>) and postMessage(message, { transfer })
    // overloads. Both are converted per WebIDL before serializing, so an invalid
    // transfer list throws a TypeError without detaching anything.
    if (!options.isUndefinedOrNull()) {
        bool isSequence = hasIteratorMethod(globalObject, options);
        RETURN_IF_EXCEPTION(scope, {});
        if (isSequence) {
            transferList = convert<IDLSequence<IDLObject>>(*globalObject, options);
            RETURN_IF_EXCEPTION(scope, {});
        } else {
            auto serializeOptions = convertDictionary<StructuredSerializeOptions>(*globalObject, options);
            RETURN_IF_EXCEPTION(scope, {});
            transferList = WTF::move(serializeOptions.transfer);
        }
    }

    Vector<RefPtr<MessagePort>> ports;
    ExceptionOr<Ref<SerializedScriptValue>> serialized = SerializedScriptValue::create(*globalObject, value, WTF::move(transferList), ports, SerializationForStorage::No, SerializationContext::WorkerPostMessage);
    RETURN_IF_EXCEPTION(scope, {});
    if (serialized.hasException()) {
        WebCore::propagateException(*globalObject, scope, serialized.releaseException());
        RELEASE_AND_RETURN(scope, {});
    }
    RETURN_IF_EXCEPTION(scope, {});

    ExceptionOr<Vector<TransferredMessagePort>> disentangledPorts = MessagePort::disentanglePorts(WTF::move(ports));
    if (disentangledPorts.hasException()) {
        WebCore::propagateException(*globalObject, scope, disentangledPorts.releaseException());
        RELEASE_AND_RETURN(scope, {});
    }
    RETURN_IF_EXCEPTION(scope, {});

    proxy->postMessageToWorkerObject(MessageWithMessagePorts { serialized.releaseReturnValue(), disentangledPorts.releaseReturnValue() });

    return JSValue::encode(jsUndefined());
}

} // namespace WebCore
