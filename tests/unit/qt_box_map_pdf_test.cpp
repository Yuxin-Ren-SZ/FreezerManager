// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/BoxMapPdf.h"

#include <memory>
#include <string>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <QByteArray>
#include <QDate>
#include <QPageSize>

#include "fmgr/v1/box.grpc.pb.h"
#include "fmgr/v1/sample.grpc.pb.h"
#include "qt/BoxServiceClient.h"
#include "qt/SampleServiceClient.h"

namespace {

  using fmgr::qt::BoxMapPdf;
  using fmgr::qt::BoxServiceClient;
  using fmgr::qt::SampleServiceClient;

  // QPdfWriter flate-compresses page *content* streams, so the glyphs drawn by
  // QPainter are not greppable in the raw bytes. Text assertions therefore run
  // against BoxMapPdf::buildModel() (the structured layout the renderer paints),
  // while the PDF-byte tests assert only on the uncompressed container envelope
  // (the %PDF magic and the page /MediaBox). System Qt6 here ships no Qt6::Pdf,
  // so QPdfDocument text extraction is unavailable by design.

  // 8×12 box "bt-1" labelled rows A..H, cols 1..12 (A1..H12). One sample sits at
  // A1; everything else is free. `empty` collapses the type to zero positions.
  class FakeBoxService final : public fmgr::v1::BoxService::Service {
  public:
    bool empty = false;
    bool fail_get_box = false;
    bool fail_list_types = false;
    bool unknown_type = false; // GetBox reports a box_type_id absent from the list
    bool empty_label = false;  // GetBox returns no label → title falls back to id

    grpc::Status GetBox(grpc::ServerContext* /*ctx*/, const fmgr::v1::GetBoxRequest* req,
                        fmgr::v1::GetBoxResponse* resp) override {
      if (fail_get_box) {
        return {grpc::StatusCode::NOT_FOUND, "no such box"};
      }
      fmgr::v1::Box* b = resp->mutable_box();
      b->set_id(req->box_id());
      b->set_lab_id("lab-1");
      b->set_box_type_id(unknown_type ? "bt-missing" : "bt-1");
      if (!empty_label) {
        b->set_label("Tumor Bank Box 7");
      }
      return grpc::Status::OK;
    }

    grpc::Status ListBoxTypes(grpc::ServerContext* /*ctx*/,
                              const fmgr::v1::ListBoxTypesRequest* /*req*/,
                              fmgr::v1::ListBoxTypesResponse* resp) override {
      if (fail_list_types) {
        return {grpc::StatusCode::PERMISSION_DENIED, "denied"};
      }
      fmgr::v1::BoxType* bt = resp->add_box_types();
      bt->set_id("bt-1");
      if (empty) {
        return grpc::Status::OK;
      }
      for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 12; ++col) {
          fmgr::v1::BoxPosition* p = bt->add_positions();
          p->set_label(std::string(1, static_cast<char>('A' + row)) + std::to_string(col + 1));
          p->set_row(row);
          p->set_col(col);
        }
      }
      return grpc::Status::OK;
    }
  };

  class FakeSampleService final : public fmgr::v1::SampleService::Service {
  public:
    bool fail_list = false;

    grpc::Status ListSamples(grpc::ServerContext* /*ctx*/,
                             const fmgr::v1::ListSamplesRequest* /*req*/,
                             fmgr::v1::ListSamplesResponse* resp) override {
      if (fail_list) {
        return {grpc::StatusCode::UNAVAILABLE, "samples down"};
      }
      fmgr::v1::Sample* s = resp->add_samples();
      s->set_id("s-1");
      s->set_name("HeLa P12");
      s->set_box_id("box-1");
      s->set_position_label("A1");
      return grpc::Status::OK;
    }
  };

  class BoxMapPdfTest : public ::testing::Test {
  protected:
    void SetUp() override {
      grpc::ServerBuilder builder;
      builder.RegisterService(&box_service_);
      builder.RegisterService(&sample_service_);
      server_ = builder.BuildAndStart();
      ASSERT_NE(server_, nullptr);
      auto channel = server_->InProcessChannel(grpc::ChannelArguments());
      boxes_ = std::make_unique<BoxServiceClient>(fmgr::v1::BoxService::NewStub(channel));
      samples_ = std::make_unique<SampleServiceClient>(fmgr::v1::SampleService::NewStub(channel));
      pdf_ = std::make_unique<BoxMapPdf>(boxes_.get(), samples_.get());
    }

    void TearDown() override {
      if (server_) {
        server_->Shutdown();
        server_->Wait();
      }
    }

    BoxMapPdf::Model model() {
      return pdf_->buildModel(QStringLiteral("box-1"), QStringLiteral("lab-1"),
                              QStringLiteral("tok"), QDate(2026, 6, 30));
    }

    const BoxMapPdf::Cell* cell(const BoxMapPdf::Model& m, const QString& label) {
      for (const auto& c : m.cells) {
        if (c.position_label == label) {
          return &c;
        }
      }
      return nullptr;
    }

    FakeBoxService box_service_;
    FakeSampleService sample_service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<BoxServiceClient> boxes_;
    std::unique_ptr<SampleServiceClient> samples_;
    std::unique_ptr<BoxMapPdf> pdf_;
  };

  TEST_F(BoxMapPdfTest, GeneratesNonEmptyPdf) {
    const QByteArray bytes = pdf_->generate(QStringLiteral("box-1"), QStringLiteral("lab-1"),
                                            QStringLiteral("tok"), QDate(2026, 6, 30));
    EXPECT_FALSE(bytes.isEmpty());
    EXPECT_TRUE(bytes.startsWith("%PDF"));
  }

  TEST_F(BoxMapPdfTest, RendersBoxTitleAndMetadata) {
    const auto m = model();
    ASSERT_TRUE(m.ok);
    EXPECT_TRUE(m.title.contains(QStringLiteral("Tumor Bank Box 7")));
    // Dimensions and date land in the subtitle line.
    EXPECT_TRUE(m.subtitle.contains(QStringLiteral("8")));
    EXPECT_TRUE(m.subtitle.contains(QStringLiteral("12")));
    EXPECT_TRUE(m.subtitle.contains(QStringLiteral("2026-06-30")));
  }

  TEST_F(BoxMapPdfTest, RendersAllPositionLabels) {
    const auto m = model();
    ASSERT_TRUE(m.ok);
    EXPECT_EQ(m.cells.size(), 96u);
    for (int row = 0; row < 8; ++row) {
      for (int col = 0; col < 12; ++col) {
        const QString label = QString(QChar('A' + row)) + QString::number(col + 1);
        EXPECT_NE(cell(m, label), nullptr) << label.toStdString();
      }
    }
  }

  TEST_F(BoxMapPdfTest, RendersOccupiedPositionsWithSampleName) {
    const auto m = model();
    const auto* a1 = cell(m, QStringLiteral("A1"));
    ASSERT_NE(a1, nullptr);
    EXPECT_TRUE(a1->occupied);
    EXPECT_EQ(a1->sample_name, QStringLiteral("HeLa P12"));
  }

  TEST_F(BoxMapPdfTest, RendersEmptyPositionsAsBlank) {
    const auto m = model();
    const auto* h12 = cell(m, QStringLiteral("H12"));
    ASSERT_NE(h12, nullptr);
    EXPECT_FALSE(h12->occupied);
    EXPECT_TRUE(h12->sample_name.isEmpty());
  }

  TEST_F(BoxMapPdfTest, HandlesEmptyBox) {
    box_service_.empty = true;
    const auto m = model();
    EXPECT_TRUE(m.ok);
    EXPECT_TRUE(m.cells.empty());
    const QByteArray bytes = pdf_->generate(QStringLiteral("box-1"), QStringLiteral("lab-1"),
                                            QStringLiteral("tok"), QDate(2026, 6, 30));
    EXPECT_TRUE(bytes.startsWith("%PDF"));
  }

  TEST_F(BoxMapPdfTest, RespectsPageSize) {
    // Default is US Letter — 612×792 pt, surfaced in the page /MediaBox.
    const QByteArray bytes = pdf_->generate(QStringLiteral("box-1"), QStringLiteral("lab-1"),
                                            QStringLiteral("tok"), QDate(2026, 6, 30));
    EXPECT_TRUE(bytes.contains("MediaBox"));
    EXPECT_TRUE(bytes.contains("792"));

    pdf_->setPageSize(QPageSize(QPageSize::A4));
    const QByteArray a4 = pdf_->generate(QStringLiteral("box-1"), QStringLiteral("lab-1"),
                                         QStringLiteral("tok"), QDate(2026, 6, 30));
    // A4 is 842 pt tall — distinct from Letter, proving the size is honoured.
    EXPECT_TRUE(a4.contains("842"));
  }

  TEST_F(BoxMapPdfTest, GetBoxFailureYieldsErrorModelAndEmptyPdf) {
    box_service_.fail_get_box = true;
    const auto m = model();
    EXPECT_FALSE(m.ok);
    EXPECT_EQ(m.error, "no such box");

    const QByteArray bytes = pdf_->generate(QStringLiteral("box-1"), QStringLiteral("lab-1"),
                                            QStringLiteral("tok"), QDate(2026, 6, 30));
    EXPECT_TRUE(bytes.isEmpty());
  }

  TEST_F(BoxMapPdfTest, ListBoxTypesFailureYieldsLayoutError) {
    box_service_.fail_list_types = true;
    const auto m = model();
    EXPECT_FALSE(m.ok);
    EXPECT_EQ(m.error, "failed to resolve box layout");

    const QByteArray bytes = pdf_->generate(QStringLiteral("box-1"), QStringLiteral("lab-1"),
                                            QStringLiteral("tok"), QDate(2026, 6, 30));
    EXPECT_TRUE(bytes.isEmpty());
  }

  TEST_F(BoxMapPdfTest, UnknownBoxTypeYieldsLayoutError) {
    box_service_.unknown_type = true;
    const auto m = model();
    EXPECT_FALSE(m.ok);
    EXPECT_EQ(m.error, "failed to resolve box layout");
  }

  TEST_F(BoxMapPdfTest, EmptyLabelFallsBackToBoxId) {
    box_service_.empty_label = true;
    const auto m = model();
    ASSERT_TRUE(m.ok);
    EXPECT_EQ(m.title, QStringLiteral("box-1"));
  }

  // A ListSamples failure propagates out of rebuildOccupants → setBox() false →
  // buildModel reports the layout error rather than a half-populated grid.
  TEST_F(BoxMapPdfTest, SampleListingFailureYieldsLayoutError) {
    sample_service_.fail_list = true;
    const auto m = model();
    EXPECT_FALSE(m.ok);
    EXPECT_EQ(m.error, "failed to resolve box layout");
  }

} // namespace
