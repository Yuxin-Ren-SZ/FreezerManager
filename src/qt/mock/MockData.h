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

// ── Deterministic UUIDs ────────────────────────────────────────────
inline constexpr const char* kLab   = "f1000000-0000-4000-8000-000000000001";
inline constexpr const char* kUser0 = "f1000000-0000-4000-8000-00000000a001";
inline constexpr const char* kUser1 = "f1000000-0000-4000-8000-00000000a002";
inline constexpr const char* kUser2 = "f1000000-0000-4000-8000-00000000a003";

// ── Freezer count ──────────────────────────────────────────────────
inline constexpr int kFreezerCount = 8;
inline constexpr int kBoxTypeCount = 3;
inline constexpr int kBoxCount     = 12;
inline constexpr int kSampleCount  = 60;

// ── Fake freezers ──────────────────────────────────────────────────
struct FreezerSpec { const char* name; const char* location; const char* model; double temp; };
inline std::vector<Freezer> makeFreezers() {
  const FreezerSpec specs[] = {
    {"Ultra-Low A",     "Bldg 1, Rm 101", "Thermo TSX600",   -80},
    {"Ultra-Low B",     "Bldg 1, Rm 101", "Thermo TSX600",   -80},
    {"LN2 Dewar 1",     "Bldg 2, Rm 203", "CryoDiff LS-750", -196},
    {"LN2 Dewar 2",     "Bldg 2, Rm 203", "CryoDiff LS-750", -196},
    {"Walk-in Freezer", "Bldg 3, Rm B02", "Nor-Lake KL",     -20},
    {"Chest Freezer 3", "Bldg 1, Rm 105", "Thermo TSC 300",  -80},
    {"Fridge A",        "Bldg 4, Rm 301", "Labcold RLDF",    4},
    {"Fridge B",        "Bldg 4, Rm 302", "Liebherr LKexv",  4},
  };
  std::vector<Freezer> fv;
  char buf[64];
  for (int i = 0; i < kFreezerCount; ++i) {
    Freezer f;
    snprintf(buf, sizeof(buf), "f1000000-0000-4000-a000-0000000%05d", i);
    f.id = FreezerId::parse(buf);
    f.lab_id = LabId::parse(kLab);
    f.name = specs[i].name;
    f.location = specs[i].location;
    f.model = specs[i].model;
    f.temp_target_c = specs[i].temp;
    snprintf(buf, sizeof(buf), "f1000000-0000-4000-e000-0000000%05d", i);
    f.layout_root_id = StorageContainerId::parse(buf);
    f.created_at = Timestamp::from_unix_micros(1700000000000000ULL);
    fv.push_back(std::move(f));
  }
  return fv;
}

// ── Fake box types ─────────────────────────────────────────────────
inline std::vector<BoxType> makeBoxTypes() {
  std::vector<BoxType> btv;
  const char* ids[] = {
    "f1000000-0000-4000-c000-000000000001",  // 9x9
    "f1000000-0000-4000-c000-000000000002",  // 10x10
    "f1000000-0000-4000-c000-000000000003",  // 5x5 mixed
  };
  const char* names[] = {"9x9 Cryobox", "10x10 Cryobox", "5x5 Mixed Format"};
  const int dims[] = {9, 10, 5};
  const bool mixed[] = {false, false, true};

  for (int t = 0; t < kBoxTypeCount; ++t) {
    BoxType bt;
    bt.id = BoxTypeId::parse(ids[t]);
    bt.lab_id = LabId::parse(kLab);
    bt.name = names[t];
    bt.manufacturer = (t < 2) ? "Thermo Scientific" : "Eppendorf";
    bt.sku = (t < 2) ? "CRYO-" + std::string(dims[t] == 9 ? "9X9" : "10X10") : "EP-MIX5X5";
    for (int r = 0; r < dims[t]; ++r) {
      for (int c = 0; c < dims[t]; ++c) {
        Position pos;
        pos.label = std::string(1, char('A' + r)) + std::to_string(c + 1);
        pos.row = r;
        pos.col = c;
        if (mixed[t]) {
          // Top 2 rows for 50mL, middle row for 15mL, bottom 2 for 1.5mL
          if (r < 2)       pos.accepts = {"tube_50mL"};
          else if (r < 3)  pos.accepts = {"tube_15mL"};
          else              pos.accepts = {"cryovial_1.5mL", "cryovial_2mL"};
        } else {
          pos.accepts = {"cryovial_1.5mL", "cryovial_2mL"};
        }
        bt.positions.push_back(std::move(pos));
      }
    }
    bt.created_at = Timestamp::from_unix_micros(1700000000000000ULL);
    btv.push_back(std::move(bt));
  }
  return btv;
}

// ── Fake boxes ─────────────────────────────────────────────────────
inline std::vector<Box> makeBoxes() {
  // Assign boxes to freezers: [0,1]->9x9, [2,3]->10x10, [4,5,6]->5x5, [7,8,9,10,11]->9x9
  const int btIdx[] = {0,0, 1,1, 2,2,2, 0,0,0,0,0};
  char buf[64];
  std::vector<Box> bv;
  for (int i = 0; i < kBoxCount; ++i) {
    Box b;
    snprintf(buf, sizeof(buf), "f1000000-0000-4000-d000-0000000%05d", i);
    b.id = BoxId::parse(buf);
    b.lab_id = LabId::parse(kLab);
    snprintf(buf, sizeof(buf), "f1000000-0000-4000-c000-00000000000%d", btIdx[i] + 1);
    b.box_type_id = BoxTypeId::parse(buf);
    snprintf(buf, sizeof(buf), "f1000000-0000-4000-e000-0000000%05d", i);
    b.storage_container_id = StorageContainerId::parse(buf);
    b.label = "Box-" + std::to_string(i + 1);
    b.created_at = Timestamp::from_unix_micros(1700000000000000ULL);
    bv.push_back(std::move(b));
  }
  return bv;
}

// ── Fake samples ───────────────────────────────────────────────────
inline std::vector<Sample> makeSamples() {
  const char* names[] = {
    "PBMC Donor 001",   "Plasma Donor 001",  "Serum Donor 002",  "DNA Extract 003",
    "RNA Extract 004",  "Whole Blood 005",   "CSF Donor 006",    "Tissue Biopsy 007",
    "Buffy Coat 008",   "Cell Pellet 009",   "Urine Donor 010",  "Saliva Donor 011",
    "Bone Marrow 012",  "Lymph Node 013",    "Tumor Fragment 014","Plasma Donor 015",
    "DNA Extract 016",  "RNA Extract 017",   "PBMC Donor 018",   "Serum Donor 019",
    "Whole Blood 020",  "CSF Donor 021",     "PBMC Donor 022",   "Plasma Donor 023",
    "Serum Donor 024",  "DNA Extract 025",   "RNA Extract 026",  "Whole Blood 027",
    "CSF Donor 028",    "Tissue Biopsy 029", "Buffy Coat 030",   "Cell Pellet 031",
    "Urine Donor 032",  "Saliva Donor 033",  "Bone Marrow 034",  "Lymph Node 035",
    "Tumor Fragment 036","Plasma Donor 037", "DNA Extract 038",  "RNA Extract 039",
    "PBMC Donor 040",   "Serum Donor 041",   "Whole Blood 042",  "CSF Donor 043",
    "Tissue Biopsy 044", "Saliva Donor 045", "Bone Marrow 046",  "Lymph Node 047",
    "PBMC Donor 048",   "Plasma Donor 049",  "DNA Extract 050",  "RNA Extract 051",
    "CSF Donor 052",    "Urine Donor 053",   "Saliva Donor 054", "Liver Biopsy 055",
    "Kidney Tissue 056", "Spleen Sample 057", "Pancreas 058",    "Thyroid 059",
  };

  SampleStatus statuses[] = {
    SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,
    SampleStatus::CheckedOut,SampleStatus::Active,SampleStatus::Active,SampleStatus::Depleted,
    SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,
    SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,SampleStatus::CheckedOut,
    SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,
    SampleStatus::Active,SampleStatus::Depleted,SampleStatus::Active,SampleStatus::Active,
    SampleStatus::Active,SampleStatus::Active,SampleStatus::CheckedOut,SampleStatus::Active,
    SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,
    SampleStatus::Active,SampleStatus::Active,SampleStatus::Depleted,SampleStatus::Active,
    SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,
    SampleStatus::CheckedOut,SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,
    SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,
    SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,
    SampleStatus::Depleted,SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,
    SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,SampleStatus::Active,
  };

  const char* itemTypes[] = {
    "f1000000-0000-4000-8000-000000000011",  // blood
    "f1000000-0000-4000-8000-000000000012",  // tissue
    "f1000000-0000-4000-8000-000000000013",  // fluid
  };

  const int dims[] = {9, 9, 10, 10, 5, 5, 5, 9, 9, 9, 9, 9};
  const int boxBtIdx[] = {0,0, 1,1, 2,2,2, 0,0,0,0,0};

  char buf[64];
  std::vector<Sample> sv;
  for (int i = 0; i < kSampleCount; ++i) {
    Sample s;
    snprintf(buf, sizeof(buf), "f1000000-0000-4000-f000-0000000%05d", i);
    s.id = SampleId::parse(buf);
    s.lab_id = LabId::parse(kLab);
    s.item_type_id = ItemTypeId::parse(itemTypes[i % 3]);
    s.name = names[i];
    {
      char bcBuf[16];
      snprintf(bcBuf, sizeof(bcBuf), "BC-%05d", 20001 + i);
      s.barcode = bcBuf;
    }
    s.status = statuses[i];

    // Assign to box
    int boxIdx = i % kBoxCount;
    int D = dims[boxIdx];  // grid dimension for this box type
    snprintf(buf, sizeof(buf), "f1000000-0000-4000-d000-0000000%05d", boxIdx);
    s.box_id = BoxId::parse(buf);

    int row = (i / kBoxCount) % D;
    int col = (i + boxIdx) % D;
    s.position_label = std::string(1, char('A' + row)) + std::to_string(col + 1);

    s.volume_value = 200 + (i * 137) % 1800;
    s.volume_unit = VolumeUnit::Microliter;
    s.created_by = UserId::parse(kUser0);
    s.last_modified_by = UserId::parse(kUser0);
    s.created_at = Timestamp::from_unix_micros(1700000000000000ULL + i * 1000000000ULL);
    s.last_modified_at = s.created_at;

    // Mock parent-child for every 10th sample
    if (i >= 10 && i % 10 == 0) {
      snprintf(buf, sizeof(buf), "f1000000-0000-4000-f000-0000000%05d", i - 5);
      s.parent_sample_id = SampleId::parse(buf);
    }
    sv.push_back(std::move(s));
  }
  return sv;
}

// ── Fake audit events ──────────────────────────────────────────────
struct MockAuditEvent {
  std::string sampleId;  // index into makeSamples()
  std::string userName;
  CheckoutAction action;
  std::string reason;
  int64_t microsAgo;  // microseconds ago from "now"
  int64_t volumeDelta;
};

inline std::vector<MockAuditEvent> makeAuditEvents() {
  return {
    {"f1000000-0000-4000-f000-0000000000004", "Zhang Wei", CheckoutAction::CheckedOut, "qPCR analysis", 3600000000LL, -50},
    {"f1000000-0000-4000-f000-0000000000004", "Zhang Wei", CheckoutAction::CheckedIn,  "analysis complete", 1800000000LL, 0},
    {"f1000000-0000-4000-f000-0000000000015", "Li Na",     CheckoutAction::CheckedOut, "sequencing prep", 7200000000LL, -100},
    {"f1000000-0000-4000-f000-0000000000007", "Wang Lei",  CheckoutAction::CheckedOut, "depleted sample", 9000000000LL, -200},
    {"f1000000-0000-4000-f000-0000000000007", "Wang Lei",  CheckoutAction::Destroyed,  "consumed", 86400000000LL, 0},
    {"f1000000-0000-4000-f000-0000000000026", "Chen Yi",   CheckoutAction::CheckedOut, "ELISA assay", 14400000000LL, -80},
    {"f1000000-0000-4000-f000-0000000000026", "Chen Yi",   CheckoutAction::CheckedIn,  "assay done", 10800000000LL, 0},
    {"f1000000-0000-4000-f000-0000000000040", "Li Na",     CheckoutAction::CheckedOut, "flow cytometry", 17280000000LL, -150},
    {"f1000000-0000-4000-f000-0000000000021", "Wang Lei",  CheckoutAction::CheckedOut, "discard expired", 25920000000LL, -500},
    {"f1000000-0000-4000-f000-0000000000021", "Wang Lei",  CheckoutAction::Destroyed,  "expired", 25000000000LL, -500},
    {"f1000000-0000-4000-f000-0000000000034", "Chen Yi",   CheckoutAction::CheckedOut, "histology", 34560000000LL, -30},
    {"f1000000-0000-4000-f000-0000000000053", "Zhang Wei", CheckoutAction::CheckedOut, "metabolomics", 43200000000LL, -200},
  };
}

}  // namespace fmgr::qt::mock

#endif  // FMGR_QT_MOCK_MOCKDATA_H
