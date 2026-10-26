#include "root.h"
#include "NodeWorkerState.h"

#include "AddEventListenerOptions.h"
#include "Event.h"
#include "EventListener.h"
#include "EventNames.h"
#include "InternalModuleRegistry.h"
#include "JSBuffer.h"
#include "JSMessagePort.h"
#include "MessagePortPipe.h"
#include "ScriptExecutionContext.h"
#include "StructuredSerializeOptions.h"
#include "WebCoreJSBuiltins.h"
#include "Worker.h"
#include "ZigGlobalObject.h"
#include <JavaScriptCore/DeferTermination.h>
#include <JavaScriptCore/JSArray.h>
#include <JavaScriptCore/JSGenericTypedArrayViewInlines.h>
#include <JavaScriptCore/JSMap.h>
#include <wtf/TZoneMallocInlines.h>

extern "C" void Bun__ConsoleObject__useWorkerStdio(JSC::JSGlobalObject*);

namespace Bun {

using namespace JSC;
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(NodeWorkerState);

NodeWorkerState::NodeWorkerState() = default;
NodeWorkerState::~NodeWorkerState() = default;

class NodeWorkerPortListener final : public EventListener {
public:
    static Ref<NodeWorkerPortListener> create(NodeWorkerState::NodePort port)
    {
        return adoptRef(*new NodeWorkerPortListener(port));
    }

    bool operator==(const EventListener& other) const final { return this == &other; }

    void handleEvent(ScriptExecutionContext& context, Event& event) final
    {
        auto* globalObject = defaultGlobalObject(context.globalObject());
        auto* state = globalObject ? globalObject->nodeWorkerState() : nullptr;
        if (!state)
            return;
        Ref protectedThis { *this };
        state->didReceiveMessage(*globalObject, m_port, context, event);
    }

private:
    explicit NodeWorkerPortListener(NodeWorkerState::NodePort port)
        : EventListener(NativeEventListenerType)
        , m_port(port)
    {
    }

    const NodeWorkerState::NodePort m_port;
};

void NodeWorkerState::listen(NodePort which)
{
    ASSERT(!m_listeners[which]);
    Ref listener = NodeWorkerPortListener::create(which);
    m_listeners[which] = listener.copyRef();
    static_cast<EventTarget&>(*m_ports[which]).addEventListener(eventNames().messageEvent, WTF::move(listener), {});
}

void NodeWorkerState::stopListening(NodePort which)
{
    if (RefPtr listener = std::exchange(m_listeners[which], nullptr))
        static_cast<EventTarget&>(*m_ports[which]).removeEventListener(eventNames().messageEvent, *listener, {});
}

void NodeWorkerState::takeFromParent(Zig::GlobalObject& globalObject, WorkerOptions& options)
{
    auto& vm = globalObject.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(!globalObject.hasProcessObject());

    auto transferred = std::exchange(options.nodePorts, {});
    ASSERT(transferred.size() == NodePort::NodePortCount);
    if (transferred.size() == NodePort::NodePortCount) [[likely]] {
        auto& context = *globalObject.scriptExecutionContext();
        for (unsigned i = 0; i < NodePort::NodePortCount; i++) {
            if (transferred[i].pipe)
                m_ports[i] = MessagePort::entangle(context, WTF::move(transferred[i]));
        }

        // node starts parentPort for the worker. It delivers once the entry has evaluated and a 'message' listener exists.
        auto& parent = *m_ports[NodePort::Parent];
        parent.holdStartUntilEntrySettled();
        parent.start();

        // Unref'd before the listener is added: neither the hub port nor an unread stdin keeps the thread alive.
        auto& hub = *m_ports[NodePort::Hub];
        hub.holdStartUntilEntrySettled();
        hub.jsUnref(&globalObject);
        listen(NodePort::Hub);
        if (RefPtr stdinPort = m_ports[NodePort::Stdin]) {
            stdinPort->jsUnref(&globalObject);
            listen(NodePort::Stdin);
        }

        Bun__ConsoleObject__useWorkerStdio(&globalObject);

        // Keeps the order of 1.4: an entry's process.nextTick callbacks run after its microtasks. Node does that for an ES module entry and runs them first for a CommonJS one.
        vm.setOnEachMicrotaskTick(nullptr);
    }

    JSMap* environmentData = nullptr;
    JSValue workerData = takeWorkerDataFromParent(globalObject, options, environmentData);
    RETURN_IF_EXCEPTION(scope, );
    if (workerData)
        m_workerData.set(vm, &globalObject, workerData);
    if (environmentData)
        globalObject.setNodeWorkerEnvironmentData(environmentData);
}

void NodeWorkerState::entryDidSettle(Zig::GlobalObject& globalObject)
{
    if (std::exchange(m_entrySettled, true))
        return;

    if (RefPtr parent = m_ports[NodePort::Parent])
        parent->entrySettled();

    RefPtr hub = m_ports[NodePort::Hub];
    if (!hub)
        return;
    hub->entrySettled();
    if (!MessagePortPipe::queuedCount(hub->pipe()->state(hub->side())))
        return;

    // An entry that failed is about to stop the worker, so what postMessageToThread() sent meanwhile is answered on this stack and not by the loop.
    auto& vm = globalObject.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    CLEAR_IF_EXCEPTION(scope);
    if (vm.hasPendingTerminationException())
        return;
    hub->deliverQueuedMessages();
    CLEAR_IF_EXCEPTION(scope);
}

void NodeWorkerState::didReceiveMessage(Zig::GlobalObject& globalObject, NodePort which, ScriptExecutionContext& context, Event& event)
{
    RefPtr port = m_ports[which];
    if (which == NodePort::Stdout || which == NodePort::Stderr) {
        auto& queue = m_consoleQueues[which - 1];
        if (!queue.isAwaitingAck)
            return;
        if (queue.waiting.isEmpty()) {
            queue.isAwaitingAck = false;
            port->jsUnref(&globalObject);
            return;
        }
        auto waiting = std::exchange(queue.waiting, {});
        postConsoleOutput(globalObject, which, waiting.span());
        return;
    }

    auto& vm = globalObject.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    {
        // A termination in the middle of the build would leave the port with no reader.
        DeferTerminationForAWhile deferTermination { vm };
        if (which == NodePort::Stdin)
            stdioStream(globalObject, 0);
        else
            globalObject.internalModuleRegistry()->requireId(&globalObject, vm, InternalModuleRegistry::Field::InternalWorkerMessaging);
    }
    if (auto* exception = scope.exception()) [[unlikely]] {
        if (!vm.isTerminationException(exception)) {
            scope.clearException();
            Zig::GlobalObject::reportUncaughtExceptionAtEventLoop(&globalObject, exception);
        }
        return;
    }
    if (m_listeners[which]) [[unlikely]]
        return;

    // What was built listens in this object's place, and a listener added during a dispatch does not get the event being dispatched.
    EventListenerVector listeners = static_cast<EventTarget&>(*port).eventListeners(eventNames().messageEvent);
    for (auto& registered : listeners) {
        if (!registered->wasRemoved())
            registered->callback().handleEvent(context, event);
    }
}

JSValue NodeWorkerState::stdioStream(Zig::GlobalObject& globalObject, unsigned fd)
{
    ASSERT(fd < 3);
    if (auto* stream = m_stdioStreams[fd].get())
        return stream;

    auto& vm = globalObject.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RefPtr port = m_ports[fd];
    ASSERT(port || !fd);
    if (fd && !port) [[unlikely]]
        return jsUndefined();

    // The stream has the port from here on, so what the console holds for it goes out first.
    if (fd) {
        stopListening(static_cast<NodePort>(fd));
        releaseConsoleQueue(globalObject, fd);
    }

    JSValue result;
    {
        // A termination (node:vm's timeout) in the middle of the build would leave half of a stream on the port.
        DeferTerminationForAWhile deferTermination { vm };
        auto* build = JSFunction::create(vm, &globalObject, processObjectInternalsGetNodeWorkerStdioStreamCodeGenerator(vm), &globalObject);
        MarkedArgumentBuffer args;
        args.append(jsNumber(fd));
        args.append(port ? toJS(&globalObject, &globalObject, *port) : jsUndefined());
        result = JSC::profiledCall(&globalObject, ProfilingReason::API, build, JSC::getCallData(build), globalObject.globalThis(), args);
    }
    RETURN_IF_EXCEPTION(scope, {});

    JSObject* stream = nullptr;
    if (!fd) {
        stream = asObject(result);
        stopListening(NodePort::Stdin);
    } else {
        auto* built = uncheckedDowncast<JSArray>(result);
        stream = asObject(built->getIndexQuickly(0));
        m_stdioFlush[fd - 1].set(vm, &globalObject, asObject(built->getIndexQuickly(1)));
        m_consoleWrite[fd - 1].set(vm, &globalObject, asObject(built->getIndexQuickly(2)));
    }
    m_stdioStreams[fd].set(vm, &globalObject, stream);
    return stream;
}

void NodeWorkerState::postConsoleOutput(Zig::GlobalObject& globalObject, unsigned fd, std::span<const uint8_t> bytes)
{
    auto& vm = globalObject.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    RefPtr port = m_ports[fd];

    // The reader (makePortReadable in internal/worker/stdio) takes an array of chunks.
    if (auto* chunk = JSUint8Array::createUninitialized(&globalObject, globalObject.typedArrayStructure(TypeUint8, false), bytes.size())) [[likely]] {
        memcpySpan(chunk->typedSpan(), bytes);
        MarkedArgumentBuffer chunks;
        chunks.append(chunk);
        if (JSArray* payload = constructArray(&globalObject, static_cast<ArrayAllocationProfile*>(nullptr), chunks))
            (void)port->postMessage(globalObject, payload, {});
    }
    // Out of memory, or the port is gone: a console call does not throw for that.
    if (scope.exception()) [[unlikely]]
        (void)scope.tryClearException();
}

void NodeWorkerState::releaseConsoleQueue(Zig::GlobalObject& globalObject, unsigned fd)
{
    auto& queue = m_consoleQueues[fd - 1];
    if (auto waiting = std::exchange(queue.waiting, {}); !waiting.isEmpty())
        postConsoleOutput(globalObject, fd, waiting.span());
    if (std::exchange(queue.isAwaitingAck, false))
        m_ports[fd]->jsUnref(&globalObject);
}

void NodeWorkerState::writeConsole(Zig::GlobalObject& globalObject, unsigned fd, std::span<const uint8_t> bytes)
{
    ASSERT(fd == 1 || fd == 2);
    if (bytes.empty())
        return;

    if (JSObject* write = m_consoleWrite[fd - 1].get()) {
        auto& vm = globalObject.vm();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        if (auto* chunk = createBuffer(&globalObject, bytes)) {
            MarkedArgumentBuffer args;
            args.append(chunk);
            JSC::profiledCall(&globalObject, ProfilingReason::API, write, JSC::getCallData(write), jsUndefined(), args);
        }
        if (scope.exception()) [[unlikely]]
            (void)scope.tryClearException();
        return;
    }

    RefPtr port = m_ports[fd];
    if (!port) [[unlikely]]
        return;
    auto& queue = m_consoleQueues[fd - 1];
    if (queue.isAwaitingAck) {
        queue.waiting.append(bytes);
        return;
    }
    postConsoleOutput(globalObject, fd, bytes);
    if (m_isExiting)
        return;

    // The port holds the loop while the batch is in flight, so what waits behind it is not lost to an idle exit.
    queue.isAwaitingAck = true;
    if (!m_listeners[fd])
        listen(static_cast<NodePort>(fd));
    port->jsRef(&globalObject);
}

void NodeWorkerState::flushStdioForExit(Zig::GlobalObject& globalObject)
{
    auto& vm = globalObject.vm();
    m_isExiting = true;
    for (unsigned fd = 1; fd <= 2; fd++) {
        if (JSObject* flush = m_stdioFlush[fd - 1].get()) {
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            MarkedArgumentBuffer args;
            JSC::profiledCall(&globalObject, ProfilingReason::API, flush, JSC::getCallData(flush), jsUndefined(), args);
            if (auto* exception = scope.exception()) [[unlikely]] {
                if (vm.isTerminationException(exception))
                    return;
                scope.clearException();
                Zig::GlobalObject::reportUncaughtExceptionAtEventLoop(&globalObject, exception);
            }
            continue;
        }
        if (m_ports[fd])
            releaseConsoleQueue(globalObject, fd);
    }
}

template<typename Visitor>
void NodeWorkerState::visit(Visitor& visitor)
{
    visitor.append(m_workerData);
    visitor.append(m_stdioStreams.begin(), m_stdioStreams.end());
    visitor.append(m_stdioFlush.begin(), m_stdioFlush.end());
    visitor.append(m_consoleWrite.begin(), m_consoleWrite.end());
}

template void NodeWorkerState::visit(JSC::AbstractSlotVisitor&);
template void NodeWorkerState::visit(JSC::SlotVisitor&);

} // namespace Bun

extern "C" void Bun__NodeWorker__writeConsole(JSC::JSGlobalObject* lexicalGlobalObject, uint8_t fd, const uint8_t* bytes, size_t length)
{
    // The worker's own global, also for a console call from a ShadowRealm or a node:vm context.
    auto* globalObject = defaultGlobalObject(JSC::getVM(lexicalGlobalObject));
    if (auto* nodeWorker = globalObject->nodeWorkerState())
        nodeWorker->writeConsole(*globalObject, fd, { bytes, length });
}
