// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_MOCK_MOCKDATA_EXT_H
#define FMGR_QT_MOCK_MOCKDATA_EXT_H

/// Extended mock data for the full FreezerManager screens:
/// donors, studies, temperature logs, pick lists, transfer requests,
/// richer sample types, item type definitions, custom field definitions,
/// and lab membership.

#include <QString>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fmgr::qt::mock {

// ── Lab / User ──────────────────────────────────────────────────────
struct MockLab {
    std::string id;
    QString name;
    QString institution;
    QString department;
};

struct MockMember {
    std::string id;
    QString name;
    QString email;
    QString role;  // "LabAdmin" | "Member"
    std::optional<std::string> avatarInitials;
};

inline std::vector<MockLab> makeLabs() {
    return {
        {"lab1", "Chen Lab", "Stanford University", "Molecular Biology Core"},
        {"lab2", "Immunology Dept", "Stanford University", "Clinical Research"},
        {"lab3", "Zhang Group", "Peking University", "Oncology Research"},
    };
}

inline std::vector<MockMember> makeMembers() {
    return {
        {"u1", "Dr. Li Wei", "liwei@stanford.edu", "LabAdmin", "LW"},
        {"u2", "Zhang Na", "zna@stanford.edu", "Member", "ZN"},
        {"u3", "Wang Lei", "wlei@stanford.edu", "Member", "WL"},
        {"u4", "Chen Yi", "cyi@stanford.edu", "Member", "CY"},
        {"u5", "Liu Fang", "lfang@stanford.edu", "LabAdmin", "LF"},
    };
}

// ── Donors / Subjects (PHI-aware) ───────────────────────────────────
struct MockDonor {
    std::string id;
    QString donorCode;       // e.g. "DON-001"
    QString fullName;        // PHI field
    QString mrn;             // PHI: medical record number
    QString dob;             // PHI: date of birth
    QString sex;
    QString consentStatus;   // "Consented" | "Withdrawn" | "Pending"
    QString irbProtocol;
    QString enrolledStudy;
    int sampleCount;
};

inline std::vector<MockDonor> makeDonors() {
    return {
        {"don1", "DON-001", "Alice Johnson",   "MRN-10001", "1985-03-12", "F", "Consented", "IRB-2024-001", "Immunology Study", 8},
        {"don2", "DON-002", "Bob Smith",       "MRN-10002", "1978-07-24", "M", "Consented", "IRB-2024-001", "Immunology Study", 5},
        {"don3", "DON-003", "Carol Davis",     "MRN-10003", "1990-01-15", "F", "Consented", "IRB-2024-002", "Oncology Trial",   12},
        {"don4", "DON-004", "David Lee",       "MRN-10004", "1982-11-08", "M", "Withdrawn", "IRB-2024-001", "Immunology Study", 3},
        {"don5", "DON-005", "Eve Martinez",    "MRN-10005", "1995-05-20", "F", "Consented", "IRB-2024-003", "Metabolomics",     6},
        {"don6", "DON-006", "Frank Wilson",    "MRN-10006", "1970-09-03", "M", "Pending",   "IRB-2024-002", "Oncology Trial",   0},
        {"don7", "DON-007", "Grace Kim",       "MRN-10007", "1988-12-30", "F", "Consented", "IRB-2024-001", "Immunology Study", 4},
        {"don8", "DON-008", "Henry Brown",     "MRN-10008", "1975-06-18", "M", "Consented", "IRB-2024-004", "Cardiology",       7},
    };
}

// ── Item Type definitions ───────────────────────────────────────────
struct MockItemType {
    std::string id;
    QString name;
    QString category;        // "Blood", "Tissue", "Fluid", "DNA/RNA", etc.
    std::optional<std::string> parentTypeId;  // for field inheritance
    QStringList acceptedContainers;
};

inline std::vector<MockItemType> makeItemTypes() {
    return {
        {"it1",  "Liquid Biospecimen", "Blood",    {},           {"tube_5mL", "tube_10mL"}},
        {"it2",  "Whole Blood",        "Blood",    "it1",        {"tube_5mL", "tube_10mL"}},
        {"it3",  "Plasma",             "Blood",    "it1",        {"cryovial_1.5mL", "cryovial_2mL"}},
        {"it4",  "Serum",              "Blood",    "it1",        {"cryovial_1.5mL", "cryovial_2mL"}},
        {"it5",  "PBMC",               "Blood",    "it1",        {"cryovial_1.5mL", "cryovial_2mL"}},
        {"it6",  "Buffy Coat",         "Blood",    "it1",        {"cryovial_1.5mL"}},
        {"it7",  "Tissue",             "Tissue",   {},           {"tube_50mL", "cassette"}},
        {"it8",  "Tumor Fragment",     "Tissue",   "it7",        {"tube_50mL", "cassette"}},
        {"it9",  "Lymph Node",         "Tissue",   "it7",        {"tube_50mL"}},
        {"it10", "CSF",                "Fluid",    {},           {"cryovial_1.5mL", "cryovial_2mL"}},
        {"it11", "Urine",              "Fluid",    {},           {"tube_15mL", "tube_50mL"}},
        {"it12", "DNA Extract",        "DNA/RNA",  {},           {"cryovial_0.5mL", "cryovial_1.5mL"}},
        {"it13", "RNA Extract",        "DNA/RNA",  {},           {"cryovial_0.5mL", "cryovial_1.5mL"}},
        {"it14", "gDNA",               "DNA/RNA",  "it12",       {"cryovial_0.5mL", "cryovial_1.5mL"}},
    };
}

// ── Custom Field Definitions ────────────────────────────────────────
struct MockCustomField {
    std::string id;
    QString name;
    QString fieldType;   // "text", "number", "date", "select"
    QString scopeTypeId; // item type this field applies to (empty = all)
    bool required;
    std::optional<QString> options;  // for "select" type, semicolon-separated
};

inline std::vector<MockCustomField> makeCustomFields() {
    return {
        {"cf1",  "Donor ID",         "text",   "",     true,  {}},
        {"cf2",  "Draw Date",        "date",   "",     true,  {}},
        {"cf3",  "IRB Protocol",     "text",   "",     true,  {}},
        {"cf4",  "Anticoagulant",    "select", "it2",  true,  "EDTA;Heparin;Citrate;None"},
        {"cf5",  "Viability %",      "number", "it5",  false, {}},
        {"cf6",  "Cell Count",       "number", "it5",  false, {}},
        {"cf7",  "Tumor Grade",      "select", "it8",  false, "I;II;III;IV"},
        {"cf8",  "Fixative",         "select", "it7",  true,  "Formalin;FFPE;Fresh Frozen;RNA Later"},
        {"cf9",  "Concentration",    "number", "it12", false, {}},
        {"cf10", "A260/A280 Ratio",  "number", "it12", false, {}},
        {"cf11", "RIN Value",        "number", "it13", false, {}},
        {"cf12", "Notes",            "text",   "",     false, {}},
    };
}

// ── Studies / Protocols ─────────────────────────────────────────────
struct MockStudy {
    std::string id;
    QString name;
    QString irbProtocol;
    QString piName;
    QString status;         // "Active" | "Completed" | "Suspended"
    int enrolledDonors;
    int totalSamples;
    QString startDate;
    std::optional<QString> endDate;
    QString description;
};

inline std::vector<MockStudy> makeStudies() {
    return {
        {"st1", "Immunology Study",    "IRB-2024-001", "Dr. Li Wei", "Active",    4, 17, "2024-01-15", {},         "Characterizing immune cell populations in healthy donors using PBMC, plasma, and serum profiling."},
        {"st2", "Oncology Trial",      "IRB-2024-002", "Dr. Li Wei", "Active",    2, 12, "2024-03-01", {},         "Evaluating tumor microenvironment markers in solid tumor biopsies."},
        {"st3", "Metabolomics Study",  "IRB-2024-003", "Dr. Liu Fang","Active",   1, 6,  "2024-06-10", {},         "Metabolomic profiling of urine and plasma samples from healthy volunteers."},
        {"st4", "Cardiology Research", "IRB-2024-004", "Dr. Liu Fang","Completed", 1, 7, "2023-09-01", "2024-05-30", "Investigating cardiac biomarkers in blood and tissue samples."},
    };
}

// ── Temperature Logs & Alarms ───────────────────────────────────────
struct MockTempLog {
    std::string id;
    QString freezerName;
    double temperature;
    double targetTemp;
    QString timestamp;      // ISO-like
    QString eventType;      // "reading", "door_open", "defrost", "alarm"
    QString severity;       // "ok", "warning", "critical", "info"
};

inline std::vector<MockTempLog> makeTempLogs() {
    return {
        // Recent readings for Ultra-Low A
        {"tl1",  "Ultra-Low A", -79.8, -80, "2026-06-14T08:00", "reading",   "ok"},
        {"tl2",  "Ultra-Low A", -80.1, -80, "2026-06-14T07:45", "reading",   "ok"},
        {"tl3",  "Ultra-Low A", -78.2, -80, "2026-06-14T07:30", "reading",   "ok"},
        {"tl4",  "Ultra-Low A", -81.0, -80, "2026-06-14T07:15", "reading",   "ok"},
        {"tl5",  "Ultra-Low A", -76.5, -80, "2026-06-14T06:30", "door_open", "warning"},
        {"tl6",  "Ultra-Low A", -74.0, -80, "2026-06-14T06:20", "alarm",     "critical"},
        {"tl7",  "Ultra-Low A", -79.5, -80, "2026-06-14T06:00", "reading",   "ok"},
        // LN2 Dewar 1
        {"tl8",  "LN2 Dewar 1", -195.8, -196, "2026-06-14T08:00", "reading", "ok"},
        {"tl9",  "LN2 Dewar 1", -196.0, -196, "2026-06-14T07:00", "reading", "ok"},
        {"tl10", "LN2 Dewar 1", -193.2, -196, "2026-06-14T05:30", "alarm",   "warning"},
        {"tl11", "LN2 Dewar 1", -195.5, -196, "2026-06-14T05:00", "reading", "ok"},
        // Walk-in Freezer
        {"tl12", "Walk-in Freezer", -19.8, -20, "2026-06-14T08:00", "reading",  "ok"},
        {"tl13", "Walk-in Freezer", -20.1, -20, "2026-06-14T07:00", "reading",  "ok"},
        {"tl14", "Walk-in Freezer", -18.5, -20, "2026-06-13T22:00", "defrost",  "info"},
        {"tl15", "Walk-in Freezer", -15.0, -20, "2026-06-13T21:30", "alarm",    "warning"},
        // Chest Freezer 3
        {"tl16", "Chest Freezer 3", -79.9, -80, "2026-06-14T08:00", "reading", "ok"},
        {"tl17", "Chest Freezer 3", -80.0, -80, "2026-06-14T07:00", "reading", "ok"},
    };
}

// ── Pick Lists / Worklists ──────────────────────────────────────────
struct MockPickItem {
    std::string sampleId;   // sample name for mock simplicity
    QString sampleName;
    QString position;       // e.g. "Ultra-Low A → Rack 2 → Box-1 → A3"
    QString barcode;
    QString status;
    int quantity;
};

struct MockPickList {
    std::string id;
    QString name;
    QString createdBy;
    QString createdAt;
    std::vector<MockPickItem> items;
    QString optimizedRoute;  // "Ultra-Low A → LN2 Dewar 1 → Walk-in Freezer"
};

inline std::vector<MockPickList> makePickLists() {
    MockPickList pl1;
    pl1.id = "pl1";
    pl1.name = "qPCR Run — Plate 3";
    pl1.createdBy = "Zhang Wei";
    pl1.createdAt = "2026-06-14 09:30";
    pl1.items = {
        {"", "PBMC Donor 001",   "Ultra-Low A → Rack 2 → Box-1 → A3", "BC-20001", "Active", 1},
        {"", "Plasma Donor 001", "Ultra-Low A → Rack 2 → Box-1 → B3", "BC-20002", "Active", 1},
        {"", "DNA Extract 003",  "Ultra-Low A → Rack 1 → Box-3 → A1", "BC-20004", "Active", 2},
        {"", "RNA Extract 004",  "LN2 Dewar 1 → Box-4 → B1",          "BC-20005", "Checked Out", 1},
        {"", "Whole Blood 005",  "Walk-in Freezer → Box-5 → A1",        "BC-20006", "Active", 1},
    };
    pl1.optimizedRoute = "Ultra-Low A (3 items) → LN2 Dewar 1 (1 item) → Walk-in Freezer (1 item)";

    MockPickList pl2;
    pl2.id = "pl2";
    pl2.name = "ELISA Assay — Batch 2";
    pl2.createdBy = "Chen Yi";
    pl2.createdAt = "2026-06-13 14:15";
    pl2.items = {
        {"", "Serum Donor 002",   "Ultra-Low A → Rack 1 → Box-2 → C1", "BC-20003", "Active", 2},
        {"", "Plasma Donor 015",  "Ultra-Low B → Rack 1 → Box-2 → A2", "BC-20015", "Active", 1},
    };
    pl2.optimizedRoute = "Ultra-Low A (1 item) → Ultra-Low B (1 item)";

    return {pl1, pl2};
}

// ── Transfer Requests ───────────────────────────────────────────────
struct MockTransferRequest {
    std::string id;
    QString sampleName;
    QString fromLab;
    QString toLab;
    QString requestedBy;
    QString requestedAt;
    QString status;          // "Pending", "Approved", "Rejected", "Completed"
    QString reason;
    std::optional<QString> approvedBy;
    std::optional<QString> approvedAt;
};

inline std::vector<MockTransferRequest> makeTransferRequests() {
    return {
        {"tr1", "PBMC Donor 022",  "Chen Lab", "Immunology Dept", "Dr. Li Wei",    "2026-06-12", "Pending",   "Collaborative study — shared IRB", {}, {}},
        {"tr2", "Tumor Fragment 014","Chen Lab","Zhang Group",     "Wang Lei",      "2026-06-10", "Approved",  "Pathology review request", "Dr. Liu Fang", "2026-06-11"},
        {"tr3", "CSF Donor 028",    "Chen Lab", "Immunology Dept", "Chen Yi",       "2026-06-09", "Completed", "Biomarker analysis", "Dr. Li Wei", "2026-06-09"},
        {"tr4", "Plasma Donor 037", "Chen Lab", "Zhang Group",     "Zhang Na",      "2026-06-08", "Rejected",  "Sample volume insufficient", "Dr. Liu Fang", "2026-06-09"},
        {"tr5", "Liver Biopsy 055", "Chen Lab", "Zhang Group",     "Dr. Li Wei",    "2026-06-14", "Pending",   "Multi-center validation — IRB-2024-002", {}, {}},
    };
}

// ── Dashboard Activity Feed ─────────────────────────────────────────
struct MockActivity {
    QString icon;    // emoji or text indicator
    QString user;
    QString action;
    QString detail;
    QString time;
};

inline std::vector<MockActivity> makeActivities() {
    return {
        {"\xF0\x9F\x94\xB5", "Zhang Wei", "checked out", "PBMC Donor 001",     "10 min ago"},
        {"\xF0\x9F\x9F\xA2", "Li Na",     "added",       "Plasma Donor 015",   "32 min ago"},
        {"\xF0\x9F\x94\xB4", "Wang Lei",  "depleted",     "Buffy Coat 008",     "1 hour ago"},
        {"\xF0\x9F\x9F\xA1", "Zhang Wei", "checked out", "Whole Blood 005",    "2 hours ago"},
        {"\xF0\x9F\x94\xB5", "Chen Yi",   "returned",    "DNA Extract 003",    "3 hours ago"},
        {"\xF0\x9F\x9F\xA2", "Li Na",     "added",       "CSF Donor 021",      "5 hours ago"},
        {"\xF0\x9F\x94\xB4", "Zhang Wei", "checked out", "RNA Extract 026",    "6 hours ago"},
        {"\xF0\x9F\x9F\xA1", "Wang Lei",  "aliquoted",   "Serum Donor 019",    "8 hours ago"},
        {"\xF0\x9F\x94\xB5", "Chen Yi",   "transferred", "Tumor Fragment 014",  "1 day ago"},
        {"\xF0\x9F\x9F\xA2", "Li Na",     "imported",    "12 samples via CSV", "2 days ago"},
    };
}

// ── Freezer utilization for dashboard ───────────────────────────────
struct MockFreezerUtil {
    QString name;
    double temp;
    double fillPct;
    int totalSlots;
    int occupiedSlots;
};

inline std::vector<MockFreezerUtil> makeFreezerUtil() {
    return {
        {"Ultra-Low A",     -80,  72, 810, 583},
        {"Ultra-Low B",     -80,  48, 810, 389},
        {"LN2 Dewar 1",     -196, 45, 500, 225},
        {"LN2 Dewar 2",     -196, 22, 500, 110},
        {"Walk-in Freezer", -20,  88, 200, 176},
        {"Chest Freezer 3", -80,  31, 300, 93},
        {"Fridge A",         4,   55, 150, 83},
    };
}

}  // namespace fmgr::qt::mock

#endif
