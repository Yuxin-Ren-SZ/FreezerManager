// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleLookupWidget.h"

#include <memory>
#include <string>
#include <vector>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
#include <QStackedWidget>

#include "fmgr/v1/box.grpc.pb.h"
#include "fmgr/v1/sample.grpc.pb.h"
#include "qt/BoxServiceClient.h"
#include "qt/SampleServiceClient.h"

namespace {

  using fmgr::qt::BoxServiceClient;
  using fmgr::qt::SampleLookupWidget;
  using fmgr::qt::SampleServiceClient;

  // Resolves barcode/name queries to a configurable sample set. ListSamples honours
  // the barcode filter; an unfiltered call returns every sample (the widget's
  // name-fallback path).
  class FakeSampleService final : public fmgr::v1::SampleService::Service {
  public:
    std::vector<fmgr::v1::Sample> samples;
    // When set, ListSamples returns this gRPC error instead of OK.
    grpc::StatusCode fail_list = grpc::StatusCode::OK;
    std::string fail_message;

    grpc::Status ListSamples(grpc::ServerContext* /*ctx*/, const fmgr::v1::ListSamplesRequest* req,
                             fmgr::v1::ListSamplesResponse* resp) override {
      if (fail_list != grpc::StatusCode::OK) {
        return {fail_list, "injected ListSamples: " + fail_message};
      }
      for (const auto& s : samples) {
        if (req->has_barcode() && s.barcode() != req->barcode()) {
          continue;
        }
        *resp->add_samples() = s;
      }
      return grpc::Status::OK;
    }
  };

  // Minimal BoxService: a single box in a single freezer-root container, enough for
  // the resolver to produce a non-empty path.
  class FakeBoxService final : public fmgr::v1::BoxService::Service {
  public:
    grpc::StatusCode fail_get_box = grpc::StatusCode::OK;
    std::string fail_message;

    grpc::Status GetBox(grpc::ServerContext* /*ctx*/, const fmgr::v1::GetBoxRequest* req,
                        fmgr::v1::GetBoxResponse* resp) override {
      if (fail_get_box != grpc::StatusCode::OK) {
        return {fail_get_box, "injected GetBox: " + fail_message};
      }
      auto* box = resp->mutable_box();
      box->set_id(req->box_id());
      box->set_lab_id("lab-1");
      box->set_storage_container_id("root-1");
      box->set_label("Box 7");
      return grpc::Status::OK;
    }

    grpc::Status ListStorageContainers(grpc::ServerContext* /*ctx*/,
                                       const fmgr::v1::ListStorageContainersRequest* req,
                                       fmgr::v1::ListStorageContainersResponse* resp) override {
      if (!req->has_parent_id()) {
        auto* c = resp->add_containers();
        c->set_id("root-1");
        c->set_lab_id("lab-1");
        c->set_kind(fmgr::v1::CONTAINER_KIND_GENERIC);
        c->set_name("Root");
      }
      return grpc::Status::OK;
    }

    grpc::Status ListFreezers(grpc::ServerContext* /*ctx*/,
                              const fmgr::v1::ListFreezersRequest* /*req*/,
                              fmgr::v1::ListFreezersResponse* resp) override {
      auto* f = resp->add_freezers();
      f->set_id("frz-1");
      f->set_lab_id("lab-1");
      f->set_name("Freezer A");
      f->set_layout_root_id("root-1");
      return grpc::Status::OK;
    }
  };

  fmgr::v1::Sample makeSample(const std::string& id, const std::string& name,
                              const std::string& barcode, const std::string& box_id,
                              const std::string& pos,
                              fmgr::v1::SampleStatus status = fmgr::v1::SAMPLE_STATUS_ACTIVE) {
    fmgr::v1::Sample s;
    s.set_id(id);
    s.set_name(name);
    s.set_barcode(barcode);
    s.set_status(status);
    if (!box_id.empty()) {
      s.set_box_id(box_id);
      s.set_position_label(pos);
    }
    return s;
  }

  class SampleLookupWidgetTest : public ::testing::Test {
  protected:
    void SetUp() override {
      grpc::ServerBuilder builder;
      builder.RegisterService(&sample_service_);
      builder.RegisterService(&box_service_);
      server_ = builder.BuildAndStart();
      ASSERT_NE(server_, nullptr);
      auto channel = server_->InProcessChannel(grpc::ChannelArguments());
      sample_client_ =
          std::make_unique<SampleServiceClient>(fmgr::v1::SampleService::NewStub(channel));
      box_client_ = std::make_unique<BoxServiceClient>(fmgr::v1::BoxService::NewStub(channel));
      widget_ = std::make_unique<SampleLookupWidget>(sample_client_.get(), box_client_.get());
      widget_->setToken(QStringLiteral("tok"));
      widget_->setScope(QStringLiteral("lab-1"));
    }

    void TearDown() override {
      widget_.reset();
      if (server_) {
        server_->Shutdown();
        server_->Wait();
      }
    }

    // Type a query into the input and press Enter.
    void enterQuery(const QString& text) {
      auto* input = widget_->findChild<QLineEdit*>(QStringLiteral("lookupInput"));
      ASSERT_NE(input, nullptr);
      input->setText(text);
      input->returnPressed();
    }

    QStackedWidget* stack() {
      return widget_->findChild<QStackedWidget*>();
    }

    FakeSampleService sample_service_;
    FakeBoxService box_service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<SampleServiceClient> sample_client_;
    std::unique_ptr<BoxServiceClient> box_client_;
    std::unique_ptr<SampleLookupWidget> widget_;
  };

  TEST_F(SampleLookupWidgetTest, EnterTriggersLookupAndShowsPath) {
    sample_service_.samples.push_back(makeSample("s-1", "Plasma A", "BC-1", "box-1", "A1"));

    QSignalSpy spy(widget_.get(), &SampleLookupWidget::sampleSelected);
    enterQuery(QStringLiteral("BC-1"));

    auto* path = widget_->findChild<QWidget*>(QStringLiteral("pathView"));
    ASSERT_NE(path, nullptr);
    const auto labels = path->findChildren<QLabel*>();
    ASSERT_FALSE(labels.isEmpty());
    bool has_freezer = false;
    bool has_position = false;
    for (auto* l : labels) {
      if (l->text().contains(QStringLiteral("Freezer A")))
        has_freezer = true;
      if (l->text().contains(QStringLiteral("A1")))
        has_position = true;
    }
    EXPECT_TRUE(has_freezer);
    EXPECT_TRUE(has_position);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).toString(), QStringLiteral("s-1"));
  }

  TEST_F(SampleLookupWidgetTest, NoMatchShowsNotFoundState) {
    enterQuery(QStringLiteral("does-not-exist"));

    auto* not_found = widget_->findChild<QLabel*>(QStringLiteral("notFoundLabel"));
    ASSERT_NE(not_found, nullptr);
    EXPECT_EQ(stack()->currentWidget(), not_found);
    EXPECT_TRUE(not_found->text().contains(QStringLiteral("does-not-exist")));
  }

  TEST_F(SampleLookupWidgetTest, MultipleMatchesShowsDisambiguation) {
    // Two samples share a barcode → ambiguous.
    sample_service_.samples.push_back(makeSample("s-1", "Plasma A", "DUP", "box-1", "A1"));
    sample_service_.samples.push_back(makeSample("s-2", "Plasma B", "DUP", "box-1", "B2"));

    enterQuery(QStringLiteral("DUP"));

    auto* list = widget_->findChild<QListWidget*>(QStringLiteral("matchList"));
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(stack()->currentWidget(), list);
    EXPECT_EQ(list->count(), 2);
  }

  TEST_F(SampleLookupWidgetTest, ClearsInputAfterSuccessfulLookup) {
    sample_service_.samples.push_back(makeSample("s-1", "Plasma A", "BC-1", "box-1", "A1"));

    enterQuery(QStringLiteral("BC-1"));

    auto* input = widget_->findChild<QLineEdit*>(QStringLiteral("lookupInput"));
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(input->text().isEmpty());
  }

  TEST_F(SampleLookupWidgetTest, WidgetAutofocusesInputOnShow) {
    widget_->show();
    widget_->activateWindow();
    QApplication::processEvents();

    auto* input = widget_->findChild<QLineEdit*>(QStringLiteral("lookupInput"));
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(input->hasFocus());
  }

  // ── Error-path and scenario tests ─────────────────────────────────

  TEST_F(SampleLookupWidgetTest, NameMatchFallbackWhenNoBarcode) {
    // Barcode-first search returns empty (no barcode match), widget falls back
    // to a lab-wide unfiltered search and client-side name match.
    sample_service_.samples.push_back(makeSample("s-nb", "Serum X", "", "box-1", "B3"));

    QSignalSpy spy(widget_.get(), &SampleLookupWidget::sampleSelected);
    enterQuery(QStringLiteral("Serum X"));

    // Should find via name fallback and emit sampleSelected.
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).toString(), QStringLiteral("s-nb"));
  }

  TEST_F(SampleLookupWidgetTest, DisambiguationClickShowsFoundPath) {
    sample_service_.samples.push_back(makeSample("s-a", "Plasma A", "DUP", "box-1", "A1"));
    sample_service_.samples.push_back(makeSample("s-b", "Plasma B", "DUP", "box-1", "B2"));

    enterQuery(QStringLiteral("DUP"));

    auto* list = widget_->findChild<QListWidget*>(QStringLiteral("matchList"));
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 2);

    // Click the first item — should resolve to showFound with the path.
    QSignalSpy spy(widget_.get(), &SampleLookupWidget::sampleSelected);
    emit list->itemActivated(list->item(0));
    // Disambiguation click does NOT emit sampleSelected (only single-match does).
    EXPECT_EQ(spy.count(), 0);

    auto* path = widget_->findChild<QWidget*>(QStringLiteral("pathView"));
    ASSERT_NE(path, nullptr);
    const auto labels = path->findChildren<QLabel*>();
    bool has_a1 = false;
    for (auto* l : labels) {
      if (l->text().contains(QStringLiteral("A1")))
        has_a1 = true;
    }
    EXPECT_TRUE(has_a1);
  }

  TEST_F(SampleLookupWidgetTest, GetBoxFailureShowsLocationUnavailable) {
    sample_service_.samples.push_back(makeSample("s-1", "Plasma A", "BC-1", "ghost", "A1"));
    box_service_.fail_get_box = grpc::StatusCode::NOT_FOUND;
    box_service_.fail_message = "box deleted";

    enterQuery(QStringLiteral("BC-1"));

    // The found page should show an error label instead of breadcrumb.
    auto* path = widget_->findChild<QWidget*>(QStringLiteral("pathView"));
    ASSERT_NE(path, nullptr);
    const auto labels = path->findChildren<QLabel*>();
    bool has_error = false;
    for (auto* l : labels) {
      if (l->text().contains(QStringLiteral("Location unavailable")))
        has_error = true;
    }
    EXPECT_TRUE(has_error);
  }

  TEST_F(SampleLookupWidgetTest, UnplacedSampleShowsMessage) {
    sample_service_.samples.push_back(makeSample("s-up", "Drift Tube", "BC-UP", "", ""));

    enterQuery(QStringLiteral("BC-UP"));

    auto* path = widget_->findChild<QWidget*>(QStringLiteral("pathView"));
    ASSERT_NE(path, nullptr);
    const auto labels = path->findChildren<QLabel*>();
    bool has_unplaced = false;
    for (auto* l : labels) {
      if (l->text().contains(QStringLiteral("Unplaced")))
        has_unplaced = true;
    }
    EXPECT_TRUE(has_unplaced);
  }

  TEST_F(SampleLookupWidgetTest, CheckedOutSampleShowsStatusBadge) {
    sample_service_.samples.push_back(makeSample("s-co", "Plasma Old", "BC-CO", "box-1", "C3",
                                                 fmgr::v1::SAMPLE_STATUS_CHECKED_OUT));

    enterQuery(QStringLiteral("BC-CO"));

    auto* badge = widget_->findChild<QLabel*>(QStringLiteral("statusBadge"));
    ASSERT_NE(badge, nullptr);
    EXPECT_TRUE(badge->text().contains(QStringLiteral("Checked out")));
  }

  TEST_F(SampleLookupWidgetTest, EmptyQueryIsNoOp) {
    QSignalSpy spy(widget_.get(), &SampleLookupWidget::sampleSelected);
    enterQuery(QString());

    EXPECT_EQ(spy.count(), 0);
  }

} // namespace
