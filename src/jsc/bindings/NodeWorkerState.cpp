#include "root.h"
#include "NodeWorkerState.h"

#include "AddEventListenerOptions.h"
#include "BunClientData.h"
#include "BunProcess.h"
#include "Event.h"
#include "EventListener.h"
#include "EventNames.h"
#include "InternalModuleRegistry.h"
#include "JSMessagePort.h"
#include "MessagePortPipe.h"
#include "ScriptExecutionContext.h"
#include "SerializedScriptValue.h"
#include "WebCoreJSBuiltins.h"
#include "ZigGlobalObject.h"
#include <JavaScriptCore/CustomGetterSetter.h>
#include <JavaScriptCore/JSArray.h>
#include <JavaScriptCore/JSMap.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMallocInlines.h>

namespace Bun {

using namespace JSC;
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(NodeWorkerState);

NodeWorkerState::NodeWorkerState() = default;
NodeWorkerState::~NodeWorkerState() = default;

// Stands on a port for as long as the worker has not built what receives that port's messages:
// process.stdin (the parent writes to worker.stdin) or internal/worker/messaging (a thread calls
// postMessageToThread). The first message builds it. What is built takes this listener off the
// port (stopListeningForArrival) and attaches its own, which that message is then handed to. A
// worker that gets no such message builds neither.
class NodeWorkerPortArrivalListener final : public EventListener {
public:
    static Ref<NodeWorkerPortArrivalListener> create(NodeWorkerState::NodePort port)
    {
        return adoptRef(*new NodeWorkerPortArrivalListener(port));
    }

    bool operator==(const EventListener& other) const final { return this == &other; }

    void handleEvent(ScriptExecutionContext& context, Event& event) final
    {
        auto* globalObject = defaultGlobalObject(context.globalObject());
        auto* state = globalObject ? globalObject->nodeWorkerState() : nullptr;
        RefPtr port = state ? state->port(m_port) : nullptr;
        if (!port)
            return;
        Ref protectedThis { *this };
        EventTarget& target = *port;

        auto& vm = globalObject->vm();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        if (m_port == NodeWorkerState::NodePort::Stdin)
            state->stdioStream(*globalObject, 0);
        else
            globalObject->internalModuleRegistry()->requireId(globalObject, vm, InternalModuleRegistry::Field::InternalWorkerMessaging);
        if (auto* exception = scope.exception()) [[unlikely]] {
            if (!vm.isTerminationException(exception)) {
                scope.clearException();
                Zig::GlobalObject::reportUncaughtExceptionAtEventLoop(globalObject, exception);
            }
            return;
        }

        // A listener added during a dispatch does not see the event being dispatched.
        EventListenerVector listeners = target.eventListeners(eventNames().messageEvent);
        for (auto& registered : listeners) {
            if (registered->wasRemoved())
                continue;
            registered->callback().handleEvent(context, event);
        }
    }

private:
    explicit NodeWorkerPortArrivalListener(NodeWorkerState::NodePort port)
        : EventListener(NativeEventListenerType)
        , m_port(port)
    {
    }

    const NodeWorkerState::NodePort m_port;
};

static void listenForArrival(Zig::GlobalObject& globalObject, MessagePort& port, NodeWorkerState::NodePort which)
{
    // Unref before listening: a 'message' listener refs the loop, and neither an unread stdin nor the
    // hub port may keep the thread alive.
    port.jsUnref(&globalObject);
    static_cast<EventTarget&>(port).addEventListener(eventNames().messageEvent, NodeWorkerPortArrivalListener::create(which), {});
}

void NodeWorkerState::stopListeningForArrival(NodePort which)
{
    RefPtr port = m_ports[which];
    if (!port)
        return;
    EventTarget& target = *port;
    EventListenerVector listeners = target.eventListeners(eventNames().messageEvent);
    for (auto& registered : listeners) {
        if (registered->callback().type() == EventListener::NativeEventListenerType)
            target.removeEventListener(eventNames().messageEvent, registered->callback(), {});
    }
}

void NodeWorkerState::takeFromParent(Zig::GlobalObject& globalObject, WorkerOptions& options)
{
    auto& vm = globalObject.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& context = *globalObject.scriptExecutionContext();
    m_tookFromParent = true;

    if (auto transferred = std::exchange(options.nodePorts, std::nullopt)) {
        for (unsigned i = 0; i < NodePort::NodePortCount; i++) {
            if ((*transferred)[i].pipe)
                m_ports[i] = MessagePort::entangle(context, WTF::move((*transferred)[i]));
        }

        // Started now so that what the parent posts is delivered as soon as the entry has evaluated
        // and a 'message' listener exists. Without a listener the port holds no loop ref.
        auto& parent = *m_ports[NodePort::Parent];
        parent.holdStartUntilEntrySettled();
        parent.start();

        auto& hub = *m_ports[NodePort::Hub];
        hub.holdStartUntilEntrySettled();
        listenForArrival(globalObject, hub, NodePort::Hub);

        // The parent's write to worker.stdin completes when the worker's reader acks it, whether
        // or not the worker ever reads process.stdin.
        if (auto& stdinPort = m_ports[NodePort::Stdin])
            listenForArrival(globalObject, *stdinPort, NodePort::Stdin);

        // `process` normally does not exist yet and installs these when it is made.
        if (globalObject.hasProcessObject())
            globalObject.processObject()->installNodeWorkerStdioFlushListeners(vm);

        // The process.nextTick callbacks that the entry queues run after the microtasks it queues
        // (Node's order for an ES module entry). The global arms a one-shot hook that would run
        // them first (ZigGlobalObject.cpp).
        vm.setOnEachMicrotaskTick(nullptr);
    }

    // Everything the parent transferred is entangled here even if the worker never reads workerData:
    // its peers see 'close' when this thread exits.
    if (RefPtr serialized = WTF::move(options.workerDataAndEnvironmentData)) {
        auto ports = MessagePort::entanglePorts(context, WTF::move(options.dataMessagePorts));
        JSValue deserialized = serialized->deserialize(globalObject, &globalObject, WTF::move(ports));
        RETURN_IF_EXCEPTION(scope, );
        // Always an Array of length 2, from the constructor in JSWorker.cpp.
        if (auto* pair = dynamicDowncast<JSArray>(deserialized)) {
            ASSERT(pair->length() == 2 && pair->canGetIndexQuickly(0u) && pair->canGetIndexQuickly(1u));
            m_workerData.set(vm, &globalObject, pair->getIndexQuickly(0));
            // Workers that this one starts inherit it, whether or not node:worker_threads is loaded here.
            if (auto* environmentData = dynamicDowncast<JSMap>(pair->getIndexQuickly(1)))
                globalObject.setNodeWorkerEnvironmentData(environmentData);
        }
    }

    if (options.workerDataHasJSTransferables) {
        scope.release();
        globalObject.internalModuleRegistry()->requireId(&globalObject, vm, InternalModuleRegistry::Field::NodeWorkerThreads);
    }
}

void NodeWorkerState::entryDidSettle(Zig::GlobalObject& globalObject)
{
    if (std::exchange(m_entrySettled, true))
        return;

    // parentPort starts delivering now (whatever the parent posted meanwhile is buffered in the pipe).
    if (RefPtr parent = m_ports[NodePort::Parent])
        parent->entrySettled();

    RefPtr hub = m_ports[NodePort::Hub];
    if (!hub)
        return;
    hub->entrySettled();
    if (!MessagePortPipe::queuedCount(hub->pipe()->state(hub->side())))
        return;

    // What other threads sent with postMessageToThread() while the entry was loading is answered on
    // this stack, not by the loop: an entry that failed is about to stop the worker, and each
    // sender is waiting for its reply. A failed entry can leave its exception pending here; the
    // replies cannot observe it and shutdown reports it either way.
    auto& vm = globalObject.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    CLEAR_IF_EXCEPTION(scope);
    if (vm.hasPendingTerminationException())
        return;
    hub->deliverQueuedMessages();
    CLEAR_IF_EXCEPTION(scope);
}

JSValue NodeWorkerState::stdioStream(Zig::GlobalObject& globalObject, unsigned fd)
{
    ASSERT(fd < 3 && hasChannels());
    if (auto* stream = m_stdioStreams[fd].get())
        return stream;

    auto& vm = globalObject.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RefPtr port = m_ports[fd];

    auto* build = JSFunction::create(vm, &globalObject, processObjectInternalsGetNodeWorkerStdioStreamCodeGenerator(vm), &globalObject);
    MarkedArgumentBuffer args;
    args.append(jsNumber(fd));
    args.append(port ? toJS(&globalObject, &globalObject, *port) : jsUndefined());
    JSValue result = JSC::profiledCall(&globalObject, ProfilingReason::API, build, JSC::getCallData(build), globalObject.globalThis(), args);
    RETURN_IF_EXCEPTION(scope, {});

    JSObject* stream = nullptr;
    if (fd == 0) {
        stream = asObject(result);
        // The reader listens for itself from here on.
        stopListeningForArrival(NodePort::Stdin);
    } else {
        // [stream, flush], as getStdioWriteStream returns [stream, sink].
        auto* pair = uncheckedDowncast<JSArray>(result);
        stream = asObject(pair->getIndexQuickly(0));
        m_stdioFlush[fd - 1].set(vm, &globalObject, asObject(pair->getIndexQuickly(1)));
    }
    m_stdioStreams[fd].set(vm, &globalObject, stream);
    return stream;
}

void NodeWorkerState::flushStdio(Zig::GlobalObject& globalObject, unsigned fd)
{
    ASSERT(fd == 1 || fd == 2);
    JSObject* flush = m_stdioFlush[fd - 1].get();
    if (!flush)
        return;
    MarkedArgumentBuffer args;
    JSC::profiledCall(&globalObject, ProfilingReason::API, flush, JSC::getCallData(flush), jsUndefined(), args);
}

JSValue NodeWorkerState::console(VM& vm, Zig::GlobalObject& globalObject, bool& isFinal)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* nativeConsole = m_nativeConsole.get();
    // Before the start sequence nothing is decided, and the build of the Console may itself read
    // `console`: both see the console the global was born with.
    isFinal = m_tookFromParent && !m_isBuildingConsole;
    // A Worker from the global constructor keeps that one.
    if (!isFinal || !hasChannels())
        return nativeConsole;
    SetForScope building { m_isBuildingConsole, true };

    JSValue stdoutStream = stdioStream(globalObject, 1);
    RETURN_IF_EXCEPTION(scope, {});
    JSValue stderrStream = stdioStream(globalObject, 2);
    RETURN_IF_EXCEPTION(scope, {});
    JSValue consoleConstructor = nativeConsole->get(&globalObject, Identifier::fromString(vm, "Console"_s));
    RETURN_IF_EXCEPTION(scope, {});
    // { stdout, stderr } without a prototype: user code has run by now, and the constructor must
    // not take its other options from what that put on Object.prototype.
    JSObject* options = constructEmptyObject(vm, globalObject.nullPrototypeObjectStructure());
    options->putDirect(vm, Identifier::fromString(vm, "stdout"_s), stdoutStream);
    options->putDirect(vm, Identifier::fromString(vm, "stderr"_s), stderrStream);
    MarkedArgumentBuffer args;
    args.append(options);
    RELEASE_AND_RETURN(scope, JSC::construct(&globalObject, consoleConstructor, args, "console.Console is not a constructor"_s));
}

JSC_DEFINE_CUSTOM_GETTER(nodeWorkerConsoleGetter, (JSGlobalObject * lexicalGlobalObject, EncodedJSValue thisValue, PropertyName propertyName))
{
    auto& vm = JSC::getVM(lexicalGlobalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* globalObject = dynamicDowncast<Zig::GlobalObject>(JSValue::decode(thisValue));
    auto* nodeWorker = globalObject ? globalObject->nodeWorkerState() : nullptr;
    if (!nodeWorker) [[unlikely]]
        return JSValue::encode(jsUndefined());
    bool isFinal = false;
    JSValue console = nodeWorker->console(vm, *globalObject, isFinal);
    RETURN_IF_EXCEPTION(scope, {});
    if (isFinal)
        globalObject->putDirect(vm, propertyName, console, PropertyAttribute::DontEnum | 0);
    return JSValue::encode(console);
}

void NodeWorkerState::installConsoleSlot(VM& vm, Zig::GlobalObject& globalObject, PropertyName consoleName, JSObject* nativeConsole)
{
    m_nativeConsole.set(vm, &globalObject, nativeConsole);
    globalObject.putDirectCustomAccessor(vm, consoleName, CustomGetterSetter::create(vm, nodeWorkerConsoleGetter, nullptr), PropertyAttribute::DontEnum | PropertyAttribute::CustomValue);
    // node:console is `export default console`: the console that has `.Console`, whichever of the
    // two is read first.
    auto* registry = globalObject.internalModuleRegistry();
    registry->internalField(InternalModuleRegistry::Field::NodeConsole).set(vm, registry, nativeConsole);
}

void NodeWorkerState::setMessagePortPrototypes(VM& vm, Zig::GlobalObject& globalObject, JSObject* prototype, JSObject* emitterPrototype)
{
    m_messagePortPrototype.set(vm, &globalObject, prototype);
    m_messagePortEmitterPrototype.set(vm, &globalObject, emitterPrototype);
    m_hasMessagePortMemberSlots = true;
}

void NodeWorkerState::setMessagePortNativeClose(VM& vm, Zig::GlobalObject& globalObject, JSObject* close)
{
    m_messagePortNativeClose.set(vm, &globalObject, close);
}

template<typename Visitor>
void NodeWorkerState::visit(Visitor& visitor)
{
    visitor.append(m_workerData);
    visitor.append(m_stdioStreams.begin(), m_stdioStreams.end());
    visitor.append(m_stdioFlush.begin(), m_stdioFlush.end());
    visitor.append(m_nativeConsole);
    visitor.append(m_messagePortPrototype);
    visitor.append(m_messagePortEmitterPrototype);
    visitor.append(m_messagePortNativeClose);
}

template void NodeWorkerState::visit(JSC::AbstractSlotVisitor&);
template void NodeWorkerState::visit(JSC::SlotVisitor&);

} // namespace Bun
