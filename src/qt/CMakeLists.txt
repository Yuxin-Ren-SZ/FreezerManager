# SPDX-License-Identifier: AGPL-3.0-or-later
# Qt 6 desktop client — experimental exploration.

find_package(Qt6 REQUIRED COMPONENTS Widgets Quick QuickWidgets Quick3D Svg OpenGL)

add_executable(freezermanager_qt
  main.cc
  MainWindow.h
  MainWindow.cc
  mock/MockData.h
  models/SampleTableModel.h
  pages/SampleBrowserPage.h
  pages/DashboardPage.h
  pages/FreezerExplorerPage.h
  pages/Freezer3DPage.h
  pages/SampleDetailPage.h
  widgets/BoxGridView.h
  resources.qrc
)

target_link_libraries(freezermanager_qt
  PRIVATE
    FreezerManager::core
    Qt6::Widgets
    Qt6::Quick
    Qt6::QuickWidgets
    Qt6::Quick3D
    Qt6::Svg
    Qt6::OpenGL
)

target_include_directories(freezermanager_qt
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)

# C++20 is set globally; ensure Qt target gets it too.
set_target_properties(freezermanager_qt PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

# Qt MOC/AUTOMOC is handled by Qt6::Widgets and friends.
set_target_properties(freezermanager_qt PROPERTIES
  AUTOMOC ON
  AUTORCC ON
  AUTOUIC ON
)
