// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_LOCATIONPATHRESOLVER_H
#define FMGR_QT_LOCATIONPATHRESOLVER_H

#include <string>

#include <QList>
#include <QString>

namespace fmgr::qt {

class BoxServiceClient;

// Resolves a placed sample to its full human-readable location path
// (Freezer → … → Box → Position) for the single-handed lookup module. Walks the
// storage-container parent chain via BoxServiceClient::listStorageContainers and
// names the enclosing freezer by matching its layout_root_id. UI-agnostic (no
// widget deps) so the chain walk — including the cycle/orphan guards — is
// unit-testable headlessly.
class LocationPathResolver {
 public:
  // One rung of the location path. label is what the UI shows; kind lets the UI
  // prefix it ("Freezer", "Rack", …).
  struct PathSegment {
    enum class Kind { Freezer, Container, Box, Position };
    Kind kind = Kind::Container;
    QString label;
    QString detail;  // container kind name (e.g. "Rack"); empty otherwise
  };

  struct Result {
    bool ok = false;      // false only on a hard error (e.g. box not found)
    bool placed = false;  // false when the sample has no box → segments empty
    bool partial = false;  // chain broke (orphan/cycle) → path is best-effort
    std::string error;
    QList<PathSegment> segments;  // ordered freezer → … → box → position
  };

  explicit LocationPathResolver(BoxServiceClient* box_client);

  // Resolve box_id/position_label (a sample's placement) within lab_id. An empty
  // box_id is an unplaced sample (ok + !placed, empty segments).
  Result resolve(const QString& session_token, const QString& lab_id,
                 const QString& box_id, const QString& position_label) const;

 private:
  BoxServiceClient* box_client_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_LOCATIONPATHRESOLVER_H
