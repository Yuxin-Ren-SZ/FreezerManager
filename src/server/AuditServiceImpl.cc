// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/AuditServiceImpl.h"

#include "audit/AuditChainVerifier.h"
#include "cli/CsvWriter.h"
#include "core/audit_event.h"
#include "core/permissions.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/AuditTraits.h"
#include "storage/IStorageBackend.h"

#include <fmgr/v1/audit.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fmgr::server {
  namespace {

    // A deployment SystemAdmin is the only principal that holds a global-only
    // permission globally (build_session_context promotes global-only grants to
    // global_permissions for the SystemAdmin role alone). LabProvision is the
    // stable marker for "deployment-wide administrator".
    [[nodiscard]] bool is_system_admin(const auth::SessionContext& sctx) {
      return sctx.has_global(core::Permission::LabProvision);
    }

    // Wall-clock micros, matching how repositories stamp audit rows (see e.g.
    // src/storage/SampleOps.h). Used as the default WatchAuditFeed cursor ("now").
    [[nodiscard]] std::int64_t now_unix_micros() {
      return std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    }

    // WatchAuditFeed poll cadence. The tail loop sleeps one interval between
    // backend polls but wakes every slice to stay responsive to cancellation.
    constexpr std::chrono::milliseconds k_watch_poll_slice{100};
    constexpr int k_watch_poll_slices = 10; // 100ms * 10 = ~1s between polls

    void fill_event(fmgr::v1::AuditEvent* out, const core::AuditEvent& event) {
      out->set_id(event.id.to_string());
      out->mutable_at()->set_unix_micros(event.at.unix_micros());
      out->set_actor_user_id(event.actor_user_id.to_string());
      out->set_actor_session_id(event.actor_session_id);
      if (event.lab_id.has_value()) {
        out->set_lab_id(event.lab_id->to_string());
      }
      out->set_action(event.action);
      out->set_entity_kind(event.entity_kind);
      if (event.entity_id.has_value()) {
        out->set_entity_id(*event.entity_id);
      }
      out->set_before_json(event.before_json);
      out->set_after_json(event.after_json);
      out->set_request_id(event.request_id);
      out->set_prev_hash(event.prev_hash);
      out->set_this_hash(event.this_hash);
    }

    // Tail query for WatchAuditFeed: the same filters as ListAuditEvents plus a
    // `at >= cursor` lower bound, ordered (at, id) so the cursor advances
    // monotonically.
    [[nodiscard]] storage::Query<core::AuditEvent>
    build_watch_query(const fmgr::v1::WatchAuditFeedRequest& req,
                      const std::optional<core::LabId>& lab_filter, std::int64_t cursor_micros) {
      auto query = storage::Query<core::AuditEvent>::all();
      if (lab_filter.has_value()) {
        query = query.and_where(storage::field<core::AuditEvent, std::string>(
                                    core::AuditEvent::Field::LabId) == lab_filter->to_string());
      }
      if (req.has_entity_kind()) {
        query = query.and_where(storage::field<core::AuditEvent, std::string>(
                                    core::AuditEvent::Field::EntityKind) == req.entity_kind());
      }
      if (req.has_entity_id()) {
        query = query.and_where(storage::field<core::AuditEvent, std::string>(
                                    core::AuditEvent::Field::EntityId) == req.entity_id());
      }
      return query
          .and_where(storage::greater_or_equal(
              storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
              core::Timestamp::from_unix_micros(cursor_micros)))
          .order_by(storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
                    storage::SortDirection::Ascending)
          .order_by(storage::field<core::AuditEvent, std::string>(core::AuditEvent::Field::Id),
                    storage::SortDirection::Ascending);
    }

    // Write the rows newer than the cursor, suppressing ids already delivered at
    // the exact cursor microsecond (the query lower bound is `>=`, so a same-micro
    // row can reappear). Advances the cursor and rebuilds the dedup set. Returns
    // false when the client has hung up (Write failed), signalling the caller to
    // end the stream.
    [[nodiscard]] bool emit_new_events(grpc::ServerWriter<fmgr::v1::AuditEvent>& writer,
                                       const std::vector<core::AuditEvent>& events,
                                       std::int64_t& cursor_micros,
                                       std::unordered_set<std::string>& sent_at_cursor) {
      std::int64_t max_at = cursor_micros;
      for (const auto& event : events) {
        const std::int64_t event_at = event.at.unix_micros();
        const std::string id = event.id.to_string();
        if (event_at == cursor_micros && sent_at_cursor.contains(id)) {
          continue;
        }
        fmgr::v1::AuditEvent out;
        fill_event(&out, event);
        if (!writer.Write(out)) {
          return false;
        }
        max_at = std::max(max_at, event_at);
      }
      if (max_at > cursor_micros) {
        cursor_micros = max_at;
        sent_at_cursor.clear();
      }
      for (const auto& event : events) {
        if (event.at.unix_micros() == cursor_micros) {
          sent_at_cursor.insert(event.id.to_string());
        }
      }
      return true;
    }

    // Load every audit row in canonical chain order (at ASC, id ASC), the same
    // ordering the chain hash was computed over. Used by VerifyAuditChain and as
    // the basis for the (small-deployment) export path.
    // TODO(F-6, doc/CODE_REVIEW_2026-06-12.md): stream/chunk for large audit logs;
    // the whole table is materialized in memory here.
    [[nodiscard]] std::vector<core::AuditEvent> load_ordered_events(storage::ITransaction& txn) {
      return txn.repo<core::AuditEvent>().query(
          storage::Query<core::AuditEvent>::all()
              .order_by(
                  storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
                  storage::SortDirection::Ascending)
              .order_by(storage::field<core::AuditEvent, std::string>(core::AuditEvent::Field::Id),
                        storage::SortDirection::Ascending));
    }

    // A page token is an opaque client-supplied integer offset. A malformed token
    // is a client error, not an INTERNAL fault, so translate parse failures to
    // ConstraintViolation -> INVALID_ARGUMENT (see doc/CODE_REVIEW_2026-06-12.md F-4).
    [[nodiscard]] std::size_t parse_page_offset(const std::string& token) {
      try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(token, &consumed);
        if (consumed != token.size()) {
          throw std::invalid_argument("trailing characters");
        }
        return static_cast<std::size_t>(value);
      } catch (const std::exception&) {
        throw storage::ConstraintViolation("invalid page_token");
      }
    }

  } // namespace

  AuditServiceImpl::AuditServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend)
      : auth_(auth), backend_(backend), middleware_(auth) {
    using P = core::Permission;
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuditService/ListAuditEvents", P::AuditRead);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuditService/GetAuditEvent", P::AuditRead);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuditService/VerifyAuditChain", P::AuditRead);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuditService/ExportAuditLog", P::AuditExport);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuditService/WatchAuditFeed", P::AuditRead);
  }

  grpc::Status AuditServiceImpl::ListAuditEvents(grpc::ServerContext* ctx,
                                                 const fmgr::v1::ListAuditEventsRequest* req,
                                                 fmgr::v1::ListAuditEventsResponse* resp) {
    try {
      // Lab-scoped list gates on AuditRead for the lab; an unscoped list spans
      // every lab and is restricted to a deployment SystemAdmin.
      std::optional<core::LabId> lab_filter;
      auth::SessionContext sctx;
      if (req->has_lab_id()) {
        lab_filter = core::LabId::parse(req->lab_id());
        sctx =
            middleware_.authorize(extract_bearer(*ctx), core::Permission::AuditRead, *lab_filter);
      } else {
        sctx = auth_.validate_token(extract_bearer(*ctx));
        if (!sctx.mfa_complete) {
          throw auth::MfaRequired("MFA required before this operation");
        }
        if (!is_system_admin(sctx)) {
          throw auth::PermissionDenied(
              "deployment-wide audit read requires a system administrator");
        }
      }

      auto query = storage::Query<core::AuditEvent>::all();
      if (lab_filter.has_value()) {
        query = query.and_where(storage::field<core::AuditEvent, std::string>(
                                    core::AuditEvent::Field::LabId) == lab_filter->to_string());
      }
      if (req->has_entity_kind()) {
        query = query.and_where(storage::field<core::AuditEvent, std::string>(
                                    core::AuditEvent::Field::EntityKind) == req->entity_kind());
      }
      if (req->has_entity_id()) {
        query = query.and_where(storage::field<core::AuditEvent, std::string>(
                                    core::AuditEvent::Field::EntityId) == req->entity_id());
      }
      if (req->has_since()) {
        query = query.and_where(storage::greater_or_equal(
            storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
            core::Timestamp::from_unix_micros(req->since().unix_micros())));
      }
      if (req->has_until()) {
        query = query.and_where(storage::less_or_equal(
            storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
            core::Timestamp::from_unix_micros(req->until().unix_micros())));
      }
      query =
          query
              .order_by(
                  storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
                  storage::SortDirection::Ascending)
              .order_by(storage::field<core::AuditEvent, std::string>(core::AuditEvent::Field::Id),
                        storage::SortDirection::Ascending);

      // Page token is a plain integer offset; page size 0 means "no limit".
      std::size_t offset = 0;
      if (!req->page().page_token().empty()) {
        offset = parse_page_offset(req->page().page_token());
        query = query.offset(offset);
      }
      const auto page_size = req->page().page_size();
      if (page_size > 0) {
        query = query.limit(static_cast<std::size_t>(page_size));
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto events = txn->repo<core::AuditEvent>().query(query);
      txn->commit();

      for (const auto& event : events) {
        fill_event(resp->add_events(), event);
      }
      if (page_size > 0 && events.size() == static_cast<std::size_t>(page_size)) {
        resp->mutable_page()->set_next_page_token(std::to_string(offset + events.size()));
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status AuditServiceImpl::GetAuditEvent(grpc::ServerContext* ctx,
                                               const fmgr::v1::GetAuditEventRequest* req,
                                               fmgr::v1::GetAuditEventResponse* resp) {
    try {
      const auto event_id = core::AuditEventId::parse(req->audit_event_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto event = txn->repo<core::AuditEvent>().find_by_id(event_id);
      txn->commit();

      if (!event.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "audit event not found"};
      }
      // A lab-scoped row is readable with AuditRead for that lab or by a system
      // admin; a global (lab-less) row is system-admin only.
      const bool allowed =
          is_system_admin(sctx) || (event->lab_id.has_value() &&
                                    sctx.has_for_lab(*event->lab_id, core::Permission::AuditRead));
      if (!allowed) {
        throw auth::PermissionDenied("audit.read required for this event's lab");
      }
      fill_event(resp->mutable_event(), *event);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status AuditServiceImpl::VerifyAuditChain(grpc::ServerContext* ctx,
                                                  const fmgr::v1::VerifyAuditChainRequest* /*req*/,
                                                  fmgr::v1::VerifyAuditChainResponse* resp) {
    try {
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }
      // The chain links every row globally; verifying a per-lab subset is not
      // meaningful, so verification is deployment-wide and system-admin only.
      // (req.lab_id is accepted for forward compatibility but does not scope the
      // walk.)
      if (!is_system_admin(sctx)) {
        throw auth::PermissionDenied("audit chain verification requires a system administrator");
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto events = load_ordered_events(*txn);
      txn->commit();

      const auto report = audit::verify_audit_chain(events);
      resp->set_chain_intact(report.ok);
      resp->set_events_verified(static_cast<std::int64_t>(report.verified_count));
      if (report.ok) {
        resp->set_detail("chain intact");
      } else if (report.first_error.has_value()) {
        resp->set_detail(report.first_error->detail);
      } else {
        resp->set_detail("chain verification failed");
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status AuditServiceImpl::ExportAuditLog(grpc::ServerContext* ctx,
                                                const fmgr::v1::ExportAuditLogRequest* req,
                                                fmgr::v1::ExportAuditLogResponse* resp) {
    try {
      std::optional<core::LabId> lab_filter;
      auth::SessionContext sctx;
      if (req->has_lab_id()) {
        lab_filter = core::LabId::parse(req->lab_id());
        sctx =
            middleware_.authorize(extract_bearer(*ctx), core::Permission::AuditExport, *lab_filter);
      } else {
        sctx = auth_.validate_token(extract_bearer(*ctx));
        if (!sctx.mfa_complete) {
          throw auth::MfaRequired("MFA required before this operation");
        }
        if (!is_system_admin(sctx)) {
          throw auth::PermissionDenied(
              "deployment-wide audit export requires a system administrator");
        }
      }

      auto query = storage::Query<core::AuditEvent>::all();
      if (lab_filter.has_value()) {
        query = query.and_where(storage::field<core::AuditEvent, std::string>(
                                    core::AuditEvent::Field::LabId) == lab_filter->to_string());
      }
      if (req->has_since()) {
        query = query.and_where(storage::greater_or_equal(
            storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
            core::Timestamp::from_unix_micros(req->since().unix_micros())));
      }
      query =
          query
              .order_by(
                  storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
                  storage::SortDirection::Ascending)
              .order_by(storage::field<core::AuditEvent, std::string>(core::AuditEvent::Field::Id),
                        storage::SortDirection::Ascending);

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto events = txn->repo<core::AuditEvent>().query(query);
      txn->commit();

      std::ostringstream out;
      cli::write_csv_row(out, {"id", "at_micros", "actor_user_id", "actor_session_id", "lab_id",
                               "action", "entity_kind", "entity_id", "request_id", "prev_hash",
                               "this_hash"});
      for (const auto& event : events) {
        cli::write_csv_row(
            out, {event.id.to_string(), std::to_string(event.at.unix_micros()),
                  event.actor_user_id.to_string(), event.actor_session_id,
                  event.lab_id.has_value() ? event.lab_id->to_string() : std::string(),
                  event.action, event.entity_kind, event.entity_id.value_or(std::string()),
                  event.request_id, event.prev_hash, event.this_hash});
      }
      resp->set_csv_content(out.str());
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status AuditServiceImpl::WatchAuditFeed(grpc::ServerContext* ctx,
                                                const fmgr::v1::WatchAuditFeedRequest* req,
                                                grpc::ServerWriter<fmgr::v1::AuditEvent>* writer) {
    try {
      // Authorize once, at stream-open, with the same gating as ListAuditEvents:
      // a lab-scoped feed needs AuditRead for that lab; an unscoped feed spans
      // every lab and is system-admin only.
      std::optional<core::LabId> lab_filter;
      auth::SessionContext sctx;
      if (req->has_lab_id()) {
        lab_filter = core::LabId::parse(req->lab_id());
        sctx =
            middleware_.authorize(extract_bearer(*ctx), core::Permission::AuditRead, *lab_filter);
      } else {
        sctx = auth_.validate_token(extract_bearer(*ctx));
        if (!sctx.mfa_complete) {
          throw auth::MfaRequired("MFA required before this operation");
        }
        if (!is_system_admin(sctx)) {
          throw auth::PermissionDenied(
              "deployment-wide audit feed requires a system administrator");
        }
      }

      // Poll-tail cursor. We re-query rows with at >= cursor each cycle (>= not >
      // so newly-appended rows sharing the cursor microsecond are not missed) and
      // suppress ids already emitted at that exact microsecond via `sent_at_cursor`.
      // No LISTEN/NOTIFY yet — this stays portable across SQLite and Postgres.
      std::int64_t cursor_micros =
          req->has_since() ? req->since().unix_micros() : now_unix_micros();
      std::unordered_set<std::string> sent_at_cursor;

      while (!ctx->IsCancelled()) {
        auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
        rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
        const auto events =
            txn->repo<core::AuditEvent>().query(build_watch_query(*req, lab_filter, cursor_micros));
        txn->commit();

        if (!emit_new_events(*writer, events, cursor_micros, sent_at_cursor)) {
          return grpc::Status::OK; // client/gateway hung up
        }

        for (int i = 0; i < k_watch_poll_slices && !ctx->IsCancelled(); ++i) {
          std::this_thread::sleep_for(k_watch_poll_slice);
        }
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
