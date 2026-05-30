#include "worker.hpp"

#include "data_store.hpp"
#include "data_store/proto.hpp"
#include "server.hpp"
#include "session.hpp"
#include "worker_pool.hpp"

#include <algorithm>
#include <cstring>

#include <ace/Log_Msg.h>
#include <ace/Message_Block.h>

#include "nlohmann/json.hpp"

namespace data_store::server {

namespace {

using nlohmann::json;

ACE_Message_Block* make_block(WorkMsg* msg) {
    auto* mb = new ACE_Message_Block(sizeof(WorkMsg*));
    *reinterpret_cast<WorkMsg**>(mb->wr_ptr()) = msg;
    mb->wr_ptr(sizeof(WorkMsg*));
    return mb;
}

WorkMsg* unwrap_block(ACE_Message_Block* mb) {
    return *reinterpret_cast<WorkMsg**>(mb->rd_ptr());
}

/// Build a `{"ok":false,"id":...,"err":"..."}` response line.
std::string error_response(const json& reqId, const std::string& msg) {
    json r;
    r["ok"]  = false;
    if (!reqId.is_null()) r["id"] = reqId;
    r["err"] = msg;
    return r.dump() + "\n";
}

/// Build a `{"ev":"changed","k":"...","v":"...","prev":...}` line.
std::string notify_line(const std::string& key,
                        const std::string& newValue,
                        const std::optional<std::string>& prev) {
    json r;
    r["ev"] = "changed";
    r["k"]  = key;
    r["v"]  = newValue;
    if (prev.has_value()) r["prev"] = *prev;
    else                  r["prev"] = nullptr;
    return r.dump() + "\n";
}

} // namespace

Worker::Worker(std::shared_ptr<DataStore> store, int id)
  : m_store(std::move(store)), m_id(id) {
}

Worker::~Worker() {
    close();
}

int Worker::open(void* /*args*/) {
    if (activate(THR_NEW_LWP | THR_JOINABLE, 1) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [Worker:%t] %M %N:%l worker %d "
                                   "activate failed errno=%d\n"),
                          m_id, errno),
                         -1);
    }
    return 0;
}

int Worker::enqueue(WorkMsg* msg) {
    if (!msg) return -1;
    ACE_Message_Block* mb = make_block(msg);
    if (putq(mb) == -1) {
        mb->release();
        delete msg;
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [Worker:%t] %M %N:%l worker %d "
                                   "putq failed errno=%d\n"),
                          m_id, errno),
                         -1);
    }
    return 0;
}

int Worker::svc() {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [Worker:%t] %M %N:%l worker %d started\n"),
               m_id));

    while (true) {
        ACE_Message_Block* mb = nullptr;
        if (getq(mb) == -1) break;
        WorkMsg* msg = unwrap_block(mb);
        mb->release();
        if (!msg) continue;

        switch (msg->kind) {
            case WorkKind::ProcessRequest: handle_process_request(msg); break;
            case WorkKind::DeliverNotify:  handle_deliver_notify(msg);  break;
            case WorkKind::SessionClosed:  handle_session_closed(msg);  break;
        }
        delete msg;
    }

    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [Worker:%t] %M %N:%l worker %d exiting\n"),
               m_id));
    return 0;
}

int Worker::close(u_long /*flags*/) {
    msg_queue()->deactivate();
    wait();

    // Drain leftover messages so we don't leak Sessions.
    ACE_Message_Block* mb = nullptr;
    while (msg_queue()->dequeue_head(mb,
            const_cast<ACE_Time_Value*>(&ACE_Time_Value::zero)) != -1) {
        if (!mb) break;
        WorkMsg* msg = unwrap_block(mb);
        mb->release();
        if (msg && msg->kind == WorkKind::SessionClosed && msg->session) {
            delete msg->session;
        }
        delete msg;
    }
    return 0;
}

// ----------------------------- handlers -----------------------------

void Worker::handle_process_request(WorkMsg* msg) {
    Session* s = msg->session;
    if (!s) return;

    json req;
    json reqId;
    try {
        req = json::parse(msg->payload);
        if (req.contains("id")) reqId = req["id"];
    } catch (const std::exception& e) {
        s->send(error_response(reqId, std::string("bad json: ") + e.what()));
        return;
    }

    if (!req.is_object() || !req.contains("op") || !req["op"].is_string()) {
        s->send(error_response(reqId, "missing op"));
        return;
    }

    const std::string opStr = req["op"].get<std::string>();
    const proto::Op   op    = proto::parse_op(opStr);
    if (op == proto::Op::Unknown) {
        s->send(error_response(reqId, "unknown op '" + opStr + "'"));
        return;
    }
    if (!req.contains("keys") || !req["keys"].is_array()) {
        s->send(error_response(reqId, "missing keys array"));
        return;
    }

    json resp;
    resp["ok"] = true;
    if (!reqId.is_null()) resp["id"] = reqId;

    switch (op) {
        case proto::Op::Set: {
            // Each element is `{"k":"...","v":"..."}`. Collect notify
            // dispatches; emit after the response is on the wire so the
            // initiating client sees its ack before peers see the push.
            struct Push {
                Session*                 watcher;
                std::string              line;
            };
            std::vector<Push> pushes;

            for (const auto& e : req["keys"]) {
                if (!e.is_object() || !e.contains("k") || !e.contains("v") ||
                    !e["k"].is_string() || !e["v"].is_string()) {
                    s->send(error_response(reqId, "set entry needs k+v strings"));
                    return;
                }
                const std::string k = e["k"].get<std::string>();
                const std::string v = e["v"].get<std::string>();
                auto r = m_store->set(k, v);
                if (!r.changed) continue;
                std::string line = notify_line(k, v, r.prev);
                for (Session* w : r.watchers) {
                    if (!w) continue;
                    pushes.push_back({w, line});
                }
            }
            s->send(resp.dump() + "\n");

            // Notify fan-out: same-Worker → write directly; cross-Worker
            // → enqueue DeliverNotify on the watcher's owner Worker.
            for (auto& p : pushes) {
                if (p.watcher->owner() == this) {
                    p.watcher->send(p.line);
                } else if (p.watcher->owner()) {
                    auto* m = new WorkMsg{
                        WorkKind::DeliverNotify, p.watcher, p.line};
                    p.watcher->owner()->enqueue(m);
                }
            }
            return;
        }

        case proto::Op::Get: {
            resp["data"] = json::array();
            for (const auto& e : req["keys"]) {
                if (!e.is_string()) {
                    s->send(error_response(reqId, "get keys must be strings"));
                    return;
                }
                const std::string k = e.get<std::string>();
                auto v = m_store->get(k);
                json item;
                item["k"] = k;
                if (v.has_value()) item["v"] = *v;
                else               item["v"] = nullptr;
                resp["data"].push_back(item);
            }
            s->send(resp.dump() + "\n");
            return;
        }

        case proto::Op::Register: {
            for (const auto& e : req["keys"]) {
                if (!e.is_string()) {
                    s->send(error_response(reqId, "register keys must be strings"));
                    return;
                }
                const std::string k = e.get<std::string>();
                m_store->watch(s, k);
                s->note_watch(k);
            }
            s->send(resp.dump() + "\n");
            return;
        }

        case proto::Op::Remove: {
            for (const auto& e : req["keys"]) {
                if (!e.is_string()) {
                    s->send(error_response(reqId, "remove keys must be strings"));
                    return;
                }
                const std::string k = e.get<std::string>();
                m_store->unwatch(s, k);
                s->note_unwatch(k);
            }
            s->send(resp.dump() + "\n");
            return;
        }

        case proto::Op::Unknown:
            // Unreachable — already handled above.
            return;
    }
}

void Worker::handle_deliver_notify(WorkMsg* msg) {
    // Caller (any Worker) packaged a notify destined for one of OUR
    // sessions. We're the sole writer to its socket, so send inline.
    if (msg->session) {
        msg->session->send(msg->payload);
    }
}

void Worker::handle_session_closed(WorkMsg* msg) {
    if (!msg->session) return;
    // Reactor already removed the handler; drop watches and delete
    // the Session. Any in-flight DeliverNotify aimed at this Session
    // that's still in our queue will hit a nullptr/dead pointer —
    // mitigated by the drain in close() and by the fact that on a
    // clean disconnect the reactor-thread notify-then-delete order
    // means new DeliverNotifies stop coming before this fires.
    m_store->unwatch_all(msg->session);
    delete msg->session;
}

} // namespace data_store::server
