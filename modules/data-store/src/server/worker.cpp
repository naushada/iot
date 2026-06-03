#include "worker.hpp"

#include "data_store.hpp"
#include "data_store/proto.hpp"
#include "proto/value_json.hpp"
#include "schema.hpp"
#include "server.hpp"
#include "session.hpp"
#include "worker_pool.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

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

/// Encode an EMP response carrying optional JSON body and a status.
/// Body of "" + status Ok is a bare ack; non-Ok statuses are paired
/// with `{"err":"..."}` for human-readable diagnostics.
std::string encode_response(proto::Op    op,
                            std::uint8_t reqID,
                            proto::Status st,
                            const json&  body) {
    std::string out;
    const std::string s = body.is_null() ? std::string{} : body.dump();
    proto::encode_frame_response(op, reqID, st,
                                 std::string_view(s.data(), s.size()), out);
    return out;
}

std::string encode_error(proto::Op    op,
                         std::uint8_t reqID,
                         proto::Status st,
                         const std::string& msg) {
    json b;
    b["err"] = msg;
    return encode_response(op, reqID, st, b);
}

/// Encode a NotifyEvent push frame (server → client, no correlation).
std::string encode_notify_push(const std::string& key,
                               const Value&       newValue,
                               const std::optional<Value>& prev) {
    json b;
    b["k"] = key;
    b["v"] = value_to_json(newValue);
    if (prev.has_value()) b["prev"] = value_to_json(*prev);
    else                  b["prev"] = nullptr;
    std::string out;
    const std::string s = b.dump();
    proto::encode_frame_push(proto::Op::NotifyEvent,
                             std::string_view(s.data(), s.size()), out);
    return out;
}

} // namespace

Worker::Worker(std::shared_ptr<DataStore>      store,
               std::shared_ptr<SchemaRegistry> schema,
               int                             id)
  : m_store(std::move(store)),
    m_schema(std::move(schema)),
    m_id(id) {
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

    // msg->payload holds the FULL raw EMP frame (header || body).
    // session.cpp already validated payload_size against
    // kMaxPayloadBytes; here we trust the bounds.
    if (msg->payload.size() < proto::kHeaderSize) {
        return; // pathological — drop quietly, reactor will close on next read
    }
    proto::Header h{};
    proto::decode_header(msg->payload.data(), h);
    const char*  body_ptr = msg->payload.data() + proto::kHeaderSize;
    const std::size_t body_len = msg->payload.size() - proto::kHeaderSize;

    // EMP requests carry no status prefix — the JSON body starts at
    // payload offset 0.
    json req;
    if (body_len > 0) {
        try {
            req = json::parse(body_ptr, body_ptr + body_len);
        } catch (const std::exception& e) {
            s->send(encode_error(proto::parse_op(h.cmdID), h.reqID,
                                 proto::Status::BadPayload,
                                 std::string("bad json: ") + e.what()));
            return;
        }
    }

    const proto::Op op = proto::parse_op(h.cmdID);
    if (op == proto::Op::Unknown) {
        s->send(encode_error(op, h.reqID, proto::Status::BadOpcode,
                             "unknown opcode 0x" +
                             [&]{
                                 char b[8];
                                 std::snprintf(b, sizeof(b), "%04x", h.cmdID);
                                 return std::string(b);
                             }()));
        return;
    }

    if (!proto::is_command(h.type)) {
        // Server only consumes commands. Responses / pushes from a
        // client violate the protocol — drop with BadFrame.
        s->send(encode_error(op, h.reqID, proto::Status::BadFrame,
                             "expected command frame"));
        return;
    }

    // Set/Get/RegisterWatch/RemoveWatch carry `keys: [...]` in the
    // body. SchemaDump has no payload — skip the check for it.
    if (op != proto::Op::SchemaDump) {
        if (!req.is_object() || !req.contains("keys") || !req["keys"].is_array()) {
            s->send(encode_error(op, h.reqID, proto::Status::BadPayload,
                                 "missing keys array"));
            return;
        }
    }

    switch (op) {
        case proto::Op::Set: {
            // Each element is a single-key object `{"keyname": <typed-value>}`.
            // Collect notify dispatches; emit after the response is on
            // the wire so the initiating client sees its ack before
            // peers see the push.
            struct Push {
                Session*     watcher;
                std::string  frame;
            };
            std::vector<Push> pushes;

            // L17b — volatile flag: write to in-memory overlay only.
            // The value survives until server restart (not persisted).
            const bool volatile_set = req.value("volatile", false);

            for (const auto& e : req["keys"]) {
                if (!e.is_object() || e.size() != 1) {
                    s->send(encode_error(op, h.reqID,
                                         proto::Status::BadPayload,
                                         "set entry must be a single-key object"));
                    return;
                }
                auto it = e.begin();
                const std::string k = it.key();
                Value v = value_from_json(it.value());
                // REQ-DS-014 / schema check: reject type mismatches
                // before touching the store. Schema is optional; an
                // unknown key returns nullopt → passthrough.
                if (m_schema) {
                    auto err = m_schema->validate_set(k, v);
                    if (err) {
                        s->send(encode_error(op, h.reqID,
                                             proto::Status::SchemaRejected,
                                             *err));
                        return;
                    }
                    // L17c — per-key ACL check.
                    auto acl_err = m_schema->check_write_acl(
                        k, s->peer_uid(), s->peer_gid());
                    if (acl_err) {
                        s->send(encode_error(op, h.reqID,
                                             proto::Status::SchemaRejected,
                                             *acl_err));
                        return;
                    }
                }
                // L17d — rate-limit check. Reject sets within the
                // configured window of the last set on the same key.
                if (m_store->is_rate_limited(k)) {
                    s->send(encode_error(op, h.reqID,
                                         proto::Status::RateLimited,
                                         "rate-limited: " + k));
                    return;
                }
                auto r = volatile_set
                       ? m_store->set_volatile(k, v)
                       : m_store->set(k, v);
                if (!r.changed) continue;
                std::string frame = encode_notify_push(k, v, r.prev);
                for (Session* w : r.watchers) {
                    if (!w) continue;
                    pushes.push_back({w, frame});
                }
            }
            s->send(encode_response(op, h.reqID, proto::Status::Ok, json{}));

            // Notify fan-out: same-Worker → write directly; cross-Worker
            // → enqueue DeliverNotify on the watcher's owner Worker
            // carrying the pre-encoded push frame.
            for (auto& p : pushes) {
                if (p.watcher->owner() == this) {
                    p.watcher->send(p.frame);
                } else if (p.watcher->owner()) {
                    auto* m = new WorkMsg{
                        WorkKind::DeliverNotify, p.watcher, p.frame};
                    p.watcher->owner()->enqueue(m);
                }
            }
            return;
        }

        case proto::Op::Get: {
            json resp;
            resp["data"] = json::array();
            for (const auto& e : req["keys"]) {
                if (!e.is_string()) {
                    s->send(encode_error(op, h.reqID,
                                         proto::Status::BadPayload,
                                         "get keys must be strings"));
                    return;
                }
                const std::string k = e.get<std::string>();
                auto v = m_store->get(k);
                json item;
                item["k"] = k;
                if (v.has_value()) {
                    item["v"] = value_to_json(*v);
                } else if (m_schema) {
                    // Fall back to the schema default when the
                    // key is unset.
                    auto def = m_schema->default_for(k);
                    if (def) item["v"] = value_to_json(*def);
                    else     item["v"] = nullptr;
                } else {
                    item["v"] = nullptr;
                }
                resp["data"].push_back(item);
            }
            s->send(encode_response(op, h.reqID, proto::Status::Ok, resp));
            return;
        }

        case proto::Op::RegisterWatch: {
            for (const auto& e : req["keys"]) {
                if (!e.is_string()) {
                    s->send(encode_error(op, h.reqID,
                                         proto::Status::BadPayload,
                                         "register keys must be strings"));
                    return;
                }
                const std::string k = e.get<std::string>();
                m_store->watch(s, k);
                s->note_watch(k);
            }
            s->send(encode_response(op, h.reqID, proto::Status::Ok, json{}));
            return;
        }

        case proto::Op::RemoveWatch: {
            for (const auto& e : req["keys"]) {
                if (!e.is_string()) {
                    s->send(encode_error(op, h.reqID,
                                         proto::Status::BadPayload,
                                         "remove keys must be strings"));
                    return;
                }
                const std::string k = e.get<std::string>();
                m_store->unwatch(s, k);
                s->note_unwatch(k);
            }
            s->send(encode_response(op, h.reqID, proto::Status::Ok, json{}));
            return;
        }

        case proto::Op::SchemaDump: {
            // L16/D7 — return the loaded schema as a JSON object so
            // ds-cli `svc list` can enumerate services.* without
            // hardcoding a daemon list. Always succeeds with Ok; an
            // empty schema dir produces {"keys":{}, "namespaces":[]}.
            json resp;
            if (m_schema) {
                // SchemaRegistry::dump_json returns a serialized
                // string so the schema header stays free of
                // <nlohmann/json.hpp>. Parse it back here.
                resp = json::parse(m_schema->dump_json());
            } else {
                resp["keys"]       = json::object();
                resp["namespaces"] = json::array();
            }
            s->send(encode_response(op, h.reqID, proto::Status::Ok, resp));
            return;
        }

        case proto::Op::NotifyEvent:
        case proto::Op::Unknown:
            // Server doesn't accept these as commands.
            s->send(encode_error(op, h.reqID, proto::Status::BadOpcode,
                                 "opcode not valid as a request"));
            return;
    }
}

void Worker::handle_deliver_notify(WorkMsg* msg) {
    // Caller (any Worker) packaged an already-encoded EMP push frame
    // destined for one of OUR sessions. We're the sole writer to its
    // socket, so send inline.
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
