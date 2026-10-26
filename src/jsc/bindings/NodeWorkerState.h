#pragma once

#include "root.h"
#include "MessagePort.h"
#include "WorkerOptions.h"
#include <JavaScriptCore/WriteBarrier.h>
#include <wtf/TZoneMalloc.h>

namespace Zig {
class GlobalObject;
}

namespace Bun {

// The worker side of a `Worker` made by node:worker_threads: what its parent sent (workerData, the
// stdio / postMessageToThread / parentPort channel ends) and what the worker has built over it.
//
// One per Kind::Node default global, null on every other. The start sequence fills it without
// running builtin JS (takeFromParent). Everything that needs a builtin module is built on first
// use: process.stdin / stdout / stderr and console from here, the rest of node:worker_threads when
// the module is required, when a MessagePort member it defines is read, or (its messaging half)
// when a postMessageToThread() message arrives.
class NodeWorkerState {
    WTF_MAKE_TZONE_ALLOCATED(NodeWorkerState);
    WTF_MAKE_NONCOPYABLE(NodeWorkerState);

public:
    using NodePort = WebCore::WorkerOptions::NodePort;

    NodeWorkerState();
    ~NodeWorkerState();

    // Before preloads and the entry point. Throws what deserializing workerData throws.
    void takeFromParent(Zig::GlobalObject&, WebCore::WorkerOptions&);
    // The entry module finished (or failed) its top-level evaluation.
    void entryDidSettle(Zig::GlobalObject&);

    // False before takeFromParent, and for a Worker that the global constructor made with three
    // arguments: its stdio is the process's, its console the native one, and it has no parentPort.
    bool hasChannels() const { return !!m_ports[NodePort::Stdout]; }
    WebCore::MessagePort* port(NodePort which) const { return m_ports[which].get(); }
    // Until the worker builds what reads the Stdin / Hub port, a native listener stands on it and
    // builds that on the first message (NodeWorkerState.cpp). What is built listens for itself.
    void stopListeningForArrival(NodePort);

    // process.stdin (0) / stdout (1) / stderr (2) over the parent's ports, built on first use.
    // Throws and stays unbuilt when the build fails, so a later read tries again.
    JSC::JSValue stdioStream(Zig::GlobalObject&, unsigned fd);
    // The body of the process 'exit' listener of stdout (1) / stderr (2): no loop turn is left for
    // the parent's ack of a batch, so what waits behind it is posted now (node's flushSync).
    void flushStdio(Zig::GlobalObject&, unsigned fd);

    // node routes a worker's console through its process.stdout / process.stderr, so the parent's
    // worker.stdout sees it. Bun's console writes the fds, so the global `console` of a node worker
    // is a Console over stdioStream(1) and (2) instead, built on the first read of the property.
    // The console the global was born with stays what require("node:console") is.
    // The property is a slot until then: a failed build (the stack limit) throws to whoever read
    // `console`, and the next read builds again.
    void installConsoleSlot(JSC::VM&, Zig::GlobalObject&, JSC::PropertyName, JSC::JSObject* nativeConsole);
    // What the slot reads as, and whether that is final. Throws when the Console cannot be built.
    JSC::JSValue console(JSC::VM&, Zig::GlobalObject&, bool& isFinal);

    // workerData as deserialized by takeFromParent (empty before it). node:worker_threads exports it.
    JSC::JSValue workerData() const { return m_workerData.get(); }

    // MessagePort.prototype has node's emitter methods, close(callback) and the util.inspect hook in
    // a node worker from the start, as in Node. node:worker_threads defines them, so until it is
    // evaluated each one is a slot that loads it (JSMessagePort.cpp).
    JSC::JSObject* messagePortPrototype() const { return m_messagePortPrototype.get(); }
    JSC::JSObject* messagePortEmitterPrototype() const { return m_messagePortEmitterPrototype.get(); }
    void setMessagePortPrototypes(JSC::VM&, Zig::GlobalObject&, JSC::JSObject* prototype, JSC::JSObject* emitterPrototype);
    // The close() that node:worker_threads wraps, once the slot in front of it is gone.
    JSC::JSObject* messagePortNativeClose() const { return m_messagePortNativeClose.get(); }
    void setMessagePortNativeClose(JSC::VM&, Zig::GlobalObject&, JSC::JSObject*);
    bool hasMessagePortMemberSlots() const { return m_hasMessagePortMemberSlots; }
    void setHasMessagePortMemberSlots(bool has) { m_hasMessagePortMemberSlots = has; }
    bool isLoadingMessagePortMembers() const { return m_isLoadingMessagePortMembers; }
    void setIsLoadingMessagePortMembers(bool loading) { m_isLoadingMessagePortMembers = loading; }

    template<typename Visitor> void visit(Visitor&);

private:
    // Worker thread only.
    std::array<RefPtr<WebCore::MessagePort>, NodePort::NodePortCount> m_ports;
    bool m_tookFromParent { false };
    bool m_entrySettled { false };
    bool m_isBuildingConsole { false };
    bool m_hasMessagePortMemberSlots { false };
    bool m_isLoadingMessagePortMembers { false };

    // The global is the owner of these barriers and visits them.
    JSC::WriteBarrier<JSC::Unknown> m_workerData;
    std::array<JSC::WriteBarrier<JSC::JSObject>, 3> m_stdioStreams;
    // For stdout and stderr: completes the writev that is waiting for the parent's ack.
    std::array<JSC::WriteBarrier<JSC::JSObject>, 2> m_stdioFlush;
    JSC::WriteBarrier<JSC::JSObject> m_nativeConsole;
    JSC::WriteBarrier<JSC::JSObject> m_messagePortPrototype;
    JSC::WriteBarrier<JSC::JSObject> m_messagePortEmitterPrototype;
    JSC::WriteBarrier<JSC::JSObject> m_messagePortNativeClose;
};

} // namespace Bun
