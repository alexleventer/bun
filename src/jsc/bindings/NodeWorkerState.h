#pragma once

#include "root.h"
#include "MessagePort.h"
#include "WorkerOptions.h"
#include <JavaScriptCore/WriteBarrier.h>
#include <wtf/TZoneMalloc.h>

namespace Zig {
class GlobalObject;
}

namespace WebCore {
class Event;
class EventListener;
}

namespace Bun {

// The worker side of a node:worker_threads Worker: what its parent sent, and the stdio that the worker builds over it on first use. Only the default global of such a worker has one.
class NodeWorkerState {
    WTF_MAKE_TZONE_ALLOCATED(NodeWorkerState);
    WTF_MAKE_NONCOPYABLE(NodeWorkerState);

public:
    using NodePort = WebCore::WorkerOptions::NodePort;

    NodeWorkerState();
    ~NodeWorkerState();

    // The worker's start sequence, before preloads and the entry point. It evaluates no builtin module. Throws what deserializing workerData throws.
    void takeFromParent(Zig::GlobalObject&, WebCore::WorkerOptions&);
    void entryDidSettle(Zig::GlobalObject&);

    WebCore::MessagePort* port(NodePort which) const { return m_ports[which].get(); }
    JSC::JSValue workerData() const { return m_workerData.get(); }
    // Whether the worker's env had a NODE_CHANNEL_FD when the worker started. Node then disables the IPC surface of `process`.
    bool envNamesIpcChannel() const { return m_envNamesIpcChannel; }
    void setEnvNamesIpcChannel(bool value) { m_envNamesIpcChannel = value; }

    // process.stdin (0), stdout (1) or stderr (2). A build that throws is not remembered: the next call builds again.
    JSC::JSValue stdioStream(Zig::GlobalObject&, unsigned fd);
    // One console call's output for fd 1 or 2. It goes through process.stdout / stderr once the worker has built that, and from here to the same port until then.
    void writeConsole(Zig::GlobalObject&, unsigned fd, std::span<const uint8_t>);
    // node's flushSync: a synchronous exit leaves no loop turn for the parent's ack, so what waits for one is posted now.
    void flushStdioForExit(Zig::GlobalObject&);

    void stopListening(NodePort);
    void didReceiveMessage(Zig::GlobalObject&, NodePort, WebCore::ScriptExecutionContext&, WebCore::Event&);

    template<typename Visitor> void visit(Visitor&);

private:
    void listen(NodePort);
    void postConsoleOutput(Zig::GlobalObject&, unsigned fd, std::span<const uint8_t>);
    // Posts what waits for an ack without one, and stops waiting.
    void releaseConsoleQueue(Zig::GlobalObject&, unsigned fd);

    // The flow control of internal/worker/stdio: one batch is in flight, and what is written meanwhile waits for the reader's ack.
    struct ConsoleQueue {
        Vector<uint8_t> waiting;
        bool isAwaitingAck { false };
    };

    std::array<RefPtr<WebCore::MessagePort>, NodePort::NodePortCount> m_ports;
    // On a port until the worker builds what reads it: for the first message to stdin or from the postMessageToThread hub, and for the acks of console output.
    std::array<RefPtr<WebCore::EventListener>, NodePort::NodePortCount> m_listeners;
    std::array<ConsoleQueue, 2> m_consoleQueues;
    bool m_entrySettled { false };
    bool m_isExiting { false };
    bool m_envNamesIpcChannel { false };

    JSC::WriteBarrier<JSC::Unknown> m_workerData;
    std::array<JSC::WriteBarrier<JSC::JSObject>, 3> m_stdioStreams;
    // Of process.stdout and stderr (internal/worker/stdio): what completes the writev that waits for an ack, and what writes console output to the stream.
    std::array<JSC::WriteBarrier<JSC::JSObject>, 2> m_stdioFlush;
    std::array<JSC::WriteBarrier<JSC::JSObject>, 2> m_consoleWrite;
};

} // namespace Bun
