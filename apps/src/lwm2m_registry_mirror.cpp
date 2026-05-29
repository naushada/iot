#include "lwm2m_registry_mirror.hpp"

#include <ace/Log_Msg.h>
#include <chrono>
#include <thread>

namespace lwm2m {

RegistryMirror::RegistryMirror(DbClient* db) : m_db(db) {}

RegistryMirror::~RegistryMirror() {
    stop();
}

int RegistryMirror::open(void* /*args*/) {
    if (activate(THR_NEW_LWP | THR_JOINABLE, 1) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [RegistryMirror:%t] %M %N:%l activate failed\n")),
                         -1);
    }
    return 0;
}

int RegistryMirror::post(RegistryEvent ev) {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_queue.size() >= kHighWater) {
            m_dropped.fetch_add(1);
            return -1;     // RISK-06: drop mirror, log, keep in-memory copy
        }
        m_queue.push(std::move(ev));
    }
    m_enqueued.fetch_add(1);
    return 0;
}

void RegistryMirror::stop() {
    if (m_stop.exchange(true)) return;
    msg_queue()->deactivate();
    wait();
}

int RegistryMirror::svc() {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [RegistryMirror:%t] %M %N:%l worker started\n")));

    while (!m_stop.load()) {
        RegistryEvent ev;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (!m_queue.empty()) {
                ev = std::move(m_queue.front());
                m_queue.pop();
                have = true;
            }
        }
        if (have) {
            persist(ev);
            m_flushed.fetch_add(1);
        } else {
            // No work; sleep briefly. A condition_variable would be tidier
            // but the queue is rarely empty for long under load — keep it
            // simple for L3 and revisit in L9 perf tuning.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [RegistryMirror:%t] %M %N:%l worker exiting "
                        "(enqueued=%Q flushed=%Q dropped=%Q)\n"),
               m_enqueued.load(), m_flushed.load(), m_dropped.load()));
    return 0;
}

void RegistryMirror::persist(const RegistryEvent& ev) {
    // L3 stub. The Mongo document schema is its own (small) PR; the
    // skeleton below is what we expect to land:
    //
    //   collection: lwm2m_registrations
    //   document:
    //     {
    //       _id          : ObjectId(),
    //       location     : "/rd/123",
    //       endpoint     : "urn:dev:ops:...",
    //       short_server_id : 1,
    //       lifetime_s   : 86400,
    //       binding      : "U",
    //       sms_number   : "",
    //       lwm2m_version: "1.1",
    //       peer_host    : "10.0.0.5",
    //       peer_port    : 56830,
    //       advertised   : [ ... link entries ... ],
    //       registered_at: ISODate(),
    //       expires_at   : ISODate(),
    //     }
    //
    //   Add    → DbClient::create_document(coll, json)
    //   Update → DbClient::update_collection(coll, {location:loc}, $set:json)
    //   Remove → DbClient::delete_document(coll, {location:loc})
    //
    // We deliberately do not call DbClient methods yet because that drags
    // mongo-cxx into a unit-test build that has no mongod available.
    // RDD REQ-REG-010 will be re-tested with the real wiring landed.
    (void)ev;
    (void)m_db;
}

std::vector<ServerRegistration> RegistryMirror::reconstruct() {
    // L3 stub matching `persist`. The intended read path:
    //   1. DbClient::get_documents("lwm2m_registrations", "{}", projection)
    //   2. For each doc: parse the JSON above into ServerRegistration.
    //   3. Drop documents whose expires_at <= now (no point reviving the
    //      already-expired).
    //   4. Return the surviving set; ClientRegistry::load_from() installs
    //      them.
    return {};
}

} // namespace lwm2m
