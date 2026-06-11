// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_MOCK_MOCKDATA_H
#define FMGR_QT_MOCK_MOCKDATA_H

#include "core/box.h"
#include "core/enums.h"
#include "core/freezer.h"
#include "core/ids.h"
#include "core/sample.h"
#include "core/timestamp.h"

#include <string>
#include <vector>

namespace fmgr::qt::mock {

using namespace fmgr::core;

// Deterministic UUID base prefixes (no dynamic formatting).
inline constexpr const char* kLab     = "aaaa0000-bbbb-4000-8000-000000000001";
inline constexpr const char* kUser    = "aaaa0000-bbbb-4000-8000-0000000000aa";
inline constexpr const char* kBoxType = "aaaa0000-bbbb-4000-c000-000000000001";
inline constexpr const char* kStorage = "aaaa0000-bbbb-4000-e000-000000000001";
inline constexpr const char* kItem0   = "aaaa0000-bbbb-4000-8000-000000000011";
inline constexpr const char* kItem1   = "aaaa0000-bbbb-4000-8000-000000000012";
inline constexpr const char* kItem2   = "aaaa0000-bbbb-4000-8000-000000000013";

/// Generates realistic-looking fake data for UI prototyping.
/// All IDs are hardcoded deterministic strings — no runtime hex formatting.

// --- Fake freezers ---
inline std::vector<Freezer> makeFreezers() {
  const char* ids[] = {
    "aaaa0000-bbbb-4000-a000-000000000000",
    "aaaa0000-bbbb-4000-a000-000000000001",
    "aaaa0000-bbbb-4000-a000-000000000002",
    "aaaa0000-bbbb-4000-a000-000000000003",
  };
  const char* roots[] = {
    "aaaa0000-bbbb-4000-e000-000000000000",
    "aaaa0000-bbbb-4000-e000-000000000001",
    "aaaa0000-bbbb-4000-e000-000000000002",
    "aaaa0000-bbbb-4000-e000-000000000003",
  };
  const char* names[] = {"Ultra-Low Freezer A", "LN2 Dewar 1",
                         "-20C Walk-in", "-80C Chest 3"};
  const double temps[] = {-80.0, -196.0, -20.0, -80.0};
  std::vector<Freezer> freezers;
  for (int i = 0; i < 4; ++i) {
    Freezer f;
    f.id = FreezerId::parse(ids[i]);
    f.lab_id = LabId::parse(kLab);
    f.name = names[i];
    f.location = std::string("Building ") + std::to_string((i % 2) + 1) +
                 ", Room 10" + std::to_string(i + 1);
    f.model = (i == 1) ? "CryoDiffusion LS-750" : "Thermo Scientific TSX";
    f.temp_target_c = temps[i];
    f.layout_root_id = StorageContainerId::parse(roots[i]);
    f.created_at = Timestamp::from_unix_micros(1700000000000000 + i * 86400000000);
    freezers.push_back(std::move(f));
  }
  return freezers;
}

// --- Fake box types ---
inline BoxType make9x9BoxType() {
  BoxType bt;
  bt.id = BoxTypeId::parse(kBoxType);
  bt.lab_id = LabId::parse(kLab);
  bt.name = "9x9 Cryobox";
  bt.manufacturer = "Thermo Scientific";
  bt.sku = "CRYO-9X9";
  for (int r = 0; r < 9; ++r) {
    for (int c = 0; c < 9; ++c) {
      Position pos;
      pos.label = std::string(1, static_cast<char>('A' + r)) + std::to_string(c + 1);
      pos.row = r;
      pos.col = c;
      pos.accepts = {"cryovial_1.5mL", "cryovial_2mL"};
      bt.positions.push_back(std::move(pos));
    }
  }
  bt.created_at = Timestamp::from_unix_micros(1700000000000000);
  return bt;
}

// --- Fake boxes ---
inline std::vector<Box> makeBoxes() {
  const char* ids[] = {
    "aaaa0000-bbbb-4000-d000-000000000000",
    "aaaa0000-bbbb-4000-d000-000000000001",
    "aaaa0000-bbbb-4000-d000-000000000002",
    "aaaa0000-bbbb-4000-d000-000000000003",
    "aaaa0000-bbbb-4000-d000-000000000004",
    "aaaa0000-bbbb-4000-d000-000000000005",
  };
  std::vector<Box> boxes;
  for (int i = 0; i < 6; ++i) {
    Box b;
    b.id = BoxId::parse(ids[i]);
    b.lab_id = LabId::parse(kLab);
    b.box_type_id = BoxTypeId::parse(kBoxType);
    b.storage_container_id = StorageContainerId::parse(kStorage);
    b.label = "Box " + std::to_string(i + 1);
    b.created_at = Timestamp::from_unix_micros(1700000000000000);
    boxes.push_back(std::move(b));
  }
  return boxes;
}

// --- Fake samples ---
inline std::vector<Sample> makeSamples() {
  const char* names[] = {
      "PBMC Donor 001",    "Plasma Donor 001",   "Serum Donor 002",
      "DNA Extract 003",   "RNA Extract 004",    "Whole Blood 005",
      "CSF Donor 006",     "Tissue Biopsy 007",  "Buffy Coat 008",
      "Cell Pellet 009",   "Urine Donor 010",    "Saliva Donor 011",
      "Bone Marrow 012",   "Lymph Node 013",     "Tumor Fragment 014",
      "Plasma Donor 015",  "DNA Extract 016",    "RNA Extract 017",
      "PBMC Donor 018",    "Serum Donor 019",    "Whole Blood 020",
      "CSF Donor 021",
  };
  const char* barcodes[] = {
      "BC-10001","BC-10002","BC-10003","BC-10004","BC-10005","BC-10006",
      "BC-10007","BC-10008","BC-10009","BC-10010","BC-10011","BC-10012",
      "BC-10013","BC-10014","BC-10015","BC-10016","BC-10017","BC-10018",
      "BC-10019","BC-10020","BC-10021","BC-10022",
  };
  SampleStatus statuses[] = {
      SampleStatus::Active,      SampleStatus::Active,      SampleStatus::Active,
      SampleStatus::Active,      SampleStatus::CheckedOut,  SampleStatus::Active,
      SampleStatus::Active,      SampleStatus::Depleted,    SampleStatus::Active,
      SampleStatus::Active,      SampleStatus::Active,      SampleStatus::Active,
      SampleStatus::Active,      SampleStatus::Active,      SampleStatus::Active,
      SampleStatus::CheckedOut,  SampleStatus::Active,      SampleStatus::Active,
      SampleStatus::Active,      SampleStatus::Active,      SampleStatus::Active,
      SampleStatus::Depleted,
  };
  const char* itemTypes[] = {kItem0, kItem1, kItem2};
  const char* boxIds[] = {
    "aaaa0000-bbbb-4000-d000-000000000000",
    "aaaa0000-bbbb-4000-d000-000000000001",
    "aaaa0000-bbbb-4000-d000-000000000002",
    "aaaa0000-bbbb-4000-d000-000000000003",
    "aaaa0000-bbbb-4000-d000-000000000004",
    "aaaa0000-bbbb-4000-d000-000000000005",
  };

  std::vector<Sample> samples;
  char uuidBuf[64];
  for (int i = 0; i < 22; ++i) {
    Sample s;
    // snprintf with %04x guarantees 4 hex digits with leading zeros
    snprintf(uuidBuf, sizeof(uuidBuf), "aaaa0000-bbbb-4000-f000-0000%04x0000", i);
    s.id = SampleId::parse(uuidBuf);
    s.lab_id = LabId::parse(kLab);
    s.item_type_id = ItemTypeId::parse(itemTypes[i % 3]);
    s.name = names[i];
    s.barcode = barcodes[i];
    s.box_id = BoxId::parse(boxIds[i % 6]);
    int row = (i % 6) / 3 + (i % 3);
    int col = i % 9;
    s.position_label = std::string(1, static_cast<char>('A' + row)) + std::to_string(col + 1);
    s.volume_value = 500 + (i * 73) % 1500;
    s.volume_unit = VolumeUnit::Microliter;
    s.status = statuses[i];
    s.created_by = UserId::parse(kUser);
    s.last_modified_by = UserId::parse(kUser);
    s.created_at = Timestamp::from_unix_micros(1700000000000000);
    s.last_modified_at = Timestamp::from_unix_micros(1710000000000000);
    samples.push_back(std::move(s));
  }
  return samples;
}

}  // namespace fmgr::qt::mock

#endif  // FMGR_QT_MOCK_MOCKDATA_H
