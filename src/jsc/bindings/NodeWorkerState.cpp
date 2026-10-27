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

extern "C" void Bun__ConsoleObject__useWorkerStdio(JSC::JSGlobalObject*, bool hasOwnEnv, bool ownEnvNamesForceColor);

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
        state->didReceiveMessage(*globalObject, m_port, *this, context, event);
    }

private:
    explicit NodeWorkerPortListener(NodeWorkerState::NodePort port)
        : EventListener(NativeEventListenerType)
        , m_port(port)
    {
    }

    const NodeWorkerState::NodePort m_port;
};

void NodeWorkerState::listen(NodePort which, Ref<EventListener>&& listener)
{
    stopListening(which);
    m_listeners[which] = listener.copyRef();
    EventTarget& target = *m_ports[which];
    target.addEventListener(eventNames().messageEvent, WTF::move(listener), {});
}

void NodeWorkerState::stopListening(NodePort which)
{
    if (RefPtr listener = std::exchange(m_listeners[which], nullptr)) {
        EventTarget& target = *m_ports[which];
        target.removeEventListener(eventNames().messageEvent, *listener, {});
    }
}

void NodeWorkerState::takeFromParent(Zig::GlobalObject& globalObject, WorkerOptions& options)
{
    auto& vm = globalObject.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(!globalObject.hasProcessObject());

    // No ends for a worker whose parent context had stopped (JSWorker.cpp): its console and its process.stdout / stderr are the process's, as for a thread that has no parent Worker.
    auto transferred = std::exchange(options.nodePorts, {});
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

        // Unref'd before the listener is added: none of these ports keeps the thread alive while nothing waits on it.
        auto& hub = *m_ports[NodePort::Hub];
        hub.holdStartUntilEntrySettled();
        hub.jsUnref(&globalObject);
        listen(NodePort::Hub, NodeWorkerPortListener::create(NodePort::Hub));
        // Listening starts the port. On stdout and stderr that takes what the parent's reader asks for ahead of any output: left in the port, it would pass for the ack of the first batch.
        for (auto which : { NodePort::Stdin, NodePort::Stdout, NodePort::Stderr }) {
            if (RefPtr port = m_ports[which]) {
                port->jsUnref(&globalObject);
                listen(which, NodeWorkerPortListener::create(which));
            }
        }

        Bun__ConsoleObject__useWorkerStdio(&globalObject, m_hasOwnEnv, m_ownEnvNamesForceColor);
    }

    // Keeps the order of 1.4: an entry's process.nextTick callbacks run after its microtasks. Node does that for an ES module entry and runs them first for a CommonJS one.
    vm.setOnEachMicrotaskTick(nullptr);

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

    // An entry that threw is about to stop the worker, so what postMessageToThread() sent meanwhile is answered on this stack and not by the loop. Not when the worker already asked to stop (an unhandled rejection of the entry): no script runs then, and the sender gets no answer, as in node.
    auto& vm = globalObject.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    CLEAR_IF_EXCEPTION(scope);
    if (vm.hasPendingTerminationException())
        return;
    hub->deliverQueuedMessages();
    CLEAR_IF_EXCEPTION(scope);
}

void NodeWorkerState::didReceiveMessage(Zig::GlobalObject& globalObject, NodePort which, EventListener& receiver, ScriptExecutionContext& context, Event& event)
{
    RefPtr port = m_ports[which];
    if (which == NodePort::Stdout || which == NodePort::Stderr) {
        // The reader's ack of the batch in flight. Anything else it sends (it asks once ahead of any output) has nothing to release.
        auto& queue = m_consoleQueues[which - 1];
        if (!queue.isAwaitingAck)
            return;
        auto bytes = std::exchange(queue.bytes, {});
        auto ends = std::exchange(queue.ends, {});
        if (ends.isEmpty() || !postConsoleOutput(globalObject, which, bytes.span(), ends.span())) {
            queue.isAwaitingAck = false;
            port->jsUnref(&globalObject);
        }
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
        if (scope.tryClearException())
            Zig::GlobalObject::reportUncaughtExceptionAtEventLoop(&globalObject, exception);
        return;
    }
    if (m_listeners[which] == &receiver) [[unlikely]]
        return;

    // What was built listens in this object's place, and a listener added during a dispatch does not get the event being dispatched.
    EventListenerVector listeners = port->eventListeners(eventNames().messageEvent);
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
    // The `process` of a worker with no channels has no slot that comes here for fd 1 or 2.
    ASSERT(port || !fd);
    if (fd && !port) [[unlikely]]
        return jsUndefined();
    auto* queue = fd ? &m_consoleQueues[fd - 1] : nullptr;

    // A termination (node:vm's timeout) in the middle would leave half of a stream on the port, or drop what the console has waiting for it.
    DeferTerminationForAWhile deferTermination { vm };
    auto* build = JSFunction::create(vm, &globalObject, processObjectInternalsGetNodeWorkerStdioStreamCodeGenerator(vm), &globalObject);
    MarkedArgumentBuffer args;
    args.append(jsNumber(fd));
    args.append(port ? toJS(&globalObject, &globalObject, *port) : jsUndefined());
    // The stream starts with the console's batch in flight: its first writev waits for that ack.
    bool awaitsConsoleAck = queue && queue->isAwaitingAck;
    args.append(jsBoolean(awaitsConsoleAck));
    JSValue result = JSC::profiledCall(&globalObject, ProfilingReason::API, build, JSC::getCallData(build), globalObject.globalThis(), args);
    RETURN_IF_EXCEPTION(scope, {});

    if (!fd) {
        JSObject* stream = result.getObject();
        RELEASE_ASSERT(stream);
        if (port)
            stopListening(NodePort::Stdin);
        m_stdioStreams[0].set(vm, &globalObject, stream);
        return stream;
    }

    auto* built = dynamicDowncast<JSArray>(result);
    RELEASE_ASSERT(built && built->length() == 3);
    JSObject* stream = built->getIndexQuickly(0).getObject();
    JSObject* flush = built->getIndexQuickly(1).getObject();
    JSObject* write = built->getIndexQuickly(2).getObject();
    RELEASE_ASSERT(stream && flush && flush->isCallable() && write && write->isCallable());

    // The stream has the port from here on, and with it the ack that the console waited for and the loop ref held for that. A batch that a console call posted from inside the build is not the stream's: its loop ref goes here.
    stopListening(static_cast<NodePort>(fd));
    if (std::exchange(queue->isAwaitingAck, false) && !awaitsConsoleAck)
        port->jsUnref(&globalObject);
    m_stdioStreams[fd].set(vm, &globalObject, stream);
    m_stdioFlush[fd - 1].set(vm, &globalObject, flush);
    m_consoleWrite[fd - 1].set(vm, &globalObject, write);

    // What the console wrote behind that batch goes through the stream, ahead of what the caller is about to write.
    auto bytes = std::exchange(queue->bytes, {});
    auto ends = std::exchange(queue->ends, {});
    size_t begin = 0;
    for (size_t end : ends) {
        writeConsoleToStream(globalObject, write, bytes.subspan(begin, end - begin));
        RETURN_IF_EXCEPTION(scope, {});
        begin = end;
    }
    return stream;
}

bool NodeWorkerState::postConsoleOutput(Zig::GlobalObject& globalObject, unsigned fd, std::span<const uint8_t> bytes, std::span<const size_t> ends)
{
    auto& vm = globalObject.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    // Nothing here runs script. A termination that lands in it (node:vm's timeout) waits, so the caller knows whether the batch left.
    DeferTerminationForAWhile deferTermination { vm };

    // The reader (makePortReadable in internal/worker/stdio) takes an array of chunks, one for each console call.
    MarkedArgumentBuffer chunks;
    size_t begin = 0;
    for (size_t end : ends) {
        auto* chunk = JSUint8Array::createUninitialized(&globalObject, globalObject.typedArrayStructure(TypeUint8, false), end - begin);
        if (scope.exception()) [[unlikely]]
            break;
        memcpySpan(chunk->typedSpan(), bytes.subspan(begin, end - begin));
        chunks.append(chunk);
        begin = end;
    }
    bool posted = false;
    if (!scope.exception() && !chunks.hasOverflowed()) [[likely]] {
        JSArray* payload = constructArray(&globalObject, static_cast<ArrayAllocationProfile*>(nullptr), chunks);
        if (!scope.exception()) [[likely]] {
            posted = !m_ports[fd]->postMessage(globalObject, payload, {}).hasException();
            (void)scope.exception();
        }
    }
    // Out of memory, or the port is gone: a console call does not throw for that.
    CLEAR_IF_EXCEPTION(scope);
    return posted;
}

void NodeWorkerState::releaseConsoleQueue(Zig::GlobalObject& globalObject, unsigned fd)
{
    auto& queue = m_consoleQueues[fd - 1];
    auto bytes = std::exchange(queue.bytes, {});
    if (auto ends = std::exchange(queue.ends, {}); !ends.isEmpty())
        postConsoleOutput(globalObject, fd, bytes.span(), ends.span());
    if (std::exchange(queue.isAwaitingAck, false))
        m_ports[fd]->jsUnref(&globalObject);
}

void NodeWorkerState::writeConsoleToStream(Zig::GlobalObject& globalObject, JSObject* write, std::span<const uint8_t> bytes)
{
    // A string, as the Console of node:console gives the stream.
    MarkedArgumentBuffer args;
    args.append(jsString(globalObject.vm(), String::fromUTF8ReplacingInvalidSequences(bytes)));
    JSC::profiledCall(&globalObject, ProfilingReason::API, write, JSC::getCallData(write), jsUndefined(), args);
}

void NodeWorkerState::writeConsole(Zig::GlobalObject& globalObject, unsigned fd, std::span<const uint8_t> bytes)
{
    ASSERT(fd == 1 || fd == 2);
    if (bytes.empty())
        return;

    if (JSObject* write = m_consoleWrite[fd - 1].get())
        return writeConsoleToStream(globalObject, write, bytes);

    RefPtr port = m_ports[fd];
    if (!port) [[unlikely]]
        return;
    auto& queue = m_consoleQueues[fd - 1];
    if (queue.isAwaitingAck) {
        queue.bytes.append(bytes);
        queue.ends.append(queue.bytes.size());
        return;
    }
    size_t end = bytes.size();
    bool posted = postConsoleOutput(globalObject, fd, bytes, { &end, 1 });
    if (m_isExiting || !posted)
        return;

    // The port holds the loop while the batch is in flight, so what waits behind it is not lost to an idle exit.
    ASSERT(m_listeners[fd]);
    queue.isAwaitingAck = true;
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
                if (!scope.tryClearException())
                    return;
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

// May leave an exception pending (NodeWorkerState::writeConsole).
extern "C" void Bun__NodeWorker__writeConsole(JSC::JSGlobalObject* lexicalGlobalObject, uint8_t fd, const uint8_t* bytes, size_t length)
{
    // The worker's own global, also for a console call from a ShadowRealm or a node:vm context.
    auto* globalObject = defaultGlobalObject(JSC::getVM(lexicalGlobalObject));
    if (auto* nodeWorker = globalObject->nodeWorkerState())
        nodeWorker->writeConsole(*globalObject, fd, { bytes, length });
}
