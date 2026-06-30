# Qt 6 desktop client.
#
# Qt 6 is the system package (qt6-base-dev), found via CMake — not Conan. We link
# it dynamically to stay compliant with Qt's LGPLv3 under our dual license. See the
# note in the repo-root conanfile.py.
#
# If Qt 6 is not installed the whole client (library, executable, and its unit
# tests) is silently skipped so the rest of the build is unaffected.

find_package(Qt6 QUIET COMPONENTS Widgets)

if(NOT Qt6_FOUND)
  message(STATUS "Qt6 not found — skipping freezermanager-qt desktop client")
  return()
endif()

message(STATUS "Qt6 ${Qt6_VERSION} found — building freezermanager-qt desktop client")

# AUTOMOC handles the Q_OBJECT meta-object generation for QWidget subclasses.
set(CMAKE_AUTOMOC ON)

# Command/widget logic lives in a library so it can be unit-tested without
# spawning the GUI; the executable is a thin main() shim over it. Mirrors the
# pattern used by src/cli.
add_library(
  freezermanager_qt_lib
  AuthServiceClient.cc
  BarcodeScanController.cc
  BarcodeScanWidget.cc
  BoxGridModel.cc
  BoxGridWidget.cc
  BoxServiceClient.cc
  ConfigManager.cc
  GrpcChannel.cc
  ImportWizardDialog.cc
  LabServiceClient.cc
  LabTreeModel.cc
  LabTreeWidget.cc
  LocationPathResolver.cc
  LoginDialog.cc
  MainWindow.cc
  SampleBrowserWidget.cc
  SampleLookupWidget.cc
  SampleSearchBar.cc
  SampleServiceClient.cc
  SampleTableModel.cc
  SampleWatchSubscriber.cc
  SessionManager.cc
)
add_library(FreezerManager::qt_lib ALIAS freezermanager_qt_lib)

target_include_directories(
  freezermanager_qt_lib
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(
  freezermanager_qt_lib
  PUBLIC
    FreezerManager::proto
    Qt6::Widgets
)

add_executable(freezermanager-qt main.cc)
target_link_libraries(freezermanager-qt PRIVATE FreezerManager::qt_lib)
